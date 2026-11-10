#include "FTSceneLook.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ForgottenThird.h"
#include "FTText.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#include <string>
#include <vector>

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

int32 FTScene::ApplyLook(UWorld* World, const ftcore::SceneLook& Look, const ftcore::SceneLook& Current)
{
	if (!World)
	{
		return -1;
	}
	APostProcessVolume* Volume = nullptr;
	for (TActorIterator<APostProcessVolume> It(World); It; ++It)
	{
		if (It->bUnbound && It->Priority == 0.f)
		{
			Volume = *It;
			break;
		}
	}
	if (!Volume)
	{
		return -1;
	}
	int32 Written = 0;
	// Each field: written, with its override on, only where the new look differs from what's there.
#define FT_LOOK_SET(Field, Statement) 	if (Look.Field != Current.Field) 	{ 		Statement; 		++Written; 	}

	// Camera.
	FPostProcessSettings& Finish = Volume->Settings;
#define FT_LOOK_POST(Field, Setting, Value) FT_LOOK_SET(Field, Finish.bOverride_##Setting = true; Finish.Setting = Value)
	FT_LOOK_POST(exposureBias, AutoExposureBias, Look.exposureBias)
	FT_LOOK_POST(exposureMinEV100, AutoExposureMinBrightness, Look.exposureMinEV100)
	FT_LOOK_POST(exposureMaxEV100, AutoExposureMaxBrightness, Look.exposureMaxEV100)
	FT_LOOK_POST(exposureSpeedUp, AutoExposureSpeedUp, Look.exposureSpeedUp)
	FT_LOOK_POST(exposureSpeedDown, AutoExposureSpeedDown, Look.exposureSpeedDown)
	FT_LOOK_POST(whiteBalanceKelvin, WhiteTemp, Look.whiteBalanceKelvin)
	FT_LOOK_POST(saturation, ColorSaturation, FVector4(Look.saturation, Look.saturation, Look.saturation, 1.f))
	FT_LOOK_POST(contrast, ColorContrast, FVector4(Look.contrast, Look.contrast, Look.contrast, 1.f))
	FT_LOOK_POST(filmSlope, FilmSlope, Look.filmSlope)
	FT_LOOK_POST(filmToe, FilmToe, Look.filmToe)
	FT_LOOK_POST(filmShoulder, FilmShoulder, Look.filmShoulder)
	FT_LOOK_POST(filmBlackClip, FilmBlackClip, Look.filmBlackClip)
	FT_LOOK_POST(filmWhiteClip, FilmWhiteClip, Look.filmWhiteClip)
	FT_LOOK_POST(localHighlightContrast, LocalExposureHighlightContrastScale, Look.localHighlightContrast)
	FT_LOOK_POST(localShadowContrast, LocalExposureShadowContrastScale, Look.localShadowContrast)
	FT_LOOK_POST(lumenSkylightLeaking, LumenSkylightLeaking, Look.lumenSkylightLeaking)
	FT_LOOK_POST(bloom, BloomIntensity, Look.bloom)
	FT_LOOK_POST(vignette, VignetteIntensity, Look.vignette)
	FT_LOOK_POST(fringe, SceneFringeIntensity, Look.fringe)
	FT_LOOK_POST(filmGrain, FilmGrainIntensity, Look.filmGrain)
	FT_LOOK_POST(filmGrainShadows, FilmGrainIntensityShadows, Look.filmGrainShadows)
	FT_LOOK_POST(filmGrainHighlights, FilmGrainIntensityHighlights, Look.filmGrainHighlights)
#undef FT_LOOK_POST

	// Light: the sun is the one the atmosphere takes as its sun.
	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		UDirectionalLightComponent* SunLight = It->GetComponent();
		if (SunLight && SunLight->IsUsedAsAtmosphereSunLight())
		{
			FT_LOOK_SET(sunLux, SunLight->SetIntensity(Look.sunLux))
			FT_LOOK_SET(sunKelvin, SunLight->SetUseTemperature(true); SunLight->SetTemperature(Look.sunKelvin))
			break;
		}
	}
	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		if (USkyLightComponent* SkyLight = It->GetLightComponent())
		{
			FT_LOOK_SET(skyLight, SkyLight->SetIntensity(Look.skyLight))
			break;
		}
	}

	// Air.
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		if (UExponentialHeightFogComponent* FogComponent = It->GetComponent())
		{
			FT_LOOK_SET(fogDensity, FogComponent->SetFogDensity(Look.fogDensity))
			FT_LOOK_SET(fogFalloff, FogComponent->SetFogHeightFalloff(Look.fogFalloff))
			FT_LOOK_SET(volumetricFog, FogComponent->SetVolumetricFog(Look.hasVolumetricFog()); FogComponent->SetVolumetricFogExtinctionScale(Look.volumetricFog))
			FT_LOOK_SET(fogForward, FogComponent->SetVolumetricFogScatteringDistribution(Look.fogForward))
			if (Look.fogAlbedoR != Current.fogAlbedoR || Look.fogAlbedoG != Current.fogAlbedoG || Look.fogAlbedoB != Current.fogAlbedoB)
			{
				FogComponent->SetVolumetricFogAlbedo(FLinearColor(Look.fogAlbedoR, Look.fogAlbedoG, Look.fogAlbedoB).ToFColor(true));
				++Written;
			}
			break;
		}
	}

	// Practical lights: every light the map tags as one. A map built before the look knew them keeps its own values until a
	// world's look moves them. A sodium street lamp (also tagged FTSodium) takes the sodium colour.
	const bool bPracticals = Look.practicalKelvin != Current.practicalKelvin || Look.sodiumKelvin != Current.sodiumKelvin
		|| Look.practicalCandela != Current.practicalCandela || Look.practicalScatter != Current.practicalScatter;
	for (TActorIterator<AActor> It(World); It && bPracticals; ++It)
	{
		if (!It->ActorHasTag(PracticalTag))
		{
			continue;
		}
		const float Kelvin = It->ActorHasTag(SodiumTag) ? Look.sodiumKelvin : Look.practicalKelvin;
		TInlineComponentArray<ULocalLightComponent*> Lights(*It);
		for (ULocalLightComponent* Light : Lights)
		{
			Light->SetIntensityUnits(ELightUnits::Candelas);
			Light->SetIntensity(Look.practicalCandela);
			Light->SetUseTemperature(true);
			Light->SetTemperature(Kelvin);
			Light->SetVolumetricScatteringIntensity(Look.practicalScatter);
			++Written;
		}
	}
	// The cloud layer can only be hidden or shown: a map built without one stays clear.
	if (Look.hasClouds() != Current.hasClouds())
	{
		for (TActorIterator<AVolumetricCloud> It(World); It; ++It)
		{
			if (USceneComponent* Root = It->GetRootComponent())
			{
				Root->SetVisibility(Look.hasClouds(), true);
				++Written;
			}
		}
	}
	// The layer's base and depth, and with any cover the world's own clouds (M_Env_Clouds) in place of the engine's; the
	// engine's come back when the cover goes. Without the built material the layer keeps the engine's, with a warning.
	const bool bCloudCover = Look.cloudCover > 0.f;
	const bool bCloudMaterial = bCloudCover != (Current.cloudCover > 0.f);
	const bool bCloudLayer = bCloudMaterial || Look.cloudBaseKm != Current.cloudBaseKm || Look.cloudThickKm != Current.cloudThickKm;
	for (TActorIterator<AVolumetricCloud> It(World); It && bCloudLayer; ++It)
	{
		UVolumetricCloudComponent* Layer = It->FindComponentByClass<UVolumetricCloudComponent>();
		if (!Layer)
		{
			continue;
		}
		FT_LOOK_SET(cloudBaseKm, Layer->SetLayerBottomAltitude(Look.cloudBaseKm))
		FT_LOOK_SET(cloudThickKm, Layer->SetLayerHeight(Look.cloudThickKm))
		if (bCloudMaterial)
		{
			const TCHAR* Package = bCloudCover ? EnvCloudMaterial : EngineCloudMaterial;
			const FString Path = FString(Package) + TEXT(".") + FPackageName::GetShortName(Package);
			if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_Quiet | LOAD_NoWarn))
			{
				Layer->SetMaterial(Material);
				++Written;
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Look: no cloud material %s, so the layer keeps the one it has (FTBuildEnvMaterials builds %s)."),
					Package, EnvCloudMaterial);
			}
		}
	}
