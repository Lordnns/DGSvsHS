
#pragma once

#include "CoreMinimal.h"
#include "Net/WireTypes.h"

namespace UnrealvsHS::Client
{
	class FUvHSAutoPilot
	{
	public:
		FUvHSAutoPilot(uint8 InBotId, uint64 InSeed);

		Wire::FInputCmd Sample(uint32 ClientTick, const FVector2D& PredictedLocalPos) const;

		uint8  GetBotId() const { return BotId; }
		uint64 GetSeed()  const { return Seed; }

	private:
		uint8  BotId = 0;
		uint64 Seed  = 0;
		
		float OrbitRadius        = 0.f;
		float OrbitAngularSpeed  = 0.f;
		float OrbitPhase         = 0.f;
		
		float AimAngularSpeed    = 0.f;
		float AimPhase           = 0.f;

		int32 FireOnTicks        = 1;
		int32 FireOffTicks       = 1;
	};
}
