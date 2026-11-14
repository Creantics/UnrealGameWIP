#include "FTBuildWaveMaterialsCommandlet.h"

#include "ForgottenThird.h"
#include "FTWorldWave.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FTImportMaterials.h"
#include "FTStyleParameters.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "FTWorldWaveShader.inl"
#endif

#if WITH_EDITOR
namespace
{
const TCHAR* const kTag = TEXT("FTBuildWaveMaterials");

FString WavePackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

FString ObjectPath(const FString& Package)
{
	return Package + TEXT(".") + FPackageName::GetShortName(Package);
}

FString TwinPackage(const TCHAR* Master)
{
	return FString(FTWorldWave::Folder) / (FPackageName::GetShortName(Master) + FTWorldWave::TwinSuffix);
}

// Adds an expression to a material the way the material editor does.
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

// The same in a material function.
template <typename T>
T* AddFunctionExpression(UMaterialFunction* Function, int32 X, int32 Y)
{
	T* Expression = NewObject<T>(Function, NAME_None, RF_Transactional);
	Function->GetExpressionCollection().AddExpression(Expression);
	Expression->Function = Function;
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

void AddOutput(UMaterialExpressionCustom* Custom, FName Name, ECustomMaterialOutputType Type)
{
	FCustomOutput& Out = Custom->AdditionalOutputs.AddDefaulted_GetRef();
	Out.OutputName = Name;
	Out.OutputType = Type;
}

void SetCollectionParameter(UMaterialExpressionCollectionParameter* Node, UMaterialParameterCollection* Collection, const TCHAR* Name)
{
	Node->Collection = Collection;
	Node->ParameterName = FName(Name);
	Node->ParameterId = Collection->GetParameterId(FName(Name));
}

// The cut a master wires in (FTWorldWave.h): the shared ragged edge, the tag and local radius from custom primitive data.
UMaterialFunction* BuildCutFunction(UPackage* Package, UMaterialParameterCollection* Look)
{
	UMaterialFunction* Function = NewObject<UMaterialFunction>(Package, FName(FPackageName::GetShortName(Package->GetName())),
		RF_Public | RF_Standalone | RF_Transactional);
	Function->Description = TEXT("The world wave's cut: a tagged thing clipped on the front's ragged edge (FTWorldWave.h).");
	Function->bExposeToLibrary = false;

	UMaterialExpressionCustom* Custom = AddFunctionExpression<UMaterialExpressionCustom>(Function, -300, 0);
	Custom->Description = TEXT("FTWorldWaveCut");
	Custom->OutputType = CMOT_Float1;
	Custom->Code = FString(FTWorldWaveFnsCode) + FTWorldWaveCutBody;
	Custom->Inputs.Reset();
	AddInput(Custom, TEXT("WorldPos"), AddFunctionExpression<UMaterialExpressionWorldPosition>(Function, -900, -300));
	AddInput(Custom, TEXT("CamPos"), AddFunctionExpression<UMaterialExpressionCameraPositionWS>(Function, -900, -200));
	struct FCollectionInput
	{
		const TCHAR* Input;
		const TCHAR* Parameter;
	};
	const FCollectionInput FromCollection[] = {
		{TEXT("WOrigin"), FTWorldWave::Origin},
		{TEXT("WRadius"), FTWorldWave::Radius},
		{TEXT("WBand"), FTWorldWave::BandWidth},
		{TEXT("WDirection"), FTWorldWave::Direction},
		{TEXT("WTime"), FTWorldWave::WaveTime},
	};
	int32 Row = 0;
	for (const FCollectionInput& In : FromCollection)
	{
		UMaterialExpressionCollectionParameter* Node =
			AddFunctionExpression<UMaterialExpressionCollectionParameter>(Function, -900, -100 + 80 * Row++);
		SetCollectionParameter(Node, Look, In.Parameter);
		AddInput(Custom, In.Input, Node);
	}
	const auto AddPrimitiveData = [Function, &Row](const TCHAR* Name, int32 Slot)
	{
		UMaterialExpressionScalarParameter* P = AddFunctionExpression<UMaterialExpressionScalarParameter>(Function, -900, -100 + 80 * Row++);
		P->ParameterName = FName(Name);
		P->DefaultValue = 0.f;
		P->Group = TEXT("WorldWave");
		P->bUseCustomPrimitiveData = true;
		P->PrimitiveDataIndex = static_cast<uint8>(Slot);
		P->UpdateParameterGuid(true, false);
		return P;
	};
	AddInput(Custom, TEXT("Tag"), AddPrimitiveData(FTWorldWave::TagParameter, FTWorldWave::kTagSlot));
	AddInput(Custom, TEXT("LocalR"), AddPrimitiveData(FTWorldWave::LocalParameter, FTWorldWave::kLocalSlot));
	AddOutput(Custom, TEXT("InkOut"), CMOT_Float1);
	AddOutput(Custom, TEXT("CoreOut"), CMOT_Float3);
	Custom->RebuildOutputs();

	struct FOutputSpec
	{
		const TCHAR* Name;
		const TCHAR* Description;
		int32 CustomOutput;
	};
	const FOutputSpec Outputs[] = {
		{TEXT("Clip"), TEXT("1 keeps the pixel, 0 clips it: OpacityMask (or Opacity on a translucent master)."), 0},
		{TEXT("Ink"), TEXT("0..1 the arriving thing's leading strip: lerp the base colour to ink black by it."), 1},
		{TEXT("Core"), TEXT("The strip's paper-white core: add it to the emissive."), 2},
	};
	for (int32 I = 0; I < static_cast<int32>(UE_ARRAY_COUNT(Outputs)); ++I)
	{
		UMaterialExpressionFunctionOutput* Out = AddFunctionExpression<UMaterialExpressionFunctionOutput>(Function, 100, -100 + 120 * I);
		Out->OutputName = FName(Outputs[I].Name);
		Out->Description = Outputs[I].Description;
		Out->SortPriority = I;
		Out->Id = FGuid::NewDeterministicGuid(FString(TEXT("FTWorldWave.")) + Outputs[I].Name);
		Out->A.Connect(Outputs[I].CustomOutput, Custom);
	}

	Function->PreEditChange(nullptr);
	Function->PostEditChange();
	return Function;
}

// The ink post-process (FTWorldWaveShader.inl): after tonemapping, after the world style (priority 0).
UMaterial* BuildInkMaterial(UPackage* Package, UMaterialParameterCollection* Look)
{
	UMaterial* Material = NewObject<UMaterial>(Package, FName(FPackageName::GetShortName(Package->GetName())),
		RF_Public | RF_Standalone | RF_Transactional);
	Material->MaterialDomain = MD_PostProcess;
	Material->BlendableLocation = BL_SceneColorAfterTonemapping;
	Material->BlendablePriority = 10;

	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -400, 0);
	Custom->Description = TEXT("FTWorldWaveInk");
	Custom->OutputType = CMOT_Float3;
	Custom->Code = FString(FTWorldWaveFnsCode) + FTWorldWaveInkBody;
	Custom->Inputs.Reset();

	const auto AddScene = [Material, Custom](const TCHAR* Name, ESceneTextureId Id, int32 Y)
	{
		UMaterialExpressionSceneTexture* Node = AddExpression<UMaterialExpressionSceneTexture>(Material, -1300, Y);
		Node->SceneTextureId = Id;
		AddInput(Custom, Name, Node);
	};
	AddScene(TEXT("SceneColor"), PPI_PostProcessInput0, -800);
	AddScene(TEXT("SceneDepthIn"), PPI_SceneDepth, -700);
	AddScene(TEXT("SceneNormalIn"), PPI_WorldNormal, -600);
	AddInput(Custom, TEXT("WorldPos"), AddExpression<UMaterialExpressionWorldPosition>(Material, -1300, -500));

	struct FCollectionInput
	{
		const TCHAR* Input;
		const TCHAR* Parameter;
	};
	const FCollectionInput FromCollection[] = {
		{TEXT("WOrigin"), FTWorldWave::Origin},
		{TEXT("WOriginScreen"), FTWorldWave::OriginScreen},
		{TEXT("WFoot"), FTWorldWave::StreakFoot},
		{TEXT("WTip"), FTWorldWave::StreakTip},
		{TEXT("WRadius"), FTWorldWave::Radius},
		{TEXT("WBand"), FTWorldWave::BandWidth},
		{TEXT("WPrelude"), FTWorldWave::Prelude},
		{TEXT("WDirection"), FTWorldWave::Direction},
		{TEXT("WInside"), FTWorldWave::CameraInside},
		{TEXT("WTime"), FTWorldWave::WaveTime},
	};
	int32 Row = 0;
	for (const FCollectionInput& In : FromCollection)
	{
		UMaterialExpressionCollectionParameter* Node = AddExpression<UMaterialExpressionCollectionParameter>(Material, -1300, -400 + 70 * Row++);
		SetCollectionParameter(Node, Look, In.Parameter);
		AddInput(Custom, In.Input, Node);
	}

	struct FScalarSpec
	{
		const TCHAR* Name;
		float Value;
	};
	const FScalarSpec Scalars[] = {
		{TEXT("SceneKeep"), 0.15f},  // the scene's colour kept inside the front
		{TEXT("DisplacePx"), 12.f},  // the band's displacement of the lookup, pixels at 1080p
		{TEXT("SmearMinPx"), 20.f},  // the band's outward smear, pixels at 1080p
		{TEXT("SmearMaxPx"), 40.f},
		{TEXT("LinePx"), 1.5f},      // the ink lines' sampling ring
		{TEXT("HatchPx"), 7.f},      // the hatch's spacing on screen
		{TEXT("Tooth"), 0.12f},      // the paper's grain
		{TEXT("StreakPx"), 1.6f},    // the prelude streak's half width at its foot
	};
	UMaterialExpressionScalarParameter* Sky = AddExpression<UMaterialExpressionScalarParameter>(Material, -1000, 300);
	Sky->ParameterName = FTStyle::SkyDepthName();
	Sky->DefaultValue = FTStyle::DefaultSkyDepth;
	Sky->Group = TEXT("Scene");
	Sky->UpdateParameterGuid(true, false);
	AddInput(Custom, TEXT("SkyDepth"), Sky);
	Row = 0;
	for (const FScalarSpec& Spec : Scalars)
	{
		UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -1000, 380 + 70 * Row++);
		P->ParameterName = FName(Spec.Name);
		P->DefaultValue = Spec.Value;
		P->Group = TEXT("WorldWave");
		P->UpdateParameterGuid(true, false);
		AddInput(Custom, Spec.Name, P);
	}
	const auto AddColor = [Material, Custom, &Row](const TCHAR* Name, const float* Rgb)
	{
		UMaterialExpressionVectorParameter* P = AddExpression<UMaterialExpressionVectorParameter>(Material, -1000, 380 + 70 * Row++);
		P->ParameterName = FName(Name);
		P->DefaultValue = FLinearColor(Rgb[0], Rgb[1], Rgb[2], 1.f);
		P->Group = TEXT("WorldWave");
		P->UpdateParameterGuid(true, false);
		AddInput(Custom, Name, P);
	};
	AddColor(TEXT("PaperColor"), FTWorldWave::Paper);
	AddColor(TEXT("InkColor"), FTWorldWave::Ink);

	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Custom);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

