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
// (A, the default, writes the atlas directly). Dev only: -RainCoverRingForce=5|9 (on top of the normal ring, every 5 m
// trigger re-uploads that many of the ring's own slot tiles, read off disk unbuffered: the corner and jump cells),
// -RainCoverTour=<cm a frame> (the lead set down along a fixed loop that crosses tile sides and corners), -RainCoverCompareDir=<dir> (each baked tile against the
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
#include "Engine/Level.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Rendering/NaniteResources.h"
#include "StaticMeshResources.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Hash/CityHash.h"
#include "IO/IoHash.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "RenderCommandFence.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#if !UE_BUILD_SHIPPING
#include "LandscapeComponent.h"
#include "LandscapeProxy.h"
#include "ContentStreaming.h"
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
constexpr float kRingTolM = ftcore::kRainCoverRingTolM; // the ruled height tolerance (2.5 mm, ruling 12825)
constexpr int32 kRingBakePerTick = 32;
constexpr int32 kRingSwitchSettle = 60; // ticks after the bake switches go on
constexpr double kRingShaderWaitS = 1200.0; // the most the bake waits for shader compiles
constexpr int32 kRingPool = 25;         // 5 x 5 round the lead's tile
constexpr int32 kRingAlign = 4096;      // unbuffered reads: offset, size and buffer sector-aligned

// The live r.Nanite.MaxPixelsPerEdge, which the bake's and the witness's Nanite raster runs at; -1 when the cvar is not
// found (refused by ftcore::rainCoverMayBake).
float RingPixelsPerEdge()
{
	const IConsoleVariable* const V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.MaxPixelsPerEdge"));
	return V ? V->GetFloat() : -1.f;
}

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
#if !UE_BUILD_SHIPPING
	// Dev only, as its includes and every use are (ruling 13200 point 3).
	struct FLandSaved
	{
		TWeakObjectPtr<ULandscapeComponent> Comp;
		int32 Forced = -1;
	};
#endif
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
	TMap<FIntPoint, float> BoundsTop; // per tile: the top (m) of the highest box the key buckets into it (the clamp's bounds)
	int32 Raised = 0, Clamped = 0;    // tiles whose top the clamp raised to their bounds, and capped by the depth draw
	int32 Deep = 0, PackRefused = 0;  // tiles drawn past depthM (the per-tile far), and refused by the pack's tolerance
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
	double BakeMs = 0.0, BakeT0 = 0.0, ShaderWaitT0 = 0.0;
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
	int32 ForceSlot[2][9] = {}, ForceTi[2][9] = {}, ForceK[2] = {0, 0};
	int32 ForceRot = 0, ForceReady = -1, ForceLast = 1, ForceLate = 0, ForceBusy = 0, ForceHits = 0, ForceUp = 0, ForceStale = 0;
	int32 ForceFull = 0, ForcePad = 0, PadNext = 0;
	TArray<int32> Textured; // every tile with a payload: what an open slot's read takes (its slot vector hides it)
	// -RainCoverTour=<cm a frame>: the lead set down along a fixed loop (tile edges and corners crossed), one step a frame.
	float Tour = 0.f;
	TArray<FVector2D> TourPts; // cm
	int32 TourLeg = 0;
	double TourAlong = 0.0, TourDist = 0.0;
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
	// Fails that one texel of registration explains: some texel in the tile's own 3x3 around it agrees with the draw
	// (same height within the tolerance, or open for a gain). By side; and the worst height fail that no neighbour explains.
	int64 WEdge[4] = {0, 0, 0, 0}, WSide[2] = {0, 0};
	float WOffMaxCm = 0.f;
	FVector WOffWorstAt = FVector::ZeroVector;
	int32 WOffWorstSq = -1, WOffWorstSide = -1;
	// Every failing texel: plan point (m), the higher of the two heights (m), its side and its square.
	struct FWFail
	{
		float X = 0.f, Y = 0.f, Z = 0.f, Dz = 0.f;
		uint8 Side = 0; // 0 height, the tile higher; 1 height, the draw higher; 2 gained (tile only); 3 lost (draw only)
		int32 Sq = 0;   // index into WitnessTiles
	};
	TArray<FWFail> WFailAt;
	struct FWSquare
	{
		float CamZ = 0.f, TopZ = 0.f;
		int64 Side[4] = {0, 0, 0, 0};
		int64 Edge[4] = {0, 0, 0, 0};
		double DzSum[2] = {0.0, 0.0};
		float DzMax[2] = {0.f, 0.f};
		// Ruling 8836 items 3-4. Dir: per side, the fails that the tile's neighbour in each of the 8 directions explains
		// (counted independently); TileEdge: fails on the tile's border row or column (some neighbours unread). Pre and Post:
		// per side, the fails that a redraw at the bake's own top and far clears, before the settle and after the witness
		// draw. TopLow: draw-higher fails the post redraw clears, with the witness height over the bake top [0] or not [1].
		int64 Dir[4][8] = {};
		int64 TileEdge[4] = {0, 0, 0, 0};
		int64 Pre[4] = {0, 0, 0, 0}, Post[4] = {0, 0, 0, 0};
		int64 TopLow[2] = {0, 0};
		int64 PreOff = 0, PostOff = 0; // texels where the redraw and the tile disagree (all texels of the square)
		int64 Owned = 0, Unowned = 0, EmptySolos = 0, Candidates = 0, OwnTexels = 0;
		// dz histogram per sign (tile higher, draw higher); bin edges kWDzEdges (m).
		int64 DzHist[2][8] = {};
	};
	TMap<int32, FWSquare> WFailTile; // by WitnessTiles index
	FVector WWorstAt = FVector::ZeroVector;
	int32 WWorstSide = -1, WWorstSq = -1;
	// Ruling 8836: the redraws at the bake frame (pre: on arrival, before the settle; post: after the witness draw), a
	// one-component draw, and one row per failing texel for -RainCoverWitnessCsv=<file>.
	TArray<float> DepthPre, DepthPost, DepthSolo;
	bool bPre = false;
	int64 WPreOff = 0, WPostOff = 0, WCtrlSquares = 0, WCtrlPostOff = 0, WCtrlPreOff = 0, WNoRedraw = 0;
	FString WitnessCsv;
	// -RainCoverLandscapeFull (dev, off by default): while the switches are on, every landscape component at ForcedLOD 0 with
	// its heightmap held fully resident, for the bake and the witness alike. Bake it into a fresh dir: the key leaves it out.
	bool bLandFull = false;
#if !UE_BUILD_SHIPPING
	TArray<FLandSaved> LandSaved;
#endif
	// -RainCoverWitnessDump=<dir>: per square, the witness, post and pre heights as raw float32 (NaN: no hit), for a
	// texel-for-texel compare of two witness runs.
	FString WitnessDump;
	int32 WitnessSettle = 20; // -RainCoverWitnessSettle=<ticks>: the witness's wait over each square (the bake's: SettleTicks)
	struct FWRow
	{
		int32 Sq = 0;
		int16 Px = 0, Py = 0, Tx = 0, Ty = 0; // the witness frame's texel; the tile's
		uint8 Side = 0, Dirs = 0, Flags = 0; // Flags: 1 tile border, 2 at the max code, 4 pre clears, 8 post clears, 16 on the square's own tile
		float X = 0.f, Y = 0.f, Tz = NAN, Rz = NAN, PreZ = NAN, PostZ = NAN;
		int32 Owner = -1; // into WOwners
	};
	TArray<FWRow> WRows;
	TArray<FString> WOwners;

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
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover ring reads: every payload read unbuffered (OpenReadNoBuffering, the OS file cache bypassed)."));
	if (R.Force > 0)
	{
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover ring force: %d slots a hit, each with one unbuffered tile read, re-uploaded on top of the normal ring: %d hits, %d FULL (all %d slots up), %d slot uploads (%d into open or flat slots), %d skipped (the slot's tile changed since its read), %d late (read not back, skipped), %d ticks a buffer waited on its fence."),
			R.Force, R.ForceHits, R.ForceFull, R.Force, R.ForceUp, R.ForcePad, R.ForceStale, R.ForceLate, R.ForceBusy);
	}
	if (R.Tour > 0.f)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover tour: %.1f cm a frame, %.0f m walked, leg %d of %d."), R.Tour, R.TourDist * 0.01, R.TourLeg,
			R.TourPts.Num());
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
	FParse::Value(Sw, TEXT("RainCoverTour="), R->Tour);
	R->Tour = FMath::Clamp(R->Tour, 0.f, 200.f);
	// The file is read first: with the bare switch first, || never reached the value and a listed file was dropped.
	FString WitnessFile;
	const bool bWitnessFile = FParse::Value(Sw, TEXT("RainCoverWitness="), WitnessFile);
	R->bWitness = bWitnessFile || FParse::Param(Sw, TEXT("RainCoverWitness"));
	FParse::Value(Sw, TEXT("RainCoverWitnessCsv="), R->WitnessCsv);
	FParse::Value(Sw, TEXT("RainCoverWitnessSettle="), R->WitnessSettle);
	FParse::Value(Sw, TEXT("RainCoverWitnessDump="), R->WitnessDump);
	R->bLandFull = FParse::Param(Sw, TEXT("RainCoverLandscapeFull"));
	if (R->bLandFull && BakeDir.IsEmpty())
	{
		// Ruling 9035 item 5: a forced-landscape bake never writes the shipped cache; it runs on a copy (-RainCoverBakeDir).
		UE_LOG(LogForgottenThird, Error,
			TEXT("RainCover VOID: -RainCoverLandscapeFull without -RainCoverBakeDir (it would rebake the shipped cache); the flag and the witness are off."));
		R->bLandFull = false;
		R->bWitness = false;
	}
	R->WitnessSettle = FMath::Clamp(R->WitnessSettle, 0, 100000);
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
	// Every run states its dev flags (all off by default, none in Shipping).
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover dev flags: force %d, tour %.1f cm a frame, atlas check every %d crossings, witness %d (%d listed), compare %s, key dumps %d, homes %d, ground count %d."),
		R->Force, R->Tour, R->AtlasCheckEvery, R->bWitness ? 1 : 0, R->WitnessList.Num(), R->CompareDir.IsEmpty() ? TEXT("off") : *R->CompareDir,
		R->KeyDump.Num(), FParse::Param(Sw, TEXT("RainCoverHomes")) ? 1 : 0, FParse::Param(Sw, TEXT("RainCoverGroundCount")) ? 1 : 0);
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

