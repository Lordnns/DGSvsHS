using System.Numerics;
using Arch.Core;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Server.Systems;

public static class RoundDirector
{
    private static readonly QueryDescription PlayerQuery = new QueryDescription().WithAll<PlayerTag>();
    private static readonly QueryDescription PlayerStateQuery = new QueryDescription().WithAll<PlayerTag, Alive, DisableTimer>();
    private static readonly QueryDescription EnemyQuery = new QueryDescription().WithAll<EnemyTag>();

    public static void Run(World world, SimContext ctx)
    {
        int connectedPlayers = world.CountEntities(in PlayerQuery);
        if (connectedPlayers == 0 && ctx.Round.Phase != RoundPhase.PreGame)
        {
            // No clients — back to idle.
            ctx.Round.Phase = RoundPhase.PreGame;
            ctx.Round.Round = 0;
            ctx.Round.RoundTimer = 0f;
            ctx.Round.InterRoundTimer = 0f;
            ctx.Round.SpawnsRemaining = 0;
            ctx.Round.SpawnTarget = 0;
            return;
        }

        switch (ctx.Round.Phase)
        {
            case RoundPhase.PreGame:
                break;

            case RoundPhase.InterRound:
                ctx.Round.InterRoundTimer -= Constants.SimDt;
                if (ctx.Round.InterRoundTimer <= 0f)
                {
                    ctx.Round.Round++;
                    if (ctx.Round.Round > Constants.TotalRounds)
                    {
                        ctx.Round.Phase = RoundPhase.Victory;
                    }
                    else
                    {
                        ctx.Round.Phase = RoundPhase.InRound;
                        ctx.Round.RoundTimer = 0f;
                        StartWave(ref ctx.Round, ctx.Round.Round);
                    }
                }
                break;

            case RoundPhase.InRound:
                ctx.Round.RoundTimer += Constants.SimDt;
                TickWave(world, ctx);

                if (AllConnectedPlayersDisabled(world))
                {
                    ResetToRoundOne(world, ctx);
                }
                else if (ctx.Round.SpawnsRemaining == 0 && world.CountEntities(in EnemyQuery) == 0)
                {
                    ctx.Round.Phase = RoundPhase.InterRound;
                    ctx.Round.InterRoundTimer = Constants.InterRoundDelaySec;
                }
                break;

            case RoundPhase.Victory:
            case RoundPhase.Defeat:
                break;
        }
    }

    private static void StartWave(ref RoundStateData round, int forRound)
    {
        int target = TargetEnemiesForRound(forRound);
        round.SpawnTarget = target;
        round.SpawnsRemaining = target;
        round.SpawnInterval = Constants.RoundSpawnWindowSec / Math.Max(1, target);
        round.SpawnAccumulator = 0f;
    }

    public static int TargetEnemiesForRound(int forRound)
    {
        if (forRound < 1) return 0;
        float scaled = Constants.BaseEnemiesPerRound * MathF.Pow(Constants.EnemyScalingPerRound, forRound - 1);
        return (int)MathF.Round(scaled);
    }

    private static void TickWave(World world, SimContext ctx)
    {
        if (ctx.Round.SpawnsRemaining <= 0) return;
        ctx.Round.SpawnAccumulator += Constants.SimDt;
        while (ctx.Round.SpawnAccumulator >= ctx.Round.SpawnInterval && ctx.Round.SpawnsRemaining > 0)
        {
            ctx.Round.SpawnAccumulator -= ctx.Round.SpawnInterval;
            SpawnOneEnemy(world, ctx);
            ctx.Round.SpawnsRemaining--;
        }
    }

    private static void SpawnOneEnemy(World world, SimContext ctx)
    {
        float angle = ctx.Rng.NextRange(0f, MathF.PI * 2f);
        float r = Constants.ArenaRadius - Constants.EnemyRadius - 0.1f;
        var pos = new Vector2(MathF.Cos(angle) * r, MathF.Sin(angle) * r);

        ushort id = ctx.NextEnemyId++;
        world.Create(
            new EnemyTag(),
            new EnemyId { Value = id },
            new Position2D { Value = pos },
            new Velocity2D { Value = Vector2.Zero });
    }

    private static bool AllConnectedPlayersDisabled(World world)
    {
        int total = 0, disabled = 0;
        world.Query(in PlayerStateQuery, (ref Alive alive, ref DisableTimer dt) =>
        {
            if (!alive.Bool) return;
            total++;
            if (dt.Seconds > 0f) disabled++;
        });
        return total > 0 && disabled == total;
    }

    private static readonly QueryDescription PlayerResetQuery = new QueryDescription()
        .WithAll<PlayerTag, Alive, DisableTimer, FireCooldown>();

    private static void ResetToRoundOne(World world, SimContext ctx)
    {
        ctx.Round.Round = 0;
        ctx.Round.Phase = RoundPhase.InterRound;
        ctx.Round.InterRoundTimer = Constants.InterRoundDelaySec;
        ctx.Round.RoundTimer = 0f;
        ctx.Round.SpawnTarget = 0;
        ctx.Round.SpawnsRemaining = 0;
        ctx.Round.SpawnInterval = 0f;
        ctx.Round.SpawnAccumulator = 0f;

        SimBootstrap.DestroyAllEnemies(world);

        world.Query(in PlayerResetQuery, (ref Alive alive, ref DisableTimer dt, ref FireCooldown cd) =>
        {
            alive = Alive.From(true);
            dt.Seconds = 0f;
            cd.Seconds = 0f;
        });
    }
}
