#include "UvHSClientPlayerController.h"
#include "UvHSWorldRenderer.h"
#include "Gameplay/UvHSConstants.h"

#include "GameFramework/InputSettings.h"
#include "InputCoreTypes.h"

AUvHSClientPlayerController::AUvHSClientPlayerController()
{
	bShowMouseCursor      = true;
	bEnableMouseOverEvents = false;
	bEnableClickEvents    = false;
}

void AUvHSClientPlayerController::BeginPlay()
{
	Super::BeginPlay();
	SetShowMouseCursor(true);
}

UnrealvsHS::Wire::FInputCmd AUvHSClientPlayerController::SampleInput(
	uint32 ClientTick, const FVector2D& PredictedPlayerPos) const
{
	using namespace UnrealvsHS;

	Wire::FInputCmd Cmd;
	Cmd.Tick = ClientTick;

	FVector CamLoc; FRotator CamRot;
	GetPlayerViewPoint(CamLoc, CamRot);
	const FRotationMatrix CamMtx(CamRot);
	const FVector CamUpW    = CamMtx.GetUnitAxis(EAxis::Z);
	const FVector CamRightW = CamMtx.GetUnitAxis(EAxis::Y);
	
	FVector2D ScreenUp2D   (CamUpW.X,    CamUpW.Y);
	FVector2D ScreenRight2D(CamRightW.X, CamRightW.Y);
	if (ScreenUp2D.SquaredLength()    > 1e-6) ScreenUp2D    = ScreenUp2D.GetSafeNormal();
	if (ScreenRight2D.SquaredLength() > 1e-6) ScreenRight2D = ScreenRight2D.GetSafeNormal();

	FVector2D Move = FVector2D::ZeroVector;
	if (IsInputKeyDown(EKeys::W)) Move += ScreenUp2D;
	if (IsInputKeyDown(EKeys::S)) Move -= ScreenUp2D;
	if (IsInputKeyDown(EKeys::D)) Move += ScreenRight2D;
	if (IsInputKeyDown(EKeys::A)) Move -= ScreenRight2D;
	const double Mag = Move.Size();
	if (Mag > 1.0) Move /= Mag;
	Cmd.Move = Move;

	// ---- Aim (mouse → world plane at EntityZ → direction from player) ----
	FVector RayOrigin, RayDir;
	if (DeprojectMousePositionToWorld(RayOrigin, RayDir))
	{
		const double Z      = (double)Client::FUvHSWorldRenderer::EntityZ;
		const double DirZ   = (double)RayDir.Z;
		FVector2D AimDir(1.0, 0.0);
		if (FMath::Abs(DirZ) > 1e-6)
		{
			const double T   = (Z - (double)RayOrigin.Z) / DirZ;
			const FVector Hit = RayOrigin + RayDir * T;
			
			const FVector2D HitM(
				Hit.X / (double)Client::FUvHSWorldRenderer::UnrealsPerMeter,
				Hit.Y / (double)Client::FUvHSWorldRenderer::UnrealsPerMeter);
			const FVector2D Delta = HitM - PredictedPlayerPos;
			if (Delta.SquaredLength() > 1e-6)
			{
				AimDir = Delta.GetSafeNormal();
			}
		}
		Cmd.Aim = AimDir;
	}
	else
	{
		Cmd.Aim = FVector2D(1.0, 0.0);
	}

	// ---- Fire ----
	const bool bFire = IsInputKeyDown(EKeys::LeftMouseButton);
	Cmd.Flags = bFire ? Wire::EInputFlags::Fire : Wire::EInputFlags::None;

	return Cmd;
}
