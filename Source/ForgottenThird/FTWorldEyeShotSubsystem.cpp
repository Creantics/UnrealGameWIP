#include "FTWorldEyeShotSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicRHI.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "FTSceneLook.h"
#include "ForgottenThird.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "ShaderCompiler.h"

#include "ftcore/MassingPlan.h"

namespace
{
// How long the saved still may take to reach the disk, in ticks.
constexpr int32 kPendingTicks = 6000;

// "x,y,z" as three numbers, or false.
bool ParseTriple(const TCHAR* Key, double& X, double& Y, double& Z)
{
	FString Text;
	if (!FParse::Value(FCommandLine::Get(), Key, Text, false))
	{
		return false;
	}
	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT(","));
	if (Parts.Num() != 3 || !Parts[0].IsNumeric() || !Parts[1].IsNumeric() || !Parts[2].IsNumeric())
	{
		return false;
	}
	X = FCString::Atod(*Parts[0]);
	Y = FCString::Atod(*Parts[1]);
	Z = FCString::Atod(*Parts[2]);
	return true;
}
} // namespace

bool UFTWorldEyeShotSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	FString Shot;
	return World && World->WorldType == EWorldType::Game && FParse::Value(FCommandLine::Get(), TEXT("WorldEyeShot="), Shot) &&
		!Shot.IsEmpty() && Super::ShouldCreateSubsystem(Outer);
}

void UFTWorldEyeShotSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	FParse::Value(FCommandLine::Get(), TEXT("WorldEyeShot="), File);
	double EyeX = 0.0, EyeY = 0.0, EyeZ = 0.0, AtX = 0.0, AtY = 0.0, AtZ = 0.0;
	if (!ParseTriple(TEXT("WorldEye="), EyeX, EyeY, EyeZ) || !ParseTriple(TEXT("WorldEyeAt="), AtX, AtY, AtZ))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: -WorldEye=<x,y,z> and -WorldEyeAt=<x,y,z> are both needed."));
		Finish(1);
		return;
	}
	double Side = 1000.0;
	double Lens = 35.0;
	FParse::Value(FCommandLine::Get(), TEXT("WorldEyeSide="), Side);
	FParse::Value(FCommandLine::Get(), TEXT("WorldEyeLens="), Lens);
	FParse::Value(FCommandLine::Get(), TEXT("WorldEyeDelay="), Delay);
	FParse::Value(FCommandLine::Get(), TEXT("WorldEyeGpuMs="), GpuFrames);
	if (!FParse::Value(FCommandLine::Get(), TEXT("WorldEyeSize="), Size) || Size.IsEmpty())
	{
		Size = TEXT("2560x1440");
	}
	if (Lens <= 0.0 || Side <= 0.0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: -WorldEyeLens and -WorldEyeSide must be above 0."));
		Finish(1);
		return;
	}
	const ftcore::MassingVec EyeM = ftcore::massingEnginePoint(Side, EyeX, EyeY, EyeZ);
	const ftcore::MassingVec AtM = ftcore::massingEnginePoint(Side, AtX, AtY, AtZ);
	const FVector From(EyeM.x * 100.0, EyeM.y * 100.0, EyeM.z * 100.0);
	const FVector To(AtM.x * 100.0, AtM.y * 100.0, AtM.z * 100.0);
	const FRotator Facing = (To - From).Rotation();
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Eye = InWorld.SpawnActor<ACameraActor>(From, Facing, Spawn);
	if (!Eye)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: the eye's camera could not be spawned."));
		Finish(1);
		return;
	}
	// Horizontal field of view for the lens on a 36 mm-wide frame.
	const double Fov = FMath::RadiansToDegrees(2.0 * FMath::Atan(18.0 / Lens));
	UCameraComponent* Camera = Eye->GetCameraComponent();
	Camera->SetFieldOfView(static_cast<float>(Fov));
	Camera->SetConstraintAspectRatio(false);
	// The same name again only proves the new still landed once the old one is gone.
	IFileManager::Get().Delete(*File, false, true, true);
	bActive = true;
	UE_LOG(LogForgottenThird, Display,
		TEXT("WorldEyeShot: eye (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f) in the plan frame, %.1f mm (%.1f degrees across), "
			 "engine eye %s, yaw %.2f, pitch %.2f; %s to %s after %.1f s."),
		EyeX, EyeY, EyeZ, AtX, AtY, AtZ, Lens, Fov, *From.ToString(), Facing.Yaw, Facing.Pitch, *Size, *File, Delay);
}

