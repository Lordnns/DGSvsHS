#include "QuicServer.h"
#include "QuicCert.h"               // self-signed cert generator (OpenSSL isolated)
#include "Gameplay/UvHSConstants.h"

// Linux PEM-file cert path uses these (defined unconditionally — CoreMinimal
// already pulls most of them but we include explicitly to be safe in IWYU).
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformFile.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif


DEFINE_LOG_CATEGORY_STATIC(LogQuicServer, Log, All);

namespace UnrealvsHS::Net
{
	using namespace Constants;
	using UnrealvsHS::Server::FRecipientSnapshotState;
	using UnrealvsHS::Server::FSnapshotPriority;
	using UnrealvsHS::Server::FWorldStateHistory;

#if PLATFORM_LINUX
#include <dlfcn.h>
	typedef QUIC_STATUS (QUIC_API *FMsQuicOpenVersion)(uint32_t, const void**);
	typedef void (QUIC_API *FMsQuicClose)(const void*);
	static FMsQuicOpenVersion GMsQuicOpenVersion = nullptr;
	static FMsQuicClose GMsQuicClose = nullptr;
	static void* GMsQuicLibHandle = nullptr;
#endif

	// ---------- Slot type (definition lives here so the header doesn't pull msquic.h) ----------

	struct FQuicSlot
	{
		uint8           PlayerId        = 0;
		HQUIC           Connection      = nullptr;
		HQUIC           ControlStream   = nullptr;
		bool            bHandshakeDone  = false;
		TArray<uint8>   RxBuf;
		FQuicServer*    OwnerServer     = nullptr;
	};
	
	struct FQuicPendingSend
	{
		QUIC_BUFFER QuicBuf;
		uint8*      Data   = nullptr;
		uint32      Length = 0;
	};

	static FQuicPendingSend* MakePendingSendCopy(const uint8* SrcBytes, int32 SrcLen)
	{
		auto* S = new FQuicPendingSend();
		S->Data   = new uint8[SrcLen];
		S->Length = (uint32)SrcLen;
		FMemory::Memcpy(S->Data, SrcBytes, SrcLen);
		S->QuicBuf.Buffer = S->Data;
		S->QuicBuf.Length = (uint32)SrcLen;
		return S;
	}

	static void FreePendingSend(FQuicPendingSend* S)
	{
		if (!S) return;
		delete[] S->Data;
		delete S;
	}

	// ---------- Lifecycle ----------

	FQuicServer::FQuicServer()
	{
		Slots.SetNum(Constants::MaxPlayers);
		RecipientStates.SetNum(Constants::MaxPlayers);
		HighestInputTick.SetNum(Constants::MaxPlayers);
		for (int32 i = 0; i < Constants::MaxPlayers; ++i)
		{
			HighestInputTick[i] = MakeUnique<FThreadSafeCounter>();
		}
	}

	FQuicServer::~FQuicServer()
	{
		Stop();
	}

	bool FQuicServer::Start(uint16 ListenPort)
	{
		Port = ListenPort;

		if (!LoadMsQuic())             { UE_LOG(LogQuicServer, Error, TEXT("LoadMsQuic failed"));         return false; }
		if (!OpenRegistration())       { UE_LOG(LogQuicServer, Error, TEXT("OpenRegistration failed"));  Stop(); return false; }
		if (!LoadServerCertificate())  { UE_LOG(LogQuicServer, Error, TEXT("Cert generation failed"));   Stop(); return false; }
		if (!OpenConfiguration())      { UE_LOG(LogQuicServer, Error, TEXT("OpenConfiguration failed")); Stop(); return false; }
		if (!OpenListener(ListenPort)) { UE_LOG(LogQuicServer, Error, TEXT("OpenListener failed"));      Stop(); return false; }

		bRunning = true;
		UE_LOG(LogQuicServer, Display,
			TEXT("[QuicServer] msquic initialized, ALPN=dgsvshs/2, port=%u, MaxPlayers=%d"),
			(uint32)Port, Constants::MaxPlayers);
		return true;
	}

