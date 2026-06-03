using System.Numerics;

namespace DGSvsHS.ArchServer.Server;

// ---------- Enemy archetype ----------

public struct EnemyTag { }

public struct EnemyId
{
    public ushort Value;
}

public struct Position2D
{
    public Vector2 Value;
}

public struct Velocity2D
{
    public Vector2 Value;
}

// ---------- Player archetype ----------

public struct PlayerTag { }

public struct PlayerSlot
{
    public byte Value;
}

public struct Aim2D
{
    public Vector2 Value;
}

public struct FireCooldown
{
    public float Seconds;
}

public struct DisableTimer
{
    public float Seconds;
}

public struct Alive
{
    public byte Value;
    public bool Bool => Value != 0;
    public static Alive From(bool b) => new() { Value = (byte)(b ? 1 : 0) };
}
