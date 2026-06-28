#include "UvHSAutoPilot.h"
#include "Gameplay/UvHSConstants.h"
#include "Gameplay/DeterministicRng.h"

namespace UnrealvsHS::Client
{
	FUvHSAutoPilot::FUvHSAutoPilot(uint8 InBotId, uint64 InSeed)
		: BotId(InBotId), Seed(InSeed)
	{
		FDeterministicRng Rng = FDeterministicRng::FromSeed(Seed ^ (0xA5A5A5A5ULL + BotId));

		OrbitRadius       = Rng.NextRange(Constants::ArenaRadius * 0.4f, Constants::ArenaRadius * 0.75f);
		OrbitAngularSpeed = Rng.NextRange(0.5f, 1.2f);
		OrbitPhase        = Rng.NextRange(0.f, 2.f * PI);

		AimAngularSpeed   = Rng.NextRange(2.0f, 4.5f);
		AimPhase          = Rng.NextRange(0.f, 2.f * PI);
		
		FireOnTicks  = FMath::RoundToInt(Rng.NextRange(30.f, 60.f));
		FireOffTicks = FMath::RoundToInt(Rng.NextRange(6.f, 18.f));
	}

	Wire::FInputCmd FUvHSAutoPilot::Sample(uint32 ClientTick, const FVector2D& PredictedLocalPos) const
	{
		const float T = ClientTick * Constants::SimDt;

		// ---- Move: walk toward the next point on the orbit circle. ----
		const float OrbitAngle = OrbitPhase + OrbitAngularSpeed * T;
		const FVector2D Target(FMath::Cos(OrbitAngle) * OrbitRadius,
		                       FMath::Sin(OrbitAngle) * OrbitRadius);
		FVector2D ToTarget = Target - PredictedLocalPos;
		FVector2D Move = (ToTarget.SquaredLength() > 0.04) ? ToTarget.GetSafeNormal() : FVector2D::ZeroVector;

		// ---- Aim: pure tick-driven rotation, independent of snapshots. ----
		const float AimAngle = AimPhase + AimAngularSpeed * T;
		const FVector2D Aim(FMath::Cos(AimAngle), FMath::Sin(AimAngle));

		// ---- Fire: fixed duty cycle in ticks. ----
		const int32 Period = FireOnTicks + FireOffTicks;
		const int32 Phase  = Period > 0 ? (int32)(ClientTick % (uint32)Period) : 0;
		const bool  bFire  = Phase < FireOnTicks;

		Wire::FInputCmd Cmd;
		Cmd.Tick  = ClientTick;
		Cmd.Move  = Move;
		Cmd.Aim   = Aim;
		Cmd.Flags = bFire ? Wire::EInputFlags::Fire : Wire::EInputFlags::None;
		return Cmd;
	}
}
