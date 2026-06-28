
#pragma once

#include "CoreMinimal.h"
#include "SimContext.h"

namespace UnrealvsHS::Server::Sim
{
	void TickAdvance(FSimContext& Ctx);
	
	void RoundDirector(FSimContext& Ctx);
	
	void PlayerInput(FSimContext& Ctx);
	
	void ResolveFiresCurrentTick(FSimContext& Ctx);
	
	void RewindResolve(FSimContext& Ctx);
	
	void RewindRecord(FSimContext& Ctx);
	
	void EnemySeek(FSimContext& Ctx);
	
	void SyncChaosToFragments(FSimContext& Ctx);
	
	void PlayerEnemyContact(FSimContext& Ctx);
	
	void CaptureSnapshotFull(const FSimContext& Ctx, Wire::FSnapshot& OutSnap);
}
