#include "FTRainCoverSubsystem.h"

#include "FTRainCoverCollection.h"
#include "ForgottenThird.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Math/OrthoMatrix.h"
#include "Misc/Parse.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/UObjectIterator.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "TextureResource.h"
#if !UE_BUILD_SHIPPING
#include "Camera/CameraActor.h"
#include "Engine/HitResult.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "FTWaterSubsystem.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Hash/CityHash.h"
#include "IO/IoHash.h"
#include "Materials/MaterialInterface.h"
#include "RenderCommandFence.h"
#include "UObject/Package.h"
#include <atomic>
#endif

#include "ftcore/LookTable.h"

#include <cmath>
#include <string_view>

CSV_DEFINE_CATEGORY(RainCover, true);

#if !UE_BUILD_SHIPPING
// 11-25 truth probe (ruling 7047 item 5, dev): the truth passes' and the strip's state; the streamed upload's.
struct FFTRainTruth
{
	struct FCand
	{
		FBox Box;
		TWeakObjectPtr<UStaticMeshComponent> Comp;
		int32 Inst = -1;
	};
	struct FSaved
	{
		TWeakObjectPtr<UStaticMeshComponent> Comp;
		float LD = 0.f, Cached = 0.f, MinDraw = 0.f;
		int32 Forced = 0, IStart = 0, IEnd = 0;
		bool bIsm = false;
	};
	// Per reference (tiles' own, then seams'), as drawn in state 3: depth m and the square.
	TArray<TArray<float>> RefD;
	TArray<ftcore::RainCoverFrame> RefF;
	TArray<FString> RefLabel;
	// Per tile: drawn with all three switches at the tile's own top.
	TArray<TArray<float>> TruthD;
	TArray<FCand> Cands;
	TMap<FIntPoint, TArray<int32>> CandByTile;
	TArray<FSaved> Saved;
	TArray<TWeakObjectPtr<AActor>> Hlod;
	int32 OldForceLod = -1, OldFoliageLod = -1, OldGpuLod = 1;
	int32 Pass = 0, Wait = 0;
	bool bSetUp = false, bStarted = false;
	// From the command line: switches held on every tile and reference draw (1 LOD0, 2 no cull, 4 no HLOD), the truth passes
	// to run (bit per pass; all and plain always run) and the settle ticks after a switch change.
	int32 Base = 0, Mask = 0x3F, Settle = 60;
	FString Csv;
	int32 CsvRows = 0;
	// Strip mode (5b, 5c).
	int32 StripAt = 0, FlatFull = 0, FlatPart = 0, Textured = 0, NoHit = 0;
	double BuildMs = 0.0, StripT0 = 0.0;
	int64 Bytes = 0;
	TArray<FVector> FlatAt; // the first flat-full tiles: centre and Z
};

struct FFTRainStream
{
	int32 K = 5;
	TArray<FString> Files;
	int32 Next = 0;
	TArray<uint16> Buf[2];
	FRenderCommandFence Fence[2];
	TFuture<int32> Read[2];
	bool bReading[2] = {false, false};
	int32 Ready = -1; // the buffer read and waiting for the next trigger
	int32 Last = 1;   // the buffer last uploaded
	int32 Late = 0, Busy = 0, Reads = 0, Short = 0;
	double ReadMsSum = 0.0, ReadMsMax = 0.0;
	std::atomic<int64> LastReadUs{0};
};
#endif

namespace
{
// What the depth capture draws. Without the trim it is the set before 11-23: it leaves out only what can't shelter. The
// trim also turns off what lights, shades or post-processes, none of which moves the nearest opaque surface.
void SetCoverShowFlags(FEngineShowFlags& F, bool bTrim)
{
	// Only what can shelter: opaque static and instanced meshes.
	F.SetSkeletalMeshes(false);
	F.SetParticles(false);
	F.SetTranslucency(false);
	F.SetDecals(false);
	F.SetFog(false);
	F.SetVolumetricFog(false);
	F.SetAtmosphere(false);
	F.SetLandscape(false);
	F.SetDynamicShadows(false);
	F.SetPostProcessing(false);
	if (!bTrim)
	{
		return;
	}
	// Grooms ride characters, like the skeletal meshes.
	F.SetHair(false);
	// Lighting, GI, reflections, AO and shadows. VolumetricLightmap and IndirectLightingCache stay on: with either off the depth
	// map changed in the 11-23 diff (75,258 and 75,272 texels over nine squares, up to 16.5 m). Lighting off moved no texel in that
	// diff (cell dl) and is re-gated on the build that ships it (TRIM_PATH1_1123); with it on the capture cost 0.3-0.6 ms more
	// render thread at its spike in one same-session pair (tt against tl, 11-23).
	F.SetLighting(false);
	F.SetDeferredLighting(false);
	F.SetGlobalIllumination(false);
	F.SetLumenGlobalIllumination(false);
	F.SetLumenReflections(false);
	F.SetReflectionEnvironment(false);
	F.SetScreenSpaceReflections(false);
	F.SetAmbientOcclusion(false);
	F.SetScreenSpaceAO(false);
	F.SetDistanceFieldAO(false);
	F.SetSkyLighting(false);
	F.SetAmbientCubemap(false);
	F.SetSpecular(false);
	F.SetMegaLights(false);
	F.SetContactShadows(false);
	F.SetCapsuleShadows(false);
	F.SetRayTracedDistanceFieldShadows(false);
	F.SetLightFunctions(false);
	F.SetTexturedLightProfiles(false);
	F.SetLightShafts(false);
	F.SetCloud(false);
	F.SetDeferredAtmospherePass(false);
	F.SetHeterogeneousVolumes(false);
	// Temporal and post: no history, so no velocities.
	F.SetTemporalAA(false);
	F.SetAntiAliasing(false);
	F.SetMotionBlur(false);
	F.SetBloom(false);
	F.SetEyeAdaptation(false);
	F.SetLocalExposure(false);
	F.SetDepthOfField(false);
	// Flat panels and text: no shelter.
	F.SetTextRender(false);
	F.SetWidgetComponents(false);
	F.SetMediaPlanes(false);
	F.SetPaper2DSprites(false);
}

// Straight down (pitch -90, yaw 0): the image's right runs along +Y, its down along -X (ftcore/RainCover.h's frame).
void SetCoverCapture(USceneCaptureComponent2D& C, UTextureRenderTarget2D* Into, const ftcore::RainCoverSpec& S, bool bTrim)
{
	C.ProjectionType = ECameraProjectionMode::Orthographic;
	C.OrthoWidth = S.sizeM * 100.f;
	// Depth measured from the capture itself (occluder Z = top Z - depth): no automatic plane shift.
	C.bAutoCalculateOrthoPlanes = false;
	C.bUpdateOrthoPlanes = false;
	C.CaptureSource = SCS_SceneDepth;
	C.TextureTarget = Into;
	C.bCaptureEveryFrame = false;
	C.bCaptureOnMovement = false;
	C.MaxViewDistanceOverride = S.depthM * 100.f;
	SetCoverShowFlags(C.ShowFlags, bTrim);
}
} // namespace

void FTRainCoverCaptureSettings(USceneCaptureComponent2D& C, UTextureRenderTarget2D* Into, const ftcore::RainCoverSpec& S)
{
	SetCoverCapture(C, Into, S, true);
}

bool UFTRainCoverSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) && Super::ShouldCreateSubsystem(Outer);
}

void UFTRainCoverSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	const FTCHARToUTF8 Map(*InWorld.GetMapName());
	if (ftcore::lookWorldForMap(std::string_view(Map.Get(), static_cast<size_t>(Map.Length()))) != "solhaven")
	{
		return;
	}
	if (!FTRainCover::Enabled())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover: off (on only with -RainCover); rain falls and soaks everywhere."));
		return;
	}
	Target = FTRainCover::LoadTarget();
	if (!Target || !FTRainCover::Params(InWorld))
	{
		UE_LOG(LogForgottenThird, Warning,
			TEXT("RainCover: off, %s is missing (build it with -run=FTBuildVfx -Only=rain, or FTBuildSurfaces)."),
			Target ? FTRainCover::Collection : *FTRainCover::TargetPackage());
		return;
	}

	// The resident ring ships (FTRainCoverRing.cpp). The moving capture below is dev only: -RainCoverLegacy, or a probe or
	// cell built on it. A shipping build never spawns it.
	bool bLegacy = false;
#if !UE_BUILD_SHIPPING
	{
		const TCHAR* Sw = FTLookSwitches();
		FString Unused;
		bLegacy = FParse::Param(Sw, TEXT("RainCoverLegacy")) || FParse::Value(Sw, TEXT("RainCoverTileProbe="), Unused) ||
			FParse::Param(Sw, TEXT("RainCoverUploadProbe")) || FParse::Param(Sw, TEXT("RainCoverDiff")) || FParse::Param(Sw, TEXT("RainCoverSplit")) ||
			FParse::Param(Sw, TEXT("RainCoverProbe"));
	}
#endif
	if (!bLegacy)
	{
		bActive = RingBegin(InWorld);
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover: %s (the resident ring, %.0f m tiles at %d)."), bActive ? TEXT("on") : TEXT("off"), Spec.sizeM,
			Spec.resolution);
		return;
	}

	FActorSpawnParameters Params;
	Params.ObjectFlags = RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Capture = InWorld.SpawnActor<ASceneCapture2D>(FVector::ZeroVector, FRotator(-90.f, 0.f, 0.f), Params);
	USceneCaptureComponent2D* C = Capture ? Capture->GetCaptureComponent2D() : nullptr;
	if (!C)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("RainCover: off, couldn't spawn the capture."));
		return;
	}
	bool bTrim = true;
#if !UE_BUILD_SHIPPING
	const TCHAR* Switches = FTLookSwitches();
	bTrim = !FParse::Param(Switches, TEXT("RainCoverUntrimmed"));
	C->bAlwaysPersistRenderingState = FParse::Param(Switches, TEXT("RainCoverPersist"));
	C->bRenderInMainRenderer = FParse::Param(Switches, TEXT("RainCoverMainRenderer"));
	bDiff = FParse::Param(Switches, TEXT("RainCoverDiff"));
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover levers: trim %d, persist %d, main renderer %d, diff %d."), bTrim ? 1 : 0,
		C->bAlwaysPersistRenderingState ? 1 : 0, C->bRenderInMainRenderer ? 1 : 0, bDiff ? 1 : 0);
#endif
	SetCoverCapture(*C, Target, Spec, bTrim);
#if !UE_BUILD_SHIPPING
	// Path-1 draw cuts (dev, 11-23), on this capture only (never the diff reference), one lever each:
	// -RainCoverMaxDist=<m> shortens the far plane below Spec.depthM; -RainCoverLOD=<f> sets LODDistanceFactor;
	// -RainCoverHideSmall=<cm> hides every static mesh component whose bounds' largest extent is below that, listed once here.
	float CutM = 0.f, CutLod = 0.f, CutCm = 0.f;
	if (FParse::Value(Switches, TEXT("RainCoverMaxDist="), CutM) && CutM > 0.f)
	{
		C->MaxViewDistanceOverride = CutM * 100.f;
	}
	if (FParse::Value(Switches, TEXT("RainCoverLOD="), CutLod) && CutLod > 0.f)
	{
		C->LODDistanceFactor = CutLod;
	}
	int32 Hidden = 0;
	if (FParse::Value(Switches, TEXT("RainCoverHideSmall="), CutCm) && CutCm > 0.f)
	{
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* M = *It;
			if (M->GetWorld() == &InWorld && M->IsRegistered() && M->Bounds.BoxExtent.GetMax() * 2.0 < CutCm)
			{
				C->HideComponent(M);
				++Hidden;
			}
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover cuts: far %.0f m, LOD factor %.2f, hide below %.0f cm (%d components)."),
		C->MaxViewDistanceOverride * 0.01f, C->LODDistanceFactor, CutCm, Hidden);
	// -RainCoverShowFlags="Name=0,Name=1": a dev lever over single show flags on top of the set above, to find what moves the map.
	FString Flags;
	if (FParse::Value(Switches, TEXT("RainCoverShowFlags="), Flags, false))
	{
		const bool bSet = C->ShowFlags.SetFromString(*Flags);
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover levers: show flags \"%s\" %s; Lighting %d, VolumetricLightmap %d, IndirectLightingCache %d."),
			*Flags, bSet ? TEXT("set") : TEXT("not set"), C->ShowFlags.Lighting ? 1 : 0, C->ShowFlags.VolumetricLightmap ? 1 : 0,
			C->ShowFlags.IndirectLightingCache ? 1 : 0);
	}
#endif

	// Path 2 (11-25, dropped): a recapture split over three ticks (FTRainCoverSubsystem.h) is a dev switch only,
	// -RainCoverSplit; the whole capture is the default. After the dev cuts so the halves take them.
	bool bWantSplit = false;
#if !UE_BUILD_SHIPPING
	// With the main renderer each half lands in its tick's main render, before the copy tick's command; only the diff,
	// which runs both halves and the copy in one tick, keeps it whole.
	bWantSplit = FParse::Param(Switches, TEXT("RainCoverSplit")) && !(C->bRenderInMainRenderer && bDiff);
#endif
	bSplit = bWantSplit && SplitSetup(*C);
