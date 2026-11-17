#include "FTNightLook.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/RectLight.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTCharacterFillSubsystem.h"
#include "FTLampPoolSubsystem.h"
#include "FTLookSubsystem.h"
#include "FTSceneLook.h"
#include "ForgottenThird.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"

#include "ftcore/HeroCorner.h"
#include "ftcore/LookTable.h"

namespace
{
// A numeric -Key=value switch. A value that is not a number (-NightToe=abc) is logged and the default kept, rather than
// read as 0 and applied. True when a number was read.
bool ReadNumber(const TCHAR* Cmd, const TCHAR* Key, float& Out)
{
	FString Text;
	if (!FParse::Value(Cmd, Key, Text))
	{
		return false;
	}
	if (Text.IsEmpty() || !FCString::IsNumeric(*Text))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Night: -%s%s is not a number; kept %.3f."), Key, *Text, Out);
		return false;
	}
	Out = FCString::Atof(*Text);
	return true;
}

// A -Key=a,b,c switch over Count numbers: all of them or none (a short or non-numeric list is logged and the defaults
// kept). Allocates only while reading the command line, once.
void ReadList(const TCHAR* Cmd, const TCHAR* Key, float* Out, int32 Count)
{
	FString Text;
	if (!FParse::Value(Cmd, Key, Text, false))
	{
		return;
	}
	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT(","));
	bool bOk = Parts.Num() == Count;
	for (const FString& P : Parts)
	{
		bOk = bOk && !P.IsEmpty() && FCString::IsNumeric(*P);
	}
	if (!bOk)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Night: -%s%s is not %d numbers; kept the defaults."), Key, *Text, Count);
		return;
	}
	for (int32 i = 0; i < Count; ++i)
	{
		Out[i] = FCString::Atof(*Parts[i]);
	}
}

