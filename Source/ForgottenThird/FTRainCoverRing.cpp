// Rain under cover, the resident ring (path 3b, 11-25): what ships in place of the moving 60 m capture.
//
// The strip's tiles (Data/Look/rain_cover_tiles.txt, ftcore rainCoverTileLine) are baked once per content key into the
// cache (Saved/RainCover/<map>, or -RainCoverBakeDir=<dir>) as v2 tile files (ftcore RainCoverTileHeader): per tile a find
// draw from 400 m gives its highest occluder, then a draw from 1 m over that, LOD0, no cull distances and every
// world-position offset off (the wind frozen: plants at rest), packed to R16 over the tile's own range. A tile's key sums
// what its boxes hold (mesh package hash, materials and their WPO flag, cull and LOD settings, transforms), with the file
// version and the bake settings; a file whose key matches is kept.
//
// In play RT_RainCover is a 3 x 3 atlas of G16 tiles (ftcore rainCoverRingTexel) and CoverSlot0..8 say which tile each slot
// holds. The nine round the lead's tile are filled at set-up (synchronously, before CoverOn goes to 1); the sixteen round
// those are read into a 25-entry RAM pool off the game thread with unbuffered reads (4096-aligned), so a crossing uploads
// its 3 or 5 entering tiles from RAM in the tick it happens and sets their slot vectors with them. A tile whose read has not
// landed is late: its slot reads open (never waited on) until the read lands, then it is uploaded.
//
// Switches: -RainCoverRebake bakes every tile; -RainCoverBakeOnly bakes (or checks) and exits, never setting CoverOn;
// -RainCoverBakeSettle=<ticks> (600) before the key; -RainCoverRoute=B uploads through a staging texture and a copy
// (A, the default, writes the atlas directly). Dev only: -RainCoverRingForce=5|9 (every 5 m trigger reads that many baked
// tiles off disk and uploads them: the corner and jump cells), -RainCoverCompareDir=<dir> (each baked tile against the
// same tile there: the determinism check), -RainCoverAtlasCheck=<n> (the atlas read back after the fill and every n
// crossings, against the tile files through the Core mapping), -RainCoverHomes (the cache's raw and Oodle sizes),
// -RainCoverWitness[=<file>] (the 60 m draw at the plant tiles and listed tiles against the baked tiles; with
// -RainCoverTileLod0 -RainCoverTileNoCull the switched witness), -RainCoverKeyDump=<x,y>[;<x,y>...] (each component's key
// inputs for the named tiles logged at key time; nothing else changes), -RainCoverKeyCaptures=<n> (n - 1 idle captures more at
// the bake capture: the origin tiles' key with one capture and with several).

#include "FTRainCoverSubsystem.h"

#include "FTRainCoverCollection.h"
#include "ForgottenThird.h"

#include "Async/Async.h"
#include "Camera/CameraActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Rendering/NaniteResources.h"
#include "StaticMeshResources.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Hash/CityHash.h"
#include "IO/IoHash.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "RenderCommandFence.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#if !UE_BUILD_SHIPPING
#include "Misc/Compression.h"
#endif

#include <atomic>
#include <cmath>

CSV_DECLARE_CATEGORY_EXTERN(RainCover);

namespace FTRainRingDetail
{
constexpr float kRingFindTopM = 400.f; // over the strip's highest box (242 m)
constexpr float kRingFindFarM = 500.f; // ... down to -100 m
constexpr float kRingMarginM = 1.f;    // a tile's top over its highest occluder
constexpr float kRingTolM = 0.002f;    // the ruled height tolerance
constexpr int32 kRingBakePerTick = 32;
constexpr int32 kRingSwitchSettle = 60; // ticks after the bake switches go on
constexpr int32 kRingPool = 25;         // 5 x 5 round the lead's tile
constexpr int32 kRingAlign = 4096;      // unbuffered reads: offset, size and buffer sector-aligned

int32 RingCVarInt(const TCHAR* Name, int32 Fallback)
{
	IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
	return V ? V->GetInt() : Fallback;
}

void RingCVarSet(const TCHAR* Name, int32 Value)
{
	if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name))
	{
		V->Set(Value, ECVF_SetByCode);
	}
}

// A tile's payload off disk, unbuffered (the OS cache bypassed): the codes past the 4096-byte header.
bool RingReadPayload(const TCHAR* Path, uint16* Into, int32 Bytes, double& Ms)
{
	const double T0 = FPlatformTime::Seconds();
	TUniquePtr<IFileHandle> H(IPlatformFile::GetPlatformPhysical().OpenReadNoBuffering(Path));
	const bool bOk = H && H->Seek(ftcore::kRainCoverFileHeader) && H->Read(reinterpret_cast<uint8*>(Into), Bytes);
	Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
	return bOk;
}

bool RingReadHeader(const FString& Path, ftcore::RainCoverTileHeader& H)
{
	TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
	if (!Ar || Ar->TotalSize() < ftcore::kRainCoverFileHeader)
	{
		return false;
	}
	uint8 Buf[ftcore::kRainCoverFileHeader];
	Ar->Serialize(Buf, ftcore::kRainCoverFileHeader);
	return ftcore::rainCoverHeaderRead(Buf, ftcore::kRainCoverFileHeader, H) &&
		Ar->TotalSize() == int64(ftcore::kRainCoverFileHeader) + ftcore::rainCoverPayloadBytes(H);
}
} // namespace FTRainRingDetail

struct FFTRainRing
{
	enum EState : uint8 { Settle, Switch, Bake, Fill, Run, Done };

	struct FSaved
	{
		TWeakObjectPtr<UStaticMeshComponent> Comp;
		float LD = 0.f, Cached = 0.f, MinDraw = 0.f;
		int32 Forced = 0, IStart = 0, IEnd = 0;
		bool bIsm = false, bWpo = false;
	};
	struct FEntry
	{
		int32 Ti = -1; // tile index held (or being read); -1 free
		uint16* Buf = nullptr;
		bool bReading = false, bReady = false;
		TFuture<bool> Read;
		std::atomic<int64> Us{0};
		FRenderCommandFence Fence;
	};

	// The strip and its cache.
	TArray<ftcore::RainCoverTile> Tiles;
	TMap<FIntPoint, int32> Index;
	TArray<FString> Path;
	TArray<uint64> Key;
	TArray<ftcore::RainCoverTileHeader> Head;
	TArray<uint8> Have; // 1: the cache's file is there and its key matches
	TSet<int32> Plant;  // tiles a material with a world-position offset touches
	TArray<int32> ToBake;
	FString Dir, CompareDir;
	int32 Comps = 0, Insts = 0, Keyed = 0;

	EState State = Settle;
	int32 Ticks = 0, SettleTicks = 600, Wait = 0, BakeAt = 0;
	bool bRebake = false, bBakeOnly = false, bRouteB = false, bSwitched = false;

	// The bake's switches (LOD0, no cull, WPO off) and what they replaced.
	TArray<FSaved> Saved;
	int32 OldForceLod = -1, OldFoliageLod = -1, OldGpuLod = 1;
	TArray<float> Depth;
	TArray<FLinearColor> Pixels;
	TArray<uint16> Codes;
	TArray<uint8> File;
	int32 Baked = 0, Empty = 0, HeaderOnly = 0, KeySame = 0, KeyChanged = 0, KeyNew = 0, Same = 0, Differ = 0, NoCompare = 0;
	double BakeMs = 0.0, BakeT0 = 0.0;
	int64 BakeBytes = 0;

	// The ring: the lead's tile, what each slot holds (tile index, -1 none), slots waiting for a late read.
	ftcore::RainCoverTile Lead;
	int32 SlotTi[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
	int32 SlotLate[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
	FName SlotName[9];
	FEntry Pool[FTRainRingDetail::kRingPool];
	uint16* Full = nullptr; // a flat, full tile's codes (all 1)
	int32 PayloadBytes = 0;

	// Counts.
	int32 Crossings = 0, Uploaded = 0, Late = 0, LateFilled = 0, Reads = 0, ReadFails = 0, Evicts = 0, NoRoom = 0;
	double ReadMsSum = 0.0, ReadMsMax = 0.0, FillMs = 0.0;

	// Dev cells.
	int32 Force = 0;
	TArray<int32> Textured;
	int32 ForceNext = 0, ForceReady = -1, ForceLast = 1, ForceLate = 0, ForceBusy = 0, ForceHits = 0;
	uint16* ForceBuf[2] = {nullptr, nullptr};
	TFuture<int32> ForceRead[2];
	bool bForceReading[2] = {false, false};
	std::atomic<int64> ForceUs{0};
	FRenderCommandFence ForceFence[2];
	ftcore::RainCoverFrame Trigger;
	bool bTriggered = false;
	int32 AtlasCheckEvery = 0, AtlasCheckAt = -1;
	TSet<FIntPoint> KeyDump; // tiles whose key inputs are logged (empty: none)
	bool bWitness = false, bWitnessLod0 = false, bWitnessNoCull = false;
	TArray<int32> WitnessList, WitnessTiles;
	int32 WitnessStep = 0, WitnessAt = 0, WitnessWait = -1, WNoDraw = 0;
	int64 WFails = 0, WAgree = 0, WBand = 0, WBelow = 0, WHeight = 0, WGained = 0, WLost = 0, WTexels = 0, WNoTile = 0;
	float WMaxCm = 0.f;
	TArray<FVector> WFailAt; // the first failing texels (m): plan point and the occluder Z
	TMap<int32, int64> WFailTile;

	~FFTRainRing()
	{
		for (FEntry& E : Pool)
		{
			if (E.bReading)
			{
				E.Read.Wait();
			}
			FMemory::Free(E.Buf);
		}
		for (int32 B = 0; B < 2; ++B)
		{
			if (bForceReading[B])
			{
				ForceRead[B].Wait();
			}
			FMemory::Free(ForceBuf[B]);
		}
		FMemory::Free(Full);
	}

	int32 Find(ftcore::RainCoverTile T) const
	{
		const int32* I = Index.Find(FIntPoint(T.x, T.y));
		return I ? *I : -1;
	}
	bool HasPayload(int32 Ti) const { return Ti >= 0 && Head[Ti].res > 0; }
};

void UFTRainCoverSubsystem::RingEnd()
{
	if (!Ring)
	{
		return;
	}
	FlushRenderingCommands(); // the uploads read the pool's buffers
	const FFTRainRing& R = *Ring;
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover ring: %d crossings, %d tiles uploaded, %d late (filled when the read landed: %d), %d reads (mean %.2f ms, max %.2f ms off the game thread, %d failed), %d evictions, %d with no free entry; route %s."),
		R.Crossings, R.Uploaded, R.Late, R.LateFilled, R.Reads, R.Reads ? R.ReadMsSum / R.Reads : 0.0, R.ReadMsMax, R.ReadFails, R.Evicts,
		R.NoRoom, R.bRouteB ? TEXT("B (staging and copy)") : TEXT("A (direct)"));
	if (R.Force > 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover ring force: %d tiles a hit: %d hits, %d late (read not back, skipped), %d ticks a buffer waited on its fence."),
			R.Force, R.ForceHits, R.ForceLate, R.ForceBusy);
	}
	Ring.Reset();
}

