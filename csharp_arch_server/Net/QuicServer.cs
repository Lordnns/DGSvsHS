using System.Collections.Concurrent;
using System.IO;
using System.Net;
using System.Numerics;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using DGSvsHS.ArchServer.Server;
using DGSvsHS.Gameplay;
using StirlingLabs.MsQuic;
using StirlingLabs.MsQuic.Bindings;
using StirlingLabs.Utilities;

namespace DGSvsHS.ArchServer.Net;

public sealed class QuicServer : IDisposable
{
    public event Action<byte>? ClientConnected;
    public event Action<byte>? ClientDisconnected;
    public event Action<byte, InputCmd>? InputReceived;

    private readonly ushort _port;
    private readonly QuicRegistration _registration;
    private QuicCertificate _certificate;          // mutable: ConfigureCredentials takes by ref
    private readonly QuicServerConfiguration _config;
    private readonly QuicListener _listener;

    private readonly Slot[] _slots = new Slot[Constants.MaxPlayers];

    private readonly ConcurrentQueue<NetworkEvent> _events = new();

    private readonly InputCmd[] _inputDecodeBuf = new InputCmd[WireCodec.MaxInputBatch];

    private readonly uint[] _highestInputTick = new uint[Constants.MaxPlayers];

    private readonly List<EnemySnap> _selectedEnemiesScratch = new(256);
    private readonly List<SnapshotPriority.ScoredEnemy> _scoredScratch = new(4096);

    private readonly List<EnemyDeltaEntry> _changedScratch = new(2048);
    private readonly List<ushort> _removedScratch = new(256);
    private readonly List<EnemySnap> _addedScratch = new(512);
    private readonly HashSet<ushort> _includedScratch = new();
    private readonly HashSet<ushort> _currentIdsScratch = new();
    private readonly Dictionary<ushort, int> _baselineIndexByIdScratch = new(2048);

    private readonly RecipientSnapshotState[] _recipientState = new RecipientSnapshotState[Constants.MaxPlayers];

    private uint _currentServerTick;

    public bool IsRunning { get; private set; }

    public QuicServer(ushort port)
    {
        _port = port;
        _registration = new QuicRegistration("DGSvsHS");
        _certificate = MakeSelfSignedCertificate();

        var alpns = new SizedUtf8String[] { "dgsvshs/2" };
        _config = new QuicServerConfiguration(_registration, alpns);
        // QUIC_CREDENTIAL_FLAGS.NONE = server-side cert with the chain we just exported.
        _config.ConfigureCredentials(in _certificate, QUIC_CREDENTIAL_FLAGS.NONE, QUIC_ALLOWED_CIPHER_SUITE_FLAGS.NONE);

        _listener = new QuicListener(_config);
        _listener.NewConnection += OnNewConnection;
        _listener.UnobservedException += (l, ex) => Log($"listener unobserved exception: {ex.SourceException}");
    }

    public void Start()
    {
        _listener.Start(new IPEndPoint(IPAddress.Any, _port));
        IsRunning = true;
        Log($"listening on UDP/{_port} (ALPN=dgsvshs/2, msquic via StirlingLabs.MsQuic) [build=stream-event-handler-v2]");
    }

    public void SetServerTick(uint tick) => _currentServerTick = tick;

    public void PollEvents()
    {
        while (_events.TryDequeue(out var ev))
        {
            switch (ev.Kind)
            {
                case NetworkEventKind.Connected:    ClientConnected?.Invoke(ev.PlayerId); break;
                case NetworkEventKind.Disconnected: ClientDisconnected?.Invoke(ev.PlayerId); break;
                case NetworkEventKind.Input:        InputReceived?.Invoke(ev.PlayerId, ev.Input); break;
            }
        }
    }

