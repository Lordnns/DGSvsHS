
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Net/QuicClient.h"
#include "ClientSimulation.h"
#include "UvHSAutoPilot.h"
#include "UvHSWorldRenderer.h"
#include "UvHSClientGameMode.generated.h"

class AUvHSClientPlayerController;
class FArchive;

UCLASS()
class UNREALVSHS_API AUvHSClientGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AUvHSClientGameMode();
	
	UPROPERTY(EditAnywhere, Category="Connection")
	FString Host = TEXT("192.168.144.107");

	UPROPERTY(EditAnywhere, Category="Connection")
	int32 Port = 7777;

	UPROPERTY(EditAnywhere, Category="Connection")
	bool bAutoConnect = true;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	void ParseCommandLineOverrides();
	void SimTick();
	void Render();

	// ---- Trial / bot harness (TrialRunner.cs equivalent) ----
	void TrialStart();
	void TrialUpdate(float DeltaSeconds);
	void TrialEnd();
	void TrialLogLine(float Elapsed);
	bool IsBotMode() const { return BotId >= 0; }

	UnrealvsHS::Net::FQuicClient        Quic;
	UnrealvsHS::Client::FClientSimulation Sim;
	UnrealvsHS::Client::FUvHSWorldRenderer Renderer;
	
	UnrealvsHS::Wire::FInputCmd PendingBatch[UnrealvsHS::Wire::MaxInputBatch];
	int32                       PendingBatchCount = 0;

	float  TickAccumulator   = 0.0f;
	uint32 ClientTick        = 0;
	double StartupSeconds    = 0.0;

	bool   bConnected        = false;

	// ---- Bot / trial state ----
	int32   BotId            = -1;
	uint64  TrialSeed        = 1;
	float   DurationSec      = 300.f;
	float   WarmupSec        = 30.f;
	FString OutputPath;

	TUniquePtr<UnrealvsHS::Client::FUvHSAutoPilot> AutoPilot;

	double  TrialStartWall    = 0.0;
	double  NextLogWall       = 0.0;
	int32   SnapsSinceLastLog = 0;
	int32   FiresSinceLastLog = 0;
	uint64  LastDatagramsRx   = 0;
	uint64  LastSnapshotsDec  = 0;
	uint32  LastServerTickSeen = 0;
	bool    bTrialStarted     = false;
	bool    bTrialEnded       = false;
	TUniquePtr<FArchive> TrialLogArchive;
};