#undef FT_LOOK_SET
	return Written;
}

namespace
{
// World's instance of the collection in Package; null, with bBuilt false, when the collection hasn't been built (no
// material reads it, so there is nothing to fill).
UMaterialParameterCollectionInstance* CollectionInstance(UWorld* World, const TCHAR* Package, bool& bBuilt)
{
	const FString Path = FString(Package) + TEXT(".") + FPackageName::GetShortName(Package);
	const UMaterialParameterCollection* Collection = World ? LoadObject<UMaterialParameterCollection>(nullptr, *Path, nullptr, LOAD_Quiet | LOAD_NoWarn) : nullptr;
	bBuilt = Collection != nullptr;
	return Collection ? World->GetParameterCollectionInstance(Collection) : nullptr;
}
}

int32 FTScene::ApplyLookParameters(UWorld* World, const ftcore::SceneLook& Look)
{
	bool bLookBuilt = false;
	bool bEnvBuilt = false;
	UMaterialParameterCollectionInstance* LookInstance = CollectionInstance(World, LookCollection, bLookBuilt);
	UMaterialParameterCollectionInstance* EnvInstance = CollectionInstance(World, EnvCollection, bEnvBuilt);
	if ((bLookBuilt && !LookInstance) || (bEnvBuilt && !EnvInstance))
	{
		return -1;
	}
	int32 Written = 0;
	const auto Count = [&Written](bool bFound, const TCHAR* Collection, const TCHAR* Name, const TCHAR* Builder)
	{
		if (bFound)
		{
			++Written;
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Look: %s has no parameter %s; rebuild it with %s."), Collection, Name, Builder);
		}
	};
	if (LookInstance)
	{
		for (const FLookScalarParameter& Parameter : LookScalarParameters)
		{
			Count(LookInstance->SetScalarParameterValue(Parameter.Name, Look.*(Parameter.Field)), LookCollection, Parameter.Name,
				TEXT("FTBuildLookMaterials"));
		}
		for (const FLookVectorParameter& Parameter : LookVectorParameters)
		{
			Count(LookInstance->SetVectorParameterValue(Parameter.Name, FLinearColor(Look.*(Parameter.R), Look.*(Parameter.G), Look.*(Parameter.B), 1.f)),
				LookCollection, Parameter.Name, TEXT("FTBuildLookMaterials"));
		}
	}
	if (EnvInstance)
	{
		for (const FLookScalarParameter& Parameter : EnvScalarParameters)
		{
			Count(EnvInstance->SetScalarParameterValue(Parameter.Name, Look.*(Parameter.Field)), EnvCollection, Parameter.Name,
				TEXT("FTBuildEnvMaterials"));
		}
		const ftcore::SeaWaves Sea = ftcore::seaWaves(Look);
		for (int32 I = 0; I < ftcore::kSeaWaveCount; ++I)
		{
			float V[4];
			ftcore::packSeaWave(Sea.waves[I], V);
			Count(EnvInstance->SetVectorParameterValue(SeaWaveParameters[I], FLinearColor(V[0], V[1], V[2], V[3])), EnvCollection,
				SeaWaveParameters[I], TEXT("FTBuildEnvMaterials"));
		}
		Count(EnvInstance->SetScalarParameterValue(SeaSteepnessParameter, Sea.q), EnvCollection, SeaSteepnessParameter,
			TEXT("FTBuildEnvMaterials"));
	}
	return Written;
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

FString FTScene::SurfaceInstancePackage(const FString& Id)
{
	return FString::Printf(TEXT("%s/%s/MI_Surface_%s"), SurfaceFolder, *Id, *Id);
}

FString FTScene::WindowInstancePackage(const FString& Id)
{
	return FString::Printf(TEXT("%s/MIW_%s"), WindowFolder, *Id);
}

FString FTScene::SurfaceTexturePackage(const FString& Id, const TCHAR* Suffix)
{
	return FString::Printf(TEXT("%s/%s/T_%s_%s"), SurfaceFolder, *Id, *Id, Suffix);
}

UTexture* FTScene::LoadSurfaceTexture(const FString& Id, const TCHAR* Suffix)
{
	const FString Package = SurfaceTexturePackage(Id, Suffix);
	return LoadObject<UTexture>(nullptr, *(Package + TEXT(".") + FPackageName::GetShortName(Package)), nullptr,
		LOAD_NoWarn | LOAD_Quiet);
}

#if WITH_EDITOR
UTexture2D* FTScene::NewGroundMask(UObject* Outer, const TCHAR* Name, int32 Size, const uint8* Bytes, EObjectFlags Flags)
{
	UTexture2D* Texture = NewObject<UTexture2D>(Outer, Name, Flags);
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(Size, Size, 1, 1, TSF_G8, Bytes);
	// A road mask is mostly empty and a shape mask smooth, so a PNG of either is small.
	Texture->Source.Compress();
	Texture->SRGB = false;
	Texture->CompressionSettings = TC_Grayscale;
	Texture->LODGroup = TEXTUREGROUP_World;
	Texture->NeverStream = true;
	Texture->VirtualTextureStreaming = false;
	// Clamped, so past its edge a read finds the outermost ring: no road, as ftcore::roadMask leaves it empty, or the shape
	// at the field's edge.
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->PostEditChange();
	return Texture;
}
#endif

bool FTScene::LoadSurfaceTable(ftcore::SurfaceTable& Table)
{
	Table = ftcore::SurfaceTable{};
	const FString File = FPaths::ProjectDir() / TEXT("Data/Field/surfaces.json");
	if (!IFileManager::Get().FileExists(*File))
	{
		return true;
	}
	TArray<uint8> Bytes;
	std::string Error;
	std::vector<std::string> Warnings;
	if (!FFileHelper::LoadFileToArray(Bytes, *File) || !ftcore::importSurfaceTable(FT::BytesAsText(Bytes), Table, &Error, &Warnings))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("Data/Field/surfaces.json refused: %s"), *FT::ToFString(Error).TrimEnd());
		return false;
	}
	for (const std::string& Warning : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Data/Field/surfaces.json: %s"), *FT::ToFString(Warning));
	}
	return true;
}

UMaterialInterface* FTScene::LoadSurfaceFor(const ftcore::SurfaceTable& Table, ftcore::WorldKind Kind,
	ftcore::SurfaceRole Role, FLinearColor& OutTint)
{
	const ftcore::SurfaceSpec* Spec = Table.surfaceFor(Kind, Role);
	if (!Spec)
	{
		return nullptr;
	}
	const FString Package = SurfaceInstancePackage(FT::ToFString(Spec->id));
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr,
		*(Package + TEXT(".") + FPackageName::GetShortName(Package)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Material)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s is missing; run FTBuildSurfaces first. The %s keeps its flat tint."),
			*Package, *FT::ToFString(ftcore::surfaceRoleName(Role)));
		return nullptr;
	}
	const std::array<float, 3>& Tint = Table.use(Kind, Role).tint;
	OutTint = FLinearColor(Tint[0], Tint[1], Tint[2]);
	return Material;
}
