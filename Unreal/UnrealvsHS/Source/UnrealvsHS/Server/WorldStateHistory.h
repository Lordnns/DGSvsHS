

#pragma once

#include "CoreMinimal.h"
#include "Net/WireTypes.h"

namespace UnrealvsHS::Server
{
	class FWorldStateHistory
	{
	public:
		explicit FWorldStateHistory(int32 InCapacity);

		int32 Capacity()  const { return Cap; }
		int32 Count()     const { return Num; }
		uint32 NewestTick() const;
		uint32 OldestTick() const;

		void Record(const Wire::FSnapshot& Src);
		
		const Wire::FSnapshot* TryGet(uint32 Tick) const;

		void Clear();

	private:
		TArray<Wire::FSnapshot> Ring;
		int32 Cap   = 0;
		int32 Num   = 0;
		int32 Head  = 0; 
	};
}
