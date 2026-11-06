#include "FTBuildEnvMaterialsCommandlet.h"

#include "ForgottenThird.h"
#include "FTSceneLook.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FTImportMaterials.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVolumetricAdvancedMaterialOutput.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "ftcore/SeaWaves.h"
#endif

#if WITH_EDITOR
// Named, not anonymous: in a unity build the other material commandlets' own AddExpression, AddInput and PackageFile
// share this translation unit.
namespace FTEnvMaterialsBuild
{
// The plugin include the sky's custom node calls into (Plugins/FTLook/Shaders/Private/Env/Sky.ush).
const TCHAR* const kSkyInclude = TEXT("/Plugin/FTLook/Private/Env/Sky.ush");

// The view direction is the way from the camera to the pixel.
const TCHAR* const kSkyCode = TEXT(R"HLSL(return FTEnvSky(-Parameters.CameraVector, Atmosphere, Disk, Time,
	Stars, StarMag, StarTwinkle, StarExtinction, StarSkyFade,
	Glow, GlowKelvin, GlowDeg, GlowWidthDeg, GlowFloor, GlowZenith, GlowSkyFade,
	Flat, FlatLuminance);
)HLSL");

// The clouds' include (Env/Clouds.ush): the full sample, and the cheap bound the engine marches by.
const TCHAR* const kCloudsInclude = TEXT("/Plugin/FTLook/Private/Env/Clouds.ush");
const TCHAR* const kCloudCode = TEXT(R"HLSL(return FTEnvCloudExtinction(Position, H, Altitude, Time,
	Cover, Type, Density, WindDeg, WindMps, Drift, Underglow, UnderglowKelvin,
	Shape, ShapeSampler, Erosion, ErosionSampler, Glow);
)HLSL");
const TCHAR* const kCloudCoverCode = TEXT(R"HLSL(return FTEnvCloudCover(Position, H, Time, Cover, Type, WindDeg, WindMps, Drift);
)HLSL");
// The engine's tiling noise volumes the clouds take their shape and worn edges from.
const TCHAR* const kCloudShapeNoise = TEXT("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseShape128.T_VolumeNoiseShape128");
const TCHAR* const kCloudErosionNoise = TEXT("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseErosion32.T_VolumeNoiseErosion32");

// The sea's include (Env/Waves.ush): the vertices' offset, the pixel's surface and the water's medium. A custom node's
// inputs are single values, so the eight waves come in one by one and are gathered here.
const TCHAR* const kWavesInclude = TEXT("/Plugin/FTLook/Private/Env/Waves.ush");
const TCHAR* const kSeaOffsetCode = TEXT(R"HLSL(float4 W[8] = {W0, W1, W2, W3, W4, W5, W6, W7};
return FTEnvSeaOffset(Rest.xy, SeaTime, W, Q, GridCm);
)HLSL");
const TCHAR* const kSeaSurfaceCode = TEXT(R"HLSL(float4 W[8] = {W0, W1, W2, W3, W4, W5, W6, W7};
return FTEnvSeaSurface(Rest.xy, SeaTime, Time, W, Q, WaveCm, WaveSpeed, WaveStrength, Rough, EyeDepth, BehindDepth,
	Foam, FoamBandCm, WindDeg, RoughOut, FoamOut);
)HLSL");
const TCHAR* const kSeaMediumCode = TEXT(R"HLSL(return FTEnvSeaMedium(Absorb.rgb, Scatter.rgb, ScatterOut);
)HLSL");
// The grid step, in centimetres, of the mesh the sea is drawn on: waves shorter than four steps stay out of its vertices.
constexpr float kSeaGridCm = 100.f;