bool UFTRainCoverSubsystem::RingBegin(UWorld& World)
{
	using namespace FTRainRingDetail;
	const TCHAR* Sw = FTLookSwitches();
	TSharedPtr<FFTRainRing> R = MakeShared<FFTRainRing>();
	const int32 Res = Spec.resolution, N = Res * Res;
	R->PayloadBytes = N * int32(sizeof(uint16));

	// The tile list.
	const FString ListFile = FPaths::ProjectDir() / TEXT("Data/Look/rain_cover_tiles.txt");
	TArray<FString> Lines;
	FFileHelper::LoadFileToStringArray(Lines, *ListFile);
	for (const FString& Line : Lines)
	{
		ftcore::RainCoverTile T;
		if (ftcore::rainCoverTileLine(TCHAR_TO_UTF8(*Line), T) && !R->Index.Contains(FIntPoint(T.x, T.y)))
		{
			R->Index.Add(FIntPoint(T.x, T.y), R->Tiles.Num());
			R->Tiles.Add(T);
		}
	}
	if (R->Tiles.Num() == 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("RainCover: off, no tiles in %s."), *ListFile);
		return false;
	}

	// The cache: one path, resolved the same way in -game, the editor and a packaged build.
	FString BakeDir;
	R->Dir = FParse::Value(Sw, TEXT("RainCoverBakeDir="), BakeDir) && !BakeDir.IsEmpty()
		? BakeDir
		: FPaths::ProjectSavedDir() / TEXT("RainCover") / World.GetMapName();
	R->Dir = FPaths::ConvertRelativePathToFull(R->Dir);
	IFileManager::Get().MakeDirectory(*R->Dir, true);
	const int32 NT = R->Tiles.Num();
	R->Path.SetNum(NT);
	for (int32 I = 0; I < NT; ++I)
	{
		R->Path[I] = R->Dir / FString::Printf(TEXT("tile_%d_%d.rc2"), R->Tiles[I].x, R->Tiles[I].y);
	}
	R->Key.Init(0, NT);
	R->Head.SetNum(NT);
	R->Have.Init(0, NT);
	R->bRebake = FParse::Param(Sw, TEXT("RainCoverRebake"));
	R->bBakeOnly = FParse::Param(Sw, TEXT("RainCoverBakeOnly"));
	FParse::Value(Sw, TEXT("RainCoverBakeSettle="), R->SettleTicks);
	R->SettleTicks = FMath::Max(R->SettleTicks, 2);
	FString Route;
	R->bRouteB = FParse::Value(Sw, TEXT("RainCoverRoute="), Route) && Route.Equals(TEXT("B"), ESearchCase::IgnoreCase);
	for (int32 S = 0; S < 9; ++S)
	{
		R->SlotName[S] = FName(UTF8_TO_TCHAR(ftcore::rainCoverSlotParam(S)));
	}
#if !UE_BUILD_SHIPPING
	FParse::Value(Sw, TEXT("RainCoverRingForce="), R->Force);
	R->Force = R->Force >= 9 ? 9 : (R->Force > 0 ? 5 : 0);
	FParse::Value(Sw, TEXT("RainCoverCompareDir="), R->CompareDir);
	FParse::Value(Sw, TEXT("RainCoverAtlasCheck="), R->AtlasCheckEvery);
	FString DumpList;
	if (FParse::Value(Sw, TEXT("RainCoverKeyDump="), DumpList, false))
	{
		TArray<FString> Pairs;
		DumpList.ParseIntoArray(Pairs, TEXT(";"));
		for (const FString& P : Pairs)
		{
			FString Xs, Ys;
			if (P.Split(TEXT(","), &Xs, &Ys))
			{
				R->KeyDump.Add(FIntPoint(FCString::Atoi(*Xs), FCString::Atoi(*Ys)));
			}
		}
	}
	FString WitnessFile;
	R->bWitness = FParse::Param(Sw, TEXT("RainCoverWitness")) || FParse::Value(Sw, TEXT("RainCoverWitness="), WitnessFile);
	if (!WitnessFile.IsEmpty())
	{
		TArray<FString> WLines;
		FFileHelper::LoadFileToStringArray(WLines, *WitnessFile);
		for (const FString& Line : WLines)
		{
			ftcore::RainCoverTile T;
			const int32 Ti = ftcore::rainCoverTileLine(TCHAR_TO_UTF8(*Line), T) ? R->Find(T) : -1;
			if (Ti >= 0)
			{
				R->WitnessList.AddUnique(Ti);
			}
		}
	}
#endif

	// The atlas: RT_RainCover re-made at run time as 3 x 3 G16 tiles, cleared to code 0 (no occluder).
	const int32 Side = ftcore::rainCoverAtlasSide(Spec);
	Target->ClearColor = FLinearColor::Black;
	Target->InitCustomFormat(Side, Side, PF_G16, true);
	Target->UpdateResourceImmediate(true);
	if (R->bRouteB)
	{
		RingStaging = UTexture2D::CreateTransient(Res, Res, PF_G16);
		if (RingStaging)
		{
			RingStaging->SRGB = false;
			RingStaging->UpdateResource();
		}
		R->bRouteB = RingStaging != nullptr;
	}

	// The bake capture: one tile, R32F depth, the shipped trimmed set.
	RingTarget = NewObject<UTextureRenderTarget2D>(this);
	RingTarget->ClearColor = FLinearColor(1e7f, 1e7f, 1e7f, 1.f);
	RingTarget->InitCustomFormat(Res, Res, PF_R32_FLOAT, true);
	RingTarget->UpdateResourceImmediate(true);
	FActorSpawnParameters Params;
	Params.ObjectFlags = RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	RingCapture = World.SpawnActor<ASceneCapture2D>(FVector::ZeroVector, FRotator(-90.f, 0.f, 0.f), Params);
	USceneCaptureComponent2D* C = RingCapture ? RingCapture->GetCaptureComponent2D() : nullptr;
	if (!C)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("RainCover: off, couldn't spawn the bake capture."));
		return false;
	}
	FTRainCoverCaptureSettings(*C, RingTarget, Spec);
#if !UE_BUILD_SHIPPING
	// Dev only, -RainCoverKeyCaptures=<n>: n - 1 more idle captures where the bake capture is (no target, never capturing), so
	// the key of the tiles round the origin can be checked with one capture and with several.
	int32 KeyCaptures = 1;
	FParse::Value(FTLookSwitches(), TEXT("RainCoverKeyCaptures="), KeyCaptures);
	for (int32 K = 1; K < FMath::Min(KeyCaptures, 8); ++K)
	{
		if (ASceneCapture2D* Extra = World.SpawnActor<ASceneCapture2D>(FVector::ZeroVector, FRotator(-90.f, 0.f, 0.f), Params))
		{
			Extra->GetCaptureComponent2D()->bCaptureEveryFrame = false;
			Extra->GetCaptureComponent2D()->bCaptureOnMovement = false;
		}
	}