    public void BroadcastSnapshot(Snapshot snap, WorldStateHistory history)
    {
        int playersBytes = 1 + snap.Players.Count * WireCodec.PlayerSnapFullBytes;
        int firesCount   = Math.Min(16, snap.RecentFireEvents.Count);
        int firesBytes   = 1 + firesCount * WireCodec.FireEventBytes;
        int fixedOverhead = 1 + WireCodec.SnapshotHeaderBytes + playersBytes + firesBytes;

        for (byte pid = 0; pid < _slots.Length; pid++)
        {
            var slot = _slots[pid];
            if (slot is null || slot.Connection is null) continue;

            var rstate = _recipientState[pid];
            if (rstate is null)
            {
                rstate = _recipientState[pid] = new RecipientSnapshotState();
            }

            int pendingAck = Interlocked.Exchange(ref rstate.PendingAckedTick, 0);
            if (pendingAck > 0 && (uint)pendingAck > rstate.LastAckedServerTick)
            {
                rstate.LastAckedServerTick = (uint)pendingAck;
                rstate.OnAckAdvanced();
            }

            Snapshot baseline = null!;
            bool useDelta = false;
            uint baselineTick = 0;
            uint ackedTick = rstate.LastAckedServerTick;
            if (ackedTick > 0
                && snap.Tick >= ackedTick
                && (snap.Tick - ackedTick) <= (uint)Constants.MaxDeltaDepth
                && history.TryGet(ackedTick, out baseline))
            {
                useDelta = true;
                baselineTick = ackedTick;
            }

            Vector2 anchor = Vector2.Zero;
            for (int i = 0; i < snap.Players.Count; i++)
            {
                if (snap.Players[i].Id == pid) { anchor = snap.Players[i].Position; break; }
            }

            int enemySectionHeader = useDelta ? (2 + 2 + 2 + 4) : (2 + 4);
            int enemyBudget = Math.Max(0, Constants.SnapshotByteBudget - fixedOverhead - enemySectionHeader);

            var prevKind = snap.Kind;
            var prevBaseline = snap.BaselineTick;
            var prevLpi = snap.LastProcessedInputTick;
            snap.Kind = useDelta ? SnapshotKind.Delta : SnapshotKind.Full;
            snap.BaselineTick = useDelta ? baselineTick : 0u;
            snap.LastProcessedInputTick = _highestInputTick[pid];

            using var ms = new MemoryStream();
            using var w = new BinaryWriter(ms);
            w.Write(WireCodec.MsgSnapshot);
            WireCodec.WriteSnapshotHeader(w, snap);

            if (useDelta)
            {
                SnapshotPriority.SelectForDelta(
                    snap, baseline, anchor,
                    rstate.ConfirmedIds,
                    rstate.TicksSinceLastSent,
                    enemyBudget,
                    _changedScratch, _removedScratch, _addedScratch,
                    _includedScratch, _scoredScratch,
                    _currentIdsScratch, _baselineIndexByIdScratch);
                WireCodec.WriteDeltaSnapshotBody(
                    w, snap.Players,
                    _changedScratch, _removedScratch, _addedScratch,
                    snap.EnemyTotalInWorld, snap.RecentFireEvents);
            }
            else
            {
                SnapshotPriority.SelectForFull(snap, anchor, enemyBudget, _selectedEnemiesScratch, _scoredScratch);
                WireCodec.WriteFullSnapshotBody(
                    w, snap.Players, _selectedEnemiesScratch,
                    snap.EnemyTotalInWorld, snap.RecentFireEvents);

                _includedScratch.Clear();
                for (int i = 0; i < _selectedEnemiesScratch.Count; i++) _includedScratch.Add(_selectedEnemiesScratch[i].Id);
                _removedScratch.Clear();
            }

            snap.Kind = prevKind;
            snap.BaselineTick = prevBaseline;
            snap.LastProcessedInputTick = prevLpi;

            var bytes = ms.ToArray();
            bool sendOk = false;
            try
            {
                if ((snap.Tick & 63) == 0)
                      Log($"slot {pid}: DatagramsAllowed={slot.Connection.DatagramsAllowed} MaxSendLength={slot.Connection.MaxSendLength}");
                if (!slot.Connection.DatagramsAllowed) continue;
                if (bytes.Length > slot.Connection.MaxSendLength)
                {
                    Log($"slot {pid}: snapshot {bytes.Length} B > MaxSendLength {slot.Connection.MaxSendLength} — dropped (priority filter should prevent this)");
                    continue;
                }
                slot.Connection.SendDatagram(bytes);
                sendOk = true;
            }
            catch (Exception ex)
            {
                Log($"datagram send to player {pid} failed: {ex.Message}");
            }

            if (sendOk)
                rstate.OnSnapshotSent(snap.Tick, isFull: !useDelta, _includedScratch, _removedScratch);
        }
    }

    // ---------- Listener callback ----------

