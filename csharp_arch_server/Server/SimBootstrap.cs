using Arch.Core;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Server;

public static class SimBootstrap
{
    public static void ResetGlobals(SimContext ctx, ulong seed, bool godMode)
    {
        ctx.Tick = 0;
        ctx.Round = new RoundStateData
        {
            Round = 0,
            Phase = RoundPhase.PreGame,
            RoundTimer = 0f,
            InterRoundTimer = 0f,
            SpawnTarget = 0,
            SpawnsRemaining = 0,
            SpawnInterval = 0f,
            SpawnAccumulator = 0f,
        };
        ctx.Rng = DeterministicRng.FromSeed(seed);
        ctx.NextEnemyId = 0;
        ctx.GodMode = godMode;

        ctx.TickInputs.Clear();
        ctx.PendingFires.Clear();
        ctx.FireEvents.Clear();
        ctx.Rewind.Clear();
        for (int i = 0; i < ctx.PlayerRttMs.Length; i++) ctx.PlayerRttMs[i] = 60f;
    }

    private static readonly QueryDescription EnemyQuery = new QueryDescription().WithAll<EnemyTag>();

    public static void DestroyAllEnemies(World world)
    {
        var toKill = new List<Entity>();
        world.Query(in EnemyQuery, (Entity e) => toKill.Add(e));
        for (int i = 0; i < toKill.Count; i++) world.Destroy(toKill[i]);
    }

    public static int CountEnemies(World world) => world.CountEntities(in EnemyQuery);

    private static readonly QueryDescription PlayerQuery = new QueryDescription().WithAll<PlayerTag>();

    public static int CountPlayers(World world) => world.CountEntities(in PlayerQuery);
}
