

#pragma once

#include "CoreMinimal.h"
#include "WireTypes.h"
#include "WireCodec.h"

THIRD_PARTY_INCLUDES_START
#include "msquic.h"
THIRD_PARTY_INCLUDES_END

namespace UnrealvsHS::Net
{
	class FQuicClient
	{
	public:
		TFunction<void(uint8 AssignedPlayerId, uint32 ServerTick)> OnConnected;
		TFunction<void()>                                          OnDisconnected;
		TFunction<void(const Wire::FSnapshot& Snap)>               OnSnapshot;

		FQuicClient();
		~FQuicClient();

		bool Start(const FString& ServerHost, uint16 ServerPort);
		void Stop();

		bool IsRunning() const { return bRunning; }
		bool IsConnected() const { return bHandshakeDone; }
		uint8 GetAssignedPlayerId() const { return AssignedPlayerId; }
		
		void PollEvents();

		void SendInputBatch(const Wire::FInputCmd* Cmds, int32 Count);

		uint64 GetDatagramsReceived() const { return DatagramsRx.GetValue(); }
		uint64 GetSnapshotsDecoded()  const { return SnapshotsDecoded.GetValue(); }

	private:

		bool LoadMsQuic();
		bool OpenRegistration();
		bool OpenConfiguration();
		
		bool OpenAndStartConnection(const FString& InHost, uint16 InPort);
		bool OpenControlStream();
		void SendClientHello();
		
		static QUIC_STATUS QUIC_API NetClientConnectionCallback(
			HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);

		static QUIC_STATUS QUIC_API NetClientStreamCallback(
			HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
		
		void DecodeSnapshotDatagram(const uint8* Bytes, int32 Len);

		const QUIC_API_TABLE* MsQuic = nullptr;
		HQUIC Registration   = nullptr;
		HQUIC Configuration  = nullptr;
		HQUIC Connection     = nullptr;
		HQUIC ControlStream  = nullptr;

		FString  Host;
		uint16   Port             = 0;
		bool     bRunning         = false;
		bool     bHandshakeDone   = false;
		uint8    AssignedPlayerId = 0xFF;
		uint32   ServerTickAtWelcome = 0;
		
		TArray<uint8> StreamRxBuf;
		
		Wire::FSnapshot Baseline;
		bool            bHaveBaseline = false;
		
		enum class EQueuedKind : uint8 { Connected, Disconnected, Snapshot };
		struct FQueuedEvent
		{
			EQueuedKind     Kind             = EQueuedKind::Connected;
			uint8           AssignedPlayerId = 0;
			uint32          ServerTick       = 0;
			Wire::FSnapshot Snap;
		};
		TQueue<FQueuedEvent, EQueueMode::Mpsc> EventQueue;
		
		FThreadSafeCounter64 DatagramsRx;
		FThreadSafeCounter64 SnapshotsDecoded;
	};
}
