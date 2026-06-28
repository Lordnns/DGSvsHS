#include "SnapshotPriority.h"
#include "WireCodec.h"
#include "Gameplay/UvHSConstants.h"

namespace UnrealvsHS::Server
{
	using namespace Wire;
	using namespace Constants;

	void FSnapshotPriority::SelectForFull(
		const FSnapshot&    Current,
		const FVector2D&    RecipientPos,
		int32               EnemyByteBudget,
		TArray<FEnemySnap>& OutSelected,
		TArray<FScoredEnemy>& ScratchScored)
	{
		OutSelected.Reset();
		ScratchScored.Reset();
		ScratchScored.Reserve(Current.Enemies.Num());

		for (int32 i = 0; i < Current.Enemies.Num(); ++i)
		{
			const FEnemySnap& E = Current.Enemies[i];
			const double Dx = (double)E.Position.X - RecipientPos.X;
			const double Dy = (double)E.Position.Y - RecipientPos.Y;
			FScoredEnemy S{ i, (float)(Dx * Dx + Dy * Dy) };
			ScratchScored.Add(S);
		}
		ScratchScored.Sort([](const FScoredEnemy& A, const FScoredEnemy& B) { return A.Score < B.Score; });

		int32 Remaining = EnemyByteBudget;
		const int32 Sz = EnemySnapFullBytes;
		for (int32 s = 0; s < ScratchScored.Num(); ++s)
		{
			if (Sz > Remaining) break;
			Remaining -= Sz;
			OutSelected.Add(Current.Enemies[ScratchScored[s].Index]);
		}
	}

	void FSnapshotPriority::SelectForDelta(
		const FSnapshot&             Current,
		const FSnapshot&             Baseline,
		const FVector2D&             RecipientPos,
		const TSet<uint16>*          ConfirmedIds,
		const TMap<uint16, uint16>*  TicksSinceLastSent,
		int32                        EnemyByteBudget,
		TArray<FEnemyDeltaEntry>&    OutChanged,
		TArray<uint16>&              OutRemoved,
		TArray<FEnemySnap>&          OutAdded,
		TSet<uint16>&                OutIncludedIds,
		TArray<FScoredEnemy>&        ScratchScored,
		TSet<uint16>&                ScratchCurrentIds,
		TMap<uint16, int32>&         ScratchBaselineIndexById)
	{
		OutChanged.Reset();
		OutRemoved.Reset();
		OutAdded.Reset();
		OutIncludedIds.Reset();
		ScratchScored.Reset();
		ScratchCurrentIds.Reset();
		ScratchBaselineIndexById.Reset();

		for (int32 i = 0; i < Current.Enemies.Num(); ++i)
		{
			ScratchCurrentIds.Add(Current.Enemies[i].Id);
		}
		if (ConfirmedIds)
		{
			for (uint16 Cid : *ConfirmedIds)
			{
				if (!ScratchCurrentIds.Contains(Cid)) OutRemoved.Add(Cid);
			}
		}
		else
		{
			for (int32 i = 0; i < Baseline.Enemies.Num(); ++i)
			{
				const uint16 Bid = Baseline.Enemies[i].Id;
				if (!ScratchCurrentIds.Contains(Bid)) OutRemoved.Add(Bid);
			}
		}

		for (int32 i = 0; i < Baseline.Enemies.Num(); ++i)
		{
			ScratchBaselineIndexById.Add(Baseline.Enemies[i].Id, i);
		}
		
		int32 RemovedBytes = OutRemoved.Num() * 2;
		if (RemovedBytes > EnemyByteBudget)
		{
			const int32 Keepable = EnemyByteBudget / 2;
			if (Keepable < OutRemoved.Num())
			{
				OutRemoved.RemoveAt(Keepable, OutRemoved.Num() - Keepable, EAllowShrinking::No);
			}
			RemovedBytes = OutRemoved.Num() * 2;
		}
		int32 Remaining = FMath::Max(0, EnemyByteBudget - RemovedBytes);
		
		ScratchScored.Reset();
		const bool bHaveConfirmed = ConfirmedIds != nullptr;
		for (int32 i = 0; i < Current.Enemies.Num(); ++i)
		{
			const FEnemySnap& E = Current.Enemies[i];
			const bool bPendingSpawn = !bHaveConfirmed || !ConfirmedIds->Contains(E.Id);
			if (!bPendingSpawn) continue;
			const double Dx = (double)E.Position.X - RecipientPos.X;
			const double Dy = (double)E.Position.Y - RecipientPos.Y;
			ScratchScored.Add({ i, (float)(Dx * Dx + Dy * Dy) });
		}
		ScratchScored.Sort([](const FScoredEnemy& A, const FScoredEnemy& B) { return A.Score < B.Score; });

		int32 SpawnsThisSnapshot = 0;
		const int32 NewEntrySize = EnemySnapFullBytes;
		for (int32 s = 0; s < ScratchScored.Num(); ++s)
		{
			if (SpawnsThisSnapshot >= Constants::MaxSpawnsPerSnapshot) break;
			if (Remaining < NewEntrySize) break;
			const int32 Idx = ScratchScored[s].Index;
			const FEnemySnap& E = Current.Enemies[Idx];
			OutAdded.Add(E);
			OutIncludedIds.Add(E.Id);
			Remaining -= NewEntrySize;
			++SpawnsThisSnapshot;
		}
		
		ScratchScored.Reset();
		for (int32 i = 0; i < Current.Enemies.Num(); ++i)
		{
			const FEnemySnap& E = Current.Enemies[i];
			if (!bHaveConfirmed || !ConfirmedIds->Contains(E.Id)) continue;
			if (!ScratchBaselineIndexById.Contains(E.Id))         continue;
			const double Dx = (double)E.Position.X - RecipientPos.X;
			const double Dy = (double)E.Position.Y - RecipientPos.Y;
			const float Dist = (float)FMath::Sqrt(Dx * Dx + Dy * Dy);
			uint16 Tsls = 0;
			if (TicksSinceLastSent)
			{
				if (const uint16* P = TicksSinceLastSent->Find(E.Id)) Tsls = *P;
			}
			const float Score = Dist - Constants::StalenessWeight * (float)Tsls;
			ScratchScored.Add({ i, Score });
		}
		ScratchScored.Sort([](const FScoredEnemy& A, const FScoredEnemy& B) { return A.Score < B.Score; });

		const int32 ChangedEntrySize = EnemyDeltaEntryBytes;
		for (int32 s = 0; s < ScratchScored.Num(); ++s)
		{
			const int32 Idx = ScratchScored[s].Index;
			const FEnemySnap& E = Current.Enemies[Idx];
			const int32 BaseIdx = ScratchBaselineIndexById[E.Id];
			const FEnemySnap& B = Baseline.Enemies[BaseIdx];
			if (!FCodec::EnemyPositionChanged(B, E))
			{
				OutIncludedIds.Add(E.Id);
				continue;
			}
			if (ChangedEntrySize > Remaining) break;
			Remaining -= ChangedEntrySize;
			Wire::FEnemyDeltaEntry D;
			D.Id       = E.Id;
			D.Position = E.Position;
			OutChanged.Add(D);
			OutIncludedIds.Add(E.Id);
		}
	}
}
