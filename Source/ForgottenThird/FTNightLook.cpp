#include "FTNightLook.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTLampPoolSubsystem.h"
#include "FTLookSubsystem.h"
#include "FTSceneLook.h"
#include "ForgottenThird.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"

#include "ftcore/LookTable.h"

namespace
{
ftcore::NightLook Read()
{
	ftcore::NightLook L;
	const TCHAR* Cmd = FCommandLine::Get();
	FParse::Value(Cmd, TEXT("NightMoon="), L.moonShare);
	FParse::Value(Cmd, TEXT("NightSky="), L.skyFill);
	FParse::Value(Cmd, TEXT("NightFog="), L.fogKeep);
	FParse::Value(Cmd, TEXT("NightEV="), L.exposureEV100);
	FParse::Value(Cmd, TEXT("NightEVRange="), L.exposureRangeEV);
	FParse::Value(Cmd, TEXT("NightWhiteK="), L.whiteKelvin);
	FParse::Value(Cmd, TEXT("NightLamp="), L.lampCandela);
	FParse::Value(Cmd, TEXT("NightLampReach="), L.lampReachM);
	FParse::Value(Cmd, TEXT("NightLampK="), L.lampKelvin);
	FParse::Value(Cmd, TEXT("NightLampCone="), L.lampOuterDeg);
	FParse::Value(Cmd, TEXT("NightLampInner="), L.lampInnerDeg);
	FParse::Value(Cmd, TEXT("NightLampSource="), L.lampSourceCm);
	FParse::Value(Cmd, TEXT("NightShop="), L.shopCandela);
	FParse::Value(Cmd, TEXT("NightShopReach="), L.shopReachM);
	FParse::Value(Cmd, TEXT("NightShopK="), L.shopKelvin);
	FParse::Value(Cmd, TEXT("NightCap="), L.capShare);
	FParse::Value(Cmd, TEXT("NightCapFloor="), L.capFloor);
	FParse::Value(Cmd, TEXT("NightCapRadius="), L.capRadiusM);
	float Glow = 1.f;
	if (FParse::Value(Cmd, TEXT("NightGlow="), Glow))
	{
		for (float& C : L.glow)
		{
			C *= FMath::Max(Glow, 0.f);
		}
	}
	L.moonShare = FMath::Max(L.moonShare, 1e-4f);
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
	const TCHAR* Cmd = FCommandLine::Get();
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
	F.Night = ftcore::nightFrame(Look(), F.NightAmount);
	const float Elevation = ftcore::sunElevationDeg(Spec, Hours);
	const float Amount = ftcore::duskAmount(Dusk(), Elevation, Hours >= 12.0);
	F.Dusk = ftcore::duskFrame(Dusk(), Amount);
	F.Lights = FMath::Max(F.NightAmount, ftcore::duskLights(Dusk(), Elevation));
	F.Daylight = ftcore::duskDaylight(Dusk(), Elevation) * (1.f - F.NightAmount);
	F.ExposureBias = Dusk().exposureBias * Amount * (1.f - F.NightAmount);
	return F;
}

float FTNight::ProofShopNight()
{
	static const float Value = []
	{
		float Level = FTLook::Table().glassValue("proofShopNight", -1.f);
		FParse::Value(FCommandLine::Get(), TEXT("GlassProofNight="), Level);
		return Level;
	}();
	return Value;
}

float FTNight::InteriorDay()
{
	static const float Value = FMath::Max(FTLook::Table().glassValue("interiorDay", 1.f), 0.f);
	return Value;
}

bool FTNight::WriteWindowLights(UWorld& World, float Lights, float Daylight)
{
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

void FTNight::FDuskScene::Gather(UWorld& World)
{
	Skies.Reset();
	Volumes.Reset();
	for (TActorIterator<ASkyAtmosphere> It(&World); It; ++It)
	{
		if (USkyAtmosphereComponent* Sky = It->GetComponent())
		{
			Skies.Add({Sky, Sky->SkyLuminanceFactor});
		}
	}
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

bool FTNight::FDuskScene::Apply(UWorld& World, const ftcore::DuskFrame& Frame, bool bForce)
{
	const float Amount = Frame.amount;
	const bool bEnding = Amount <= 0.f && Applied > 0.f;
	if (!bForce && !bEnding && FMath::Abs(Amount - Applied) < kDuskStep)
	{
		return false;
	}
	if (Applied <= 0.f && Amount > 0.f)
	{
		Gather(World); // the day's own values, after the look has written them
	}
	Applied = Amount;
	const FLinearColor Glow = Color(Frame.skyGlow);
	for (const FSky& S : Skies)
	{
		if (USkyAtmosphereComponent* Sky = S.Sky.Get())
		{
			Sky->SetSkyLuminanceFactor(FLinearColor(S.Factor.R * Glow.R, S.Factor.G * Glow.G, S.Factor.B * Glow.B, S.Factor.A));
		}
	}
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

void FTNight::FDuskScene::Settled(UWorld& World, const ftcore::DuskFrame& Frame)
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
	Apply(World, Frame, true);
}

void FTNight::ApplyStill(UWorld& World, const FSkyFrame& Frame)
{
	int32 Suns = 0, SkyLights = 0, Fogs = 0;
	for (TActorIterator<ADirectionalLight> It(&World); It; ++It)
	{
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
	Scene.Apply(World, Frame.Dusk, true);
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
		TEXT("WorldEyeShot: dusk %.2f, night %.2f on %d suns, %d sky lights, %d fogs, %d skies, %d volumes; windows' Night %s; lights %.2f%s, "
			 "daylight %.2f, exposure bias %+.2f EV on %d volumes."),
		Frame.Dusk.amount, Frame.NightAmount, Suns, SkyLights, Fogs, Scene.NumSkies(), Scene.NumVolumes(),
		bWindows ? TEXT("written") : TEXT("not written"), Frame.Lights, bLamps ? TEXT(" (street lamps and shop lights from the pool)") : TEXT(""),
		Frame.Daylight, Frame.ExposureBias, Biased);
}