#if !UE_BUILD_SHIPPING
	// Path 3b probe (dev): the tile list, or the upload cell's transient tiles.
	FString TileFile;
	if (FParse::Value(Switches, TEXT("RainCoverTileProbe="), TileFile))
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *TileFile);
		for (const FString& Line : Lines)
		{
			TArray<FString> W;
			Line.TrimStartAndEnd().ParseIntoArrayWS(W);
			if (W.Num() == 3 && W[0] == TEXT("t"))
			{
				const ftcore::RainCoverTile T{FCString::Atoi(*W[1]), FCString::Atoi(*W[2])};
				if (!TileIndex.Contains(FIntPoint(T.x, T.y)))
				{
					TileIndex.Add(FIntPoint(T.x, T.y), TileList.Num());
					TileList.Add(T);
				}
			}
			else if (W.Num() == 3 && W[0] == TEXT("s"))
			{
				SeamList.Add(FVector2f(FCString::Atof(*W[1]), FCString::Atof(*W[2])));
			}
		}
		bTiles = TileList.Num() > 0;
		bTileStrip = bTiles && FParse::Param(Switches, TEXT("RainCoverTileStrip"));
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover tiles: probe %s, %d tiles and %d seam points from %s; %s."),
			bTiles ? TEXT("on") : TEXT("off"), TileList.Num(), SeamList.Num(), *TileFile,
			bTileStrip ? TEXT("strip mode (flat prune, key cost)") : TEXT("tile probe, then the truth passes"));
	}
	bUpload = FParse::Param(Switches, TEXT("RainCoverUploadProbe"));
	int32 StreamK = 0;
	if (bUpload && FParse::Value(Switches, TEXT("RainCoverUploadTiles="), StreamK) && StreamK > 0)
	{
		// 5d: K R16 tiles per crossing, streamed from the strip's cache off the game thread ahead of it (textured tiles only).
		const int32 Res = Spec.resolution, N = Res * Res;
		Stream = MakeShared<FFTRainStream>();
		Stream->K = StreamK;
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RainCoverTiles"));
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / TEXT("tile_*.r16")), true, false);
		Names.Sort();
		for (const FString& Nm : Names)
		{
			const FString Path = Dir / Nm;
			TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*Path));
			if (!Ar || Ar->TotalSize() != 32 + int64(N) * 2)
			{
				continue;
			}
			uint8 Head[32];
			Ar->Serialize(Head, 32);
			float Z[3];
			FMemory::Memcpy(Z, Head + 12, 12);
			if (Z[2] - Z[1] > 0.002f)
			{
				Stream->Files.Add(Path);
			}
		}
		for (int32 B = 0; B < 2; ++B)
		{
			Stream->Buf[B].SetNumZeroed(StreamK * N);
		}
		for (int32 K = 0; K < StreamK; ++K)
		{
			UTexture2D* T = UTexture2D::CreateTransient(Res, Res, PF_G16);
			if (T)
			{
				T->SRGB = false;
				T->UpdateResource();
				UploadTex.Add(T);
			}
		}
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover stream probe: on, no scene capture; %d textured tiles in %s; %d R16 tiles (%.1f MiB) per %.1f m move, read off the game thread ahead of it."),
			Stream->Files.Num(), *Dir, UploadTex.Num(), float(StreamK * N * 2) / (1024.f * 1024.f), Spec.recaptureM);
		if (Stream->Files.Num() == 0)
		{
			Stream.Reset();
			bUpload = false;
		}
	}
	else if (bUpload)
	{
		const int32 Res = Spec.resolution, N = Res * Res;
		UploadSrc.SetNumUninitialized(3 * N);
		for (int32 I = 0; I < UploadSrc.Num(); ++I)
		{
			UploadSrc[I] = 100.f + float(I % 9973); // any depths: the copy's cost doesn't depend on them
		}
		for (int32 K = 0; K < 3; ++K)
		{
			UTexture2D* T = UTexture2D::CreateTransient(Res, Res, PF_R32_FLOAT);
			if (T)
			{
				T->SRGB = false;
				T->UpdateResource();
				UploadTex.Add(T);
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover upload probe: on, no scene capture; %d tiles of %d x %d R32F (%.1f MiB) per %.1f m move."),
			UploadTex.Num(), Res, Res, float(UploadTex.Num() * N * 4) / (1024.f * 1024.f), Spec.recaptureM);
	}
#endif

	bProbe = FParse::Param(FTLookSwitches(), TEXT("RainCoverProbe"));
	bActive = true;
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover: on, %.0f m at %d, recapture every %.1f m, %s (LOD factor %.2f)."), Spec.sizeM,
		Spec.resolution, Spec.recaptureM, bSplit ? TEXT("split over three ticks") : TEXT("whole in one tick"), C->LODDistanceFactor);
}

bool UFTRainCoverSubsystem::SplitSetup(USceneCaptureComponent2D& C)
{
	const int32 Res = Spec.resolution;
	if (Res < 2 || Res % 2 != 0)
	{
		return false;
	}
	const auto Make = [this, Res]
	{
		UTextureRenderTarget2D* T = NewObject<UTextureRenderTarget2D>(this);
		T->ClearColor = Target->ClearColor;
		T->InitCustomFormat(Res, Res / 2, Target->GetFormat(), true);
		T->UpdateResourceImmediate(true);
		return T;
	};
	HalfPlus = Make();
	HalfMinus = Make();
	// As the engine builds this capture's own matrix (SceneCaptureRendering BuildOrthoMatrix: near 0, far the capture's
	// MaxViewDistanceOverride, the same float steps), with half the height: view y 0..HalfW (the image's top, +X) or
	// -HalfW..0 (-X) fills the half target, so its texels sit where the whole capture's rows 0..res/2-1 or res/2..res-1 do.
	const float HalfW = C.OrthoWidth / 2.0f;
	const float Far = C.MaxViewDistanceOverride > 0.0f ? C.MaxViewDistanceOverride : UE_FLOAT_HUGE_DISTANCE / 4.0f;
	const float ZScale = 1.0f / FMath::Max(Far - 0.0f, KINDA_SMALL_NUMBER);
	for (int32 H = 0; H < 2; ++H)
	{
		FMatrix M = FReversedZOrthoMatrix(HalfW, HalfW * 0.5f, ZScale, 0.0f);
		M.M[3][1] = H == 0 ? -1.0 : 1.0; // clip y = view y / (HalfW / 2) + M[3][1]
		HalfProjection[H] = M;
	}
	// The LOD code reads max(M[0][0], M[1][1]); half the height doubles it, so the factor doubles to pick the same LODs.
	C.LODDistanceFactor *= 2.0f;
	return true;
}

void UFTRainCoverSubsystem::ShootHalf(int32 Half)
{
	USceneCaptureComponent2D* C = Capture->GetCaptureComponent2D();
	C->TextureTarget = Half == 0 ? HalfPlus : HalfMinus;
	C->CustomProjectionMatrix = HalfProjection[Half];
	C->bUseCustomProjectionMatrix = true;
	CSV_CUSTOM_STAT(RainCover, Capture, Half + 1, ECsvCustomStatOp::Set);
	CSV_SCOPED_TIMING_STAT(RainCover, CaptureScene);
	C->CaptureScene();
}

void UFTRainCoverSubsystem::Join()
{
	CSV_CUSTOM_STAT(RainCover, Capture, 3, ECsvCustomStatOp::Set);
	FTextureRenderTargetResource* Plus = HalfPlus->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* Minus = HalfMinus->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* Into = Target->GameThread_GetRenderTargetResource();
	if (!Plus || !Minus || !Into)
	{
		return;
	}
	const FIntPoint Size(Spec.resolution, Spec.resolution / 2);
	ENQUEUE_RENDER_COMMAND(FTRainCoverJoin)([Plus, Minus, Into, Size](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		const FRDGTextureRef To = RegisterExternalTexture(GraphBuilder, Into->GetRenderTargetTexture(), TEXT("RainCover"));
		AddCopyTexturePass(GraphBuilder, RegisterExternalTexture(GraphBuilder, Plus->GetRenderTargetTexture(), TEXT("RainCoverPlusX")), To,
			FIntPoint::ZeroValue, FIntPoint::ZeroValue, Size);
		AddCopyTexturePass(GraphBuilder, RegisterExternalTexture(GraphBuilder, Minus->GetRenderTargetTexture(), TEXT("RainCoverMinusX")), To,
			FIntPoint::ZeroValue, FIntPoint(0, Size.Y), Size);
		GraphBuilder.Execute();
	});
}

void UFTRainCoverSubsystem::ShootNow()
{
	if (!bSplit)
	{
		Shoot(*Capture);
		return;
	}
	ShootHalf(0);
	ShootHalf(1);
	Join();
}

void UFTRainCoverSubsystem::Publish(UWorld& World)
{
	bHaveCapture = true;
	++Captures;
	if (UMaterialParameterCollectionInstance* P = FTRainCover::Params(World))
	{
		P->SetVectorParameterValue(FTRainCover::Frame(),
			FLinearColor(Frame.cx * 100.f, Frame.cy * 100.f, Frame.sizeM * 100.f, Frame.topZ * 100.f));
		P->SetScalarParameterValue(FTRainCover::On(), 1.f);
	}
}

void UFTRainCoverSubsystem::Deinitialize()
{
	if (bActive && !Ring)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover: %d captures."), Captures);
	}
	RingEnd();
#if !UE_BUILD_SHIPPING
	if (Stream)
	{
		for (int32 B = 0; B < 2; ++B)
		{
			if (Stream->bReading[B])
			{
				Stream->Read[B].Wait();
			}
		}
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover stream probe: %d tiles per crossing: %d reads (mean %.2f ms, max %.2f ms off the game thread, %d short), %d crossings late (read not back, skipped), %d ticks a read waited on a fence."),
			Stream->K, Stream->Reads, Stream->Reads ? Stream->ReadMsSum / Stream->Reads : 0.0, Stream->ReadMsMax, Stream->Short, Stream->Late, Stream->Busy);
	}
	if (bUpload)
	{
		FlushRenderingCommands(); // the uploads read UploadSrc
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover upload probe: %d uploads."), Uploads);
	}
#endif
	bActive = false;
	Super::Deinitialize();
}

void UFTRainCoverSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UWorld* World = GetWorld();
	if (!World || (!Capture && !Ring))
	{
		return;
	}
	const APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
	if (Ring)
	{
		RingTick(*World, Eye);
		return;
	}
	if (bProbe && Captures == 1)
	{
		Probe(Eye); // the frame after the first capture: it has rendered
	}
#if !UE_BUILD_SHIPPING
	if (bUpload) // the upload cell: no scene capture
	{
		if (Stream)
		{
			StreamTick(Eye);
		}
		else
		{
			UploadTick(Eye);
		}
		return;
	}
	if (bTiles && bHaveCapture) // the tile probe: the shipped recapture waits
	{
		TileTick(*World);
		return;
	}
#endif
	if (SplitStep > 0) // a split recapture goes on: the -X half, then the copy and the new frame together
	{
		if (SplitStep == 1)
		{
			ShootHalf(1);
			SplitStep = 2;
			return;
		}
		Join();
		SplitStep = 0;
		Frame = Pending;
		Publish(*World);
		return;
	}
#if !UE_BUILD_SHIPPING
	if (bDiff && bHaveCapture)
	{
		if (!RefCapture && ++DiffTicks >= 600) // past the warm-up
		{
			DiffStart(*World);
		}
		if (RefCapture)
		{
			DiffTick();
			return;
		}
	}
#endif
	if (!ftcore::rainCoverRecapture(Spec, bHaveCapture, Frame.cx, Frame.cy, float(Eye.X) * 0.01f, float(Eye.Y) * 0.01f))
	{
		return;
	}
	const ftcore::RainCoverFrame Next = ftcore::rainCoverFrameAt(Spec, float(Eye.X) * 0.01f, float(Eye.Y) * 0.01f, float(Eye.Z) * 0.01f);
	Capture->SetActorLocation(FVector(Next.cx, Next.cy, Next.topZ) * 100.0);
	if (bSplit)
	{
		Pending = Next; // the map keeps the old square until the copy tick
		ShootHalf(0);
		SplitStep = 1;
		return;
	}
	Frame = Next;
	Shoot(*Capture);
	Publish(*World);
}

void UFTRainCoverSubsystem::Probe(const FVector& Eye)
{
	bProbe = false;
	FTextureRenderTargetResource* Resource = Target ? Target->GameThread_GetRenderTargetResource() : nullptr;
	TArray<FLinearColor> Pixels;
	if (!Resource || !Resource->ReadLinearColorPixels(Pixels) || Pixels.Num() != Spec.resolution * Spec.resolution)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("RainCover probe: no readback."));
		return;
	}
	// The occluder's Z (m) over a plan point, or none (open).
	auto OccluderAt = [&](float X, float Y, float& Z)
	{
		float U = 0.f, V = 0.f;
		if (!ftcore::rainCoverUV(Frame, X, Y, U, V))
		{
			return false;
		}
		const int32 Px = FMath::Clamp(int32(U * Spec.resolution), 0, Spec.resolution - 1);
		const int32 Py = FMath::Clamp(int32(V * Spec.resolution), 0, Spec.resolution - 1);
		const float Depth = Pixels[Py * Spec.resolution + Px].R * 0.01f;
		if (!(Depth < Spec.depthM))
		{
			return false;
		}
		Z = Frame.topZ - Depth;
		return true;
	};
	const float Ex = float(Eye.X) * 0.01f, Ey = float(Eye.Y) * 0.01f, Ez = float(Eye.Z) * 0.01f;
	const auto Say = [&](const TCHAR* Where, float X, float Y)
	{
		float Z = 0.f;
		if (OccluderAt(X, Y, Z))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover probe: %s (%.1f, %.1f) occluder at %.2f m (eye %.2f m)."), Where, X, Y, Z, Ez);
		}
		else
		{
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover probe: %s (%.1f, %.1f) open."), Where, X, Y);
		}
	};
	Say(TEXT("eye"), Ex, Ey);
	Say(TEXT("+4 m X"), Ex + 4.f, Ey);
	Say(TEXT("+4 m Y"), Ex, Ey + 4.f);
	// Covered share: texels whose occluder stands over the eye (a roof, an awning, a deck over the camera's level).
	int32 Over = 0;
	for (const FLinearColor& P : Pixels)
	{
		const float Depth = P.R * 0.01f;
		Over += (Depth < Spec.depthM && Frame.topZ - Depth > Ez) ? 1 : 0;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover probe: %.1f%% of the %.0f m square is covered over the eye."),
		100.f * float(Over) / float(Pixels.Num()), Spec.sizeM);
}

