#include "UvHSClientGameMode.h"
#include "UvHSClientPawn.h"
#include "UvHSClientPlayerController.h"
#include "Gameplay/UvHSConstants.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY_STATIC(LogUvHSClientGM, Log, All);

AUvHSClientGameMode::AUvHSClientGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup    = TG_PrePhysics;

	DefaultPawnClass       = AUvHSClientPawn::StaticClass();
	PlayerControllerClass  = AUvHSClientPlayerController::StaticClass();
}

void AUvHSClientGameMode::BeginPlay()
{
	Super::BeginPlay();
	StartupSeconds = FPlatformTime::Seconds();

	ParseCommandLineOverrides();

	Renderer.Initialize(GetWorld());
	
	Quic.OnConnected = [this](uint8 PlayerId, uint32 ServerTick)
	{
		UE_LOG(LogUvHSClientGM, Display,
			TEXT("[ClientGM] connected: playerId=%u serverTick=%u"),
			(uint32)PlayerId, ServerTick);
		Sim.SetLocalPlayerId(PlayerId);
		bConnected = true;
	};
	Quic.OnDisconnected = [this]()
	{
		UE_LOG(LogUvHSClientGM, Warning, TEXT("[ClientGM] disconnected"));
		bConnected = false;
	};
	Quic.OnSnapshot = [this](const UnrealvsHS::Wire::FSnapshot& Snap)
	{
		const double WallNow = FPlatformTime::Seconds() - StartupSeconds;
		Sim.OnSnapshotReceived(Snap, WallNow);
		LastServerTickSeen = Snap.Tick;
		++SnapsSinceLastLog;
	};

	if (bAutoConnect)
	{
		UE_LOG(LogUvHSClientGM, Display,
			TEXT("[ClientGM] BeginPlay — auto-connecting to %s:%d"), *Host, Port);
		Quic.Start(Host, (uint16)Port);
	}
	else
	{
		UE_LOG(LogUvHSClientGM, Display, TEXT("[ClientGM] BeginPlay — AutoConnect OFF"));
	}
	
	if (IsBotMode()) TrialStart();
}

void AUvHSClientGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
	if (IsBotMode() && TrialLogArchive.IsValid())
	{
		TrialLogArchive->Flush();
		TrialLogArchive->Close();
		TrialLogArchive.Reset();
		bTrialEnded = true;
	}
	Quic.Stop();
	Renderer.Shutdown();
	Super::EndPlay(Reason);
}

// ---------- Trial harness (TrialRunner.cs equivalent) ----------

void AUvHSClientGameMode::TrialStart()
{
	if (!IsBotMode() || bTrialStarted) return;
	bTrialStarted    = true;
	TrialStartWall   = FPlatformTime::Seconds();
	NextLogWall      = TrialStartWall + 1.0;

	const FString Path = OutputPath.IsEmpty()
		? FPaths::ProjectSavedDir() / FString::Printf(TEXT("trial_%d.ndjson"), BotId)
		: OutputPath;

	IFileManager& FM = IFileManager::Get();
	TrialLogArchive.Reset(FM.CreateFileWriter(*Path, FILEWRITE_Append | FILEWRITE_AllowRead));
	if (!TrialLogArchive.IsValid())
	{
		UE_LOG(LogUvHSClientGM, Error, TEXT("[Trial] cannot open log file: %s"), *Path);
		return;
	}

	const int64 StartedAt = FDateTime::UtcNow().ToUnixTimestamp();
	const FString Header = FString::Printf(
		TEXT("{\"type\":\"header\",\"protocol\":\"dgsvshs-trial-v1\",\"leg\":\"unreal\",\"started_at_unix\":%lld,\"bot_id\":%d,\"seed\":%llu,\"duration\":%.3f,\"warmup\":%.3f}\n"),
		StartedAt, BotId, TrialSeed, DurationSec, WarmupSec);
	FTCHARToUTF8 Conv(*Header);
	TrialLogArchive->Serialize((void*)Conv.Get(), Conv.Length());

	LastDatagramsRx  = Quic.GetDatagramsReceived();
	LastSnapshotsDec = Quic.GetSnapshotsDecoded();

	UE_LOG(LogUvHSClientGM, Display,
		TEXT("[Trial] started: bot=%d seed=0x%llX duration=%.1fs warmup=%.1fs → %s"),
		BotId, TrialSeed, DurationSec, WarmupSec, *Path);
}

void AUvHSClientGameMode::TrialUpdate(float /*DeltaSeconds*/)
{
	if (!bTrialStarted || bTrialEnded) return;

	const double Now     = FPlatformTime::Seconds();
	const float  Elapsed = (float)(Now - TrialStartWall);

	if (Now >= NextLogWall)
	{
		TrialLogLine(Elapsed);
		NextLogWall += 1.0;
		SnapsSinceLastLog = 0;
		FiresSinceLastLog = 0;
	}

	if (Elapsed >= DurationSec) TrialEnd();
}

