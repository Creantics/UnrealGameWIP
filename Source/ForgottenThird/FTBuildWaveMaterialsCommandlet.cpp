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
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
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
	Custom->Code = FTWorldWaveCutBody;
	Custom->IncludeFilePaths.Add(FTWorldWaveCommonInclude);
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
		{TEXT("Ink"), TEXT("0 (kept for the masters wired to it)."), 1},
		{TEXT("Core"), TEXT("0 (kept for the masters wired to it)."), 2},
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

struct FCollectionInput
{
	const TCHAR* Input;
	const TCHAR* Parameter;
};

// The surface state behind the front (State.ush), wired into the surface masters by AddState: BaseColor, Roughness and
// Emissive in, the same three out.
UMaterialFunction* BuildStateFunction(UPackage* Package, UMaterialParameterCollection* Look)
{
	UMaterialFunction* Function = NewObject<UMaterialFunction>(Package, FName(FPackageName::GetShortName(Package->GetName())),
		RF_Public | RF_Standalone | RF_Transactional);
	Function->Description = TEXT("The world wave's surface state: behind the front a surface drains near grey, darkens, goes damp and "
								 "stained, and its light dies (FTWorldWave.h).");
	Function->bExposeToLibrary = false;

	struct FInputSpec
	{
		const TCHAR* Name;
		EFunctionInputType Type;
		FVector4f Preview;
	};
	const FInputSpec InputSpecs[] = {
		{TEXT("BaseColor"), FunctionInput_Vector3, FVector4f(0.5f, 0.5f, 0.5f, 0.f)},
		{TEXT("Roughness"), FunctionInput_Scalar, FVector4f(0.5f, 0.f, 0.f, 0.f)},
		{TEXT("Emissive"), FunctionInput_Vector3, FVector4f(0.f, 0.f, 0.f, 0.f)},
	};
	UMaterialExpressionFunctionInput* FunctionInputs[3] = {};
	for (int32 I = 0; I < 3; ++I)
	{
		UMaterialExpressionFunctionInput* In = AddFunctionExpression<UMaterialExpressionFunctionInput>(Function, -1200, -300 + 120 * I);
		In->InputName = FName(InputSpecs[I].Name);
		In->InputType = InputSpecs[I].Type;
		In->PreviewValue = InputSpecs[I].Preview;
		In->bUsePreviewValueAsDefault = true;
		In->SortPriority = I;
		In->Id = FGuid::NewDeterministicGuid(FString(TEXT("FTWorldWaveState.In.")) + InputSpecs[I].Name);
		FunctionInputs[I] = In;
	}

	UMaterialExpressionCustom* Custom = AddFunctionExpression<UMaterialExpressionCustom>(Function, -300, 0);
	Custom->Description = TEXT("FTWorldWaveState");
	Custom->OutputType = CMOT_Float3;
	Custom->Code = FTWorldWaveStateBody;
	Custom->IncludeFilePaths.Add(FTWorldWaveStateInclude);
	Custom->Inputs.Reset();
	AddInput(Custom, TEXT("Base"), FunctionInputs[0]);
	AddInput(Custom, TEXT("Rough"), FunctionInputs[1]);
	AddInput(Custom, TEXT("Emis"), FunctionInputs[2]);
	AddInput(Custom, TEXT("WorldPos"), AddFunctionExpression<UMaterialExpressionWorldPosition>(Function, -900, 100));
	AddInput(Custom, TEXT("Normal"), AddFunctionExpression<UMaterialExpressionVertexNormalWS>(Function, -900, 160));
	const FCollectionInput FromCollection[] = {
		{TEXT("WOrigin"), FTWorldWave::Origin},
		{TEXT("WOriginScreen"), FTWorldWave::OriginScreen},
		{TEXT("WRadius"), FTWorldWave::Radius},
		{TEXT("WBand"), FTWorldWave::BandWidth},
		{TEXT("WDirection"), FTWorldWave::Direction},
		{TEXT("WTime"), FTWorldWave::WaveTime},
		{TEXT("WKeep0"), FTWorldWave::Keep0},
		{TEXT("WKeep1"), FTWorldWave::Keep1},
	};
	int32 Row = 0;
	for (const FCollectionInput& In : FromCollection)
	{
		UMaterialExpressionCollectionParameter* Node =
			AddFunctionExpression<UMaterialExpressionCollectionParameter>(Function, -900, 240 + 80 * Row++);
		SetCollectionParameter(Node, Look, In.Parameter);
		AddInput(Custom, In.Input, Node);
	}
	AddOutput(Custom, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Custom, TEXT("EmisOut"), CMOT_Float3);
	Custom->RebuildOutputs();

	const TCHAR* const OutputNames[] = {TEXT("BaseColor"), TEXT("Roughness"), TEXT("Emissive")};
	for (int32 I = 0; I < 3; ++I)
	{
		UMaterialExpressionFunctionOutput* Out = AddFunctionExpression<UMaterialExpressionFunctionOutput>(Function, 100, -100 + 120 * I);
		Out->OutputName = FName(OutputNames[I]);
		Out->SortPriority = I;
		Out->Id = FGuid::NewDeterministicGuid(FString(TEXT("FTWorldWaveState.Out.")) + OutputNames[I]);
		Out->A.Connect(I, Custom);
	}

	Function->PreEditChange(nullptr);
	Function->PostEditChange();
	return Function;
}

