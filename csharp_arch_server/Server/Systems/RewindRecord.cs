using Arch.Core;

namespace DGSvsHS.ArchServer.Server.Systems;

public static class RewindRecord
{
    private static readonly QueryDescription EnemyQuery = new QueryDescription()
        .WithAll<EnemyTag, EnemyId, Position2D>();

    public static void Run(World world, SimContext ctx)
    {
        var ring = ctx.Rewind;
        int slot = ring.Head;
        int slotStart = slot * ring.Stride;
        int stride = ring.Stride;
        int count = 0;

        world.Query(in EnemyQuery, (ref EnemyId id, ref Position2D pos) =>
        {
            if (count >= stride) return;
            ring.Ids[slotStart + count] = id.Value;
            ring.Positions[slotStart + count] = pos.Value;
            count++;
        });

        ring.Headers[slot] = new RewindFrameHeader { Tick = ctx.Tick, Count = count };
        ring.Head = (slot + 1) % ring.Slots;
        if (ring.Count < ring.Slots) ring.Count++;
    }
}