#if !UE_BUILD_SHIPPING
static void RingLandLog(UWorld& World, const TCHAR* Stage, int32 Sx, int32 Sy, double Cx, double Cy, double HalfCm);
#endif
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
	R.BoundsTop.Reset();
	const auto Bucket = [&](const FBox& Bx, uint64 H, bool bWpo)
	{
		const ftcore::RainCoverTile A = ftcore::rainCoverTileAt(Spec, float(Bx.Min.X) * 0.01f, float(Bx.Min.Y) * 0.01f);
		const ftcore::RainCoverTile Z = ftcore::rainCoverTileAt(Spec, float(Bx.Max.X) * 0.01f, float(Bx.Max.Y) * 0.01f);
		for (int32 X = A.x; X <= Z.x && X - A.x < 64; ++X)
		{
			for (int32 Y = A.y; Y <= Z.y && Y - A.y < 64; ++Y)
			{
				Sum.FindOrAdd(FIntPoint(X, Y)) += H;
				const float Top = float(Bx.Max.Z) * 0.01f;
				if (float* const Bt = R.BoundsTop.Find(FIntPoint(X, Y)))
				{
					*Bt = FMath::Max(*Bt, Top);
				}
				else
				{
					R.BoundsTop.Add(FIntPoint(X, Y), Top);
				}
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
		// A material made at run time (a dynamic instance: the far signs' MID_M_Look_Sign_<n>) is keyed by its first saved
		// parent's path: its numbered name comes from the order objects were made, which a cooked build need not repeat. Its
		// own parameters are not keyed (the parent sets the blend mode the depth draw reads).
		bool bWpo = false, bRunMat = false;
		FString DumpMats;
		for (int32 S = 0; S < C->GetNumMaterials(); ++S)
		{
			const UMaterialInterface* Mi = C->GetMaterial(S);
			const bool bW = Mi && Mi->IsUsingWorldPositionOffset_Concurrent(GMaxRHIFeatureLevel);
			bWpo = bWpo || bW;
			const auto RunTime = [](const UMaterialInterface* X)
			{
				return X->HasAnyFlags(RF_Transient) || !X->HasAnyFlags(RF_WasLoaded | RF_Public);
			};
			bRunMat = bRunMat || (Mi && RunTime(Mi));
			const UMaterialInterface* Keyed = Mi;
			while (Keyed && RunTime(Keyed))
			{
				const UMaterialInstance* Inst = Cast<UMaterialInstance>(Keyed);
				Keyed = Inst ? Inst->Parent.Get() : nullptr;
			}
			const FString MN = Keyed ? Keyed->GetPathName() : Mi ? FString(TEXT("run-time ")) + Mi->GetClass()->GetName() : FString();
			H = CityHash64WithSeed(reinterpret_cast<const char*>(*MN), uint32(MN.Len() * sizeof(TCHAR)), H);
			if (R.KeyDump.Num() > 0)
			{
				DumpMats += FString::Printf(TEXT("%s[%d] %s%s wpo %d"), S ? TEXT("; ") : TEXT(""), S, *MN, Keyed != Mi && Mi ? *(FString(TEXT(" (for ")) + Mi->GetName() + TEXT(")")) : TEXT(""), bW ? 1 : 0);
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
				TEXT("RainCover keydump (%d,%d) %s | owner %s %s | saved %d | mobility %d sim %d | mesh %s hash %016llx | mats %s | ints %d %d %d %d %d %d | %s | inst %d | nanite %d | comp %016llx | sum %016llx"),
				Hit.X, Hit.Y, *C->GetPathName(), Own ? *Own->GetClass()->GetName() : TEXT("-"), Own ? *Own->GetName() : TEXT("-"),
				C->HasAnyFlags(RF_WasLoaded) ? 1 : 0, int32(C->Mobility), C->IsSimulatingPhysics() ? 1 : 0, *M->GetPathName(), HMesh, *DumpMats, Ints[0], Ints[1], Ints[2],
				Ints[3], Ints[4], Ints[5], *DumpT(C->GetComponentTransform()), I ? DumpN : -1, M->HasValidNaniteData() ? 1 : 0, H, DumpSum);
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
	// The bake's own settings: file version, the spec, the find and margin, LOD0, no cull, the wind frozen and the Nanite
	// raster's MaxPixelsPerEdge (ruling 12583: every cache baked before it reads stale), and the per-tile far (12825).
	const float Settings[13] = {float(ftcore::kRainCoverFileVersion), Spec.sizeM, float(Spec.resolution), Spec.depthM, kRingFindTopM, kRingFindFarM,
		kRingMarginM, kRingTolM, 1.f /*LOD0*/, 1.f /*no cull*/, 1.f /*WPO off*/, ftcore::kRainCoverBakeMaxPixelsPerEdge, 1.f /*per-tile far*/};
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
#if !UE_BUILD_SHIPPING
	if (R.bLandFull && (bOn ? R.LandSaved.Num() == 0 : R.LandSaved.Num() > 0))
	{
		if (bOn)
		{
			for (TObjectIterator<ULandscapeComponent> It; It; ++It)
			{
				ULandscapeComponent* L = *It;
				if (L->GetWorld() == &World && L->IsRegistered())
				{
					R.LandSaved.Add({L, L->ForcedLOD});
					L->SetForcedLOD(0);
					if (UTexture2D* H = L->GetHeightmap())
					{
						H->SetForceMipLevelsToBeResident(36000.f);
					}
				}
			}
			IStreamingManager::Get().UpdateResourceStreaming(0.f, true);
			IStreamingManager::Get().BlockTillAllRequestsFinished(60.f);
		}
		else
		{
			for (const FFTRainRing::FLandSaved& L : R.LandSaved)
			{
				if (ULandscapeComponent* C = L.Comp.Get())
				{
					C->SetForcedLOD(L.Forced);
				}
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover ring: -RainCoverLandscapeFull %s on %d landscape components (ForcedLOD 0, heightmaps held resident)."),
			bOn ? TEXT("on") : TEXT("off"), R.LandSaved.Num());
		if (!bOn)
		{
			R.LandSaved.Reset();
		}
	}
#endif
	World.SendAllEndOfFrameUpdates();
	FlushRenderingCommands();
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover ring: switches %s (LOD0 %d, no cull %d, world-position offset off on %d of %d components)."),
		bOn ? TEXT("on") : TEXT("off"), bLod0 ? 1 : 0, bNoCull ? 1 : 0, bOn ? Frozen : 0, R.Saved.Num());
	if (!bOn)
	{
		R.Saved.Reset();
	}
}

#if !UE_BUILD_SHIPPING
// -RainCoverTour: the lead set down a fixed step a frame along a loop from its own tile's centre: 128 m out, 128 m across,
// back, and the two diagonals through the tile corners (so crossings enter 3 tiles on the sides and 5 at the corners),
// 874 m round; the quadrant with the most baked tiles. Its feet go on what a trace down from 3 m over them finds.
void RingTourStep(UWorld& World, FFTRainRing& R, const ftcore::RainCoverSpec& Spec)
{
	APlayerController* PC = World.GetFirstPlayerController();
	APawn* Lead = PC ? PC->GetPawn() : nullptr;
	if (!Lead)
	{
		return;
	}
	if (R.TourPts.Num() == 0)
	{
		const ftcore::RainCoverFrame C = ftcore::rainCoverTileFrame(Spec, R.Lead, 0.f);
		int32 Best = -1, Sx = 1, Sy = 1;
		for (int32 Q = 0; Q < 4; ++Q)
		{
			const int32 Qx = (Q & 1) ? -1 : 1, Qy = (Q & 2) ? -1 : 1;
			int32 Baked = 0;
			for (int32 I = 0; I <= 2; ++I)
			{
				for (int32 J = 0; J <= 2; ++J)
				{
					Baked += R.HasPayload(R.Find(ftcore::RainCoverTile{R.Lead.x + Qx * I, R.Lead.y + Qy * J})) ? 1 : 0;
				}
			}
			if (Baked > Best)
			{
				Best = Baked;
				Sx = Qx;
				Sy = Qy;
			}
		}
		const double L = 12800.0, X0 = double(C.cx) * 100.0, Y0 = double(C.cy) * 100.0;
		const FVector2D A(X0, Y0), B(X0 + Sx * L, Y0), Cc(X0 + Sx * L, Y0 + Sy * L), D(X0, Y0 + Sy * L);
		R.TourPts = {A, B, Cc, D, A, Cc};
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover tour: from tile (%d, %d), quadrant (%d, %d) with %d of 9 tiles baked, %.1f cm a frame."),
			R.Lead.x, R.Lead.y, Sx, Sy, Best, R.Tour);
	}
	const int32 Np = R.TourPts.Num();
	R.TourAlong += R.Tour;
	R.TourDist += R.Tour;
	for (int32 Guard = 0; Guard < Np; ++Guard)
	{
		const double Len = FVector2D::Distance(R.TourPts[R.TourLeg % Np], R.TourPts[(R.TourLeg + 1) % Np]);
		if (R.TourAlong <= Len)
		{
			break;
		}
		R.TourAlong -= Len;
		++R.TourLeg;
	}
	const FVector2D P0 = R.TourPts[R.TourLeg % Np], P1 = R.TourPts[(R.TourLeg + 1) % Np];
	const double Len = FMath::Max(FVector2D::Distance(P0, P1), 1.0);
	const FVector2D P = P0 + (P1 - P0) * (R.TourAlong / Len);
	FVector At(P.X, P.Y, Lead->GetActorLocation().Z);
	const float Half = Lead->GetSimpleCollisionHalfHeight();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(RainCoverTour), false, Lead);
	FHitResult Hit;
	if (World.LineTraceSingleByChannel(Hit, FVector(P.X, P.Y, At.Z - Half + 300.0), FVector(P.X, P.Y, At.Z - 20000.0), ECC_Visibility, Q))
	{
		At.Z = Hit.ImpactPoint.Z + Half + 2.0;
	}
	Lead->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
}

// -RainCoverGroundCount (dev only, with -RainCoverBakeOnly): what floor lies under the cover. For each baked texel with
// an occluder, a visibility trace (complex, static and dynamic) runs down from biasM + 2 cm under that occluder; the
// first thing it meets is a sheltered floor (the occluder stands more than biasM over it). Tallied as landscape or other,
// strip-wide on every 4th texel each way (each sample stands for 16 texels) and on every texel in the arcade box
// (engine x 300..340 m, y -45..-15 m). Logs; changes nothing.
void RingGroundCount(UWorld& World, const FFTRainRing& R, const ftcore::RainCoverSpec& Spec)
{
	const int32 Res = Spec.resolution, N = Res * Res;
	const double T0 = FPlatformTime::Seconds();
	constexpr float kAx0 = 300.f, kAx1 = 340.f, kAy0 = -45.f, kAy1 = -15.f; // engine m: plan x 800..840, y 515..545 (engine = plan x - 500, 500 - plan y)
	int64 Samples = 0, Covered = 0, Land[2] = {0, 0}, Other[2] = {0, 0}, None[2] = {0, 0};
	int32 TilesRead = 0;
	TArray<uint16> C;
	TArray<uint8> Bytes;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(RainCoverGround), true);
	for (int32 Ti = 0; Ti < R.Tiles.Num(); ++Ti)
	{
		const ftcore::RainCoverTileHeader& H = R.Head[Ti];
		if (!R.Have[Ti] || H.hits <= 0)
		{
			continue;
		}
		C.SetNumUninitialized(N, EAllowShrinking::No);
		if (H.res == 0)
		{
			ftcore::rainCoverTileFill(H, Res, C.GetData());
		}
		else if (!FFileHelper::LoadFileToArray(Bytes, *R.Path[Ti]) || Bytes.Num() != ftcore::kRainCoverFileHeader + R.PayloadBytes)
		{
			continue;
		}
		else
		{
			FMemory::Memcpy(C.GetData(), Bytes.GetData() + ftcore::kRainCoverFileHeader, R.PayloadBytes);
		}
		++TilesRead;
		const ftcore::RainCoverFrame F = ftcore::rainCoverTileFrame(Spec, R.Tiles[Ti], H.topZ);
		for (int32 Py = 0; Py < Res; ++Py)
		{
			const float X = F.cx + (0.5f - (float(Py) + 0.5f) / float(Res)) * F.sizeM;
			const bool bRowA = X >= kAx0 && X <= kAx1;
			for (int32 Px = 0; Px < Res; ++Px)
			{
				const float Y = F.cy + ((float(Px) + 0.5f) / float(Res) - 0.5f) * F.sizeM;
				const bool bA = bRowA && Y >= kAy0 && Y <= kAy1;
				const bool bS = (Px % 4) == 0 && (Py % 4) == 0;
				if (!bA && !bS)
				{
					continue;
				}
				float Tz = 0.f;
				Samples += bS ? 1 : 0;
				if (!ftcore::rainCoverR16Unpack(C[Py * Res + Px], H.range(), Tz))
				{
					continue; // no occluder: open
				}
				Covered += bS ? 1 : 0;
				// The first face up: a face down (the trace began inside a slab, or under a roof's own underside) is passed
				// through, three times at most.
				FHitResult Hit;
				double Start = (Tz - Spec.biasM - 0.02f) * 100.0;
				const double End = (Tz - ftcore::rainCoverHeaderFar(H.farM, Spec.depthM)) * 100.0;
				bool bHit = false;
				for (int32 K = 0; K < 4 && Start > End; ++K)
				{
					bHit = World.LineTraceSingleByChannel(Hit, FVector(X, Y, Start), FVector(X, Y, End), ECC_Visibility, Q);
					if (!bHit || Hit.ImpactNormal.Z > 0.2)
					{
						break;
					}
					Start = Hit.ImpactPoint.Z - 1.0;
					bHit = false;
				}
				int64* Into = !bHit ? None : (Cast<ALandscapeProxy>(Hit.GetActor()) ? Land : Other);
				Into[0] += bS ? 16 : 0;
				Into[1] += bA ? 1 : 0;
			}
		}
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover ground: %d tiles with hits read, %lld samples (every 4th texel each way), %lld under an occluder; sheltered floor in texels (each sample x16): landscape %lld (%.0f m2), other %lld (%.0f m2), nothing below %lld; arcade box x %.0f..%.0f y %.0f..%.0f m, every texel: landscape %lld (%.1f m2), other %lld, nothing below %lld; %.1f s."),
		TilesRead, Samples, Covered, Land[0], double(Land[0]) * ftcore::rainCoverTexel(Spec) * ftcore::rainCoverTexel(Spec), Other[0],
		double(Other[0]) * ftcore::rainCoverTexel(Spec) * ftcore::rainCoverTexel(Spec), None[0], kAx0, kAx1, kAy0, kAy1, Land[1],
		double(Land[1]) * ftcore::rainCoverTexel(Spec) * ftcore::rainCoverTexel(Spec), Other[1], None[1], FPlatformTime::Seconds() - T0);
}
#endif
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
		// A tile whose header has no hits takes the open slot vector, so its slot reads open whatever codes its atlas block
		// still holds.
		const ftcore::RainCoverSlot V = Ti >= 0 && R.Head[Ti].hits > 0 ? ftcore::rainCoverSlotFor(R.Tiles[Ti], R.Head[Ti].range()) : ftcore::RainCoverSlot{};
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
		// Ruling 12583 point 2 (interim guard): a tile is baked only outside Shipping and under the sealed Nanite raster
		// setting. A refused tile is baked and written nowhere and stays open: its stale header is dropped (no hits).
		const float Ppe = RingPixelsPerEdge();
		const bool bMayBake = ftcore::rainCoverMayBake(UE_BUILD_SHIPPING != 0, Ppe);
		int32 Match = 0, Stale = 0, Missing = 0, Refused = 0;
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
				if (bMayBake)
				{
					R.ToBake.Add(Ti);
				}
				else if (!R.Have[Ti])
				{
					R.Head[Ti] = ftcore::RainCoverTileHeader{};
					++Refused;
				}
			}
		}
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover ring key: %d components, %d instances; %d of %d tiles hold something; key %.1f ms; cache %s: %d match, %d stale, %d missing; %d to bake%s; %d plant tiles (a world-position offset material)."),
			R.Comps, R.Insts, R.Keyed, NT, KeyMs, *R.Dir, Match, Stale, Missing, R.ToBake.Num(), R.bRebake ? TEXT(" (rebake all)") : TEXT(""), R.Plant.Num());
