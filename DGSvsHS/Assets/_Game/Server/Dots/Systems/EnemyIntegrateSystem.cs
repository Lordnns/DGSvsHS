#if WITH_DGS
// EnemyIntegrateSystem is intentionally empty — DOTS Physics solver now
// integrates enemy velocity into position during PhysicsSystemGroup, between
// EnemySeekSystem (force application) and SyncPhysicsToPos2DSystem (write
// back to Position2D/Velocity2D + arena clamp). See PhysicsSyncSystems.cs.
#endif
