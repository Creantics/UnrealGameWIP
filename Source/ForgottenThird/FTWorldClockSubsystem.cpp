#include "FTWorldClockSubsystem.h"

#include "FTJointCollection.h"
#include "FTSeason.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTFarSignsSubsystem.h"
#include "FTNightLightData.h"
#include "FTLookSubsystem.h"
#include "FTWorldWaveSubsystem.h"
#include "FTNightLook.h"
#include "FTSceneLook.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
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
// The day's shadow floor is rewritten when its amount moves by this much.
constexpr float kDayShadeStep = 0.01f;
// The exposure range is rewritten when the day-to-night blend moves by this much.
constexpr float kExposureStep = 0.02f;

TAutoConsoleVariable<int32> CVarDayShade(TEXT("r.FT.DayShade"), 1,
	TEXT("The day's shadow floor (FTWorldClockSubsystem.h): 1 lifts the shade by day, 0 leaves the world look's own values."),
	ECVF_Default);

ftcore::DayLookSpec ReadDayLook()
{
	ftcore::DayLookSpec Spec;
	const TCHAR* Cmd = FTLookSwitches();
	FParse::Value(Cmd, TEXT("DayLookStep="), Spec.shadeStep);
	FParse::Value(Cmd, TEXT("DayLookFog="), Spec.fogDensity);
	FParse::Value(Cmd, TEXT("DayLookKelvin="), Spec.sunKelvin);
	FParse::Value(Cmd, TEXT("DayLookWind="), Spec.windMps);
	FParse::Value(Cmd, TEXT("DayLookCover="), Spec.cloudCover);
	FParse::Value(Cmd, TEXT("DayLookContact="), Spec.contactShadowLength);
	FParse::Value(Cmd, TEXT("DayLookSigns="), Spec.signWhiteRatio);
	FParse::Value(Cmd, TEXT("DayLookShadowTint="), Spec.shadowTintChroma);
	FString Off;
	if (FParse::Value(Cmd, TEXT("DayLookOff="), Off, false))
	{
		TArray<FString> Levers;
		Off.ParseIntoArray(Levers, TEXT(","));
		for (const FString& Lever : Levers)
		{
			if (Lever.Equals(TEXT("shade"), ESearchCase::IgnoreCase)) { Spec.shadeStep = 0; }
			else if (Lever.Equals(TEXT("fog"), ESearchCase::IgnoreCase)) { Spec.fogDensity = -1.f; }
			else if (Lever.Equals(TEXT("cap"), ESearchCase::IgnoreCase)) { Spec.fogMaxOpacity = -1.f; }
			else if (Lever.Equals(TEXT("tone"), ESearchCase::IgnoreCase)) { Spec.shadowTintChroma = -1.f; Spec.highlightTintChroma = -1.f; }
			else if (Lever.Equals(TEXT("sun"), ESearchCase::IgnoreCase)) { Spec.sunKelvin = 0.f; }
			else if (Lever.Equals(TEXT("wind"), ESearchCase::IgnoreCase)) { Spec.windMps = -1.f; }
			else if (Lever.Equals(TEXT("clouds"), ESearchCase::IgnoreCase)) { Spec.cloudCover = -1.f; }
			else if (Lever.Equals(TEXT("contact"), ESearchCase::IgnoreCase)) { Spec.contactShadowLength = -1.f; }
			else if (Lever.Equals(TEXT("signs"), ESearchCase::IgnoreCase)) { Spec.signWhiteRatio = -1.f; }
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("DayLook: -DayLookOff= has no lever %s (shade, fog, cap, tone, sun, wind, clouds, contact, signs)."), *Lever);
			}
		}
	}
	return Spec;
}