void UFTRainCoverSubsystem::Shoot(ASceneCapture2D& With)
{
	CSV_CUSTOM_STAT(RainCover, Capture, 1, ECsvCustomStatOp::Set);
	CSV_SCOPED_TIMING_STAT(RainCover, CaptureScene);
	With.GetCaptureComponent2D()->CaptureScene();
}

#if !UE_BUILD_SHIPPING
void UFTRainCoverSubsystem::DiffStart(UWorld& World)
{
	RefTarget = NewObject<UTextureRenderTarget2D>(this);
	RefTarget->ClearColor = Target->ClearColor;
	RefTarget->InitCustomFormat(Spec.resolution, Spec.resolution, PF_R32_FLOAT, true);
	RefTarget->UpdateResourceImmediate(true);
	FActorSpawnParameters Params;
	Params.ObjectFlags = RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	RefCapture = World.SpawnActor<ASceneCapture2D>(FVector::ZeroVector, FRotator(-90.f, 0.f, 0.f), Params);
	if (!RefCapture || !RefCapture->GetCaptureComponent2D())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("RainCover diff: couldn't spawn the reference capture."));
		RefCapture = nullptr;
		bDiff = false;
		return;
	}
	// The reference is the map as it was before 11-23: untrimmed, no persist, its own renderer.
	SetCoverCapture(*RefCapture->GetCaptureComponent2D(), RefTarget, Spec, false);
	DiffOrigin = Frame;
	DiffSquare = DiffStep = DiffWait = 0;
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover diff: from (%.1f, %.1f), top %.2f m, 9 squares %.0f m apart, plus one on the pent eaves at (400, 0)."), DiffOrigin.cx,
		DiffOrigin.cy, DiffOrigin.topZ, Spec.sizeM);
}

void UFTRainCoverSubsystem::DiffTick()
{
	if (DiffWait > 0)
	{
		--DiffWait; // a main-renderer capture lands with the next scene render, so reads wait two ticks
		return;
	}
	const auto Place = [this]
	{
		const FVector At = FVector(DiffAt.cx, DiffAt.cy, DiffAt.topZ) * 100.0;
		Capture->SetActorLocation(At);
		RefCapture->SetActorLocation(At);
	};
	const auto ShootBoth = [this]
	{
		Shoot(*RefCapture);
		ShootNow(); // as shipped: both halves and the copy when split
	};
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("RainCoverDiff");
	switch (DiffStep)
	{
	case 0: // a fresh square: both captures moved there and drawn once
		DiffAt = DiffOrigin;
		DiffAt.cx += float(DiffSquare % 3 - 1) * Spec.sizeM;
		DiffAt.cy += float(DiffSquare / 3 - 1) * Spec.sizeM;
		if (DiffSquare == 9)
		{
			// the tenth square: centred on the pent eaves (EAVE_PENT, tops 7.4-8.1 m) of the strip's sq1/sq4, plan (900, 528)
			DiffAt.cx = 400.f;
			DiffAt.cy = 0.f;
		}
		Place();
		ShootBoth();
		DiffWait = 2;
		DiffStep = 1;
		break;
	case 1:
		if (DiffRead(RefTarget, DiffRef) && DiffRead(Target, DiffVar))
		{
			DiffCompare(*FString::Printf(TEXT("square %d first"), DiffSquare), DiffRef, DiffVar, FString());
		}
		DiffSettle = 30; // streaming settles: both drawn every tick for 30 ticks
		DiffStep = 2;
		break;
	case 2:
		ShootBoth();
		if (--DiffSettle <= 0)
		{
			DiffWait = 2;
			DiffStep = 3;
		}
		break;
	case 3:
		if (DiffRead(RefTarget, DiffRef) && DiffRead(Target, DiffVar))
		{
			DiffCompare(*FString::Printf(TEXT("square %d settled"), DiffSquare), DiffRef, DiffVar,
				Dir / FString::Printf(TEXT("sq%d"), DiffSquare));
		}
		Shoot(*RefCapture);
		DiffWait = 2;
		DiffStep = 4;
		break;
	case 4:
		if (DiffRead(RefTarget, DiffRefAgain))
		{
			DiffCompare(*FString::Printf(TEXT("square %d A-A"), DiffSquare), DiffRef, DiffRefAgain, FString());
		}
		DiffAt.cx += Spec.recaptureM; // one recapture step on: a persisted view state carries from the settled square
		Place();
		ShootBoth();
		DiffWait = 2;
		DiffStep = 5;
		break;
	default:
		if (DiffRead(RefTarget, DiffRef) && DiffRead(Target, DiffVar))
		{
			DiffCompare(*FString::Printf(TEXT("square %d +%.0f m first"), DiffSquare, Spec.recaptureM), DiffRef, DiffVar, FString());
		}
		DiffStep = 0;
		if (++DiffSquare >= 10)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover diff: done."));
			RefCapture->Destroy();
			RefCapture = nullptr;
			bDiff = false;
			bHaveCapture = false; // the shipped capture goes back over the camera on the next tick
		}
		break;
	}
}

bool UFTRainCoverSubsystem::DiffRead(UTextureRenderTarget2D* From, TArray<FLinearColor>& Out) const
{
	FTextureRenderTargetResource* Resource = From ? From->GameThread_GetRenderTargetResource() : nullptr;
	Out.Reset();
	if (!Resource || !Resource->ReadLinearColorPixels(Out) || Out.Num() != Spec.resolution * Spec.resolution)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("RainCover diff: no readback."));
		return false;
	}
	return true;
}

void UFTRainCoverSubsystem::DiffCompare(const TCHAR* Label, const TArray<FLinearColor>& Ref, const TArray<FLinearColor>& Var,
	const FString& SaveAs) const
{
	const int32 Res = Spec.resolution;
	const float Far = Spec.depthM;
	const float Texel = DiffAt.sizeM / float(Res);
	// Texels within 1 m in plan of a pawn (the leads, the followers, the crowd) count as on a character.
	TArray<bool> OnPawn;
	OnPawn.Init(false, Res * Res);
	const int32 PawnR = FMath::CeilToInt32(1.f / Texel);
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		const FVector L = It->GetActorLocation();
		float U = 0.f, V = 0.f;
		if (!ftcore::rainCoverUV(DiffAt, float(L.X) * 0.01f, float(L.Y) * 0.01f, U, V))
		{
			continue;
		}
		const int32 Px = int32(U * float(Res)), Py = int32(V * float(Res));
		for (int32 Y = FMath::Max(Py - PawnR, 0); Y <= FMath::Min(Py + PawnR, Res - 1); ++Y)
		{
			for (int32 X = FMath::Max(Px - PawnR, 0); X <= FMath::Min(Px + PawnR, Res - 1); ++X)
			{
				OnPawn[Y * Res + X] = true;
			}
		}
	}
	const UFTWaterSubsystem* Water = GetWorld()->GetSubsystem<UFTWaterSubsystem>();
	const bool bWater = Water && Water->HasBodies();
	const float Now = GetWorld()->GetTimeSeconds();
	int32 RefHit = 0, VarHit = 0, Differ = 0, RefNearer = 0, VarNearer = 0, Lost = 0, Gained = 0, Character = 0, OnWater = 0, Other = 0;
	float MaxCm = 0.f, MaxOtherCm = 0.f;
	for (int32 Py = 0; Py < Res; ++Py)
	{
		for (int32 Px = 0; Px < Res; ++Px)
		{
			const int32 I = Py * Res + Px;
			const float A = Ref[I].R * 0.01f, B = Var[I].R * 0.01f;
			const bool bA = A < Far, bB = B < Far;
			RefHit += bA ? 1 : 0;
			VarHit += bB ? 1 : 0;
			if (!bA && !bB)
			{
				continue;
			}
			const float D = (bA && bB) ? std::fabs(A - B) : Far;
			if (D <= 0.01f) // 1 cm
			{
				continue;
			}
			++Differ;
			Lost += (bA && !bB) ? 1 : 0;
			Gained += (!bA && bB) ? 1 : 0;
			if (bA && bB)
			{
				MaxCm = FMath::Max(MaxCm, D * 100.f);
				if (A < B)
				{
					++RefNearer;
				}
				else
				{
					++VarNearer;
				}
			}
			const float X = DiffAt.cx - ((float(Py) + 0.5f) / float(Res) - 0.5f) * DiffAt.sizeM;
			const float Y = DiffAt.cy + ((float(Px) + 0.5f) / float(Res) - 0.5f) * DiffAt.sizeM;
			float Surface = 0.f;
			if (OnPawn[I])
			{
				++Character;
			}
			else if (bWater && Water->Bodies().surfaceHeight(ftcore::Vec2{X, Y}, Now, Surface) &&
				((bA && std::fabs(DiffAt.topZ - A - Surface) < 1.5f) || (bB && std::fabs(DiffAt.topZ - B - Surface) < 1.5f)))
			{
				++OnWater;
			}
			else
			{
				++Other;
				MaxOtherCm = FMath::Max(MaxOtherCm, (bA && bB) ? D * 100.f : Far * 100.f);
			}
		}
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover diff: %s at (%.1f, %.1f): ref hits %d, drawn hits %d of %d; differ (>1 cm) %d: ref nearer %d, drawn nearer %d, lost %d, gained %d; on a character %d, on water %d, other %d; max %.2f cm, other max %.2f cm."),
		Label, DiffAt.cx, DiffAt.cy, RefHit, VarHit, Res * Res, Differ, RefNearer, VarNearer, Lost, Gained, Character, OnWater, Other, MaxCm,
		MaxOtherCm);
	if (!SaveAs.IsEmpty())
	{
		const auto Save = [Res](const TArray<FLinearColor>& Map, const FString& Path)
		{
			TArray<uint8> Bytes;
			Bytes.SetNumUninitialized(Res * Res * int32(sizeof(float)));
			float* Out = reinterpret_cast<float*>(Bytes.GetData());
			for (int32 I = 0; I < Res * Res; ++I)
			{
				Out[I] = Map[I].R;
			}
			return FFileHelper::SaveArrayToFile(Bytes, *Path);
		};
		const bool bSaved = Save(Ref, SaveAs + TEXT("_ref.r32")) && Save(Var, SaveAs + TEXT("_drawn.r32"));
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover diff: %s %s (%dx%d float cm, top %.2f m)."),
			bSaved ? TEXT("saved") : TEXT("couldn't save"), *SaveAs, Res, Res, DiffAt.topZ);
	}
}
// ---- Path 3b probe (dev, 11-25): fixed tiles drawn once, stored as R16, compared against the shipped capture. ----
namespace
{
constexpr float kTileFindTopM = 400.f; // over the strip's highest box (242 m)
constexpr float kTileFindFarM = 500.f; // ... down to -100 m (the lowest ground is -40 m)
constexpr float kTileMarginM = 1.f;    // a tile's top over its highest occluder
constexpr int32 kTileSettle = 20;      // ticks a camera stands before its reference
constexpr float kTileTolM = 0.002f;    // the ruled height tolerance

// Median and max of a list of times (ms); the list is sorted.
void MedMax(TArray<double>& V, double& Med, double& Max)
{
	Med = Max = 0.0;
	if (V.Num() == 0)
	{
		return;
	}
	V.Sort();
	Med = V[V.Num() / 2];
	Max = V.Last();
}
} // namespace

