
#pragma once

#include "CoreMinimal.h"

namespace UnrealvsHS
{
	struct FDeterministicRng
	{
		uint64 S0 = 0;
		uint64 S1 = 0;

		static FDeterministicRng FromSeed(uint64 Seed);

		uint64 NextU64();
		float  NextFloat01();
		float  NextRange(float Min, float Max) { return Min + (Max - Min) * NextFloat01(); }
	};
}