ftcore::NightLook Read()
{
	ftcore::NightLook L;
	const TCHAR* Cmd = FTLookSwitches();
	ReadNumber(Cmd, TEXT("NightMoon="), L.moonShare);
	// An explicit deep fill keeps it constant through the night (the floor fit off), so older scripts keep their meaning.
	if (ReadNumber(Cmd, TEXT("NightSky="), L.skyFill))
	{
		L.floorFit.floorKey = 0.f;
	}
	ReadNumber(Cmd, TEXT("NightFloor="), L.floorFit.floorKey);
	ReadNumber(Cmd, TEXT("NightSkyLum="), L.skyLuminance);
	ReadNumber(Cmd, TEXT("NightTwilightFill="), L.twilightFill);
	ReadNumber(Cmd, TEXT("NightDeepHigh="), L.deepHighDeg);
	ReadNumber(Cmd, TEXT("NightDeepLow="), L.deepLowDeg);
	ReadNumber(Cmd, TEXT("NightLampsOnHigh="), L.lampsOnHighDeg);
	ReadNumber(Cmd, TEXT("NightLampsOnLow="), L.lampsOnLowDeg);
	ReadNumber(Cmd, TEXT("NightExposureHigh="), L.exposureHighDeg);
	ReadNumber(Cmd, TEXT("NightExposureLow="), L.exposureLowDeg);
	ReadNumber(Cmd, TEXT("NightMoonRise="), L.moonRiseDeg);
	ReadNumber(Cmd, TEXT("NightMoonFull="), L.moonFullDeg);
	ReadNumber(Cmd, TEXT("NightMoonRiseShare="), L.moonRiseShare);
	ReadNumber(Cmd, TEXT("NightSunSetHalf="), L.sunSetHalfDeg);
	ReadList(Cmd, TEXT("NightDuskLift="), L.duskLift.data(), static_cast<int32>(L.duskLift.size()));
	ReadList(Cmd, TEXT("NightDuskMinEV="), L.duskMinEV.data(), static_cast<int32>(L.duskMinEV.size()));
	if (FParse::Param(Cmd, TEXT("NightLiftAll")))
	{
		L.duskLiftTable = false;
	}
	// Negative (the default) means the world look's own shadow contrast and toe; a ruling sets 0 or more.
	ReadNumber(Cmd, TEXT("NightShadowContrast="), L.shadowContrast);
	ReadNumber(Cmd, TEXT("NightToe="), L.filmToe);
	ReadNumber(Cmd, TEXT("NightCharFillStops="), L.charFillStops);
	ReadNumber(Cmd, TEXT("NightCharFillK="), L.charFillKelvin);
	ReadNumber(Cmd, TEXT("NightCharFillPitch="), L.charFillPitchDeg);
	ReadNumber(Cmd, TEXT("NightCharFillBayNear="), L.charFillBayNearM);
	ReadNumber(Cmd, TEXT("NightCharFillBayFar="), L.charFillBayFarM);
	ReadNumber(Cmd, TEXT("NightFog="), L.fogKeep);
	ReadNumber(Cmd, TEXT("NightEV="), L.exposureEV100);
	ReadNumber(Cmd, TEXT("NightEVRange="), L.exposureRangeEV);
	ReadNumber(Cmd, TEXT("NightWhiteK="), L.whiteKelvin);
	ReadNumber(Cmd, TEXT("NightLamp="), L.lampCandela);
	ReadNumber(Cmd, TEXT("NightLampReach="), L.lampReachM);
	ReadNumber(Cmd, TEXT("NightLampK="), L.lampKelvin);
	ReadNumber(Cmd, TEXT("NightLampCone="), L.lampOuterDeg);
	ReadNumber(Cmd, TEXT("NightLampInner="), L.lampInnerDeg);
	ReadNumber(Cmd, TEXT("NightLampSource="), L.lampSourceCm);
	ReadNumber(Cmd, TEXT("NightShop="), L.shopCandela);
	ReadNumber(Cmd, TEXT("NightShopReach="), L.shopReachM);
	ReadNumber(Cmd, TEXT("NightShopK="), L.shopKelvin);
	ReadNumber(Cmd, TEXT("NightHeroShopK="), L.heroShopKelvin);
	ReadNumber(Cmd, TEXT("NightHeroLampK="), L.heroLampKelvin);
	ReadNumber(Cmd, TEXT("NightHeroFrontLm="), L.heroFrontLumens);
	ReadNumber(Cmd, TEXT("NightHeroSkyLm="), L.heroSkyLumens);
	ReadNumber(Cmd, TEXT("NightHeroSkySpec="), L.heroSkySpecular);
	ReadNumber(Cmd, TEXT("NightHeroFrontSpec="), L.heroFrontSpecular);
	ReadList(Cmd, TEXT("NightHeroSkyTint="), L.heroSkyTint.data(), static_cast<int32>(L.heroSkyTint.size()));
	ReadNumber(Cmd, TEXT("NightCap="), L.capShare);
	ReadNumber(Cmd, TEXT("NightCapFloor="), L.capFloor);
	ReadNumber(Cmd, TEXT("NightCapRadius="), L.capRadiusM);
	float Glow = 1.f;
	if (ReadNumber(Cmd, TEXT("NightGlow="), Glow))
	{
		for (float& C : L.glow)
		{
			C *= FMath::Max(Glow, 0.f);
		}
	}
	L.moonShare = FMath::Max(L.moonShare, 1e-4f);
	UE_LOG(LogForgottenThird, Display, TEXT("Night: moon %.3f, sky fill %.3f as twilight ends to %.3f in the deep night (sun %.1f to %.1f degrees), dome %.3f."),
		L.moonShare, L.twilightFill, L.skyFill, L.deepHighDeg, L.deepLowDeg, L.skyLuminance);
	if (ftcore::nightFloorFitActive(L))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Night: deep fill from the floor fit (median %.1f at moon %.3f): %.4f at 20:30, %.4f at 22:00, %.4f at 24:00 (the still keeps %.3f)."),
			L.floorFit.floorKey, L.floorFit.moonShare, ftcore::nightDeepFill(L, -34.5f), ftcore::nightDeepFill(L, -53.4f),
			ftcore::nightDeepFill(L, -68.f), L.skyFill);
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Night: deep fill constant %.3f (the floor fit off: -NightSky= given, -NightFloor=0, or moon %.3f is not the fit's %.3f)."),
			L.skyFill, L.moonShare, L.floorFit.moonShare);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Night: lamps on from %.1f to %.1f degrees, exposure to the night's from %.1f to %.1f, moon up from %.1f to %.1f (%.2f at rise), "
		"the sun's light halving every %.2f degrees once down, night shadow contrast %.2f, toe %.2f (under 0: the look's own)."),
		L.lampsOnHighDeg, L.lampsOnLowDeg, L.exposureHighDeg, L.exposureLowDeg, L.moonRiseDeg, L.moonFullDeg, L.moonRiseShare,
		L.sunSetHalfDeg, L.shadowContrast, L.filmToe);
	return L;
}

