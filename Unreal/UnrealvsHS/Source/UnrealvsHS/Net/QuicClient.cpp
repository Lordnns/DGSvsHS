#include "QuicClient.h"
#include "Gameplay/UvHSConstants.h"

DEFINE_LOG_CATEGORY_STATIC(LogQuicClient, Log, All);

namespace UnrealvsHS::Net
{
#if PLATFORM_LINUX
#include <dlfcn.h>
	typedef QUIC_STATUS (QUIC_API *FMsQuicClientOpenVersion)(uint32_t, const void**);
	typedef void (QUIC_API *FMsQuicClientClose)(const void*);
	static FMsQuicClientOpenVersion GMsQuicClientOpenVersion = nullptr;
	static FMsQuicClientClose GMsQuicClientClose = nullptr;
	static void* GMsQuicClientLibHandle = nullptr;
#endif
	
	struct FQuicClientPendingSend
	{
		QUIC_BUFFER QuicBuf;
		uint8*      Data   = nullptr;
		uint32      Length = 0;
	};

	static FQuicClientPendingSend* MakeClientPendingSendCopy(const uint8* SrcBytes, int32 SrcLen)
	{
		auto* S = new FQuicClientPendingSend();
		S->Data   = new uint8[SrcLen];
		S->Length = (uint32)SrcLen;
		FMemory::Memcpy(S->Data, SrcBytes, SrcLen);
		S->QuicBuf.Buffer = S->Data;
		S->QuicBuf.Length = (uint32)SrcLen;
		return S;
	}

	static void FreeClientPendingSend(FQuicClientPendingSend* S)
	{
		if (!S) return;
		delete[] S->Data;
		delete S;
	}

	// ---------- Lifecycle ----------

	FQuicClient::FQuicClient()  = default;
	FQuicClient::~FQuicClient() { Stop(); }

	bool FQuicClient::Start(const FString& ServerHost, uint16 ServerPort)
	{
		Host = ServerHost;
		Port = ServerPort;

		if (!LoadMsQuic())                        { UE_LOG(LogQuicClient, Error, TEXT("LoadMsQuic failed"));         return false; }
		if (!OpenRegistration())                  { UE_LOG(LogQuicClient, Error, TEXT("OpenRegistration failed"));  Stop(); return false; }
		if (!OpenConfiguration())                 { UE_LOG(LogQuicClient, Error, TEXT("OpenConfiguration failed")); Stop(); return false; }
		if (!OpenAndStartConnection(Host, Port))  { UE_LOG(LogQuicClient, Error, TEXT("ConnectionStart failed"));   Stop(); return false; }

		bRunning = true;
		UE_LOG(LogQuicClient, Display,
			TEXT("[QuicClient] connecting to %s:%u (ALPN=dgsvshs/2)"),
			*Host, (uint32)Port);
		return true;
	}

