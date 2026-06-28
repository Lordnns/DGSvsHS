
#pragma once

#include "CoreMinimal.h"
#include "WireTypes.h"
#include "WireCodec.h"
#include "SnapshotPriority.h"
#include "Server/RecipientSnapshotState.h"
#include "Server/WorldStateHistory.h"

THIRD_PARTY_INCLUDES_START
#include "msquic.h"
THIRD_PARTY_INCLUDES_END

namespace UnrealvsHS::Net
{
	struct FQuicSlot;

	class FQuicServer
	{
	public:
		TFunction<void(uint8 PlayerId)>                       OnClientConnected;
		TFunction<void(uint8 PlayerId)>                       OnClientDisconnected;
		TFunction<void(uint8 PlayerId, const Wire::FInputCmd&)> OnInputReceived;

		FQuicServer();
		~FQuicServer();
		
		bool Start(uint16 ListenPort);

		void Stop();

		bool IsRunning() const { return bRunning; }

		void SetServerTick(uint32 Tick) { CurrentServerTick = Tick; }

		void BroadcastSnapshot(const Wire::FSnapshot& Snap, const UnrealvsHS::Server::FWorldStateHistory& History);

		void PollEvents();

		uint64 GetDatagramsReceived()   const { return DatagramsRx.GetValue();   }
		uint64 GetInputBatchesParsed()  const { return InputBatchRx.GetValue();  }
		uint64 GetInputCmdsQueued()     const { return InputCmdRx.GetValue();    }

	private:

		bool LoadMsQuic();
		bool OpenRegistration();
		bool OpenConfiguration();
		bool LoadServerCertificate();
		bool OpenListener(uint16 Port);

		int32 FindFreeSlot() const;
		void  FreeSlot(int32 Index);
		
		static QUIC_STATUS QUIC_API NetServerListenerCallback(
			HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event);

		static QUIC_STATUS QUIC_API NetServerConnectionCallback(
			HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
		
		static QUIC_STATUS QUIC_API NetServerStreamCallback(
			HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);

		uint16   Port             = 0;
		bool     bRunning         = false;
		uint32   CurrentServerTick = 0;
		
		HQUIC    Registration     = nullptr;
		HQUIC    Configuration    = nullptr;
		HQUIC    Listener         = nullptr;
		const QUIC_API_TABLE* MsQuic = nullptr;

		TArray<uint8> CertPkcs12;

		TArray<TUniquePtr<FQuicSlot>> Slots;

		TArray<TUniquePtr<UnrealvsHS::Server::FRecipientSnapshotState>> RecipientStates;

		TArray<TUniquePtr<FThreadSafeCounter>> HighestInputTick;

		TArray<Wire::FEnemySnap>                                    SelectedEnemiesScratch;
		TArray<Wire::FEnemyDeltaEntry>                              ChangedScratch;
		TArray<uint16>                                              RemovedScratch;
		TArray<Wire::FEnemySnap>                                    AddedScratch;
		TSet<uint16>                                                IncludedScratch;
		TArray<UnrealvsHS::Server::FSnapshotPriority::FScoredEnemy> ScoredScratch;
		TSet<uint16>                                                CurrentIdsScratch;
		TMap<uint16, int32>                                         BaselineIndexScratch;
		
		FThreadSafeCounter64 DatagramsRx;
		FThreadSafeCounter64 InputBatchRx;
		FThreadSafeCounter64 InputCmdRx;

		enum class EQueuedKind : uint8 { Connected, Disconnected, Input };
		struct FQueuedEvent
		{
			EQueuedKind     Kind     = EQueuedKind::Connected;
			uint8           PlayerId = 0;
			Wire::FInputCmd Input;
		};
		TQueue<FQueuedEvent, EQueueMode::Mpsc> EventQueue;
	};
}
