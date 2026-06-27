#if WITH_DGS
using Unity.Burst;
using Unity.Collections;
using Unity.Entities;
using Unity.Mathematics;
using Unity.Physics;
using DGSvsHS.Gameplay;
using Unity.Physics.Extensions;

namespace DGSvsHS.Server.Dots
{
    [UpdateInGroup(typeof(SimulationSystemGroup))]
    [UpdateAfter(typeof(PlayerInputSystem))]
    public partial struct EnemySeekSystem : ISystem
    {
        public void OnCreate(ref SystemState state)
        {
            state.RequireForUpdate<EnemyTag>();
        }

        public void OnUpdate(ref SystemState state)
        {
            var playerTargets = new NativeList<float2>(Constants.MaxPlayers, Allocator.TempJob);
            foreach (var (pos, alive, dt) in
                     SystemAPI.Query<RefRO<Position2D>, RefRO<Alive>, RefRO<DisableTimer>>()
                              .WithAll<PlayerTag>())
            {
                if (!alive.ValueRO.Bool) continue;
                if (dt.ValueRO.Seconds > 0f) continue;
                playerTargets.Add(pos.ValueRO.Value);
            }

            new SeekJob
            {
                PlayerTargets = playerTargets.AsArray(),
                ImpulseMagnitude = Constants.EnemyDriveForce * Constants.SimDt,
            }.ScheduleParallel();

            state.Dependency = playerTargets.Dispose(state.Dependency);
        }

        [BurstCompile]
        [WithAll(typeof(EnemyTag))]
        private partial struct SeekJob : IJobEntity
        {
            [ReadOnly] public NativeArray<float2> PlayerTargets;
            public float ImpulseMagnitude;

            public void Execute(in Position2D pos, in PhysicsMass mass, ref PhysicsVelocity vel)
            {
                if (PlayerTargets.Length == 0) return;

                float bestSq = float.MaxValue;
                float2 best = PlayerTargets[0];
                for (int i = 0; i < PlayerTargets.Length; i++)
                {
                    float2 d = PlayerTargets[i] - pos.Value;
                    float sq = math.lengthsq(d);
                    if (sq < bestSq) { bestSq = sq; best = PlayerTargets[i]; }
                }

                float len = math.sqrt(bestSq);
                if (len <= 0.0001f) return;
                float2 dir = (best - pos.Value) / len;

                float3 impulse = new float3(dir.x * ImpulseMagnitude, dir.y * ImpulseMagnitude, 0f);
                vel.ApplyLinearImpulse(in mass, impulse);
            }
        }
    }
}
#endif
