using System.Numerics;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Server;

public sealed class SimContext
{
    public uint Tick;

    public RoundStateData Round;

    public DeterministicRng Rng;

    public ushort NextEnemyId;

    public bool GodMode;

    public readonly List<TickInput> TickInputs = new(64);

    public readonly List<PendingFire> PendingFires = new(16);

    public readonly List<FireEvent> FireEvents = new(16);

    public readonly float[] PlayerRttMs = new float[Constants.MaxPlayers];

    public readonly RewindRing Rewind = new(Constants.SnapshotHistoryTicks, Constants.MaxEnemies);

    public SimContext()
    {
        for (int i = 0; i < PlayerRttMs.Length; i++) PlayerRttMs[i] = 60f;
    }
}

public struct RoundStateData
{
    public int Round;
    public RoundPhase Phase;
    public float RoundTimer;
    public float InterRoundTimer;
    public int SpawnTarget;
    public int SpawnsRemaining;
    public float SpawnInterval;
    public float SpawnAccumulator;
}

// ---------- Per-tick sidecar messages ----------

public struct TickInput
{
    public byte PlayerId;
    public uint Tick;
    public uint LastAckedServerTick;
    public Vector2 Move;
    public Vector2 Aim;
    public InputFlags Flags;
    public bool Fire => (Flags & InputFlags.Fire) != 0;
}

public struct PendingFire
{
    public byte PlayerId;
    public uint ClientInputTick;
    public Vector2 Origin;
    public Vector2 Direction;
}

// ---------- Rewind ring ----------

public struct RewindFrameHeader
{
    public uint Tick;
    public int Count;
}

public sealed class RewindRing
{
    public readonly int Slots;
    public readonly int Stride;
    public readonly RewindFrameHeader[] Headers;
    public readonly ushort[] Ids;
    public readonly Vector2[] Positions;
    public int Head;
    public int Count;

    public RewindRing(int slots, int stride)
    {
        Slots = slots;
        Stride = stride;
        Headers = new RewindFrameHeader[slots];
        Ids = new ushort[slots * stride];
        Positions = new Vector2[slots * stride];
        Head = 0;
        Count = 0;
    }

    public void Clear()
    {
        Head = 0;
        Count = 0;
        for (int i = 0; i < Headers.Length; i++) Headers[i] = default;
    }
}