#endif

	// The RAM pool: 25 tiles' codes, sector-aligned; and a flat, full tile's.
	for (FFTRainRing::FEntry& E : R->Pool)
	{
		E.Buf = static_cast<uint16*>(FMemory::Malloc(R->PayloadBytes, kRingAlign));
	}
	R->Full = static_cast<uint16*>(FMemory::Malloc(R->PayloadBytes, kRingAlign));
	for (int32 I = 0; I < N; ++I)
	{
		R->Full[I] = 1;
	}
	if (R->Force > 0)
	{
		for (int32 B = 0; B < 2; ++B)
		{
			R->ForceBuf[B] = static_cast<uint16*>(FMemory::Malloc(SIZE_T(R->PayloadBytes) * R->Force, kRingAlign));
		}
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover ring: %d tiles from %s; cache %s; atlas %d x %d G16; route %s; key after %d ticks%s%s; RAM pool %d x %.2f MiB."),
		NT, *ListFile, *R->Dir, Side, Side, R->bRouteB ? TEXT("B (staging and copy)") : TEXT("A (direct)"), R->SettleTicks,
		R->bRebake ? TEXT(", rebake all") : TEXT(""), R->bBakeOnly ? TEXT(", bake only (the cover stays off)") : TEXT(""), kRingPool,
		double(R->PayloadBytes) / (1024.0 * 1024.0));
	Ring = R;
	return true;
}

namespace FTRainRingDetail
{
// The content key per tile: per component the mesh package's saved hash (editor builds; a cooked build, its name), the
// materials and their WPO flag, the cull and LOD settings, the transform; per instance its transform. Summed order-free
// into the tiles its box touches, then sealed with the bake's own settings. Also counts and the tiles a WPO material touches.
void RingKeyTiles(UWorld& World, const ftcore::RainCoverSpec& Spec, FFTRainRing& R)
{
	TMap<FIntPoint, uint64> Sum;
	R.Comps = R.Insts = 0;
	R.Plant.Reset();
	const auto Bucket = [&](const FBox& Bx, uint64 H, bool bWpo)
	{
		const ftcore::RainCoverTile A = ftcore::rainCoverTileAt(Spec, float(Bx.Min.X) * 0.01f, float(Bx.Min.Y) * 0.01f);
		const ftcore::RainCoverTile Z = ftcore::rainCoverTileAt(Spec, float(Bx.Max.X) * 0.01f, float(Bx.Max.Y) * 0.01f);
		for (int32 X = A.x; X <= Z.x && X - A.x < 64; ++X)
		{
			for (int32 Y = A.y; Y <= Z.y && Y - A.y < 64; ++Y)
			{
				Sum.FindOrAdd(FIntPoint(X, Y)) += H;
				if (bWpo)
				{
					const int32* Ti = R.Index.Find(FIntPoint(X, Y));
					if (Ti)
					{
						R.Plant.Add(*Ti);
					}
				}
			}
		}
	};
	const auto HashT = [](const FTransform& X, uint64 Seed)
	{
		const FVector L = X.GetLocation(), S = X.GetScale3D();
		const FQuat Q = X.GetRotation();
		const double V[10] = {L.X, L.Y, L.Z, S.X, S.Y, S.Z, Q.X, Q.Y, Q.Z, Q.W};
		return CityHash64WithSeed(reinterpret_cast<const char*>(V), uint32(sizeof(V)), Seed);
	};
	// Dev only (-RainCoverKeyDump): the named tile a box touches, if any. Read only; the key is the same with or without it.
	const auto DumpHit = [&](const FBox& Bx, FIntPoint& Hit)
	{
		if (R.KeyDump.Num() == 0)
		{
			return false;
		}
		const ftcore::RainCoverTile A = ftcore::rainCoverTileAt(Spec, float(Bx.Min.X) * 0.01f, float(Bx.Min.Y) * 0.01f);
		const ftcore::RainCoverTile Z = ftcore::rainCoverTileAt(Spec, float(Bx.Max.X) * 0.01f, float(Bx.Max.Y) * 0.01f);
		for (const FIntPoint& T : R.KeyDump)
		{
			if (T.X >= A.x && T.X <= Z.x && T.X - A.x < 64 && T.Y >= A.y && T.Y <= Z.y && T.Y - A.y < 64)
			{
				Hit = T;
				return true;
			}
		}
		return false;
	};
	const auto DumpT = [](const FTransform& X)
	{
		const FVector L = X.GetLocation(), S = X.GetScale3D();
		const FQuat Q = X.GetRotation();
		return FString::Printf(TEXT("loc %.6f %.6f %.6f scale %.6f %.6f %.6f quat %.9f %.9f %.9f %.9f"), L.X, L.Y, L.Z, S.X, S.Y, S.Z, Q.X, Q.Y, Q.Z, Q.W);
	};
	int32 SkipBy[5] = {0, 0, 0, 0, 0}, NoProxy = 0, RunMat = 0;
	int32 Census[2][2] = {{0, 0}, {0, 0}}; // [kept, left out][saved, run-time]
	TMap<FString, int32> RunTimeBy;         // run-time components by owner class ("left out " first when the capture can't draw it)
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		UStaticMeshComponent* C = *It;
		const UStaticMesh* M = C->GetStaticMesh();
		if (C->GetWorld() != &World || !C->IsRegistered() || !M)
		{
			continue;
		}
		// Only what the bake's capture can draw: the find and the depth draw are the same capture component (RingDraw), so one
		// test covers both. FPrimitiveSceneProxy::IsShown (PrimitiveSceneProxy.cpp:1515) draws in a game view only with DrawInGame,
		// which is the proxy desc's IsVisible (:318; USceneComponent::IsVisible is false when bHiddenInGame is set or the visible
		// flag is off) and not the owner actor hidden (:508); it drops one hidden in scene captures (:1596) and one only its
		// owner sees (the capture's view actor owns no world mesh). So the editor-only capture and camera proxy meshes leave
		// the key as they leave both draws: both are bHiddenInGame (SceneCaptureComponent.cpp:276, CameraComponent.cpp:168).
		const AActor* Owner = C->GetOwner();
		const int32 Skip = !C->IsVisible() ? 1 : (Owner && Owner->IsHidden()) ? 2 : C->bHiddenInSceneCapture ? 3 : C->bOnlyOwnerSee ? 4 : 0;
		// The census (once a bake, not per frame): every component saved in the map or made at run time, kept or left out.
		const bool bSavedComp = C->HasAnyFlags(RF_WasLoaded);
		++Census[Skip ? 1 : 0][bSavedComp ? 0 : 1];
		if (!bSavedComp)
		{
			RunTimeBy.FindOrAdd(FString::Printf(TEXT("%s%s"), Skip ? TEXT("left out ") : TEXT(""),
				Owner ? *Owner->GetClass()->GetName() : TEXT("no owner"))) += 1;
		}
		if (Skip)
		{
			++SkipBy[Skip];
			FIntPoint SkipHit(0, 0);
			if (DumpHit(C->Bounds.GetBox(), SkipHit))
			{
				UE_LOG(LogForgottenThird, Display, TEXT("RainCover keydump (%d,%d) SKIPPED %s %s | owner %s %s | saved %d | mesh %s | %s"), SkipHit.X,
					SkipHit.Y, Skip == 1 ? TEXT("not visible") : Skip == 2 ? TEXT("owner hidden") : Skip == 3 ? TEXT("hidden in captures") : TEXT("owner only"),
					*C->GetPathName(), Owner ? *Owner->GetClass()->GetName() : TEXT("-"), Owner ? *Owner->GetName() : TEXT("-"),
					C->HasAnyFlags(RF_WasLoaded) ? 1 : 0, *M->GetPathName(), *DumpT(C->GetComponentTransform()));
			}
			continue;
		}
		++R.Comps;
		NoProxy += C->SceneProxy ? 0 : 1;
		// The mesh by what an editor -game run and a cooked build both hold (a package's saved hash is editor-only data): its
		// package name, LOD0's vertex and triangle counts and the render bounds, and for a Nanite mesh its input counts and
		// bounds. An edit that moves vertices inside the bounds and keeps every count is not seen: rebake by hand after one.
		const FString PN = M->GetPackage()->GetName();
		uint64 H = CityHash64(reinterpret_cast<const char*>(*PN), uint32(PN.Len() * sizeof(TCHAR)));
		{
			const FBoxSphereBounds B = M->GetBounds();
			const double Geo[8] = {double(M->GetNumVertices(0)), double(M->GetNumTriangles(0)), B.Origin.X, B.Origin.Y, B.Origin.Z, B.BoxExtent.X,
				B.BoxExtent.Y, B.BoxExtent.Z};
			H = CityHash64WithSeed(reinterpret_cast<const char*>(Geo), uint32(sizeof(Geo)), H);
			const FStaticMeshRenderData* RD = M->GetRenderData();
			if (M->HasValidNaniteData() && RD && RD->NaniteResourcesPtr.IsValid())
			{
				const Nanite::FResources& NR = *RD->NaniteResourcesPtr;
				const double Nan[8] = {double(NR.NumInputTriangles), double(NR.NumInputVertices), NR.MeshBounds.Origin.X, NR.MeshBounds.Origin.Y,
					NR.MeshBounds.Origin.Z, NR.MeshBounds.BoxExtent.X, NR.MeshBounds.BoxExtent.Y, NR.MeshBounds.BoxExtent.Z};
				H = CityHash64WithSeed(reinterpret_cast<const char*>(Nan), uint32(sizeof(Nan)), H);
			}
		}
		const uint64 HMesh = H;
		// Materials by path. The world-position offset flag feeds the plant tiles only, not the key: the bake turns the offset
		// off on every component (the Switch state), so a material's offset can't change a payload, and its path is hashed.
		bool bWpo = false, bRunMat = false;
		FString DumpMats;
		for (int32 S = 0; S < C->GetNumMaterials(); ++S)
		{
			const UMaterialInterface* Mi = C->GetMaterial(S);
			const FString MN = Mi ? Mi->GetPathName() : FString();
			const bool bW = Mi && Mi->IsUsingWorldPositionOffset_Concurrent(GMaxRHIFeatureLevel);
			bWpo = bWpo || bW;
			bRunMat = bRunMat || (Mi && (Mi->HasAnyFlags(RF_Transient) || !Mi->HasAnyFlags(RF_WasLoaded | RF_Public)));
			H = CityHash64WithSeed(reinterpret_cast<const char*>(*MN), uint32(MN.Len() * sizeof(TCHAR)), H);
			if (R.KeyDump.Num() > 0)
			{
				DumpMats += FString::Printf(TEXT("%s[%d] %s wpo %d"), S ? TEXT("; ") : TEXT(""), S, *MN, bW ? 1 : 0);
			}
		}
		RunMat += bRunMat ? 1 : 0;
		const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C);
		const int32 Ints[6] = {C->ForcedLodModel, C->MinLOD, I ? I->InstanceStartCullDistance : -1, I ? I->InstanceEndCullDistance : -1,
			int32(C->LDMaxDrawDistance), int32(C->MinDrawDistance)};
		H = CityHash64WithSeed(reinterpret_cast<const char*>(Ints), uint32(sizeof(Ints)), H);
		H = HashT(C->GetComponentTransform(), H);
		FIntPoint Hit(0, 0);
		int32 DumpN = 0;
		uint64 DumpSum = 0;
		if (I)
		{
			const FBox MB = M->GetBoundingBox();
			for (int32 K = 0; K < I->GetInstanceCount(); ++K)
			{
				FTransform X;
				if (I->GetInstanceTransform(K, X, true))
				{
					++R.Insts;
					const FBox IB = MB.TransformBy(X);
					const uint64 IH = HashT(X, H);
					Bucket(IB, IH, bWpo);
					if (DumpHit(IB, Hit))
					{
						++DumpN;
						DumpSum += IH;
					}
				}
			}
		}
		else
		{
			Bucket(C->Bounds.GetBox(), H, bWpo);
			if (DumpHit(C->Bounds.GetBox(), Hit))
			{
				DumpN = 1;
				DumpSum = H;
			}
		}
		if (DumpN > 0)
		{
			const AActor* Own = Owner;
			UE_LOG(LogForgottenThird, Display,
				TEXT("RainCover keydump (%d,%d) %s | owner %s %s | saved %d | mobility %d sim %d | mesh %s hash %016llx | mats %s | ints %d %d %d %d %d %d | %s | inst %d | comp %016llx | sum %016llx"),
				Hit.X, Hit.Y, *C->GetPathName(), Own ? *Own->GetClass()->GetName() : TEXT("-"), Own ? *Own->GetName() : TEXT("-"),
				C->HasAnyFlags(RF_WasLoaded) ? 1 : 0, int32(C->Mobility), C->IsSimulatingPhysics() ? 1 : 0, *M->GetPathName(), HMesh, *DumpMats, Ints[0], Ints[1], Ints[2],
				Ints[3], Ints[4], Ints[5], *DumpT(C->GetComponentTransform()), I ? DumpN : -1, H, DumpSum);
		}
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover key filter: %d components the capture can't draw left out (%d not visible, %d owner hidden, %d hidden in captures, %d owner only); of the %d kept, %d with no scene proxy, %d with a run-time material."),
		SkipBy[1] + SkipBy[2] + SkipBy[3] + SkipBy[4], SkipBy[1], SkipBy[2], SkipBy[3], SkipBy[4], R.Comps, NoProxy, RunMat);
	{
		RunTimeBy.ValueSort([](int32 A, int32 B) { return A > B; });
		FString By;
		for (const TPair<FString, int32>& P : RunTimeBy)
		{
			By += FString::Printf(TEXT("%s%s %d"), By.IsEmpty() ? TEXT("") : TEXT(", "), *P.Key, P.Value);
		}
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover key census: %d components (%d saved in the map, %d made at run time); kept %d saved + %d run-time, left out %d saved + %d run-time; run-time by owner class: %s."),
			Census[0][0] + Census[0][1] + Census[1][0] + Census[1][1], Census[0][0] + Census[1][0], Census[0][1] + Census[1][1], Census[0][0],
			Census[0][1], Census[1][0], Census[1][1], By.IsEmpty() ? TEXT("none") : *By);
	}
	// The bake's own settings: file version, the spec, the find and margin, LOD0, no cull and the wind frozen.
	const float Settings[11] = {float(ftcore::kRainCoverFileVersion), Spec.sizeM, float(Spec.resolution), Spec.depthM, kRingFindTopM, kRingFindFarM,
		kRingMarginM, kRingTolM, 1.f /*LOD0*/, 1.f /*no cull*/, 1.f /*WPO off*/};
	const uint64 Seal = CityHash64(reinterpret_cast<const char*>(Settings), uint32(sizeof(Settings)));
	R.Keyed = 0;
	for (int32 Ti = 0; Ti < R.Tiles.Num(); ++Ti)
	{
		const uint64* S = Sum.Find(FIntPoint(R.Tiles[Ti].x, R.Tiles[Ti].y));
		R.Keyed += S ? 1 : 0;
		const uint64 V = S ? *S : 0ull;
		R.Key[Ti] = CityHash64WithSeed(reinterpret_cast<const char*>(&V), uint32(sizeof(V)), Seal);
		if (R.KeyDump.Contains(FIntPoint(R.Tiles[Ti].x, R.Tiles[Ti].y)))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover keydump (%d,%d) tile sum %016llx key %016llx"), R.Tiles[Ti].x, R.Tiles[Ti].y, V,
				R.Key[Ti]);
		}
	}
}