ftcore::DayShadeLook ReadDayShade()
{
	ftcore::DayShadeLook Look;
	if (FTDayLook::Enabled())
	{
		// The day look's bracket step and fog cap; the -DayShade* switches below still go over them.
		Look = ftcore::dayLookShade(Look, FTDayLook::Spec());
	}
	const TCHAR* Cmd = FTLookSwitches();
	FParse::Value(Cmd, TEXT("DayShadeSky="), Look.skyScale);
	FParse::Value(Cmd, TEXT("DayShadeContrast="), Look.shadowContrast);
	FParse::Value(Cmd, TEXT("DayShadeBoost="), Look.diffuseBoost);
	FParse::Value(Cmd, TEXT("DayShadeToe="), Look.filmToe);
	FParse::Value(Cmd, TEXT("DayShadeHighlight="), Look.highlightContrast);
	FParse::Value(Cmd, TEXT("DayShadeMinEV="), Look.minEV100);
	FParse::Value(Cmd, TEXT("DayShadeFogCap="), Look.fogMaxOpacity);
	float FogSky = 1.f;
	if (FParse::Value(Cmd, TEXT("DayShadeFogSky="), FogSky))
	{
		for (float& Channel : Look.fogSky)
		{
			Channel *= FMath::Max(FogSky, 0.f);
		}
	}
	return Look;
}
} // namespace

const ftcore::DayShadeLook& FTDayShade::Look()
{
	static const ftcore::DayShadeLook Spec = ReadDayShade();
	return Spec;
}

bool FTDayShade::Enabled()
{
	static const bool bOff = FParse::Param(FTLookSwitches(), TEXT("NoDayShade"));
	return !bOff && CVarDayShade.GetValueOnGameThread() != 0;
}

bool FTDayLook::Enabled()
{
	static const bool bOn = FParse::Param(FTLookSwitches(), TEXT("DayLook"));
	return bOn;
}

const ftcore::DayLookSpec& FTDayLook::Spec()
{
	static const ftcore::DayLookSpec Spec = ReadDayLook();
	return Spec;
}

bool FTDayLook::CloudShadows(const ftcore::SceneLook& Look)
{
	return Spec().cloudCover >= 0.f && Look.cloudCover > 0.f && Look.hasClouds();
}

ftcore::DayShadeBase FTDayShade::BaseFor(const ftcore::SceneLook& WorldLook)
{
	ftcore::DayShadeBase Base;
	Base.shadowContrast = WorldLook.localShadowContrast;
	Base.diffuseBoost = 1.f;
	Base.filmToe = WorldLook.filmToe;
	Base.highlightContrast = WorldLook.localHighlightContrast;
	Base.minEV100 = WorldLook.exposureMinEV100;
	return Base;
}

const ftcore::DayShadeFade& FTDayShade::Fade()
{
	static const ftcore::DayShadeFade Spec = []
	{
		ftcore::DayShadeFade F;
		FParse::Value(FTLookSwitches(), TEXT("DayShadeFadeHigh="), F.highDeg);
		FParse::Value(FTLookSwitches(), TEXT("DayShadeFadeLow="), F.lowDeg);
		return F;
	}();
	return Spec;
}

float FTDayShade::AmountAt(const FTNight::FSkyFrame& Sky, float SunElevationDeg)
{
	return Enabled() ? ftcore::dayShadeAmount(Fade(), Sky.NightAmount, SunElevationDeg) : 0.f;
}

void FTDayShade::WriteFog(UExponentialHeightFogComponent& Fog, const ftcore::DayShadeFrame& Frame)
{
	Fog.SetFogMaxOpacity(Frame.fogMaxOpacity);
	Fog.SetSkyAtmosphereAmbientContributionColorScale(FLinearColor(Frame.fogSky[0], Frame.fogSky[1], Frame.fogSky[2]));
}

