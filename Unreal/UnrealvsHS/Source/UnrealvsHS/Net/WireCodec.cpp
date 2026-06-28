#include "WireCodec.h"
#include "UvHSConstants.h"

namespace UnrealvsHS::Wire
{
	// ---------- Quantization ----------

	int16 FCodec::QuantPos(float Meters)
	{
		int32 Q = FMath::RoundToInt(Meters * (float)Constants::PositionScale);
		return (int16)FMath::Clamp(Q, (int32)MIN_int16, (int32)MAX_int16);
	}

	float FCodec::DequantPos(int16 Q)
	{
		return (float)Q / (float)Constants::PositionScale;
	}

	int16 FCodec::QuantAngle(float DirX, float DirY)
	{
		float A = (DirX * DirX + DirY * DirY) > 1e-4f ? FMath::Atan2(DirY, DirX) : 0.0f;
		int32 Q = FMath::RoundToInt(A * (float)Constants::AngleScale);
		return (int16)FMath::Clamp(Q, (int32)MIN_int16, (int32)MAX_int16);
	}

	void FCodec::DequantAngle(int16 Q, float& OutDirX, float& OutDirY)
	{
		float A = (float)Q / (float)Constants::AngleScale;
		OutDirX = FMath::Cos(A);
		OutDirY = FMath::Sin(A);
	}

	uint16 FCodec::QuantDisable(float Seconds)
	{
		int32 T = FMath::RoundToInt(Seconds * Constants::TicksPerSecond);
		return (uint16)FMath::Clamp(T, 0, (int32)MAX_uint16);
	}

	float FCodec::DequantDisable(uint16 T)
	{
		return (float)T / Constants::TicksPerSecond;
	}

	bool FCodec::EnemyPositionChanged(const FEnemySnap& Baseline, const FEnemySnap& Current)
	{
		return QuantPos((float)Baseline.Position.X) != QuantPos((float)Current.Position.X)
		    || QuantPos((float)Baseline.Position.Y) != QuantPos((float)Current.Position.Y);
	}

	// ---------- ClientHello ----------

	void FCodec::WriteClientHello(TArray<uint8>& Out, uint8 Capabilities)
	{
		FWireWriter W(Out);
		W.U32((uint32)Constants::ProtocolVersion);
		W.U8(Capabilities);
	}

	bool FCodec::ReadClientHello(FWireReader& R, uint32& OutVersion, uint8& OutCapabilities)
	{
		if (!R.Has(5)) return false;
		OutVersion = R.U32();
		OutCapabilities = R.U8();
		return true;
	}

	// ---------- ServerWelcome ----------

	void FCodec::WriteServerWelcome(TArray<uint8>& Out, uint8 PlayerId, uint32 ServerTick)
	{
		FWireWriter W(Out);
		W.U32((uint32)Constants::ProtocolVersion);
		W.U8(PlayerId);
		W.U32(ServerTick);
		W.U16((uint16)Constants::SimTickMs);
		W.U16((uint16)Constants::SnapshotEveryNTicks);
	}

	bool FCodec::ReadServerWelcome(FWireReader& R, FServerWelcome& Out)
	{
		if (!R.Has(ServerWelcomePayloadBytes)) return false;
		Out.ProtocolVersion     = R.U32();
		Out.PlayerId            = R.U8();
		Out.ServerTick          = R.U32();
		Out.SimTickMs           = R.U16();
		Out.SnapshotEveryNTicks = R.U16();
		return true;
	}

	// ---------- Input batch ----------

	static void WriteOneInput(FWireWriter& W, const FInputCmd& Cmd)
	{
		W.U32(Cmd.Tick);
		W.U32(Cmd.LastAckedServerTick);
		W.I16(FCodec::QuantPos((float)Cmd.Move.X));
		W.I16(FCodec::QuantPos((float)Cmd.Move.Y));
		W.I16(FCodec::QuantAngle((float)Cmd.Aim.X, (float)Cmd.Aim.Y));
		W.U8((uint8)Cmd.Flags);
	}