// One tile-sized draw straight down from At, far plane FarM: depth m per texel (image order).
bool RingDraw(ASceneCapture2D& Cap, UTextureRenderTarget2D& Into, const ftcore::RainCoverFrame& At, float FarM, TArray<FLinearColor>& Pixels,
	TArray<float>& DepthM)
{
	USceneCaptureComponent2D* C = Cap.GetCaptureComponent2D();
	Cap.SetActorLocation(FVector(At.cx, At.cy, At.topZ) * 100.0);
	C->MaxViewDistanceOverride = FarM * 100.f;
	C->CaptureScene();
	FTextureRenderTargetResource* Res = Into.GameThread_GetRenderTargetResource();
	Pixels.Reset();
	if (!Res || !Res->ReadLinearColorPixels(Pixels) || Pixels.Num() != Into.SizeX * Into.SizeY)
	{
		return false;
	}
	DepthM.SetNumUninitialized(Pixels.Num(), EAllowShrinking::No);
	for (int32 I = 0; I < Pixels.Num(); ++I)
	{
		DepthM[I] = Pixels[I].R * 0.01f;
	}
	return true;
}
} // namespace FTRainRingDetail

namespace
{
// The bake's switches on (world-position offset off: the wind frozen; LOD0 and no cull distances as asked) or back as they
// were. The bake takes all three; the witness the freeze alone (the shipped LOD and cull), or all three when switched.
void RingSwitches(UWorld& World, FFTRainRing& R, bool bOn, bool bLod0 = true, bool bNoCull = true)
{
	bLod0 = bOn && bLod0;
	bNoCull = bOn && bNoCull;
	using namespace FTRainRingDetail;
	if (bOn && R.Saved.Num() == 0)
	{
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* C = *It;
			if (C->GetWorld() != &World || !C->IsRegistered() || !C->GetStaticMesh())
			{
				continue;
			}
			FFTRainRing::FSaved S;
			S.Comp = C;
			S.LD = C->LDMaxDrawDistance;
			S.Cached = C->CachedMaxDrawDistance;
			S.MinDraw = C->MinDrawDistance;
			S.Forced = C->ForcedLodModel;
			S.bWpo = C->bEvaluateWorldPositionOffset;
			if (const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C))
			{
				S.bIsm = true;
				S.IStart = I->InstanceStartCullDistance;
				S.IEnd = I->InstanceEndCullDistance;
			}
			R.Saved.Add(S);
		}
		R.OldForceLod = RingCVarInt(TEXT("r.ForceLOD"), -1);
		R.OldFoliageLod = RingCVarInt(TEXT("foliage.ForceLOD"), -1);
		R.OldGpuLod = RingCVarInt(TEXT("r.InstancedStaticMeshes.GpuLod"), 1);
	}
	RingCVarSet(TEXT("r.ForceLOD"), bLod0 ? 0 : R.OldForceLod);
	RingCVarSet(TEXT("foliage.ForceLOD"), bLod0 ? 0 : R.OldFoliageLod);
	RingCVarSet(TEXT("r.InstancedStaticMeshes.GpuLod"), bLod0 ? 0 : R.OldGpuLod);
	int32 Frozen = 0;
	for (const FFTRainRing::FSaved& S : R.Saved)
	{
		UStaticMeshComponent* C = S.Comp.Get();
		if (!C)
		{
			continue;
		}
		C->SetForcedLodModel(bLod0 ? 1 : S.Forced);
		C->SetCullDistance(bNoCull ? 0.f : S.LD);
		C->SetCachedMaxDrawDistance(bNoCull ? 0.f : S.Cached);
		C->MinDrawDistance = bNoCull ? 0.f : S.MinDraw;
		if (S.bIsm)
		{
			static_cast<UInstancedStaticMeshComponent*>(C)->SetCullDistances(bNoCull ? 0 : S.IStart, bNoCull ? 0 : S.IEnd);
		}
		if (S.bWpo)
		{
			C->SetEvaluateWorldPositionOffset(!bOn); // the wind frozen: plants at rest
			++Frozen;
		}
		C->MarkRenderStateDirty();
	}
	R.bSwitched = bOn;
	World.SendAllEndOfFrameUpdates();
	FlushRenderingCommands();
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover ring: switches %s (LOD0 %d, no cull %d, world-position offset off on %d of %d components)."),
		bOn ? TEXT("on") : TEXT("off"), bLod0 ? 1 : 0, bNoCull ? 1 : 0, bOn ? Frozen : 0, R.Saved.Num());
	if (!bOn)
	{
		R.Saved.Reset();
	}
}
} // namespace