FString PackageFile(const TCHAR* Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

// Adds an expression to the material the way the material editor does.
UMaterialExpression* AddExpressionOf(UMaterial* Material, UClass* Class, int32 X, int32 Y)
{
	UMaterialExpression* Expression = NewObject<UMaterialExpression>(Material, Class, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

template <typename T>
T* AddExpression(UMaterial* Material, int32 X, int32 Y)
{
	return CastChecked<T>(AddExpressionOf(Material, T::StaticClass(), X, Y));
}

// An engine expression whose class isn't exported, made by name; null, with an error logged, when the engine has none.
UMaterialExpression* AddEngineExpression(UMaterial* Material, const TCHAR* ClassName, int32 X, int32 Y)
{
	UClass* Class = FindObject<UClass>(nullptr, *(FString(TEXT("/Script/Engine.")) + ClassName));
	if (!Class)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildEnvMaterials: the engine has no %s."), ClassName);
		return nullptr;
	}
	return AddExpressionOf(Material, Class, X, Y);
}

void AddInput(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From, int32 Output = 0)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(Output, From);
}

void AddOutput(UMaterialExpressionCustom* Custom, FName Name, ECustomMaterialOutputType Type)
{
	FCustomOutput& Out = Custom->AdditionalOutputs.AddDefaulted_GetRef();
	Out.OutputName = Name;
	Out.OutputType = Type;
}

UMaterialExpressionCustom* AddCustom(UMaterial* Material, const TCHAR* Description, const TCHAR* Include, const TCHAR* Code,
	ECustomMaterialOutputType Type, int32 Y)
{
	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -450, Y);
	Custom->Description = Description;
	Custom->OutputType = Type;
	Custom->Code = Code;
	Custom->IncludeFilePaths.Add(Include);
	Custom->Inputs.Reset();
	return Custom;
}

UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, FName Name, float Value, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("Env");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionVectorParameter* AddVector(UMaterial* Material, FName Name, const FLinearColor& Value, int32 Y)
{
	UMaterialExpressionVectorParameter* P = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("Env");
	P->UpdateParameterGuid(true, false);
	return P;
}

// A fixed texture (an engine one), sampled the way its own settings ask.
UMaterialExpressionTextureObject* AddTextureObject(UMaterial* Material, UTexture* Texture, int32 Y)
{
	UMaterialExpressionTextureObject* T = AddExpression<UMaterialExpressionTextureObject>(Material, -900, Y);
	T->Texture = Texture;
	T->SamplerType = MaterialExpressionUtils::GetSamplerTypeForTexture(Texture);
	return T;
}

// An engine texture; null, with an error logged, when the engine has none at Path.
UTexture* LoadEngineTexture(const TCHAR* Path)
{
	UTexture* Texture = LoadObject<UTexture>(nullptr, Path);
	if (!Texture)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildEnvMaterials: the engine has no texture %s."), Path);
	}
	return Texture;
}

// One of a parameter collection's parameters (the environment's, or the look's for the sea's water).
UMaterialExpressionCollectionParameter* AddEnv(UMaterial* Material, UMaterialParameterCollection* Collection, FName Name, int32 Y)
{
	UMaterialExpressionCollectionParameter* P = AddExpression<UMaterialExpressionCollectionParameter>(Material, -900, Y);
	P->Collection = Collection;
	P->ParameterName = Name;
	P->ParameterId = Collection->GetParameterId(Name);
	return P;
}

UMaterial* NewMaterial(const TCHAR* PackageName)
{
	UPackage* Package = CreatePackage(PackageName);
	return NewObject<UMaterial>(Package, FName(FPackageName::GetShortName(Package->GetName())), RF_Public | RF_Standalone | RF_Transactional);
}

void Finish(UMaterial* Material)
{
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
}