	static bool ReadOneInput(FWireReader& R, FInputCmd& Out)
	{
		if (!R.Has(InputCmdWireBytes)) return false;
		Out.Tick                = R.U32();
		Out.LastAckedServerTick = R.U32();
		int16 MxQ  = R.I16();
		int16 MyQ  = R.I16();
		int16 AimQ = R.I16();
		uint8 Flg  = R.U8();
		Out.Move = FVector2D(FCodec::DequantPos(MxQ), FCodec::DequantPos(MyQ));
		float DX, DY;
		FCodec::DequantAngle(AimQ, DX, DY);
		Out.Aim   = FVector2D(DX, DY);
		Out.Flags = (EInputFlags)Flg;
		return true;
	}

	void FCodec::WriteInputBatch(TArray<uint8>& Out, const FInputCmd* Cmds, int32 Count)
	{
		check(Count >= 1 && Count <= MaxInputBatch);
		FWireWriter W(Out);
		W.U8((uint8)Count);
		for (int32 i = 0; i < Count; ++i) WriteOneInput(W, Cmds[i]);
	}

	int32 FCodec::ReadInputBatch(FWireReader& R, FInputCmd OutCmds[MaxInputBatch])
	{
		if (!R.Has(1)) return -1;
		uint8 Count = R.U8();
		if (Count < 1 || Count > MaxInputBatch) return -1;
		for (uint8 i = 0; i < Count; ++i)
		{
			if (!ReadOneInput(R, OutCmds[i])) return -1;
		}
		return Count;
	}

	// ---------- Snapshot header ----------

	void FCodec::WriteSnapshotHeader(TArray<uint8>& Out, const FSnapshot& Snap)
	{
		FWireWriter W(Out);
		W.U8((uint8)Snap.Kind);
		W.U32(Snap.Tick);
		W.U32(Snap.BaselineTick);
		W.U32(Snap.LastProcessedInputTick);
		W.U16(Snap.Round);
		W.F32(Snap.RoundTimer);
		W.F32(Snap.InterRoundTimer);
		W.U8((uint8)Snap.Phase);
	}

	bool FCodec::ReadSnapshotHeader(FWireReader& R, FSnapshot& OutHeader)
	{
		if (!R.Has(SnapshotHeaderBytes)) return false;
		uint8 KindByte = R.U8();
		if (KindByte > 1) return false;
		OutHeader.Kind                    = (ESnapshotKind)KindByte;
		OutHeader.Tick                    = R.U32();
		OutHeader.BaselineTick            = R.U32();
		OutHeader.LastProcessedInputTick  = R.U32();
		OutHeader.Round                   = R.U16();
		OutHeader.RoundTimer              = R.F32();
		OutHeader.InterRoundTimer         = R.F32();
		uint8 PhaseByte = R.U8();
		if (PhaseByte > (uint8)ERoundPhase::Defeat) return false;
		OutHeader.Phase                   = (ERoundPhase)PhaseByte;
		return true;
	}

	// ---------- Per-entity writers/readers ----------

	static void WritePlayerSnap(FWireWriter& W, const FPlayerSnap& P)
	{
		W.U8(P.Id);
		W.I16(FCodec::QuantPos((float)P.Position.X));
		W.I16(FCodec::QuantPos((float)P.Position.Y));
		W.I16(FCodec::QuantAngle((float)P.Aim.X, (float)P.Aim.Y));
		W.U8(P.bAlive ? 1 : 0);
		W.U16(FCodec::QuantDisable(P.DisableTimer));
	}

	static bool ReadPlayerSnap(FWireReader& R, FPlayerSnap& Out)
	{
		if (!R.Has(PlayerSnapFullBytes)) return false;
		Out.Id            = R.U8();
		int16 Px          = R.I16();
		int16 Py          = R.I16();
		int16 Aim         = R.I16();
		uint8 Alive       = R.U8();
		uint16 Disable    = R.U16();
		Out.Position      = FVector2D(FCodec::DequantPos(Px), FCodec::DequantPos(Py));
		float DX, DY;
		FCodec::DequantAngle(Aim, DX, DY);
		Out.Aim           = FVector2D(DX, DY);
		Out.bAlive        = Alive != 0;
		Out.DisableTimer  = FCodec::DequantDisable(Disable);
		return true;
	}

	static void WriteEnemySnap(FWireWriter& W, const FEnemySnap& E)
	{
		W.U16(E.Id);
		W.I16(FCodec::QuantPos((float)E.Position.X));
		W.I16(FCodec::QuantPos((float)E.Position.Y));
	}

