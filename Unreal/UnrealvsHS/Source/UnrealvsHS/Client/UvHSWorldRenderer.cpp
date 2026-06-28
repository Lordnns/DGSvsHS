#include "UvHSWorldRenderer.h"
#include "Gameplay/UvHSConstants.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "DrawDebugHelpers.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogUvHSRenderer, Log, All);

namespace UnrealvsHS::Client
{
	using namespace Constants;

	static const FLinearColor LocalPlayerColor  = FLinearColor(0.30f, 0.90f, 1.00f, 1.0f);
	static const FLinearColor RemotePlayerColor = FLinearColor(1.00f, 0.90f, 0.30f, 1.0f);
	static const FLinearColor EnemyColor        = FLinearColor(1.00f, 0.30f, 0.30f, 1.0f);
	static const FLinearColor BeamLocalColor    = FLinearColor(0.50f, 1.00f, 0.80f, 1.0f);
	static const FLinearColor BeamRemoteColor   = FLinearColor(1.00f, 0.80f, 0.20f, 1.0f);
	static const FLinearColor FloorColor        = FLinearColor(0.05f, 0.05f, 0.08f, 1.0f);
	
	static constexpr int32 EnemyPoolHardCap = 1024;

	void FUvHSWorldRenderer::Initialize(UWorld* InWorld)
	{
		if (bInitialized) return;
		if (!InWorld)
		{
			UE_LOG(LogUvHSRenderer, Error, TEXT("Initialize called with null world"));
			return;
		}
		World = InWorld;
		
		{
			FActorSpawnParameters P; P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector::ZeroVector, FRotator::ZeroRotator, P))
			{
				Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
				Sky->GetLightComponent()->SetIntensity(1.0f);
				
				Sky->GetLightComponent()->SetRealTimeCaptureEnabled(true);
				Sky->GetLightComponent()->RecaptureSky();
				SkyLightActor = Sky;
			}
			if (ADirectionalLight* Dir = World->SpawnActor<ADirectionalLight>(
				FVector(0, 0, 5000), FRotator(-60.0f, 30.0f, 0.0f), P))
			{
				Dir->GetLightComponent()->SetMobility(EComponentMobility::Movable);
				Dir->GetLightComponent()->SetIntensity(5.0f);
				DirLightActor = Dir;
			}
		}
		
		FloorActor = SpawnFloor();
		
		PlayerActors.SetNum(MaxPlayers);
		PlayerMIDs.SetNum(MaxPlayers);
		for (int32 i = 0; i < MaxPlayers; ++i)
		{
			PlayerActors[i] = SpawnSphereActor(PlayerRadius * 2.0f);
			if (PlayerActors[i])
			{
				PlayerActors[i]->SetActorHiddenInGame(true);
				ApplyMaterialColor(PlayerActors[i], RemotePlayerColor);
				
				if (UStaticMeshComponent* M = PlayerActors[i]->GetStaticMeshComponent())
				{
					if (UMaterialInterface* Mat = M->GetMaterial(0))
					{
						if (auto* MID = Cast<UMaterialInstanceDynamic>(Mat))
						{
							PlayerMIDs[i] = MID;
						}
					}
				}
			}
		}

		bInitialized = true;
		UE_LOG(LogUvHSRenderer, Display,
			TEXT("[UvHSWorldRenderer] initialized: floor + %d player slots"), MaxPlayers);
	}

	void FUvHSWorldRenderer::Shutdown()
	{
		if (!World) return;

		if (FloorActor)    { FloorActor->Destroy();    FloorActor = nullptr; }
		if (SkyLightActor) { SkyLightActor->Destroy(); SkyLightActor = nullptr; }
		if (DirLightActor) { DirLightActor->Destroy(); DirLightActor = nullptr; }
		for (AStaticMeshActor* A : PlayerActors) if (A) A->Destroy();
		for (AStaticMeshActor* A : EnemyActors)  if (A) A->Destroy();
		PlayerActors.Reset();
		PlayerMIDs.Reset();
		EnemyActors.Reset();
		EnemyMIDs.Reset();

		bInitialized = false;
		World = nullptr;
	}

	// ---------- Per-frame rendering ----------

	void FUvHSWorldRenderer::BeginFrame()
	{
		if (!bInitialized) return;
		for (AStaticMeshActor* A : PlayerActors) if (A) A->SetActorHiddenInGame(true);

		for (int32 i = 0; i < EnemyHighWater && i < EnemyActors.Num(); ++i)
		{
			if (EnemyActors[i]) EnemyActors[i]->SetActorHiddenInGame(true);
		}
	}

	void FUvHSWorldRenderer::DrawLocalPlayer(const FVector2D& WorldPos, bool bAlive, bool bDisabled)
	{
		if (!bInitialized) return;

		AStaticMeshActor* A = PlayerActors[0];
		if (!A) return;
		A->SetActorHiddenInGame(false);
		A->SetActorLocation(ToUnreal(WorldPos));

		const float Alpha = (!bAlive || bDisabled) ? 0.4f : 1.0f;
		FLinearColor C = LocalPlayerColor;
		C.A = Alpha;
		if (PlayerMIDs[0]) PlayerMIDs[0]->SetVectorParameterValue(TEXT("BaseColor"), C);
	}

	void FUvHSWorldRenderer::DrawRemotePlayer(uint8 SlotId, const FVector2D& WorldPos, bool bAlive, bool bDisabled)
	{
		if (!bInitialized) return;
		
		const int32 ActorIdx = ((int32)SlotId % (MaxPlayers - 1)) + 1;
		if (!PlayerActors.IsValidIndex(ActorIdx) || !PlayerActors[ActorIdx]) return;

		PlayerActors[ActorIdx]->SetActorHiddenInGame(false);
		PlayerActors[ActorIdx]->SetActorLocation(ToUnreal(WorldPos));

		const float Alpha = (!bAlive || bDisabled) ? 0.4f : 1.0f;
		FLinearColor C = RemotePlayerColor;
		C.A = Alpha;
		if (PlayerMIDs[ActorIdx]) PlayerMIDs[ActorIdx]->SetVectorParameterValue(TEXT("BaseColor"), C);
	}

	void FUvHSWorldRenderer::DrawEnemy(int32 PoolIdx, const FVector2D& WorldPos)
	{
		if (!bInitialized) return;
		if (PoolIdx < 0 || PoolIdx >= EnemyPoolHardCap) return;
		
		while (EnemyActors.Num() <= PoolIdx)
		{
			AStaticMeshActor* A = SpawnSphereActor(EnemyRadius * 2.0f);
			UMaterialInstanceDynamic* MID = nullptr;
			if (A)
			{
				A->SetActorHiddenInGame(true);
				ApplyMaterialColor(A, EnemyColor);
				if (UStaticMeshComponent* M = A->GetStaticMeshComponent())
				{
					if (UMaterialInterface* Mat = M->GetMaterial(0))
					{
						MID = Cast<UMaterialInstanceDynamic>(Mat);
					}
				}
			}
			EnemyActors.Add(A);
			EnemyMIDs.Add(MID);
		}

		AStaticMeshActor* A = EnemyActors[PoolIdx];
		if (!A) return;
		A->SetActorHiddenInGame(false);
		A->SetActorLocation(ToUnreal(WorldPos));
	}

	void FUvHSWorldRenderer::EndFrame(int32 EnemyCountThisFrame)
	{
		EnemyHighWater = FMath::Max(EnemyHighWater, EnemyCountThisFrame);
	}

	void FUvHSWorldRenderer::DrawBeam(const FVector2D& OriginM, const FVector2D& DirUnit, float DistanceM, bool bLocal)
	{
		if (!World) return;

		const FVector Start = ToUnreal(OriginM);
		const FVector2D EndM = OriginM + DirUnit * DistanceM;
		const FVector End   = ToUnreal(EndM);
		const FColor C = (bLocal ? BeamLocalColor : BeamRemoteColor).ToFColor( true);
		
		DrawDebugLine(World, Start, End, C, false, 0.08f,
			 0, 2.5f);
	}

	// ---------- Helpers ----------

	AStaticMeshActor* FUvHSWorldRenderer::SpawnSphereActor(float DiameterMeters)
	{
		if (!World) return nullptr;
		
		const float Scale = DiameterMeters / 1.0f;

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* A = World->SpawnActor<AStaticMeshActor>(
			FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!A) return nullptr;

		A->SetMobility(EComponentMobility::Movable);
		A->SetActorScale3D(FVector(Scale, Scale, Scale));

		UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		if (UStaticMeshComponent* M = A->GetStaticMeshComponent())
		{
			M->SetMobility(EComponentMobility::Movable);
			if (SphereMesh) M->SetStaticMesh(SphereMesh);
			M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			M->SetCastShadow(false);
		}
		return A;
	}

	AStaticMeshActor* FUvHSWorldRenderer::SpawnFloor()
	{
		if (!World) return nullptr;
		
		const float ArenaCm = ArenaRadius * 2.0f * UnrealsPerMeter;
		const float Scale   = (ArenaCm * 1.2f) / 100.0f;

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* A = World->SpawnActor<AStaticMeshActor>(
			FVector(0, 0, 0), FRotator::ZeroRotator, Params);
		if (!A) return nullptr;

		A->SetMobility(EComponentMobility::Movable);
		A->SetActorScale3D(FVector(Scale, Scale, 1.0f));

		UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
		if (UStaticMeshComponent* M = A->GetStaticMeshComponent())
		{
			M->SetMobility(EComponentMobility::Movable);
			if (PlaneMesh) M->SetStaticMesh(PlaneMesh);
			M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			M->SetCastShadow(false);
		}
		ApplyMaterialColor(A, FloorColor);
		return A;
	}

	void FUvHSWorldRenderer::ApplyMaterialColor(AStaticMeshActor* Actor, const FLinearColor& Color)
	{
		if (!Actor) return;
		UStaticMeshComponent* M = Actor->GetStaticMeshComponent();
		if (!M) return;
		
		UMaterial* ParentMat = LoadObject<UMaterial>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		if (!ParentMat) return;

		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(ParentMat, Actor);
		if (!MID) return;
		MID->SetVectorParameterValue(TEXT("Color"), Color);    // some engine versions use "Color"
		MID->SetVectorParameterValue(TEXT("BaseColor"), Color);// others use "BaseColor"
		M->SetMaterial(0, MID);
	}
}
