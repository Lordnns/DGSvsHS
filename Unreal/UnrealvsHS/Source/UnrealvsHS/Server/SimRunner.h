

#pragma once

#include "CoreMinimal.h"
#include "SimContext.h"

namespace UnrealvsHS::Server
{
	class FSimRunner
	{
	public:
		FSimRunner();
		
		int32 AdvanceWallTime(FSimContext& Ctx, float WallDeltaSec);
		
		const Wire::FSnapshot& GetLastSnapshot() const { return LastSnapshot; }
		
		double GetTickWallMsSum() const   { return TickWallMsSum; }
		int32  GetTickCountSince() const  { return TicksSinceReset; }
		void   ResetTickStats()           { TickWallMsSum = 0.0; TicksSinceReset = 0; }

	private:
		float           Accumulator        = 0.0f;
		int32           MaxStepsPerCall    = 5;
		Wire::FSnapshot LastSnapshot;
		double          TickWallMsSum      = 0.0;
		int32           TicksSinceReset    = 0;

		void RunOneTick(FSimContext& Ctx);
	};
}