// The collection, its defaults SceneLook{}'s (the sea's waves the ones ftcore::seaWaves builds from it). Each parameter's
// id comes from its name, so a rebuild changes no id.
UMaterialParameterCollection* BuildCollection()
{
	UPackage* Package = CreatePackage(FTScene::EnvCollection);
	UMaterialParameterCollection* Collection = NewObject<UMaterialParameterCollection>(Package,
		FName(FPackageName::GetShortName(Package->GetName())), RF_Public | RF_Standalone | RF_Transactional);
	const ftcore::SceneLook Default;
	const auto AddScalarParameter = [Collection](const TCHAR* Name, float Value)
	{
		FCollectionScalarParameter& Scalar = Collection->ScalarParameters.AddDefaulted_GetRef();
		Scalar.ParameterName = Name;
		Scalar.DefaultValue = Value;
		Scalar.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTEnv.")) + Name);
	};
	for (const FTScene::FLookScalarParameter& Parameter : FTScene::EnvScalarParameters)
	{
		AddScalarParameter(Parameter.Name, Default.*(Parameter.Field));
	}
	const ftcore::SeaWaves Sea = ftcore::seaWaves(Default);
	AddScalarParameter(FTScene::SeaSteepnessParameter, Sea.q);
	for (int32 I = 0; I < ftcore::kSeaWaveCount; ++I)
	{
		float V[4];
		ftcore::packSeaWave(Sea.waves[I], V);
		FCollectionVectorParameter& Vector = Collection->VectorParameters.AddDefaulted_GetRef();
		Vector.ParameterName = FTScene::SeaWaveParameters[I];
		Vector.DefaultValue = FLinearColor(V[0], V[1], V[2], V[3]);
		Vector.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTEnv.")) + FTScene::SeaWaveParameters[I]);
	}
	Collection->PostEditChange();
	return Collection;
}

// The sky: unlit and opaque, drawn behind everything as the sky (so it takes no height fog or aerial perspective, and the
// sky light's real-time capture takes it in). The atmosphere's luminance along the view and its sun's disc (light 0)
// come from the engine's nodes; the stars, skyglow and flat dome from the world's look. The star and glow constants
// below are the material's own, to tune per instance: how much a star twinkles at the zenith (more toward the horizon),
// how many magnitudes the air dims it per air mass (a city's hazy air), how bright the atmosphere may be before stars
// fade (as a share of a magnitude-0 star's peak) and before the skyglow does (times the glow's own peak), how much of
// the glow the far side of the sky keeps, and how much of it the whole sky keeps overhead.
UMaterial* BuildSky(UMaterialParameterCollection* Env)
{
	UMaterial* Material = NewMaterial(FTScene::EnvSkyMaterial);
	Material->SetShadingModel(MSM_Unlit);
	Material->BlendMode = BLEND_Opaque;
	Material->bIsSky = true;

	UMaterialExpression* Atmosphere = AddEngineExpression(Material, TEXT("MaterialExpressionSkyAtmosphereViewLuminance"), -900, -300);
	UMaterialExpression* Disk = AddEngineExpression(Material, TEXT("MaterialExpressionSkyAtmosphereLightDiskLuminance"), -900, -200);
	if (!Atmosphere || !Disk)
	{
		return nullptr;
	}

	UMaterialExpressionCustom* Sky = AddCustom(Material, TEXT("FTEnvSky"), kSkyInclude, kSkyCode, CMOT_Float3, 0);
	AddInput(Sky, TEXT("Atmosphere"), Atmosphere);
	AddInput(Sky, TEXT("Disk"), Disk);
	AddInput(Sky, TEXT("Time"), AddExpression<UMaterialExpressionTime>(Material, -900, -100));
	AddInput(Sky, TEXT("Stars"), AddEnv(Material, Env, TEXT("SkyStars"), 0));
	AddInput(Sky, TEXT("StarMag"), AddEnv(Material, Env, TEXT("SkyStarMag"), 100));
	AddInput(Sky, TEXT("StarTwinkle"), AddScalar(Material, TEXT("StarTwinkle"), 0.1f, 200));
	AddInput(Sky, TEXT("StarExtinction"), AddScalar(Material, TEXT("StarExtinction"), 0.3f, 300));
	AddInput(Sky, TEXT("StarSkyFade"), AddScalar(Material, TEXT("StarSkyFade"), 0.02f, 400));
	AddInput(Sky, TEXT("Glow"), AddEnv(Material, Env, TEXT("SkyGlow"), 500));
	AddInput(Sky, TEXT("GlowKelvin"), AddEnv(Material, Env, TEXT("SkyGlowKelvin"), 600));
	AddInput(Sky, TEXT("GlowDeg"), AddEnv(Material, Env, TEXT("SkyGlowDeg"), 700));
	AddInput(Sky, TEXT("GlowWidthDeg"), AddEnv(Material, Env, TEXT("SkyGlowWidthDeg"), 800));
	AddInput(Sky, TEXT("GlowFloor"), AddScalar(Material, TEXT("GlowFloor"), 0.25f, 900));
	AddInput(Sky, TEXT("GlowZenith"), AddScalar(Material, TEXT("GlowZenith"), 0.04f, 1000));
	AddInput(Sky, TEXT("GlowSkyFade"), AddScalar(Material, TEXT("GlowSkyFade"), 4.f, 1100));
	AddInput(Sky, TEXT("Flat"), AddEnv(Material, Env, TEXT("SkyFlat"), 1200));
	AddInput(Sky, TEXT("FlatLuminance"), AddEnv(Material, Env, TEXT("SkyFlatLuminance"), 1300));
	Sky->RebuildOutputs();

	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Sky);
	Finish(Material);
	return Material;
}

