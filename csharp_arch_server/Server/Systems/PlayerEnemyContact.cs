using System.Numerics;
using Arch.Core;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Server.Systems;

public static class PlayerEnemyContact
{
    private static readonly QueryDescription EnemyPosQuery = new QueryDescription()
        .WithAll<EnemyTag, Position2D>();

    private static readonly QueryDescription PlayerStateQuery = new QueryDescription()
        .WithAll<PlayerTag, Position2D, DisableTimer, Alive>();

    private static readonly List<Vector2> _enemyPosScratch = new(4096);

    public static void Run(World world, SimContext ctx)
    {
        if (ctx.GodMode) return;

        _enemyPosScratch.Clear();
        world.Query(in EnemyPosQuery, (ref Position2D pos) => _enemyPosScratch.Add(pos.Value));
        var enemyPos = _enemyPosScratch;

        float killRadius = Constants.PlayerKillRadius + Constants.EnemyRadius;
        float killRadiusSq = killRadius * killRadius;
        float disableSeconds = Constants.DisableDurationSec;

        world.Query(in PlayerStateQuery, (ref Position2D pos, ref DisableTimer dt, ref Alive alive) =>
        {
            if (alive.Value == 0) return;
            if (dt.Seconds > 0f) return;

            Vector2 p = pos.Value;
            for (int i = 0; i < enemyPos.Count; i++)
            {
                Vector2 d = enemyPos[i] - p;
                if (d.LengthSquared() <= killRadiusSq)
                {
                    dt.Seconds = disableSeconds;
                    return;
                }
            }
        });
    }
}