// The post-process (FTWorldWaveShader.inl, Post.ush): before depth of field, on the linear scene colour, so the
// second world's grade sits under the look's own tonemap.
UMaterial* BuildPostMaterial(UPackage* Package, UMaterialParameterCollection* Look)
{
	UMaterial* Material = NewObject<UMaterial>(Package, FName(FPackageName::GetShortName(Package->GetName())),
		RF_Public | RF_Standalone | RF_Transactional);
	Material->MaterialDomain = MD_PostProcess;
	Material->BlendableLocation = BL_SceneColorBeforeDOF;
	Material->BlendablePriority = 10;

	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -400, 0);
	Custom->Description = TEXT("FTWorldWavePost");
	Custom->OutputType = CMOT_Float3;
	Custom->Code = FTWorldWavePostBody;
	Custom->IncludeFilePaths.Add(FTWorldWavePostInclude);
	Custom->Inputs.Reset();

	const auto AddScene = [Material, Custom](const TCHAR* Name, ESceneTextureId Id, int32 Y)
	{
		UMaterialExpressionSceneTexture* Node = AddExpression<UMaterialExpressionSceneTexture>(Material, -1300, Y);
		Node->SceneTextureId = Id;
		AddInput(Custom, Name, Node);
	};
	AddScene(TEXT("SceneColor"), PPI_PostProcessInput0, -800);
	AddScene(TEXT("SceneDepthIn"), PPI_SceneDepth, -700);
	AddScene(TEXT("SceneNormal"), PPI_WorldNormal, -650);
	AddInput(Custom, TEXT("WorldPos"), AddExpression<UMaterialExpressionWorldPosition>(Material, -1300, -600));
	AddInput(Custom, TEXT("CamPos"), AddExpression<UMaterialExpressionCameraPositionWS>(Material, -1300, -540));

	const FCollectionInput FromCollection[] = {
		{TEXT("WOrigin"), FTWorldWave::Origin},
		{TEXT("WOriginScreen"), FTWorldWave::OriginScreen},
		{TEXT("WFoot"), FTWorldWave::StreakFoot},
		{TEXT("WTip"), FTWorldWave::StreakTip},
		{TEXT("WRadius"), FTWorldWave::Radius},
		{TEXT("WBand"), FTWorldWave::BandWidth},
		{TEXT("WPrelude"), FTWorldWave::Prelude},
		{TEXT("WDirection"), FTWorldWave::Direction},
		{TEXT("WTime"), FTWorldWave::WaveTime},
		{TEXT("WGround"), FTWorldWave::Ground},
		{TEXT("WGain"), FTWorldWave::Gain},
		{TEXT("WKeep0"), FTWorldWave::Keep0},
	};
	int32 Row = 0;
	for (const FCollectionInput& In : FromCollection)
	{
		UMaterialExpressionCollectionParameter* Node = AddExpression<UMaterialExpressionCollectionParameter>(Material, -1300, -460 + 70 * Row++);
		SetCollectionParameter(Node, Look, In.Parameter);
		AddInput(Custom, In.Input, Node);
	}
	UMaterialExpressionScalarParameter* Sky = AddExpression<UMaterialExpressionScalarParameter>(Material, -1000, 400);
	Sky->ParameterName = FTStyle::SkyDepthName();
	Sky->DefaultValue = FTStyle::DefaultSkyDepth;
	Sky->Group = TEXT("Scene");
	Sky->UpdateParameterGuid(true, false);
	AddInput(Custom, TEXT("SkyDepth"), Sky);

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
	// The state too (a master built with it already has it: AddState leaves that one as it is).
	FTWorldWave::AddState(kTag, Twin, -600, 1300);
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

	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: world-wave cut wired in%s."), Tag, *Material->GetName(),
		bWasOpaque ? TEXT(" (opaque -> masked)") : TEXT(""));
	return true;
}