    private void OnNewConnection(QuicListener listener, QuicServerConnection conn)
    {
        int slotIndex = FindFreeSlot();
        if (slotIndex < 0)
        {
            Log($"connection refused — server full ({Constants.MaxPlayers} slots)");
            try { conn.Shutdown(silent: false); } catch { }
            try { conn.Dispose(); } catch { }
            return;
        }

        byte pid = (byte)slotIndex;
        var slot = new Slot { PlayerId = pid, Connection = conn };
        _slots[slotIndex] = slot;

        if (_recipientState[slotIndex] is null) _recipientState[slotIndex] = new RecipientSnapshotState();
        else _recipientState[slotIndex].Clear();

        conn.ReceiveDatagramsAsync = false;

        conn.IncomingStream         += (c, stream) => OnIncomingStream(slot, stream);
        conn.DatagramReceived       += (c, data) => OnDatagramReceived(slot, data);
        conn.ConnectionShutdown     += (c, _, _, _) => OnConnectionShutdown(slot);
        conn.Connected              += c => Log($"slot {slot.PlayerId}: handshake complete (DatagramsAllowed={c.DatagramsAllowed}, MaxSendLength={c.MaxSendLength}, ALPN={c.NegotiatedAlpn})");

        Log($"connection accepted from {conn.RemoteEndPoint} → slot {pid}");
        _events.Enqueue(new NetworkEvent { Kind = NetworkEventKind.Connected, PlayerId = pid });
    }

    // ---------- Per-connection callbacks ----------

    private void OnIncomingStream(Slot slot, QuicStream stream)
    {
        Log($"slot {slot.PlayerId}: incoming bidi stream (id={(stream.TryGetId(out long id) ? id.ToString() : "?")})");
        slot.ControlStream ??= stream;

        var rxBuf = new byte[256];
        int rxLen = 0;
        bool handshakeDone = false;

        stream.DataReceived = (s) =>
        {
            try
            {
                int got = s.Receive(rxBuf.AsSpan(rxLen));
                if (got <= 0) return;
                rxLen += got;
                Log($"slot {slot.PlayerId}: stream RX +{got} B (buffered {rxLen}, available {s.DataAvailable})");

                if (handshakeDone) return;

                if (rxLen < 5) return;
                uint frameLen = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(rxBuf.AsSpan(0, 4));

                int totalFrameBytes = 4 + 1 + (int)frameLen;
                if (rxLen < totalFrameBytes) return;

                byte msgType = rxBuf[4];
                if (msgType != WireCodec.MsgClientHello)
                {
                    Log($"slot {slot.PlayerId}: unexpected control msg 0x{msgType:X2} (expected ClientHello 0x01)");
                    return;
                }

                using (var ms = new MemoryStream(rxBuf, 5, (int)frameLen))
                using (var r = new BinaryReader(ms))
                {
                    WireCodec.ReadClientHello(r, out uint clientVersion, out byte caps);
                    Log($"slot {slot.PlayerId}: ClientHello version={clientVersion} caps=0x{caps:X2}");

                    if (clientVersion != WireCodec.ProtocolVersion)
                    {
                        Log($"slot {slot.PlayerId}: version mismatch (client={clientVersion} server={WireCodec.ProtocolVersion}) — disconnecting");
                        slot.Connection?.Shutdown(silent: false);
                        return;
                    }
                }

                using var wms = new MemoryStream();
                using (var ww = new BinaryWriter(wms, System.Text.Encoding.UTF8, leaveOpen: true))
                {
                    WireCodec.WriteServerWelcome(ww, slot.PlayerId, _currentServerTick);
                }
                byte[] welcome = WireCodec.FrameStreamMessage(WireCodec.MsgServerWelcome, wms.ToArray());
                _ = s.SendAsync(welcome.AsMemory(), QUIC_SEND_FLAGS.NONE);
                Log($"slot {slot.PlayerId}: sent ServerWelcome (proto={WireCodec.ProtocolVersion}, tick={_currentServerTick}, {welcome.Length} B)");

                handshakeDone = true;
                rxLen = 0;
            }
            catch (Exception ex)
            {
                Log($"slot {slot.PlayerId}: stream RX error: {ex.Message}");
            }
        };
    }

    private long _datagramRxCount;
    private long _inputBatchCount;
    private long _inputCmdCount;