bool UFTRainCoverSubsystem::TileSetup(UWorld& World)
{
	TileTarget = NewObject<UTextureRenderTarget2D>(this);
	TileTarget->ClearColor = Target->ClearColor;
	TileTarget->InitCustomFormat(Spec.resolution, Spec.resolution, PF_R32_FLOAT, true);
	TileTarget->UpdateResourceImmediate(true);
	FActorSpawnParameters Params;
	Params.ObjectFlags = RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	TileCapture = World.SpawnActor<ASceneCapture2D>(FVector::ZeroVector, FRotator(-90.f, 0.f, 0.f), Params);
	TileCamera = World.SpawnActor<ACameraActor>(FVector::ZeroVector, FRotator(-45.f, 0.f, 0.f), Params);
	USceneCaptureComponent2D* C = TileCapture ? TileCapture->GetCaptureComponent2D() : nullptr;
	if (!C || !TileCamera)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("RainCover tiles: couldn't spawn the tile capture or camera."));
		return false;
	}
	SetCoverCapture(*C, TileTarget, Spec, true); // the shipped trimmed set
	const int32 N = TileList.Num();
	TileTop.Init(0.f, N);
	TileFloor.Init(0.f, N);
	TileFar.SetNum(N);
	TileCode.SetNum(N);
	TileRange.SetNum(N);
	TileDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RainCoverTiles"));
	IFileManager::Get().MakeDirectory(*TileDir, true);
	Truth = MakeShared<FFTRainTruth>();
	{
		const TCHAR* Sw = FTLookSwitches();
		Truth->Base = (FParse::Param(Sw, TEXT("RainCoverTileLod0")) ? 1 : 0) | (FParse::Param(Sw, TEXT("RainCoverTileNoCull")) ? 2 : 0) |
			(FParse::Param(Sw, TEXT("RainCoverTileNoHlod")) ? 4 : 0);
		FParse::Value(Sw, TEXT("RainCoverTruthPasses="), Truth->Mask);
		Truth->Mask |= 1 | (1 << 5);
		FParse::Value(Sw, TEXT("RainCoverTruthSettle="), Truth->Settle);
		Truth->Settle = FMath::Max(Truth->Settle, 2);
	}
	const int32 Items = N + SeamList.Num();
	Truth->RefD.SetNum(Items);
	Truth->RefF.SetNum(Items);
	Truth->RefLabel.SetNum(Items);
	const APlayerController* PC = World.GetFirstPlayerController();
	const FVector Eye = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover tiles: set up at tick %d, camera at (%.1f, %.1f, %.1f) m, cache %s."), TileTicks,
		Eye.X * 0.01, Eye.Y * 0.01, Eye.Z * 0.01, *TileDir);
	return true;
}

bool UFTRainCoverSubsystem::TileDraw(const ftcore::RainCoverFrame& At, float FarM, TArray<float>& DepthM, double& EnqueueMs, double& ReadMs)
{
	USceneCaptureComponent2D* C = TileCapture->GetCaptureComponent2D();
	TileCapture->SetActorLocation(FVector(At.cx, At.cy, At.topZ) * 100.0);
	C->MaxViewDistanceOverride = FarM * 100.f;
	const double T0 = FPlatformTime::Seconds();
	C->CaptureScene(); // enqueues the draw
	const double T1 = FPlatformTime::Seconds();
	const bool bRead = DiffRead(TileTarget, TilePixels); // flushes (the draw on the render thread and GPU), then copies back
	const double T2 = FPlatformTime::Seconds();
	EnqueueMs = (T1 - T0) * 1000.0;
	ReadMs = (T2 - T1) * 1000.0;
	if (!bRead)
	{
		return false;
	}
	DepthM.SetNumUninitialized(TilePixels.Num());
	for (int32 I = 0; I < TilePixels.Num(); ++I)
	{
		DepthM[I] = TilePixels[I].R * 0.01f;
	}
	return true;
}

void UFTRainCoverSubsystem::TileFarLoop()
{
	const int32 Res = Spec.resolution, N = Res * Res;
	TArray<double> FindMs, CapMs, ReadMs, PackMs, WriteMs, CacheMs;
	int32 Empty = 0, OverTol = 0, BadCache = 0, Flips = 0;
	float QMaxAll = 0.f;
	const double L0 = FPlatformTime::Seconds();
	for (int32 I = 0; I < TileList.Num(); ++I)
	{
		const ftcore::RainCoverTile T = TileList[I];
		ftcore::RainCoverFrame F = ftcore::rainCoverTileFrame(Spec, T, kTileFindTopM);
		double FE = 0.0, FR = 0.0;
		if (!TileDraw(F, kTileFindFarM, TileScratch, FE, FR))
		{
			continue;
		}
		int32 FindHits = 0;
		float MaxZ = 0.f;
		for (const float D : TileScratch)
		{
			if (D < kTileFindFarM)
			{
				MaxZ = FindHits == 0 ? kTileFindTopM - D : FMath::Max(MaxZ, kTileFindTopM - D);
				++FindHits;
			}
		}
		if (I > 0)
		{
			FindMs.Add(FE + FR);
		}
		if (FindHits == 0) // nothing from 400 m down to -100 m: stored as all open
		{
			++Empty;
			TileTop[I] = kTileFindTopM;
			TileFloor[I] = kTileFindTopM - kTileFindFarM;
			TileCode[I].Init(uint16(ftcore::kRainCoverR16None), N);
			TileRange[I] = ftcore::RainCoverR16Range{};
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover tile: %d (%d, %d)%s: empty (find %.2f + %.2f ms)."), I, T.x, T.y,
				I == 0 ? TEXT(" cold") : TEXT(""), FE, FR);
			continue;
		}
		F.topZ = MaxZ + kTileMarginM;
		TileTop[I] = F.topZ;
		TileFloor[I] = F.topZ - Spec.depthM;
		double CE = 0.0, CR = 0.0;
		if (!TileDraw(F, Spec.depthM, TileFar[I], CE, CR))
		{
			continue;
		}
		TileCode[I].SetNumUninitialized(N);
		const double P0 = FPlatformTime::Seconds();
		const bool bTol = ftcore::rainCoverR16Pack(TileFar[I].GetData(), N, F.topZ, Spec.depthM, TileCode[I].GetData(), TileRange[I], kTileTolM);
		const double P1 = FPlatformTime::Seconds();
		// The cache file: a 32-byte header (magic, tile, top, range, hits, resolution), then the codes.
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(32 + N * 2);
		uint8* B = Bytes.GetData();
		const int32 Head[2] = {T.x, T.y};
		const float HeadZ[3] = {F.topZ, TileRange[I].minZ, TileRange[I].maxZ};
		const int32 HeadN[2] = {TileRange[I].hits, Res};
		FMemory::Memcpy(B, "FTRC", 4);
		FMemory::Memcpy(B + 4, Head, 8);
		FMemory::Memcpy(B + 12, HeadZ, 12);
		FMemory::Memcpy(B + 24, HeadN, 8);
		FMemory::Memcpy(B + 32, TileCode[I].GetData(), N * 2);
		const FString Path = TileDir / FString::Printf(TEXT("tile_%d_%d.r16"), T.x, T.y);
		const bool bWrote = FFileHelper::SaveArrayToFile(Bytes, *Path);
		const double P2 = FPlatformTime::Seconds();
		TArray<uint8> Back;
		const bool bBack = FFileHelper::LoadFileToArray(Back, *Path);
		const double P3 = FPlatformTime::Seconds();
		const bool bSame = bWrote && bBack && Back.Num() == Bytes.Num() && FMemory::Memcmp(Back.GetData(), Bytes.GetData(), Bytes.Num()) == 0;
		BadCache += bSame ? 0 : 1;
		// The R16 codes back to heights against this tile's own R32F capture: the storage's error alone.
		float QMax = 0.f;
		int32 QFlip = 0;
		for (int32 K = 0; K < N; ++K)
		{
			const bool bHit = TileFar[I][K] < Spec.depthM;
			float Z = 0.f;
			const bool bCode = ftcore::rainCoverR16Unpack(TileCode[I][K], TileRange[I], Z);
			if (bHit != bCode)
			{
				++QFlip;
			}
			else if (bHit)
			{
				QMax = FMath::Max(QMax, std::fabs(Z - (F.topZ - TileFar[I][K])));
			}
		}
		Flips += QFlip;
		QMaxAll = FMath::Max(QMaxAll, QMax);
		OverTol += bTol ? 0 : 1;
		if (I > 0)
		{
			CapMs.Add(CE);
			ReadMs.Add(CR);
			PackMs.Add((P1 - P0) * 1000.0);
			WriteMs.Add((P2 - P1) * 1000.0);
			CacheMs.Add((P3 - P2) * 1000.0);
		}
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover tile: %d (%d, %d)%s: find %.2f + %.2f ms, %d hits, top %.2f m; draw %.2f ms, read %.2f ms; %d hits, z %.2f..%.2f m; pack %.3f ms%s, write %.3f ms, cache read (warm) %.3f ms%s; R16 max %.3f mm, flips %d."),
			I, T.x, T.y, I == 0 ? TEXT(" cold") : TEXT(""), FE, FR, FindHits, F.topZ, CE, CR, TileRange[I].hits, TileRange[I].minZ,
			TileRange[I].maxZ, (P1 - P0) * 1000.0, bTol ? TEXT("") : TEXT(" (over 2 mm)"), (P2 - P1) * 1000.0, (P3 - P2) * 1000.0,
			bSame ? TEXT("") : TEXT(" (MISMATCH)"), QMax * 1000.f, QFlip);
	}
	const double Wall = (FPlatformTime::Seconds() - L0) * 1000.0;
	double M[6][2];
	MedMax(FindMs, M[0][0], M[0][1]);
	MedMax(CapMs, M[1][0], M[1][1]);
	MedMax(ReadMs, M[2][0], M[2][1]);
	MedMax(PackMs, M[3][0], M[3][1]);
	MedMax(WriteMs, M[4][0], M[4][1]);
	MedMax(CacheMs, M[5][0], M[5][1]);
	TileHeld = TileList.Num() - Empty;
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover tiles: far loop %.0f ms for %d tiles, %d held occluders, %d empty; median/max past the first: find %.2f/%.2f, draw %.2f/%.2f, read %.2f/%.2f, pack %.3f/%.3f, write %.3f/%.3f, cache read %.3f/%.3f ms; over 2 mm %d, cache mismatches %d, R16 max %.3f mm, flips %d."),
		Wall, TileList.Num(), TileHeld, Empty, M[0][0], M[0][1], M[1][0], M[1][1], M[2][0], M[2][1], M[3][0], M[3][1], M[4][0], M[4][1],
		M[5][0], M[5][1], OverTol, BadCache, QMaxAll * 1000.f, Flips);
}

