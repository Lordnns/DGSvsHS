#if WITH_DGS
using Unity.Burst;
using Unity.Entities;
using Unity.Mathematics;
using Unity.Physics;
using Unity.Physics.Systems;
using Unity.Transforms;
using DGSvsHS.Gameplay;

namespace DGSvsHS.Server.Dots
{
    [DisableAutoCreation]
    [UpdateInGroup(typeof(FixedStepSimulationSystemGroup))]
    [UpdateBefore(typeof(PhysicsSystemGroup))]
    public partial struct SyncPos2DToLocalTransformSystem : ISystem
    {
        private int _logFrame;

        public void OnUpdate(ref SystemState state)
        {
            if (++_logFrame % 60 == 0)
            {
                UnityEngine.Debug.Log($"[SyncIn] run dt={state.WorldUnmanaged.Time.DeltaTime:F4}");
            }
            new SyncPlayerJob().ScheduleParallel();
        }

        [BurstCompile]
        [WithAll(typeof(PlayerTag))]
        private partial struct SyncPlayerJob : IJobEntity
        {
            public void Execute(in Position2D pos, ref LocalTransform xf)
            {
                xf.Position = new float3(pos.Value.x, pos.Value.y, 0f);
            }
        }
    }

    // -------- Post-physics: write LocalTransform → Position2D / Velocity2D, clamp arena --------
    [DisableAutoCreation]
    [UpdateInGroup(typeof(FixedStepSimulationSystemGroup))]
    [UpdateAfter(typeof(PhysicsSystemGroup))]
    public partial struct SyncPhysicsToPos2DSystem : ISystem
    {
        private int _logFrame;

        public void OnUpdate(ref SystemState state)
        {
            _logFrame++;
            if (_logFrame % 60 == 0)
            {
                float dt = state.WorldUnmanaged.Time.DeltaTime;

                int physWorldDyn = -1, physWorldStat = -1;
                if (SystemAPI.HasSingleton<Unity.Physics.PhysicsWorldSingleton>())
                {
                    var pw = SystemAPI.GetSingleton<Unity.Physics.PhysicsWorldSingleton>();
                    physWorldDyn = pw.PhysicsWorld.NumDynamicBodies;
                    physWorldStat = pw.PhysicsWorld.NumStaticBodies;
                }

                Unity.Transforms.LocalTransform firstXf = default;
                Unity.Physics.PhysicsVelocity firstPv = default;
                bool any = false;
                foreach (var (xf, pv) in SystemAPI
                             .Query<RefRO<Unity.Transforms.LocalTransform>, RefRO<Unity.Physics.PhysicsVelocity>>()
                             .WithAll<EnemyTag>())
                {
                    firstXf = xf.ValueRO;
                    firstPv = pv.ValueRO;
                    any = true;
                    break;
                }
                if (any)
                {
                    UnityEngine.Debug.Log(
                        $"[Sync] dt={dt:F4} PhysicsWorld={physWorldDyn}dyn/{physWorldStat}stat " +
                        $"xf=({firstXf.Position.x:F3},{firstXf.Position.y:F3}) " +
                        $"pv=({firstPv.Linear.x:F3},{firstPv.Linear.y:F3})");
                }
                else
                {
                    UnityEngine.Debug.Log($"[Sync] dt={dt:F4} PhysicsWorld={physWorldDyn}dyn/{physWorldStat}stat (no enemies yet)");
                }
            }

            float enemyMax = Constants.ArenaRadius - Constants.EnemyRadius;

            new SyncEnemyJob { ArenaMaxRadius = enemyMax }.ScheduleParallel();
            new PinPlayerJob().ScheduleParallel();
        }

        [BurstCompile]
        [WithAll(typeof(EnemyTag))]
        private partial struct SyncEnemyJob : IJobEntity
        {
            public float ArenaMaxRadius;

            public void Execute(ref Position2D pos, ref Velocity2D vel,
                                ref LocalTransform xf, ref PhysicsVelocity pv)
            {
                xf.Position.z = 0f; // defend against any Z drift
                float r = math.length(xf.Position.xy);
                if (r > ArenaMaxRadius && r > 0f)
                {
                    float s = ArenaMaxRadius / r;
                    xf.Position.x *= s;
                    xf.Position.y *= s;
                }
                pos.Value = xf.Position.xy;
                vel.Value = pv.Linear.xy;
            }
        }

        [BurstCompile]
        [WithAll(typeof(PlayerTag))]
        private partial struct PinPlayerJob : IJobEntity
        {
            public void Execute(in Position2D pos, ref LocalTransform xf, ref PhysicsVelocity pv)
            {
                xf.Position = new float3(pos.Value.x, pos.Value.y, 0f);
                pv.Linear = float3.zero;
                pv.Angular = float3.zero;
            }
        }
    }
}
#endif