ftcore::DuskLook ReadDusk()
{
	ftcore::DuskLook D;
	if (const ftcore::LookTable::DuskRow* Row = FTLook::Table().findDusk("solhaven"))
	{
		D = Row->look;
		UE_LOG(LogForgottenThird, Display, TEXT("Dusk: the look table's row (%s): a still at a sun %.1f degrees up, the bell %.1f / %.1f / %.1f, "
			"exposure bias %.2f EV, lights on from %.1f to %.1f degrees, daylight from %.1f to %.1f."), UTF8_TO_TCHAR(Row->status.c_str()), D.stillDeg,
			D.startDeg, D.peakDeg, D.endDeg, D.exposureBias, D.lightsOnDeg, D.lightsFullDeg, D.dayEndDeg, D.dayFullDeg);
	}
	const TCHAR* Cmd = FTLookSwitches();
	FParse::Value(Cmd, TEXT("DuskPeak="), D.peakDeg);
	FParse::Value(Cmd, TEXT("DuskStart="), D.startDeg);
	FParse::Value(Cmd, TEXT("DuskEnd="), D.endDeg);
	FParse::Value(Cmd, TEXT("DuskFill="), D.fillScale);
	FParse::Value(Cmd, TEXT("DuskFlare="), D.flare);
	if (FParse::Param(Cmd, TEXT("NoDusk")))
	{
		D.startDeg = D.peakDeg; // an empty bell: duskAmount is 0 everywhere
	}
	return D;
}

// A step of the dusk amount worth writing the sky and lens for.
constexpr float kDuskStep = 0.01f;
// A step of the night's sky factor worth writing, as a log ratio (about 2%).
constexpr float kSkyRatioStep = 0.02f;
} // namespace

const ftcore::NightLook& FTNight::Look()
{
	static const ftcore::NightLook Spec = Read();
	return Spec;
}

const ftcore::DuskLook& FTNight::Dusk()
{
	static const ftcore::DuskLook Spec = ReadDusk();
	return Spec;
}

FTNight::FSkyFrame FTNight::FrameAt(const ftcore::WorldClockSpec& Spec, double Hours)
{
	FSkyFrame F;
	F.NightAmount = ftcore::nightAmount(Spec, Hours);
	const float Elevation = ftcore::sunElevationDeg(Spec, Hours);
	// The fill and the dome go on darkening after NightAmount is full, by the sun's depth below the horizon; the lamps,
	// the exposure and the moon follow the sun's elevation (ftcore::nightFrameAt).
	F.Night = ftcore::nightFrameAt(Look(), F.NightAmount, Elevation);
	const float Amount = ftcore::duskAmount(Dusk(), Elevation, Hours >= 12.0);
	F.Dusk = ftcore::duskFrame(Dusk(), Amount);
	// Windows and shops on with the lamps' curve (or the dusk row's, if earlier), before the exposure leaves the day's.
	F.Lights = FMath::Max(F.Night.lamps, ftcore::duskLights(Dusk(), Elevation));
	F.Daylight = ftcore::duskDaylight(Dusk(), Elevation) * (1.f - F.NightAmount);
	F.ExposureBias = Dusk().exposureBias * Amount * (1.f - F.NightAmount);
	return F;
}

float FTNight::ProofShopNight()
{
	static const float Value = []
	{
		float Level = FTLook::Table().glassValue("proofShopNight", -1.f);
		FParse::Value(FTLookSwitches(), TEXT("GlassProofNight="), Level);
		return Level;
	}();
	return Value;
}

float FTNight::InteriorDay()
{
	// The look table's, or the switch's (a still that measures the pane's mirror alone sets 0).
	static const float Value = []
	{
		float Level = FTLook::Table().glassValue("interiorDay", 1.f);
		FParse::Value(FTLookSwitches(), TEXT("GlassInteriorDay="), Level);
		return FMath::Max(Level, 0.f);
	}();
	return Value;
}

float FTNight::BayLampLumens()
{
	static const float Value = []
	{
		float Level = FTLook::Table().glassValue("bayLampLumens", 40.f);
		FParse::Value(FTLookSwitches(), TEXT("GlassBayLumens="), Level);
		return FMath::Max(Level, 0.f);
	}();
	return Value;
}