bool AddState(const TCHAR* Tag, UMaterial* Material, int32 X, int32 Y)
{
	if (!Material)
	{
		return false;
	}
	UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *ObjectPath(StateFunctionPath), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Function)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: no %s (run -run=FTBuildWaveMaterials first); %s takes no world-wave state."), Tag,
			StateFunctionPath, *Material->GetName());
		return false;
	}
	for (UMaterialExpression* Expression : Material->GetExpressionCollection().Expressions)
	{
		const UMaterialExpressionMaterialFunctionCall* Existing = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
		if (Existing && Existing->MaterialFunction && Existing->MaterialFunction->GetPathName() == Function->GetPathName())
		{
			return true;
		}
	}
	UMaterialExpressionMaterialFunctionCall* Call = AddExpression<UMaterialExpressionMaterialFunctionCall>(Material, X, Y);
	if (!Call->SetMaterialFunction(Function) || Call->FunctionInputs.Num() < 3 || Call->FunctionOutputs.Num() < 3)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s is not BaseColor, Roughness, Emissive in and out; %s takes no world-wave state."), Tag,
			StateFunctionPath, *Material->GetName());
		Material->GetExpressionCollection().RemoveExpression(Call);
		return false;
	}
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	// Each of the master's inputs goes through the call: what fed it feeds the call's input (an unconnected one keeps the
	// function's default, which is the master's own default too), and the call's output feeds it.
	const auto Route = [Call](FExpressionInput& MasterInput, int32 Index)
	{
		if (MasterInput.Expression)
		{
			Call->FunctionInputs[Index].Input.Connect(MasterInput.OutputIndex, MasterInput.Expression);
		}
		MasterInput.Connect(Index, Call);
	};
	Route(Inputs->BaseColor, 0);
	// A constant roughness the master set by hand: carry it in.
	if (!Inputs->Roughness.Expression && Inputs->Roughness.UseConstant)
	{
		UMaterialExpressionConstant* Rough = AddExpression<UMaterialExpressionConstant>(Material, X - 300, Y + 120);
		Rough->R = Inputs->Roughness.Constant;
		Call->FunctionInputs[1].Input.Connect(0, Rough);
		Inputs->Roughness.UseConstant = false;
	}
	Route(Inputs->Roughness, 1);
	if (Inputs->EmissiveColor.UseConstant && !Inputs->EmissiveColor.Expression)
	{
		Inputs->EmissiveColor.UseConstant = false;
	}
	Route(Inputs->EmissiveColor, 2);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: world-wave state wired in."), Tag, *Material->GetName());
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
	TArray<FString> Packages = {FTWorldWave::CutFunctionPath, FTWorldWave::StateFunctionPath, FTWorldWave::PostMaterialPath,
		FTWorldWave::OldInkMaterialPath};
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
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(WavePackageFile(FTWorldWave::PostMaterialPath)), true);

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
	UMaterialFunction* State = BuildStateFunction(CreatePackage(FTWorldWave::StateFunctionPath), Look);
	if (!Save(State))
	{
		return 1;
	}
	UMaterial* Post = BuildPostMaterial(CreatePackage(FTWorldWave::PostMaterialPath), Look);
	TArray<UMaterial*> Built = {Post};
	for (const TCHAR* Master : FTWorldWave::PropMasters)
	{
		// A missing twin is not fatal: the dressing leaves that master's props uncut (it finds no twin) and the rest build.
		UMaterial* Twin = BuildTwin(Master);
		if (!Twin)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: no twin for %s; its props stay uncut."), kTag, Master);
			continue;
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
		TEXT("%s: built MF_WorldWaveCut, MF_WorldWaveState, PP_WorldWave (%d expressions) and %d prop twins; the parameters are in "
			 "%s. Now rebuild the masters that wire them in: -run=FTBuildSurfaces -Master -Weather, -run=FTImportPlant -Masters, "
			 "-run=FTImportVehicle -Source=<the car's export>."),
		kTag, Post->GetExpressionCollection().Expressions.Num(), Built.Num() - 1, FTWorldWave::CollectionPath);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildWaveMaterials needs an editor build."));
	return 1;
#endif
}
