using System.Numerics;
using Arch.Core;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Server.Systems;

public static class SnapshotCapture
{
    private static readonly QueryDescription PlayerQuery = new QueryDescription()
        .WithAll<PlayerTag, PlayerSlot, Position2D, Aim2D, FireCooldown, DisableTimer, Alive>();

    private static readonly QueryDescription EnemyQuery = new QueryDescription()
        .WithAll<EnemyTag, EnemyId, Position2D>();

    public static void Run(World world, SimContext ctx, Snapshot target)
    {
        if (target == null) return;
        target.Clear();
        target.Kind = SnapshotKind.Full;
        target.Tick = ctx.Tick;
        target.LastProcessedInputTick = 0;
        target.Round = ctx.Round.Round;
        target.RoundTimer = ctx.Round.RoundTimer;
        target.InterRoundTimer = ctx.Round.InterRoundTimer;
        target.Phase = ctx.Round.Phase;

        var players = target.Players;
        world.Query(in PlayerQuery, (
            ref PlayerSlot slot, ref Position2D pos, ref Aim2D aim,
            ref FireCooldown cd, ref DisableTimer dt, ref Alive alive) =>
        {
            players.Add(new PlayerSnap
            {
                Id = slot.Value,
                Position = pos.Value,
                Aim = aim.Value,
                Alive = alive.Bool,
                DisableTimer = dt.Seconds,
            });
        });

        var enemies = target.Enemies;
        uint enemyCount = 0;
        world.Query(in EnemyQuery, (ref EnemyId id, ref Position2D pos) =>
        {
            enemies.Add(new EnemySnap { Id = id.Value, Position = pos.Value });
            enemyCount++;
        });
        target.EnemyTotalInWorld = enemyCount;

        for (int i = 0; i < ctx.FireEvents.Count; i++) target.RecentFireEvents.Add(ctx.FireEvents[i]);
    }
}