float FTNight::BayLampDayLumens()
{
	static const float Value = []
	{
		float Level = FTLook::Table().glassValue("bayLampDayLumens", 3000.f);
		FParse::Value(FTLookSwitches(), TEXT("GlassBayDayLumens="), Level);
		return FMath::Max(Level, 0.f);
	}();
	return Value;
}

float FTNight::BayLampAt(float Lights)
{
	const float Share = FMath::Clamp(Lights, 0.f, 1.f);
	const float Day = BayLampDayLumens(), Night = BayLampLumens();
	// Geometric between the two, so dusk sits between them as the exposure does (a straight blend would hold the day's
	// output most of the way into the night).
	return Day > 0.f && Night > 0.f ? Day * FMath::Pow(Night / Day, Share) : FMath::Lerp(Day, Night, Share);
}

int32 FTNight::WriteBayLights(UWorld& World, float Lights)
{
	// -NoBayLights (look switch, not in Shipping): the fixtures held dark, for the cost pair with and without them.
	static const bool bNoBayLights = FParse::Param(FTLookSwitches(), TEXT("NoBayLights"));
	const float Lumens = bNoBayLights ? 0.f : BayLampAt(Lights);
	const ftcore::NightLook& L = Look();
	const float Share = FMath::Clamp(Lights, 0.f, 1.f);
	int32 Written = 0, Fronts = 0, Skies = 0;
	for (TActorIterator<ARectLight> It(&World); It; ++It)
	{
		URectLightComponent* Hero = It->RectLightComponent.Get();
		const bool bFront = Hero && It->ActorHasTag(HeroFrontTag);
		const bool bSky = Hero && !bFront && It->ActorHasTag(HeroSkyTag);
		if (bFront || bSky)
		{
			// The face ruling's hero corner lights: a lit front's wide warm key, and the lane's cool floor (night only; its
			// lumens shared between its lengths).
			const float Lm = Share * (bFront ? FMath::Max(0.f, L.heroFrontLumens) : FMath::Max(0.f, L.heroSkyLumens) / ftcore::kHeroSkyFloors);
			Fronts += bFront ? 1 : 0;
			Skies += bSky ? 1 : 0;
			Hero->SetIntensity(Lm);
			Hero->SetVisibility(Lm > 0.f);
			Hero->SetVolumetricScatteringIntensity(0.f);
			if (bFront)
			{
				Hero->SetUseTemperature(L.heroShopKelvin > 0.f);
				Hero->SetTemperature(L.heroShopKelvin > 0.f ? L.heroShopKelvin : L.shopKelvin);
				Hero->SetSpecularScale(FMath::Max(0.f, L.heroFrontSpecular));
			}
			else
			{
				Hero->SetUseTemperature(false);
				Hero->SetLightColor(Color(L.heroSkyTint));
				Hero->SetSpecularScale(FMath::Max(0.f, L.heroSkySpecular));
			}
			++Written;
			continue;
		}
		URectLightComponent* Light = It->ActorHasTag(BayLightTag) ? It->RectLightComponent.Get() : nullptr;
		if (!Light)
		{
			continue;
		}
		Light->SetIntensity(Lumens);
		Light->SetVisibility(Lumens > 0.f);
		// A shop lamp lights the goods, not fog inside the shop (11-14 haze ruling).
		Light->SetVolumetricScatteringIntensity(0.f);
		++Written;
	}
	// The values the hero corner's lights resolve to, logged when the share has moved a quarter (or the first time).
	static float LastLogged = -1.f;
	if (Fronts + Skies > 0 && FMath::Abs(Share - LastLogged) >= 0.25f)
	{
		LastLogged = Share;
		UE_LOG(LogForgottenThird, Display, TEXT("Night: hero corner at lights %.2f: %d front keys %.1f lm at %.0f K, %d floor lengths %.1f lm (of %.1f) tint %.2f,%.2f,%.2f "
			"specular %.2f; pool lamps %.0f K, shops %.0f K."), Share, Fronts, Share * L.heroFrontLumens,
			L.heroShopKelvin > 0.f ? L.heroShopKelvin : L.shopKelvin, Skies, Share * L.heroSkyLumens / ftcore::kHeroSkyFloors, Share * L.heroSkyLumens,
			L.heroSkyTint[0], L.heroSkyTint[1], L.heroSkyTint[2], L.heroSkySpecular, L.heroLampKelvin > 0.f ? L.heroLampKelvin : L.lampKelvin,
			L.heroShopKelvin > 0.f ? L.heroShopKelvin : L.shopKelvin);
	}
	return Written;
}