	static bool ReadEnemySnap(FWireReader& R, FEnemySnap& Out)
	{
		if (!R.Has(EnemySnapFullBytes)) return false;
		Out.Id        = R.U16();
		int16 Px      = R.I16();
		int16 Py      = R.I16();
		Out.Position  = FVector2D(FCodec::DequantPos(Px), FCodec::DequantPos(Py));
		return true;
	}

	static void WriteFire(FWireWriter& W, const FFireEvent& F)
	{
		W.U32(F.Tick);
		W.U8(F.ShooterId);
		W.I16(FCodec::QuantPos((float)F.Origin.X));
		W.I16(FCodec::QuantPos((float)F.Origin.Y));
		W.I16(FCodec::QuantAngle((float)F.Direction.X, (float)F.Direction.Y));
		W.I16(FCodec::QuantPos(F.Distance));
		W.U8(F.KillCount);
	}

	static bool ReadFire(FWireReader& R, FFireEvent& Out)
	{
		if (!R.Has(FireEventBytes)) return false;
		Out.Tick       = R.U32();
		Out.ShooterId  = R.U8();
		int16 Ox       = R.I16();
		int16 Oy       = R.I16();
		int16 Ang      = R.I16();
		int16 Dist     = R.I16();
		Out.KillCount  = R.U8();
		Out.Origin     = FVector2D(FCodec::DequantPos(Ox), FCodec::DequantPos(Oy));
		float DX, DY;
		FCodec::DequantAngle(Ang, DX, DY);
		Out.Direction  = FVector2D(DX, DY);
		Out.Distance   = FCodec::DequantPos(Dist);
		return true;
	}

	// ---------- Full snapshot body ----------

	void FCodec::WriteFullSnapshotBody(
		TArray<uint8>& Out,
		TArrayView<const FPlayerSnap> Players,
		TArrayView<const FEnemySnap>  Enemies,
		uint32 EnemyTotalInWorld,
		TArrayView<const FFireEvent>  Fires)
	{
		FWireWriter W(Out);
		const int32 PCount = FMath::Min(Players.Num(), (int32)MAX_uint8);
		W.U8((uint8)PCount);
		for (int32 i = 0; i < PCount; ++i) WritePlayerSnap(W, Players[i]);

		const int32 ECount = FMath::Min(Enemies.Num(), (int32)MAX_uint16);
		W.U16((uint16)ECount);
		W.U32(EnemyTotalInWorld);
		for (int32 i = 0; i < ECount; ++i) WriteEnemySnap(W, Enemies[i]);

		const int32 FCount = FMath::Min(Fires.Num(), 16);
		W.U8((uint8)FCount);
		for (int32 i = 0; i < FCount; ++i) WriteFire(W, Fires[i]);
	}

	bool FCodec::ReadFullSnapshotBody(FWireReader& R, FSnapshot& InOut)
	{
		if (!R.Has(1)) return false;
		uint8 PCount = R.U8();
		if (PCount > Constants::MaxPlayers) return false;
		for (uint8 i = 0; i < PCount; ++i)
		{
			FPlayerSnap P;
			if (!ReadPlayerSnap(R, P)) return false;
			InOut.Players.Add(P);
		}

		if (!R.Has(2 + 4)) return false;
		uint16 ECount = R.U16();
		InOut.EnemyTotalInWorld = R.U32();
		for (uint16 i = 0; i < ECount; ++i)
		{
			FEnemySnap E;
			if (!ReadEnemySnap(R, E)) return false;
			InOut.Enemies.Add(E);
		}

		if (!R.Has(1)) return false;
		uint8 FCount = R.U8();
		if (FCount > 16) return false;
		for (uint8 i = 0; i < FCount; ++i)
		{
			FFireEvent F;
			if (!ReadFire(R, F)) return false;
			InOut.RecentFireEvents.Add(F);
		}
		return true;
	}

	// ---------- Delta snapshot body ----------

