#include "ClientSimulation.h"
#include "Gameplay/UvHSConstants.h"

namespace UnrealvsHS::Client
{
	using namespace Constants;

	void FClientSimulation::SetLocalPlayerId(uint8 InId)
	{
		LocalPlayerId      = InId;
		bHaveLocalPlayer   = false;
		PendingInputs.Reset();
	}

	// ---------- Prediction ----------

	void FClientSimulation::PushPredictedInput(const Wire::FInputCmd& Cmd)
	{
		PendingInputs.Add(Cmd);

		while (PendingInputs.Num() > MaxDeltaDepth)
		{
			PendingInputs.RemoveAt(0, EAllowShrinking::No);
		}

		if (!bHaveLocalPlayer) return;
		ApplyInputToPredictedLocalPlayer(Cmd, /*bEmitPredictedFire=*/ true);
	}

	void FClientSimulation::ApplyInputToPredictedLocalPlayer(const Wire::FInputCmd& Cmd, bool bEmitPredictedFire)
	{
		if (!PredictedLocalPlayer.bAlive)
		{
			PredictedLocalPlayer.FireCooldown = FMath::Max(0.0f, PredictedLocalPlayer.FireCooldown - SimDt);
			PredictedLocalPlayer.DisableTimer = FMath::Max(0.0f, PredictedLocalPlayer.DisableTimer - SimDt);
			return;
		}
		
		FVector2D Move = Cmd.Move;
		const double Mag = Move.Size();
		if (Mag > 1.0) Move /= Mag;
		PredictedLocalPlayer.Position += Move * (double)PlayerSpeed * (double)SimDt;

		const double R    = PredictedLocalPlayer.Position.Size();
		const double MaxR = (double)(ArenaRadius - PlayerRadius);
		if (R > MaxR) PredictedLocalPlayer.Position *= (MaxR / R);

		if (Cmd.Aim.SquaredLength() > 1e-4)
		{
			PredictedLocalPlayer.Aim = Cmd.Aim.GetSafeNormal();
		}

		if (Cmd.IsFire()
			&& PredictedLocalPlayer.FireCooldown <= 0.0f
			&& PredictedLocalPlayer.DisableTimer <= 0.0f)
		{
			PredictedLocalPlayer.FireCooldown = PlayerFireCooldownSec;
			if (bEmitPredictedFire)
			{
				Wire::FFireEvent F;
				F.Tick      = Cmd.Tick;
				F.ShooterId = LocalPlayerId;
				F.Origin    = PredictedLocalPlayer.Position;
				F.Direction = PredictedLocalPlayer.Aim;
				F.Distance  = BulletMaxRange;
				F.KillCount = 0;
				NewPredictedFires.Add(F);
			}
		}

		PredictedLocalPlayer.FireCooldown = FMath::Max(0.0f, PredictedLocalPlayer.FireCooldown - SimDt);
		PredictedLocalPlayer.DisableTimer = FMath::Max(0.0f, PredictedLocalPlayer.DisableTimer - SimDt);
	}

	// ---------- Snapshot intake ----------

	void FClientSimulation::OnSnapshotReceived(const Wire::FSnapshot& S, double WallTime)
	{

		if (bHaveB)
		{
			SnapA          = SnapB;
			SnapARecvTime  = SnapBRecvTime;
			bHaveA         = true;
		}
		SnapB         = S;
		SnapBRecvTime = WallTime;
		bHaveB        = true;
		
		for (const Wire::FFireEvent& Ev : SnapB.RecentFireEvents)
		{
			if (Ev.ShooterId == LocalPlayerId) continue;
			NewFireEvents.Add(Ev);
		}

		ReconcileLocalPlayer(SnapB);
	}

	void FClientSimulation::ReconcileLocalPlayer(const Wire::FSnapshot& S)
	{

		const Wire::FPlayerSnap* Mine = nullptr;
		for (const Wire::FPlayerSnap& P : S.Players)
		{
			if (P.Id == LocalPlayerId) { Mine = &P; break; }
		}
		if (!Mine) return;

		PredictedLocalPlayer.Id           = LocalPlayerId;
		PredictedLocalPlayer.Position     = Mine->Position;
		PredictedLocalPlayer.Aim          = Mine->Aim;
		PredictedLocalPlayer.DisableTimer = Mine->DisableTimer;
		PredictedLocalPlayer.bAlive       = Mine->bAlive;
		bHaveLocalPlayer = true;
		
		while (PendingInputs.Num() > 0 && PendingInputs[0].Tick <= S.LastProcessedInputTick)
		{
			PendingInputs.RemoveAt(0, EAllowShrinking::No);
		}

		for (const Wire::FInputCmd& Cmd : PendingInputs)
		{
			ApplyInputToPredictedLocalPlayer(Cmd, /*bEmitPredictedFire=*/ false);
		}
	}

	// ---------- Interpolation queries ----------

	bool FClientSimulation::GetInterpolation(double WallNow, float& OutAlpha) const
	{
		OutAlpha = 1.0f;
		if (!bHaveA || !bHaveB) return false;

		const double RenderTime = WallNow - (double)(InterpolationBufferMs / 1000.0f);
		if (RenderTime <= SnapARecvTime) { OutAlpha = 0.0f; return true; }
		if (RenderTime >= SnapBRecvTime) { OutAlpha = 1.0f; return true; }

		const double Span = SnapBRecvTime - SnapARecvTime;
		OutAlpha = Span > 0.0 ? (float)((RenderTime - SnapARecvTime) / Span) : 1.0f;
		return true;
	}

	bool FClientSimulation::TryGetInterpolatedPlayer(
		uint8 PlayerId, double WallNow,
		FVector2D& OutPos, FVector2D& OutAim,
		bool& bOutAlive, bool& bOutDisabled) const
	{
		OutPos       = FVector2D::ZeroVector;
		OutAim       = FVector2D(1.0, 0.0);
		bOutAlive    = false;
		bOutDisabled = false;

		float Alpha = 1.0f;
		if (!GetInterpolation(WallNow, Alpha)) return false;

		const Wire::FPlayerSnap* From = nullptr;
		const Wire::FPlayerSnap* To   = nullptr;
		for (const Wire::FPlayerSnap& P : SnapA.Players) if (P.Id == PlayerId) { From = &P; break; }
		for (const Wire::FPlayerSnap& P : SnapB.Players) if (P.Id == PlayerId) { To   = &P; break; }
		if (!From || !To) return false;

		OutPos       = FMath::Lerp(From->Position, To->Position, (double)Alpha);
		OutAim       = FMath::Lerp(From->Aim, To->Aim, (double)Alpha).GetSafeNormal();
		bOutAlive    = To->bAlive;
		bOutDisabled = To->DisableTimer > 0.0f;
		return true;
	}

	bool FClientSimulation::TryGetInterpolatedEnemy(
		uint16 EnemyId, double WallNow,
		FVector2D& OutPos) const
	{
		OutPos = FVector2D::ZeroVector;

		float Alpha = 1.0f;
		if (!GetInterpolation(WallNow, Alpha)) return false;

		const Wire::FEnemySnap* From = nullptr;
		const Wire::FEnemySnap* To   = nullptr;
		for (const Wire::FEnemySnap& E : SnapA.Enemies) if (E.Id == EnemyId) { From = &E; break; }
		for (const Wire::FEnemySnap& E : SnapB.Enemies) if (E.Id == EnemyId) { To   = &E; break; }
		if (!From || !To) return false;

		OutPos = FMath::Lerp(From->Position, To->Position, (double)Alpha);
		return true;
	}
}