bool FTDayShade::WritePost(UWorld& World, const ftcore::DayShadeFrame& Frame, bool bExposure)
{
	for (TActorIterator<APostProcessVolume> It(&World); It; ++It)
	{
		if (It->bUnbound && It->Priority == 0.f)
		{
			FPostProcessSettings& S = It->Settings;
			S.bOverride_LocalExposureShadowContrastScale = true;
			S.LocalExposureShadowContrastScale = Frame.shadowContrast;
			S.bOverride_LumenDiffuseColorBoost = true;
			S.LumenDiffuseColorBoost = Frame.diffuseBoost;
			S.bOverride_FilmToe = true;
			S.FilmToe = Frame.filmToe;
			S.bOverride_LocalExposureHighlightContrastScale = true;
			S.LocalExposureHighlightContrastScale = Frame.highlightContrast;
			if (bExposure)
			{
				S.bOverride_AutoExposureMinBrightness = true;
				S.AutoExposureMinBrightness = Frame.minEV100;
			}
			return true;
		}
	}
	return false;
}

float FTDayShade::StillAmount()
{
	const TCHAR* Cmd = FTLookSwitches();
	FString Shot;
	float Ignored = 0.f;
	if (!FParse::Value(Cmd, TEXT("WorldEyeShot="), Shot) || Shot.IsEmpty() || FParse::Value(Cmd, TEXT("TimeOfDay="), Ignored) ||
		FParse::Param(Cmd, TEXT("WorldEyeNight")) || FParse::Param(Cmd, TEXT("WorldEyeDusk")))
	{
		return -1.f;
	}
	float Hours = static_cast<float>(ftcore::kWorldClockStartHours);
	FParse::Value(Cmd, TEXT("WorldEyeSunHour="), Hours);
	// Only a full-day hour: a dusk or night hour's still (FTNight::ApplyStill) keeps exactly what it had.
	const ftcore::WorldClockSpec Spec;
	const float Amount = AmountAt(FTNight::FrameAt(Spec, Hours), ftcore::sunElevationDeg(Spec, Hours));
	return Amount >= 0.999f ? Amount : -1.f;
}

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
	FTDemoBootNoteWorld(Outer); // this world's boot row, not the last one's (see ForgottenThird.h)
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) &&
		!FParse::Param(FTLookSwitches(), TEXT("NoDayClock")) && Super::ShouldCreateSubsystem(Outer);
}

void UFTWorldClockSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// -JointDepth=<0..1>: the surfaces' joint depth (parallax, joint normals and cavities), in any world, still or played.
	FTJoint::ApplyCommandLine(InWorld);
	// -Season=<0..3 or spring, summer, autumn, winter>: the plants' season (FTSeason.h).
	FTSeason::ApplyCommandLine(InWorld);
	const TCHAR* Cmd = FTLookSwitches();
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
	// A level streamed in later (the hero corner sub-level) brings bay fixtures the first write missed, and a fixed
	// -TimeOfDay never moves the share again: light everything once more on the next tick.
	LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddWeakLambda(this, [this](ULevel*, UWorld* Into)
	{
		if (Into == GetWorld())
		{
			AppliedHours = -1.0;
			AppliedBayLights = -1.f;
		}
	});
	Apply(true);
	UE_LOG(LogForgottenThird, Display, TEXT("WorldClock: %.2f h%s, a day in %.1f real min; %d suns, %d sky lights, %d fogs, %d lamps and shop lights; night %.2f."),
		Clock.hours, Clock.paused ? TEXT(" (fixed)") : TEXT(""), Clock.dayMinutes, Suns.Num(), Skies.Num(), Fogs.Num(), Lamps.Num(), Night);
}

