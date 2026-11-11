#include "FTWorldEyeShotSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicRHI.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "FTLampPoolSubsystem.h"
#include "FTLookSubsystem.h"
#include "FTNightLightData.h"
#include "FTNightLook.h"
#include "FTSceneLook.h"
#include "FTWorldClockSubsystem.h"
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
#include "Misc/Paths.h"
#include "ShaderCompiler.h"
#include "UnrealClient.h"

#include "ftcore/LookTable.h"
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
	double WalkX = 0.0, WalkY = 0.0, WalkZ = 0.0;
	if (ParseTriple(TEXT("WorldEyeWalkTo="), WalkX, WalkY, WalkZ))
	{
		float Speed = 1.4f, Fps = 60.f;
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeWalkSpeed="), Speed);
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeWalkFps="), Fps);
		const ftcore::MassingVec WalkM = ftcore::massingEnginePoint(Side, WalkX, WalkY, WalkZ);
		WalkFrom = From;
		WalkTo = FVector(WalkM.x * 100.0, WalkM.y * 100.0, WalkM.z * 100.0);
		WalkFrames = FMath::Clamp(FMath::CeilToInt32(FVector::Dist(WalkFrom, WalkTo) / 100.0 / FMath::Max(Speed, 0.05f) * FMath::Max(Fps, 1.f)), 1, 3600);
		bWalk = true;
		UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: walk to engine %s at %.2f m/s, %d frames at %.0f a second."), *WalkTo.ToString(), Speed, WalkFrames, Fps);
	}
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
	if (GpuFrames > 0 && GpuSampled < GpuFrames + 1)
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
	// The walk: one step and one frame a tick, then the still from the far end.
	if (bWalk && Eye && WalkFrame < WalkFrames)
	{
		++WalkFrame;
		Eye->SetActorLocation(FMath::Lerp(WalkFrom, WalkTo, static_cast<double>(WalkFrame) / WalkFrames));
		FScreenshotRequest::RequestScreenshot(FString::Printf(TEXT("%s_f%04d.png"), *FPaths::ChangeExtension(File, TEXT("")), WalkFrame), false, false);
		return;
	}
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
	if (!Viewport)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: no game viewport to shoot from."));
		Finish(1);
		return;
	}
	// -WorldEyePathTrace=<samples>: a reference still. The path tracer takes the settled view over, one sample a frame
	// with the camera still, and the still is the viewport itself (a HighResShot would restart the samples), so its
	// size is the window's (-ResX, -ResY).
	static int32 PathSamples = -1;
	static int32 PathFrames = 0;
	if (PathSamples < 0)
	{
		PathSamples = 0;
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyePathTrace="), PathSamples);
		if (PathSamples > 0)
		{
			Viewport->EngineShowFlags.SetPathTracing(true);
			Viewport->Exec(World, *FString::Printf(TEXT("r.PathTracing.SamplesPerPixel %d"), PathSamples), *GLog);
			Viewport->Exec(World, TEXT("r.PathTracing.MaxBounces 8"), *GLog);
			UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: path tracing, %d samples a pixel."), PathSamples);
		}
	}
	if (PathSamples > 0)
	{
		// A few frames over the sample count, for the denoiser and the frame the samples finish on.
		if (++PathFrames < PathSamples + 8)
		{
			return;
		}
		FScreenshotRequest::RequestScreenshot(File, false, false);
		UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: path-traced still after %d frames."), PathFrames);
		bShot = true;
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
	// A day still of a Solhaven map (or any map given -WorldEyeSunHour) takes the clock's sun at its start hour (or at
	// -WorldEyeSunHour), the one a played day starts with, whatever sun the map was built with; a running clock
	// (-TimeOfDay) keeps its own.
	const UFTWorldClockSubsystem* DayClock = World.GetSubsystem<UFTWorldClockSubsystem>();
	float SunHour = static_cast<float>(ftcore::kWorldClockStartHours);
	bool bSunHour = FParse::Value(FCommandLine::Get(), TEXT("WorldEyeSunHour="), SunHour);
	// -WorldEyeDusk: the evening hour the sun stands at the dusk's peak (FTNightLook.h).
	if (!bSunHour && FParse::Param(FCommandLine::Get(), TEXT("WorldEyeDusk")))
	{
		SunHour = static_cast<float>(ftcore::duskPeakHour(FTNight::Dusk(), ftcore::WorldClockSpec{}.latitudeDeg));
		bSunHour = true;
	}
	const FTCHARToUTF8 MapName(*World.GetMapName());
	const bool bSolhaven =
		ftcore::lookWorldForMap(std::string_view(MapName.Get(), static_cast<size_t>(MapName.Length()))) == "solhaven";
	if (!FParse::Param(FCommandLine::Get(), TEXT("WorldEyeNight")) && (bSolhaven || bSunHour) && !(DayClock && DayClock->IsRunning()))
	{
		const FRotator SunAt = FTClock::SunRotation(ftcore::WorldClockSpec{}, SunHour);
		int32 Turned = 0;
		for (TActorIterator<ADirectionalLight> It(&World); It; ++It)
		{
			if (ULightComponent* Light = It->GetLightComponent())
			{
				Light->SetMobility(EComponentMobility::Movable);
				Light->SetWorldRotation(SunAt);
				++Turned;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: day sun at %.2f h (pitch %.1f, yaw %.1f) on %d directional lights."),
			SunHour, SunAt.Pitch, SunAt.Yaw, Turned);
		// A low sun's hour takes the clock's dusk and dimming night too (sky, lens, fill, fog, windows).
		const FTNight::FSkyFrame Sky = FTNight::FrameAt(ftcore::WorldClockSpec{}, SunHour);
		if (bSunHour && (Sky.Dusk.amount > 0.f || Sky.NightAmount > 0.f))
		{
			FTNight::ApplyStill(World, Sky);
		}
	}
	// The ground's wetness on a Solhaven map, as the running clock sets it: the look's own times the night frame's share
	// (damp by day, full at night).
	if (bSolhaven)
	{
		const bool bWetNight = FParse::Param(FCommandLine::Get(), TEXT("WorldEyeNight"));
		const UFTLookSubsystem* Look = World.GetSubsystem<UFTLookSubsystem>();
		const FString LookPath = FTScene::LookCollection;
		UMaterialParameterCollection* LookCollection = LoadObject<UMaterialParameterCollection>(
			nullptr, *(LookPath + TEXT(".") + FPackageName::GetShortName(LookPath)), nullptr, LOAD_NoWarn | LOAD_Quiet);
		UMaterialParameterCollectionInstance* LookParams = LookCollection ? World.GetParameterCollectionInstance(LookCollection) : nullptr;
		const float Wet = Look ? Look->GetLook().wetness * ftcore::nightFrame(FTNight::Look(), bWetNight ? 1.f : 0.f).wetShare : 0.f;
		const bool bWet = Look && LookParams && LookParams->SetScalarParameterValue(TEXT("Wetness"), Wet);
		UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: wetness %.3f %s."), Wet, bWet ? TEXT("written") : TEXT("not written (no look or MPC_Look)"));
		// For a still alone (the clock does not run under a still): -WindowParallax=<0..1>, 0 shows every room and shop
		// display case flat; -ShopDayLevel= and -ShopNightLevel=.
		const TCHAR* const Switches[] = {TEXT("WindowParallax="), TEXT("ShopDayLevel="), TEXT("ShopNightLevel=")};
		const TCHAR* const Names[] = {TEXT("Parallax"), TEXT("ShopDayLevel"), TEXT("ShopNightLevel")};
		for (int32 S = 0; S < 3; ++S)
		{
			float Value = -1.f;
			if (FParse::Value(FCommandLine::Get(), Switches[S], Value) && Value >= 0.f)
			{
				const FString WindowPath = FTScene::WindowCollection;
				UMaterialParameterCollection* WindowCollection = LoadObject<UMaterialParameterCollection>(
					nullptr, *(WindowPath + TEXT(".") + FPackageName::GetShortName(WindowPath)), nullptr, LOAD_NoWarn | LOAD_Quiet);
				UMaterialParameterCollectionInstance* WindowParams = WindowCollection ? World.GetParameterCollectionInstance(WindowCollection) : nullptr;
				const bool bSet = WindowParams && WindowParams->SetScalarParameterValue(Names[S], S < 1 ? FMath::Clamp(Value, 0.f, 1.f) : Value);
				UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: window collection %s %.2f %s."), Names[S], Value, bSet ? TEXT("written") : TEXT("not written"));
			}
		}
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("WorldEyeNight")))
	{
		// The night spec the running clock reaches at full night (FTNightLook.h), so a still and a played night match: the
		// moon's share of the map's sun, the sky fill's share of the map's sky light (net), the exposure, the lamps' and
		// shop lights' candelas and reach in metres, the fog's kept colour and the city's glow. The -WorldEyeNight*
		// switches below override it for a still alone.
		const ftcore::NightLook& Spec = FTNight::Look();
		const ftcore::NightFrame Frame = ftcore::nightFrame(Spec, 1.f);
		float SunShare = Spec.moonShare, SkyFill = Spec.skyFill, Ev = Spec.exposureEV100, LampCd = Spec.lampCandela,
			  LampRadius = Spec.lampReachM, ShopCd = Spec.shopCandela, ShopRadius = Spec.shopReachM, Glow = -1.f;
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightSun="), SunShare);
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightSky="), SkyFill);
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightEV="), Ev);
		float EvRange = Spec.exposureRangeEV;
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightEVRange="), EvRange);
		EvRange = FMath::Max(EvRange, 0.f);
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightLamp="), LampCd);
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightLampRadius="), LampRadius);
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightShop="), ShopCd);
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightShopRadius="), ShopRadius);
		FParse::Value(FCommandLine::Get(), TEXT("WorldEyeNightGlow="), Glow);
		SunShare = FMath::Max(SunShare, 1e-4f);
		const FString Path = FTScene::WindowCollection;
		UMaterialParameterCollection* Collection =
			LoadObject<UMaterialParameterCollection>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
		UMaterialParameterCollectionInstance* Windows = Collection ? World.GetParameterCollectionInstance(Collection) : nullptr;
		const bool bNight = Windows && Windows->SetScalarParameterValue(TEXT("Night"), 1.f);
		const bool bGlow = Glow >= 0.f && Windows && Windows->SetScalarParameterValue(TEXT("NightGlow"), Glow);
		int32 Suns = 0, Skies = 0, Volumes = 0;
		for (TActorIterator<ADirectionalLight> It(&World); It; ++It)
		{
			ULightComponent* Light = It->GetLightComponent();
			// On a Solhaven map the moon stands where the clock puts it at midnight (or -WorldEyeSunHour), not at the
			// built sun's angle.
			if (bSolhaven || bSunHour)
			{
				Light->SetMobility(EComponentMobility::Movable);
				Light->SetWorldRotation(FTClock::SunRotation(ftcore::WorldClockSpec{}, bSunHour ? SunHour : 0.f));
			}
			Light->SetIntensity(Light->Intensity * SunShare);
			Light->SetLightColor(FTNight::Color(Spec.moonTint), false);
			++Suns;
		}
		// A real-time capture of the atmosphere already dims (and tints) with the sun, so it is lifted back by the moon's
		// share to land at SkyFill of the map's: the blue fill the street keeps where no lamp reaches.
		for (TActorIterator<ASkyLight> It(&World); It; ++It)
		{
			USkyLightComponent* Light = It->GetLightComponent();
			Light->SetIntensity(Light->Intensity * (Light->bRealTimeCapture ? SkyFill / SunShare : SkyFill));
			Light->SetLightColor(FTNight::Color(Spec.skyTint));
			++Skies;
		}
		// The height fog keeps its share of its own colour and takes the city's warm glow, as the clock's full night.
		int32 Fogs = 0;
		const FLinearColor FogGlow(Frame.fogGlow[0], Frame.fogGlow[1], Frame.fogGlow[2]);
		for (TActorIterator<AExponentialHeightFog> It(&World); It; ++It)
		{
			if (UExponentialHeightFogComponent* Fog = It->GetComponent())
			{
				Fog->SetFogInscatteringColor(Fog->FogInscatteringLuminance * Frame.fogKeep + FogGlow);
				++Fogs;
			}
		}
		// The street lamps and shop spill lights the massing build placed, off until now (on the actor's tag or the
		// light's own), and the lamps' heads. The build's lamps are bright enough for a street at a city night's exposure;
		// at this one they would burn out, so they take LampCd.
		int32 Lamps = 0, Shops = 0, Heads = 0, Lenses = 0;
		TArray<FVector> LampAt;
		const FString LampPath = FTScene::NightLampMaterial;
		UMaterialInterface* LampMaterial = LoadObject<UMaterialInterface>(nullptr,
			*(LampPath + TEXT(".") + FPackageName::GetShortName(LampPath)), nullptr, LOAD_NoWarn | LOAD_Quiet);
		// A map whose lights are data (AFTNightLights) lights the nearest of them from the lamp pool, forced to night at
		// the same values, instead of the tagged light actors; its lens instances already wear the lamp material.
		UFTLampPoolSubsystem* Pool = World.GetSubsystem<UFTLampPoolSubsystem>();
		const bool bPooled = Pool && Pool->ForceNight(LampCd, LampRadius * 100.f, ShopCd, ShopRadius * 100.f);
		if (bPooled)
		{
			Lamps = Pool->GetLampCount();
			Shops = Pool->GetShopCount();
			for (TActorIterator<AFTNightLights> It(&World); It; ++It)
			{
				UInstancedStaticMeshComponent* Mesh = It->LampHeads;
				if (Mesh && LampMaterial && Mesh->GetInstanceCount() > 0)
				{
					Mesh->SetMaterial(0, LampMaterial);
					Heads += Mesh->GetInstanceCount();
				}
			}
			UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: night from the lamp pool: %d+%d lights over %d lamps and %d shops."),
				Pool->GetLampPoolSize(), Pool->GetShopPoolSize(), Lamps, Shops);
		}
		for (TActorIterator<AActor> It(&World); !bPooled && It; ++It)
		{
			const bool bLampActor = It->ActorHasTag(FTScene::NightLampTag);
			const bool bShopActor = It->ActorHasTag(FTScene::NightShopTag);
			const bool bHeadActor = It->ActorHasTag(FTScene::NightLampHeadTag);
			TInlineComponentArray<UActorComponent*> Components(*It);
			for (UActorComponent* Component : Components)
			{
				ULightComponent* Light = Cast<ULightComponent>(Component);
				const bool bLamp = Light && (bLampActor || Component->ComponentHasTag(FTScene::NightLampTag));
				const bool bShop = Light && !bLamp && (bShopActor || Component->ComponentHasTag(FTScene::NightShopTag));
				if (bLamp || bShop)
				{
					Light->SetMobility(EComponentMobility::Movable);
					Light->SetVisibility(true);
					Light->SetHiddenInGame(false);
					Light->SetCastShadows(false);
					if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(Light))
					{
						Local->SetIntensityUnits(ELightUnits::Candelas);
						Local->SetAttenuationRadius((bLamp ? LampRadius : ShopRadius) * 100.f);
					}
					Light->SetIntensity(bLamp ? LampCd : ShopCd);
					Light->SetLightColor(FLinearColor::White);
					Light->SetUseTemperature(true);
					Light->SetTemperature(bLamp ? Spec.lampKelvin : Spec.shopKelvin);
					if (bLamp)
					{
						LampAt.Add(Light->GetComponentLocation());
						++Lamps;
					}
					else
					{
						++Shops;
					}
				}
				UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component);
				if (Mesh && LampMaterial && (bHeadActor || Mesh->ComponentHasTag(FTScene::NightLampHeadTag)))
				{
					for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
					{
						Mesh->SetMaterial(Slot, LampMaterial);
					}
					++Heads;
				}
			}
		}
		// A map whose lamps carry no head of their own gets a stand-in: a glowing lens, 30 cm across and 2 cm thick, hung
		// just under each lamp's light (which sits just under the lens), clear of the head's mesh.
		UStaticMesh* Disc = (!bPooled && Heads == 0 && LampMaterial && LampAt.Num() > 0)
			? LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"), nullptr, LOAD_NoWarn | LOAD_Quiet)
			: nullptr;
		for (int32 L = 0; Disc && L < LampAt.Num(); ++L)
		{
			const FVector At = LampAt[L];
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AStaticMeshActor* Lens = World.SpawnActor<AStaticMeshActor>(At - FVector(0.0, 0.0, 2.0), FRotator::ZeroRotator, Spawn);
			if (!Lens)
			{
				continue;
			}
			UStaticMeshComponent* Mesh = Lens->GetStaticMeshComponent();
			Mesh->SetMobility(EComponentMobility::Movable);
			Mesh->SetStaticMesh(Disc);
			Mesh->SetMaterial(0, LampMaterial);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->SetCastShadow(false);
			Lens->SetActorScale3D(FVector(0.3, 0.3, 0.02));
			++Lenses;
		}
		for (TActorIterator<APostProcessVolume> It(&World); It; ++It)
		{
			// The night's exposure range round Ev (as the clock's from half night) and its white balance.
			It->Settings.bOverride_AutoExposureMinBrightness = true;
			It->Settings.AutoExposureMinBrightness = Ev - EvRange;
			It->Settings.bOverride_AutoExposureMaxBrightness = true;
			It->Settings.AutoExposureMaxBrightness = Ev + EvRange;
			It->Settings.bOverride_WhiteTemp = true;
			It->Settings.WhiteTemp = Frame.whiteKelvin;
			++Volumes;
		}
		const FString GlowText = bGlow ? FString::SanitizeFloat(Glow) : FString(TEXT("as built"));
		UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: night: windows' Night %s, NightGlow %s; %d moons at %.3f of the map's sun and %d sky "
			"lights at %.3f fill; %d fogs with the city glow; %d street lamps at %.0f cd (%.0f m), %d shop lights at %.0f cd, %d lamp heads and %d stand-in lenses%s; exposure EV %.1f +- %.2f, white %.0f K in %d volumes."),
			bNight ? TEXT("1") : TEXT("unset (no window collection)"), *GlowText, Suns, SunShare, Skies,
			SkyFill, Fogs, Lamps, LampCd, LampRadius, Shops, ShopCd, Heads, Lenses, LampMaterial ? TEXT("") : TEXT(" (no lamp material)"), Ev, EvRange,
			Frame.whiteKelvin, Volumes);
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
