#include "FTBuildEnvMaterialsCommandlet.h"

#include "ForgottenThird.h"
#include "FTSceneLook.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FTImportMaterials.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
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

void AddInput(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(0, From);
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

// One of the environment collection's parameters.
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

// The collection, its defaults SceneLook{}'s. Each parameter's id comes from its name, so a rebuild changes no id.
UMaterialParameterCollection* BuildCollection()
{
	UPackage* Package = CreatePackage(FTScene::EnvCollection);
	UMaterialParameterCollection* Collection = NewObject<UMaterialParameterCollection>(Package,
		FName(FPackageName::GetShortName(Package->GetName())), RF_Public | RF_Standalone | RF_Transactional);
	const ftcore::SceneLook Default;
	for (const FTScene::FLookScalarParameter& Parameter : FTScene::EnvScalarParameters)
	{
		FCollectionScalarParameter& Scalar = Collection->ScalarParameters.AddDefaulted_GetRef();
		Scalar.ParameterName = Parameter.Name;
		Scalar.DefaultValue = Default.*(Parameter.Field);
		Scalar.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTEnv.")) + Parameter.Name);
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
	const TCHAR* const Packages[] = {FTScene::EnvCollection, FTScene::EnvSkyMaterial};
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