	void FQuicClient::Stop()
	{
		if (MsQuic == nullptr) return;

		if (Connection)
		{
			MsQuic->ConnectionShutdown(Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
			MsQuic->ConnectionClose(Connection);
			Connection = nullptr;
		}
		
		ControlStream = nullptr;

		if (Configuration) { MsQuic->ConfigurationClose(Configuration); Configuration = nullptr; }
		if (Registration)  { MsQuic->RegistrationClose(Registration);   Registration = nullptr; }

#if PLATFORM_LINUX
		if (GMsQuicClientClose) GMsQuicClientClose(MsQuic);
#else
		MsQuicClose(MsQuic);
#endif
		MsQuic = nullptr;

		bRunning         = false;
		bHandshakeDone   = false;
		AssignedPlayerId = 0xFF;
		StreamRxBuf.Reset();
		bHaveBaseline    = false;
		Baseline.Reset();

		UE_LOG(LogQuicClient, Display, TEXT("[QuicClient] stopped"));
	}

	// ---------- msquic init ----------

	bool FQuicClient::LoadMsQuic()
	{
#if PLATFORM_LINUX
		if (!GMsQuicClientLibHandle)
		{
			GMsQuicClientLibHandle = dlopen("libmsquic.so.2", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
			if (!GMsQuicClientLibHandle)
			{
				UE_LOG(LogQuicClient, Error, TEXT("dlopen libmsquic.so.2 failed: %s"), UTF8_TO_TCHAR(dlerror()));
				return false;
			}
			GMsQuicClientOpenVersion = (FMsQuicClientOpenVersion)dlsym(GMsQuicClientLibHandle, "MsQuicOpenVersion");
			GMsQuicClientClose = (FMsQuicClientClose)dlsym(GMsQuicClientLibHandle, "MsQuicClose");
		}
		if (!GMsQuicClientOpenVersion || !GMsQuicClientClose)
		{
			UE_LOG(LogQuicClient, Error, TEXT("Failed to dlsym MsQuicOpenVersion or MsQuicClose"));
			return false;
		}
		const QUIC_STATUS S = GMsQuicClientOpenVersion(2, (const void**)&MsQuic);
#else
		const QUIC_STATUS S = MsQuicOpen2(&MsQuic);
#endif

		if (QUIC_FAILED(S))
		{
			UE_LOG(LogQuicClient, Error, TEXT("MsQuicOpen2 failed: 0x%X"), S);
			return false;
		}
		return true;
	}

	bool FQuicClient::OpenRegistration()
	{
		QUIC_REGISTRATION_CONFIG RegConfig = {};
		RegConfig.AppName          = "UnrealvsHS-Client";
		RegConfig.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;

		const QUIC_STATUS S = MsQuic->RegistrationOpen(&RegConfig, &Registration);
		if (QUIC_FAILED(S))
		{
			UE_LOG(LogQuicClient, Error, TEXT("RegistrationOpen failed: 0x%X"), S);
			return false;
		}
		return true;
	}

	bool FQuicClient::OpenConfiguration()
	{
		static const char* AlpnString = "dgsvshs/2";
		QUIC_BUFFER Alpn = {};
		Alpn.Buffer = (uint8_t*)AlpnString;
		Alpn.Length = (uint32)FCStringAnsi::Strlen(AlpnString);

		QUIC_SETTINGS Settings = {};
		Settings.IdleTimeoutMs                       = 30000;
		Settings.IsSet.IdleTimeoutMs                 = true;
		Settings.DatagramReceiveEnabled              = true;
		Settings.IsSet.DatagramReceiveEnabled        = true;
		Settings.SendBufferingEnabled                = false;
		Settings.IsSet.SendBufferingEnabled          = true;

		const QUIC_STATUS S = MsQuic->ConfigurationOpen(
			Registration, &Alpn, 1, &Settings, sizeof(Settings), nullptr, &Configuration);
		if (QUIC_FAILED(S))
		{
			UE_LOG(LogQuicClient, Error, TEXT("ConfigurationOpen failed: 0x%X"), S);
			return false;
		}
		
		QUIC_CREDENTIAL_CONFIG CredConfig = {};
		CredConfig.Type  = QUIC_CREDENTIAL_TYPE_NONE;
		CredConfig.Flags = (QUIC_CREDENTIAL_FLAGS)(
			QUIC_CREDENTIAL_FLAG_CLIENT
			| QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);

		const QUIC_STATUS S2 = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig);
		if (QUIC_FAILED(S2))
		{
			UE_LOG(LogQuicClient, Error, TEXT("ConfigurationLoadCredential failed: 0x%X"), S2);
			return false;
		}
		return true;
	}

	// ---------- Connection start ----------

	bool FQuicClient::OpenAndStartConnection(const FString& InHost, uint16 InPort)
	{
		const QUIC_STATUS S = MsQuic->ConnectionOpen(
			Registration, &FQuicClient::NetClientConnectionCallback, /*Context*/ this, &Connection);
		if (QUIC_FAILED(S))
		{
			UE_LOG(LogQuicClient, Error, TEXT("ConnectionOpen failed: 0x%X"), S);
			return false;
		}

		FTCHARToUTF8 HostUtf8(*InHost);
		const QUIC_STATUS S2 = MsQuic->ConnectionStart(
			Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, HostUtf8.Get(), InPort);
		if (QUIC_FAILED(S2))
		{
			UE_LOG(LogQuicClient, Error, TEXT("ConnectionStart failed: 0x%X"), S2);
			return false;
		}
		return true;
	}

	bool FQuicClient::OpenControlStream()
	{
		const QUIC_STATUS S = MsQuic->StreamOpen(
			Connection, QUIC_STREAM_OPEN_FLAG_NONE,
			&FQuicClient::NetClientStreamCallback, /*Context*/ this, &ControlStream);
		if (QUIC_FAILED(S))
		{
			UE_LOG(LogQuicClient, Error, TEXT("StreamOpen failed: 0x%X"), S);
			return false;
		}
		const QUIC_STATUS S2 = MsQuic->StreamStart(ControlStream, QUIC_STREAM_START_FLAG_NONE);
		if (QUIC_FAILED(S2))
		{
			UE_LOG(LogQuicClient, Error, TEXT("StreamStart failed: 0x%X"), S2);
			return false;
		}
		return true;
	}

	void FQuicClient::SendClientHello()
	{
		TArray<uint8> Payload;
		Wire::FCodec::WriteClientHello(Payload, 0);
		TArray<uint8> Framed;
		Wire::FCodec::FrameStreamMessage(Framed, Wire::MsgClientHello, Payload);

		FQuicClientPendingSend* PSend = MakeClientPendingSendCopy(Framed.GetData(), Framed.Num());
		const QUIC_STATUS S = MsQuic->StreamSend(
			ControlStream, &PSend->QuicBuf, 1, QUIC_SEND_FLAG_NONE, /*context*/ PSend);
		if (QUIC_FAILED(S))
		{
			FreeClientPendingSend(PSend);
			UE_LOG(LogQuicClient, Warning, TEXT("[QuicClient] ClientHello StreamSend failed 0x%X"), S);
		}
		else
		{
			UE_LOG(LogQuicClient, Display, TEXT("[QuicClient] sent ClientHello (%d B)"), Framed.Num());
		}
	}

	// ---------- Input send ----------

	void FQuicClient::SendInputBatch(const Wire::FInputCmd* Cmds, int32 Count)
	{
		if (!bRunning || !bHandshakeDone || Connection == nullptr) return;
		if (Count <= 0 || Count > Wire::MaxInputBatch) return;
		
		TArray<uint8> Body;
		Body.Reserve(1 + 1 + Wire::InputCmdWireBytes * Count);
		Body.Add(Wire::MsgInput);
		Wire::FCodec::WriteInputBatch(Body, Cmds, Count);

		FQuicClientPendingSend* PSend = MakeClientPendingSendCopy(Body.GetData(), Body.Num());
		const QUIC_STATUS S = MsQuic->DatagramSend(
			Connection, &PSend->QuicBuf, 1, QUIC_SEND_FLAG_NONE, /*context*/ PSend);
		if (QUIC_FAILED(S))
		{
			FreeClientPendingSend(PSend);
			UE_LOG(LogQuicClient, Verbose, TEXT("[QuicClient] input DatagramSend failed 0x%X size=%d"),
				S, Body.Num());
		}
	}

	// ---------- Connection callback ----------

	QUIC_STATUS QUIC_API FQuicClient::NetClientConnectionCallback(
		HQUIC InConn, void* Context, QUIC_CONNECTION_EVENT* Ev)
	{
		auto* Self = static_cast<FQuicClient*>(Context);
		if (!Self) return QUIC_STATUS_SUCCESS;

		switch (Ev->Type)
		{
			case QUIC_CONNECTION_EVENT_CONNECTED:
			{
				UE_LOG(LogQuicClient, Display, TEXT("[QuicClient] TLS handshake complete — opening control stream"));
				if (!Self->OpenControlStream())
				{
					UE_LOG(LogQuicClient, Error, TEXT("[QuicClient] failed to open control stream"));
					return QUIC_STATUS_SUCCESS;
				}
				Self->SendClientHello();
				return QUIC_STATUS_SUCCESS;
			}

			case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
			{
				const QUIC_BUFFER* Buf = Ev->DATAGRAM_RECEIVED.Buffer;
				Self->DatagramsRx.Increment();
				if (!Buf || Buf->Length < 1) return QUIC_STATUS_SUCCESS;

				const uint8 MsgType = Buf->Buffer[0];
				if (MsgType != Wire::MsgSnapshot) return QUIC_STATUS_SUCCESS;

				Self->DecodeSnapshotDatagram(Buf->Buffer + 1, (int32)Buf->Length - 1);
				return QUIC_STATUS_SUCCESS;
			}

			case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
			{
				auto* PSend = static_cast<FQuicClientPendingSend*>(Ev->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
				const QUIC_DATAGRAM_SEND_STATE St = Ev->DATAGRAM_SEND_STATE_CHANGED.State;
					
				const bool bTerminal =
					St == QUIC_DATAGRAM_SEND_LOST_DISCARDED
					|| St == QUIC_DATAGRAM_SEND_ACKNOWLEDGED
					|| St == QUIC_DATAGRAM_SEND_ACKNOWLEDGED_SPURIOUS
					|| St == QUIC_DATAGRAM_SEND_CANCELED;
				if (bTerminal && PSend) FreeClientPendingSend(PSend);
				return QUIC_STATUS_SUCCESS;
			}

			case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
			case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
				UE_LOG(LogQuicClient, Display, TEXT("[QuicClient] shutdown initiated"));
				return QUIC_STATUS_SUCCESS;

			case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
			{
				UE_LOG(LogQuicClient, Display, TEXT("[QuicClient] shutdown complete"));
				FQueuedEvent QEv;
				QEv.Kind = EQueuedKind::Disconnected;
				Self->EventQueue.Enqueue(QEv);
				return QUIC_STATUS_SUCCESS;
			}

			default:
				return QUIC_STATUS_SUCCESS;
		}
	}

	// ---------- Stream callback (ServerWelcome reader) ----------

	QUIC_STATUS QUIC_API FQuicClient::NetClientStreamCallback(
		HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Ev)
	{
		auto* Self = static_cast<FQuicClient*>(Context);
		if (!Self) return QUIC_STATUS_SUCCESS;

		switch (Ev->Type)
		{
			case QUIC_STREAM_EVENT_RECEIVE:
			{
				const uint32 BufCount = Ev->RECEIVE.BufferCount;
				for (uint32 b = 0; b < BufCount; ++b)
				{
					const QUIC_BUFFER& Bb = Ev->RECEIVE.Buffers[b];
					Self->StreamRxBuf.Append(Bb.Buffer, (int32)Bb.Length);
				}

				if (Self->bHandshakeDone) return QUIC_STATUS_SUCCESS;
					
				if (Self->StreamRxBuf.Num() < 5) return QUIC_STATUS_SUCCESS;

				const uint32 FrameLen =
					(uint32)Self->StreamRxBuf[0]
					| ((uint32)Self->StreamRxBuf[1] << 8)
					| ((uint32)Self->StreamRxBuf[2] << 16)
					| ((uint32)Self->StreamRxBuf[3] << 24);

				const int32 TotalNeeded = 4 + 1 + (int32)FrameLen;
				if (Self->StreamRxBuf.Num() < TotalNeeded) return QUIC_STATUS_SUCCESS;

				const uint8 MsgType = Self->StreamRxBuf[4];
				if (MsgType != Wire::MsgServerWelcome)
				{
					UE_LOG(LogQuicClient, Warning,
						TEXT("[QuicClient] unexpected control msg 0x%02X (expected ServerWelcome)"), MsgType);
					Self->MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
					return QUIC_STATUS_SUCCESS;
				}

				Wire::FWireReader R(Self->StreamRxBuf.GetData() + 5, (int32)FrameLen);
				Wire::FServerWelcome Welcome;
				if (!Wire::FCodec::ReadServerWelcome(R, Welcome))
				{
					UE_LOG(LogQuicClient, Warning, TEXT("[QuicClient] malformed ServerWelcome"));
					Self->MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
					return QUIC_STATUS_SUCCESS;
				}

				if (Welcome.ProtocolVersion != (uint32)Constants::ProtocolVersion)
				{
					UE_LOG(LogQuicClient, Warning,
						TEXT("[QuicClient] protocol version mismatch (server=%u client=%u)"),
						Welcome.ProtocolVersion, (uint32)Constants::ProtocolVersion);
					return QUIC_STATUS_SUCCESS;
				}

				Self->AssignedPlayerId     = Welcome.PlayerId;
				Self->ServerTickAtWelcome  = Welcome.ServerTick;
				Self->bHandshakeDone       = true;
				Self->StreamRxBuf.Reset();

				UE_LOG(LogQuicClient, Display,
					TEXT("[QuicClient] ServerWelcome: assigned playerId=%u serverTick=%u"),
					(uint32)Welcome.PlayerId, Welcome.ServerTick);

				FQueuedEvent QEv;
				QEv.Kind             = EQueuedKind::Connected;
				QEv.AssignedPlayerId = Welcome.PlayerId;
				QEv.ServerTick       = Welcome.ServerTick;
				Self->EventQueue.Enqueue(QEv);
				return QUIC_STATUS_SUCCESS;
			}

			case QUIC_STREAM_EVENT_SEND_COMPLETE:
			{
				auto* PSend = static_cast<FQuicClientPendingSend*>(Ev->SEND_COMPLETE.ClientContext);
				FreeClientPendingSend(PSend);
				return QUIC_STATUS_SUCCESS;
			}

			case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
				Self->MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
				return QUIC_STATUS_SUCCESS;

			case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
				Self->MsQuic->StreamClose(Stream);
				if (Self->ControlStream == Stream) Self->ControlStream = nullptr;
				return QUIC_STATUS_SUCCESS;

			default:
				return QUIC_STATUS_SUCCESS;
		}
	}

	// ---------- Snapshot decode ----------

	void FQuicClient::DecodeSnapshotDatagram(const uint8* Bytes, int32 Len)
	{
		Wire::FWireReader R(Bytes, Len);
		Wire::FSnapshot Decoded;
		if (!Wire::FCodec::ReadSnapshotHeader(R, Decoded))
		{
			UE_LOG(LogQuicClient, Verbose, TEXT("[QuicClient] snapshot header decode failed (len=%d)"), Len);
			return;
		}

		if (Decoded.Kind == Wire::ESnapshotKind::Full)
		{
			if (!Wire::FCodec::ReadFullSnapshotBody(R, Decoded))
			{
				UE_LOG(LogQuicClient, Verbose, TEXT("[QuicClient] full snapshot body decode failed (tick=%u)"), Decoded.Tick);
				return;
			}
			
			Baseline      = Decoded;
			bHaveBaseline = true;
		}
		else
		{
			if (!bHaveBaseline || Baseline.Tick != Decoded.BaselineTick)
			{
				UE_LOG(LogQuicClient, Verbose,
					TEXT("[QuicClient] delta tick=%u baseline=%u not available (have=%d ourBaseline=%u) — dropping"),
					Decoded.Tick, Decoded.BaselineTick,
					bHaveBaseline ? 1 : 0,
					bHaveBaseline ? Baseline.Tick : 0u);
				return;
			}
			
			Decoded.Enemies = Baseline.Enemies;

			if (!Wire::FCodec::ApplyDeltaSnapshotBody(R, Decoded))
			{
				UE_LOG(LogQuicClient, Verbose, TEXT("[QuicClient] delta apply failed (tick=%u)"), Decoded.Tick);
				return;
			}

			Baseline = Decoded;
		}

		SnapshotsDecoded.Increment();

		FQueuedEvent QEv;
		QEv.Kind = EQueuedKind::Snapshot;
		QEv.Snap = Decoded;
		EventQueue.Enqueue(QEv);
	}

	// ---------- PollEvents ----------

	void FQuicClient::PollEvents()
	{
		FQueuedEvent Ev;
		while (EventQueue.Dequeue(Ev))
		{
			switch (Ev.Kind)
			{
				case EQueuedKind::Connected:
					if (OnConnected) OnConnected(Ev.AssignedPlayerId, Ev.ServerTick);
					break;
				case EQueuedKind::Disconnected:
					if (OnDisconnected) OnDisconnected();
					break;
				case EQueuedKind::Snapshot:
					if (OnSnapshot) OnSnapshot(Ev.Snap);
					break;
			}
		}
	}
}
