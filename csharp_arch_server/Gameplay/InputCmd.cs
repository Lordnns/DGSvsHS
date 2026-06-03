using System.Numerics;

namespace DGSvsHS.Gameplay;

public struct InputCmd
{
    public uint Tick;
    public uint LastAckedServerTick;
    public Vector2 Move;
    public Vector2 Aim;
    public InputFlags Flags;

    public bool Fire => (Flags & InputFlags.Fire) != 0;

    public static InputCmd Empty(uint tick) => new()
    {
        Tick = tick,
        LastAckedServerTick = 0,
        Move = Vector2.Zero,
        Aim = Vector2.UnitX,
        Flags = InputFlags.None,
    };
}

[System.Flags]
public enum InputFlags : byte
{
    None = 0,
    Fire = 1 << 0,
}
