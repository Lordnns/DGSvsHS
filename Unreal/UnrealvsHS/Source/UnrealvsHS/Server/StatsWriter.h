

#pragma once

#include "CoreMinimal.h"
#include "SimContext.h"

namespace UnrealvsHS::Server
{
	class FStatsWriter
	{
	public:
		void Update(const FSimContext& Ctx, int32 InnerTicksThisFrame, float OuterDeltaSec);

	private:
		float WindowAcc       = 0.0f;
		int32 InnerTicks      = 0;
		int32 OuterFrames     = 0;
		bool  bPrimed         = false;
	};
}