bool UFTRainCoverSubsystem::TilePlaceCamera(UWorld& World, float X, float Y)
{
	// The lowest surface under (X, Y): trace down from the find top, again from just under each hit.
	FCollisionQueryParams Q(SCENE_QUERY_STAT(RainCoverTileProbe), true);
	float Z = kTileFindTopM, Ground = 0.f;
	int32 Layers = 0;
	for (int32 K = 0; K < 32; ++K)
	{
		FHitResult Hit;
		if (!World.LineTraceSingleByChannel(Hit, FVector(X, Y, Z) * 100.0, FVector(X, Y, kTileFindTopM - kTileFindFarM) * 100.0, ECC_Visibility, Q))
		{
			break;
		}
		Ground = float(Hit.ImpactPoint.Z) * 0.01f;
		++Layers;
		Z = Ground - 0.05f;
	}
	TileCam = FVector(X, Y, Ground + 2.f) * 100.0;
	TileCamera->SetActorLocation(TileCam);
	APlayerController* PC = World.GetFirstPlayerController();
	if (PC)
	{
		PC->SetViewTarget(TileCamera);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover tile camera: (%.1f, %.1f) ground %.2f m (%d surfaces over it)."), X, Y, Ground, Layers);
	return PC != nullptr;
}

void UFTRainCoverSubsystem::TileCompareRef(const TCHAR* Label, int32 Own, int32 Item, float X, float Y)
{
	const int32 Res = Spec.resolution;
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const FVector Eye = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
	const bool bHeld = FVector::Dist(Eye, TileCam) < 100.0;
	// The square the shipped capture would draw from this camera.
	const ftcore::RainCoverFrame R = ftcore::rainCoverFrameAt(Spec, X, Y, float(TileCam.Z) * 0.01f);
	double E = 0.0, Rd = 0.0;
	TArray<float> Ref;
	if (!TileDraw(R, Spec.depthM, Ref, E, Rd))
	{
		return;
	}
	if (Truth && Truth->RefD.IsValidIndex(Item)) // kept for the truth passes
	{
		Truth->RefD[Item] = Ref;
		Truth->RefF[Item] = R;
		Truth->RefLabel[Item] = Label;
	}
	// Texels within 1 m in plan of a pawn count as on a character (as the diff does).
	TArray<bool> OnPawn;
	OnPawn.Init(false, Res * Res);
	const float Texel = R.sizeM / float(Res);
	const int32 PawnR = FMath::CeilToInt32(1.f / Texel);
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		const FVector L = It->GetActorLocation();
		float U = 0.f, V = 0.f;
		if (!ftcore::rainCoverUV(R, float(L.X) * 0.01f, float(L.Y) * 0.01f, U, V))
		{
			continue;
		}
		const int32 Px = int32(U * float(Res)), Py = int32(V * float(Res));
		for (int32 Yy = FMath::Max(Py - PawnR, 0); Yy <= FMath::Min(Py + PawnR, Res - 1); ++Yy)
		{
			for (int32 Xx = FMath::Max(Px - PawnR, 0); Xx <= FMath::Min(Px + PawnR, Res - 1); ++Xx)
			{
				OnPawn[Yy * Res + Xx] = true;
			}
		}
	}
	const UFTWaterSubsystem* Water = GetWorld()->GetSubsystem<UFTWaterSubsystem>();
	const bool bWater = Water && Water->HasBodies();
	const float Now = GetWorld()->GetTimeSeconds();
	// Classes: agree; band (the tile's occluder over the reference's top); below either far plane; fails: height over 2 mm,
	// gained (tile only), lost (reference only); of the fails, on a tile's edge texel, on a character, on water.
	struct FCount
	{
		int32 Agree = 0, Band = 0, BelowRef = 0, BelowTile = 0, Height = 0, Gained = 0, Lost = 0, Edge = 0, Char = 0, OnWater = 0;
		float MaxCm = 0.f;
	};
	FCount Stored, OnTile;
	int32 NoTile = 0, RefHits = 0, TileHits = 0;
	const auto Classify = [&](FCount& Cn, bool bT, float Tz, float Floor, bool bR, float Rz, int32 I, int32 Tx, int32 Ty, float Wx, float Wy)
	{
		if (!bT && !bR)
		{
			++Cn.Agree;
			return;
		}
		if (bT && bR)
		{
			const float Dz = std::fabs(Tz - Rz);
			if (Dz <= kTileTolM)
			{
				++Cn.Agree;
				return;
			}
			if (Tz > R.topZ)
			{
				++Cn.Band;
				return;
			}
			++Cn.Height;
			Cn.MaxCm = FMath::Max(Cn.MaxCm, Dz * 100.f);
		}
		else if (bT)
		{
			if (Tz > R.topZ)
			{
				++Cn.Band;
				return;
			}
			if (Tz < R.topZ - Spec.depthM)
			{
				++Cn.BelowRef;
				return;
			}
			++Cn.Gained;
		}
		else
		{
			if (Rz < Floor)
			{
				++Cn.BelowTile;
				return;
			}
			++Cn.Lost;
		}
		Cn.Edge += (Tx == 0 || Tx == Res - 1 || Ty == 0 || Ty == Res - 1) ? 1 : 0;
		float Surface = 0.f;
		if (OnPawn[I])
		{
			++Cn.Char;
		}
		else if (bWater && Water->Bodies().surfaceHeight(ftcore::Vec2{Wx, Wy}, Now, Surface) &&
			((bR && std::fabs(Rz - Surface) < 1.5f) || (bT && std::fabs(Tz - Surface) < 1.5f)))
		{
			++Cn.OnWater;
		}
	};
	for (int32 Py = 0; Py < Res; ++Py)
	{
		for (int32 Px = 0; Px < Res; ++Px)
		{
			const int32 I = Py * Res + Px;
			const float Wx = R.cx - ((float(Py) + 0.5f) / float(Res) - 0.5f) * R.sizeM;
			const float Wy = R.cy + ((float(Px) + 0.5f) / float(Res) - 0.5f) * R.sizeM;
			ftcore::RainCoverTile T;
			int32 Tx = 0, Ty = 0;
			const int32* Ti = ftcore::rainCoverTileTexel(Spec, Wx, Wy, T, Tx, Ty) ? TileIndex.Find(FIntPoint(T.x, T.y)) : nullptr;
			if (!Ti || TileCode[*Ti].Num() != Res * Res)
			{
				++NoTile;
				continue;
			}
			const int32 K = Ty * Res + Tx;
			const bool bR = Ref[I] < Spec.depthM;
			const float Rz = R.topZ - Ref[I];
			float Tz = 0.f;
			const bool bT = ftcore::rainCoverR16Unpack(TileCode[*Ti][K], TileRange[*Ti], Tz);
			RefHits += bR ? 1 : 0;
			TileHits += bT ? 1 : 0;
			Classify(Stored, bT, Tz, TileFloor[*Ti], bR, Rz, I, Tx, Ty, Wx, Wy);
			if (Own >= 0 && *Ti == Own && TileOn.Num() == Res * Res)
			{
				const bool bO = TileOn[K] < Spec.depthM;
				Classify(OnTile, bO, TileTop[Own] - TileOn[K], TileFloor[Own], bR, Rz, I, Tx, Ty, Wx, Wy);
			}
		}
	}
	const int32 Fails = Stored.Height + Stored.Gained + Stored.Lost;
	++TileRefs;
	TileRefFails += Fails > 0 ? 1 : 0;
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover tile ref: %s at (%.1f, %.1f), camera %.2f m%s, top %.2f m, draw %.2f + %.2f ms: ref hits %d, tile hits %d, no tile %d; stored R16: agree %d, band %d, below ref far %d, below tile far %d; FAILS %d (height %d, max %.2f cm; gained %d, lost %d; on an edge %d, on a character %d, on water %d)."),
		Label, X, Y, TileCam.Z * 0.01, bHeld ? TEXT("") : TEXT(" (VIEW TARGET NOT HELD)"), R.topZ, E, Rd, RefHits, TileHits, NoTile,
		Stored.Agree, Stored.Band, Stored.BelowRef, Stored.BelowTile, Fails, Stored.Height, Stored.MaxCm, Stored.Gained, Stored.Lost,
		Stored.Edge, Stored.Char, Stored.OnWater);
	if (Own >= 0)
	{
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover tile ref: %s on-tile R32F against it: agree %d, band %d, below ref far %d, below tile far %d; fails %d (height %d, max %.2f cm; gained %d, lost %d; character %d, water %d)."),
			Label, OnTile.Agree, OnTile.Band, OnTile.BelowRef, OnTile.BelowTile, OnTile.Height + OnTile.Gained + OnTile.Lost, OnTile.Height,
			OnTile.MaxCm, OnTile.Gained, OnTile.Lost, OnTile.Char, OnTile.OnWater);
	}
}

void UFTRainCoverSubsystem::TileTick(UWorld& World)
{
	const int32 Res = Spec.resolution, N = Res * Res;
	const int32 Items = TileList.Num() + SeamList.Num();
	// The far capture of a tile against a second draw of it (camera elsewhere, same top and far plane), R32F both.
	const auto Against = [&](int32 I, const TArray<float>& B, int32& HitDiff, int32& Over, float& MaxMm)
	{
		HitDiff = Over = 0;
		MaxMm = 0.f;
		const TArray<float>& A = TileFar[I];
		if (A.Num() != N || B.Num() != N)
		{
			return;
		}
		for (int32 K = 0; K < N; ++K)
		{
			const bool bA = A[K] < Spec.depthM, bB = B[K] < Spec.depthM;
			if (bA != bB)
			{
				++HitDiff;
			}
			else if (bA)
			{
				const float D = std::fabs(A[K] - B[K]);
				Over += D > kTileTolM ? 1 : 0;
				MaxMm = FMath::Max(MaxMm, D * 1000.f);
			}
		}
	};
	switch (TileState)
	{
	case 0: // past the warm-up, with every post-load spawner run
		if (++TileTicks < 600)
		{
			return;
		}
		if (!TileSetup(World))
		{
			bTiles = false;
			return;
		}
		if (Truth->Base != 0) // the draw switches held from the command line, on every draw below
		{
			TruthSetUp(World);
			TruthSwitches(World, false, false, false);
			TileWait = Truth->Settle;
		}
		TileState = 1;
		return;
	case 1: // the load-time loop, in one tick, from the camera's own place
		if (TileWait > 0)
		{
			--TileWait;
			return;
		}
		if (bTileStrip) // 5b and 5c: the whole strip, 32 tiles a tick, nothing kept
		{
			TileState = 10;
			return;
		}
		TileFarLoop();
		TileState = 2;
		TileWait = 2;
		return;
	case 2: // two ticks on: the first five again (a stale readback would differ)
		if (--TileWait > 0)
		{
			return;
		}
		for (int32 I = 0, Done = 0; I < TileList.Num() && Done < 5; ++I)
		{
			if (TileFar[I].Num() != N)
			{
				continue;
			}
			++Done;
			double E = 0.0, Rd = 0.0;
			TArray<float> Again;
			if (TileDraw(ftcore::rainCoverTileFrame(Spec, TileList[I], TileTop[I]), Spec.depthM, Again, E, Rd))
			{
				int32 HitDiff = 0, Over = 0;
				float MaxMm = 0.f;
				Against(I, Again, HitDiff, Over, MaxMm);
				UE_LOG(LogForgottenThird, Display, TEXT("RainCover tile again: %d (%d, %d) two ticks on: hits differ %d, over 2 mm %d, max %.3f mm."), I,
					TileList[I].x, TileList[I].y, HitDiff, Over, MaxMm);
			}
		}
		TileState = 3;
		TileAt = 0;
		{
			const ftcore::RainCoverFrame F = ftcore::rainCoverTileFrame(Spec, TileList[0], 0.f);
			TilePlaceCamera(World, F.cx, F.cy);
		}
		TileWait = kTileSettle;
		return;
	case 3: // one camera place at a time: on each tile's centre, then each seam point
		if (--TileWait > 0)
		{
			return;
		}
		if (TileAt < TileList.Num())
		{
			const int32 I = TileAt;
			const ftcore::RainCoverTile T = TileList[I];
			const ftcore::RainCoverFrame F = ftcore::rainCoverTileFrame(Spec, T, TileTop[I]);
			double E = 0.0, Rd = 0.0;
			TileOn.Reset();
			if (TileFar[I].Num() == N)
			{
				if (TileDraw(F, Spec.depthM, TileOn, E, Rd))
				{
					int32 HitDiff = 0, Over = 0;
					float MaxMm = 0.f;
					Against(I, TileOn, HitDiff, Over, MaxMm);
					UE_LOG(LogForgottenThird, Display,
						TEXT("RainCover tile on: %d (%d, %d), camera on the tile, same top: draw %.2f ms, read %.2f ms; far against on-tile: hits differ %d, over 2 mm %d, max %.3f mm."),
						I, T.x, T.y, E, Rd, HitDiff, Over, MaxMm);
				}
			}
			else // an empty tile: look again from the find top with the camera on it
			{
				TArray<float> Find;
				if (TileDraw(ftcore::rainCoverTileFrame(Spec, T, kTileFindTopM), kTileFindFarM, Find, E, Rd))
				{
					int32 Hits = 0;
					for (const float D : Find)
					{
						Hits += D < kTileFindFarM ? 1 : 0;
					}
					UE_LOG(LogForgottenThird, Display, TEXT("RainCover tile on: %d (%d, %d) empty from afar; camera on the tile: %d hits."), I, T.x, T.y, Hits);
				}
			}
			TileCompareRef(*FString::Printf(TEXT("tile %d (%d, %d)"), I, T.x, T.y), I, TileAt, F.cx, F.cy);
		}
		else
		{
			const FVector2f S = SeamList[TileAt - TileList.Num()];
			TileOn.Reset();
			TileCompareRef(*FString::Printf(TEXT("seam %d"), TileAt - TileList.Num()), -1, TileAt, S.X, S.Y);
		}
		++TileAt;
		if (TileAt < Items)
		{
			float X = 0.f, Y = 0.f;
			if (TileAt < TileList.Num())
			{
				const ftcore::RainCoverFrame F = ftcore::rainCoverTileFrame(Spec, TileList[TileAt], 0.f);
				X = F.cx;
				Y = F.cy;
			}
			else
			{
				X = SeamList[TileAt - TileList.Num()].X;
				Y = SeamList[TileAt - TileList.Num()].Y;
			}
			TilePlaceCamera(World, X, Y);
			TileWait = kTileSettle;
			return;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover tiles: done: %d tiles (%d held occluders), %d references, %d with fails."), TileList.Num(),
			TileHeld, TileRefs, TileRefFails);
		TileState = 5; // 5a: the truth passes
		return;
	case 5:
		TruthTick(World);
		return;
	case 10:
	case 11:
		StripTick(World);
		return;
	default:
		return;
	}
}

void UFTRainCoverSubsystem::UploadTick(const FVector& Eye)
{
	const float Ex = float(Eye.X) * 0.01f, Ey = float(Eye.Y) * 0.01f;
	if (!ftcore::rainCoverRecapture(Spec, bUploadAny, UploadFrame.cx, UploadFrame.cy, Ex, Ey))
	{
		return;
	}
	UploadFrame = ftcore::rainCoverFrameAt(Spec, Ex, Ey, float(Eye.Z) * 0.01f);
	bUploadAny = true;
	CSV_CUSTOM_STAT(RainCover, Upload, 1, ECsvCustomStatOp::Set);
	CSV_SCOPED_TIMING_STAT(RainCover, UploadEnqueue);
	const int32 Res = Spec.resolution;
	for (int32 K = 0; K < UploadTex.Num(); ++K)
	{
		FTextureResource* Into = UploadTex[K] ? UploadTex[K]->GetResource() : nullptr;
		if (!Into)
		{
			continue;
		}
		const uint8* Src = reinterpret_cast<const uint8*>(UploadSrc.GetData() + K * Res * Res);
		ENQUEUE_RENDER_COMMAND(FTRainCoverUpload)([Into, Src, Res](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Tex = Into->GetTexture2DRHI();
			if (Tex)
			{
				RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, uint32(Res), uint32(Res)), uint32(Res) * sizeof(float), Src);
			}
		});
	}
	++Uploads;
}
// ---- 11-25 truth probe (ruling 7047 item 5, dev): three draw switches, a truth at each tile's own top, the strip, the stream. ----
namespace
{
constexpr float kTruthSlackM = 0.15f;   // a candidate's box against a failing texel
constexpr int32 kTruthCsvMax = 400000;  // rows
constexpr int32 kRcStripPerTick = 32;
constexpr int32 kRcStripNameMax = 8;

struct FTruthPass
{
	const TCHAR* Name;
	bool bLod0, bNoCull, bNoHlod;
};
constexpr FTruthPass kTruthPasses[] = {
	{TEXT("all"), true, true, true}, {TEXT("lod0"), true, false, false}, {TEXT("nocull"), false, true, false},
	{TEXT("nohlod"), false, false, true}, {TEXT("again"), true, true, true}, {TEXT("plain"), false, false, false}};
constexpr int32 kPassAgain = 4; // all three switches again, after the others: truth must repeat texel for texel
constexpr int32 kPassPlain = 5; // the command-line switches only: the stored draws must repeat

// One draw against truth, texel for texel. Signed delta = draw Z - truth Z.
struct FTruthCount
{
	int32 Agree = 0, Band = 0, BelowDraw = 0, BelowTruth = 0, Up = 0, Down = 0, Gained = 0, Lost = 0, Edge = 0, NoTile = 0;
	float MinCm = 0.f, MaxCm = 0.f;
	int32 Fails() const { return Up + Down + Gained + Lost; }
	void Add(const FTruthCount& O)
	{
		Agree += O.Agree; Band += O.Band; BelowDraw += O.BelowDraw; BelowTruth += O.BelowTruth; Up += O.Up; Down += O.Down;
		Gained += O.Gained; Lost += O.Lost; Edge += O.Edge; NoTile += O.NoTile;
		MinCm = FMath::Min(MinCm, O.MinCm);
		MaxCm = FMath::Max(MaxCm, O.MaxCm);
	}
	FString Text() const
	{
		return FString::Printf(TEXT("agree %d, band %d, below draw far %d, below truth far %d, no tile %d; FAILS %d (up %d, down %d, signed %.2f..%.2f cm; gained %d, lost %d; on an edge %d)"),
			Agree, Band, BelowDraw, BelowTruth, NoTile, Fails(), Up, Down, MinCm, MaxCm, Gained, Lost, Edge);
	}
};

int32 RcCVarInt(const TCHAR* Name, int32 Fallback)
{
	IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
	return V ? V->GetInt() : Fallback;
}

void RcCVarSet(const TCHAR* Name, int32 Value)
{
	if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name))
	{
		V->Set(Value, ECVF_SetByCode);
	}
}
} // namespace