#if !UE_BUILD_SHIPPING
		if (R.bLandFull && Match > 0 && !R.bRebake)
		{
			// Ruling 9035 item 5: a cached tile against a forced witness fails by construction, so the run is void.
			UE_LOG(LogForgottenThird, Error,
				TEXT("RainCover VOID: -RainCoverLandscapeFull with %d tiles from the cache (baked without it; the key leaves the flag out); the witness is off. Rebake on a copy: -RainCoverBakeDir=<copy> -RainCoverRebake."),
				Match);
			R.bWitness = false;
		}
#endif
		if (!bMayBake && (Refused > 0 || R.bRebake))
		{
			// Under -RainCoverBakeOnly or -RainCoverRebake a refusal ends the run as an error; otherwise it goes on with the
			// matching tiles.
			if (R.bBakeOnly || R.bRebake)
			{
				UE_LOG(LogForgottenThird, Error,
					TEXT("RainCover bake refused: r.Nanite.MaxPixelsPerEdge %.6f (the bake needs %.6f; -1: no such cvar), shipping %d; %d tiles left open; the run ends."),
					Ppe, ftcore::kRainCoverBakeMaxPixelsPerEdge, UE_BUILD_SHIPPING != 0 ? 1 : 0, Refused);
				R.State = FFTRainRing::Done;
				FTRequestExit(1, TEXT("RainCoverBakeRefused"));
				return;
			}
			UE_LOG(LogForgottenThird, Warning,
				TEXT("RainCover bake refused: r.Nanite.MaxPixelsPerEdge %.6f (the bake needs %.6f; -1: no such cvar), shipping %d; %d tiles left open; on with the matching tiles."),
				Ppe, ftcore::kRainCoverBakeMaxPixelsPerEdge, UE_BUILD_SHIPPING != 0 ? 1 : 0, Refused);
		}
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
		// A material still compiling draws as the engine's default (a first launch after a master rebuild): the bake waits
		// until every shader is in, at most kRingShaderWaitS, and logs the wait so a bake taken over the cap is named.
		if (GShaderCompilingManager && GShaderCompilingManager->GetNumRemainingJobs() > 0)
		{
			if (R.ShaderWaitT0 == 0.0)
			{
				R.ShaderWaitT0 = FPlatformTime::Seconds();
				UE_LOG(LogForgottenThird, Display, TEXT("RainCover bake: waiting for %d shader jobs before the bake."),
					GShaderCompilingManager->GetNumRemainingJobs());
			}
			if (FPlatformTime::Seconds() - R.ShaderWaitT0 < kRingShaderWaitS)
			{
				return;
			}
			UE_LOG(LogForgottenThird, Warning, TEXT("RainCover bake: %d shader jobs still compiling after %.0f s; baking anyway."),
				GShaderCompilingManager->GetNumRemainingJobs(), kRingShaderWaitS);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover bake: shader wait %.1f s."),
			R.ShaderWaitT0 == 0.0 ? 0.0 : FPlatformTime::Seconds() - R.ShaderWaitT0);
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
			float MaxZ = 0.f, MinZ = 0.f;
			for (const float D : R.Depth)
			{
				if (D < kRingFindFarM)
				{
					MaxZ = bAny ? FMath::Max(MaxZ, kRingFindTopM - D) : kRingFindTopM - D;
					MinZ = bAny ? FMath::Min(MinZ, kRingFindTopM - D) : kRingFindTopM - D;
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
				const float* const Bt = R.BoundsTop.Find(FIntPoint(R.Tiles[Ti].x, R.Tiles[Ti].y));
				// Ruling 12825 point 9 and ruling 17662: the far that reaches the find's lowest hit from the top raised by bounds,
				// as the float that meets the clamp's own cap (rainCoverBakeFar); the clamp, run with it, never binds and stays as
				// the guard.
				const float FarT = ftcore::rainCoverBakeFar(MaxZ, Bt != nullptr, Bt ? double(*Bt) : 0.0, MinZ, Spec.depthM, kRingMarginM);
				const ftcore::RainCoverTop Top = ftcore::rainCoverTopClamp(MaxZ, Bt != nullptr, Bt ? double(*Bt) : 0.0, MinZ, FarT, kRingMarginM);
				F.topZ = float(Top.topZ);
				R.Deep += FarT > Spec.depthM ? 1 : 0;
				R.Raised += Top.raisedByBounds ? 1 : 0;
				R.Clamped += Top.clampedByDepth ? 1 : 0;
				if (Top.raisedByBounds || Top.clampedByDepth)
				{
					UE_LOG(LogForgottenThird, Display, TEXT("RainCover: tile (%d,%d) top %.2f (find max %.2f, bounds %.2f, minZ %.2f): raised by bounds %s, clamped by depth %s."),
						R.Tiles[Ti].x, R.Tiles[Ti].y, F.topZ, MaxZ, Bt ? *Bt : 0.f, MinZ, Top.raisedByBounds ? TEXT("yes") : TEXT("no"),
						Top.clampedByDepth ? TEXT("yes") : TEXT("no"));
				}
#if !UE_BUILD_SHIPPING
				if (R.WitnessList.Contains(Ti))
				{
					RingLandLog(World, TEXT("bake"), R.Tiles[Ti].x, R.Tiles[Ti].y, F.cx * 100.0, F.cy * 100.0, Spec.sizeM * 50.0);
				}
#endif
				if (!RingDraw(*RingCapture, *RingTarget, F, FarT, R.Pixels, R.Depth))
				{
					continue;
				}
				ftcore::RainCoverR16Range Range;
				if (!ftcore::rainCoverR16Pack(R.Depth.GetData(), N, F.topZ, FarT, R.Codes.GetData(), Range, kRingTolM))
				{
					// Ruling 12825 point 9: a pack past the tolerance refuses the tile, as the guard does: its header is
					// cleared, it stays open for this run and nothing is written.
					UE_LOG(LogForgottenThird, Error, TEXT("RainCover pack refused: tile (%d,%d) range %.2f m packs at %.3f mm, over the %.1f mm tolerance; the tile stays open and is not written."),
						R.Tiles[Ti].x, R.Tiles[Ti].y, Range.maxZ - Range.minZ, ftcore::rainCoverR16Error(Range.maxZ - Range.minZ) * 1000.f, kRingTolM * 1000.f);
					R.Head[Ti] = ftcore::RainCoverTileHeader{};
					R.Have[Ti] = 0;
					++R.PackRefused;
					continue;
				}
				H.topZ = F.topZ;
				H.farM = FarT;
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
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover: %d tiles raised by bounds, %d clamped by depth, %d drawn past %.0f m (the per-tile far), %d refused by the pack."),
			R.Raised, R.Clamped, R.Deep, Spec.depthM, R.PackRefused);
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
		if (R.PackRefused > 0 && (R.bBakeOnly || R.bRebake))
		{
			// Under -RainCoverBakeOnly or -RainCoverRebake a pack refusal ends the run as an error, after the whole pass, so one
			// run names every refused tile.
			UE_LOG(LogForgottenThird, Error, TEXT("RainCover pack refused %d tiles over the %.1f mm tolerance; the run ends."), R.PackRefused, kRingTolM * 1000.f);
			R.State = FFTRainRing::Done;
			FTRequestExit(1, TEXT("RainCoverPackRefused"));
			return;
		}
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
		if (FParse::Param(FTLookSwitches(), TEXT("RainCoverGroundCount")))
		{
			RingGroundCount(World, R, Spec);
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
#if !UE_BUILD_SHIPPING
	if (R.Tour > 0.f)
	{
		RingTourStep(World, R, Spec);
	}
#endif

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
	// The corner (5) and jump (9) cells, on top of the normal ring (crossings, late tiles and the prefetch all run as
	// shipped): on every 5 m trigger, that many of the ring's own slot tiles, read off disk (unbuffered) since the last
	// hit, go up again into their own slots. A slot whose tile changed since its read is skipped, so the atlas stays true.
	const auto ForceTick = [&]()
	{
		for (int32 B = 0; B < 2; ++B)
		{
			if (R.bForceReading[B] && R.ForceRead[B].IsReady())
			{
				R.bForceReading[B] = false;
				const int32 Good = R.ForceRead[B].Get();
				++R.Reads;
				R.ReadFails += Good == R.ForceK[B] ? 0 : 1;
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
				// The next Force slots round the ring (from where the last read stopped), each with one file read: a slot
				// whose tile has a payload reads its own tile; an open or flat slot reads the next baked tile in turn (its
				// upload goes into an open slot, which its slot vector hides, or is the flat tile's own codes).
				if (R.Textured.Num() == 0)
				{
					for (int32 Ti = 0; Ti < NT; ++Ti)
					{
						if (R.HasPayload(Ti))
						{
							R.Textured.Add(Ti);
						}
					}
				}
				const TCHAR* Files[9] = {};
				int32 K = 0;
				for (int32 Step = 0; Step < 9 && K < R.Force && R.Textured.Num() > 0; ++Step)
				{
					const int32 S = (R.ForceRot + Step) % 9;
					const int32 Ti = R.SlotTi[S];
					R.ForceSlot[B][K] = S;
					R.ForceTi[B][K] = Ti;
					Files[K++] = *R.Path[R.HasPayload(Ti) ? Ti : R.Textured[R.PadNext++ % R.Textured.Num()]];
				}
				R.ForceRot = (R.ForceRot + R.Force) % 9;
				R.ForceK[B] = K;
				uint16* Into = R.ForceBuf[B];
				const int32 Bytes = R.PayloadBytes, Words = N;
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
		int32 Up = 0;
		{
			CSV_SCOPED_TIMING_STAT(RainCover, UploadEnqueue);
			for (int32 J = 0; J < R.ForceK[B]; ++J)
			{
				// A slot whose tile changed since its read is skipped. A late slot (open until its read lands, then
				// placed anew) and an empty one take the read tile behind their open slot vector.
				const int32 S = R.ForceSlot[B][J], Ti = R.ForceTi[B][J];
				if (R.SlotTi[S] != Ti)
				{
					++R.ForceStale;
					continue;
				}
				const bool bFlat = Ti >= 0 && R.Head[Ti].hits > 0 && !R.HasPayload(Ti);
				Upload(S, bFlat ? R.Full : R.ForceBuf[B] + SIZE_T(J) * N);
				R.ForcePad += R.HasPayload(Ti) ? 0 : 1;
				++Up;
			}
			R.ForceFence[B].BeginFence();
		}
		CSV_CUSTOM_STAT(RainCover, Upload, Up, ECsvCustomStatOp::Accumulate);
		CSV_CUSTOM_STAT(RainCover, ForceHit, Up, ECsvCustomStatOp::Set); // the hit marker: Force on a full hit
		R.ForceUp += Up;
		R.ForceFull += Up == R.Force ? 1 : 0;
		++R.ForceHits;
	};
	if (R.Force > 0)
	{
		ForceTick();
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
		CSV_CUSTOM_STAT(RainCover, Upload, R.Uploaded - Before, ECsvCustomStatOp::Accumulate); // the force cells add theirs the same tick
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
// texel through the Core mapping. Classes as the 3b probe's: agree within kRingTolM (2.5 mm); band (the tile's occluder over the square's
// top, which the draw can't see); below either far plane; fails: height, gained (tile only), lost (draw only). The meshes
// standing at the first failing texels are named. Exits when done.
// Ruling 11-26 item 2: the landscape components under a witness square, at a stage of its draws. In an ortho view the
// landscape's screen-size LOD has no distance term (ComputeBoundsScreenRadiusSquared: DistSqr * ProjMatrix.M[2][3] = 0), so
// the bake and witness frames (the same width) pick the same LOD; what can differ is the heightmap's resident mips, which
// the section's LOD bias follows (FetchHeightmapLODBiases) and the streamer sets from the game camera.
static void RingLandLog(UWorld& World, const TCHAR* Stage, int32 Sx, int32 Sy, double Cx, double Cy, double HalfCm)
{
	const FBox2D Sq(FVector2D(Cx - HalfCm, Cy - HalfCm), FVector2D(Cx + HalfCm, Cy + HalfCm));
	int32 Seen = 0;
	for (TObjectIterator<ULandscapeComponent> It; It; ++It)
	{
		const ULandscapeComponent* L = *It;
		if (L->GetWorld() != &World || !L->IsRegistered())
		{
			continue;
		}
		const FBox B = L->Bounds.GetBox();
		if (!Sq.Intersect(FBox2D(FVector2D(B.Min), FVector2D(B.Max))))
		{
			continue;
		}
		++Seen;
		const UTexture2D* H = L->GetHeightmap();
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover witness landscape (%d, %d) %s: %s | proxy %d | nanite %d | forced %d | bias %d | heightmap %s resident %d of %d mips"),
			Sx, Sy, Stage, *L->GetPathName(), L->SceneProxy ? 1 : 0, L->IsNaniteActive() ? 1 : 0, L->ForcedLOD, L->LODBias,
			H ? *H->GetName() : TEXT("none"), H ? H->GetNumResidentMips() : -1, H ? H->GetNumMips() : -1);
	}
	if (Seen == 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover witness landscape (%d, %d) %s: no landscape component under the square."), Sx, Sy, Stage);
	}
}

// Lower edges (m) of the witness's dz histogram bins; the last bin is open.
constexpr float kWDzEdges[8] = {0.002f, 0.005f, 0.01f, 0.03f, 0.1f, 0.3f, 1.f, 10.f};

void UFTRainCoverSubsystem::RingWitnessTick(UWorld& World)
{
	using namespace FTRainRingDetail;
	FFTRainRing& R = *Ring;
	const int32 Res = Spec.resolution, N = Res * Res;
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
		// Ruling 12583 point 2: the witness draws through the bake's capture (RingDraw), so its Nanite raster runs at the
		// live r.Nanite.MaxPixelsPerEdge; it takes the bake's gate, and a refusal ends the run as an error.
		const float Ppe = RingPixelsPerEdge();
		if (!ftcore::rainCoverMayBake(UE_BUILD_SHIPPING != 0, Ppe))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("RainCover witness refused: r.Nanite.MaxPixelsPerEdge %.6f (the bake's is %.6f; -1: no such cvar); the run ends."),
				Ppe, ftcore::kRainCoverBakeMaxPixelsPerEdge);
			R.bWitness = false;
			R.WitnessStep = 2;
			FTRequestExit(1, TEXT("RainCoverWitnessRefused"));
			return;
		}
		const TCHAR* Sw = FTLookSwitches();
		R.bWitnessLod0 = FParse::Param(Sw, TEXT("RainCoverTileLod0"));
		R.bWitnessNoCull = FParse::Param(Sw, TEXT("RainCoverTileNoCull"));
		RingSwitches(World, R, true, R.bWitnessLod0, R.bWitnessNoCull);
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover witness: %d squares (%d plant tiles, %d listed), %s draw, the wind frozen, settle %d ticks a square."),
			R.WitnessTiles.Num(), PlantSorted.Num(), R.WitnessList.Num(),
			R.bWitnessLod0 || R.bWitnessNoCull ? TEXT("the switched") : TEXT("the shipped-settings"), R.WitnessSettle);
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
			// The pre redraw: the bake's own frame (its top, and its far: the find's for a tile with no hits), drawn on
			// arrival, before the settle.
			const ftcore::RainCoverTileHeader& Hw = R.Head[Wt];
			const ftcore::RainCoverFrame Fb = ftcore::rainCoverTileFrame(Spec, R.Tiles[Wt], Hw.topZ);
			RingLandLog(World, TEXT("pre"), R.Tiles[Wt].x, R.Tiles[Wt].y, Tf.cx * 100.0, Tf.cy * 100.0, Spec.sizeM * 50.0);
			R.bPre = R.Have[Wt] && RingDraw(*RingCapture, *RingTarget, Fb, Hw.hits > 0 ? ftcore::rainCoverHeaderFar(Hw.farM, Spec.depthM) : kRingFindFarM, R.Pixels, R.DepthPre);
			R.WitnessWait = R.WitnessSettle;
			return;
		}
		if (R.WitnessWait-- > 0)
		{
			return;
		}
		R.WitnessWait = -1;
		++R.WitnessAt;
		const ftcore::RainCoverFrame F = ftcore::rainCoverFrameAt(Spec, float(TileCam.X) * 0.01f, float(TileCam.Y) * 0.01f, float(TileCam.Z) * 0.01f);
		// The witness's own far (ruling 12825 point 9): one per square, from the square's tile, so its draw reaches as deep as
		// that tile's bake did: depthM, or down to the tile's floor (its top less its far) from the witness's top.
		const ftcore::RainCoverTileHeader& Hs = R.Head[Wt];
		const float FarW = Hs.hits > 0 ? FMath::Max(Spec.depthM, F.topZ - (Hs.topZ - ftcore::rainCoverHeaderFar(Hs.farM, Spec.depthM))) : Spec.depthM;
		if (!RingDraw(*RingCapture, *RingTarget, F, FarW, R.Pixels, R.Depth))
		{
			++R.WNoDraw;
			return;
		}
		// The post redraw: the bake's own frame again, after the settle and the witness draw, into its own buffer.
		const ftcore::RainCoverTileHeader& Hb = R.Head[Wt];
		const ftcore::RainCoverFrame Fb = ftcore::rainCoverTileFrame(Spec, R.Tiles[Wt], Hb.topZ);
		const float FarB = Hb.hits > 0 ? ftcore::rainCoverHeaderFar(Hb.farM, Spec.depthM) : kRingFindFarM;
		RingLandLog(World, TEXT("post"), R.Tiles[Wt].x, R.Tiles[Wt].y, Fb.cx * 100.0, Fb.cy * 100.0, Spec.sizeM * 50.0);
		const bool bPost = R.Have[Wt] && RingDraw(*RingCapture, *RingTarget, Fb, FarB, R.Pixels, R.DepthPost);
		const bool bPre = R.bPre && R.DepthPre.Num() == N;
		if (!R.WitnessDump.IsEmpty())
		{
			// Witness heights in its own frame, then post and pre in the bake frame; NaN where nothing was hit.
			TArray<float> Out;
			Out.SetNumUninitialized(3 * N);
			for (int32 I = 0; I < N; ++I)
			{
				const float Dw = R.Depth[I];
				Out[I] = FMath::IsFinite(Dw) && Dw < FarW ? F.topZ - Dw : NAN;
				const float Dp = bPost ? R.DepthPost[I] : NAN;
				Out[N + I] = FMath::IsFinite(Dp) && Dp < FarB ? Fb.topZ - Dp : NAN;
				const float Dq = bPre ? R.DepthPre[I] : NAN;
				Out[2 * N + I] = FMath::IsFinite(Dq) && Dq < FarB ? Fb.topZ - Dq : NAN;
			}
			const FString File = FPaths::Combine(R.WitnessDump, FString::Printf(TEXT("sq_%d_%d.f32"), R.Tiles[Wt].x, R.Tiles[Wt].y));
			const bool bSaved = FFileHelper::SaveArrayToFile(TArrayView<const uint8>(reinterpret_cast<const uint8*>(Out.GetData()), Out.Num() * 4), *File);
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover witness dump (%d, %d): %s %s (res %d; witness top %.4f, bake top %.4f)."), R.Tiles[Wt].x,
				R.Tiles[Wt].y, bSaved ? TEXT("saved") : TEXT("FAILED"), *File, Res, F.topZ, Fb.topZ);
		}
		const auto BakeZ = [&](const TArray<float>& Dbuf, int32 I, bool& bHit) -> float
		{
			const float D = Dbuf[I];
			bHit = FMath::IsFinite(D) && D < FarB;
			return bHit ? Fb.topZ - D : NAN;
		};
		const int32 RowsAt = R.WRows.Num();
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
		FFTRainRing::FWSquare Square;
		Square.CamZ = float(TileCam.Z) * 0.01f;
		Square.TopZ = F.topZ;
		for (int32 Py = 0; Py < Res; ++Py)
		{
			for (int32 Px = 0; Px < Res; ++Px)
			{
				const float X = F.cx + (0.5f - (float(Py) + 0.5f) / float(Res)) * F.sizeM;
				const float Y = F.cy + ((float(Px) + 0.5f) / float(Res) - 0.5f) * F.sizeM;
				const float D = R.Depth[Py * Res + Px];
				const bool bR = FMath::IsFinite(D) && D < FarW;
				const float Rz = F.topZ - D;
				++R.WTexels;
				ftcore::RainCoverTile T;
				int32 Tx = 0, Ty = 0;
				const int32 Ti = ftcore::rainCoverTileTexel(Spec, X, Y, T, Tx, Ty) ? R.Find(T) : -1;
				bool bT = false;
				float Tz = 0.f, Floor = -1.0e9f;
				const TArray<uint16>* C = nullptr;
				const ftcore::RainCoverTileHeader* Hp = nullptr;
				if (Ti < 0 || !R.Have[Ti])
				{
					++R.WNoTile; // unlisted or unbaked: the ring reads it open
				}
				else
				{
					Hp = &R.Head[Ti];
					Floor = Hp->hits > 0 ? Hp->topZ - ftcore::rainCoverHeaderFar(Hp->farM, Spec.depthM) : kRingFindTopM - kRingFindFarM;
					C = Hp->hits > 0 ? CodesOf(Ti) : nullptr;
					bT = C && ftcore::rainCoverR16Unpack((*C)[Ty * Res + Tx], Hp->range(), Tz);
				}
				// Which texels of the same tile, one away in each of the 8 directions, agree with the draw: covered at the
				// draw's height (bR) or open (!bR). Bit k for (dx, dy) in the order (-1,-1) (0,-1) (1,-1) (-1,0) (1,0) (-1,1)
				// (0,1) (1,1), dx along the tile's columns (engine +Y), dy along its rows (engine -X). Texels past the tile's
				// edge are not read (the fail is marked as on the tile's border).
				static constexpr int32 kDx[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, kDy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
				const auto NeighbourMask = [&]() -> uint8
				{
					uint8 Mask = 0;
					for (int32 K = 0; K < 8; ++K)
					{
						const int32 Ox = Tx + kDx[K], Oy = Ty + kDy[K];
						if (Ox < 0 || Oy < 0 || Ox >= Res || Oy >= Res)
						{
							continue;
						}
						float Nz = 0.f;
						const bool bN = C && Hp && ftcore::rainCoverR16Unpack((*C)[Oy * Res + Ox], Hp->range(), Nz);
						if (bR ? (bN && FMath::Abs(Nz - Rz) <= kRingTolM) : !bN)
						{
							Mask |= uint8(1u << K);
						}
					}
					return Mask;
				};
				// The redraws against the tile at this texel, on the square's own tile only (the redraw's texel is the tile's
				// texel: same frame, same grid). Every texel counts, so a square with no fails is the control.
				const bool bOwn = Hp && Ti == Wt;
				const int32 Pix = Ty * Res + Tx;
				bool bPreHit = false, bPostHit = false;
				const float PreZ = bOwn && bPre ? BakeZ(R.DepthPre, Pix, bPreHit) : NAN;
				const float PostZ = bOwn && bPost ? BakeZ(R.DepthPost, Pix, bPostHit) : NAN;
				const bool bPreAgree = bOwn && bPre && (bT ? (bPreHit && FMath::Abs(PreZ - Tz) <= kRingTolM) : !bPreHit);
				const bool bPostAgree = bOwn && bPost && (bT ? (bPostHit && FMath::Abs(PostZ - Tz) <= kRingTolM) : !bPostHit);
				if (bOwn)
				{
					Square.PreOff += bPre && !bPreAgree ? 1 : 0;
					Square.PostOff += bPost && !bPostAgree ? 1 : 0;
					++Square.OwnTexels;
				}
				bool bFail = false;
				uint8 Side = 0;
				float FailDz = 0.f;
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
						Side = Tz > Rz ? 0 : 1;
						++R.WSide[Side];
						FailDz = Dz;
						if (Dz * 100.f > R.WMaxCm)
						{
							R.WMaxCm = Dz * 100.f;
							R.WWorstAt = FVector(X, Y, FMath::Max(Tz, Rz));
							R.WWorstSide = Side;
							R.WWorstSq = R.WitnessAt - 1;
						}
						bFail = true;
					}
				}
				else if (bT)
				{
					if (Tz > F.topZ)
					{
						++R.WBand;
					}
					else if (Tz < F.topZ - FarW)
					{
						++R.WBelow;
					}
					else
					{
						++R.WGained;
						Side = 2;
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
					Side = 3;
					bFail = true;
				}
				if (bFail)
				{
					++Fails;
					FFTRainRing::FWFail& W = R.WFailAt.AddDefaulted_GetRef();
					W.X = X;
					W.Y = Y;
					W.Z = bR && bT ? FMath::Max(Rz, Tz) : (bR ? Rz : Tz);
					W.Dz = FailDz;
					W.Side = Side;
					W.Sq = R.WitnessAt - 1;
					FFTRainRing::FWSquare& Sq = Square;
					++Sq.Side[Side];
					const uint8 Dirs = NeighbourMask();
					const bool bBorder = Tx == 0 || Ty == 0 || Tx == Res - 1 || Ty == Res - 1;
					Sq.TileEdge[Side] += bBorder ? 1 : 0;
					for (int32 K = 0; K < 8; ++K)
					{
						Sq.Dir[Side][K] += (Dirs >> K) & 1u;
					}
					Sq.Pre[Side] += bPreAgree ? 1 : 0;
					Sq.Post[Side] += bPostAgree ? 1 : 0;
					if (Side == 1 && bPostAgree)
					{
						++Sq.TopLow[Rz > Fb.topZ ? 0 : 1];
					}
					{
						FFTRainRing::FWRow& Row = R.WRows.AddDefaulted_GetRef();
						Row.Sq = R.WitnessAt - 1;
						Row.Px = int16(Px);
						Row.Py = int16(Py);
						Row.Tx = int16(Tx);
						Row.Ty = int16(Ty);
						Row.Side = Side;
						Row.Dirs = Dirs;
						Row.Flags = uint8((bBorder ? 1 : 0) | (C && (*C)[Ty * Res + Tx] == ftcore::kRainCoverR16Top ? 2 : 0) | (bPreAgree ? 4 : 0) |
							(bPostAgree ? 8 : 0) | (bOwn ? 16 : 0));
						Row.X = X;
						Row.Y = Y;
						Row.Tz = bT ? Tz : NAN;
						Row.Rz = bR ? Rz : NAN;
						Row.PreZ = PreZ;
						Row.PostZ = PostZ;
					}
					if (Dirs != 0)
					{
						++R.WEdge[Side];
						++Sq.Edge[Side];
					}
					else if (Side < 2 && FailDz * 100.f > R.WOffMaxCm)
					{
						R.WOffMaxCm = FailDz * 100.f;
						R.WOffWorstAt = FVector(X, Y, W.Z);
						R.WOffWorstSide = Side;
						R.WOffWorstSq = R.WitnessAt - 1;
					}
					if (Side < 2)
					{
						int32 Bin = 0;
						while (Bin < 7 && FailDz >= kWDzEdges[Bin + 1])
						{
							++Bin;
						}
						++Sq.DzHist[Side][Bin];
						Sq.DzSum[Side] += FailDz;
						Sq.DzMax[Side] = FMath::Max(Sq.DzMax[Side], FailDz);
					}
				}
			}
		}
		// Owners of the square's tile-higher fails, from the bake frame (8836 item 4). Candidates: every registered primitive
		// whose bounds (15 cm slack) hold a fail's (X, Y, tile height), the 32 holding the most. Each is drawn alone (the
		// show-only list) at the bake frame and owns the unowned fails it draws at the tile's height; an owner is then drawn
		// alone at the witness frame too. The capture's mode and list are put back after every draw.
		{
			TArray<int32> Fail0;
			FBox Area(ForceInit);
			for (int32 I = RowsAt; I < R.WRows.Num(); ++I)
			{
				const FFTRainRing::FWRow& Rw = R.WRows[I];
				if (Rw.Side == 0 && (Rw.Flags & 16) != 0)
				{
					Fail0.Add(I);
					Area += FVector(Rw.X, Rw.Y, Rw.Tz) * 100.0;
				}
			}
			if (Fail0.Num() > 0 && bPost)
			{
				constexpr double kSlackCm = 15.0;
				TArray<TPair<int32, UPrimitiveComponent*>> Cands;
				for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
				{
					UPrimitiveComponent* P = *It;
					if (P->GetWorld() != &World || !P->IsRegistered())
					{
						continue;
					}
					const FBox B = P->Bounds.GetBox().ExpandBy(kSlackCm);
					if (!B.Intersect(Area.ExpandBy(1.0)))
					{
						continue;
					}
					int32 Count = 0;
					for (const int32 I : Fail0)
					{
						const FFTRainRing::FWRow& Rw = R.WRows[I];
						Count += B.IsInsideOrOn(FVector(Rw.X, Rw.Y, Rw.Tz) * 100.0) ? 1 : 0;
					}
					if (Count > 0)
					{
						Cands.Emplace(Count, P);
					}
				}
				Cands.Sort([](const TPair<int32, UPrimitiveComponent*>& A, const TPair<int32, UPrimitiveComponent*>& B) { return A.Key > B.Key; });
				Square.Candidates = Cands.Num();
				USceneCaptureComponent2D* Cc = RingCapture->GetCaptureComponent2D();
				const ESceneCapturePrimitiveRenderMode Mode = Cc->PrimitiveRenderMode;
				const TArray<TWeakObjectPtr<UPrimitiveComponent>> Shown = Cc->ShowOnlyComponents;
				const auto Solo = [&](UPrimitiveComponent* P, const ftcore::RainCoverFrame& At, float Far) -> bool
				{
					Cc->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
					Cc->ShowOnlyComponents.Reset();
					Cc->ShowOnlyComponents.Add(P);
					const bool bOk = RingDraw(*RingCapture, *RingTarget, At, Far, R.Pixels, R.DepthSolo);
					Cc->PrimitiveRenderMode = Mode;
					Cc->ShowOnlyComponents = Shown;
					return bOk;
				};
				const FVector BakeCam = FVector(Fb.cx, Fb.cy, Fb.topZ) * 100.0;
				const FVector WitCam = FVector(F.cx, F.cy, F.topZ) * 100.0;
				const double Now = World.GetTimeSeconds();
				for (int32 K = 0; K < FMath::Min(Cands.Num(), 32); ++K)
				{
					UPrimitiveComponent* P = Cands[K].Value;
					const int32 OwnerId = R.WOwners.Num();
					int32 Hits = 0, Mine = 0, Drawn = 0;
					if (Solo(P, Fb, FarB))
					{
						for (const float D : R.DepthSolo)
						{
							Hits += FMath::IsFinite(D) && D < FarB ? 1 : 0;
						}
						for (const int32 I : Fail0)
						{
							FFTRainRing::FWRow& Rw = R.WRows[I];
							bool bHit = false;
							const float Z = Rw.Owner < 0 ? BakeZ(R.DepthSolo, Rw.Ty * Res + Rw.Tx, bHit) : NAN;
							if (bHit && FMath::Abs(Z - Rw.Tz) <= kRingTolM)
							{
								Rw.Owner = OwnerId;
								++Mine;
							}
						}
					}
					Square.EmptySolos += Hits == 0 ? 1 : 0;
					if (Mine == 0)
					{
						continue;
					}
					if (Solo(P, F, FarW))
					{
						for (const int32 I : Fail0)
						{
							const FFTRainRing::FWRow& Rw = R.WRows[I];
							const float D = R.DepthSolo[Rw.Py * Res + Rw.Px];
							Drawn += Rw.Owner == OwnerId && FMath::IsFinite(D) && D < FarW && FMath::Abs(F.topZ - D - Rw.Tz) <= kRingTolM ? 1 : 0;
						}
					}
					const AActor* O = P->GetOwner();
					const UStaticMeshComponent* Sm = Cast<UStaticMeshComponent>(P);
					const UStaticMesh* M = Sm ? Sm->GetStaticMesh() : nullptr;
					const FStaticMeshRenderData* Rd = M ? M->GetRenderData() : nullptr;
					const UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(P);
					const FBox Bx = P->Bounds.GetBox();
					const ULevel* Lv = P->GetComponentLevel();
					R.WOwners.Add(FString::Printf(TEXT("%s on %s"), *P->GetName(), O ? *O->GetName() : TEXT("-")));
					UE_LOG(LogForgottenThird, Display,
						TEXT("RainCover witness owner (%d, %d) #%d %s on %s (%s), mesh %s, level %s (visible %d), scene proxy %d: owns %d tile-higher fails (solo bake-frame hits %d), drawn alone in the witness frame at the tile's height %d of %d; registered %d visible %d hidden in game %d owner hidden %d in the capture's hidden actors %d hidden in captures %d captures only %d nanite %d forced LOD %d first LOD %d max draw %.0f cached %.0f min draw %.0f cull %d-%d; from the bake capture %.1f m, the witness capture %.1f m, the game camera %.1f m; on a player's screen %.1f s ago."),
						R.Tiles[Wt].x, R.Tiles[Wt].y, OwnerId, *P->GetName(), O ? *O->GetName() : TEXT("-"), *P->GetClass()->GetName(), M ? *M->GetName() : TEXT("-"),
						Lv ? *Lv->GetOuter()->GetName() : TEXT("-"), Lv && Lv->bIsVisible ? 1 : 0, P->SceneProxy ? 1 : 0, Mine, Hits, Drawn, Mine, P->IsRegistered() ? 1 : 0, P->IsVisible() ? 1 : 0, P->bHiddenInGame ? 1 : 0, O && O->IsHidden() ? 1 : 0,
						O && Cc->HiddenActors.Contains(O) ? 1 : 0, P->bHiddenInSceneCapture ? 1 : 0, P->bVisibleInSceneCaptureOnly ? 1 : 0,
						M && M->HasValidNaniteData() ? 1 : 0, Sm ? Sm->ForcedLodModel : -1, Rd ? int32(Rd->CurrentFirstLODIdx) : -1, P->LDMaxDrawDistance,
						P->CachedMaxDrawDistance, P->MinDrawDistance, Ism ? Ism->InstanceStartCullDistance : -1, Ism ? Ism->InstanceEndCullDistance : -1,
						FMath::Sqrt(Bx.ComputeSquaredDistanceToPoint(BakeCam)) * 0.01, FMath::Sqrt(Bx.ComputeSquaredDistanceToPoint(WitCam)) * 0.01,
						FMath::Sqrt(Bx.ComputeSquaredDistanceToPoint(TileCam)) * 0.01, Now - P->GetLastRenderTimeOnScreen());
				}
				for (const int32 I : Fail0)
				{
					(R.WRows[I].Owner >= 0 ? Square.Owned : Square.Unowned) += 1;
				}
			}
			else
			{
				Square.Unowned += Fail0.Num();
			}
		}
		R.WFails += Fails;
		R.WPreOff += Square.PreOff;
		R.WPostOff += Square.PostOff;
		if (Fails > 0)
		{
			R.WFailTile.Add(R.WitnessAt - 1, Square);
		}
		if (!(bPre && bPost))
		{
			++R.WNoRedraw; // a square without both redraws is no control, whatever its fails
		}
		else if (Fails == 0 && Square.OwnTexels > 0)
		{
			// A square with no fails: the control. Its redraws must equal its tile.
			++R.WCtrlSquares;
			R.WCtrlPreOff += Square.PreOff;
			R.WCtrlPostOff += Square.PostOff;
		}
		return;
	}

	// Done: every failing texel against the bounds of every registered static mesh (instances one by one, 15 cm slack),
	// tallied per mesh and per side; a texel counts once per component. Then the switches back, the report, exit.
	struct FMeshTally
	{
		int64 Side[4] = {0, 0, 0, 0};
		int32 Comps = 0;
		int64 BestFails = -1;
		TWeakObjectPtr<UStaticMeshComponent> First; // the component with the most failing texels in its bounds
	};
	TMap<FString, FMeshTally> Meshes;
	TBitArray<> Named(false, R.WFailAt.Num());
	if (R.WFailAt.Num() > 0)
	{
		constexpr double kSlackCm = 15.0;
		TMap<FIntPoint, TArray<int32>> Grid; // 1 m cells
		for (int32 I = 0; I < R.WFailAt.Num(); ++I)
		{
			Grid.FindOrAdd(FIntPoint(FMath::FloorToInt(R.WFailAt[I].X), FMath::FloorToInt(R.WFailAt[I].Y))).Add(I);
		}
		TArray<int32> Stamp;
		Stamp.Init(-1, R.WFailAt.Num());
		int32 CompId = 0;
		const auto Test = [&](const FBox& Bx, FMeshTally& T, int32 Id)
		{
			const double X0 = (Bx.Min.X - kSlackCm) * 0.01, X1 = (Bx.Max.X + kSlackCm) * 0.01;
			const double Y0 = (Bx.Min.Y - kSlackCm) * 0.01, Y1 = (Bx.Max.Y + kSlackCm) * 0.01;
			const double Z0 = (Bx.Min.Z - kSlackCm) * 0.01, Z1 = (Bx.Max.Z + kSlackCm) * 0.01;
			const auto Cell = [&](const TArray<int32>& In)
			{
				for (const int32 I : In)
				{
					const FFTRainRing::FWFail& W = R.WFailAt[I];
					if (Stamp[I] != Id && W.X >= X0 && W.X <= X1 && W.Y >= Y0 && W.Y <= Y1 && W.Z >= Z0 && W.Z <= Z1)
					{
						Stamp[I] = Id;
						++T.Side[W.Side];
						Named[I] = true;
					}
				}
			};
			const int64 Cells = int64(FMath::FloorToInt(X1) - FMath::FloorToInt(X0) + 1) * int64(FMath::FloorToInt(Y1) - FMath::FloorToInt(Y0) + 1);
			if (Cells > Grid.Num())
			{
				for (const TPair<FIntPoint, TArray<int32>>& G : Grid)
				{
					Cell(G.Value);
				}
				return;
			}
			for (int32 Gx = FMath::FloorToInt(X0); Gx <= FMath::FloorToInt(X1); ++Gx)
			{
				for (int32 Gy = FMath::FloorToInt(Y0); Gy <= FMath::FloorToInt(Y1); ++Gy)
				{
					if (const TArray<int32>* In = Grid.Find(FIntPoint(Gx, Gy)))
					{
						Cell(*In);
					}
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
			FMeshTally& T = Meshes.FindOrAdd(M->GetName());
			++T.Comps;
			const int64 Before = T.Side[0] + T.Side[1] + T.Side[2] + T.Side[3];
			const int32 Id = CompId++;
			if (const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C))
			{
				const FBox MB = M->GetBoundingBox();
				for (int32 K = 0; K < I->GetInstanceCount(); ++K)
				{
					FTransform X;
					if (I->GetInstanceTransform(K, X, true))
					{
						Test(MB.TransformBy(X), T, Id);
					}
				}
			}
			else
			{
				Test(C->Bounds.GetBox(), T, Id);
			}
			const int64 Mine = T.Side[0] + T.Side[1] + T.Side[2] + T.Side[3] - Before;
			if (Mine > T.BestFails)
			{
				T.BestFails = Mine;
				T.First = C;
			}
		}
	}
	RingSwitches(World, R, false);
	const auto Total = [](const FMeshTally& T) { return T.Side[0] + T.Side[1] + T.Side[2] + T.Side[3]; };
	Meshes.ValueSort([&](const FMeshTally& A, const FMeshTally& B) { return Total(A) > Total(B); });
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover witness (%s): %d squares (%d plant tiles), %d not drawn; %lld texels: agree %lld, band over the square's top %lld, below a far plane %lld, on no baked tile %lld; FAILS %lld (height %lld, worst %.1f cm; gained %lld; lost %lld)."),
		R.bWitnessLod0 || R.bWitnessNoCull ? TEXT("switched") : TEXT("shipped settings"), R.WitnessAt, R.Plant.Num(), R.WNoDraw, R.WTexels, R.WAgree,
		R.WBand, R.WBelow, R.WNoTile, R.WFails, R.WHeight, R.WMaxCm, R.WGained, R.WLost);
	if (R.WWorstSq >= 0)
	{
		const ftcore::RainCoverTile Wt = R.Tiles[R.WitnessTiles[R.WWorstSq]];
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover witness worst: %.1f cm, %s, square (%d, %d), at engine (%.2f, %.2f) m, the higher height %.2f m."), R.WMaxCm,
			R.WWorstSide == 0 ? TEXT("the tile higher") : TEXT("the draw higher"), Wt.x, Wt.y, R.WWorstAt.X, R.WWorstAt.Y, R.WWorstAt.Z);
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover witness edge: one texel of registration explains tile higher %lld of %lld, draw higher %lld of %lld, gained %lld of %lld, lost %lld of %lld; left over %lld."),
		R.WEdge[0], R.WSide[0], R.WEdge[1], R.WSide[1], R.WEdge[2], R.WGained, R.WEdge[3], R.WLost,
		R.WFails - R.WEdge[0] - R.WEdge[1] - R.WEdge[2] - R.WEdge[3]);
	if (R.WOffWorstSq >= 0)
	{
		const ftcore::RainCoverTile Wt = R.Tiles[R.WitnessTiles[R.WOffWorstSq]];
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover witness worst left over: %.1f cm, %s, square (%d, %d), at engine (%.2f, %.2f) m, the higher height %.2f m."), R.WOffMaxCm,
			R.WOffWorstSide == 0 ? TEXT("the tile higher") : TEXT("the draw higher"), Wt.x, Wt.y, R.WOffWorstAt.X, R.WOffWorstAt.Y, R.WOffWorstAt.Z);
	}
	// Per square: the camera's Z and the frame's top, the bake's top and range, and the fails by side
	// (tile higher / draw higher with mean and max dz; gained; lost).
	for (const TPair<int32, FFTRainRing::FWSquare>& P : R.WFailTile)
	{
		const int32 Ti = R.WitnessTiles[P.Key];
		const FFTRainRing::FWSquare& S = P.Value;
		const ftcore::RainCoverTileHeader& H = R.Head[Ti];
		const auto Mean = [&](int32 Sd) { return S.Side[Sd] > 0 ? S.DzSum[Sd] / double(S.Side[Sd]) : 0.0; };
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover witness square (%d, %d)%s: camera Z %.2f, frame top %.2f; bake top %.2f, min %.2f, max %.2f, %d hits; tile higher %lld (mean %.2f, max %.2f m), draw higher %lld (mean %.2f, max %.2f m), gained %lld, lost %lld; one texel explains %lld / %lld / %lld / %lld."),
			R.Tiles[Ti].x, R.Tiles[Ti].y, R.Plant.Contains(Ti) ? TEXT(" plant") : TEXT(""), S.CamZ, S.TopZ, H.topZ, H.minZ, H.maxZ, H.hits,
			S.Side[0], Mean(0), S.DzMax[0], S.Side[1], Mean(1), S.DzMax[1], S.Side[2], S.Side[3], S.Edge[0], S.Edge[1], S.Edge[2], S.Edge[3]);
		const auto Dirs = [&](int32 Sd)
		{
			return FString::Printf(TEXT("%lld %lld %lld %lld %lld %lld %lld %lld"), S.Dir[Sd][0], S.Dir[Sd][1], S.Dir[Sd][2], S.Dir[Sd][3], S.Dir[Sd][4],
				S.Dir[Sd][5], S.Dir[Sd][6], S.Dir[Sd][7]);
		};
		const auto Hist = [&](int32 Sd)
		{
			return FString::Printf(TEXT("%lld %lld %lld %lld %lld %lld %lld %lld"), S.DzHist[Sd][0], S.DzHist[Sd][1], S.DzHist[Sd][2], S.DzHist[Sd][3],
				S.DzHist[Sd][4], S.DzHist[Sd][5], S.DzHist[Sd][6], S.DzHist[Sd][7]);
		};
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover witness square (%d, %d) dz histogram (m; bins from 0.002, 0.005, 0.01, 0.03, 0.1, 0.3, 1, 10): tile higher [%s], draw higher [%s]."),
			R.Tiles[Ti].x, R.Tiles[Ti].y, *Hist(0), *Hist(1));
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover witness square (%d, %d) redraw at the bake frame: off the tile pre %lld, post %lld of %lld own texels; fails cleared pre %lld / %lld / %lld / %lld, post %lld / %lld / %lld / %lld; draw higher cleared post with the draw over the bake top %lld, not %lld; on the tile's border %lld / %lld / %lld / %lld; by direction (-1,-1) (0,-1) (1,-1) (-1,0) (1,0) (-1,1) (0,1) (1,1): tile higher [%s], draw higher [%s], gained [%s], lost [%s]; tile-higher owners: %lld candidates, %lld owned, %lld unowned, %lld solos drew nothing."),
			R.Tiles[Ti].x, R.Tiles[Ti].y, S.PreOff, S.PostOff, S.OwnTexels, S.Pre[0], S.Pre[1], S.Pre[2], S.Pre[3], S.Post[0], S.Post[1], S.Post[2],
			S.Post[3], S.TopLow[0], S.TopLow[1], S.TileEdge[0], S.TileEdge[1], S.TileEdge[2], S.TileEdge[3], *Dirs(0), *Dirs(1), *Dirs(2), *Dirs(3),
			S.Candidates, S.Owned, S.Unowned, S.EmptySolos);
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover witness redraw: off the tile pre %lld, post %lld in all squares; controls (squares with no fail and both redraws): %lld squares, off pre %lld, post %lld; squares without both redraws %lld."),
		R.WPreOff, R.WPostOff, R.WCtrlSquares, R.WCtrlPreOff, R.WCtrlPostOff, R.WNoRedraw);
	if (!R.WitnessCsv.IsEmpty())
	{
		// One row per failing texel. Side 0 tile higher, 1 draw higher, 2 gained, 3 lost; dirs: the 8-bit mask above;
		// flags: 1 tile border, 2 at the max code, 4 pre clears, 8 post clears, 16 on the square's own tile.
		FString Csv = TEXT("square_x,square_y,px,py,tx,ty,x,y,side,tile_z,witness_z,pre_z,post_z,bake_top,frame_top,dirs,flags,owner\n");
		const auto Z = [](float V) { return FMath::IsNaN(V) ? FString() : FString::Printf(TEXT("%.4f"), V); };
		for (const FFTRainRing::FWRow& Rw : R.WRows)
		{
			const int32 Ti = R.WitnessTiles[Rw.Sq];
			const FFTRainRing::FWSquare* S = R.WFailTile.Find(Rw.Sq);
			Csv += FString::Printf(TEXT("%d,%d,%d,%d,%d,%d,%.3f,%.3f,%d,%s,%s,%s,%s,%.4f,%.4f,%d,%d,\"%s\"\n"), R.Tiles[Ti].x, R.Tiles[Ti].y, Rw.Px, Rw.Py,
				Rw.Tx, Rw.Ty, Rw.X, Rw.Y, Rw.Side, *Z(Rw.Tz), *Z(Rw.Rz), *Z(Rw.PreZ), *Z(Rw.PostZ), R.Head[Ti].topZ, S ? S->TopZ : 0.f, Rw.Dirs,
				Rw.Flags, Rw.Owner >= 0 ? *R.WOwners[Rw.Owner] : TEXT(""));
		}
		const bool bSaved = FFileHelper::SaveStringToFile(Csv, *R.WitnessCsv, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover witness csv: %d rows to %s%s."), R.WRows.Num(), *R.WitnessCsv, bSaved ? TEXT("") : TEXT(" FAILED"));
	}
	int64 Unnamed[4] = {0, 0, 0, 0};
	for (int32 I = 0; I < R.WFailAt.Num(); ++I)
	{
		Unnamed[R.WFailAt[I].Side] += Named[I] ? 0 : 1;
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover witness: all %d failing texels against %d meshes' bounds; in no mesh's bounds: tile higher %lld, draw higher %lld, gained %lld, lost %lld."),
		R.WFailAt.Num(), Meshes.Num(), Unnamed[0], Unnamed[1], Unnamed[2], Unnamed[3]);
	// The top meshes, with what decides whether a game frame draws them (the flags of the component with the most failing
	// texels in its bounds). Counts are by bounds, so one texel can count for several meshes: not a partition. "On screen"
	// is the player's own view, not the witness square's.
	const double Now = World.GetTimeSeconds();
	int32 K = 0;
	for (const TPair<FString, FMeshTally>& P : Meshes)
	{
		// The top 24 with a fail, and every KeiCar mesh whatever its count.
		if (!((K++ < 24 && Total(P.Value) > 0) || P.Key.Contains(TEXT("KeiCar"))))
		{
			continue;
		}
		const FMeshTally& T = P.Value;
		const UStaticMeshComponent* C = T.First.Get();
		const AActor* O = C ? C->GetOwner() : nullptr;
		const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C);
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover witness mesh %s: tile higher %lld, draw higher %lld, gained %lld, lost %lld; %d components; most %lld in %s on %s (%s): visible %d, hidden in game %d, owner hidden %d, hidden in captures %d, main pass %d, captures only %d, owner only %d, forced LOD %d, min LOD %d, max draw %.0f cm, cull %d-%d, on the player's screen %.1f s ago."),
			*P.Key, T.Side[0], T.Side[1], T.Side[2], T.Side[3], T.Comps, T.BestFails, C ? *C->GetName() : TEXT("-"), O ? *O->GetName() : TEXT("-"),
			O ? *O->GetClass()->GetName() : TEXT("-"), C && C->IsVisible() ? 1 : 0, C && C->bHiddenInGame ? 1 : 0, O && O->IsHidden() ? 1 : 0,
			C && C->bHiddenInSceneCapture ? 1 : 0, C && C->bRenderInMainPass ? 1 : 0, C && C->bVisibleInSceneCaptureOnly ? 1 : 0,
			C && C->bOnlyOwnerSee ? 1 : 0, C ? C->ForcedLodModel : -1, C ? C->MinLOD : -1, C ? C->LDMaxDrawDistance : 0.f,
			I ? I->InstanceStartCullDistance : -1, I ? I->InstanceEndCullDistance : -1, C ? Now - C->GetLastRenderTimeOnScreen() : -1.0);
	}
	R.WitnessStep = 2;
	FPlatformMisc::RequestExit(false, TEXT("RainCoverWitness"));
}
#endif
