#include "FTBuildStyleMaterialsCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/BlendableInterface.h"
#include "FTStyleParameters.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/StylePack.h"

#include "FTStyleShader.inl"
#endif

#if WITH_EDITOR
namespace
{
struct FInstanceSpec
{
	const TCHAR* Package;
	ftcore::StyleSettings Style;
};

FString PackageFile(const TCHAR* Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

// Adds an expression to the material the way the material editor does.
template <typename T>
T* AddExpression(UMaterial* Material, int32 X, int32 Y)
{
	T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

void AddInput(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(0, From);
}

UMaterial* BuildStyleMaterial(UPackage* Package)
{
	UMaterial* Material = NewObject<UMaterial>(Package, FName(FPackageName::GetShortName(Package->GetName())),
		RF_Public | RF_Standalone | RF_Transactional);
	Material->MaterialDomain = MD_PostProcess;
	// After tonemapping: the pack colours are display-space sRGB (docs/STYLE_PACKS_UE.md).
	Material->BlendableLocation = BL_SceneColorAfterTonemapping;
	Material->BlendablePriority = 0;

	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -400, 0);
	Custom->Description = TEXT("FTStyle");
	Custom->OutputType = CMOT_Float3;
	Custom->Code = FTStyleShaderCode;
	Custom->Inputs.Reset();

	// Scene inputs. Wiring the SceneTexture nodes in is what makes the material read PostProcessInput0 and
	// SceneDepth; the HLSL samples the depth ring itself through SceneTextureLookup.
	UMaterialExpressionSceneTexture* Color = AddExpression<UMaterialExpressionSceneTexture>(Material, -1200, -400);
	Color->SceneTextureId = PPI_PostProcessInput0;
	AddInput(Custom, TEXT("SceneColor"), Color);
	UMaterialExpressionSceneTexture* Depth = AddExpression<UMaterialExpressionSceneTexture>(Material, -1200, -250);
	Depth->SceneTextureId = PPI_SceneDepth;
	AddInput(Custom, TEXT("SceneDepthIn"), Depth);
	UMaterialExpressionWorldPosition* WorldPos = AddExpression<UMaterialExpressionWorldPosition>(Material, -1200, -100);
	AddInput(Custom, TEXT("WorldPos"), WorldPos);
	UMaterialExpressionTime* Time = AddExpression<UMaterialExpressionTime>(Material, -1200, 0);
	AddInput(Custom, TEXT("TimeSec"), Time);

	// One parameter per style field, defaulting to the identity (None).
	int32 Row = 0;
	FTStyle::VisitParameters(
		ftcore::StyleSettings{},
		[&](FName Name, float Value)
		{
			UMaterialExpressionScalarParameter* P =
				AddExpression<UMaterialExpressionScalarParameter>(Material, -900, 100 + 60 * Row++);
			P->ParameterName = Name;
			P->DefaultValue = Value;
			P->Group = TEXT("Style");
			P->UpdateParameterGuid(true, false);
			AddInput(Custom, Name, P);
		},
		[&](FName Name, const FLinearColor& Value)
		{
			UMaterialExpressionVectorParameter* P =
				AddExpression<UMaterialExpressionVectorParameter>(Material, -900, 100 + 60 * Row++);
			P->ParameterName = Name;
			P->DefaultValue = Value;
			P->Group = TEXT("Style");
			P->UpdateParameterGuid(true, false);
			AddInput(Custom, Name, P);
		});
	UMaterialExpressionScalarParameter* Sky = AddExpression<UMaterialExpressionScalarParameter>(Material, -900, 100 + 60 * Row++);
	Sky->ParameterName = FTStyle::SkyDepthName();
	Sky->DefaultValue = FTStyle::DefaultSkyDepth;
	Sky->Group = TEXT("Scene");
	Sky->UpdateParameterGuid(true, false);
	AddInput(Custom, Sky->ParameterName, Sky);

	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Custom);

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

UMaterialInstanceConstant* BuildInstance(UPackage* Package, UMaterial* Parent, const ftcore::StyleSettings& Style)
{
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Package,
		FName(FPackageName::GetShortName(Package->GetName())), RF_Public | RF_Standalone | RF_Transactional);
	Instance->SetParentEditorOnly(Parent);
	FTStyle::VisitParameters(
		Style,
		[Instance](FName Name, float Value) { Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Name), Value); },
		[Instance](FName Name, const FLinearColor& Value)
		{ Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Name), Value); });
	Instance->PreEditChange(nullptr);
	Instance->PostEditChange();
	return Instance;
}

bool Save(UPackage* Package, UObject* Asset)
{
	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(*Package->GetName());
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildStyleMaterials: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildStyleMaterials: saved %s."), *File);
	return true;
}
}
#endif

UFTBuildStyleMaterialsCommandlet::UFTBuildStyleMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildStyleMaterialsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	const FInstanceSpec Instances[] = {
		{TEXT("/Game/Rendering/MI_Style_Solhaven"), ftcore::stylePack(ftcore::StylePack::Solhaven)},
		{TEXT("/Game/Rendering/MI_Style_Lattice"), ftcore::stylePack(ftcore::StylePack::Lattice)},
		{TEXT("/Game/Rendering/MI_Style_Unwritten"), ftcore::stylePack(ftcore::StylePack::Unwritten)},
		{TEXT("/Game/Rendering/MI_Style_Restored"), ftcore::stylePack(ftcore::StylePack::Restored)},
		{TEXT("/Game/Rendering/MI_Style_InkSolhaven"), ftcore::inkSolhavenStyle()},
	};

	// Start from nothing: replace every file up front, before anything loads the old assets.
	TArray<const TCHAR*> Packages;
	Packages.Add(FTStyle::MaterialPackage);
	for (const FInstanceSpec& Spec : Instances)
	{
		Packages.Add(Spec.Package);
	}
	for (const TCHAR* Package : Packages)
	{
		const FString File = PackageFile(Package);
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildStyleMaterials: couldn't replace %s."), *File);
			return 1;
		}
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFile(FTStyle::MaterialPackage)), true);

	UPackage* MaterialPackage = CreatePackage(FTStyle::MaterialPackage);
	UMaterial* Material = BuildStyleMaterial(MaterialPackage);
	TArray<TPair<UPackage*, UObject*>> ToSave;
	ToSave.Emplace(MaterialPackage, Material);

	for (const FInstanceSpec& Spec : Instances)
	{
		UPackage* Package = CreatePackage(Spec.Package);
		ToSave.Emplace(Package, BuildInstance(Package, Material, Spec.Style));
	}

	// Under -NullRHI there may be nothing to compile; with a renderer, let the shaders finish first.
	FAssetCompilingManager::Get().FinishAllCompilation();

	bool bOk = true;
	for (const TPair<UPackage*, UObject*>& Item : ToSave)
	{
		bOk &= Save(Item.Key, Item.Value);
	}
	if (!bOk)
	{
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildStyleMaterials: built PP_Style (%d expressions) and %d instances."),
		Material->GetExpressionCollection().Expressions.Num(), static_cast<int32>(UE_ARRAY_COUNT(Instances)));
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildStyleMaterials needs an editor build."));
	return 1;
#endif
}