void UFTRainCoverSubsystem::TruthSwitches(UWorld& World, bool bLod0, bool bNoCull, bool bNoHlod)
{
	FFTRainTruth& T = *Truth;
	bLod0 = bLod0 || (T.Base & 1) != 0;
	bNoCull = bNoCull || (T.Base & 2) != 0;
	bNoHlod = bNoHlod || (T.Base & 4) != 0;
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth switches: LOD0 %d, no cull %d, no HLOD %d (%d components, %d HLOD actors; command line %d)."),
		bLod0 ? 1 : 0, bNoCull ? 1 : 0, bNoHlod ? 1 : 0, T.Saved.Num(), T.Hlod.Num(), T.Base);
	RcCVarSet(TEXT("r.ForceLOD"), bLod0 ? 0 : T.OldForceLod);
	RcCVarSet(TEXT("foliage.ForceLOD"), bLod0 ? 0 : T.OldFoliageLod);
	RcCVarSet(TEXT("r.InstancedStaticMeshes.GpuLod"), bLod0 ? 0 : T.OldGpuLod);
	for (const FFTRainTruth::FSaved& S : T.Saved)
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
			UInstancedStaticMeshComponent* I = static_cast<UInstancedStaticMeshComponent*>(C);
			I->SetCullDistances(bNoCull ? 0 : S.IStart, bNoCull ? 0 : S.IEnd);
		}
		C->MarkRenderStateDirty(); // the ISM proxy reads GpuLod when it is made
	}
	USceneCaptureComponent2D* Cap = TileCapture->GetCaptureComponent2D();
	Cap->HiddenActors.Reset();
	if (bNoHlod)
	{
		for (const TWeakObjectPtr<AActor>& A : T.Hlod)
		{
			if (A.IsValid())
			{
				Cap->HiddenActors.Add(A.Get());
				if (T.Base & 4) // held from the command line: the shipped capture drops them too
				{
					Capture->GetCaptureComponent2D()->HiddenActors.AddUnique(A.Get());
				}
			}
		}
	}
	World.SendAllEndOfFrameUpdates();
	FlushRenderingCommands();
}

void UFTRainCoverSubsystem::TruthSetUp(UWorld& World)
{
	FFTRainTruth& T = *Truth;
	if (T.bSetUp)
	{
		return;
	}
	T.bSetUp = true;
	const int32 NT = TileList.Num();
	// Set up: candidates over the tiles, the saved draw settings, the HLOD actors, the two captures' settings.
	const double S0 = FPlatformTime::Seconds();
	int32 Comps = 0, Isms = 0, Insts = 0, MinDrawSet = 0;
	const auto AddCand = [&](const FBox& B, UStaticMeshComponent* C, int32 Inst)
	{
		const ftcore::RainCoverTile A = ftcore::rainCoverTileAt(Spec, float(B.Min.X) * 0.01f, float(B.Min.Y) * 0.01f);
		const ftcore::RainCoverTile Z = ftcore::rainCoverTileAt(Spec, float(B.Max.X) * 0.01f, float(B.Max.Y) * 0.01f);
		int32 Ci = -1;
		for (int32 X = A.x; X <= Z.x && X - A.x < 64; ++X)
		{
			for (int32 Y = A.y; Y <= Z.y && Y - A.y < 64; ++Y)
			{
				if (!TileIndex.Contains(FIntPoint(X, Y)))
				{
					continue;
				}
				if (Ci < 0)
				{
					Ci = T.Cands.Add(FFTRainTruth::FCand{B, C, Inst});
				}
				T.CandByTile.FindOrAdd(FIntPoint(X, Y)).Add(Ci);
			}
		}
	};
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		UStaticMeshComponent* C = *It;
		if (C->GetWorld() != &World || !C->IsRegistered() || !C->GetStaticMesh())
		{
			continue;
		}
		++Comps;
		UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C);
		FFTRainTruth::FSaved S;
		S.Comp = C;
		S.LD = C->LDMaxDrawDistance;
		S.Cached = C->CachedMaxDrawDistance;
		S.MinDraw = C->MinDrawDistance;
		S.Forced = C->ForcedLodModel;
		S.bIsm = I != nullptr;
		MinDrawSet += C->MinDrawDistance > 0.f ? 1 : 0;
		if (I)
		{
			++Isms;
			S.IStart = I->InstanceStartCullDistance;
			S.IEnd = I->InstanceEndCullDistance;
			const FBox MB = C->GetStaticMesh()->GetBoundingBox();
			for (int32 K = 0; K < I->GetInstanceCount(); ++K)
			{
				FTransform X;
				if (I->GetInstanceTransform(K, X, true))
				{
					++Insts;
					AddCand(MB.TransformBy(X), C, K);
				}
			}
		}
		else if (C->IsVisible())
		{
			AddCand(C->Bounds.GetBox(), C, -1);
		}
		T.Saved.Add(S);
	}
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		const FString Cls = It->GetClass()->GetName();
		if (Cls.Contains(TEXT("HLOD")) || Cls.Contains(TEXT("LODActor")))
		{
			T.Hlod.Add(*It);
		}
	}
	T.OldForceLod = RcCVarInt(TEXT("r.ForceLOD"), -1);
	T.OldFoliageLod = RcCVarInt(TEXT("foliage.ForceLOD"), -1);
	T.OldGpuLod = RcCVarInt(TEXT("r.InstancedStaticMeshes.GpuLod"), 1);
	T.TruthD.SetNum(NT);
	T.Csv = TEXT("kind,item,tile,tx,ty,wx,wy,ztruth,zdraw,dcm_draw_minus_truth,cand_at_ztruth,cand_at_zdraw\n");
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover truth: set up in %.0f ms: %d static mesh components (%d instanced, %d instances, %d with a min draw distance), %d candidate boxes over %d tiles; %d HLOD actors%s; cvars r.ForceLOD %d, foliage.ForceLOD %d, r.InstancedStaticMeshes.GpuLod %d."),
		(FPlatformTime::Seconds() - S0) * 1000.0, Comps, Isms, Insts, MinDrawSet, T.Cands.Num(), T.CandByTile.Num(), T.Hlod.Num(),
		T.Hlod.Num() == 0 ? TEXT(" (the no-HLOD switch is a no-op)") : TEXT(""), T.OldForceLod, T.OldFoliageLod, T.OldGpuLod);
	const USceneCaptureComponent2D* A = Capture->GetCaptureComponent2D();
	const USceneCaptureComponent2D* B = TileCapture->GetCaptureComponent2D();
	const FString FA = A->ShowFlags.ToString(), FB = B->ShowFlags.ToString();
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover truth: shipped capture against the tile capture: LOD factor %.3f / %.3f, ortho width %.0f / %.0f, source %d / %d, render mode %d / %d, main renderer %d / %d, hidden actors %d / %d, hidden components %d / %d, show only %d / %d, show flags %s."),
		A->LODDistanceFactor, B->LODDistanceFactor, A->OrthoWidth, B->OrthoWidth, int32(A->CaptureSource), int32(B->CaptureSource),
		int32(A->PrimitiveRenderMode), int32(B->PrimitiveRenderMode), A->bRenderInMainRenderer ? 1 : 0, B->bRenderInMainRenderer ? 1 : 0,
		A->HiddenActors.Num(), B->HiddenActors.Num(), A->HiddenComponents.Num(), B->HiddenComponents.Num(), A->ShowOnlyComponents.Num(),
		B->ShowOnlyComponents.Num(), FA == FB ? TEXT("equal") : *FString::Printf(TEXT("DIFFER (%s | %s)"), *FA, *FB));
}

