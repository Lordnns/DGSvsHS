
#pragma once

#include "CoreMinimal.h"
#include "Gameplay/UvHSConstants.h"
#include "HAL/PlatformAtomics.h"

namespace UnrealvsHS::Server
{
	class FRecipientSnapshotState
	{
	public:
		uint32 LastAckedServerTick = 0;

		TSet<uint16>                  ConfirmedIds;
		TMap<uint16, uint16>          TicksSinceLastSent;
		
		FThreadSafeCounter            PendingAckedTick;
		
		void OnSnapshotSent(
			uint32 Tick,
			bool   bIsFull,
			const TSet<uint16>&    Included,
			const TArray<uint16>&  Removed);
		
		void OnAckAdvanced();
		
		void Clear();

	private:
		struct FPendingEntry
		{
			uint32       Tick;
			bool         bIsFull;
			TSet<uint16> Included;
			TArray<uint16> Removed;
		};
		
		TArray<FPendingEntry> Pending;

		static constexpr int32 MaxPending = Constants::MaxDeltaDepth * 2;
	};
}