bool FTNight::WriteWindowLights(UWorld& World, float Lights, float Daylight)
{
	WriteBayLights(World, Lights);
	const FString Path = FTScene::WindowCollection;
	UMaterialParameterCollection* Collection =
		LoadObject<UMaterialParameterCollection>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	UMaterialParameterCollectionInstance* Windows = Collection ? World.GetParameterCollectionInstance(Collection) : nullptr;
	if (!Windows)
	{
		return false;
	}
	// The proof front's interior light at night: the look table's, or the switch's; the collection's default otherwise.
	Windows->SetScalarParameterValue(TEXT("Night"), Lights);
	Windows->SetScalarParameterValue(TEXT("Daylight"), Daylight * InteriorDay());
	if (ProofShopNight() >= 0.f)
	{
		Windows->SetScalarParameterValue(TEXT("ProofShopNight"), ProofShopNight());
	}
	return true;
}

void FTNight::FDuskScene::GatherSkies(UWorld& World)
{
	bSkiesGathered = true;
	Skies.Reset();
	for (TActorIterator<ASkyAtmosphere> It(&World); It; ++It)
	{
		if (USkyAtmosphereComponent* Sky = It->GetComponent())
		{
			Skies.Add({Sky, Sky->SkyLuminanceFactor});
		}
	}
}

void FTNight::FDuskScene::Gather(UWorld& World)
{
	Volumes.Reset();
	for (TActorIterator<APostProcessVolume> It(&World); It; ++It)
	{
		const FPostProcessSettings& S = It->Settings;
		FVolume V;
		V.Volume = *It;
		V.bFlare = S.bOverride_LensFlareIntensity != 0;
		V.bThreshold = S.bOverride_LensFlareThreshold != 0;
		V.bBloom = S.bOverride_BloomIntensity != 0;
		V.Flare = V.bFlare ? S.LensFlareIntensity : 1.f;
		V.Threshold = V.bThreshold ? S.LensFlareThreshold : 8.f;
		V.Bloom = V.bBloom ? S.BloomIntensity : 0.675f;
		Volumes.Add(V);
	}
}

bool FTNight::FDuskScene::Apply(UWorld& World, const ftcore::DuskFrame& Frame, float NightSky, bool bForce)
{
	const float Amount = Frame.amount;
	const float Night = FMath::Clamp(NightSky, 1e-4f, 1.f);
	const bool bEnding = Amount <= 0.f && Applied > 0.f;
	const bool bDusk = bForce || bEnding || FMath::Abs(Amount - Applied) >= kDuskStep;
	// The night's factor runs from 1 down to about 0.03: a step in ratio, so the darkest end moves as smoothly as the day's.
	const bool bNight = bForce || FMath::Abs(FMath::Loge(Night / AppliedSky)) >= kSkyRatioStep || (Night == 1.f && AppliedSky != 1.f);
	if (!bDusk && !bNight)
	{
		return false;
	}
	if (!bSkiesGathered)
	{
		GatherSkies(World); // the map's own factors: nothing else writes them, so they are the day's whenever read
	}
	AppliedSky = Night;
	const FLinearColor Glow = Color(Frame.skyGlow) * Night;
	for (const FSky& S : Skies)
	{
		if (USkyAtmosphereComponent* Sky = S.Sky.Get())
		{
			Sky->SetSkyLuminanceFactor(FLinearColor(S.Factor.R * Glow.R, S.Factor.G * Glow.G, S.Factor.B * Glow.B, S.Factor.A));
		}
	}
	if (!bDusk)
	{
		return true;
	}
	if (Applied <= 0.f && Amount > 0.f)
	{
		Gather(World); // the volumes' own values, after the look has written them
	}
	Applied = Amount;
	for (const FVolume& V : Volumes)
	{
		if (APostProcessVolume* Volume = V.Volume.Get())
		{
			FPostProcessSettings& S = Volume->Settings;
			const bool bOn = Amount > 0.f;
			S.bOverride_LensFlareIntensity = V.bFlare || bOn;
			S.bOverride_LensFlareThreshold = V.bThreshold || bOn;
			S.bOverride_BloomIntensity = V.bBloom || bOn;
			S.LensFlareIntensity = FMath::Lerp(V.Flare, Frame.flare, Frame.flareMix);
			S.LensFlareThreshold = FMath::Lerp(V.Threshold, Frame.flareThreshold, Frame.flareMix);
			S.BloomIntensity = V.Bloom + Frame.bloomAdd;
		}
	}
	return true;
}

