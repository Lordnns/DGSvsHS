
#pragma once

#include "CoreMinimal.h"
#include "Net/WireTypes.h"

namespace UnrealvsHS::Client
{
	struct FPredictedLocalPlayer
	{
		uint8     Id            = 0;
		FVector2D Position      = FVector2D::ZeroVector;
		FVector2D Aim           = FVector2D(1.0, 0.0);
		float     FireCooldown  = 0.0f;
		float     DisableTimer  = 0.0f;
		bool      bAlive        = true;
	};

	class FClientSimulation
	{
	public:
		// ---------- Setup ----------
		void  SetLocalPlayerId(uint8 InId);
		uint8 GetLocalPlayerId() const { return LocalPlayerId; }
		bool  HasLocalPlayer() const { return bHaveLocalPlayer; }

		// ---------- Prediction ----------
		void PushPredictedInput(const Wire::FInputCmd& Cmd);

		const FPredictedLocalPlayer& GetPredictedLocalPlayer() const { return PredictedLocalPlayer; }
		
		uint32 LastAckedServerTick() const { return bHaveB ? SnapB.Tick : 0u; }

		// ---------- Snapshot intake ----------
		void OnSnapshotReceived(const Wire::FSnapshot& S, double WallTime);

		// ---------- Interpolation queries (renderer) ----------
		bool TryGetInterpolatedPlayer(
			uint8 PlayerId, double WallNow,
			FVector2D& OutPos, FVector2D& OutAim,
			bool& bOutAlive, bool& bOutDisabled) const;

		bool TryGetInterpolatedEnemy(
			uint16 EnemyId, double WallNow,
			FVector2D& OutPos) const;

		const Wire::FSnapshot& GetLatestSnapshot() const { return SnapB; }
		bool                   HasLatestSnapshot() const { return bHaveB; }

		// ---------- Fire event queues ----------
		TArray<Wire::FFireEvent> NewFireEvents;
		TArray<Wire::FFireEvent> NewPredictedFires;

	private:

		bool GetInterpolation(double WallNow, float& OutAlpha) const;
		
		void ApplyInputToPredictedLocalPlayer(const Wire::FInputCmd& Cmd, bool bEmitPredictedFire);
		
		void ReconcileLocalPlayer(const Wire::FSnapshot& S);

		uint8                    LocalPlayerId        = 0;
		bool                     bHaveLocalPlayer     = false;
		FPredictedLocalPlayer    PredictedLocalPlayer;
		
		TArray<Wire::FInputCmd>  PendingInputs;
		
		Wire::FSnapshot          SnapA;
		Wire::FSnapshot          SnapB;
		bool                     bHaveA = false;
		bool                     bHaveB = false;
		double                   SnapARecvTime = 0.0;
		double                   SnapBRecvTime = 0.0;
	};
}