// The clouds: a volume material for the volumetric cloud layer (Env/Clouds.ush). Its albedo is the world's cloudAlbedo,
// its extinction (the volume's SubsurfaceColor) and its glow the include's, and the output node holds the light: a
// forward lobe that gives the silver edge with the sun or moon behind, a weaker back lobe, one octave of the engine's
// multiple-scattering approximation so thick cloud stays bright (the second is the expensive one), and a marched
// shadow so the bases go dark. The cheap bound (FTEnvCloudCover, no texture) is its conservative density, so the
// engine skips the empty sky without running the full sample.
UMaterial* BuildClouds(UMaterialParameterCollection* Env)
{
	UTexture* ShapeNoise = LoadEngineTexture(kCloudShapeNoise);
	UTexture* ErosionNoise = LoadEngineTexture(kCloudErosionNoise);
	if (!ShapeNoise || !ErosionNoise)
	{
		return nullptr;
	}
	UMaterial* Material = NewMaterial(FTScene::EnvCloudMaterial);
	Material->MaterialDomain = MD_Volume;
	Material->SetUsageByFlag(MATUSAGE_VolumetricCloud, true);

	// The engine's cloud sample: its outputs are the altitude, the height in the layer and the share of the layer's depth.
	UMaterialExpression* CloudSample = AddEngineExpression(Material, TEXT("MaterialExpressionCloudSampleAttribute"), -900, -500);
	if (!CloudSample)
	{
		return nullptr;
	}
	UMaterialExpressionWorldPosition* Position = AddExpression<UMaterialExpressionWorldPosition>(Material, -900, -600);
	UMaterialExpressionTime* Time = AddExpression<UMaterialExpressionTime>(Material, -900, -400);
	UMaterialExpressionCollectionParameter* Cover = AddEnv(Material, Env, TEXT("CloudCover"), -300);
	UMaterialExpressionCollectionParameter* Type = AddEnv(Material, Env, TEXT("CloudType"), -200);
	UMaterialExpressionCollectionParameter* WindDeg = AddEnv(Material, Env, TEXT("WindDeg"), -100);
	UMaterialExpressionCollectionParameter* WindMps = AddEnv(Material, Env, TEXT("WindMps"), 0);
	UMaterialExpressionCollectionParameter* Drift = AddEnv(Material, Env, TEXT("CloudDrift"), 100);

	UMaterialExpressionCustom* Cloud = AddCustom(Material, TEXT("FTEnvCloudExtinction"), kCloudsInclude, kCloudCode, CMOT_Float3, 0);
	AddOutput(Cloud, TEXT("Glow"), CMOT_Float3);
	AddInput(Cloud, TEXT("Position"), Position);
	AddInput(Cloud, TEXT("H"), CloudSample, 2);
	AddInput(Cloud, TEXT("Altitude"), CloudSample, 1);
	AddInput(Cloud, TEXT("Time"), Time);
	AddInput(Cloud, TEXT("Cover"), Cover);
	AddInput(Cloud, TEXT("Type"), Type);
	AddInput(Cloud, TEXT("Density"), AddEnv(Material, Env, TEXT("CloudDensity"), 200));
	AddInput(Cloud, TEXT("WindDeg"), WindDeg);
	AddInput(Cloud, TEXT("WindMps"), WindMps);
	AddInput(Cloud, TEXT("Drift"), Drift);
	AddInput(Cloud, TEXT("Underglow"), AddEnv(Material, Env, TEXT("CloudUnderglow"), 300));
	AddInput(Cloud, TEXT("UnderglowKelvin"), AddEnv(Material, Env, TEXT("CloudUnderglowKelvin"), 400));
	AddInput(Cloud, TEXT("Shape"), AddTextureObject(Material, ShapeNoise, 500));
	AddInput(Cloud, TEXT("Erosion"), AddTextureObject(Material, ErosionNoise, 600));
	Cloud->RebuildOutputs();

	UMaterialExpressionCustom* Bound = AddCustom(Material, TEXT("FTEnvCloudCover"), kCloudsInclude, kCloudCoverCode, CMOT_Float1, 800);
	AddInput(Bound, TEXT("Position"), Position);
	AddInput(Bound, TEXT("H"), CloudSample, 2);
	AddInput(Bound, TEXT("Time"), Time);
	AddInput(Bound, TEXT("Cover"), Cover);
	AddInput(Bound, TEXT("Type"), Type);
	AddInput(Bound, TEXT("WindDeg"), WindDeg);
	AddInput(Bound, TEXT("WindMps"), WindMps);
	AddInput(Bound, TEXT("Drift"), Drift);
	Bound->RebuildOutputs();

	UMaterialExpressionVolumetricAdvancedMaterialOutput* Light = AddExpression<UMaterialExpressionVolumetricAdvancedMaterialOutput>(Material, 0, 800);
	Light->ConstPhaseG = 0.7f;
	Light->ConstPhaseG2 = -0.2f;
	Light->ConstPhaseBlend = 0.3f;
	Light->MultiScatteringApproximationOctaveCount = 1;
	Light->ConstMultiScatteringContribution = 0.6f;
	Light->ConstMultiScatteringOcclusion = 0.4f;
	Light->ConstMultiScatteringEccentricity = 0.6f;
	Light->bRayMarchVolumeShadow = true;
	Light->ConservativeDensity.Connect(0, Bound);

	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, AddEnv(Material, Env, TEXT("CloudAlbedo"), -700));
	Inputs->SubsurfaceColor.Connect(0, Cloud);
	Inputs->EmissiveColor.Connect(1, Cloud);
	Finish(Material);
	return Material;
}