void UFTRainCoverSubsystem::TruthTick(UWorld& World)
{
	FFTRainTruth& T = *Truth;
	const int32 Res = Spec.resolution, N = Res * Res;
	const int32 NT = TileList.Num();
	const auto CandText = [&](int32 Ci, const FVector& TileO, const FVector& RefO) -> FString
	{
		const FFTRainTruth::FCand& Cd = T.Cands[Ci];
		const UStaticMeshComponent* C = Cd.Comp.Get();
		if (!C)
		{
			return TEXT("(gone)");
		}
		const UStaticMesh* M = C->GetStaticMesh();
		const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C);
		const AActor* O = C->GetOwner();
		return FString::Printf(
			TEXT("%s %s/%s inst %d mesh %s (LODs %d, nanite %d) forced %d minLOD %d gpuLod %d cull %d..%d LD %.0f cached %.0f minDraw %.0f hlodRelevant %d box z %.2f..%.2f, %.1f m from the tile's capture, %.1f m from the ref's"),
			*C->GetClass()->GetName(), O ? *O->GetName() : TEXT("-"), *C->GetName(), Cd.Inst, M ? *M->GetName() : TEXT("-"),
			M ? M->GetNumLODs() : 0, M && M->IsNaniteEnabled() ? 1 : 0, C->ForcedLodModel, C->MinLOD, I ? int32(I->bUseGpuLodSelection) : -1,
			I ? I->InstanceStartCullDistance : -1, I ? I->InstanceEndCullDistance : -1, C->LDMaxDrawDistance, C->CachedMaxDrawDistance,
			C->MinDrawDistance, O && O->IsHLODRelevant() ? 1 : 0, Cd.Box.Min.Z * 0.01, Cd.Box.Max.Z * 0.01,
			TileO.Z > -1e6 ? FMath::Sqrt(Cd.Box.ComputeSquaredDistanceToPoint(TileO)) * 0.01 : -1.0, RefO.Z > -1e6 ? FMath::Sqrt(Cd.Box.ComputeSquaredDistanceToPoint(RefO)) * 0.01 : -1.0);
	};
	// The candidate under a texel at height Z (m): XY within slack, Z within its box, nearest top first.
	const auto Name = [&](int32 Ti, float Wx, float Wy, float Z) -> int32
	{
		const TArray<int32>* L = T.CandByTile.Find(FIntPoint(TileList[Ti].x, TileList[Ti].y));
		if (!L)
		{
			return -1;
		}
		const double S = kTruthSlackM * 100.0, X = Wx * 100.0, Y = Wy * 100.0, Zc = Z * 100.0;
		int32 Best = -1;
		double BestD = 1e30;
		for (const int32 Ci : *L)
		{
			const FBox& B = T.Cands[Ci].Box;
			if (X < B.Min.X - S || X > B.Max.X + S || Y < B.Min.Y - S || Y > B.Max.Y + S || Zc < B.Min.Z - S || Zc > B.Max.Z + S)
			{
				continue;
			}
			const double D = FMath::Abs(B.Max.Z - Zc);
			if (D < BestD)
			{
				BestD = D;
				Best = Ci;
			}
		}
		return Best;
	};
	const auto CandId = [&](int32 Ci) -> FString
	{
		const UStaticMeshComponent* Cc = Ci >= 0 ? T.Cands[Ci].Comp.Get() : nullptr;
		const AActor* Oc = Cc ? Cc->GetOwner() : nullptr;
		return FString::Printf(TEXT("%s/%s#%d"), Oc ? *Oc->GetName() : TEXT("-"), Cc ? *Cc->GetName() : TEXT("-"), Ci >= 0 ? T.Cands[Ci].Inst : -1);
	};
	// A draw (depth m over frame F, far depthM) against truth. Own >= 0: F is that tile's own frame (texel for texel).
	// Kind non-null: failing texels go to the CSV and the per-candidate tally.
	const auto Compare = [&](const TArray<float>& D, const ftcore::RainCoverFrame& F, int32 Own, const TCHAR* Kind, int32 Item,
		TMap<int32, int32>* Tally) -> FTruthCount
	{
		FTruthCount Cn;
		if (D.Num() != N)
		{
			Cn.NoTile = N;
			return Cn;
		}
		for (int32 Py = 0; Py < Res; ++Py)
		{
			for (int32 Px = 0; Px < Res; ++Px)
			{
				const int32 I = Py * Res + Px;
				const float Wx = F.cx - ((float(Py) + 0.5f) / float(Res) - 0.5f) * F.sizeM;
				const float Wy = F.cy + ((float(Px) + 0.5f) / float(Res) - 0.5f) * F.sizeM;
				int32 Ti = Own, Tx = Px, Ty = Py;
				if (Own < 0)
				{
					ftcore::RainCoverTile Tl;
					const int32* Found = ftcore::rainCoverTileTexel(Spec, Wx, Wy, Tl, Tx, Ty) ? TileIndex.Find(FIntPoint(Tl.x, Tl.y)) : nullptr;
					Ti = Found ? *Found : -1;
				}
				if (Ti < 0 || T.TruthD[Ti].Num() != N)
				{
					++Cn.NoTile;
					continue;
				}
				const int32 K = Ty * Res + Tx;
				const bool bT = T.TruthD[Ti][K] < Spec.depthM, bD = D[I] < Spec.depthM;
				const float Zt = TileTop[Ti] - T.TruthD[Ti][K], Zd = F.topZ - D[I];
				bool bFail = false;
				if (!bT && !bD)
				{
					++Cn.Agree;
				}
				else if (bT && bD)
				{
					const float Dz = Zd - Zt;
					if (std::fabs(Dz) <= kTileTolM)
					{
						++Cn.Agree;
					}
					else if (Zt > F.topZ)
					{
						++Cn.Band;
					}
					else
					{
						bFail = true;
						(Dz > 0.f ? Cn.Up : Cn.Down) += 1;
						Cn.MinCm = FMath::Min(Cn.MinCm, Dz * 100.f);
						Cn.MaxCm = FMath::Max(Cn.MaxCm, Dz * 100.f);
					}
				}
				else if (bT)
				{
					if (Zt > F.topZ)
					{
						++Cn.Band;
					}
					else if (Zt < F.topZ - Spec.depthM)
					{
						++Cn.BelowDraw;
					}
					else
					{
						bFail = true;
						++Cn.Lost;
					}
				}
				else if (Zd < TileFloor[Ti])
				{
					++Cn.BelowTruth;
				}
				else
				{
					bFail = true;
					++Cn.Gained;
				}
				if (!bFail)
				{
					continue;
				}
				Cn.Edge += (Tx == 0 || Tx == Res - 1 || Ty == 0 || Ty == Res - 1) ? 1 : 0;
				if (!Kind)
				{
					continue;
				}
				const float Zhi = bT && bD ? FMath::Max(Zt, Zd) : (bT ? Zt : Zd);
				const int32 Ci = Name(Ti, Wx, Wy, Zhi);
				if (Tally)
				{
					Tally->FindOrAdd(Ci) += 1;
				}
				if (T.CsvRows < kTruthCsvMax)
				{
					++T.CsvRows;
					T.Csv += FString::Printf(TEXT("%s,%d,%d,%d,%d,%.3f,%.3f,%s,%s,%s,%s,%s\n"), Kind, Item, Ti, Tx, Ty, Wx, Wy,
						bT ? *FString::Printf(TEXT("%.4f"), Zt) : TEXT(""), bD ? *FString::Printf(TEXT("%.4f"), Zd) : TEXT(""),
						bT && bD ? *FString::Printf(TEXT("%.2f"), (Zd - Zt) * 100.f) : (bT ? TEXT("lost") : TEXT("gained")),
						bT ? *CandId(Name(Ti, Wx, Wy, Zt)) : TEXT(""), bD ? *CandId(Name(Ti, Wx, Wy, Zd)) : TEXT(""));
				}
			}
		}
		return Cn;
	};
	const auto LogTally = [&](const TCHAR* What, const TMap<int32, int32>& Tally, const FVector& TileO, const FVector& RefO)
	{
		TArray<TPair<int32, int32>> L;
		for (const TPair<int32, int32>& P : Tally)
		{
			L.Add(P);
		}
		L.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B) { return A.Value > B.Value; });
		for (int32 J = 0; J < FMath::Min(3, L.Num()); ++J)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth name: %s #%d: %d texels: %s."), What, J + 1, L[J].Value,
				L[J].Key >= 0 ? *CandText(L[J].Key, TileO, RefO) : TEXT("no candidate box under it"));
		}
	};

	if (!T.bStarted)
	{
		T.bStarted = true;
		TruthSetUp(World);
		const FTruthPass& P0s = kTruthPasses[0];
		TruthSwitches(World, P0s.bLod0, P0s.bNoCull, P0s.bNoHlod);
		T.Wait = T.Settle;
		return;
	}
	if (--T.Wait > 0)
	{
		return;
	}
	const FTruthPass& P = kTruthPasses[T.Pass];
	const double P0 = FPlatformTime::Seconds();
	FTruthCount TileSum, RefSum, FarSum, StoredSum;
	int32 TilesFail = 0, RefsFail = 0, TruthIsTile = 0, TruthDiffers = 0, PlainTileDiff = 0, PlainRefDiff = 0, AgainDiff = 0, AgainTiles = 0;
	TArray<float> Draw;
	double E = 0.0, Rd = 0.0;
	for (int32 I = 0; I < NT; ++I)
	{
		if (TileFar[I].Num() != N)
		{
			continue;
		}
		const ftcore::RainCoverFrame F = ftcore::rainCoverTileFrame(Spec, TileList[I], TileTop[I]);
		if (!TileDraw(F, Spec.depthM, Draw, E, Rd))
		{
			continue;
		}
		if (T.Pass == 0)
		{
			T.TruthD[I] = Draw;
			int32 Diff = 0;
			for (int32 K = 0; K < N; ++K)
			{
				Diff += (Draw[K] != TileFar[I][K]) ? 1 : 0;
			}
			TruthDiffers += Diff;
			TruthIsTile += Diff == 0 ? 1 : 0;
			// The tile draw as stored (switches off) against truth: signed, named.
			TMap<int32, int32> Tally;
			const FTruthCount C = Compare(TileFar[I], F, I, TEXT("tilefar"), I, &Tally);
			FarSum.Add(C);
			TilesFail += C.Fails() > 0 ? 1 : 0;
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth tile: %d (%d, %d) top %.2f m: truth differs from the tile draw on %d texels; tile draw against truth: %s."),
				I, TileList[I].x, TileList[I].y, TileTop[I], Diff, *C.Text());
			if (C.Fails() > 0)
			{
				LogTally(*FString::Printf(TEXT("tile %d"), I), Tally, FVector(F.cx, F.cy, F.topZ) * 100.0, FVector(0, 0, -1e9));
			}
		}
		else if (T.Pass == kPassPlain)
		{
			int32 Diff = 0;
			for (int32 K = 0; K < N; ++K)
			{
				Diff += (Draw[K] != TileFar[I][K]) ? 1 : 0;
			}
			PlainTileDiff += Diff;
		}
		else if (T.Pass == kPassAgain)
		{
			int32 Diff = 0;
			for (int32 K = 0; K < N; ++K)
			{
				Diff += (Draw[K] != T.TruthD[I][K]) ? 1 : 0;
			}
			AgainDiff += Diff;
			AgainTiles += Diff > 0 ? 1 : 0;
		}
		else
		{
			const FTruthCount C = Compare(Draw, F, I, nullptr, I, nullptr);
			TileSum.Add(C);
			TilesFail += C.Fails() > 0 ? 1 : 0;
			if (C.Fails() > 0)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth %s tile: %d (%d, %d): %s."), P.Name, I, TileList[I].x, TileList[I].y, *C.Text());
			}
		}
	}
	for (int32 R = 0; R < T.RefD.Num(); ++R)
	{
		if (T.Pass == kPassAgain || T.RefD[R].Num() != N || !TileDraw(T.RefF[R], Spec.depthM, Draw, E, Rd))
		{
			continue;
		}
		const ftcore::RainCoverFrame& F = T.RefF[R];
		if (T.Pass == kPassPlain)
		{
			int32 Diff = 0;
			for (int32 K = 0; K < N; ++K)
			{
				Diff += (Draw[K] != T.RefD[R][K]) ? 1 : 0;
			}
			PlainRefDiff += Diff;
			continue;
		}
		if (T.Pass == 0)
		{
			// The shipped 60 m draw as stored (4b) against truth: signed, named.
			TMap<int32, int32> Tally;
			const FTruthCount C = Compare(T.RefD[R], F, -1, TEXT("ref60"), R, &Tally);
			StoredSum.Add(C);
			RefsFail += C.Fails() > 0 ? 1 : 0;
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth ref: %s, top %.2f m: shipped draw against truth: %s."), *T.RefLabel[R], F.topZ, *C.Text());
			if (C.Fails() > 0)
			{
				LogTally(*T.RefLabel[R], Tally, FVector(0, 0, -1e9), FVector(F.cx, F.cy, F.topZ) * 100.0);
			}
		}
		const FTruthCount C = Compare(Draw, F, -1, nullptr, R, nullptr);
		RefSum.Add(C);
		if (T.Pass != 0)
		{
			RefsFail += C.Fails() > 0 ? 1 : 0;
		}
		if (C.Fails() > 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth %s ref: %s: %s."), P.Name, *T.RefLabel[R], *C.Text());
		}
	}
	const double Ms = (FPlatformTime::Seconds() - P0) * 1000.0;
	if (T.Pass == 0)
	{
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover truth pass all (LOD0, no cull, no HLOD) in %.0f ms: truth equals the tile draw on %d of %d tiles (%d texels differ)%s. Tile draws against truth: %d tiles fail; %s. Shipped 60 m draws against truth: %d of %d fail; %s. The 60 m draw with all three switches against truth: %s."),
			Ms, TruthIsTile, NT, TruthDiffers, TruthDiffers == 0 ? TEXT(": NOT A CHECK, truth is the tile draw again") : TEXT(""), TilesFail,
			*FarSum.Text(), RefsFail, T.RefD.Num(), *StoredSum.Text(), *RefSum.Text());
		const FString Path = TileDir / TEXT("truth_fails.csv");
		const bool bSaved = FFileHelper::SaveStringToFile(T.Csv, *Path);
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth: %d failing texels %s to %s%s."), T.CsvRows, bSaved ? TEXT("saved") : TEXT("NOT saved"), *Path,
			T.CsvRows >= kTruthCsvMax ? TEXT(" (capped)") : TEXT(""));
		T.Csv.Empty();
	}
	else if (T.Pass == kPassAgain)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth pass again (all three switches, after the others) in %.0f ms: truth differs from the first on %d texels over %d tiles (0: truth is stable)."),
			Ms, AgainDiff, AgainTiles);
	}
	else if (T.Pass == kPassPlain)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth pass plain (switches restored) in %.0f ms: tile draws differ from the stored on %d texels, 60 m draws on %d (0 and 0: restored and repeatable)."),
			Ms, PlainTileDiff, PlainRefDiff);
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth pass %s in %.0f ms: tile draws against truth: %d tiles fail; %s. 60 m draws against truth: %d fail; %s."),
			P.Name, Ms, TilesFail, *TileSum.Text(), RefsFail, *RefSum.Text());
	}
	do
	{
		++T.Pass;
	} while (T.Pass < int32(UE_ARRAY_COUNT(kTruthPasses)) && (T.Mask & (1 << T.Pass)) == 0);
	if (T.Pass < int32(UE_ARRAY_COUNT(kTruthPasses)))
	{
		const FTruthPass& Q = kTruthPasses[T.Pass];
		TruthSwitches(World, Q.bLod0, Q.bNoCull, Q.bNoHlod);
		T.Wait = T.Settle;
		return;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover truth: done."));
	TileState = 4;
	FPlatformMisc::RequestExit(false, TEXT("RainCoverTruth"));
}

