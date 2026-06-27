#if WITH_DGS
using Unity.Entities;
using Unity.Mathematics;
using Unity.Physics;
using DGSvsHS.Gameplay;

namespace DGSvsHS.Server.Dots
{
    public static class PhysicsColliders
    {
        private static BlobAssetReference<Unity.Physics.Collider> s_Enemy;
        private static BlobAssetReference<Unity.Physics.Collider> s_Player;

        public static BlobAssetReference<Unity.Physics.Collider> Enemy()
        {
            if (!s_Enemy.IsCreated)
            {
                s_Enemy = Unity.Physics.SphereCollider.Create(new SphereGeometry
                {
                    Center = float3.zero,
                    Radius = Constants.EnemyRadius,
                });
            }
            return s_Enemy;
        }

        public static BlobAssetReference<Unity.Physics.Collider> Player()
        {
            if (!s_Player.IsCreated)
            {
                s_Player = Unity.Physics.SphereCollider.Create(new SphereGeometry
                {
                    Center = float3.zero,
                    Radius = Constants.PlayerRadius,
                });
            }
            return s_Player;
        }
    }
}
#endif