void UFTWorldClockSubsystem::Deinitialize()
{
	FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
	LevelAddedHandle.Reset();
	Super::Deinitialize();
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
			Suns.Add({Light, Light->Intensity, Light->GetLightColor(), Light->ContactShadowLength});
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
				Lamps.Add({Light, Hash, false});
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
	const FTNight::FSkyFrame Sky = FTNight::FrameAt(Spec, Clock.hours);
	const ftcore::NightFrame& Frame = Sky.Night;
	const float SunElevation = ftcore::sunElevationDeg(Spec, Clock.hours);
	// The day's shadow floor (eased out on the sun's elevation before the lamps start; 0 by night); only in a world
	// with a look.
	const UWorld* ShadeWorld = GetWorld();
	const UFTLookSubsystem* ShadeLook = ShadeWorld ? ShadeWorld->GetSubsystem<UFTLookSubsystem>() : nullptr;
	const bool bShade = ShadeLook && !ShadeLook->GetWorldLook().IsEmpty();
	const float ShadeAmount = bShade ? FTDayShade::AmountAt(Sky, SunElevation) : 0.f;
	ftcore::DayShadeFrame Shade = ftcore::dayShadeFrame(FTDayShade::Look(),
		FTDayShade::BaseFor(bShade ? ShadeLook->GetLook() : ftcore::SceneLook{}), ShadeAmount);
	// The night's local shadow contrast and film toe, where given (-NightShadowContrast=, -NightToe=), on the exposure
	// blend; unset (under 0) keeps the world look's own.
	const ftcore::NightLook& NightSpec = FTNight::Look();
	if (NightSpec.shadowContrast >= 0.f)
	{
		Shade.shadowContrast = FMath::Lerp(Shade.shadowContrast, NightSpec.shadowContrast, Frame.exposureBlend);
	}
	if (NightSpec.filmToe >= 0.f)
	{
		Shade.filmToe = FMath::Lerp(Shade.filmToe, NightSpec.filmToe, Frame.exposureBlend);
	}
	for (const FLit& S : Suns)
	{
		if (ULightComponent* Light = S.Light.Get())
		{
			Light->SetWorldRotation(Rotation);
			Light->SetIntensity(S.Intensity * Frame.sunScale);
			Light->SetLightColor(FTNight::SunColor(S.Color, Sky), false);
		}
	}
	for (const FSky& S : Skies)
	{
		if (USkyLightComponent* Light = S.Light.Get())
		{
			Light->SetIntensity(S.Intensity * FTNight::SkyScale(Sky, S.bRealTime) * Shade.skyScale);
			Light->SetLightColor(FTNight::SkyColor(S.Color, Sky));
		}
	}
	for (const FFog& F : Fogs)
	{
		if (UExponentialHeightFogComponent* Fog = F.Fog.Get())
		{
			Fog->SetFogInscatteringColor(FTNight::FogColor(F.Color, Sky));
		}
	}
	// The exposure range from the volumes' own (day) to the night's, by the exposure blend (ftcore::nightExposureBlend, on
	// the sun's elevation, after the lamps are half on); written when the blend has moved a step or reached an end.
	LampFraction = Frame.lamps;
	bool bExposureWritten = false;
	const float Blend = Frame.exposureBlend;
	if (bForce || AppliedExposureBlend < 0.f || FMath::Abs(Blend - AppliedExposureBlend) >= kExposureStep ||
		(Blend != AppliedExposureBlend && (Blend == 0.f || Blend == 1.f)))
	{
		bExposureWritten = true;
		AppliedExposureBlend = Blend;
		bExposureLocked = Frame.lockExposure;
		for (const FExposure& E : Exposures)
		{
			if (APostProcessVolume* Volume = E.Volume.Get())
			{
				FPostProcessSettings& S = Volume->Settings;
				S.bOverride_AutoExposureMinBrightness = bExposureLocked || E.bMin;
				S.bOverride_AutoExposureMaxBrightness = bExposureLocked || E.bMax;
				// The min on the dusk table (NightLook::duskMinEV) from the blend's first step, the max blended;
				// exactly the night's at blend 1.
				const ftcore::ExposureRange Range =
					ftcore::nightExposureRange(NightSpec, Frame, SunElevation, E.Min, E.Max);
				S.AutoExposureMinBrightness = Range.minEV100;
				S.AutoExposureMaxBrightness = Range.maxEV100;
			}
		}
	}
	if (bForce)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Clock: %.2f h, sun %.1f degrees, night %.2f, lamps %.2f, exposure blend %.2f (EV %.2f..%.2f at full), "
			"sky fill %.4f (real time %.3f), sun/moon %.4f (moon ramp %.2f), lights %.2f, shadow contrast %.2f, toe %.2f, white %.0f K at mix %.2f%s."),
			Clock.hours, ftcore::sunElevationDeg(Spec, Clock.hours), Night, Frame.lamps, Frame.exposureBlend, Frame.exposureMinEV100,
			Frame.exposureMaxEV100, Frame.skyScale, Frame.skyScaleRealTime, Frame.sunScale, Frame.moonScale, Sky.Lights, Shade.shadowContrast,
			Shade.filmToe, Frame.whiteKelvin, Frame.whiteMix, bShade ? TEXT("") : TEXT(" (no world look: not written)"));
	}
	if (UWorld* World = GetWorld())
	{
		DuskScene.Apply(*World, Sky.Dusk, Sky.Night.skyLuminance, false);
		// The proof bay's ceiling fixtures with the shop lights, written when the share moves a step or reaches an end.
		if (bForce || AppliedBayLights < 0.f || FMath::Abs(Sky.Lights - AppliedBayLights) >= 0.01f ||
			(Sky.Lights != AppliedBayLights && (Sky.Lights == 0.f || Sky.Lights == 1.f)))
		{
			FTNight::WriteBayLights(*World, Sky.Lights);
			AppliedBayLights = Sky.Lights;
		}
		// The day's shadow floor in the look's volume and the fogs: written when its amount has moved a step or reached
		// an end, and after the exposure range above has been written (the day's exposure floor goes over the volume's
		// own, never over the night's range).
		if (bShade && (bForce || bExposureWritten || AppliedDayShade < 0.f || FMath::Abs(ShadeAmount - AppliedDayShade) >= kDayShadeStep ||
			(ShadeAmount != AppliedDayShade && (ShadeAmount == 0.f || ShadeAmount == 1.f))))
		{
			for (const FFog& F : Fogs)
			{
				if (UExponentialHeightFogComponent* Fog = F.Fog.Get())
				{
					FTDayShade::WriteFog(*Fog, Shade);
				}
			}
			if (FTDayShade::WritePost(*World, Shade, !bExposureLocked))
			{
				AppliedDayShade = ShadeAmount;
			}
		}
		// The day's own look (FTDayLook): the row toward the day in the look subsystem and the sun's contact shadows,
		// written when its amount has moved a step or reached an end; the sun's cloud shadows when the night fills or
		// empties; the far signs on the lights' share. Nothing while a world wave writes the look; all of it after.
		if (bShade && FTDayLook::Enabled())
		{
			const float DayAmount = ftcore::dayShadeAmount(FTDayShade::Fade(), Sky.NightAmount, SunElevation);
			UFTLookSubsystem* DayLooks = World->GetSubsystem<UFTLookSubsystem>();
			const UFTWorldWaveSubsystem* Wave = World->GetSubsystem<UFTWorldWaveSubsystem>();
			if (Wave && Wave->HasWave())
			{
				AppliedDayLook = -1.f;
				AppliedCloudShadows = -1;
			}
			else if (DayLooks)
			{
				if (bForce || AppliedDayLook < 0.f || FMath::Abs(DayAmount - AppliedDayLook) >= kDayShadeStep ||
					(DayAmount != AppliedDayLook && (DayAmount == 0.f || DayAmount == 1.f)))
				{
					DayLooks->SetDayLook(DayAmount);
					AppliedDayLook = DayAmount;
					for (const FLit& S : Suns)
					{
						if (ULightComponent* Light = S.Light.Get())
						{
							const float Contact = ftcore::dayLookContactShadow(FTDayLook::Spec(), S.ContactShadow, DayAmount);
							if (Light->ContactShadowLength != Contact)
							{
								Light->ContactShadowLength = Contact;
								Light->MarkRenderStateDirty();
							}
						}
					}
				}
				const int32 Clouds = ftcore::dayLookCloudsOn(Sky.NightAmount) && FTDayLook::CloudShadows(DayLooks->GetLook()) ? 1 : 0;
				if (Clouds != AppliedCloudShadows)
				{
					if (Clouds != 0)
					{
						const int32 Had = FTScene::SetSunCloudShadows(World, true);
						MapCloudShadows = MapCloudShadows < 0 ? Had : MapCloudShadows;
					}
					else if (MapCloudShadows >= 0)
					{
						// Only ever put back what the day look changed: a night it never touched keeps the map's own.
						FTScene::SetSunCloudShadows(World, MapCloudShadows != 0);
					}
					AppliedCloudShadows = Clouds;
				}
			}
			UFTFarSignsSubsystem* Signs = World->GetSubsystem<UFTFarSignsSubsystem>();
			if (Signs)
			{
				const ULightComponent* SunLight = Suns.Num() > 0 ? Suns[0].Light.Get() : nullptr;
				Signs->SetDayLight(SunLight ? SunLight->Intensity : 0.f, SunElevation, Sky.Lights);
			}
			if (bForce)
			{
				const ftcore::SceneLook& Day = DayLooks ? DayLooks->GetLook() : ftcore::SceneLook{};
				const ULightComponent* SunLight = Suns.Num() > 0 ? Suns[0].Light.Get() : nullptr;
				UE_LOG(LogForgottenThird, Display, TEXT("DayLook: amount %.2f at sun %.1f degrees (step %d, shade sky %.2f, contrast %.2f, highlight %.2f, boost %.2f, fog cap %.2f); fog %.4f, sun %.0f K, shadow tint %.0f/%.3f, highlight tint %.0f/%.3f, wind %.1f m/s, cover %.2f; contact %.3f, cloud shadows %d (map's %d); signs %.2f%s."),
					DayAmount, SunElevation, FTDayLook::Spec().shadeStep, FTDayShade::Look().skyScale, FTDayShade::Look().shadowContrast,
					FTDayShade::Look().highlightContrast, FTDayShade::Look().diffuseBoost, FTDayShade::Look().fogMaxOpacity, Day.fogDensity,
					Day.sunKelvin, Day.shadowTintHue, Day.shadowTintChroma, Day.highlightTintHue, Day.highlightTintChroma, Day.windMps,
					Day.cloudCover, SunLight ? SunLight->ContactShadowLength : -1.f, AppliedCloudShadows, MapCloudShadows,
					Signs ? Signs->GetSignStrength() : -1.f, (Wave && Wave->HasWave()) ? TEXT(" (a wave runs: not written)") : TEXT(""));
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
		// The shop and window lights come on with the dusk row's lights (or the night, if more); the rooms' daylight
		// falls with the sun.
		WindowParams->SetScalarParameterValue(TEXT("Night"), Sky.Lights);
		WindowParams->SetScalarParameterValue(TEXT("Daylight"), Sky.Daylight * FTNight::InteriorDay());
		if (FTNight::ProofShopNight() >= 0.f)
		{
			WindowParams->SetScalarParameterValue(TEXT("ProofShopNight"), FTNight::ProofShopNight());
		}
		// -WindowParallax=<0..1> at run time: 0 shows every room and shop display case flat (no movement behind glass).
		static const float Parallax = []
		{
			float Value = -1.f;
			FParse::Value(FTLookSwitches(), TEXT("WindowParallax="), Value);
			return Value;
		}();
		if (Parallax >= 0.f)
		{
			WindowParams->SetScalarParameterValue(TEXT("Parallax"), FMath::Clamp(Parallax, 0.f, 1.f));
		}
		// -ShopDayLevel=<scale>  -ShopNightLevel=<scale>: a shop's own light by day and by night, times the built one.
		static const FVector2f ShopLevels = []
		{
			FVector2f Value(-1.f, -1.f);
			FParse::Value(FTLookSwitches(), TEXT("ShopDayLevel="), Value.X);
			FParse::Value(FTLookSwitches(), TEXT("ShopNightLevel="), Value.Y);
			return Value;
		}();
		if (ShopLevels.X >= 0.f)
		{
			WindowParams->SetScalarParameterValue(TEXT("ShopDayLevel"), ShopLevels.X);
		}
		if (ShopLevels.Y >= 0.f)
		{
			WindowParams->SetScalarParameterValue(TEXT("ShopNightLevel"), ShopLevels.Y);
		}
	}
	WetShare = Frame.wetShare;
	WriteWetness();
	for (FLamp& L : Lamps)
	{
		const bool bOn = ftcore::nightLampOn(LampFraction, L.Key);
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
		// -WorldEyeWet=<0..1> holds the ground at that wetness (a wet or dry proof of the same hour).
		static const float WetOverride = FTWorldEyeWet();
		Params->SetScalarParameterValue(TEXT("Wetness"), WetOverride >= 0.f ? WetOverride : Look->GetLook().wetness * WetShare);
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
			if (UWorld* DuskWorld = GetWorld())
			{
				const FTNight::FSkyFrame Settle = FTNight::FrameAt(Spec, Clock.hours);
				DuskScene.Settled(*DuskWorld, Settle.Dusk, Settle.Night.skyLuminance);
			}
			// The look has just written its own sky light and volume over the clock's: light them again on this tick,
			// even under a paused -TimeOfDay, with the day's shadow floor over the look's values.
			AppliedHours = -1.0;
			AppliedDayShade = -1.f;
			AppliedDayLook = -1.f;
		}
	}
	ftcore::worldClockTick(Clock, DeltaTime);
	Apply(false);
	const UWorld* LightWorld = GetWorld();
	if (!bDayLightLogged && !bWetPending && LightWorld && LightWorld->GetTimeSeconds() >= 12.0)
	{
		bDayLightLogged = true;
		const ULightComponent* Sun = Suns.Num() > 0 ? Suns[0].Light.Get() : nullptr;
		const UDirectionalLightComponent* SunDir = Cast<UDirectionalLightComponent>(Sun);
		const USkyLightComponent* SkyLight = Skies.Num() > 0 ? Skies[0].Light.Get() : nullptr;
		const APostProcessVolume* Volume = Exposures.Num() > 0 ? Exposures[0].Volume.Get() : nullptr;
		const UFTLookSubsystem* LightLook = LightWorld->GetSubsystem<UFTLookSubsystem>();
		const FPostProcessSettings* Post = Volume ? &Volume->Settings : nullptr;
		UE_LOG(LogForgottenThird, Display, TEXT("DayLight at %.1f s: %.2f h, sun %.1f degrees; sun %.3f lux, temperature %s %.0f K; sky light %.3f (%d sky lights); exposure method %d, bias %.2f, min %.2f, max %.2f EV100 (%d volumes); white balance %s %.0f K; clouds cover %.2f, sun cloud shadows %d strength %.2f/%.2f; day look %s, held wind and cover %d."),
			LightWorld->GetTimeSeconds(), Clock.hours, ftcore::sunElevationDeg(Spec, Clock.hours),
			Sun ? Sun->Intensity : -1.f, (Sun && Sun->bUseTemperature) ? TEXT("on") : TEXT("off"), Sun ? Sun->Temperature : 0.f,
			SkyLight ? SkyLight->Intensity : -1.f, Skies.Num(),
			Post ? static_cast<int32>(Post->AutoExposureMethod.GetValue()) : -1, Post ? Post->AutoExposureBias : 0.f,
			Post ? Post->AutoExposureMinBrightness : 0.f, Post ? Post->AutoExposureMaxBrightness : 0.f, Exposures.Num(),
			(Post && Post->bOverride_WhiteTemp) ? TEXT("set") : TEXT("default"), Post ? Post->WhiteTemp : 6500.f,
			LightLook ? LightLook->GetLook().cloudCover : -1.f, SunDir ? static_cast<int32>(SunDir->bCastCloudShadows) : -1,
			SunDir ? SunDir->CloudShadowStrength : -1.f, SunDir ? SunDir->CloudShadowOnSurfaceStrength : -1.f,
			FTDayLook::Enabled() ? TEXT("on") : TEXT("off"), (LightLook && LightLook->IsDayHeld()) ? 1 : 0);
	}
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
