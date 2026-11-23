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
#if !UE_BUILD_SHIPPING
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "RHIStats.h"
#include "RenderingThread.h"
#endif

#include "ftcore/LookTable.h"

#include <string_view>

bool UFTRainCoverSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) && Super::ShouldCreateSubsystem(Outer);
}

bool UFTRainCoverSubsystem::IsTickable() const
{
#if !UE_BUILD_SHIPPING
	return bActive || bCost;
#else
	return bActive;
#endif
}

void UFTRainCoverSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
#if !UE_BUILD_SHIPPING
	{
		const TCHAR* Switches = FTLookSwitches();
		bEveryFrame = FParse::Param(Switches, TEXT("RainCoverEveryFrame"));
		bReadback = FParse::Param(Switches, TEXT("RainCoverReadback"));
		bCost = bEveryFrame || bReadback || FParse::Param(Switches, TEXT("RainCoverCost"));
	}
#endif
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
#if !UE_BUILD_SHIPPING
	if (bReadback)
	{
		Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("FTRainCoverReadback"));
		CoverCpu.SetNumZeroed(Spec.resolution * Spec.resolution);
	}
	if (bCost)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover cost: every frame %d, readback %d."), bEveryFrame ? 1 : 0, bReadback ? 1 : 0);
	}
#endif
	UE_LOG(LogForgottenThird, Display, TEXT("RainCover: on, %.0f m at %d, recapture every %.1f m."), Spec.sizeM, Spec.resolution,
		Spec.recaptureM);
}

void UFTRainCoverSubsystem::Deinitialize()
{
	if (bActive)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("RainCover: %d captures."), Captures);
	}
#if !UE_BUILD_SHIPPING
	if (bCost)
	{
		CostSummary();
	}
	Readback.Reset();
#endif
	bActive = false;
	Super::Deinitialize();
}

void UFTRainCoverSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
#if !UE_BUILD_SHIPPING
	if (bCost)
	{
		++CostTicks;
		if (CostTicks == 1 || CostTicks == 600)
		{
			CostMemory(CostTicks == 1 ? TEXT("tick 1") : TEXT("tick 600"));
		}
		if (bReadback)
		{
			CostReadbackPoll();
		}
	}
#endif
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
	bool bRecapture = ftcore::rainCoverRecapture(Spec, bHaveCapture, Frame.cx, Frame.cy, float(Eye.X) * 0.01f, float(Eye.Y) * 0.01f);
#if !UE_BUILD_SHIPPING
	bRecapture = bRecapture || bEveryFrame;
#endif
	if (!bRecapture)
	{
		return;
	}
	Frame = ftcore::rainCoverFrameAt(Spec, float(Eye.X) * 0.01f, float(Eye.Y) * 0.01f, float(Eye.Z) * 0.01f);
	Capture->SetActorLocation(FVector(Frame.cx, Frame.cy, Frame.topZ) * 100.0);
#if !UE_BUILD_SHIPPING
	const double CallStart = FPlatformTime::Seconds();
#endif
	Capture->GetCaptureComponent2D()->CaptureScene();
#if !UE_BUILD_SHIPPING
	if (bCost)
	{
		const double Call = FPlatformTime::Seconds() - CallStart;
		CallSum += Call;
		CallMax = FMath::Max(CallMax, Call);
	}
	if (bReadback)
	{
		CostReadbackIssue();
	}
#endif
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

#if !UE_BUILD_SHIPPING
void UFTRainCoverSubsystem::CostMemory(const TCHAR* When) const
{
	FTextureMemoryStats Tex;
	RHIGetTextureMemoryStats(Tex);
	const FPlatformMemoryStats Mem = FPlatformMemory::GetStats();
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover cost memory at %s: cover map %s, RHI non-streaming textures %.1f MiB, streaming %.1f MiB, process physical %.1f MiB."),
		When, bActive ? TEXT("on") : TEXT("off"), double(Tex.NonStreamingMemorySize) / 1048576.0, double(Tex.StreamingMemorySize) / 1048576.0,
		double(Mem.UsedPhysical) / 1048576.0);
}

void UFTRainCoverSubsystem::CostReadbackIssue()
{
	if (bReadbackInFlight)
	{
		++ReadbacksSkipped; // one copy at a time: the next capture is read when this one lands
		return;
	}
	FTextureRenderTargetResource* Resource = Target ? Target->GameThread_GetRenderTargetResource() : nullptr;
	if (!Resource || !Readback)
	{
		return;
	}
	FRHIGPUTextureReadback* Copy = Readback.Get();
	ENQUEUE_RENDER_COMMAND(FTRainCoverReadbackIssue)([Copy, Resource](FRHICommandListImmediate& RHICmdList)
	{
		Copy->EnqueueCopy(RHICmdList, Resource->GetRenderTargetTexture());
	});
	bReadbackInFlight = true;
	ReadbackIssued = GFrameCounter;
	++ReadbacksIssued;
}

void UFTRainCoverSubsystem::CostReadbackPoll()
{
	if (!bReadbackInFlight || !Readback || !Readback->IsReady())
	{
		return;
	}
	bReadbackInFlight = false;
	const int32 Latency = int32(GFrameCounter - ReadbackIssued);
	LatencySum += Latency;
	LatencyMax = FMath::Max(LatencyMax, Latency);
	++ReadbacksDone;
	// Lock and copy on the render thread (render commands run in order, so this lands before the next copy is issued).
	FRHIGPUTextureReadback* Copy = Readback.Get();
	float* Out = CoverCpu.GetData();
	const int32 Res = Spec.resolution;
	double* Sum = &CopySum;
	double* Max = &CopyMax;
	ENQUEUE_RENDER_COMMAND(FTRainCoverReadbackLock)([Copy, Out, Res, Sum, Max](FRHICommandListImmediate&)
	{
		const double Start = FPlatformTime::Seconds();
		int32 RowPitch = 0;
		const float* In = static_cast<const float*>(Copy->Lock(RowPitch));
		if (In)
		{
			for (int32 Y = 0; Y < Res; ++Y)
			{
				FMemory::Memcpy(Out + Y * Res, In + Y * RowPitch, Res * sizeof(float));
			}
			Copy->Unlock();
		}
		const double T = FPlatformTime::Seconds() - Start;
		*Sum += T;
		*Max = FMath::Max(*Max, T);
	});
}

void UFTRainCoverSubsystem::CostSummary()
{
	FlushRenderingCommands();
	const int32 Res = Spec.resolution;
	UE_LOG(LogForgottenThird, Display,
		TEXT("RainCover cost: %d ticks, %d captures; CaptureScene call (game thread) mean %.3f ms max %.3f ms; readback %d issued %d done %d skipped, latency mean %.2f max %d frames, lock+copy (render thread) mean %.3f ms max %.3f ms; target %dx%d R32F = %.2f MiB, CPU copy %.2f MiB."),
		CostTicks, Captures, Captures ? 1000.0 * CallSum / Captures : 0.0, 1000.0 * CallMax, ReadbacksIssued, ReadbacksDone, ReadbacksSkipped,
		ReadbacksDone ? double(LatencySum) / ReadbacksDone : 0.0, LatencyMax, ReadbacksDone ? 1000.0 * CopySum / ReadbacksDone : 0.0,
		1000.0 * CopyMax, Res, Res, double(Res) * Res * 4.0 / 1048576.0, double(CoverCpu.Num()) * 4.0 / 1048576.0);
}
#endif

TStatId UFTRainCoverSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTRainCoverSubsystem, STATGROUP_Tickables);
}
