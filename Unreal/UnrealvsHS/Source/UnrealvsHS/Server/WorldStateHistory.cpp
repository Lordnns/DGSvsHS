#include "WorldStateHistory.h"

namespace UnrealvsHS::Server
{
	FWorldStateHistory::FWorldStateHistory(int32 InCapacity)
		: Cap(FMath::Max(1, InCapacity))
	{
		Ring.SetNum(Cap);
	}

	void FWorldStateHistory::Record(const Wire::FSnapshot& Src)
	{
		Wire::FSnapshot& Dst = Ring[Head];
		Dst.Reset();
		Dst.Kind                    = Src.Kind;
		Dst.Tick                    = Src.Tick;
		Dst.BaselineTick            = Src.BaselineTick;
		Dst.LastProcessedInputTick  = Src.LastProcessedInputTick;
		Dst.Round                   = Src.Round;
		Dst.RoundTimer              = Src.RoundTimer;
		Dst.InterRoundTimer         = Src.InterRoundTimer;
		Dst.Phase                   = Src.Phase;
		Dst.EnemyTotalInWorld       = Src.EnemyTotalInWorld;
		Dst.Players.Append(Src.Players);
		Dst.Enemies.Append(Src.Enemies);
		Dst.RecentFireEvents.Append(Src.RecentFireEvents);

		Head = (Head + 1) % Cap;
		if (Num < Cap) ++Num;
	}

	const Wire::FSnapshot* FWorldStateHistory::TryGet(uint32 Tick) const
	{
		for (int32 i = 0; i < Num; ++i)
		{
			const int32 Idx = (Head - 1 - i + Cap) % Cap;
			if (Ring[Idx].Tick == Tick) return &Ring[Idx];
		}
		return nullptr;
	}

	uint32 FWorldStateHistory::NewestTick() const
	{
		if (Num == 0) return 0;
		const int32 Idx = (Head - 1 + Cap) % Cap;
		return Ring[Idx].Tick;
	}

	uint32 FWorldStateHistory::OldestTick() const
	{
		if (Num == 0) return 0;
		const int32 Idx = (Head - Num + Cap) % Cap;
		return Ring[Idx].Tick;
	}

	void FWorldStateHistory::Clear()
	{
		Num = 0;
		Head = 0;
	}
}