void UFTRainCoverSubsystem::RingTick(UWorld& World, const FVector& Eye)
{
	using namespace FTRainRingDetail;
	FFTRainRing& R = *Ring;
	const int32 Res = Spec.resolution, N = Res * Res;
	const int32 NT = R.Tiles.Num();
	UMaterialParameterCollectionInstance* P = FTRainCover::Params(World);

	// A slot's vector: the tile it holds and its range, or none (reads open).
	const auto SetSlot = [&](int32 S, int32 Ti)
	{
		R.SlotTi[S] = Ti;
		const ftcore::RainCoverSlot V = Ti >= 0 ? ftcore::rainCoverSlotFor(R.Tiles[Ti], R.Head[Ti].range()) : ftcore::RainCoverSlot{};
		if (P)
		{
			P->SetVectorParameterValue(R.SlotName[S], FLinearColor(V.tx, V.ty, V.minZcm, V.maxZcm));
		}
	};
	// One tile's codes into its slot's block of the atlas (render thread, in order with the frame's draws).
	FTextureRenderTargetResource* Atlas = Target->GameThread_GetRenderTargetResource();
	UTexture2D* Staging = R.bRouteB ? RingStaging.Get() : nullptr;
	const auto Upload = [&](int32 S, const uint16* Src)
	{
		int32 Ox = 0, Oy = 0;
		ftcore::rainCoverRingOrigin(Spec, S, Ox, Oy);
		FTextureResource* Stage = Staging ? Staging->GetResource() : nullptr;
		ENQUEUE_RENDER_COMMAND(FTRainCoverRingUpload)([Atlas, Stage, Src, Res, Ox, Oy](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Dst = Atlas ? Atlas->GetTextureRHI() : nullptr;
			if (!Dst)
			{
				return;
			}
			if (!Stage)
			{
				RHICmdList.UpdateTexture2D(Dst, 0, FUpdateTextureRegion2D(uint32(Ox), uint32(Oy), 0, 0, uint32(Res), uint32(Res)),
					uint32(Res) * sizeof(uint16), reinterpret_cast<const uint8*>(Src));
				return;
			}
			FRHITexture* Mid = Stage->GetTextureRHI();
			if (!Mid)
			{
				return;
			}
			RHICmdList.UpdateTexture2D(Mid, 0, FUpdateTextureRegion2D(0, 0, 0, 0, uint32(Res), uint32(Res)), uint32(Res) * sizeof(uint16),
				reinterpret_cast<const uint8*>(Src));
			RHICmdList.Transition({FRHITransitionInfo(Mid, ERHIAccess::Unknown, ERHIAccess::CopySrc),
				FRHITransitionInfo(Dst, ERHIAccess::Unknown, ERHIAccess::CopyDest)});
			FRHICopyTextureInfo Copy;
			Copy.Size = FIntVector(Res, Res, 1);
			Copy.DestPosition = FIntVector(Ox, Oy, 0);
			RHICmdList.CopyTexture(Mid, Dst, Copy);
			RHICmdList.Transition({FRHITransitionInfo(Mid, ERHIAccess::CopySrc, ERHIAccess::SRVMask),
				FRHITransitionInfo(Dst, ERHIAccess::CopyDest, ERHIAccess::SRVMask)});
		});
		++R.Uploaded;
	};
	// A tile into its slot from what is in RAM: false when its read has not landed (late).
	const auto Place = [&](int32 S, int32 Ti) -> bool
	{
		if (Ti < 0 || R.Head[Ti].hits == 0) // no tile, or nothing in it: the slot reads open, nothing to upload
		{
			SetSlot(S, Ti);
			return true;
		}
		if (!R.HasPayload(Ti)) // flat and full
		{
			Upload(S, R.Full);
			SetSlot(S, Ti);
			return true;
		}
		for (FFTRainRing::FEntry& E : R.Pool)
		{
			if (E.Ti == Ti && E.bReady)
			{
				Upload(S, E.Buf);
				E.Fence.BeginFence();
				SetSlot(S, Ti);
				return true;
			}
		}
		SetSlot(S, -1);
		return false;
	};
	// Reads for the 5 x 5 round the lead's tile that RAM lacks, into entries outside it that the render thread has let go.
	const auto Prefetch = [&]()
	{
		const auto Inside = [&](int32 Ti)
		{
			return Ti >= 0 && FMath::Abs(R.Tiles[Ti].x - R.Lead.x) <= 2 && FMath::Abs(R.Tiles[Ti].y - R.Lead.y) <= 2;
		};
		for (int32 Dy = -2; Dy <= 2; ++Dy)
		{
			for (int32 Dx = -2; Dx <= 2; ++Dx)
			{
				const int32 Ti = R.Find(ftcore::RainCoverTile{R.Lead.x + Dx, R.Lead.y + Dy});
				if (!R.HasPayload(Ti))
				{
					continue;
				}
				bool bHeld = false;
				for (const FFTRainRing::FEntry& E : R.Pool)
				{
					bHeld = bHeld || E.Ti == Ti;
				}
				if (bHeld)
				{
					continue;
				}
				FFTRainRing::FEntry* Free = nullptr;
				for (FFTRainRing::FEntry& E : R.Pool)
				{
					if (!E.bReading && !Inside(E.Ti) && E.Fence.IsFenceComplete())
					{
						Free = &E;
						break;
					}
				}
				if (!Free)
				{
					++R.NoRoom;
					continue;
				}
				R.Evicts += Free->Ti >= 0 ? 1 : 0;
				Free->Ti = Ti;
				Free->bReady = false;
				Free->bReading = true;
				uint16* Into = Free->Buf;
				const TCHAR* File = *R.Path[Ti];
				const int32 Bytes = R.PayloadBytes;
				std::atomic<int64>* Us = &Free->Us;
				Free->Read = Async(EAsyncExecution::ThreadPool, [File, Into, Bytes, Us]() -> bool
				{
					double Ms = 0.0;
					const bool bOk = RingReadPayload(File, Into, Bytes, Ms);
					Us->store(int64(Ms * 1000.0));
					return bOk;
				});
			}
		}
	};

	++R.Ticks;
	switch (R.State)
	{
	case FFTRainRing::Settle:
	{
		if (R.Ticks < R.SettleTicks)
		{
			return;
		}
		const double K0 = FPlatformTime::Seconds();
		RingKeyTiles(World, Spec, R);
		const double KeyMs = (FPlatformTime::Seconds() - K0) * 1000.0;
		int32 Match = 0, Stale = 0, Missing = 0;
		for (int32 Ti = 0; Ti < NT; ++Ti)
		{
			ftcore::RainCoverTileHeader H;
			if (!RingReadHeader(R.Path[Ti], H) || H.tile.x != R.Tiles[Ti].x || H.tile.y != R.Tiles[Ti].y)
			{
				++Missing;
			}
			else if (H.key != R.Key[Ti])
			{
				++Stale;
				R.Head[Ti] = H;
			}
			else
			{
				++Match;
				R.Head[Ti] = H;
				R.Have[Ti] = 1;
			}
			if (R.bRebake || !R.Have[Ti])
			{
				R.ToBake.Add(Ti);
			}
		}
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover ring key: %d components, %d instances; %d of %d tiles hold something; key %.1f ms; cache %s: %d match, %d stale, %d missing; %d to bake%s; %d plant tiles (a world-position offset material)."),
			R.Comps, R.Insts, R.Keyed, NT, KeyMs, *R.Dir, Match, Stale, Missing, R.ToBake.Num(), R.bRebake ? TEXT(" (rebake all)") : TEXT(""), R.Plant.Num());
		if (R.ToBake.Num() > 0)
		{
			RingSwitches(World, R, true);
			R.State = FFTRainRing::Switch;
			R.Wait = 0;
			return;
		}
		R.State = FFTRainRing::Fill;
		break;
	}
	case FFTRainRing::Switch:
		if (++R.Wait < kRingSwitchSettle)
		{
			return;
		}
		R.State = FFTRainRing::Bake;
		R.BakeT0 = FPlatformTime::Seconds();
		return;
	case FFTRainRing::Bake:
	{
		R.Codes.SetNumUninitialized(N, EAllowShrinking::No);
		for (int32 Done = 0; Done < kRingBakePerTick && R.BakeAt < R.ToBake.Num(); ++Done, ++R.BakeAt)
		{
			const int32 Ti = R.ToBake[R.BakeAt];
			const double B0 = FPlatformTime::Seconds();
			ftcore::RainCoverFrame F = ftcore::rainCoverTileFrame(Spec, R.Tiles[Ti], kRingFindTopM);
			if (!RingDraw(*RingCapture, *RingTarget, F, kRingFindFarM, R.Pixels, R.Depth))
			{
				continue;
			}
			bool bAny = false;
			float MaxZ = 0.f;
			for (const float D : R.Depth)
			{
				if (D < kRingFindFarM)
				{
					MaxZ = bAny ? FMath::Max(MaxZ, kRingFindTopM - D) : kRingFindTopM - D;
					bAny = true;
				}
			}
			ftcore::RainCoverTileHeader H;
			H.tile = R.Tiles[Ti];
			H.key = R.Key[Ti];
			if (!bAny)
			{
				H.topZ = kRingFindTopM;
				++R.Empty;
			}
			else
			{
				F.topZ = MaxZ + kRingMarginM;
				if (!RingDraw(*RingCapture, *RingTarget, F, Spec.depthM, R.Pixels, R.Depth))
				{
					continue;
				}
				ftcore::RainCoverR16Range Range;
				ftcore::rainCoverR16Pack(R.Depth.GetData(), N, F.topZ, Spec.depthM, R.Codes.GetData(), Range, kRingTolM);
				H.topZ = F.topZ;
				H.minZ = Range.minZ;
				H.maxZ = Range.maxZ;
				H.hits = Range.hits;
				H.res = ftcore::rainCoverTileHeaderOnly(Range, N) ? 0 : Res;
			}
			R.HeaderOnly += H.res == 0 ? 1 : 0;
			const int32 Payload = ftcore::rainCoverPayloadBytes(H);
			R.File.SetNumUninitialized(ftcore::kRainCoverFileHeader + Payload, EAllowShrinking::No);
			ftcore::rainCoverHeaderWrite(H, R.File.GetData());
			if (Payload > 0)
			{
				FMemory::Memcpy(R.File.GetData() + ftcore::kRainCoverFileHeader, R.Codes.GetData(), Payload);
			}
			// The key it replaces (a rebake over a kept file: the key must not have moved).
			ftcore::RainCoverTileHeader Old;
			if (RingReadHeader(R.Path[Ti], Old))
			{
				(Old.key == H.key ? R.KeySame : R.KeyChanged) += 1;
			}
			else
			{
				++R.KeyNew;
			}
			FFileHelper::SaveArrayToFile(R.File, *R.Path[Ti]);
			R.Head[Ti] = H;
			R.Have[Ti] = 1;
			++R.Baked;
			R.BakeBytes += R.File.Num();
			R.BakeMs += (FPlatformTime::Seconds() - B0) * 1000.0;
#if !UE_BUILD_SHIPPING
			if (!R.CompareDir.IsEmpty()) // the determinism check: the same tile from another launch's bake
			{
				TArray<uint8> Other;
				const FString OtherPath = R.CompareDir / FPaths::GetCleanFilename(R.Path[Ti]);
				if (!FFileHelper::LoadFileToArray(Other, *OtherPath, FILEREAD_Silent))
				{
					++R.NoCompare;
				}
				else if (Other.Num() == R.File.Num() && FMemory::Memcmp(Other.GetData(), R.File.GetData(), R.File.Num()) == 0)
				{
					++R.Same;
				}
				else
				{
					++R.Differ;
					int32 Texels = 0;
					if (Other.Num() == R.File.Num())
					{
						const uint16* A = reinterpret_cast<const uint16*>(Other.GetData() + ftcore::kRainCoverFileHeader);
						const uint16* B = reinterpret_cast<const uint16*>(R.File.GetData() + ftcore::kRainCoverFileHeader);
						for (int32 K = 0; K < Payload / 2; ++K)
						{
							Texels += A[K] != B[K] ? 1 : 0;
						}
					}
					if (R.Differ <= 20)
					{
						ftcore::RainCoverTileHeader OH;
						const bool bOH = ftcore::rainCoverHeaderRead(Other.GetData(), Other.Num(), OH);
						UE_LOG(LogForgottenThird, Display,
							TEXT("RainCover ring compare: tile (%d, %d) DIFFERS: %d bytes against %d, %d texels; top %.3f / %.3f, z %.3f..%.3f / %.3f..%.3f, hits %d / %d%s."),
							H.tile.x, H.tile.y, R.File.Num(), Other.Num(), Texels, H.topZ, bOH ? OH.topZ : 0.f, H.minZ, H.maxZ, bOH ? OH.minZ : 0.f,
							bOH ? OH.maxZ : 0.f, H.hits, bOH ? OH.hits : 0, R.Plant.Contains(Ti) ? TEXT(", a plant tile") : TEXT(""));
					}
				}
			}
#endif
		}
		if (R.BakeAt < R.ToBake.Num())
		{
			return;
		}
		RingSwitches(World, R, false);
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover ring bake: %d tiles in %.0f ms wall (%.0f ms drawing, packing and writing); %d empty, %d header only, %d with codes; %.1f MiB written; keys against the files they replace: %d equal, %d changed, %d new."),
			R.Baked, (FPlatformTime::Seconds() - R.BakeT0) * 1000.0, R.BakeMs, R.Empty, R.HeaderOnly - R.Empty, R.Baked - R.HeaderOnly,
			double(R.BakeBytes) / (1024.0 * 1024.0), R.KeySame, R.KeyChanged, R.KeyNew);
#if !UE_BUILD_SHIPPING
		if (!R.CompareDir.IsEmpty())
		{
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover ring compare against %s: %d of %d tiles identical, %d differ, %d missing there."),
				*R.CompareDir, R.Same, R.Baked, R.Differ, R.NoCompare);
		}