void AUvHSClientGameMode::TrialLogLine(float Elapsed)
{
	if (!TrialLogArchive.IsValid()) return;

	const TCHAR* Phase = Elapsed < WarmupSec ? TEXT("warmup") : TEXT("measure");
	const TCHAR* State = bConnected ? TEXT("Connected") : TEXT("Disconnected");

	const FString Line = FString::Printf(
		TEXT("{\"t\":%.3f,\"phase\":\"%s\",\"bot_id\":%d,\"seed\":%llu,\"client_tick\":%u,\"server_tick\":%u,\"snaps_1s\":%d,\"fires_1s\":%d,\"state\":\"%s\"}\n"),
		Elapsed, Phase, BotId, TrialSeed, ClientTick, LastServerTickSeen,
		SnapsSinceLastLog, FiresSinceLastLog, State);

	FTCHARToUTF8 Conv(*Line);
	TrialLogArchive->Serialize((void*)Conv.Get(), Conv.Length());
	TrialLogArchive->Flush();
}

void AUvHSClientGameMode::TrialEnd()
{
	if (bTrialEnded) return;
	bTrialEnded = true;

	if (TrialLogArchive.IsValid())
	{
		TrialLogArchive->Flush();
		TrialLogArchive->Close();
		TrialLogArchive.Reset();
	}
	UE_LOG(LogUvHSClientGM, Display, TEXT("[Trial] complete — quitting."));
	
	Quic.Stop();
	
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0))
		{
			UKismetSystemLibrary::QuitGame(World, PC, EQuitPreference::Quit,  true);
			return;
		}
	}

	FPlatformMisc::RequestExit(true);
}

void AUvHSClientGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	
	Quic.PollEvents();
	
	TickAccumulator += DeltaSeconds;
	while (TickAccumulator >= UnrealvsHS::Constants::SimDt)
	{
		TickAccumulator -= UnrealvsHS::Constants::SimDt;
		SimTick();
	}

	Render();

	if (IsBotMode()) TrialUpdate(DeltaSeconds);
}

void AUvHSClientGameMode::SimTick()
{
	++ClientTick;

	if (!bConnected) return;
	
	const FVector2D PredictedPos = Sim.HasLocalPlayer()
		? Sim.GetPredictedLocalPlayer().Position
		: FVector2D::ZeroVector;

	UnrealvsHS::Wire::FInputCmd Cmd;
	if (AutoPilot.IsValid())
	{
		Cmd = AutoPilot->Sample(ClientTick, PredictedPos);
		
		if (((uint8)Cmd.Flags & (uint8)UnrealvsHS::Wire::EInputFlags::Fire) != 0)
		{
			++FiresSinceLastLog;
		}
	}
	else
	{
		AUvHSClientPlayerController* PC = nullptr;
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			PC = Cast<AUvHSClientPlayerController>(It->Get());
			if (PC) break;
		}
		if (!PC) return;
		Cmd = PC->SampleInput(ClientTick, PredictedPos);
	}
	
	Cmd.LastAckedServerTick = Sim.LastAckedServerTick();

	Sim.PushPredictedInput(Cmd);
	
	PendingBatch[0]   = Cmd;
	PendingBatchCount = 1;
	Quic.SendInputBatch(PendingBatch, PendingBatchCount);
	PendingBatchCount = 0;
}

void AUvHSClientGameMode::Render()
{
	if (!Renderer.IsInitialized()) return;

	const double WallNow = FPlatformTime::Seconds() - StartupSeconds;

	Renderer.BeginFrame();

	// ---- Local player (predicted) ----
	if (Sim.HasLocalPlayer())
	{
		const auto& LP = Sim.GetPredictedLocalPlayer();
		Renderer.DrawLocalPlayer(LP.Position, LP.bAlive, LP.DisableTimer > 0.0f);
	}

	// ---- Remote entities from latest snapshot (interpolated) ----
	int32 EnemyCount = 0;
	if (Sim.HasLatestSnapshot())
	{
		const UnrealvsHS::Wire::FSnapshot& Latest = Sim.GetLatestSnapshot();
		const uint8 LocalId = Sim.GetLocalPlayerId();

		for (const UnrealvsHS::Wire::FPlayerSnap& P : Latest.Players)
		{
			if (P.Id == LocalId) continue;
			FVector2D Pos, Aim;
			bool bAlive = false, bDisabled = false;
			if (Sim.TryGetInterpolatedPlayer(P.Id, WallNow, Pos, Aim, bAlive, bDisabled))
			{
				Renderer.DrawRemotePlayer(P.Id, Pos, bAlive, bDisabled);
			}
			else
			{
				Renderer.DrawRemotePlayer(P.Id, P.Position, P.bAlive, P.DisableTimer > 0.0f);
			}
		}

		for (const UnrealvsHS::Wire::FEnemySnap& E : Latest.Enemies)
		{
			FVector2D Pos;
			if (Sim.TryGetInterpolatedEnemy(E.Id, WallNow, Pos))
			{
				Renderer.DrawEnemy(EnemyCount++, Pos);
			}
			else
			{
				Renderer.DrawEnemy(EnemyCount++, E.Position);
			}
		}
	}

	Renderer.EndFrame(EnemyCount);

	// ---- Beams: predicted (instant) + server-confirmed (from other shooters) ----
	for (const UnrealvsHS::Wire::FFireEvent& F : Sim.NewPredictedFires)
	{
		Renderer.DrawBeam(F.Origin, F.Direction, F.Distance, /*bLocal*/ true);
	}
	Sim.NewPredictedFires.Reset();

	for (const UnrealvsHS::Wire::FFireEvent& F : Sim.NewFireEvents)
	{
		Renderer.DrawBeam(F.Origin, F.Direction, F.Distance, /*bLocal*/ false);
	}
	Sim.NewFireEvents.Reset();
}

