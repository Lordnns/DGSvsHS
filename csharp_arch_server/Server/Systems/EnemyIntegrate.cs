using Arch.Core;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Server.Systems;

public static class EnemyIntegrate
{
    private static readonly QueryDescription EnemiesQuery = new QueryDescription()
        .WithAll<EnemyTag, Position2D, Velocity2D>();

    public static void Run(World world)
    {
        float dt = Constants.SimDt;
        world.Query(in EnemiesQuery, (ref Position2D pos, ref Velocity2D vel) =>
        {
            pos.Value += vel.Value * dt;
        });
    }
}
