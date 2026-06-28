
#pragma once

#include "CoreMinimal.h"

namespace UnrealvsHS::Wire
{
	enum class EInputFlags : uint8
	{
		None = 0,
		Fire = 1 << 0,
	};

	enum class ESnapshotKind : uint8
	{
		Full  = 0,
		Delta = 1,
	};

	enum class ERoundPhase : uint8
	{
		PreGame    = 0,
		InRound    = 1,
		InterRound = 2,
		Victory    = 3,
		Defeat     = 4,
	};

	struct FInputCmd
	{
		uint32      Tick                  = 0;
		uint32      LastAckedServerTick   = 0;
		FVector2D   Move                  = FVector2D::ZeroVector;
		FVector2D   Aim                   = FVector2D(1.0, 0.0);
		EInputFlags Flags                 = EInputFlags::None;

		bool IsFire() const { return ((uint8)Flags & (uint8)EInputFlags::Fire) != 0; }
	};

	struct FPlayerSnap
	{
		uint8     Id            = 0;
		FVector2D Position      = FVector2D::ZeroVector;
		FVector2D Aim           = FVector2D(1.0, 0.0);
		bool      bAlive        = true;
		float     DisableTimer  = 0.0f;
	};

	struct FEnemySnap
	{
		uint16    Id        = 0;
		FVector2D Position  = FVector2D::ZeroVector;
	};

	struct FEnemyDeltaEntry
	{
		uint16    Id        = 0;
		FVector2D Position  = FVector2D::ZeroVector;
	};

	struct FFireEvent
	{
		uint32    Tick        = 0;
		uint8     ShooterId   = 0;
		FVector2D Origin      = FVector2D::ZeroVector;
		FVector2D Direction   = FVector2D(1.0, 0.0);
		float     Distance    = 0.0f;
		uint8     KillCount   = 0;
	};

	struct FSnapshot
	{
		ESnapshotKind          Kind                    = ESnapshotKind::Full;
		uint32                 Tick                    = 0;
		uint32                 BaselineTick            = 0;
		uint32                 LastProcessedInputTick  = 0;
		uint16                 Round                   = 0;
		float                  RoundTimer              = 0.0f;
		float                  InterRoundTimer         = 0.0f;
		ERoundPhase            Phase                   = ERoundPhase::PreGame;
		uint32                 EnemyTotalInWorld       = 0;
		TArray<FPlayerSnap>    Players;
		TArray<FEnemySnap>     Enemies;
		TArray<FFireEvent>     RecentFireEvents;

		void Reset()
		{
			Kind = ESnapshotKind::Full;
			Tick = 0;
			BaselineTick = 0;
			LastProcessedInputTick = 0;
			Round = 0;
			RoundTimer = 0.0f;
			InterRoundTimer = 0.0f;
			Phase = ERoundPhase::PreGame;
			EnemyTotalInWorld = 0;
			Players.Reset();
			Enemies.Reset();
			RecentFireEvents.Reset();
		}
	};

	struct FServerWelcome
	{
		uint32 ProtocolVersion        = 0;
		uint8  PlayerId               = 0;
		uint32 ServerTick             = 0;
		uint16 SimTickMs              = 0;
		uint16 SnapshotEveryNTicks    = 0;
	};
}