    public long DatagramsReceived => Interlocked.Read(ref _datagramRxCount);
    public long InputBatchesParsed => Interlocked.Read(ref _inputBatchCount);
    public long InputCmdsQueued => Interlocked.Read(ref _inputCmdCount);

    private void OnDatagramReceived(Slot slot, ReadOnlySpan<byte> data)
    {
        Interlocked.Increment(ref _datagramRxCount);
        if (data.Length < 1) return;
        if (data[0] != WireCodec.MsgInput) return;

        try
        {
            using var ms = new MemoryStream(data[1..].ToArray());
            using var r = new BinaryReader(ms);
            int count = WireCodec.ReadInputBatch(r, _inputDecodeBuf);
            Interlocked.Increment(ref _inputBatchCount);

            uint batchMaxClientTick = 0;
            uint batchMaxServerAck = 0;
            for (int i = 0; i < count; i++)
            {
                if (_inputDecodeBuf[i].Tick > batchMaxClientTick) batchMaxClientTick = _inputDecodeBuf[i].Tick;
                if (_inputDecodeBuf[i].LastAckedServerTick > batchMaxServerAck)
                    batchMaxServerAck = _inputDecodeBuf[i].LastAckedServerTick;
                _events.Enqueue(new NetworkEvent
                {
                    Kind = NetworkEventKind.Input,
                    PlayerId = slot.PlayerId,
                    Input = _inputDecodeBuf[i],
                });
                Interlocked.Increment(ref _inputCmdCount);
            }

            uint existing;
            do { existing = Volatile.Read(ref _highestInputTick[slot.PlayerId]); }
            while (batchMaxClientTick > existing
                   && Interlocked.CompareExchange(ref _highestInputTick[slot.PlayerId], batchMaxClientTick, existing) != existing);

            var rstate = _recipientState[slot.PlayerId];
            if (rstate != null && batchMaxServerAck > 0)
            {
                int desired = (int)batchMaxServerAck;
                int cur;
                do { cur = Volatile.Read(ref rstate.PendingAckedTick); }
                while (desired > cur
                       && Interlocked.CompareExchange(ref rstate.PendingAckedTick, desired, cur) != cur);
            }
        }
        catch (Exception ex)
        {
            Log($"slot {slot.PlayerId}: malformed input datagram ({data.Length} B): {ex.Message}");
        }
    }

    private void OnConnectionShutdown(Slot slot)
    {
        if (_slots[slot.PlayerId] is null) return;
        _slots[slot.PlayerId] = null!;
        _highestInputTick[slot.PlayerId] = 0;
        _recipientState[slot.PlayerId]?.Clear();
        try { slot.Connection?.Dispose(); } catch { }
        Log($"slot {slot.PlayerId}: disconnected — totals: datagrams={DatagramsReceived} batches={InputBatchesParsed} cmds={InputCmdsQueued}");
        _events.Enqueue(new NetworkEvent { Kind = NetworkEventKind.Disconnected, PlayerId = slot.PlayerId });
    }

    // ---------- Helpers ----------

    private int FindFreeSlot()
    {
        for (int i = 0; i < _slots.Length; i++) if (_slots[i] is null) return i;
        return -1;
    }

    private static QuicCertificate MakeSelfSignedCertificate()
    {
        using var rsa = RSA.Create(2048);
        var req = new CertificateRequest("CN=DGSvsHS-server", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        var cert = req.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1));
        byte[] pfx = cert.Export(X509ContentType.Pfx, "");
        var ms = new MemoryStream(pfx, writable: false);
        return new QuicCertificate(ms, "");
    }

    private static void Log(string msg)
    {
        Console.WriteLine($"[QuicServer] {msg}");
        Console.Out.Flush();
    }

    public void Dispose()
    {
        try { _listener.Stop(); } catch { }
        try { _listener.Dispose(); } catch { }
        try { _certificate.Free(); } catch { }
        try { _registration.Dispose(); } catch { }
        IsRunning = false;
    }

    // ---------- Internal types ----------

    private sealed class Slot
    {
        public byte PlayerId;
        public QuicServerConnection? Connection;
        public QuicStream? ControlStream;
    }

    private enum NetworkEventKind : byte { Connected, Disconnected, Input }

    private struct NetworkEvent
    {
        public NetworkEventKind Kind;
        public byte PlayerId;
        public InputCmd Input;
    }
}
