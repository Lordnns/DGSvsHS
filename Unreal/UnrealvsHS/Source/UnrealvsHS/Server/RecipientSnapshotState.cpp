#include "RecipientSnapshotState.h"

namespace UnrealvsHS::Server
{
	void FRecipientSnapshotState::OnSnapshotSent(
		uint32 Tick,
		bool   bIsFull,
		const TSet<uint16>&    Included,
		const TArray<uint16>&  Removed)
	{
		while (Pending.Num() >= MaxPending)
		{
			Pending.RemoveAt(0, EAllowShrinking::No);
		}

		FPendingEntry Entry;
		Entry.Tick    = Tick;
		Entry.bIsFull = bIsFull;
		Entry.Included = Included;
		Entry.Removed  = Removed;
		Pending.Add(MoveTemp(Entry));
		
		TArray<uint16> Keys;
		TicksSinceLastSent.GetKeys(Keys);
		for (uint16 Id : Keys)
		{
			if (Included.Contains(Id))
			{
				TicksSinceLastSent[Id] = 0;
			}
			else
			{
				uint16& Cur = TicksSinceLastSent[Id];
				if (Cur < MAX_uint16) ++Cur;
			}
		}
	}

	void FRecipientSnapshotState::OnAckAdvanced()
	{
		int32 WriteIdx = 0;
		for (int32 i = 0; i < Pending.Num(); ++i)
		{
			FPendingEntry& P = Pending[i];
			if (P.Tick > LastAckedServerTick)
			{
				if (WriteIdx != i) Pending[WriteIdx] = MoveTemp(P);
				++WriteIdx;
				continue;
			}

			if (P.bIsFull)
			{
				ConfirmedIds.Reset();
				TicksSinceLastSent.Reset();
				for (uint16 Id : P.Included)
				{
					ConfirmedIds.Add(Id);
					TicksSinceLastSent.Add(Id, 0);
				}
			}
			else
			{
				for (uint16 Id : P.Included)
				{
					ConfirmedIds.Add(Id);
					if (!TicksSinceLastSent.Contains(Id)) TicksSinceLastSent.Add(Id, 0);
				}
				for (uint16 Id : P.Removed)
				{
					ConfirmedIds.Remove(Id);
					TicksSinceLastSent.Remove(Id);
				}
			}
		}
		if (WriteIdx < Pending.Num())
		{
			Pending.RemoveAt(WriteIdx, Pending.Num() - WriteIdx, EAllowShrinking::No);
		}
	}

	void FRecipientSnapshotState::Clear()
	{
		LastAckedServerTick = 0;
		ConfirmedIds.Reset();
		TicksSinceLastSent.Reset();
		Pending.Reset();
		PendingAckedTick.Reset();
	}
}
