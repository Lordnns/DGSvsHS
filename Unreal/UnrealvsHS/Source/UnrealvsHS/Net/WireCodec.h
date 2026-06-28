// v4 wire codec. Port of csharp_arch_server/Net/WireCodec.cs and
// rust/gameplay/src/network/codec.rs. All integers little-endian. Float
// quantization: positions (i16, ×1000 → mm, ±32.767 m), angles (i16, ×AngleScale).

#pragma once

#include "CoreMinimal.h"
#include "WireTypes.h"

namespace UnrealvsHS::Wire
{
	// ---------- Message type bytes (1-byte msg type after a u32 length header on stream framing) ----------
	constexpr uint8 MsgClientHello   = 0x01;
	constexpr uint8 MsgServerWelcome = 0x02;
	constexpr uint8 MsgInput         = 0x10;
	constexpr uint8 MsgSnapshot      = 0x20;
	constexpr uint8 MsgDisconnect    = 0xF0;

	// ---------- Fixed wire sizes (kept in sync with WireFormat.md §4.4) ----------
	constexpr int32 ServerWelcomePayloadBytes = 13;
	constexpr int32 InputCmdWireBytes         = 15;
	constexpr int32 MaxInputBatch             = 4;
	constexpr int32 SnapshotHeaderBytes       = 1 + 4 + 4 + 4 + 2 + 4 + 4 + 1;   // 24
	constexpr int32 PlayerSnapFullBytes       = 1 + 2 + 2 + 2 + 1 + 2;           // 10
	constexpr int32 EnemySnapFullBytes        = 2 + 2 + 2;                       // 6
	constexpr int32 EnemyDeltaEntryBytes      = 2 + 2 + 2;                       // 6
	constexpr int32 FireEventBytes            = 4 + 1 + 2 + 2 + 2 + 2 + 1;       // 14
	constexpr int32 FullBodyArrayHeaderBytes  = 1 + 2 + 4 + 1;                   // 8

	// ---------- Byte writer / reader (little-endian) ----------

	struct FWireWriter
	{
		TArray<uint8>& Buf;
		explicit FWireWriter(TArray<uint8>& B) : Buf(B) {}

		void U8(uint8 V)   { Buf.Add(V); }
		void U16(uint16 V) { Buf.Add((uint8)(V & 0xFF)); Buf.Add((uint8)((V >> 8) & 0xFF)); }
		void U32(uint32 V) { for (int i = 0; i < 4; ++i) Buf.Add((uint8)((V >> (i * 8)) & 0xFF)); }
		void I16(int16 V)  { U16((uint16)V); }
		void F32(float V)  { uint32 U; FMemory::Memcpy(&U, &V, 4); U32(U); }
	};

	struct FWireReader
	{
		const uint8* Buf;
		int32        Len;
		int32        Pos = 0;

		FWireReader(const uint8* B, int32 L) : Buf(B), Len(L) {}

		bool   Has(int32 N) const { return Pos + N <= Len; }
		int32  Remaining()  const { return Len - Pos; }

		uint8  U8()  { return Buf[Pos++]; }
		uint16 U16() { uint16 V = (uint16)Buf[Pos] | ((uint16)Buf[Pos+1] << 8); Pos += 2; return V; }
		uint32 U32() { uint32 V = 0; for (int i = 0; i < 4; ++i) V |= (uint32)Buf[Pos+i] << (i * 8); Pos += 4; return V; }
		int16  I16() { return (int16)U16(); }
		float  F32() { uint32 U = U32(); float V; FMemory::Memcpy(&V, &U, 4); return V; }
	};

	// ---------- Codec entry points ----------

	struct FCodec
	{
		// ---------- ClientHello (0x01) ----------
		static void  WriteClientHello(TArray<uint8>& Out, uint8 Capabilities);
		static bool  ReadClientHello(FWireReader& R, uint32& OutVersion, uint8& OutCapabilities);

		// ---------- ServerWelcome (0x02) ----------
		static void  WriteServerWelcome(TArray<uint8>& Out, uint8 PlayerId, uint32 ServerTick);
		static bool  ReadServerWelcome(FWireReader& R, FServerWelcome& Out);

		// ---------- Input batch (0x10) ----------
		// `OutCmds` receives 1..MaxInputBatch decoded commands. Returns count, -1 on error.
		static void  WriteInputBatch(TArray<uint8>& Out, const FInputCmd* Cmds, int32 Count);
		static int32 ReadInputBatch(FWireReader& R, FInputCmd OutCmds[MaxInputBatch]);

		// ---------- Snapshot header (0x20 body prefix) ----------
		static void  WriteSnapshotHeader(TArray<uint8>& Out, const FSnapshot& Snap);
		static bool  ReadSnapshotHeader(FWireReader& R, FSnapshot& OutHeader);

		// ---------- Snapshot Full body ----------
		static void  WriteFullSnapshotBody(
			TArray<uint8>& Out,
			TArrayView<const FPlayerSnap> Players,
			TArrayView<const FEnemySnap>  Enemies,
			uint32 EnemyTotalInWorld,
			TArrayView<const FFireEvent>  Fires);

		static bool  ReadFullSnapshotBody(FWireReader& R, FSnapshot& InOut);

		// ---------- Snapshot Delta body ----------
		static void  WriteDeltaSnapshotBody(
			TArray<uint8>& Out,
			TArrayView<const FPlayerSnap>      Players,
			TArrayView<const FEnemyDeltaEntry> Changed,
			TArrayView<const uint16>           Removed,
			TArrayView<const FEnemySnap>       Added,
			uint32 EnemyTotalInWorld,
			TArrayView<const FFireEvent>       Fires);
		
		static bool  ApplyDeltaSnapshotBody(FWireReader& R, FSnapshot& InOut);

		// ---------- Stream framing helper ----------
		static void  FrameStreamMessage(TArray<uint8>& Out, uint8 MsgType, TArrayView<const uint8> Payload);

		// ---------- Quantization helpers ----------
		static int16 QuantPos(float Meters);
		static float DequantPos(int16 Q);
		static int16 QuantAngle(float DirX, float DirY);
		static void  DequantAngle(int16 Q, float& OutDirX, float& OutDirY);
		static uint16 QuantDisable(float Seconds);
		static float DequantDisable(uint16 T);

		static bool  EnemyPositionChanged(const FEnemySnap& Baseline, const FEnemySnap& Current);
	};
}
