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
#include "Misc/Parse.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "TextureResource.h"
#if !UE_BUILD_SHIPPING
#include "EngineUtils.h"
#include "FTWaterSubsystem.h"
#include "GameFramework/Pawn.h"
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
	// Lighting, GI, reflections, AO and shadows.
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
	F.SetVolumetricLightmap(false);
	F.SetIndirectLightingCache(false);
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

	bProbe = FParse::Param(FTLookSwitches(), TEXT("RainCoverProbe"));
	bActive = true;
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover: on, %.0f m at %d, recapture every %.1f m."), Spec.sizeM, Spec.resolution,
		Spec.recaptureM);
}

void UFTRainCoverSubsystem::Deinitialize()
{
	if (bActive)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover: %d captures."), Captures);
	}
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
	Frame = ftcore::rainCoverFrameAt(Spec, float(Eye.X) * 0.01f, float(Eye.Y) * 0.01f, float(Eye.Z) * 0.01f);
	Capture->SetActorLocation(FVector(Frame.cx, Frame.cy, Frame.topZ) * 100.0);
	Shoot(*Capture);
	bHaveCapture = true;
	++Captures;
	if (UMaterialParameterCollectionInstance* P = FTRainCover::Params(*World))
	{
		P->SetVectorParameterValue(FTRainCover::Frame(),
			FLinearColor(Frame.cx * 100.f, Frame.cy * 100.f, Frame.sizeM * 100.f, Frame.topZ * 100.f));
		P->SetScalarParameterValue(FTRainCover::On(), 1.f);
	}
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
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover diff: from (%.1f, %.1f), top %.2f m, 9 squares %.0f m apart."), DiffOrigin.cx,
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
		Shoot(*Capture);
	};
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("RainCoverDiff");
	switch (DiffStep)
	{
	case 0: // a fresh square: both captures moved there and drawn once
		DiffAt = DiffOrigin;
		DiffAt.cx += float(DiffSquare % 3 - 1) * Spec.sizeM;
		DiffAt.cy += float(DiffSquare / 3 - 1) * Spec.sizeM;
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
		if (++DiffSquare >= 9)
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
#endif

TStatId UFTRainCoverSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTRainCoverSubsystem, STATGROUP_Tickables);
}
