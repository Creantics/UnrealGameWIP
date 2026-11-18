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
#include "TextureResource.h"

#include "ftcore/LookTable.h"

#include <string_view>

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
	// Straight down (pitch -90, yaw 0): the image's right runs along +Y, its down along -X (ftcore/RainCover.h's frame).
	C->ProjectionType = ECameraProjectionMode::Orthographic;
	C->OrthoWidth = Spec.sizeM * 100.f;
	// Depth measured from the capture itself (occluder Z = top Z - depth): no automatic plane shift.
	C->bAutoCalculateOrthoPlanes = false;
	C->bUpdateOrthoPlanes = false;
	C->CaptureSource = SCS_SceneDepth;
	C->TextureTarget = Target;
	C->bCaptureEveryFrame = false;
	C->bCaptureOnMovement = false;
	C->MaxViewDistanceOverride = Spec.depthM * 100.f;
	// Only what can shelter: opaque static and instanced meshes.
	C->ShowFlags.SetSkeletalMeshes(false);
	C->ShowFlags.SetParticles(false);
	C->ShowFlags.SetTranslucency(false);
	C->ShowFlags.SetDecals(false);
	C->ShowFlags.SetFog(false);
	C->ShowFlags.SetVolumetricFog(false);
	C->ShowFlags.SetAtmosphere(false);
	C->ShowFlags.SetLandscape(false);
	C->ShowFlags.SetDynamicShadows(false);
	C->ShowFlags.SetPostProcessing(false);

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
	if (!ftcore::rainCoverRecapture(Spec, bHaveCapture, Frame.cx, Frame.cy, float(Eye.X) * 0.01f, float(Eye.Y) * 0.01f))
	{
		return;
	}
	Frame = ftcore::rainCoverFrameAt(Spec, float(Eye.X) * 0.01f, float(Eye.Y) * 0.01f, float(Eye.Z) * 0.01f);
	Capture->SetActorLocation(FVector(Frame.cx, Frame.cy, Frame.topZ) * 100.0);
	Capture->GetCaptureComponent2D()->CaptureScene();
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

TStatId UFTRainCoverSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTRainCoverSubsystem, STATGROUP_Tickables);
}