	void FQuicServer::Stop()
	{
		if (MsQuic == nullptr) return;
		
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			if (Slots[i].IsValid() && Slots[i]->Connection != nullptr)
			{
				MsQuic->ConnectionShutdown(Slots[i]->Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
				MsQuic->ConnectionClose(Slots[i]->Connection);
				Slots[i]->Connection = nullptr;
			}
			Slots[i].Reset();
		}

		if (Listener)      { MsQuic->ListenerClose(Listener);      Listener = nullptr; }
		if (Configuration) { MsQuic->ConfigurationClose(Configuration); Configuration = nullptr; }
		if (Registration)  { MsQuic->RegistrationClose(Registration);   Registration = nullptr; }

#if PLATFORM_LINUX
		if (GMsQuicClose) GMsQuicClose(MsQuic);
#else
		MsQuicClose(MsQuic);
#endif
		MsQuic = nullptr;
		CertPkcs12.Reset();
		bRunning = false;
		UE_LOG(LogQuicServer, Display, TEXT("[QuicServer] stopped"));
	}

	// ---------- msquic init ----------

	bool FQuicServer::LoadMsQuic()
	{
#if PLATFORM_LINUX
		if (!GMsQuicLibHandle)
		{
			GMsQuicLibHandle = dlopen("libmsquic.so.2", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
			if (!GMsQuicLibHandle)
			{
				UE_LOG(LogQuicServer, Error, TEXT("dlopen libmsquic.so.2 failed: %s"), UTF8_TO_TCHAR(dlerror()));
				return false;
			}
			GMsQuicOpenVersion = (FMsQuicOpenVersion)dlsym(GMsQuicLibHandle, "MsQuicOpenVersion");
			GMsQuicClose = (FMsQuicClose)dlsym(GMsQuicLibHandle, "MsQuicClose");
		}
		if (!GMsQuicOpenVersion || !GMsQuicClose)
		{
			UE_LOG(LogQuicServer, Error, TEXT("Failed to dlsym MsQuicOpenVersion or MsQuicClose"));
			return false;
		}
		const QUIC_STATUS S = GMsQuicOpenVersion(2, (const void**)&MsQuic);
#else
		const QUIC_STATUS S = MsQuicOpen2(&MsQuic);
#endif

		if (QUIC_FAILED(S))
		{
			UE_LOG(LogQuicServer, Error, TEXT("MsQuicOpen2 failed: 0x%X"), S);
			return false;
		}
		return true;
	}

	bool FQuicServer::OpenRegistration()
	{
		QUIC_REGISTRATION_CONFIG RegConfig = {};
		RegConfig.AppName       = "UnrealvsHS";
		RegConfig.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;

		const QUIC_STATUS S = MsQuic->RegistrationOpen(&RegConfig, &Registration);
		if (QUIC_FAILED(S))
		{
			UE_LOG(LogQuicServer, Error, TEXT("RegistrationOpen failed: 0x%X"), S);
			return false;
		}
		return true;
	}

	bool FQuicServer::OpenConfiguration()
	{
		static const char* AlpnString = "dgsvshs/2";
		QUIC_BUFFER Alpn = {};
		Alpn.Buffer = (uint8_t*)AlpnString;
		Alpn.Length = (uint32)FCStringAnsi::Strlen(AlpnString);

		QUIC_SETTINGS Settings = {};
		Settings.IdleTimeoutMs                       = 30000;
		Settings.IsSet.IdleTimeoutMs                 = true;
		Settings.PeerBidiStreamCount                 = 4;
		Settings.IsSet.PeerBidiStreamCount           = true;
		Settings.PeerUnidiStreamCount                = 4;
		Settings.IsSet.PeerUnidiStreamCount          = true;
		Settings.DatagramReceiveEnabled              = true;
		Settings.IsSet.DatagramReceiveEnabled        = true;
		Settings.SendBufferingEnabled                = false;
		Settings.IsSet.SendBufferingEnabled          = true;

		const QUIC_STATUS S = MsQuic->ConfigurationOpen(
			Registration, &Alpn, 1, &Settings, sizeof(Settings), nullptr, &Configuration);
		if (QUIC_FAILED(S))
		{
			UE_LOG(LogQuicServer, Error, TEXT("ConfigurationOpen failed: 0x%X"), S);
			return false;
		}

		QUIC_CREDENTIAL_CONFIG CredConfig = {};
		CredConfig.Flags                 = QUIC_CREDENTIAL_FLAG_NONE;

#if PLATFORM_WINDOWS
		CRYPT_DATA_BLOB Blob = {};
		Blob.cbData = (DWORD)CertPkcs12.Num();
		Blob.pbData = (BYTE*)CertPkcs12.GetData();
		HCERTSTORE Store = PFXImportCertStore(&Blob, L"", 0);
		if (!Store)
		{
			UE_LOG(LogQuicServer, Error, TEXT("PFXImportCertStore failed"));
			return false;
		}
		PCCERT_CONTEXT CertCtx = CertEnumCertificatesInStore(Store, nullptr);
		if (!CertCtx)
		{
			CertCloseStore(Store, 0);
			UE_LOG(LogQuicServer, Error, TEXT("CertEnumCertificatesInStore failed"));
			return false;
		}
		CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
		CredConfig.CertificateContext = (QUIC_CERTIFICATE*)CertCtx;
#else
		// Linux: PKCS12 in-memory loading via libmsquic SEGVs on this distro's
		// libmsquic.so.2 (root cause not pinned — likely an OpenSSL provider
		// mismatch between Unreal's bundled OpenSSL emitting the PFX and the
		// system OpenSSL libmsquic links against). Workaround: write cert +
		// key as PEM to two temp files, load via the FILE credential type,
		// then unlink immediately (msquic parses them synchronously).
		const FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MsQuicCert"));
		IPlatformFile::GetPlatformPhysical().CreateDirectoryTree(*TempDir);
		const int32 Pid       = (int32)FPlatformProcess::GetCurrentProcessId();
		const FString CertFs  = FPaths::Combine(TempDir, FString::Printf(TEXT("uvhs_cert_%d.pem"), Pid));
		const FString KeyFs   = FPaths::Combine(TempDir, FString::Printf(TEXT("uvhs_key_%d.pem"),  Pid));
		const FTCHARToUTF8 CertPathUtf8(*CertFs);
		const FTCHARToUTF8 KeyPathUtf8 (*KeyFs);

		if (!UnrealvsHS::Net::GenerateSelfSignedPemFiles(CertPathUtf8.Get(), KeyPathUtf8.Get()))
		{
			UE_LOG(LogQuicServer, Error, TEXT("[QuicServer] PEM cert/key generation failed (paths: %s, %s)"),
				*CertFs, *KeyFs);
			return false;
		}
		UE_LOG(LogQuicServer, Display,
			TEXT("[QuicServer] wrote dev cert PEM pair to %s (+ key) — file-based cert load"),
			*CertFs);

		QUIC_CERTIFICATE_FILE CertFile = {};
		CertFile.PrivateKeyFile  = KeyPathUtf8.Get();
		CertFile.CertificateFile = CertPathUtf8.Get();

		CredConfig.Type            = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
		CredConfig.CertificateFile = &CertFile;
#endif

		const QUIC_STATUS S2 = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig);

#if PLATFORM_WINDOWS
		if (CertCtx) CertFreeCertificateContext(CertCtx);
		if (Store) CertCloseStore(Store, 0);
#else
		// msquic parses the PEM files synchronously into in-memory cert + key,
		// so we can unlink immediately. Failure to unlink is non-fatal.
		IFileManager::Get().Delete(*CertFs, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		IFileManager::Get().Delete(*KeyFs,  /*RequireExists*/ false, /*EvenReadOnly*/ true);
#endif

		if (QUIC_FAILED(S2))
		{
			UE_LOG(LogQuicServer, Error, TEXT("ConfigurationLoadCredential failed: 0x%X"), S2);
			return false;
		}
		return true;
	}

	// ---------- Self-signed cert via OpenSSL ----------

	bool FQuicServer::LoadServerCertificate()
	{
		int32_t PfxLen = 0;
		uint8_t* PfxBytes = UnrealvsHS::Net::GenerateSelfSignedPkcs12(&PfxLen);
		if (!PfxBytes || PfxLen <= 0)
		{
			UE_LOG(LogQuicServer, Error, TEXT("[QuicServer] cert generation failed"));
			return false;
		}
		CertPkcs12.Reset(PfxLen);
		CertPkcs12.Append(PfxBytes, PfxLen);
		UnrealvsHS::Net::FreePkcs12Buffer(PfxBytes);

		UE_LOG(LogQuicServer, Display, TEXT("[QuicServer] generated dev cert (PFX %d bytes)"), CertPkcs12.Num());
		return true;
	}

	// ---------- Listener ----------

	bool FQuicServer::OpenListener(uint16 InPort)
	{
		const QUIC_STATUS S = MsQuic->ListenerOpen(
			Registration, &FQuicServer::NetServerListenerCallback, this, &Listener);
		if (QUIC_FAILED(S))
		{
			UE_LOG(LogQuicServer, Error, TEXT("ListenerOpen failed: 0x%X"), S);
			return false;
		}
		
		QUIC_ADDR Addr = {};
		QuicAddrSetFamily(&Addr, QUIC_ADDRESS_FAMILY_UNSPEC);
		QuicAddrSetPort(&Addr, InPort);

		static const char* AlpnString = "dgsvshs/2";
		QUIC_BUFFER Alpn = {};
		Alpn.Buffer = (uint8_t*)AlpnString;
		Alpn.Length = (uint32)FCStringAnsi::Strlen(AlpnString);

		const QUIC_STATUS S2 = MsQuic->ListenerStart(Listener, &Alpn, 1, &Addr);
		if (QUIC_FAILED(S2))
		{
			UE_LOG(LogQuicServer, Error, TEXT("ListenerStart on port %u failed: 0x%X"), (uint32)InPort, S2);
			return false;
		}
		return true;
	}

	// ---------- Slot management ----------

	int32 FQuicServer::FindFreeSlot() const
	{
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			if (!Slots[i].IsValid()) return i;
		}
		return INDEX_NONE;
	}

