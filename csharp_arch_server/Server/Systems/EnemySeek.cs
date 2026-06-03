using System.Numerics;
using Arch.Core;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Server.Systems;

public static class EnemySeek
{
    private static readonly QueryDescription ActiveTargetsQuery = new QueryDescription()
        .WithAll<PlayerTag, Position2D, Alive, DisableTimer>();

    private static readonly QueryDescription EnemiesQuery = new QueryDescription()
        .WithAll<EnemyTag, Position2D, Velocity2D>();

    private static readonly List<Vector2> _targetsScratch = new(Constants.MaxPlayers);

    public static void Run(World world, SimContext ctx)
    {
        _targetsScratch.Clear();
        world.Query(in ActiveTargetsQuery, (ref Position2D pos, ref Alive alive, ref DisableTimer dt) =>
        {
            if (!alive.Bool) return;
            if (dt.Seconds > 0f) return;
            _targetsScratch.Add(pos.Value);
        });

        var targets = _targetsScratch;
        float enemySpeed = Constants.EnemySpeed;

        world.Query(in EnemiesQuery, (ref Position2D pos, ref Velocity2D vel) =>
        {
            if (targets.Count == 0) { vel.Value = Vector2.Zero; return; }

            float bestSq = float.MaxValue;
            Vector2 best = targets[0];
            for (int i = 0; i < targets.Count; i++)
            {
                Vector2 d = targets[i] - pos.Value;
                float sq = d.LengthSquared();
                if (sq < bestSq) { bestSq = sq; best = targets[i]; }
            }

            Vector2 dir = best - pos.Value;
            float len = MathF.Sqrt(bestSq);
            if (len > 0.0001f) dir /= len;
            else dir = Vector2.Zero;

            vel.Value = dir * enemySpeed;
        });
    }
}