void UFTWorldEyeShotSubsystem::Tick(float DeltaTime)
{
	if (!bActive)
	{
		return;
	}
	UWorld* World = GetWorld();
	APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
	if (Player && Eye)
	{
		// Anything that sets the view (the lead's own camera on possession) is overruled until the still is saved.
		if (Player->GetViewTarget() != Eye)
		{
			Player->SetViewTarget(Eye);
		}
		// The HUD's canvas would land in the still; the look is what is being judged.
		if (AHUD* Hud = Player->GetHUD())
		{
			Hud->bShowHUD = false;
		}
	}
	if (bShot)
	{
		if (IFileManager::Get().FileExists(*File))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: %s saved; quitting."), *File);
			Finish(0);
		}
		else if (++PendingTicks >= kPendingTicks)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: %s never arrived."), *File);
			Finish(1);
		}
		return;
	}
	// A material still compiling draws as the engine's default: the wait starts over until every shader is in.
	if (GShaderCompilingManager && GShaderCompilingManager->GetNumRemainingJobs() > 0)
	{
		Waited = 0.f;
		return;
	}
	if (!bLookApplied)
	{
		ApplyLook(*World);
		bLookApplied = true;
		Waited = 0.f;
		return;
	}
	Waited += DeltaTime;
	if (Waited < Delay || !Player)
	{
		return;
	}
	// The GPU's frame time over the settled view; the first frame's sample is a frame behind, so it is skipped.
	if (GpuSampled < GpuFrames + 1)
	{
		const double Ms = FPlatformTime::ToMilliseconds64(RHIGetGPUFrameCycles(0));
		if (GpuSampled > 0)
		{
			GpuSumMs += Ms;
			GpuMinMs = GpuSampled == 1 ? Ms : FMath::Min(GpuMinMs, Ms);
			GpuMaxMs = GpuSampled == 1 ? Ms : FMath::Max(GpuMaxMs, Ms);
		}
		if (++GpuSampled <= GpuFrames)
		{
			return;
		}
		const FIntPoint ViewSize = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
		UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: GPU %.3f ms a frame on average over %d frames (%.3f to %.3f) at %d x %d."),
			GpuSumMs / GpuFrames, GpuFrames, GpuMinMs, GpuMaxMs, ViewSize.X, ViewSize.Y);
	}
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
	if (!Viewport)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: no game viewport to shoot from."));
		Finish(1);
		return;
	}
	Viewport->Exec(World, *FString::Printf(TEXT("HighResShot %s filename=\"%s\""), *Size, *File), *GLog);
	UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: shooting after %.1f s settled."), Waited);
	bShot = true;
}

TStatId UFTWorldEyeShotSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTWorldEyeShotSubsystem, STATGROUP_Tickables);
}

void UFTWorldEyeShotSubsystem::ApplyLook(UWorld& World)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("WorldEyeNight")))
	{
		float SunShare = 0.01f, SkyShare = 0.05f, Ev = -2.f;
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightSun="), SunShare);
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightSky="), SkyShare);
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightEV="), Ev);
		const FString Path = FTScene::WindowCollection;
		UMaterialParameterCollection* Collection =
			LoadObject<UMaterialParameterCollection>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
		UMaterialParameterCollectionInstance* Windows = Collection ? World.GetParameterCollectionInstance(Collection) : nullptr;
		const bool bNight = Windows && Windows->SetScalarParameterValue(TEXT("Night"), 1.f);
		int32 Suns = 0, Skies = 0, Volumes = 0;
		for (TActorIterator<ADirectionalLight> It(&World); It; ++It)
		{
			ULightComponent* Light = It->GetLightComponent();
			Light->SetIntensity(Light->Intensity * SunShare);
			++Suns;
		}
		for (TActorIterator<ASkyLight> It(&World); It; ++It)
		{
			USkyLightComponent* Light = It->GetLightComponent();
			Light->SetIntensity(Light->Intensity * SkyShare);
			++Skies;
		}
		for (TActorIterator<APostProcessVolume> It(&World); It; ++It)
		{
			It->Settings.bOverride_AutoExposureMinBrightness = true;
			It->Settings.AutoExposureMinBrightness = Ev;
			It->Settings.bOverride_AutoExposureMaxBrightness = true;
			It->Settings.AutoExposureMaxBrightness = Ev;
			++Volumes;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: night: windows' Night %s; %d suns at %.3f and %d sky lights at %.3f of the map's; "
			"exposure locked at EV %.1f in %d volumes."), bNight ? TEXT("1") : TEXT("unset (no window collection)"), Suns, SunShare, Skies, SkyShare,
			Ev, Volumes);
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("WorldEyePlainWalls")))
	{
		TMap<UMaterialInterface*, UMaterialInterface*> Plain;
		int32 Swapped = 0;
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			if (It->GetWorld() != &World)
			{
				continue;
			}
			for (int32 Slot = 0; Slot < It->GetNumMaterials(); ++Slot)
			{
				UMaterialInterface* Material = It->GetMaterial(Slot);
				const FString Name = Material ? Material->GetName() : FString();
				if (!Name.StartsWith(TEXT("MIW_")))
				{
					continue;
				}
				UMaterialInterface*& To = Plain.FindOrAdd(Material);
				if (!To)
				{
					const FString Path = FTScene::SurfaceInstancePackage(Name.RightChop(4));
					To = LoadObject<UMaterialInterface>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
				}
				if (To)
				{
					It->SetMaterial(Slot, To);
					++Swapped;
				}
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: plain walls: %d slots of %d window instances back on their surfaces."), Swapped, Plain.Num());
	}
}

void UFTWorldEyeShotSubsystem::Finish(int32 Code)
{
	bActive = false;
	FTRequestExit(Code, TEXT("WorldEyeShot"));
}
