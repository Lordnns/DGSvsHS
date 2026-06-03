using System.Buffers.Binary;
using System.IO;
using System.Numerics;
using DGSvsHS.ArchServer.Server;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Net;

public static class WireCodec
{
    public const uint ProtocolVersion = 4;

    public const byte MsgClientHello   = 0x01;
    public const byte MsgServerWelcome = 0x02;
    public const byte MsgInput         = 0x10;
    public const byte MsgSnapshot      = 0x20;
    public const byte MsgDisconnect    = 0xF0;

    // ---------- ClientHello (read) ----------

    public static void ReadClientHello(BinaryReader r, out uint version, out byte capabilities)
    {
        version = r.ReadUInt32();
        capabilities = r.ReadByte();
    }

    public const int ServerWelcomePayloadBytes = 13;

    public static void WriteServerWelcome(BinaryWriter w, byte playerId, uint serverTick)
    {
        w.Write(ProtocolVersion);
        w.Write(playerId);
        w.Write(serverTick);
        w.Write((ushort)Constants.SimTickMs);
        w.Write((ushort)Constants.SnapshotEveryNTicks);
    }

    public const int InputCmdWireBytes = 15;
    public const int MaxInputBatch = 4;

    public static int ReadInputBatch(BinaryReader r, InputCmd[] outCmds)
    {
        byte count = r.ReadByte();
        if (count < 1 || count > MaxInputBatch) throw new InvalidDataException("input batch count out of range");
        if (outCmds.Length < count) throw new InvalidDataException("output buffer too small");
        for (int i = 0; i < count; i++) outCmds[i] = ReadOneInput(r);
        return count;
    }

    private static InputCmd ReadOneInput(BinaryReader r)
    {
        uint tick = r.ReadUInt32();
        uint ack = r.ReadUInt32();
        short mxQ = r.ReadInt16();
        short myQ = r.ReadInt16();
        short aimQ = r.ReadInt16();
        byte flags = r.ReadByte();
        float ang = aimQ / (float)Constants.AngleScale;
        return new InputCmd
        {
            Tick = tick,
            LastAckedServerTick = ack,
            Move = new Vector2(mxQ / (float)Constants.PositionScale, myQ / (float)Constants.PositionScale),
            Aim = new Vector2(MathF.Cos(ang), MathF.Sin(ang)),
            Flags = (InputFlags)flags,
        };
    }


    public const int SnapshotHeaderBytes = 1 + 4 + 4 + 4 + 2 + 4 + 4 + 1;
    public const int PlayerSnapFullBytes  = 1 + 2 + 2 + 2 + 1 + 2;
    public const int EnemySnapFullBytes   = 2 + 2 + 2;
    public const int EnemyDeltaEntryBytes = 2 + 2 + 2;
    public const int FireEventBytes       = 4 + 1 + 2 + 2 + 2 + 2 + 1;
    public const int FullBodyArrayHeaderBytes = 1 + 2 + 4 + 1;

    // ---------- Header ----------

    public static void WriteSnapshotHeader(BinaryWriter w, Snapshot s)
    {
        w.Write((byte)s.Kind);
        w.Write(s.Tick);
        w.Write(s.BaselineTick);
        w.Write(s.LastProcessedInputTick);
        w.Write((ushort)s.Round);
        w.Write(s.RoundTimer);
        w.Write(s.InterRoundTimer);
        w.Write((byte)s.Phase);
    }

    // ---------- Full body ----------

    public static void WriteFullSnapshotBody(
        BinaryWriter w,
        IReadOnlyList<PlayerSnap> players,
        IReadOnlyList<EnemySnap> enemiesToSend,
        uint enemyTotalInWorld,
        IReadOnlyList<FireEvent> fires)
    {
        w.Write((byte)players.Count);
        for (int i = 0; i < players.Count; i++) WritePlayer(w, players[i]);

        w.Write((ushort)enemiesToSend.Count);
        w.Write(enemyTotalInWorld);
        for (int i = 0; i < enemiesToSend.Count; i++) WriteEnemy(w, enemiesToSend[i]);

        int fireCount = Math.Min(16, fires.Count);
        w.Write((byte)fireCount);
        for (int i = 0; i < fireCount; i++) WriteFire(w, fires[i]);
    }