#endif
		R.State = FFTRainRing::Fill;
		break;
	}
	default:
		break;
	}

	if (R.State == FFTRainRing::Fill)
	{
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FTLookSwitches(), TEXT("RainCoverHomes")))
		{
			int64 Raw = 0, Packed = 0;
			int32 Files = 0;
			TArray<uint8> Bytes, Out;
			for (int32 Ti = 0; Ti < NT; ++Ti)
			{
				if (!FFileHelper::LoadFileToArray(Bytes, *R.Path[Ti], FILEREAD_Silent))
				{
					continue;
				}
				++Files;
				Raw += Bytes.Num();
				int32 Size = FCompression::CompressMemoryBound(NAME_Oodle, Bytes.Num());
				Out.SetNumUninitialized(Size, EAllowShrinking::No);
				Packed += FCompression::CompressMemory(NAME_Oodle, Out.GetData(), Size, Bytes.GetData(), Bytes.Num()) ? Size : Bytes.Num();
			}
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover homes: %d tile files, %.1f MiB raw, %.1f MiB Oodle-compressed (as a pak would store them)."),
				Files, double(Raw) / (1024.0 * 1024.0), double(Packed) / (1024.0 * 1024.0));
		}
#endif
		if (R.bBakeOnly)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover ring: bake only, done; the cover stays off."));
			R.State = FFTRainRing::Done;
			FPlatformMisc::RequestExit(false, TEXT("RainCoverBakeOnly"));
			return;
		}
		// The nine round the lead's tile, read here (synchronously, once), then CoverOn.
		const double F0 = FPlatformTime::Seconds();
		R.Lead = ftcore::rainCoverTileAt(Spec, float(Eye.X) * 0.01f, float(Eye.Y) * 0.01f);
		int32 Next = 0;
		for (int32 Dy = -1; Dy <= 1; ++Dy)
		{
			for (int32 Dx = -1; Dx <= 1; ++Dx)
			{
				const ftcore::RainCoverTile T{R.Lead.x + Dx, R.Lead.y + Dy};
				const int32 Ti = R.Find(T);
				if (R.HasPayload(Ti))
				{
					FFTRainRing::FEntry& E = R.Pool[Next++];
					double Ms = 0.0;
					E.Ti = Ti;
					E.bReady = RingReadPayload(*R.Path[Ti], E.Buf, R.PayloadBytes, Ms);
					R.ReadFails += E.bReady ? 0 : 1;
				}
				Place(ftcore::rainCoverRingSlot(T), Ti);
			}
		}
		Prefetch();
		R.FillMs = (FPlatformTime::Seconds() - F0) * 1000.0;
		if (P)
		{
			P->SetScalarParameterValue(FTRainCover::On(), 1.f);
		}
		bHaveCapture = true;
		R.State = FFTRainRing::Run;
		R.AtlasCheckAt = R.AtlasCheckEvery > 0 ? R.Ticks + 2 : -1;
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover ring: on at tile (%d, %d); the nine filled in %.1f ms; CoverOn 1."), R.Lead.x, R.Lead.y, R.FillMs);
		return;
	}
	if (R.State != FFTRainRing::Run)
	{
		return;
	}

	// Reads that landed; a late slot is filled the tick its read lands.
	for (FFTRainRing::FEntry& E : R.Pool)
	{
		if (!E.bReading || !E.Read.IsReady())
		{
			continue;
		}
		E.bReading = false;
		E.bReady = E.Read.Get();
		++R.Reads;
		R.ReadFails += E.bReady ? 0 : 1;
		const double Ms = double(E.Us.load()) / 1000.0;
		R.ReadMsSum += Ms;
		R.ReadMsMax = FMath::Max(R.ReadMsMax, Ms);
		if (!E.bReady)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("RainCover ring: read failed: %s."), *R.Path[E.Ti]);
			E.Ti = -1; // free again; the next prefetch reads it anew
			continue;
		}
		for (int32 S = 0; S < 9; ++S)
		{
			if (R.SlotLate[S] == E.Ti)
			{
				R.SlotLate[S] = -1;
				Place(S, E.Ti);
				++R.LateFilled;
			}
		}
	}