	void FQuicServer::FreeSlot(int32 Index)
	{
		if (!Slots.IsValidIndex(Index) || !Slots[Index].IsValid()) return;
		const uint8 Pid = Slots[Index]->PlayerId;
		if (Slots[Index]->Connection)
		{
			MsQuic->ConnectionClose(Slots[Index]->Connection);
			Slots[Index]->Connection = nullptr;
		}
		Slots[Index].Reset();
		if (OnClientDisconnected) OnClientDisconnected(Pid);
	}

	// ---------- msquic callbacks ----------
	
	QUIC_STATUS QUIC_API FQuicServer::NetServerListenerCallback(
		HQUIC InListener, void* Context, QUIC_LISTENER_EVENT* Ev)
	{
		auto* Self = static_cast<FQuicServer*>(Context);
		switch (Ev->Type)
		{
			case QUIC_LISTENER_EVENT_NEW_CONNECTION:
			{
				HQUIC Conn = Ev->NEW_CONNECTION.Connection;
					
				const int32 Slot = Self->FindFreeSlot();
				if (Slot == INDEX_NONE)
				{
					UE_LOG(LogQuicServer, Warning, TEXT("server full, rejecting incoming connection"));
					Self->MsQuic->ConnectionShutdown(Conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
					return QUIC_STATUS_CONNECTION_REFUSED;
				}

				Self->Slots[Slot] = MakeUnique<FQuicSlot>();
				Self->Slots[Slot]->PlayerId   = (uint8)Slot;
				Self->Slots[Slot]->Connection = Conn;

				if (Self->RecipientStates[Slot].IsValid()) Self->RecipientStates[Slot]->Clear();
				else Self->RecipientStates[Slot] = MakeUnique<UnrealvsHS::Server::FRecipientSnapshotState>();
				Self->HighestInputTick[Slot]->Reset();

				Self->Slots[Slot]->OwnerServer = Self;

				Self->MsQuic->SetCallbackHandler(
					Conn, (void*)&FQuicServer::NetServerConnectionCallback,
					/*context*/ Self->Slots[Slot].Get());

				Self->MsQuic->ConnectionSetConfiguration(Conn, Self->Configuration);

				UE_LOG(LogQuicServer, Display, TEXT("[QuicServer] accept → slot %d"), Slot);

				// Queue Connected event for the game thread; PollEvents fires the hook.
				FQueuedEvent QEv; QEv.Kind = EQueuedKind::Connected; QEv.PlayerId = (uint8)Slot;
				Self->EventQueue.Enqueue(QEv);
				return QUIC_STATUS_SUCCESS;
			}
			default:
				return QUIC_STATUS_SUCCESS;
		}
	}

	QUIC_STATUS QUIC_API FQuicServer::NetServerConnectionCallback(
		HQUIC InConn, void* Context, QUIC_CONNECTION_EVENT* Ev)
	{
		auto* Slot  = static_cast<FQuicSlot*>(Context);
		FQuicServer* Self = Slot ? Slot->OwnerServer : nullptr;
		if (!Self) return QUIC_STATUS_SUCCESS;

		switch (Ev->Type)
		{
			case QUIC_CONNECTION_EVENT_CONNECTED:
				UE_LOG(LogQuicServer, Display, TEXT("[QuicServer] slot %u: TLS handshake complete"), Slot->PlayerId);
				return QUIC_STATUS_SUCCESS;

			case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
			{
				HQUIC Stream = Ev->PEER_STREAM_STARTED.Stream;
				Slot->ControlStream = Stream;
				Self->MsQuic->SetCallbackHandler(
					Stream, (void*)&FQuicServer::NetServerStreamCallback, /*context*/ Slot);
				UE_LOG(LogQuicServer, Verbose, TEXT("[QuicServer] slot %u: control stream opened"), Slot->PlayerId);
				return QUIC_STATUS_SUCCESS;
			}

			case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
			{

				const QUIC_BUFFER* Buf = Ev->DATAGRAM_RECEIVED.Buffer;
				Self->DatagramsRx.Increment();
				if (!Buf || Buf->Length < 1) return QUIC_STATUS_SUCCESS;
				const uint8 MsgType = Buf->Buffer[0];
				if (MsgType != Wire::MsgInput) return QUIC_STATUS_SUCCESS;

				Wire::FInputCmd Batch[Wire::MaxInputBatch];
				Wire::FWireReader R(Buf->Buffer + 1, (int32)Buf->Length - 1);
				const int32 Count = Wire::FCodec::ReadInputBatch(R, Batch);
				if (Count <= 0) return QUIC_STATUS_SUCCESS;

				Self->InputBatchRx.Increment();
				uint32 BatchMaxClientTick = 0;
				uint32 BatchMaxServerAck  = 0;
				for (int32 i = 0; i < Count; ++i)
				{
					if (Batch[i].Tick > BatchMaxClientTick) BatchMaxClientTick = Batch[i].Tick;
					if (Batch[i].LastAckedServerTick > BatchMaxServerAck) BatchMaxServerAck = Batch[i].LastAckedServerTick;

					FQueuedEvent QEv;
					QEv.Kind     = EQueuedKind::Input;
					QEv.PlayerId = Slot->PlayerId;
					QEv.Input    = Batch[i];
					Self->EventQueue.Enqueue(QEv);
					Self->InputCmdRx.Increment();
				}
					
				{
					FThreadSafeCounter& Counter = *Self->HighestInputTick[Slot->PlayerId];
					const int32 Cur = Counter.GetValue();
					if ((int32)BatchMaxClientTick > Cur) Counter.Set((int32)BatchMaxClientTick);
				}

				// Same for PendingAckedTick — drained by the game thread on
				// the next BroadcastSnapshot.
				if (BatchMaxServerAck > 0 && Self->RecipientStates[Slot->PlayerId].IsValid())
				{
					FThreadSafeCounter& PA = Self->RecipientStates[Slot->PlayerId]->PendingAckedTick;
					const int32 Cur = PA.GetValue();
					if ((int32)BatchMaxServerAck > Cur) PA.Set((int32)BatchMaxServerAck);
				}
				return QUIC_STATUS_SUCCESS;
			}

			case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
			{
				auto* PSend = static_cast<FQuicPendingSend*>(Ev->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
				const QUIC_DATAGRAM_SEND_STATE St = Ev->DATAGRAM_SEND_STATE_CHANGED.State;
					
				const bool bTerminal =
					St == QUIC_DATAGRAM_SEND_LOST_DISCARDED
					|| St == QUIC_DATAGRAM_SEND_ACKNOWLEDGED
					|| St == QUIC_DATAGRAM_SEND_ACKNOWLEDGED_SPURIOUS
					|| St == QUIC_DATAGRAM_SEND_CANCELED;
				if (bTerminal && PSend) FreePendingSend(PSend);
				return QUIC_STATUS_SUCCESS;
			}

			case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
			{
				UE_LOG(LogQuicServer, Display, TEXT("[QuicServer] slot %u: shutdown complete"), Slot->PlayerId);
				const uint8 Pid = Slot->PlayerId;
					
				FQueuedEvent QEv; QEv.Kind = EQueuedKind::Disconnected; QEv.PlayerId = Pid;
				Self->EventQueue.Enqueue(QEv);
					
				if (Slot->Connection)
				{
					Self->MsQuic->ConnectionClose(Slot->Connection);
				}
				for (int32 i = 0; i < Self->Slots.Num(); ++i)
				{
					if (Self->Slots[i].Get() == Slot) { Self->Slots[i].Reset(); break; }
				}
				return QUIC_STATUS_SUCCESS;
			}

			default:
				return QUIC_STATUS_SUCCESS;
		}
	}

	// ---------- Stream callback (ClientHello → ServerWelcome) ----------

	QUIC_STATUS QUIC_API FQuicServer::NetServerStreamCallback(
		HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Ev)
	{
		auto* Slot  = static_cast<FQuicSlot*>(Context);
		FQuicServer* Self = Slot ? Slot->OwnerServer : nullptr;
		if (!Self) return QUIC_STATUS_SUCCESS;

		switch (Ev->Type)
		{
			case QUIC_STREAM_EVENT_RECEIVE:
			{
				const uint32 BufCount = Ev->RECEIVE.BufferCount;
				for (uint32 b = 0; b < BufCount; ++b)
				{
					const QUIC_BUFFER& Bb = Ev->RECEIVE.Buffers[b];
					Slot->RxBuf.Append(Bb.Buffer, (int32)Bb.Length);
				}

				if (Slot->bHandshakeDone) return QUIC_STATUS_SUCCESS;
					
				if (Slot->RxBuf.Num() < 5) return QUIC_STATUS_SUCCESS;

				const uint32 FrameLen =
					(uint32)Slot->RxBuf[0]
					| ((uint32)Slot->RxBuf[1] << 8)
					| ((uint32)Slot->RxBuf[2] << 16)
					| ((uint32)Slot->RxBuf[3] << 24);

				const int32 TotalNeeded = 4 + 1 + (int32)FrameLen;
				if (Slot->RxBuf.Num() < TotalNeeded) return QUIC_STATUS_SUCCESS;

				const uint8 MsgType = Slot->RxBuf[4];
				if (MsgType != Wire::MsgClientHello)
				{
					UE_LOG(LogQuicServer, Warning, TEXT("[QuicServer] slot %u: unexpected control msg 0x%02X (expected ClientHello)"),
						Slot->PlayerId, MsgType);
					Self->MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
					return QUIC_STATUS_SUCCESS;
				}

				Wire::FWireReader R(Slot->RxBuf.GetData() + 5, (int32)FrameLen);
				uint32 ClientVersion = 0;
				uint8  Caps          = 0;
				if (!Wire::FCodec::ReadClientHello(R, ClientVersion, Caps))
				{
					UE_LOG(LogQuicServer, Warning, TEXT("[QuicServer] slot %u: malformed ClientHello"), Slot->PlayerId);
					Self->MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
					return QUIC_STATUS_SUCCESS;
				}

				UE_LOG(LogQuicServer, Display,
					TEXT("[QuicServer] slot %u: ClientHello version=%u caps=0x%02X"),
					Slot->PlayerId, ClientVersion, Caps);

				if (ClientVersion != (uint32)Constants::ProtocolVersion)
				{
					UE_LOG(LogQuicServer, Warning,
						TEXT("[QuicServer] slot %u: protocol version mismatch (client=%u server=%u) — disconnecting"),
						Slot->PlayerId, ClientVersion, (uint32)Constants::ProtocolVersion);
					Self->MsQuic->ConnectionShutdown(Slot->Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
					return QUIC_STATUS_SUCCESS;
				}
					
				TArray<uint8> Payload;
				Wire::FCodec::WriteServerWelcome(Payload, Slot->PlayerId, Self->CurrentServerTick);
				TArray<uint8> Framed;
				Wire::FCodec::FrameStreamMessage(Framed, Wire::MsgServerWelcome, Payload);

				FQuicPendingSend* PSend = MakePendingSendCopy(Framed.GetData(), Framed.Num());
				const QUIC_STATUS S = Self->MsQuic->StreamSend(
					Stream, &PSend->QuicBuf, 1, QUIC_SEND_FLAG_NONE, /*context*/ PSend);
				if (QUIC_FAILED(S))
				{
					FreePendingSend(PSend);
					UE_LOG(LogQuicServer, Warning, TEXT("[QuicServer] slot %u: ServerWelcome StreamSend failed 0x%X"),
						Slot->PlayerId, S);
					return QUIC_STATUS_SUCCESS;
				}

				UE_LOG(LogQuicServer, Display, TEXT("[QuicServer] slot %u: sent ServerWelcome (%d B)"),
					Slot->PlayerId, Framed.Num());

				Slot->bHandshakeDone = true;
				Slot->RxBuf.Reset();
				return QUIC_STATUS_SUCCESS;
			}

			case QUIC_STREAM_EVENT_SEND_COMPLETE:
			{
				auto* PSend = static_cast<FQuicPendingSend*>(Ev->SEND_COMPLETE.ClientContext);
				FreePendingSend(PSend);
				return QUIC_STATUS_SUCCESS;
			}

			case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
				Self->MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
				return QUIC_STATUS_SUCCESS;

			case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
				Self->MsQuic->StreamClose(Stream);
				if (Slot->ControlStream == Stream) Slot->ControlStream = nullptr;
				return QUIC_STATUS_SUCCESS;

			default:
				return QUIC_STATUS_SUCCESS;
		}
	}

	// ---------- Snapshot broadcast ----------

	void FQuicServer::BroadcastSnapshot(const Wire::FSnapshot& Snap, const FWorldStateHistory& History)
	{
		const int32 PlayersBytes  = 1 + Snap.Players.Num() * Wire::PlayerSnapFullBytes;
		const int32 FiresCount    = FMath::Min(16, Snap.RecentFireEvents.Num());
		const int32 FiresBytes    = 1 + FiresCount * Wire::FireEventBytes;
		const int32 FixedOverhead = 1 + Wire::SnapshotHeaderBytes + PlayersBytes + FiresBytes;   // 1 = msg_type

		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			FQuicSlot* Slot = Slots[i].Get();
			if (!Slot || !Slot->Connection || !Slot->bHandshakeDone) continue;

			FRecipientSnapshotState* R = RecipientStates[i].Get();
			if (!R) continue;
			
			const int32 PendingAck = R->PendingAckedTick.Reset();
			if (PendingAck > 0 && (uint32)PendingAck > R->LastAckedServerTick)
			{
				R->LastAckedServerTick = (uint32)PendingAck;
				R->OnAckAdvanced();
			}
			
			const Wire::FSnapshot* Baseline = nullptr;
			bool   bUseDelta    = false;
			uint32 BaselineTick = 0;
			if (R->LastAckedServerTick > 0
				&& Snap.Tick >= R->LastAckedServerTick
				&& (Snap.Tick - R->LastAckedServerTick) <= (uint32)Constants::MaxDeltaDepth)
			{
				if (const Wire::FSnapshot* B = History.TryGet(R->LastAckedServerTick))
				{
					Baseline     = B;
					bUseDelta    = true;
					BaselineTick = R->LastAckedServerTick;
				}
			}
			
			FVector2D Anchor = FVector2D::ZeroVector;
			for (const Wire::FPlayerSnap& P : Snap.Players)
			{
				if (P.Id == Slot->PlayerId) { Anchor = P.Position; break; }
			}
			
			const int32 EnemySectionHeader = bUseDelta ? (2 + 2 + 2 + 4) : (2 + 4);
			const int32 EnemyBudget = FMath::Max(0, Constants::SnapshotByteBudget - FixedOverhead - EnemySectionHeader);
			
			Wire::FSnapshot SnapForHeader = Snap;
			SnapForHeader.Kind                   = bUseDelta ? Wire::ESnapshotKind::Delta : Wire::ESnapshotKind::Full;
			SnapForHeader.BaselineTick           = bUseDelta ? BaselineTick : 0u;
			SnapForHeader.LastProcessedInputTick = (uint32)HighestInputTick[i]->GetValue();

			TArray<uint8> Body;
			Body.Reserve(1 + Wire::SnapshotHeaderBytes + Constants::SnapshotByteBudget);
			Body.Add(Wire::MsgSnapshot);
			Wire::FCodec::WriteSnapshotHeader(Body, SnapForHeader);

			if (bUseDelta)
			{
				FSnapshotPriority::SelectForDelta(
					Snap, *Baseline, Anchor,
					&R->ConfirmedIds, &R->TicksSinceLastSent,
					EnemyBudget,
					ChangedScratch, RemovedScratch, AddedScratch,
					IncludedScratch, ScoredScratch,
					CurrentIdsScratch, BaselineIndexScratch);

				Wire::FCodec::WriteDeltaSnapshotBody(
					Body, Snap.Players,
					ChangedScratch, RemovedScratch, AddedScratch,
					(uint32)Snap.Enemies.Num(),
					Snap.RecentFireEvents);
			}
			else
			{
				FSnapshotPriority::SelectForFull(Snap, Anchor, EnemyBudget,
					SelectedEnemiesScratch, ScoredScratch);

				Wire::FCodec::WriteFullSnapshotBody(
					Body, Snap.Players, SelectedEnemiesScratch,
					(uint32)Snap.Enemies.Num(),
					Snap.RecentFireEvents);
				
				IncludedScratch.Reset();
				for (const Wire::FEnemySnap& E : SelectedEnemiesScratch) IncludedScratch.Add(E.Id);
				RemovedScratch.Reset();
			}

			uint16 MaxLen = 0;
			uint32 ParamLen = sizeof(MaxLen);
			MsQuic->GetParam(Slot->Connection, QUIC_PARAM_CONN_DATAGRAM_SEND_ENABLED, &ParamLen, &MaxLen);

			FQuicPendingSend* PSend = MakePendingSendCopy(Body.GetData(), Body.Num());
			const QUIC_STATUS S = MsQuic->DatagramSend(
				Slot->Connection, &PSend->QuicBuf, 1, QUIC_SEND_FLAG_NONE, /*context*/ PSend);

			if (QUIC_FAILED(S))
			{
				FreePendingSend(PSend);
				UE_LOG(LogQuicServer, Verbose,
					TEXT("[QuicServer] slot %u: DatagramSend failed 0x%X (size=%d, %s)"),
					Slot->PlayerId, S, Body.Num(), bUseDelta ? TEXT("Delta") : TEXT("Full"));
				continue;
			}
			
			R->OnSnapshotSent(Snap.Tick, /*bIsFull*/ !bUseDelta, IncludedScratch, RemovedScratch);
		}
	}

	// ---------- PollEvents — drain network thread → game thread ----------

	void FQuicServer::PollEvents()
	{
		FQueuedEvent Ev;
		while (EventQueue.Dequeue(Ev))
		{
			switch (Ev.Kind)
			{
				case EQueuedKind::Connected:
					if (OnClientConnected)    OnClientConnected(Ev.PlayerId);
					break;
				case EQueuedKind::Disconnected:
					if (OnClientDisconnected) OnClientDisconnected(Ev.PlayerId);
					break;
				case EQueuedKind::Input:
					if (OnInputReceived)      OnInputReceived(Ev.PlayerId, Ev.Input);
					break;
			}
		}
	}
}
