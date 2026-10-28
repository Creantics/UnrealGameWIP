#include "FTSceneLook.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "ForgottenThird.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

namespace
{
void Label(AActor* Actor, const TCHAR* Name)
{
#if WITH_EDITOR
	Actor->SetActorLabel(Name);
#endif
}
}

void FTScene::SpawnLook(UWorld* World, const FActorSpawnParameters& Spawn, const ftcore::SceneLook& Look,
	const FRotator& SunRotation)
{
	// Light: the sun's colour comes from its temperature and the atmosphere it passes through.
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 1000), SunRotation, Spawn);
	UDirectionalLightComponent* SunLight = Sun->GetComponent();
	SunLight->SetMobility(EComponentMobility::Movable);
	SunLight->SetIntensity(Look.sunLux);
	SunLight->SetUseTemperature(true);
	SunLight->SetTemperature(Look.sunKelvin);
	SunLight->SetAtmosphereSunLight(true);
	Label(Sun, TEXT("Sun"));

	ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 800), FRotator::ZeroRotator, Spawn);
	USkyLightComponent* SkyLight = Sky->GetLightComponent();
	SkyLight->SetMobility(EComponentMobility::Movable);
	SkyLight->bRealTimeCapture = true;
	SkyLight->SetIntensity(Look.skyLight);
	Label(Sky, TEXT("SkyLight"));

	ASkyAtmosphere* Atmosphere = World->SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	Label(Atmosphere, TEXT("SkyAtmosphere"));

	// Air.
	AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(FVector(0, 0, -100), FRotator::ZeroRotator, Spawn);
	UExponentialHeightFogComponent* FogComponent = Fog->GetComponent();
	FogComponent->SetFogDensity(Look.fogDensity);
	FogComponent->SetFogHeightFalloff(Look.fogFalloff);
	FogComponent->SetVolumetricFog(Look.hasVolumetricFog());
	FogComponent->SetVolumetricFogExtinctionScale(Look.volumetricFog);
	FogComponent->SetVolumetricFogScatteringDistribution(Look.fogForward);
	Label(Fog, TEXT("HeightFog"));

	if (Look.hasClouds())
	{
		AVolumetricCloud* Clouds = World->SpawnActor<AVolumetricCloud>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
		Label(Clouds, TEXT("VolumetricCloud"));
	}

	// Camera: the level's own post-process, unbound at priority 0, so anything placed above it still wins.
	APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(FVector(0, 0, 200), FRotator::ZeroRotator, Spawn);
	Volume->bUnbound = true;
	Volume->Priority = 0.f;
	FPostProcessSettings& Finish = Volume->Settings;
	Finish.bOverride_AutoExposureBias = true;
	Finish.AutoExposureBias = Look.exposureBias;
	Finish.bOverride_AutoExposureMinBrightness = true;
	Finish.AutoExposureMinBrightness = Look.exposureMinEV100;
	Finish.bOverride_AutoExposureMaxBrightness = true;
	Finish.AutoExposureMaxBrightness = Look.exposureMaxEV100;
	Finish.bOverride_WhiteTemp = true;
	Finish.WhiteTemp = Look.whiteBalanceKelvin;
	Finish.bOverride_ColorSaturation = true;
	Finish.ColorSaturation = FVector4(Look.saturation, Look.saturation, Look.saturation, 1.f);
	Finish.bOverride_ColorContrast = true;
	Finish.ColorContrast = FVector4(Look.contrast, Look.contrast, Look.contrast, 1.f);
	Finish.bOverride_BloomIntensity = true;
	Finish.BloomIntensity = Look.bloom;
	Finish.bOverride_VignetteIntensity = true;
	Finish.VignetteIntensity = Look.vignette;
	Finish.bOverride_FilmGrainIntensity = true;
	Finish.FilmGrainIntensity = Look.filmGrain;
	Label(Volume, TEXT("PostProcessVolume"));
}

UMaterialInterface* FTScene::LoadSurface()
{
	const FString Path = FString(SurfaceMaterial) + TEXT(".") + FPackageName::GetShortName(SurfaceMaterial);
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path);
	if (!Material)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s is missing; run FTBuildWorldMaterials first."), SurfaceMaterial);
	}
	return Material;
}