// The sea: single-layer water over open water, on a grid mesh kSeaGridCm a step (Env/Waves.ush). The eight waves
// (ftcore::seaWaves, through MPC_Env) move the vertices and give each pixel its exact normal and whitecaps, evaluated at
// the pixel's rest place (the world position without the offset), so the two agree; the look water's ripples, colour
// and medium (MPC_Look) ride over them, and the foam is the opaque top layer the water output blends in. The waves run
// on a time that wraps every ftcore::kSeaLoopS, where they repeat exactly, so it keeps its precision on a long run;
// the ripples and foam drift on the plain time.
UMaterial* BuildSea(UMaterialParameterCollection* Env, UMaterialParameterCollection* Look)
{
	UMaterial* Material = NewMaterial(FTScene::EnvSeaMaterial);
	Material->SetShadingModel(MSM_SingleLayerWater);
	Material->BlendMode = BLEND_Opaque;
	Material->bTangentSpaceNormal = false;

	UMaterialExpression* Behind = AddEngineExpression(Material, TEXT("MaterialExpressionSceneDepthWithoutWater"), -900, 1500);
	if (!Behind)
	{
		return nullptr;
	}
	UMaterialExpressionWorldPosition* Rest = AddExpression<UMaterialExpressionWorldPosition>(Material, -900, -700);
	Rest->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
	UMaterialExpressionTime* SeaTime = AddExpression<UMaterialExpressionTime>(Material, -900, -600);
	SeaTime->bOverride_Period = true;
	SeaTime->Period = ftcore::kSeaLoopS;
	UMaterialExpressionTime* Time = AddExpression<UMaterialExpressionTime>(Material, -900, -500);
	UMaterialExpression* Waves[ftcore::kSeaWaveCount];
	for (int32 I = 0; I < ftcore::kSeaWaveCount; ++I)
	{
		Waves[I] = AddEnv(Material, Env, FTScene::SeaWaveParameters[I], -400 + 100 * I);
	}
	UMaterialExpressionCollectionParameter* Q = AddEnv(Material, Env, FTScene::SeaSteepnessParameter, 400);
	UMaterialExpressionCollectionParameter* WindDeg = AddEnv(Material, Env, TEXT("WindDeg"), 500);

	UMaterialExpressionCustom* Offset = AddCustom(Material, TEXT("FTEnvSeaOffset"), kWavesInclude, kSeaOffsetCode, CMOT_Float3, -800);
	AddInput(Offset, TEXT("Rest"), Rest);
	AddInput(Offset, TEXT("SeaTime"), SeaTime);
	for (int32 I = 0; I < ftcore::kSeaWaveCount; ++I)
	{
		AddInput(Offset, *FString::Printf(TEXT("W%d"), I), Waves[I]);
	}
	AddInput(Offset, TEXT("Q"), Q);
	AddInput(Offset, TEXT("GridCm"), AddScalar(Material, TEXT("GridCm"), kSeaGridCm, -800));
	Offset->RebuildOutputs();

	UMaterialExpressionCustom* Surface = AddCustom(Material, TEXT("FTEnvSeaSurface"), kWavesInclude, kSeaSurfaceCode, CMOT_Float3, 0);
	AddOutput(Surface, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Surface, TEXT("FoamOut"), CMOT_Float1);
	AddInput(Surface, TEXT("Rest"), Rest);
	AddInput(Surface, TEXT("SeaTime"), SeaTime);
	AddInput(Surface, TEXT("Time"), Time);
	for (int32 I = 0; I < ftcore::kSeaWaveCount; ++I)
	{
		AddInput(Surface, *FString::Printf(TEXT("W%d"), I), Waves[I]);
	}
	AddInput(Surface, TEXT("Q"), Q);
	AddInput(Surface, TEXT("WaveCm"), AddEnv(Material, Look, TEXT("WaterWaveCm"), 600));
	AddInput(Surface, TEXT("WaveSpeed"), AddEnv(Material, Look, TEXT("WaterWaveSpeed"), 700));
	AddInput(Surface, TEXT("WaveStrength"), AddEnv(Material, Look, TEXT("WaterWaveStrength"), 800));
	AddInput(Surface, TEXT("Rough"), AddEnv(Material, Look, TEXT("WaterRoughness"), 900));
	AddInput(Surface, TEXT("EyeDepth"), AddExpression<UMaterialExpressionPixelDepth>(Material, -900, 1000));
	AddInput(Surface, TEXT("BehindDepth"), Behind);
	AddInput(Surface, TEXT("Foam"), AddEnv(Material, Env, TEXT("SeaFoam"), 1100));
	AddInput(Surface, TEXT("FoamBandCm"), AddEnv(Material, Env, TEXT("SeaFoamBandCm"), 1200));
	AddInput(Surface, TEXT("WindDeg"), WindDeg);
	Surface->RebuildOutputs();

	UMaterialExpressionCustom* Medium = AddCustom(Material, TEXT("FTEnvSeaMedium"), kWavesInclude, kSeaMediumCode, CMOT_Float3, 1700);
	AddOutput(Medium, TEXT("ScatterOut"), CMOT_Float3);
	AddInput(Medium, TEXT("Absorb"), AddEnv(Material, Look, TEXT("WaterAbsorb"), 1700));
	AddInput(Medium, TEXT("Scatter"), AddEnv(Material, Look, TEXT("WaterScatter"), 1800));
	Medium->RebuildOutputs();

	UMaterialExpressionSingleLayerWaterMaterialOutput* Water = AddExpression<UMaterialExpressionSingleLayerWaterMaterialOutput>(Material, 0, 1700);
	Water->AbsorptionCoefficients.Connect(0, Medium);
	Water->ScatteringCoefficients.Connect(1, Medium);
	Water->PhaseG.Connect(0, AddEnv(Material, Look, TEXT("WaterPhaseG"), 1900));

	// Water's own reflectance (F0 0.02) and index; the foam is an off-white top layer, matte by the surface's roughness.
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->WorldPositionOffset.Connect(0, Offset);
	Inputs->BaseColor.Connect(0, AddVector(Material, TEXT("FoamColor"), FLinearColor(0.8f, 0.8f, 0.78f), -300));
	Inputs->Specular.Connect(0, AddScalar(Material, TEXT("Specular"), 0.25f, -200));
	Inputs->Refraction.Connect(0, AddScalar(Material, TEXT("IOR"), 1.33f, -100));
	Inputs->Normal.Connect(0, Surface);
	Inputs->Roughness.Connect(1, Surface);
	Inputs->Opacity.Connect(2, Surface);
	Finish(Material);
	return Material;
}