	void FCodec::WriteDeltaSnapshotBody(
		TArray<uint8>& Out,
		TArrayView<const FPlayerSnap>      Players,
		TArrayView<const FEnemyDeltaEntry> Changed,
		TArrayView<const uint16>           Removed,
		TArrayView<const FEnemySnap>       Added,
		uint32 EnemyTotalInWorld,
		TArrayView<const FFireEvent>       Fires)
	{
		FWireWriter W(Out);
		const int32 PCount = FMath::Min(Players.Num(), (int32)MAX_uint8);
		W.U8((uint8)PCount);
		for (int32 i = 0; i < PCount; ++i) WritePlayerSnap(W, Players[i]);

		const int32 CCount = FMath::Min(Changed.Num(), (int32)MAX_uint16);
		W.U16((uint16)CCount);
		for (int32 i = 0; i < CCount; ++i)
		{
			const FEnemyDeltaEntry& C = Changed[i];
			W.U16(C.Id);
			W.I16(QuantPos((float)C.Position.X));
			W.I16(QuantPos((float)C.Position.Y));
		}

		const int32 RCount = FMath::Min(Removed.Num(), (int32)MAX_uint16);
		W.U16((uint16)RCount);
		for (int32 i = 0; i < RCount; ++i) W.U16(Removed[i]);

		const int32 ACount = FMath::Min(Added.Num(), (int32)MAX_uint16);
		W.U16((uint16)ACount);
		for (int32 i = 0; i < ACount; ++i) WriteEnemySnap(W, Added[i]);

		W.U32(EnemyTotalInWorld);

		const int32 FCount = FMath::Min(Fires.Num(), 16);
		W.U8((uint8)FCount);
		for (int32 i = 0; i < FCount; ++i) WriteFire(W, Fires[i]);
	}

	bool FCodec::ApplyDeltaSnapshotBody(FWireReader& R, FSnapshot& InOut)
	{
		if (!R.Has(1)) return false;
		uint8 PCount = R.U8();
		if (PCount > Constants::MaxPlayers) return false;
		for (uint8 i = 0; i < PCount; ++i)
		{
			FPlayerSnap P;
			if (!ReadPlayerSnap(R, P)) return false;
			InOut.Players.Add(P);
		}

		if (!R.Has(2)) return false;
		uint16 ChangedCount = R.U16();
		for (uint16 i = 0; i < ChangedCount; ++i)
		{
			if (!R.Has(EnemyDeltaEntryBytes)) return false;
			uint16 Id  = R.U16();
			int16  Px  = R.I16();
			int16  Py  = R.I16();
			FVector2D NewPos(DequantPos(Px), DequantPos(Py));
			for (FEnemySnap& E : InOut.Enemies)
			{
				if (E.Id == Id) { E.Position = NewPos; break; }
			}
		}

		if (!R.Has(2)) return false;
		uint16 RemovedCount = R.U16();
		for (uint16 i = 0; i < RemovedCount; ++i)
		{
			if (!R.Has(2)) return false;
			uint16 Id = R.U16();
			for (int32 j = 0; j < InOut.Enemies.Num(); ++j)
			{
				if (InOut.Enemies[j].Id == Id) { InOut.Enemies.RemoveAt(j); break; }
			}
		}

		if (!R.Has(2)) return false;
		uint16 AddedCount = R.U16();
		for (uint16 i = 0; i < AddedCount; ++i)
		{
			FEnemySnap E;
			if (!ReadEnemySnap(R, E)) return false;
			InOut.Enemies.Add(E);
		}

		if (!R.Has(4)) return false;
		InOut.EnemyTotalInWorld = R.U32();

		if (!R.Has(1)) return false;
		uint8 FCount = R.U8();
		if (FCount > 16) return false;
		for (uint8 i = 0; i < FCount; ++i)
		{
			FFireEvent F;
			if (!ReadFire(R, F)) return false;
			InOut.RecentFireEvents.Add(F);
		}
		return true;
	}

	// ---------- Stream framing ----------

	void FCodec::FrameStreamMessage(TArray<uint8>& Out, uint8 MsgType, TArrayView<const uint8> Payload)
	{
		const int32 OldNum = Out.Num();
		Out.Reserve(OldNum + 4 + 1 + Payload.Num());
		FWireWriter W(Out);
		W.U32((uint32)Payload.Num());
		W.U8(MsgType);
		Out.Append(Payload.GetData(), Payload.Num());
	}
}
