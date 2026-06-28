
#pragma once

#include "CoreMinimal.h"
#include "Net/WireTypes.h"

namespace UnrealvsHS::Server
{
	class FSnapshotPriority
	{
	public:
		struct FScoredEnemy
		{
			int32 Index;
			float Score;
		};

		static void SelectForFull(
			const Wire::FSnapshot&    Current,
			const FVector2D&          RecipientPos,
			int32                     EnemyByteBudget,
			TArray<Wire::FEnemySnap>& OutSelected,
			TArray<FScoredEnemy>&     ScratchScored);

		static void SelectForDelta(
			const Wire::FSnapshot&            Current,
			const Wire::FSnapshot&            Baseline,
			const FVector2D&                  RecipientPos,
			const TSet<uint16>*               ConfirmedIds,
			const TMap<uint16, uint16>*       TicksSinceLastSent,
			int32                             EnemyByteBudget,
			TArray<Wire::FEnemyDeltaEntry>&   OutChanged,
			TArray<uint16>&                   OutRemoved,
			TArray<Wire::FEnemySnap>&         OutAdded,
			TSet<uint16>&                     OutIncludedIds,
			TArray<FScoredEnemy>&             ScratchScored,
			TSet<uint16>&                     ScratchCurrentIds,
			TMap<uint16, int32>&              ScratchBaselineIndexById);
	};
}
