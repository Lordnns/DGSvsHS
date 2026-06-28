
#pragma once

#include "CoreMinimal.h"
#include "Net/WireTypes.h"

class UWorld;
class AStaticMeshActor;
class UMaterialInstanceDynamic;

namespace UnrealvsHS::Client
{
	class FUvHSWorldRenderer
	{
	public:
		static constexpr float UnrealsPerMeter = 100.0f;
		
		static constexpr float EntityZ = 50.0f;
		
		void Initialize(UWorld* InWorld);
		
		void Shutdown();

		bool IsInitialized() const { return bInitialized; }

		// ---------- Per-frame rendering ----------
		void BeginFrame();
		void DrawLocalPlayer(const FVector2D& WorldPos, bool bAlive, bool bDisabled);
		void DrawRemotePlayer(uint8 SlotId, const FVector2D& WorldPos, bool bAlive, bool bDisabled);
		void DrawEnemy(int32 PoolIdx, const FVector2D& WorldPos);
		void EndFrame(int32 EnemyCountThisFrame);
		
		void DrawBeam(const FVector2D& OriginM, const FVector2D& DirUnit, float DistanceM, bool bLocal);

	private:
		AStaticMeshActor* SpawnSphereActor(float DiameterMeters);
		AStaticMeshActor* SpawnFloor();

		void ApplyMaterialColor(AStaticMeshActor* Actor, const FLinearColor& Color);
		
		static FVector ToUnreal(const FVector2D& Meters)
		{
			return FVector(
				(double)Meters.X * (double)UnrealsPerMeter,
				(double)Meters.Y * (double)UnrealsPerMeter,
				(double)EntityZ);
		}

		UWorld* World = nullptr;
		bool    bInitialized = false;

		AStaticMeshActor* FloorActor = nullptr;
		AActor*           SkyLightActor = nullptr;
		AActor*           DirLightActor = nullptr;
		
		TArray<AStaticMeshActor*>          PlayerActors;
		TArray<UMaterialInstanceDynamic*>  PlayerMIDs;
		
		TArray<AStaticMeshActor*>          EnemyActors;
		TArray<UMaterialInstanceDynamic*>  EnemyMIDs;

		int32 EnemyHighWater = 0;
	};
}
