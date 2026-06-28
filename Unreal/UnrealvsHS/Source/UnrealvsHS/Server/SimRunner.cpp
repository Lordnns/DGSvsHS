#include "SimRunner.h"
#include "SimSystems.h"
#include "Gameplay/UvHSConstants.h"
#include "HAL/PlatformTime.h"

namespace UnrealvsHS::Server
{
	FSimRunner::FSimRunner() {}

	void FSimRunner::RunOneTick(FSimContext& Ctx)
	{
		const double T0 = FPlatformTime::Seconds();
		
		Sim::TickAdvance(Ctx);
		Sim::SyncChaosToFragments(Ctx);
		Sim::RoundDirector(Ctx);
		Sim::PlayerInput(Ctx);
		Sim::RewindResolve(Ctx);
		Sim::EnemySeek(Ctx);
		Sim::PlayerEnemyContact(Ctx);
		Sim::RewindRecord(Ctx);
		Sim::CaptureSnapshotFull(Ctx, LastSnapshot);

		const double T1 = FPlatformTime::Seconds();
		TickWallMsSum += (T1 - T0) * 1000.0;
		TicksSinceReset++;
	}

	int32 FSimRunner::AdvanceWallTime(FSimContext& Ctx, float WallDeltaSec)
	{
		if (Ctx.State != EServerLifecycle::Running) return 0;
		Accumulator += WallDeltaSec;
		int32 Steps = 0;
		while (Accumulator >= Constants::SimDt && Steps < MaxStepsPerCall)
		{
			Accumulator -= Constants::SimDt;
			RunOneTick(Ctx);
			++Steps;
		}
		return Steps;
	}
}
