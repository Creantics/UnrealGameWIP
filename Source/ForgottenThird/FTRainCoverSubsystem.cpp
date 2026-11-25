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
#endif

#include "ftcore/LookTable.h"

#include <cmath>
#include <string_view>

CSV_DEFINE_CATEGORY(RainCover, true);

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
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover tiles: probe %s, %d tiles and %d seam points from %s."),
			bTiles ? TEXT("on") : TEXT("off"), TileList.Num(), SeamList.Num(), *TileFile);
	}
	bUpload = FParse::Param(Switches, TEXT("RainCoverUploadProbe"));
	if (bUpload)
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
	if (bActive)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover: %d captures."), Captures);
	}
#if !UE_BUILD_SHIPPING
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
	if (!World || !Capture)
	{
		return;
	}
	const APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
	if (bProbe && Captures == 1)
	{
		Probe(Eye); // the frame after the first capture: it has rendered
	}
#if !UE_BUILD_SHIPPING
	if (bUpload) // the upload cell: no scene capture
	{
		UploadTick(Eye);
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

void UFTRainCoverSubsystem::TileCompareRef(const TCHAR* Label, int32 Own, float X, float Y)
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
		TileState = 1;
		return;
	case 1: // the load-time loop, in one tick, from the camera's own place
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
			TileCompareRef(*FString::Printf(TEXT("tile %d (%d, %d)"), I, T.x, T.y), I, F.cx, F.cy);
		}
		else
		{
			const FVector2f S = SeamList[TileAt - TileList.Num()];
			TileOn.Reset();
			TileCompareRef(*FString::Printf(TEXT("seam %d"), TileAt - TileList.Num()), -1, S.X, S.Y);
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
		TileState = 4;
		FPlatformMisc::RequestExit(false, TEXT("RainCoverTileProbe"));
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
#endif

TStatId UFTRainCoverSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTRainCoverSubsystem, STATGROUP_Tickables);
}