#if !UE_BUILD_SHIPPING
	if (R.Force > 0)
	{
		// The corner (5) and jump (9) cells: on every 5 m trigger, that many baked tiles read off disk ahead of it come in.
		if (R.Textured.Num() == 0)
		{
			for (int32 Ti = 0; Ti < NT; ++Ti)
			{
				if (R.HasPayload(Ti))
				{
					R.Textured.Add(Ti);
				}
			}
			if (R.Textured.Num() == 0)
			{
				R.Force = 0;
				return;
			}
		}
		for (int32 B = 0; B < 2; ++B)
		{
			if (R.bForceReading[B] && R.ForceRead[B].IsReady())
			{
				R.bForceReading[B] = false;
				const int32 Good = R.ForceRead[B].Get();
				++R.Reads;
				R.ReadFails += Good == R.Force ? 0 : 1;
				const double Ms = double(R.ForceUs.load()) / 1000.0;
				R.ReadMsSum += Ms;
				R.ReadMsMax = FMath::Max(R.ReadMsMax, Ms);
				R.ForceReady = B;
			}
		}
		if (R.ForceReady < 0 && !R.bForceReading[0] && !R.bForceReading[1])
		{
			const int32 B = R.ForceFence[R.ForceLast ^ 1].IsFenceComplete() ? (R.ForceLast ^ 1) : (R.ForceFence[R.ForceLast].IsFenceComplete() ? R.ForceLast : -1);
			if (B < 0)
			{
				++R.ForceBusy;
			}
			else
			{
				const TCHAR* Files[9] = {};
				for (int32 J = 0; J < R.Force; ++J)
				{
					Files[J] = *R.Path[R.Textured[(R.ForceNext + J) % R.Textured.Num()]];
				}
				uint16* Into = R.ForceBuf[B];
				const int32 K = R.Force, Bytes = R.PayloadBytes, Words = N;
				std::atomic<int64>* Us = &R.ForceUs;
				R.ForceRead[B] = Async(EAsyncExecution::ThreadPool, [Files, Into, K, Bytes, Words, Us]() -> int32
				{
					const double T0 = FPlatformTime::Seconds();
					int32 Good = 0;
					for (int32 J = 0; J < K; ++J)
					{
						double Ms = 0.0;
						Good += RingReadPayload(Files[J], Into + SIZE_T(J) * Words, Bytes, Ms) ? 1 : 0;
					}
					Us->store(int64((FPlatformTime::Seconds() - T0) * 1e6));
					return Good;
				});
				R.bForceReading[B] = true;
			}
		}
		const float Ex = float(Eye.X) * 0.01f, Ey = float(Eye.Y) * 0.01f;
		if (!ftcore::rainCoverRecapture(Spec, R.bTriggered, R.Trigger.cx, R.Trigger.cy, Ex, Ey))
		{
			return;
		}
		R.Trigger = ftcore::rainCoverFrameAt(Spec, Ex, Ey, float(Eye.Z) * 0.01f);
		R.bTriggered = true;
		if (R.ForceReady < 0)
		{
			++R.ForceLate;
			return;
		}
		const int32 B = R.ForceReady;
		R.ForceReady = -1;
		R.ForceLast = B;
		CSV_CUSTOM_STAT(RainCover, Upload, R.Force, ECsvCustomStatOp::Set);
		{
			CSV_SCOPED_TIMING_STAT(RainCover, UploadEnqueue);
			for (int32 J = 0; J < R.Force; ++J)
			{
				const int32 Ti = R.Textured[(R.ForceNext + J) % R.Textured.Num()];
				Upload(J, R.ForceBuf[B] + SIZE_T(J) * N);
				SetSlot(J, Ti);
			}
			R.ForceFence[B].BeginFence();
		}
		R.ForceNext = (R.ForceNext + R.Force) % R.Textured.Num();
		++R.ForceHits;
		return;
	}
#endif

	// A crossing: the entering tiles come in from RAM this tick, with their slot vectors.
	const ftcore::RainCoverTile Now = ftcore::rainCoverTileAt(Spec, float(Eye.X) * 0.01f, float(Eye.Y) * 0.01f);
	if (Now != R.Lead)
	{
		ftcore::RainCoverTile In[9];
		const int32 Entering = ftcore::rainCoverRingEntered(R.Lead, Now, In);
		R.Lead = Now;
		++R.Crossings;
		const int32 Before = R.Uploaded;
		{
			CSV_SCOPED_TIMING_STAT(RainCover, UploadEnqueue);
			for (int32 K = 0; K < Entering; ++K)
			{
				const int32 S = ftcore::rainCoverRingSlot(In[K]);
				const int32 Ti = R.Find(In[K]);
				R.SlotLate[S] = -1;
				if (!Place(S, Ti))
				{
					R.SlotLate[S] = Ti;
					++R.Late;
				}
			}
		}
		CSV_CUSTOM_STAT(RainCover, Upload, R.Uploaded - Before, ECsvCustomStatOp::Set);
		Prefetch();
		if (R.AtlasCheckEvery > 0 && R.Crossings % R.AtlasCheckEvery == 0)
		{
			R.AtlasCheckAt = R.Ticks + 2;
		}
	}

#if !UE_BUILD_SHIPPING
	// The atlas read back against the tile files through the Core mapping (a texel's ring texel against its tile texel).
	if (R.AtlasCheckAt >= 0 && R.Ticks >= R.AtlasCheckAt)
	{
		R.AtlasCheckAt = -1;
		TArray<FLinearColor> Back;
		const int32 Side = ftcore::rainCoverAtlasSide(Spec);
		FTextureRenderTargetResource* Res2 = Target->GameThread_GetRenderTargetResource();
		if (Res2 && Res2->ReadLinearColorPixels(Back) && Back.Num() == Side * Side)
		{
			int32 Checked = 0, Wrong = 0, Skipped = 0;
			TArray<uint16> Codes;
			Codes.SetNumUninitialized(N);
			for (int32 S = 0; S < 9; ++S)
			{
				const int32 Ti = R.SlotTi[S];
				if (Ti < 0 || R.Head[Ti].hits == 0)
				{
					++Skipped;
					continue;
				}
				if (R.HasPayload(Ti))
				{
					TArray<uint8> Bytes;
					if (!FFileHelper::LoadFileToArray(Bytes, *R.Path[Ti]) || Bytes.Num() != ftcore::kRainCoverFileHeader + R.PayloadBytes)
					{
						++Skipped;
						continue;
					}
					FMemory::Memcpy(Codes.GetData(), Bytes.GetData() + ftcore::kRainCoverFileHeader, R.PayloadBytes);
				}
				else
				{
					ftcore::rainCoverTileFill(R.Head[Ti], Res, Codes.GetData());
				}
				// Every texel centre of the tile, through both mappings.
				const float Tx = ftcore::rainCoverTexel(Spec);
				for (int32 Gi = 0; Gi < Res; ++Gi)
				{
					for (int32 Gj = 0; Gj < Res; ++Gj)
					{
						const float X = (float(R.Tiles[Ti].x * Res + Gi) + 0.5f) * Tx, Y = (float(R.Tiles[Ti].y * Res + Gj) + 0.5f) * Tx;
						ftcore::RainCoverTile T1, T2;
						int32 Px = 0, Py = 0, Sl = 0, Ax = 0, Ay = 0;
						if (!ftcore::rainCoverTileTexel(Spec, X, Y, T1, Px, Py) || !ftcore::rainCoverRingTexel(Spec, X, Y, T2, Sl, Ax, Ay))
						{
							continue;
						}
						const uint16 Want = Codes[Py * Res + Px];
						const uint16 Got = uint16(FMath::RoundToInt(FMath::Clamp(Back[Ay * Side + Ax].R, 0.f, 1.f) * 65535.f));
						++Checked;
						Wrong += (Want != Got || Sl != S || !(T1 == R.Tiles[Ti])) ? 1 : 0;
					}
				}
			}
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover atlas check (crossing %d): %d texels checked, %d WRONG; %d slots skipped (open or late)."),
				R.Crossings, Checked, Wrong, Skipped);
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("RainCover atlas check: no readback."));
		}
	}
	if (R.bWitness)
	{
		RingWitnessTick(World);
	}
#endif
}