void UFTRainCoverSubsystem::StripTick(UWorld& World)
{
	FFTRainTruth& T = *Truth;
	const int32 Res = Spec.resolution, N = Res * Res;
	if (TileState == 10)
	{
		if (T.StripAt == 0)
		{
			T.StripT0 = FPlatformTime::Seconds();
		}
		TArray<uint16> Codes;
		Codes.SetNumUninitialized(N);
		for (int32 Done = 0; Done < kRcStripPerTick && T.StripAt < TileList.Num(); ++Done, ++T.StripAt)
		{
			const ftcore::RainCoverTile Tl = TileList[T.StripAt];
			const double B0 = FPlatformTime::Seconds();
			ftcore::RainCoverFrame F = ftcore::rainCoverTileFrame(Spec, Tl, kTileFindTopM);
			double E = 0.0, Rd = 0.0;
			if (!TileDraw(F, kTileFindFarM, TileScratch, E, Rd))
			{
				continue;
			}
			bool bAny = false;
			float MaxZ = 0.f;
			for (const float D : TileScratch)
			{
				if (D < kTileFindFarM)
				{
					MaxZ = bAny ? FMath::Max(MaxZ, kTileFindTopM - D) : kTileFindTopM - D;
					bAny = true;
				}
			}
			if (!bAny)
			{
				++T.NoHit;
				T.BuildMs += (FPlatformTime::Seconds() - B0) * 1000.0;
				continue;
			}
			F.topZ = MaxZ + kTileMarginM;
			if (!TileDraw(F, Spec.depthM, TileScratch, E, Rd))
			{
				continue;
			}
			ftcore::RainCoverR16Range Range;
			ftcore::rainCoverR16Pack(TileScratch.GetData(), N, F.topZ, Spec.depthM, Codes.GetData(), Range, kTileTolM);
			const bool bFlat = Range.hits > 0 && Range.maxZ - Range.minZ <= kTileTolM;
			const bool bFull = Range.hits == N;
			const int32 Kind = bFlat && bFull ? 0 : (bFlat ? 1 : 2);
			(Kind == 0 ? T.FlatFull : (Kind == 1 ? T.FlatPart : T.Textured)) += 1;
			// Flat and full: the 32-byte header only (one height). Else the header and the codes.
			TArray<uint8> Bytes;
			Bytes.SetNumUninitialized(32 + (Kind == 0 ? 0 : N * 2));
			uint8* B = Bytes.GetData();
			const int32 Head[2] = {Tl.x, Tl.y};
			const float HeadZ[3] = {F.topZ, Range.minZ, Range.maxZ};
			const int32 HeadN[2] = {Range.hits, Kind == 0 ? 0 : Res};
			FMemory::Memcpy(B, "FTRC", 4);
			FMemory::Memcpy(B + 4, Head, 8);
			FMemory::Memcpy(B + 12, HeadZ, 12);
			FMemory::Memcpy(B + 24, HeadN, 8);
			if (Kind != 0)
			{
				FMemory::Memcpy(B + 32, Codes.GetData(), N * 2);
			}
			FFileHelper::SaveArrayToFile(Bytes, *(TileDir / FString::Printf(TEXT("tile_%d_%d.r16"), Tl.x, Tl.y)));
			T.Bytes += Bytes.Num();
			T.BuildMs += (FPlatformTime::Seconds() - B0) * 1000.0;
			if (Kind == 0 && T.FlatAt.Num() < kRcStripNameMax)
			{
				T.FlatAt.Add(FVector(F.cx, F.cy, Range.minZ));
			}
		}
		if (T.StripAt < TileList.Num())
		{
			return;
		}
		const double Wall = (FPlatformTime::Seconds() - T.StripT0) * 1000.0;
		UE_LOG(LogForgottenThird, Display,
			TEXT("RainCover strip: %d tiles: flat and full (one height, no texture) %d, flat with open texels %d, textured %d, nothing found %d; build %.0f ms summed (%.0f ms wall over %d ticks); written %.1f MiB (textured and flat-open tiles %.1f MiB)."),
			TileList.Num(), T.FlatFull, T.FlatPart, T.Textured, T.NoHit, T.BuildMs, Wall, (TileList.Num() + kRcStripPerTick - 1) / kRcStripPerTick,
			double(T.Bytes) / (1024.0 * 1024.0), double(T.FlatPart + T.Textured) * double(32 + N * 2) / (1024.0 * 1024.0));
		// The flat sheet's maker: what stands at each named tile's centre within 0.15 m of the sheet's height, and the water.
		const UFTWaterSubsystem* Water = World.GetSubsystem<UFTWaterSubsystem>();
		const float Now = World.GetTimeSeconds();
		for (const FVector& At : T.FlatAt)
		{
			FString Who;
			int32 Count = 0;
			const FVector P = At * 100.0;
			for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
			{
				UStaticMeshComponent* C = *It;
				if (C->GetWorld() != &World || !C->IsRegistered() || !C->GetStaticMesh())
				{
					continue;
				}
				const auto Test = [&](const FBox& Bx, int32 Inst)
				{
					const double S = kTruthSlackM * 100.0;
					if (P.X < Bx.Min.X - S || P.X > Bx.Max.X + S || P.Y < Bx.Min.Y - S || P.Y > Bx.Max.Y + S || FMath::Abs(Bx.Max.Z - P.Z) > S)
					{
						return;
					}
					if (++Count <= 4)
					{
						const AActor* O = C->GetOwner();
						Who += FString::Printf(TEXT("%s%s %s/%s inst %d mesh %s (box %.1f x %.1f m)"), Who.IsEmpty() ? TEXT("") : TEXT("; "),
							*C->GetClass()->GetName(), O ? *O->GetName() : TEXT("-"), *C->GetName(), Inst, *C->GetStaticMesh()->GetName(),
							(Bx.Max.X - Bx.Min.X) * 0.01, (Bx.Max.Y - Bx.Min.Y) * 0.01);
					}
				};
				if (UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C))
				{
					const FBox MB = C->GetStaticMesh()->GetBoundingBox();
					for (int32 K = 0; K < I->GetInstanceCount(); ++K)
					{
						FTransform X;
						if (I->GetInstanceTransform(K, X, true))
						{
							Test(MB.TransformBy(X), K);
						}
					}
				}
				else
				{
					Test(C->Bounds.GetBox(), -1);
				}
			}
			float Surface = 0.f;
			const bool bWater = Water && Water->HasBodies() && Water->Bodies().surfaceHeight(ftcore::Vec2{float(At.X), float(At.Y)}, Now, Surface);
			UE_LOG(LogForgottenThird, Display, TEXT("RainCover strip sheet: at (%.1f, %.1f) z %.3f m: %d meshes: %s; water surface %s."), At.X, At.Y,
				At.Z, Count, Count ? *Who : TEXT("none"), bWater ? *FString::Printf(TEXT("%.3f m"), Surface) : TEXT("none here"));
		}
		TileState = 11;
		return;
	}
	// 5c: the cache key over every tile, three times. Per component: the mesh package's saved hash (editor builds; else its
	// name), the materials and their WPO flag, the cull and LOD settings, the transform; per instance its transform. Summed
	// order-free into the tiles its box touches.
	double Ms[3] = {0.0, 0.0, 0.0};
	int32 Prims = 0, Insts = 0, Buckets = 0;
	uint64 Check = 0;
	for (int32 Run = 0; Run < 3; ++Run)
	{
		const double K0 = FPlatformTime::Seconds();
		TMap<FIntPoint, uint64> Key;
		Prims = Insts = 0;
		const auto Bucket = [&](const FBox& Bx, uint64 H)
		{
			const ftcore::RainCoverTile A = ftcore::rainCoverTileAt(Spec, float(Bx.Min.X) * 0.01f, float(Bx.Min.Y) * 0.01f);
			const ftcore::RainCoverTile Z = ftcore::rainCoverTileAt(Spec, float(Bx.Max.X) * 0.01f, float(Bx.Max.Y) * 0.01f);
			for (int32 X = A.x; X <= Z.x && X - A.x < 64; ++X)
			{
				for (int32 Y = A.y; Y <= Z.y && Y - A.y < 64; ++Y)
				{
					Key.FindOrAdd(FIntPoint(X, Y)) += H;
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
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* C = *It;
			const UStaticMesh* M = C->GetStaticMesh();
			if (C->GetWorld() != &World || !C->IsRegistered() || !M)
			{
				continue;
			}
			++Prims;
			const UPackage* Pkg = M->GetPackage();
#if WITH_EDITORONLY_DATA
			const FIoHash PH = Pkg->GetSavedHash();
			uint64 H = CityHash64(reinterpret_cast<const char*>(&PH), uint32(sizeof(PH)));
#else
			const FString PN = Pkg->GetName();
			uint64 H = CityHash64(reinterpret_cast<const char*>(*PN), uint32(PN.Len() * sizeof(TCHAR)));
#endif
			for (int32 S = 0; S < C->GetNumMaterials(); ++S)
			{
				const UMaterialInterface* Mi = C->GetMaterial(S);
				const FString MN = Mi ? Mi->GetPathName() : FString();
				H = CityHash64WithSeed(reinterpret_cast<const char*>(*MN), uint32(MN.Len() * sizeof(TCHAR)), H);
				H = H * 1099511628211ull + (Mi && Mi->IsUsingWorldPositionOffset_Concurrent(GMaxRHIFeatureLevel) ? 1u : 2u);
			}
			const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C);
			const int32 Ints[6] = {C->ForcedLodModel, C->MinLOD, I ? I->InstanceStartCullDistance : -1, I ? I->InstanceEndCullDistance : -1,
				int32(C->LDMaxDrawDistance), int32(C->MinDrawDistance)};
			H = CityHash64WithSeed(reinterpret_cast<const char*>(Ints), uint32(sizeof(Ints)), H);
			H = HashT(C->GetComponentTransform(), H);
			if (I)
			{
				const FBox MB = M->GetBoundingBox();
				for (int32 K = 0; K < I->GetInstanceCount(); ++K)
				{
					FTransform X;
					if (I->GetInstanceTransform(K, X, true))
					{
						++Insts;
						Bucket(MB.TransformBy(X), HashT(X, H));
					}
				}
			}
			else
			{
				Bucket(C->Bounds.GetBox(), H);
			}
		}
		Ms[Run] = (FPlatformTime::Seconds() - K0) * 1000.0;
		Buckets = Key.Num();
		uint64 Sum = 0;
		for (const TPair<FIntPoint, uint64>& P : Key)
		{
			Sum += P.Value ^ (uint64(uint32(P.Key.X)) << 32 | uint64(uint32(P.Key.Y)));
		}
		if (Run == 0)
		{
			Check = Sum;
		}
		else if (Sum != Check)
		{
			Check = 0; // not repeatable
		}
	}
#if WITH_EDITORONLY_DATA
	const TCHAR* HashKind = TEXT("the editor's saved hash (a cooked build needs the cooker to bake one)");
#else
	const TCHAR* HashKind = TEXT("the package name only");
#endif
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover strip key: %d components, %d instances, %d tiles keyed; %.1f / %.1f / %.1f ms over three runs; repeatable %s; package hash %s."),
		Prims, Insts, Buckets, Ms[0], Ms[1], Ms[2], Check != 0 ? TEXT("yes") : TEXT("NO"), HashKind);
	TileState = 4;
	FPlatformMisc::RequestExit(false, TEXT("RainCoverTileStrip"));
}

void UFTRainCoverSubsystem::StreamTick(const FVector& Eye)
{
	FFTRainStream& S = *Stream;
	const int32 Res = Spec.resolution, N = Res * Res;
	// Start a read into buffer B: the next K textured tiles from disk, off the game thread.
	const auto StartRead = [&](int32 B)
	{
		TArray<FString> Names;
		for (int32 J = 0; J < S.K; ++J)
		{
			Names.Add(S.Files[(S.Next + J) % S.Files.Num()]);
		}
		S.Next = (S.Next + S.K) % S.Files.Num();
		uint16* Into = S.Buf[B].GetData();
		std::atomic<int64>* Last = &S.LastReadUs;
		S.Read[B] = Async(EAsyncExecution::ThreadPool, [Names = MoveTemp(Names), Into, N, Last]() -> int32
		{
			const double R0 = FPlatformTime::Seconds();
			int32 Good = 0;
			IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
			for (int32 J = 0; J < Names.Num(); ++J)
			{
				TUniquePtr<IFileHandle> H(PF.OpenRead(*Names[J]));
				if (H && H->Seek(32) && H->Read(reinterpret_cast<uint8*>(Into + J * N), int64(N) * 2))
				{
					++Good;
				}
			}
			Last->store(int64((FPlatformTime::Seconds() - R0) * 1e6));
			return Good;
		});
		S.bReading[B] = true;
	};
	// A finished read becomes the ready buffer.
	for (int32 B = 0; B < 2; ++B)
	{
		if (S.bReading[B] && S.Read[B].IsReady())
		{
			S.bReading[B] = false;
			const int32 Good = S.Read[B].Get();
			S.Short += Good == S.K ? 0 : 1;
			++S.Reads;
			const double Ms = double(S.LastReadUs.load()) / 1000.0;
			S.ReadMsSum += Ms;
			S.ReadMsMax = FMath::Max(S.ReadMsMax, Ms);
			S.Ready = B;
		}
	}
	// Nothing ready or reading: the next crossing's tiles into a buffer the render thread has let go, ahead of the crossing.
	if (S.Ready < 0 && !S.bReading[0] && !S.bReading[1])
	{
		const int32 B = S.Fence[S.Last ^ 1].IsFenceComplete() ? (S.Last ^ 1) : (S.Fence[S.Last].IsFenceComplete() ? S.Last : -1);
		if (B >= 0)
		{
			StartRead(B);
		}
		else
		{
			++S.Busy;
		}
	}
	const float Ex = float(Eye.X) * 0.01f, Ey = float(Eye.Y) * 0.01f;
	if (!ftcore::rainCoverRecapture(Spec, bUploadAny, UploadFrame.cx, UploadFrame.cy, Ex, Ey))
	{
		return;
	}
	UploadFrame = ftcore::rainCoverFrameAt(Spec, Ex, Ey, float(Eye.Z) * 0.01f);
	bUploadAny = true;
	if (S.Ready < 0)
	{
		++S.Late; // the read isn't back: skipped, never waited on
		return;
	}
	const int32 B = S.Ready;
	S.Ready = -1;
	S.Last = B;
	CSV_CUSTOM_STAT(RainCover, Upload, 1, ECsvCustomStatOp::Set);
	{
		CSV_SCOPED_TIMING_STAT(RainCover, UploadEnqueue);
		for (int32 J = 0; J < UploadTex.Num() && J < S.K; ++J)
		{
			FTextureResource* Into = UploadTex[J] ? UploadTex[J]->GetResource() : nullptr;
			if (!Into)
			{
				continue;
			}
			const uint8* Src = reinterpret_cast<const uint8*>(S.Buf[B].GetData() + J * N);
			ENQUEUE_RENDER_COMMAND(FTRainCoverStream)([Into, Src, Res](FRHICommandListImmediate& RHICmdList)
			{
				FRHITexture* Tex = Into->GetTexture2DRHI();
				if (Tex)
				{
					RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, uint32(Res), uint32(Res)), uint32(Res) * sizeof(uint16), Src);
				}
			});
		}
		S.Fence[B].BeginFence();
	}
	++Uploads;
}
#endif

TStatId UFTRainCoverSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTRainCoverSubsystem, STATGROUP_Tickables);
}
