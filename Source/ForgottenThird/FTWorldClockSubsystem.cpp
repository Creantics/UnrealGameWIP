#include "FTWorldClockSubsystem.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTNightLightData.h"
#include "FTLookSubsystem.h"
#include "FTNightLook.h"
#include "FTSceneLook.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ftcore/LookTable.h"
#include "ftcore/WindowInterior.h"

#include <string>

namespace
{
// The clock's step between light updates: 15 game seconds (half a real second on the 48-minute day).
constexpr double kStepHours = 1.0 / 240.0;
// The white balance is rewritten when the night's share of it moves by this much.
constexpr float kWhiteStep = 0.01f;
} // namespace

FRotator FTClock::SunRotation(const ftcore::WorldClockSpec& Spec, double Hours)
{
	const ftcore::SunDirection Sun = ftcore::sunDirection(Spec, Hours);
	// Towards the sun in engine axes (east +X, north -Y); the moon stands opposite once the sun is down.
	FVector Toward(Sun.east, -Sun.north, Sun.up);
	if (Sun.up < 0.f)
	{
		Toward = -Toward;
	}
	return (-Toward).Rotation();
}

bool UFTWorldClockSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) &&
		!FParse::Param(FCommandLine::Get(), TEXT("NoDayClock")) && Super::ShouldCreateSubsystem(Outer);
}

void UFTWorldClockSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	const TCHAR* Cmd = FCommandLine::Get();
	float Fixed = -1.f;
	const bool bFixed = FParse::Value(Cmd, TEXT("TimeOfDay="), Fixed);
	const FTCHARToUTF8 Map(*InWorld.GetMapName());
	const bool bSolhaven = ftcore::lookWorldForMap(std::string_view(Map.Get(), static_cast<size_t>(Map.Length()))) == "solhaven";
	FString Shot;
	const bool bEyeShot = FParse::Value(Cmd, TEXT("WorldEyeShot="), Shot) && !Shot.IsEmpty();
	if (!bFixed && (!bSolhaven || bEyeShot))
	{
		return;
	}

	const FString File = FPaths::ProjectDir() / TEXT("Data/time_curves.csv");
	TArray<uint8> Bytes;
	std::string Error;
	bCurves = IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File) &&
		ftcore::parseTimeCurves(FT::BytesAsText(Bytes), Curves, &Error);
	if (!bCurves)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("WorldClock: Data/time_curves.csv missing or refused (%s); crowd and danger read 0."),
			*FT::ToFString(Error));
	}

	float Minutes = ftcore::kWorldClockDefaultDayMinutes;
	if (FParse::Value(Cmd, TEXT("DayLength="), Minutes))
	{
		ftcore::worldClockSetDayMinutes(Clock, Minutes);
	}
	if (!bFixed && !bClockGiven)
	{
		ftcore::worldClockSet(Clock, ftcore::kWorldClockStartHours);
	}
	if (bFixed)
	{
		ftcore::worldClockSet(Clock, Fixed);
		ftcore::worldClockPause(Clock);
		bFixedByCommandLine = true;
	}
	Gather(InWorld);
	bActive = true;
	Apply(true);
	UE_LOG(LogForgottenThird, Display, TEXT("WorldClock: %.2f h%s, a day in %.1f real min; %d suns, %d sky lights, %d fogs, %d lamps and shop lights; night %.2f."),
		Clock.hours, Clock.paused ? TEXT(" (fixed)") : TEXT(""), Clock.dayMinutes, Suns.Num(), Skies.Num(), Fogs.Num(), Lamps.Num(), Night);
}

