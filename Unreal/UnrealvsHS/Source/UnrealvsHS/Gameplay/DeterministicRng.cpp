#include "DeterministicRng.h"

namespace UnrealvsHS
{
	static FORCEINLINE uint64 SplitMix64(uint64& State)
	{
		State += 0x9E3779B97F4A7C15ULL;
		uint64 Z = State;
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBULL;
		return Z ^ (Z >> 31);
	}

	static FORCEINLINE uint64 RotateLeft(uint64 X, int K)
	{
		return (X << K) | (X >> (64 - K));
	}

	FDeterministicRng FDeterministicRng::FromSeed(uint64 Seed)
	{
		FDeterministicRng Rng;
		uint64 Sm = Seed;
		Rng.S0 = SplitMix64(Sm);
		Rng.S1 = SplitMix64(Sm);
		if ((Rng.S0 | Rng.S1) == 0)
		{
			Rng.S1 = 1;
		}
		return Rng;
	}

	uint64 FDeterministicRng::NextU64()
	{
		const uint64 s0 = S0;
		uint64 s1 = S1;
		const uint64 Result = s0 + s1;
		s1 ^= s0;
		S0 = RotateLeft(s0, 24) ^ s1 ^ (s1 << 16);
		S1 = RotateLeft(s1, 37);
		return Result;
	}

	float FDeterministicRng::NextFloat01()
	{
		return (NextU64() >> 40) * (1.0f / 16777216.0f);
	}
}