void FTNight::FDuskScene::Settled(UWorld& World, const ftcore::DuskFrame& Frame, float NightSky)
{
	if (Applied > 0.f)
	{
		for (FVolume& V : Volumes)
		{
			if (const APostProcessVolume* Volume = V.Volume.Get())
			{
				V.bBloom = Volume->Settings.bOverride_BloomIntensity != 0;
				V.Bloom = V.bBloom ? Volume->Settings.BloomIntensity : 0.675f;
			}
		}
	}
	Apply(World, Frame, NightSky, true);
}

void FTNight::ApplyStill(UWorld& World, const FSkyFrame& Frame)
{
	int32 Suns = 0, SkyLights = 0, Fogs = 0;
	for (TActorIterator<ADirectionalLight> It(&World); It; ++It)
	{
		if (It->ActorHasTag(UFTCharacterFillSubsystem::LightTag))
		{
			continue; // the characters' own fill, not a sun
		}
		if (ULightComponent* Light = It->GetLightComponent())
		{
			Light->SetMobility(EComponentMobility::Movable);
			Light->SetIntensity(Light->Intensity * Frame.Night.sunScale);
			Light->SetLightColor(SunColor(Light->GetLightColor(), Frame), false);
			++Suns;
		}
	}
	for (TActorIterator<ASkyLight> It(&World); It; ++It)
	{
		if (USkyLightComponent* Light = It->GetLightComponent())
		{
			Light->SetIntensity(Light->Intensity * SkyScale(Frame, Light->bRealTimeCapture));
			Light->SetLightColor(SkyColor(Light->GetLightColor(), Frame));
			++SkyLights;
		}
	}
	for (TActorIterator<AExponentialHeightFog> It(&World); It; ++It)
	{
		if (UExponentialHeightFogComponent* Fog = It->GetComponent())
		{
			Fog->SetFogInscatteringColor(FogColor(Fog->FogInscatteringLuminance, Frame));
			++Fogs;
		}
	}
	FDuskScene Scene;
	Scene.Apply(World, Frame.Dusk, Frame.Night.skyLuminance, true);
	const bool bWindows = WriteWindowLights(World, Frame.Lights, Frame.Daylight);
	// The dusk row's exposure bias over the volumes' own, and the street lamps and shop spill lights at the lights' share.
	int32 Biased = 0;
	if (Frame.ExposureBias != 0.f)
	{
		for (TActorIterator<APostProcessVolume> It(&World); It; ++It)
		{
			FPostProcessSettings& S = It->Settings;
			S.AutoExposureBias = (S.bOverride_AutoExposureBias ? S.AutoExposureBias : 0.f) + Frame.ExposureBias;
			S.bOverride_AutoExposureBias = true;
			++Biased;
		}
	}
	bool bLamps = false;
	if (Frame.Lights > 0.f && Frame.NightAmount < 1.f)
	{
		const ftcore::NightLook& Spec = Look();
		UFTLampPoolSubsystem* Pool = World.GetSubsystem<UFTLampPoolSubsystem>();
		bLamps = Pool && Pool->ForceNight(Spec.lampCandela * Frame.Lights, Spec.lampReachM * 100.f, Spec.shopCandela * Frame.Lights, Spec.shopReachM * 100.f);
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("WorldEyeShot: dusk %.2f, night %.2f (deep %.2f, sky fill x%.3f) on %d suns, %d sky lights, %d fogs, %d skies (luminance x%.3f), %d volumes; windows' Night %s; "
			 "lights %.2f%s, daylight %.2f, exposure bias %+.2f EV on %d volumes."),
		Frame.Dusk.amount, Frame.NightAmount, Frame.Night.deep, Frame.Night.skyScale, Suns, SkyLights, Fogs, Scene.NumSkies(), Frame.Night.skyLuminance, Scene.NumVolumes(),
		bWindows ? TEXT("written") : TEXT("not written"), Frame.Lights, bLamps ? TEXT(" (street lamps and shop lights from the pool)") : TEXT(""),
		Frame.Daylight, Frame.ExposureBias, Biased);
}