void UFTWorldClockSubsystem::Gather(UWorld& World)
{
	const FString Path = FTScene::WindowCollection;
	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(
		nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	Windows = Collection ? World.GetParameterCollectionInstance(Collection) : nullptr;
	const FString LookPath = FTScene::LookCollection;
	UMaterialParameterCollection* LookCollection = LoadObject<UMaterialParameterCollection>(
		nullptr, *(LookPath + TEXT(".") + FPackageName::GetShortName(LookPath)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	LookParams = LookCollection ? World.GetParameterCollectionInstance(LookCollection) : nullptr;
	for (TActorIterator<ADirectionalLight> It(&World); It; ++It)
	{
		if (ULightComponent* Light = It->GetLightComponent())
		{
			Light->SetMobility(EComponentMobility::Movable);
			Suns.Add({Light, Light->Intensity, Light->GetLightColor()});
		}
	}
	for (TActorIterator<ASkyLight> It(&World); It; ++It)
	{
		if (USkyLightComponent* Light = It->GetLightComponent())
		{
			Skies.Add({Light, Light->Intensity, Light->GetLightColor(), Light->bRealTimeCapture});
		}
	}
	for (TActorIterator<AExponentialHeightFog> It(&World); It; ++It)
	{
		if (UExponentialHeightFogComponent* Fog = It->GetComponent())
		{
			Fogs.Add({Fog, Fog->FogInscatteringLuminance});
		}
	}
	for (TActorIterator<APostProcessVolume> It(&World); It; ++It)
	{
		const FPostProcessSettings& S = It->Settings;
		Exposures.Add({*It, S.bOverride_AutoExposureMinBrightness != 0, S.bOverride_AutoExposureMaxBrightness != 0, S.AutoExposureMinBrightness,
			S.AutoExposureMaxBrightness, S.bOverride_WhiteTemp != 0, S.WhiteTemp});
	}
	// A map whose lights are data (AFTNightLights) leaves them to FTLampPoolSubsystem's pool; only older maps' tagged
	// light actors are gathered here.
	const TActorIterator<AFTNightLights> Pooled(&World);
	if (Pooled)
	{
		return;
	}
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		const bool bTagged = It->ActorHasTag(FTScene::NightLampTag) || It->ActorHasTag(FTScene::NightShopTag);
		TInlineComponentArray<ULightComponent*> Lights(*It);
		for (ULightComponent* Light : Lights)
		{
			if (bTagged || Light->ComponentHasTag(FTScene::NightLampTag) || Light->ComponentHasTag(FTScene::NightShopTag))
			{
				const FVector At = Light->GetComponentLocation();
				const uint32 Hash = ftcore::windowHash(static_cast<uint32>(FMath::FloorToInt(At.X)) * 73856093u ^
					static_cast<uint32>(FMath::FloorToInt(At.Y)) * 19349663u);
				Light->SetMobility(EComponentMobility::Movable);
				Lamps.Add({Light, 0.4f + 0.2f * static_cast<float>(Hash & 0xFFFFu) / 65535.f, false});
			}
		}
	}
}

void UFTWorldClockSubsystem::Apply(bool bForce)
{
	if (!bForce && FMath::Abs(Clock.hours - AppliedHours) < kStepHours)
	{
		return;
	}
	AppliedHours = Clock.hours;
	Night = ftcore::nightAmount(Spec, Clock.hours);
	const FRotator Rotation = FTClock::SunRotation(Spec, Clock.hours);
	const ftcore::NightLook& Look = FTNight::Look();
	const ftcore::NightFrame Frame = ftcore::nightFrame(Look, Night);
	for (const FLit& S : Suns)
	{
		if (ULightComponent* Light = S.Light.Get())
		{
			Light->SetWorldRotation(Rotation);
			Light->SetIntensity(S.Intensity * Frame.sunScale);
			Light->SetLightColor(FMath::Lerp(S.Color, FTNight::Color(Look.moonTint), Frame.tintMix), false);
		}
	}
	for (const FSky& S : Skies)
	{
		if (USkyLightComponent* Light = S.Light.Get())
		{
			Light->SetIntensity(S.Intensity * (S.bRealTime ? Frame.skyScaleRealTime : Frame.skyScale));
			Light->SetLightColor(FMath::Lerp(S.Color, FTNight::Color(Look.skyTint), Frame.tintMix));
		}
	}
	const FLinearColor Glow(Frame.fogGlow[0], Frame.fogGlow[1], Frame.fogGlow[2]);
	for (const FFog& F : Fogs)
	{
		if (UExponentialHeightFogComponent* Fog = F.Fog.Get())
		{
			Fog->SetFogInscatteringColor(F.Color * Frame.fogKeep + Glow);
		}
	}
	// The night's exposure range from half night on (as the night still's), the volumes' own settings back by day;
	// written only when that changes.
	if (Frame.lockExposure != bExposureLocked || bForce)
	{
		bExposureLocked = Frame.lockExposure;
		for (const FExposure& E : Exposures)
		{
			if (APostProcessVolume* Volume = E.Volume.Get())
			{
				FPostProcessSettings& S = Volume->Settings;
				S.bOverride_AutoExposureMinBrightness = bExposureLocked || E.bMin;
				S.bOverride_AutoExposureMaxBrightness = bExposureLocked || E.bMax;
				S.AutoExposureMinBrightness = bExposureLocked ? Frame.exposureMinEV100 : E.Min;
				S.AutoExposureMaxBrightness = bExposureLocked ? Frame.exposureMaxEV100 : E.Max;
			}
		}
	}
	// The white balance toward the night's by NightAmount (the volume's own by day); written when it has moved a step,
	// never by full day, so the world look's own white balance stands. The day's is read again as dusk starts, after
	// the look has written it.
	if (FMath::Abs(Frame.whiteMix - AppliedWhiteMix) >= kWhiteStep || (Frame.whiteMix == 0.f && AppliedWhiteMix != 0.f))
	{
		const bool bDusk = AppliedWhiteMix == 0.f;
		AppliedWhiteMix = Frame.whiteMix;
		for (FExposure& E : Exposures)
		{
			if (APostProcessVolume* Volume = E.Volume.Get())
			{
				FPostProcessSettings& S = Volume->Settings;
				if (bDusk)
				{
					E.bWhite = S.bOverride_WhiteTemp != 0;
					E.White = S.WhiteTemp;
				}
				S.bOverride_WhiteTemp = E.bWhite || Frame.whiteMix > 0.f;
				S.WhiteTemp = FMath::Lerp(E.bWhite ? E.White : 6500.f, Frame.whiteKelvin, Frame.whiteMix);
			}
		}
	}
	if (UMaterialParameterCollectionInstance* WindowParams = Windows.Get())
	{
		WindowParams->SetScalarParameterValue(TEXT("Night"), Night);
	}
	WetShare = Frame.wetShare;
	WriteWetness();
	for (FLamp& L : Lamps)
	{
		const bool bOn = Night >= L.OnAt;
		if (bOn == L.bOn && !bForce)
		{
			continue;
		}
		L.bOn = bOn;
		if (ULightComponent* Light = L.Light.Get())
		{
			Light->SetVisibility(bOn);
			Light->SetHiddenInGame(!bOn);
		}
	}
}

void UFTWorldClockSubsystem::WriteWetness()
{
	UMaterialParameterCollectionInstance* Params = LookParams.Get();
	const UWorld* World = GetWorld();
	const UFTLookSubsystem* Look = World ? World->GetSubsystem<UFTLookSubsystem>() : nullptr;
	if (Params && Look)
	{
		Params->SetScalarParameterValue(TEXT("Wetness"), Look->GetLook().wetness * WetShare);
	}
}

void UFTWorldClockSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (bWetPending)
	{
		const UWorld* World = GetWorld();
		const UFTLookSubsystem* Look = World ? World->GetSubsystem<UFTLookSubsystem>() : nullptr;
		if (!Look || Look->IsSettled())
		{
			bWetPending = false;
			WriteWetness();
		}
	}
	ftcore::worldClockTick(Clock, DeltaTime);
	Apply(false);
}

TStatId UFTWorldClockSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTWorldClockSubsystem, STATGROUP_Tickables);
}

