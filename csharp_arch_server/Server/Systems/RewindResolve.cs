using System.Numerics;
using Arch.Core;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Server.Systems;

public static class RewindResolve
{
    private static readonly QueryDescription AliveEnemyQuery = new QueryDescription()
        .WithAll<EnemyTag, EnemyId>();

    private static readonly Dictionary<ushort, Entity> _idToEntity = new(4096);

    public static void Run(World world, SimContext ctx)
    {
        var pending = ctx.PendingFires;
        if (pending.Count == 0) return;

        _idToEntity.Clear();
        var idToEntity = _idToEntity;
        world.Query(in AliveEnemyQuery, (Entity e, ref EnemyId id) =>
        {
            idToEntity[id.Value] = e;
        });

        var ring = ctx.Rewind;
        float hitRadius = Constants.EnemyRadius + Constants.BeamRadius;
        float hitRadiusSq = hitRadius * hitRadius;
        float maxRange = Constants.BulletMaxRange;

        var killed = new HashSet<ushort>();

        for (int fi = 0; fi < pending.Count; fi++)
        {
            var f = pending[fi];
            float oneWayMs = (f.PlayerId < ctx.PlayerRttMs.Length ? ctx.PlayerRttMs[f.PlayerId] : 60f) * 0.5f;
            float viewTickF = ComputeViewTickF(ctx.Tick, oneWayMs);

            if (!FindBracketingSlots(ring, viewTickF, out int floorSlot, out int ceilSlot, out float alpha))
                continue;

            var floorHdr = ring.Headers[floorSlot];
            var ceilHdr = ring.Headers[ceilSlot];
            int floorStart = floorSlot * ring.Stride;
            int ceilStart = ceilSlot * ring.Stride;

            int kills = 0;

            for (int i = 0; i < floorHdr.Count; i++)
            {
                ushort id = ring.Ids[floorStart + i];
                Vector2 fPos = ring.Positions[floorStart + i];
                Vector2 pos = fPos;
                for (int j = 0; j < ceilHdr.Count; j++)
                {
                    if (ring.Ids[ceilStart + j] != id) continue;
                    pos = Vector2.Lerp(fPos, ring.Positions[ceilStart + j], alpha);
                    break;
                }
                if (SegmentHits(f.Origin, f.Direction, maxRange, pos, hitRadiusSq))
                {
                    if (killed.Add(id)) kills++;
                }
            }

            if (alpha >= 0.5f)
            {
                for (int j = 0; j < ceilHdr.Count; j++)
                {
                    ushort id = ring.Ids[ceilStart + j];
                    bool inFloor = false;
                    for (int i = 0; i < floorHdr.Count; i++)
                    {
                        if (ring.Ids[floorStart + i] == id) { inFloor = true; break; }
                    }
                    if (inFloor) continue;
                    Vector2 pos = ring.Positions[ceilStart + j];
                    if (SegmentHits(f.Origin, f.Direction, maxRange, pos, hitRadiusSq))
                    {
                        if (killed.Add(id)) kills++;
                    }
                }
            }

            ctx.FireEvents.Add(new FireEvent
            {
                Tick = ctx.Tick,
                ShooterId = f.PlayerId,
                Origin = f.Origin,
                Direction = f.Direction,
                Distance = maxRange,
                KillCount = (byte)Math.Min(255, kills),
            });
        }

        foreach (var id in killed)
        {
            if (idToEntity.TryGetValue(id, out var entity))
            {
                world.Destroy(entity);
            }
        }

        pending.Clear();
    }

    private static bool FindBracketingSlots(RewindRing ring, float viewTickF, out int floorSlot, out int ceilSlot, out float alpha)
    {
        floorSlot = -1; ceilSlot = -1; alpha = 0f;
        if (ring.Count == 0) return false;

        uint viewFloor = (uint)MathF.Floor(viewTickF);
        uint viewCeil = viewFloor + 1;

        for (int i = 0; i < ring.Count; i++)
        {
            int slot = (ring.Head - 1 - i + ring.Slots) % ring.Slots;
            var hdr = ring.Headers[slot];
            if (hdr.Tick == viewFloor) floorSlot = slot;
            if (hdr.Tick == viewCeil) ceilSlot = slot;
        }
        if (floorSlot < 0)
        {
            int oldest = (ring.Head - ring.Count + ring.Slots) % ring.Slots;
            floorSlot = oldest;
            ceilSlot = oldest;
            alpha = 0f;
            return true;
        }
        if (ceilSlot < 0)
        {
            ceilSlot = floorSlot;
            alpha = 0f;
            return true;
        }
        alpha = Math.Clamp(viewTickF - viewFloor, 0f, 1f);
        return true;
    }

    private static float ComputeViewTickF(uint serverTick, float oneWayLatencyMs)
    {
        return serverTick
               - (oneWayLatencyMs / 1000f) * Constants.TicksPerSecond
               - (Constants.InterpolationBufferMs / 1000f) * Constants.TicksPerSecond;
    }

    private static bool SegmentHits(Vector2 origin, Vector2 dir, float maxRange, Vector2 enemyPos, float hitRadiusSq)
    {
        Vector2 toEnemy = enemyPos - origin;
        float t = Vector2.Dot(toEnemy, dir);
        if (t < 0f || t > maxRange) return false;
        Vector2 closest = origin + dir * t;
        return (enemyPos - closest).LengthSquared() <= hitRadiusSq;
    }
}
