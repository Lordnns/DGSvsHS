#include "StatsWriter.h"
#include "Gameplay/UvHSConstants.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

namespace UnrealvsHS::Server
{
	static const TCHAR* StateName(EServerLifecycle S)
	{
		switch (S)
		{
			case EServerLifecycle::Booting:      return TEXT("Booting");
			case EServerLifecycle::Idle:         return TEXT("Idle");
			case EServerLifecycle::Running:      return TEXT("Running");
			case EServerLifecycle::Resetting:    return TEXT("Resetting");
			case EServerLifecycle::ShuttingDown: return TEXT("ShuttingDown");
			default:                             return TEXT("?");
		}
	}

	void FStatsWriter::Update(const FSimContext& Ctx, int32 InnerTicksThisFrame, float OuterDeltaSec)
	{
		OuterFrames++;
		InnerTicks += InnerTicksThisFrame;
		WindowAcc  += OuterDeltaSec;

		if (WindowAcc < 0.05f) return;

		if (!bPrimed)
		{
			bPrimed     = true;
			WindowAcc   = 0.0f;
			InnerTicks  = 0;
			OuterFrames = 0;
			return;
		}

		const float Elapsed = WindowAcc;
		const float InnerFps = InnerTicks  / Elapsed;
		const float OuterFps = OuterFrames / Elapsed;
		const int32 ToSpawn = Ctx.Round.SpawnTarget;
		const int32 Spawned = Ctx.Round.SpawnTarget - Ctx.Round.SpawnsRemaining;
		const int32 Alive   = Ctx.CachedEnemyCount;
		const double UnixSec = FDateTime::UtcNow().ToUnixTimestampDecimal();

		const FString Json = FString::Printf(
			TEXT("{\"t\":%.3f,\"inner_fps\":%.2f,\"outer_fps\":%.2f,\"to_spawn\":%d,\"spawned\":%d,\"alive\":%d,\"tick\":%u,\"state\":\"%s\"}\n"),
			UnixSec, InnerFps, OuterFps, ToSpawn, Spawned, Alive, Ctx.Tick, StateName(Ctx.State));
		
		FFileHelper::SaveStringToFile(Json, TEXT("/tmp/stats.log"),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			FILEWRITE_None);

		WindowAcc   = 0.0f;
		InnerTicks  = 0;
		OuterFrames = 0;
	}
}