void UFTWorldClockSubsystem::SetTimeOfDay(float Hours)
{
	bClockGiven = true;
	ftcore::worldClockSet(Clock, Hours);
	if (bActive)
	{
		Apply(true);
	}
}

void UFTWorldClockSubsystem::AdvanceTo(float Hours)
{
	bClockGiven = true;
	ftcore::worldClockAdvanceTo(Clock, Hours);
	if (bActive)
	{
		Apply(true);
	}
}

void UFTWorldClockSubsystem::PauseClock()
{
	ftcore::worldClockPause(Clock);
}

void UFTWorldClockSubsystem::ResumeClock()
{
	ftcore::worldClockResume(Clock);
}

void UFTWorldClockSubsystem::SetDayLength(float RealMinutes)
{
	ftcore::worldClockSetDayMinutes(Clock, RealMinutes);
}

float UFTWorldClockSubsystem::GetCrowdDensity() const
{
	return bCurves ? ftcore::timeCurveAt(Curves.crowd, Clock.hours) : 0.f;
}

float UFTWorldClockSubsystem::GetDanger() const
{
	return bCurves ? ftcore::timeCurveAt(Curves.danger, Clock.hours) : 0.f;
}

void UFTWorldClockSubsystem::SetClock(const ftcore::WorldClock& InClock)
{
	Clock = ftcore::worldClockSanitized(InClock);
	bClockGiven = true;
	if (bActive)
	{
		Apply(true);
	}
}