// A prop master's twin with the cut wired in (FTWorldWave::PropMasters).
UMaterial* BuildTwin(const TCHAR* MasterPath)
{
	UMaterial* Master = LoadObject<UMaterial>(nullptr, *ObjectPath(MasterPath), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Master)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: no %s to twin (import a prop first)."), kTag, MasterPath);
		return nullptr;
	}
	const FString Path = TwinPackage(MasterPath);
	UPackage* Package = CreatePackage(*Path);
	UMaterial* Twin = DuplicateObject<UMaterial>(Master, Package, FName(FPackageName::GetShortName(Path)));
	Twin->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	Twin->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Twin->SetUsageByFlag(MATUSAGE_Nanite, true);
	if (!FTWorldWave::AddCut(kTag, Twin, -600, 900))
	{
		return nullptr;
	}
	Twin->PreEditChange(nullptr);
	Twin->PostEditChange();
	return Twin;
}

bool Save(UObject* Asset)
{
	UPackage* Package = Asset->GetPackage();
	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = WavePackageFile(Package->GetName());
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: couldn't save %s."), kTag, *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: saved %s."), kTag, *File);
	return true;
}
} // namespace

namespace FTWorldWave
{
bool AddCut(const TCHAR* Tag, UMaterial* Material, int32 X, int32 Y)
{
	if (!Material)
	{
		return false;
	}
	UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *ObjectPath(CutFunctionPath), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Function)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: no %s (run -run=FTBuildWaveMaterials first); %s takes no world-wave cut."), Tag,
			CutFunctionPath, *Material->GetName());
		return false;
	}
	UMaterialExpressionMaterialFunctionCall* Call = AddExpression<UMaterialExpressionMaterialFunctionCall>(Material, X, Y);
	if (!Call->SetMaterialFunction(Function) || Call->FunctionOutputs.Num() < 3)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s gives %d outputs, not Clip, Ink and Core; %s takes no world-wave cut."), Tag,
			CutFunctionPath, Call->FunctionOutputs.Num(), *Material->GetName());
		Material->GetExpressionCollection().RemoveExpression(Call);
		return false;
	}
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	const bool bTranslucent = Material->BlendMode != BLEND_Opaque && Material->BlendMode != BLEND_Masked;
	if (bTranslucent)
	{
		// Glass and the like: the opacity times the clip; no strip on a see-through surface.
		UMaterialExpressionMultiply* Product = AddExpression<UMaterialExpressionMultiply>(Material, X + 300, Y);
		if (Inputs->Opacity.Expression)
		{
			Product->A.Connect(Inputs->Opacity.OutputIndex, Inputs->Opacity.Expression);
		}
		else
		{
			Product->ConstA = Inputs->Opacity.UseConstant ? Inputs->Opacity.Constant : 1.f;
		}
		Product->B.Connect(0, Call);
		Inputs->Opacity.UseConstant = false;
		Inputs->Opacity.Connect(0, Product);
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s (translucent): opacity times the world-wave clip."), Tag, *Material->GetName());
		return true;
	}

	const bool bWasOpaque = Material->BlendMode == BLEND_Opaque;
	if (bWasOpaque)
	{
		Material->BlendMode = BLEND_Masked;
		Material->OpacityMaskClipValue = 0.5f;
	}
	if (Inputs->OpacityMask.Expression && !bWasOpaque)
	{
		UMaterialExpressionMultiply* Product = AddExpression<UMaterialExpressionMultiply>(Material, X + 300, Y);
		Product->A.Connect(Inputs->OpacityMask.OutputIndex, Inputs->OpacityMask.Expression);
		Product->B.Connect(0, Call);
		Inputs->OpacityMask.Connect(0, Product);
	}
	else
	{
		Inputs->OpacityMask.UseConstant = false;
		Inputs->OpacityMask.Connect(0, Call);
	}

	// The arriving thing's leading strip: the base colour to ink, the core on the emissive.
	if (Inputs->BaseColor.Expression)
	{
		UMaterialExpressionConstant3Vector* InkColor = AddExpression<UMaterialExpressionConstant3Vector>(Material, X, Y + 150);
		// Ink is display sRGB; the base colour is linear.
		InkColor->Constant = FLinearColor::FromSRGBColor(FColor(
			static_cast<uint8>(Ink[0] * 255.f + 0.5f), static_cast<uint8>(Ink[1] * 255.f + 0.5f), static_cast<uint8>(Ink[2] * 255.f + 0.5f)));
		UMaterialExpressionLinearInterpolate* Lerp = AddExpression<UMaterialExpressionLinearInterpolate>(Material, X + 300, Y + 150);
		Lerp->A.Connect(Inputs->BaseColor.OutputIndex, Inputs->BaseColor.Expression);
		Lerp->B.Connect(0, InkColor);
		Lerp->Alpha.Connect(1, Call);
		Inputs->BaseColor.Connect(0, Lerp);
	}
	if (Inputs->EmissiveColor.Expression)
	{
		UMaterialExpressionAdd* Sum = AddExpression<UMaterialExpressionAdd>(Material, X + 300, Y + 300);
		Sum->A.Connect(Inputs->EmissiveColor.OutputIndex, Inputs->EmissiveColor.Expression);
		Sum->B.Connect(2, Call);
		Inputs->EmissiveColor.Connect(0, Sum);
	}
	else
	{
		Inputs->EmissiveColor.UseConstant = false;
		Inputs->EmissiveColor.Connect(2, Call);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: world-wave cut wired in%s."), Tag, *Material->GetName(),
		bWasOpaque ? TEXT(" (opaque -> masked)") : TEXT(""));
	return true;
}
} // namespace FTWorldWave
#endif