#if !UE_BUILD_SHIPPING
// The fidelity witness: the shipped 60 m draw (its frame over a camera on the tile, the trimmed set, the shipped LOD and
// cull unless -RainCoverTileLod0 / -RainCoverTileNoCull, the wind frozen as in the bake) against the baked tiles, texel for
// texel through the Core mapping. Classes as the 3b probe's: agree within 2 mm; band (the tile's occluder over the square's
// top, which the draw can't see); below either far plane; fails: height, gained (tile only), lost (draw only). The meshes
// standing at the first failing texels are named. Exits when done.
void UFTRainCoverSubsystem::RingWitnessTick(UWorld& World)
{
	using namespace FTRainRingDetail;
	FFTRainRing& R = *Ring;
	const int32 Res = Spec.resolution, N = Res * Res;
	constexpr int32 kWitnessSettle = 20;
	constexpr int32 kWitnessNamed = 256;
	if (R.WitnessStep == 2)
	{
		return;
	}
	if (R.WitnessStep == 0)
	{
		TArray<int32> PlantSorted = R.Plant.Array();
		PlantSorted.Sort();
		R.WitnessTiles = PlantSorted;
		for (const int32 Ti : R.WitnessList)
		{
			R.WitnessTiles.AddUnique(Ti);
		}
		if (!TileCamera)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags = RF_Transient;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			TileCamera = World.SpawnActor<ACameraActor>(FVector::ZeroVector, FRotator(-45.f, 0.f, 0.f), Params);
		}
		if (!TileCamera || R.WitnessTiles.Num() == 0)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("RainCover witness: off (%s)."), TileCamera ? TEXT("no tiles") : TEXT("no camera"));
			R.bWitness = false;
			return;
		}
		const TCHAR* Sw = FTLookSwitches();
		R.bWitnessLod0 = FParse::Param(Sw, TEXT("RainCoverTileLod0"));
		R.bWitnessNoCull = FParse::Param(Sw, TEXT("RainCoverTileNoCull"));
		RingSwitches(World, R, true, R.bWitnessLod0, R.bWitnessNoCull);
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover witness: %d squares (%d plant tiles, %d listed), %s draw, the wind frozen."),
			R.WitnessTiles.Num(), PlantSorted.Num(), R.WitnessList.Num(),
			R.bWitnessLod0 || R.bWitnessNoCull ? TEXT("the switched") : TEXT("the shipped-settings"));
		R.WitnessStep = 1;
		return;
	}
	if (R.WitnessAt < R.WitnessTiles.Num())
	{
		const int32 Wt = R.WitnessTiles[R.WitnessAt];
		if (R.WitnessWait < 0)
		{
			const ftcore::RainCoverFrame Tf = ftcore::rainCoverTileFrame(Spec, R.Tiles[Wt], 0.f);
			TilePlaceCamera(World, Tf.cx, Tf.cy);
			R.WitnessWait = kWitnessSettle;
			return;
		}
		if (R.WitnessWait-- > 0)
		{
			return;
		}
		R.WitnessWait = -1;
		++R.WitnessAt;
		const ftcore::RainCoverFrame F = ftcore::rainCoverFrameAt(Spec, float(TileCam.X) * 0.01f, float(TileCam.Y) * 0.01f, float(TileCam.Z) * 0.01f);
		if (!RingDraw(*RingCapture, *RingTarget, F, Spec.depthM, R.Pixels, R.Depth))
		{
			++R.WNoDraw;
			return;
		}
		TMap<int32, TArray<uint16>> Codes;
		const auto CodesOf = [&](int32 Ti) -> const TArray<uint16>*
		{
			if (TArray<uint16>* Have = Codes.Find(Ti))
			{
				return Have;
			}
			TArray<uint16>& C = Codes.Add(Ti);
			C.SetNumUninitialized(N);
			const ftcore::RainCoverTileHeader& H = R.Head[Ti];
			if (H.res == 0)
			{
				ftcore::rainCoverTileFill(H, Res, C.GetData());
				return &C;
			}
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *R.Path[Ti]) || Bytes.Num() != ftcore::kRainCoverFileHeader + R.PayloadBytes)
			{
				Codes.Remove(Ti);
				return nullptr;
			}
			FMemory::Memcpy(C.GetData(), Bytes.GetData() + ftcore::kRainCoverFileHeader, R.PayloadBytes);
			return &C;
		};
		int64 Fails = 0;
		for (int32 Py = 0; Py < Res; ++Py)
		{
			for (int32 Px = 0; Px < Res; ++Px)
			{
				const float X = F.cx + (0.5f - (float(Py) + 0.5f) / float(Res)) * F.sizeM;
				const float Y = F.cy + ((float(Px) + 0.5f) / float(Res) - 0.5f) * F.sizeM;
				const float D = R.Depth[Py * Res + Px];
				const bool bR = FMath::IsFinite(D) && D < Spec.depthM;
				const float Rz = F.topZ - D;
				++R.WTexels;
				ftcore::RainCoverTile T;
				int32 Tx = 0, Ty = 0;
				const int32 Ti = ftcore::rainCoverTileTexel(Spec, X, Y, T, Tx, Ty) ? R.Find(T) : -1;
				bool bT = false;
				float Tz = 0.f, Floor = -1.0e9f;
				if (Ti < 0 || !R.Have[Ti])
				{
					++R.WNoTile; // unlisted or unbaked: the ring reads it open
				}
				else
				{
					const ftcore::RainCoverTileHeader& H = R.Head[Ti];
					Floor = H.hits > 0 ? H.topZ - Spec.depthM : kRingFindTopM - kRingFindFarM;
					const TArray<uint16>* C = H.hits > 0 ? CodesOf(Ti) : nullptr;
					bT = C && ftcore::rainCoverR16Unpack((*C)[Ty * Res + Tx], H.range(), Tz);
				}
				bool bFail = false;
				if (!bT && !bR)
				{
					++R.WAgree;
				}
				else if (bT && bR)
				{
					const float Dz = FMath::Abs(Tz - Rz);
					if (Dz <= kRingTolM)
					{
						++R.WAgree;
					}
					else if (Tz > F.topZ)
					{
						++R.WBand;
					}
					else
					{
						++R.WHeight;
						R.WMaxCm = FMath::Max(R.WMaxCm, Dz * 100.f);
						bFail = true;
					}
				}
				else if (bT)
				{
					if (Tz > F.topZ)
					{
						++R.WBand;
					}
					else if (Tz < F.topZ - Spec.depthM)
					{
						++R.WBelow;
					}
					else
					{
						++R.WGained;
						bFail = true;
					}
				}
				else if (Rz < Floor)
				{
					++R.WBelow;
				}
				else
				{
					++R.WLost;
					bFail = true;
				}
				if (bFail)
				{
					++Fails;
					if (R.WFailAt.Num() < kWitnessNamed)
					{
						R.WFailAt.Add(FVector(X, Y, bR ? Rz : Tz));
					}
				}
			}
		}
		R.WFails += Fails;
		if (Fails > 0)
		{
			R.WFailTile.Add(Wt, Fails);
		}
		return;
	}

	// Done: name what stands at the first failing texels, put the switches back, report, exit.
	TMap<FString, int32> Names;
	if (R.WFailAt.Num() > 0)
	{
		constexpr double kSlackCm = 15.0;
		const auto Test = [&](const FBox& Bx, const UStaticMesh* M)
		{
			for (const FVector& P : R.WFailAt)
			{
				const FVector Q = P * 100.0;
				if (Q.X >= Bx.Min.X - kSlackCm && Q.X <= Bx.Max.X + kSlackCm && Q.Y >= Bx.Min.Y - kSlackCm && Q.Y <= Bx.Max.Y + kSlackCm &&
					Q.Z >= Bx.Min.Z - kSlackCm && Q.Z <= Bx.Max.Z + kSlackCm)
				{
					Names.FindOrAdd(M->GetName()) += 1;
				}
			}
		};
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* C = *It;
			const UStaticMesh* M = C->GetStaticMesh();
			if (C->GetWorld() != &World || !C->IsRegistered() || !M)
			{
				continue;
			}
			if (const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C))
			{
				const FBox MB = M->GetBoundingBox();
				for (int32 K = 0; K < I->GetInstanceCount(); ++K)
				{
					FTransform X;
					if (I->GetInstanceTransform(K, X, true))
					{
						Test(MB.TransformBy(X), M);
					}
				}
			}
			else
			{
				Test(C->Bounds.GetBox(), M);
			}
		}
	}
	RingSwitches(World, R, false);
	Names.ValueSort([](int32 A, int32 B) { return A > B; });
	FString Named, Tiles;
	int32 K = 0;
	for (const TPair<FString, int32>& P : Names)
	{
		if (K++ >= 16)
		{
			break;
		}
		Named += FString::Printf(TEXT("%s%s %d"), Named.IsEmpty() ? TEXT("") : TEXT("; "), *P.Key, P.Value);
	}
	for (const TPair<int32, int64>& P : R.WFailTile)
	{
		Tiles += FString::Printf(TEXT("%s(%d, %d) %lld%s"), Tiles.IsEmpty() ? TEXT("") : TEXT("; "), R.Tiles[P.Key].x, R.Tiles[P.Key].y, P.Value,
			R.Plant.Contains(P.Key) ? TEXT(" plant") : TEXT(""));
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover witness (%s): %d squares (%d plant tiles), %d not drawn; %lld texels: agree %lld, band over the square's top %lld, below a far plane %lld, on no baked tile %lld; FAILS %lld (height %lld, worst %.1f cm; gained %lld; lost %lld)."),
		R.bWitnessLod0 || R.bWitnessNoCull ? TEXT("switched") : TEXT("shipped settings"), R.WitnessAt, R.Plant.Num(), R.WNoDraw, R.WTexels, R.WAgree,
		R.WBand, R.WBelow, R.WNoTile, R.WFails, R.WHeight, R.WMaxCm, R.WGained, R.WLost);
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover witness fails by square: %s."), Tiles.IsEmpty() ? TEXT("none") : *Tiles);
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover witness: meshes at the first %d failing texels: %s."), R.WFailAt.Num(),
		Named.IsEmpty() ? TEXT("none") : *Named);
	R.WitnessStep = 2;
	FPlatformMisc::RequestExit(false, TEXT("RainCoverWitness"));
}
#endif