bool Save(UObject* Asset, const TCHAR* PackageName)
{
	const FString File = PackageFile(PackageName);
	FAssetRegistryModule::AssetCreated(Asset);
	UPackage* Package = Asset->GetPackage();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildEnvMaterials: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildEnvMaterials: saved %s."), *File);
	return true;
}
} // namespace FTEnvMaterialsBuild
#endif

UFTBuildEnvMaterialsCommandlet::UFTBuildEnvMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildEnvMaterialsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	using namespace FTEnvMaterialsBuild;
	// The sea reads the look's water from its collection, so that one is built first (FTBuildLookMaterials); it's looked
	// for before anything here is replaced.
	const FString LookPath = FString(FTScene::LookCollection) + TEXT(".") + FPackageName::GetShortName(FTScene::LookCollection);
	UMaterialParameterCollection* Look = LoadObject<UMaterialParameterCollection>(nullptr, *LookPath);
	if (!Look)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildEnvMaterials: %s is missing; build it first with FTBuildLookMaterials."),
			FTScene::LookCollection);
		return 1;
	}
	const TCHAR* const Packages[] = {FTScene::EnvCollection, FTScene::EnvSkyMaterial, FTScene::EnvCloudMaterial, FTScene::EnvSeaMaterial};
	// Start from nothing: replace the files up front, before anything loads the old assets.
	for (const TCHAR* Package : Packages)
	{
		const FString File = PackageFile(Package);
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildEnvMaterials: couldn't replace %s."), *File);
			return 1;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	}

	UMaterialParameterCollection* Env = BuildCollection();
	if (!Save(Env, FTScene::EnvCollection))
	{
		return 1;
	}
	struct FBuilt
	{
		UMaterial* Material;
		const TCHAR* Package;
	};
	const FBuilt Built[] = {
		{BuildSky(Env), FTScene::EnvSkyMaterial},
		{BuildClouds(Env), FTScene::EnvCloudMaterial},
		{BuildSea(Env, Look), FTScene::EnvSeaMaterial},
	};
	FAssetCompilingManager::Get().FinishAllCompilation();
	int32 Errors = 0;
	for (const FBuilt& Each : Built)
	{
		Errors += Each.Material ? FT::CompileErrors(TEXT("FTBuildEnvMaterials"), Each.Material) : 1;
	}
	if (Errors > 0)
	{
		return 1;
	}
	for (const FBuilt& Each : Built)
	{
		if (!Save(Each.Material, Each.Package))
		{
			return 1;
		}
	}
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildEnvMaterials needs an editor build."));
	return 1;
#endif
}