    // ---------- Delta body ----------

    public static void WriteDeltaSnapshotBody(
        BinaryWriter w,
        IReadOnlyList<PlayerSnap> players,
        IReadOnlyList<EnemyDeltaEntry> changed,
        IReadOnlyList<ushort> removed,
        IReadOnlyList<EnemySnap> added,
        uint enemyTotalInWorld,
        IReadOnlyList<FireEvent> fires)
    {
        w.Write((byte)players.Count);
        for (int i = 0; i < players.Count; i++) WritePlayer(w, players[i]);

        w.Write((ushort)changed.Count);
        for (int i = 0; i < changed.Count; i++)
        {
            var e = changed[i];
            w.Write(e.Id);
            w.Write(QuantPos(e.Position.X));
            w.Write(QuantPos(e.Position.Y));
        }

        w.Write((ushort)removed.Count);
        for (int i = 0; i < removed.Count; i++) w.Write(removed[i]);

        w.Write((ushort)added.Count);
        for (int i = 0; i < added.Count; i++) WriteEnemy(w, added[i]);

        w.Write(enemyTotalInWorld);

        int fireCount = Math.Min(16, fires.Count);
        w.Write((byte)fireCount);
        for (int i = 0; i < fireCount; i++) WriteFire(w, fires[i]);
    }

    // ---------- Quantization for delta change-detection ----------

    public static bool EnemyPositionChanged(in EnemySnap baseline, in EnemySnap current)
    {
        return QuantPos(baseline.Position.X) != QuantPos(current.Position.X)
            || QuantPos(baseline.Position.Y) != QuantPos(current.Position.Y);
    }

    private static void WritePlayer(BinaryWriter w, in PlayerSnap p)
    {
        w.Write(p.Id);
        w.Write(QuantPos(p.Position.X));
        w.Write(QuantPos(p.Position.Y));
        w.Write(QuantAngle(MathF.Atan2(p.Aim.Y, p.Aim.X)));
        w.Write((byte)(p.Alive ? 1 : 0));
        ushort disableTicks = (ushort)Math.Min(ushort.MaxValue, (int)MathF.Round(p.DisableTimer * Constants.TicksPerSecond));
        w.Write(disableTicks);
    }

    private static void WriteEnemy(BinaryWriter w, in EnemySnap e)
    {
        w.Write(e.Id);
        w.Write(QuantPos(e.Position.X));
        w.Write(QuantPos(e.Position.Y));
    }

    private static void WriteFire(BinaryWriter w, in FireEvent f)
    {
        w.Write(f.Tick);
        w.Write(f.ShooterId);
        w.Write(QuantPos(f.Origin.X));
        w.Write(QuantPos(f.Origin.Y));
        w.Write(QuantAngle(MathF.Atan2(f.Direction.Y, f.Direction.X)));
        w.Write(QuantPos(f.Distance));
        w.Write(f.KillCount);
    }

    private static short QuantPos(float m)
    {
        int q = (int)MathF.Round(m * Constants.PositionScale);
        if (q > short.MaxValue) q = short.MaxValue;
        else if (q < short.MinValue) q = short.MinValue;
        return (short)q;
    }

    private static short QuantAngle(float rad)
    {
        int q = (int)MathF.Round(rad * Constants.AngleScale);
        if (q > short.MaxValue) q = short.MaxValue;
        else if (q < short.MinValue) q = short.MinValue;
        return (short)q;
    }

    // ---------- Stream framing helpers ----------

    public static byte[] FrameStreamMessage(byte msgType, ReadOnlySpan<byte> payload)
    {
        var frame = new byte[4 + 1 + payload.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(0, 4), (uint)payload.Length);
        frame[4] = msgType;
        payload.CopyTo(frame.AsSpan(5));
        return frame;
    }
}