void AUvHSClientGameMode::ParseCommandLineOverrides()
{
	FString CmdHost;
	if (FParse::Value(FCommandLine::Get(), TEXT("server="), CmdHost) && !CmdHost.IsEmpty())
	{
		Host = CmdHost;
	}
	int32 CmdPort = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("port="), CmdPort) && CmdPort > 0 && CmdPort < 65536)
	{
		Port = CmdPort;
	}
	
	const FString FullCmd = FCommandLine::Get();
	auto ParseInt = [&](const TCHAR* Long, const TCHAR* UeKey, int32& Out) -> bool
	{
		if (FParse::Value(FCommandLine::Get(), UeKey, Out)) return true;
		const int32 Idx = FullCmd.Find(Long, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		if (Idx < 0) return false;
		FString Rest = FullCmd.Mid(Idx + FCString::Strlen(Long)).TrimStart();
		FString Token; FString Tail;
		if (!Rest.Split(TEXT(" "), &Token, &Tail))
		{
			Token = Rest;
		}
		if (Token.IsEmpty()) return false;
		Out = FCString::Atoi(*Token);
		return true;
	};
	auto ParseFloat = [&](const TCHAR* Long, const TCHAR* UeKey, float& Out) -> bool
	{
		if (FParse::Value(FCommandLine::Get(), UeKey, Out)) return true;
		const int32 Idx = FullCmd.Find(Long, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		if (Idx < 0) return false;
		FString Rest = FullCmd.Mid(Idx + FCString::Strlen(Long)).TrimStart();
		FString Token; FString Tail;
		if (!Rest.Split(TEXT(" "), &Token, &Tail)) { Token = Rest; }
		if (Token.IsEmpty()) return false;
		Out = FCString::Atof(*Token);
		return true;
	};
	auto ParseString = [&](const TCHAR* Long, const TCHAR* UeKey, FString& Out) -> bool
	{
		if (FParse::Value(FCommandLine::Get(), UeKey, Out)) return true;
		const int32 Idx = FullCmd.Find(Long, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		if (Idx < 0) return false;
		FString Rest = FullCmd.Mid(Idx + FCString::Strlen(Long)).TrimStart();
		FString Token; FString Tail;
		if (!Rest.Split(TEXT(" "), &Token, &Tail)) { Token = Rest; }
		if (Token.IsEmpty()) return false;
		Out = Token;
		return true;
	};
	
	FString CmdHostLong;
	if (ParseString(TEXT("--server"), TEXT("__nope__="), CmdHostLong) && !CmdHostLong.IsEmpty())
	{
		Host = CmdHostLong;
	}
	int32 CmdPortLong = 0;
	if (ParseInt(TEXT("--port"), TEXT("__nope__="), CmdPortLong) && CmdPortLong > 0 && CmdPortLong < 65536)
	{
		Port = CmdPortLong;
	}

	ParseInt   (TEXT("--bot-id"),   TEXT("bot-id="),   BotId);
	int32 SeedI = 0;
	if (ParseInt(TEXT("--seed"),    TEXT("seed="),     SeedI)) TrialSeed = (uint64)(uint32)SeedI;
	ParseFloat (TEXT("--duration"), TEXT("duration="), DurationSec);
	ParseFloat (TEXT("--warmup"),   TEXT("warmup="),   WarmupSec);
	ParseString(TEXT("--output"),   TEXT("output="),   OutputPath);

	if (IsBotMode())
	{
		AutoPilot = MakeUnique<UnrealvsHS::Client::FUvHSAutoPilot>((uint8)BotId, TrialSeed);

		UE_LOG(LogUvHSClientGM, Display,
			TEXT("[ClientGM] bot mode: botId=%d seed=0x%llX duration=%.1fs warmup=%.1fs output=%s"),
			BotId, TrialSeed, DurationSec, WarmupSec,
			OutputPath.IsEmpty() ? TEXT("(default)") : *OutputPath);
	}

	UE_LOG(LogUvHSClientGM, Display,
		TEXT("[ClientGM] target = %s:%d (autoConnect=%d)"), *Host, Port, bAutoConnect ? 1 : 0);
}