UFTBuildWaveMaterialsCommandlet::UFTBuildWaveMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildWaveMaterialsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	// Start from nothing under the wave's folder: replace every file up front, before anything loads the old assets.
	// The look's collection is never deleted; the wave's parameters are added to it in place.
	TArray<FString> Packages = {FTWorldWave::CutFunctionPath, FTWorldWave::InkMaterialPath};
	for (const TCHAR* Master : FTWorldWave::PropMasters)
	{
		Packages.Add(TwinPackage(Master));
	}
	for (const FString& Package : Packages)
	{
		const FString File = WavePackageFile(Package);
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: couldn't replace %s."), kTag, *File);
			return 1;
		}
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(WavePackageFile(FTWorldWave::InkMaterialPath)), true);

	UMaterialParameterCollection* Look = FTWorldWave::LoadOrBuild();
	if (!Look)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s is missing or couldn't be saved (build it with -run=FTBuildLookMaterials)."), kTag,
			FTWorldWave::CollectionPath);
		return 1;
	}

	UMaterialFunction* Cut = BuildCutFunction(CreatePackage(FTWorldWave::CutFunctionPath), Look);
	if (!Save(Cut))
	{
		return 1;
	}
	UMaterial* Ink = BuildInkMaterial(CreatePackage(FTWorldWave::InkMaterialPath), Look);
	TArray<UMaterial*> Built = {Ink};
	for (const TCHAR* Master : FTWorldWave::PropMasters)
	{
		UMaterial* Twin = BuildTwin(Master);
		if (!Twin)
		{
			return 1;
		}
		Built.Add(Twin);
	}

	// Under -NullRHI there may be nothing to compile; with a renderer, let the shaders finish first.
	FAssetCompilingManager::Get().FinishAllCompilation();
	int32 Errors = 0;
	for (UMaterial* Material : Built)
	{
		Errors += FT::CompileErrors(kTag, Material);
	}
	bool bOk = Errors == 0;
	for (UMaterial* Material : Built)
	{
		bOk &= Save(Material);
	}
	if (!bOk)
	{
		return 1;
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("%s: built MF_WorldWaveCut, PP_WorldWave_Ink (%d expressions) and %d prop twins; the parameters are in %s. Now rebuild "
			 "the masters that wire the cut in: -run=FTImportPlant -Masters, -run=FTImportVehicle -Source=<the car's export>."),
		kTag, Ink->GetExpressionCollection().Expressions.Num(), Built.Num() - 1, FTWorldWave::CollectionPath);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildWaveMaterials needs an editor build."));
	return 1;
#endif
}
