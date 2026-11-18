#include "FTBuildWaveMaterialsCommandlet.h"

#include "ForgottenThird.h"
#include "FTWorldWave.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "Misc/Parse.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FTImportMaterials.h"
#include "FTStyleParameters.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionStaticBool.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "FTWorldWaveShader.inl"
#endif

#if WITH_EDITOR
// Every helper lives in the file's namespace (RULE 09:42): a unity build that pairs this file with another builder
// must not see two anonymous AddExpression or AddInput.
namespace FTWorldWave
{
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
	Custom->RebuildOutputs();

	struct FOutputSpec
	{
		const TCHAR* Name;
		const TCHAR* Description;
		int32 CustomOutput;
	};
	const FOutputSpec Outputs[] = {
		{TEXT("Clip"), TEXT("1 keeps the pixel, 0 clips it: OpacityMask (or Opacity on a translucent master)."), 0},
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

	// Character (a static bool, off unless the master passes it on; AddState's bCharacter): the character state, which
	// drops the leaks and puddles and takes its grime from the pre-skinned local position (State.ush, FTWaveCharacter).
	UMaterialExpressionFunctionInput* CharacterIn = AddFunctionExpression<UMaterialExpressionFunctionInput>(Function, -1200, 60);
	CharacterIn->InputName = FName(TEXT("Character"));
	CharacterIn->InputType = FunctionInput_StaticBool;
	CharacterIn->PreviewValue = FVector4f(0.f, 0.f, 0.f, 0.f);
	CharacterIn->bUsePreviewValueAsDefault = true;
	CharacterIn->SortPriority = 3;
	CharacterIn->Id = FGuid::NewDeterministicGuid(TEXT("FTWorldWaveState.In.Character"));
	// A static bool input takes its default only from a connected Preview (PreviewValue is ignored for it: every master
	// that left Character unset failed to compile, "Missing Preview connection", and drew the default material).
	UMaterialExpressionStaticBool* CharacterOff = AddFunctionExpression<UMaterialExpressionStaticBool>(Function, -1450, 60);
	CharacterOff->Value = 0;
	CharacterIn->Preview.Connect(0, CharacterOff);

	UMaterialExpressionWorldPosition* WorldPos = AddFunctionExpression<UMaterialExpressionWorldPosition>(Function, -900, 100);
	UMaterialExpressionCustom* Custom = AddFunctionExpression<UMaterialExpressionCustom>(Function, -300, 0);
	UMaterialExpressionCustom* CharacterCustom = AddFunctionExpression<UMaterialExpressionCustom>(Function, -300, 500);
	Custom->Description = TEXT("FTWorldWaveState");
	Custom->Code = FTWorldWaveStateBody;
	CharacterCustom->Description = TEXT("FTWorldWaveState (character)");
	CharacterCustom->Code = FTWorldWaveCharacterBody;
	for (UMaterialExpressionCustom* Node : {Custom, CharacterCustom})
	{
		Node->OutputType = CMOT_Float3;
		Node->IncludeFilePaths.Add(FTWorldWaveStateInclude);
		Node->Inputs.Reset();
		AddInput(Node, TEXT("Base"), FunctionInputs[0]);
		AddInput(Node, TEXT("Rough"), FunctionInputs[1]);
		AddInput(Node, TEXT("Emis"), FunctionInputs[2]);
		AddInput(Node, TEXT("WorldPos"), WorldPos);
	}
	AddInput(Custom, TEXT("Normal"), AddFunctionExpression<UMaterialExpressionVertexNormalWS>(Function, -900, 160));
	// The pre-skinned position is vertex-shader only: it reaches the pixel shader through an interpolator.
	UMaterialExpressionLocalPosition* PreSkinned = AddFunctionExpression<UMaterialExpressionLocalPosition>(Function, -1100, 560);
	PreSkinned->LocalOrigin = ELocalPositionOrigin::InstancePreSkinning;
	PreSkinned->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;
	UMaterialExpressionVertexInterpolator* ToPixel = AddFunctionExpression<UMaterialExpressionVertexInterpolator>(Function, -900, 560);
	ToPixel->Input.Connect(0, PreSkinned);
	AddInput(CharacterCustom, TEXT("LocalPos"), ToPixel);
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
		AddInput(CharacterCustom, In.Input, Node);
	}
	for (UMaterialExpressionCustom* Node : {Custom, CharacterCustom})
	{
		AddOutput(Node, TEXT("RoughOut"), CMOT_Float1);
		AddOutput(Node, TEXT("EmisOut"), CMOT_Float3);
		Node->RebuildOutputs();
	}

	const TCHAR* const OutputNames[] = {TEXT("BaseColor"), TEXT("Roughness"), TEXT("Emissive")};
	for (int32 I = 0; I < 3; ++I)
	{
		UMaterialExpressionStaticSwitch* Switch = AddFunctionExpression<UMaterialExpressionStaticSwitch>(Function, -50, -100 + 160 * I);
		Switch->A.Connect(I, CharacterCustom);
		Switch->B.Connect(I, Custom);
		Switch->Value.Connect(0, CharacterIn);
		UMaterialExpressionFunctionOutput* Out = AddFunctionExpression<UMaterialExpressionFunctionOutput>(Function, 100, -100 + 120 * I);
		Out->OutputName = FName(OutputNames[I]);
		Out->SortPriority = I;
		Out->Id = FGuid::NewDeterministicGuid(FString(TEXT("FTWorldWaveState.Out.")) + OutputNames[I]);
		Out->A.Connect(0, Switch);
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
	// The catch-all's tag (FTWorldWave::kCatchStencil) and the tagged piece's own depth, to know it is the one seen.
	AddScene(TEXT("CustomDepthIn"), PPI_CustomDepth, -900);
	AddScene(TEXT("CustomStencilIn"), PPI_CustomStencil, -1000);
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
	Package->MarkAsFullyLoaded(); // built here and saved over the old file (Main's note)
	UMaterial* Twin = DuplicateObject<UMaterial>(Master, Package, FName(FPackageName::GetShortName(Path)));
	Twin->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	Twin->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Twin->SetUsageByFlag(MATUSAGE_Nanite, true);
	if (!FTWorldWave::AddCut(kTag, Twin, -600, 900))
	{
		return nullptr;
	}
	// The state too (a master built with it already has it: AddState leaves that one as it is).
	// A character master's twin takes the character state (no leaks or puddles on a body; the thin line at the cut).
	const bool bCharacter = FString(MasterPath).StartsWith(TEXT("/Game/Characters/"));
	FTWorldWave::AddState(kTag, Twin, -600, 1300, bCharacter);
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
	if (!Call->SetMaterialFunction(Function) || Call->FunctionOutputs.Num() < 1)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s gives %d outputs, not Clip; %s takes no world-wave cut."), Tag,
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

bool AddState(const TCHAR* Tag, UMaterial* Material, int32 X, int32 Y, bool bCharacter)
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
	// The call's Character input always takes an explicit static bool (besides the function's own false Preview): true for
	// the character state (State.ush, FTWaveCharacter), false for every other master. A false one already there is
	// switched on for a character; a true one is never switched off here.
	const auto SetCharacter = [Material, X, Y, bCharacter, Tag](UMaterialExpressionMaterialFunctionCall* Call)
	{
		for (FFunctionExpressionInput& In : Call->FunctionInputs)
		{
			if (In.ExpressionInput && In.ExpressionInput->InputName == FName(TEXT("Character")))
			{
				if (!In.Input.Expression)
				{
					UMaterialExpressionStaticBool* Flag = AddExpression<UMaterialExpressionStaticBool>(Material, X - 300, Y + 240);
					Flag->Value = bCharacter ? 1 : 0;
					In.Input.Connect(0, Flag);
				}
				else if (UMaterialExpressionStaticBool* Flag = Cast<UMaterialExpressionStaticBool>(In.Input.Expression); Flag && bCharacter)
				{
					Flag->Value = 1;
				}
				return;
			}
		}
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: %s has no Character input (rebuild it with -run=FTBuildWaveMaterials); %s takes the surface state."), Tag,
			StateFunctionPath, *Material->GetName());
	};
	// The runtime's marker (FTWorldWave::AwareParameter): a scalar parameter whose default the audit reads from the
	// material's cached parameters: 1 with the character state, 0 without. Like the flag, one already there is switched on
	// for a character and never off here. So that no cook can strip it as unreferenced, it is wired into the main call's
	// emissive input as (what fed it) + marker * 0, which cannot change the output; a scalar 0 stands in for an unconnected
	// input (the function's emissive default is black too). Done once: an input already fed that way is left alone.
	const auto SetMarker = [Material, X, Y, bCharacter](UMaterialExpressionMaterialFunctionCall* MainCall)
	{
		const FName Name(AwareParameter);
		UMaterialExpressionScalarParameter* Marker = nullptr;
		for (UMaterialExpression* Expression : Material->GetExpressionCollection().Expressions)
		{
			if (UMaterialExpressionScalarParameter* Found = Cast<UMaterialExpressionScalarParameter>(Expression); Found && Found->ParameterName == Name)
			{
				Marker = Found;
				break;
			}
		}
		if (Marker)
		{
			if (bCharacter)
			{
				Marker->DefaultValue = 1.f;
			}
		}
		else
		{
			Marker = AddExpression<UMaterialExpressionScalarParameter>(Material, X - 300, Y + 360);
			Marker->ParameterName = Name;
			Marker->DefaultValue = bCharacter ? 1.f : 0.f;
			Marker->Group = FName(TEXT("WorldWave"));
			Marker->Desc = TEXT("The world wave's runtime marker: 1 when this master takes the character state. Read by the audit; "
								"added times zero into the state's emissive input so it is kept.");
		}
		FExpressionInput& Emis = MainCall->FunctionInputs[2].Input;
		if (const UMaterialExpressionAdd* Sum = Cast<UMaterialExpressionAdd>(Emis.Expression))
		{
			const UMaterialExpressionMultiply* Times = Cast<UMaterialExpressionMultiply>(Sum->B.Expression);
			if (Times && Times->A.Expression == Marker)
			{
				return;
			}
		}
		UMaterialExpressionMultiply* Zeroed = AddExpression<UMaterialExpressionMultiply>(Material, X - 150, Y + 360);
		Zeroed->A.Connect(0, Marker);
		Zeroed->ConstB = 0.f;
		UMaterialExpressionAdd* Kept = AddExpression<UMaterialExpressionAdd>(Material, X - 60, Y + 300);
		if (Emis.Expression)
		{
			Kept->A.Connect(Emis.OutputIndex, Emis.Expression);
		}
		else
		{
			Kept->ConstA = 0.f;
		}
		Kept->B.Connect(0, Zeroed);
		Emis.Connect(0, Kept);
	};
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	const auto IsState = [Function](const UMaterialExpression* Expression)
	{
		const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
		return Call && Call->MaterialFunction && Call->MaterialFunction->GetPathName() == Function->GetPathName();
	};
	// The cloth model's fuzz colour (the SubsurfaceColor pin) goes through a second call of the state, as its BaseColor, so
	// the sheen greys with the cloth under it instead of keeping the first world's colour. Its other inputs keep the
	// function's defaults and its other outputs are unused. One already there just takes the flag.
	// The field is current on a fresh master too: UMaterial::SetShadingModel (Material.cpp, UE 5.8) sets ShadingModels
	// from the new model at once, not at PostEditChange, and the private ShadingModel has no getter to test besides it.
	const auto RouteFuzz = [&]()
	{
		if (!Material->GetShadingModels().HasShadingModel(MSM_Cloth) || !Inputs->SubsurfaceColor.Expression)
		{
			return;
		}
		if (IsState(Inputs->SubsurfaceColor.Expression))
		{
			UMaterialExpressionMaterialFunctionCall* Fuzz = Cast<UMaterialExpressionMaterialFunctionCall>(Inputs->SubsurfaceColor.Expression);
			Fuzz->UpdateFromFunctionResource();
			SetCharacter(Fuzz);
			return;
		}
		UMaterialExpressionMaterialFunctionCall* Fuzz = AddExpression<UMaterialExpressionMaterialFunctionCall>(Material, X, Y + 500);
		if (!Fuzz->SetMaterialFunction(Function) || Fuzz->FunctionInputs.Num() < 3 || Fuzz->FunctionOutputs.Num() < 3)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: %s's fuzz colour keeps the first world (the state's inputs are not as expected)."), Tag,
				*Material->GetName());
			Material->GetExpressionCollection().RemoveExpression(Fuzz);
			return;
		}
		Fuzz->FunctionInputs[0].Input.Connect(Inputs->SubsurfaceColor.OutputIndex, Inputs->SubsurfaceColor.Expression);
		Inputs->SubsurfaceColor.Connect(0, Fuzz);
		SetCharacter(Fuzz);
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: the cloth's fuzz colour goes through the world-wave state too."), Tag, *Material->GetName());
	};
	UMaterialExpressionMaterialFunctionCall* Existing = nullptr;
	for (UMaterialExpression* Expression : Material->GetExpressionCollection().Expressions)
	{
		// The main call is the one BaseColor's chain holds, not the fuzz's.
		if (IsState(Expression) && Expression != Inputs->SubsurfaceColor.Expression)
		{
			Existing = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			break;
		}
	}
	if (Existing)
	{
		// A call saved before the function gained an input lists the old inputs until it is refreshed.
		Existing->UpdateFromFunctionResource();
		SetCharacter(Existing);
		RouteFuzz();
		SetMarker(Existing);
		return true;
	}
	UMaterialExpressionMaterialFunctionCall* Call = AddExpression<UMaterialExpressionMaterialFunctionCall>(Material, X, Y);
	if (!Call->SetMaterialFunction(Function) || Call->FunctionInputs.Num() < 3 || Call->FunctionOutputs.Num() < 3)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s is not BaseColor, Roughness, Emissive in and out; %s takes no world-wave state."), Tag,
			StateFunctionPath, *Material->GetName());
		Material->GetExpressionCollection().RemoveExpression(Call);
		return false;
	}
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
	SetCharacter(Call);
	RouteFuzz();
	SetMarker(Call);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: world-wave state wired in%s."), Tag, *Material->GetName(),
		bCharacter ? TEXT(" (character)") : TEXT(""));
	return true;
}

bool CharacterOn(const UMaterial& Material)
{
	const FString State = ObjectPath(StateFunctionPath);
	for (const UMaterialExpression* Expression : Material.GetExpressionCollection().Expressions)
	{
		const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
		if (!Call || !Call->MaterialFunction || Call->MaterialFunction->GetPathName() != State)
		{
			continue;
		}
		for (const FFunctionExpressionInput& In : Call->FunctionInputs)
		{
			if (In.ExpressionInput && In.ExpressionInput->InputName == FName(TEXT("Character")) && In.Input.Expression)
			{
				// Only an explicit static bool is read; anything else there (a static bool parameter, say) is unknown, so
				// it counts as off and is logged rather than guessed.
				if (const UMaterialExpressionStaticBool* Flag = Cast<UMaterialExpressionStaticBool>(In.Input.Expression))
				{
					return Flag->Value != 0;
				}
				UE_LOG(LogForgottenThird, Warning, TEXT("FTWorldWave: %s: the state's Character pin takes a %s, not a static bool; read as off."),
					*Material.GetPathName(), *In.Input.Expression->GetClass()->GetName());
				return false;
			}
		}
	}
	return false;
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
	// Every wave asset is built new in memory and saved over its file; no file is deleted before its replacement is saved,
	// so a run that fails part way leaves the old asset on disk, never a missing one. The look's collection is never
	// rebuilt; the wave's parameters are added to it in place.
	TArray<FString> Packages = {FTWorldWave::CutFunctionPath, FTWorldWave::StateFunctionPath, FTWorldWave::PostMaterialPath};
	for (const TCHAR* Master : FTWorldWave::PropMasters)
	{
		Packages.Add(FTWorldWave::TwinPackage(Master));
	}
	// Some may be loaded already: once the character masters wear the state, a lead mesh a constructor finds at start-up
	// pulls MF_WorldWaveState in, and its loader holds the file open, so the save couldn't move the new file over it.
	// Every loaded one lets go of its file first. The asset itself is rebuilt in place below (a new object over the same
	// name keeps its address, so the loaded masters' calls see the rebuilt one).
	FlushAsyncLoading();
	TArray<UObject*> Loaded;
	for (const FString& Package : Packages)
	{
		if (UPackage* Found = FindPackage(nullptr, *Package))
		{
			Loaded.Add(Found);
			UE_LOG(LogForgottenThird, Display, TEXT("%s: %s was loaded at start; its file is let go and the asset rebuilt in place."), FTWorldWave::kTag,
				*Package);
		}
	}
	if (Loaded.Num() > 0)
	{
		ResetLoaders(Loaded);
	}
	// A package made here counts as fully loaded, so a master that imports it later takes the new asset in memory rather
	// than loading the old file into it.
	const auto WavePackage = [](const TCHAR* Path)
	{
		UPackage* Package = CreatePackage(Path);
		Package->MarkAsFullyLoaded();
		return Package;
	};
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FTWorldWave::WavePackageFile(FTWorldWave::PostMaterialPath)), true);

	// Everything is built in memory first, then checked, then saved in one loop: a compile error saves nothing, not even the
	// functions or the collection, so the files on disk stay one matching set.
	UMaterialParameterCollection* Look = FTWorldWave::Load();
	if (!Look)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s is missing (build it with -run=FTBuildLookMaterials)."), FTWorldWave::kTag,
			FTWorldWave::CollectionPath);
		return 1;
	}
	const bool bLookChanged = FTWorldWave::AddParametersTo(*Look) > 0;
	if (bLookChanged)
	{
		Look->PostEditChange();
	}

	UMaterialFunction* Cut = FTWorldWave::BuildCutFunction(WavePackage(FTWorldWave::CutFunctionPath), Look);
	UMaterialFunction* State = FTWorldWave::BuildStateFunction(WavePackage(FTWorldWave::StateFunctionPath), Look);
	UMaterial* Post = FTWorldWave::BuildPostMaterial(WavePackage(FTWorldWave::PostMaterialPath), Look);
	TArray<UMaterial*> Built = {Post};
	for (const TCHAR* Master : FTWorldWave::PropMasters)
	{
		// A missing twin is not fatal: the dressing leaves that master's props uncut (it finds no twin) and the rest build.
		UMaterial* Twin = FTWorldWave::BuildTwin(Master);
		if (!Twin)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: no twin for %s; its props stay uncut."), FTWorldWave::kTag, Master);
			continue;
		}
		Built.Add(Twin);
	}

	// Under -NullRHI there may be nothing to compile; with a renderer, let the shaders finish first.
	FAssetCompilingManager::Get().FinishAllCompilation();
	int32 Errors = 0;
	for (UMaterial* Material : Built)
	{
		Errors += FT::CompileErrors(FTWorldWave::kTag, Material);
	}
	// A compile error saves nothing: a broken material must never overwrite the good file on disk.
	if (Errors > 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %d compile errors; nothing saved, the files on disk are as they were."), FTWorldWave::kTag, Errors);
		return 1;
	}
	TArray<UObject*> ToSave = {Cut, State};
	if (bLookChanged)
	{
		ToSave.Insert(Look, 0);
	}
	ToSave.Append(Built);
	bool bOk = true;
	for (UObject* Asset : ToSave)
	{
		bOk &= FTWorldWave::Save(Asset);
	}
	if (!bOk)
	{
		return 1;
	}
	// The retired ink material has no replacement; it goes only once everything above is saved.
	const FString OldInk = FTWorldWave::WavePackageFile(FTWorldWave::OldInkMaterialPath);
	if (IFileManager::Get().FileExists(*OldInk) && !IFileManager::Get().Delete(*OldInk, false, true))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: couldn't remove the retired %s; nothing uses it."), FTWorldWave::kTag, *OldInk);
	}
	// -Flags: the surface masters already on disk take an explicit static false on their state call's Character input in
	// place (what SetCharacter now wires on every build), so none leans on the function's Preview alone.
	if (FParse::Param(*Params, TEXT("Flags")))
	{
		const TCHAR* const Surfaces[] = {TEXT("/Game/Rendering/M_WorldGround"), TEXT("/Game/Rendering/M_WorldSurface"),
			TEXT("/Game/Rendering/M_WorldSurfaceTex"), TEXT("/Game/Rendering/Windows/M_WorldSurfaceWin"), TEXT("/Game/Rendering/Foliage/M_FTFoliage"),
			TEXT("/Game/Rendering/Foliage/M_FTBark")};
		TArray<UMaterial*> Flagged;
		for (const TCHAR* Path : Surfaces)
		{
			UMaterial* Material = LoadObject<UMaterial>(nullptr, *FTWorldWave::ObjectPath(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Material)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("%s: -Flags: no %s here; skipped."), FTWorldWave::kTag, Path);
				continue;
			}
			const int32 Before = Material->GetExpressionCollection().Expressions.Num();
			if (!FTWorldWave::AddState(FTWorldWave::kTag, Material, 300, -600, false))
			{
				return 1;
			}
			if (Material->GetExpressionCollection().Expressions.Num() == Before)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("%s: -Flags: %s already has its flag."), FTWorldWave::kTag, Path);
				continue;
			}
			Material->PreEditChange(nullptr);
			Material->PostEditChange();
			Flagged.Add(Material);
		}
		FAssetCompilingManager::Get().FinishAllCompilation();
		// All checked before any is saved, as above.
		int32 FlagErrors = 0;
		for (UMaterial* Material : Flagged)
		{
			FlagErrors += FT::CompileErrors(FTWorldWave::kTag, Material);
		}
		if (FlagErrors > 0)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: -Flags: %d compile errors; no surface master saved."), FTWorldWave::kTag, FlagErrors);
			return 1;
		}
		for (UMaterial* Material : Flagged)
		{
			if (!FTWorldWave::Save(Material))
			{
				return 1;
			}
			UE_LOG(LogForgottenThird, Display, TEXT("%s: -Flags: %s takes an explicit Character false."), FTWorldWave::kTag, *Material->GetPathName());
		}
	}
	// -Worn: the worn masters already on disk take the character state in place (what their builders now wire in, without
	// rebuilding them or touching their instances); -Worn=<path;path> adds more. A master that already has it is not saved.
	FString WornExtra;
	if (FParse::Param(*Params, TEXT("Worn")) || FParse::Value(*Params, TEXT("Worn="), WornExtra))
	{
		TArray<FString> Worn = {TEXT("/Game/Characters/Shared/Materials/M_FTCharacter"), TEXT("/Game/Characters/Shared/Materials/M_FTCharacterTex"),
			TEXT("/Game/Characters/Shared/Materials/M_FTHair"), TEXT("/Game/Characters/Shared/Materials/M_FTSkin"),
			TEXT("/Game/Characters/Shared/Materials/Garment/M_FTGarment"), TEXT("/Game/Characters/Shared/Materials/Garment/M_FTGarmentCloth"),
			TEXT("/Game/Characters/Shared/Materials/Garment/M_FTGarmentClothMasked")};
		TArray<FString> More;
		WornExtra.ParseIntoArray(More, TEXT(";"), true);
		Worn.Append(More);
		using FTWorldWave::CharacterOn;
		TArray<UMaterial*> Changed;
		for (const FString& Path : Worn)
		{
			UMaterial* Material = LoadObject<UMaterial>(nullptr, *FTWorldWave::ObjectPath(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Material)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("%s: -Worn: no %s here; skipped."), FTWorldWave::kTag, *Path);
				continue;
			}
			const bool bWasOn = CharacterOn(*Material);
			// The runtime's marker as the audit reads it, before the edit (a marker at 0 is switched on without adding one).
			const bool bWasAware = FTWorldWave::WaveAware(*Material);
			const int32 Before = Material->GetExpressionCollection().Expressions.Num();
			if (!FTWorldWave::AddState(FTWorldWave::kTag, Material, 300, 1300, true))
			{
				return 1;
			}
			// Unchanged only when the flag and the marker were on and nothing was added (a cloth master's fuzz call is one).
			if (bWasOn && bWasAware && CharacterOn(*Material) && Material->GetExpressionCollection().Expressions.Num() == Before)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("%s: -Worn: %s already takes the character state."), FTWorldWave::kTag, *Path);
				continue;
			}
			Material->PreEditChange(nullptr);
			Material->PostEditChange();
			Changed.Add(Material);
		}
		FAssetCompilingManager::Get().FinishAllCompilation();
		int32 WornErrors = 0;
		for (UMaterial* Material : Changed)
		{
			WornErrors += FT::CompileErrors(FTWorldWave::kTag, Material);
		}
		if (WornErrors > 0)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: -Worn: %d compile errors; no worn master saved."), FTWorldWave::kTag, WornErrors);
			return 1;
		}
		for (UMaterial* Material : Changed)
		{
			if (!FTWorldWave::Save(Material))
			{
				return 1;
			}
			UE_LOG(LogForgottenThird, Display, TEXT("%s: -Worn: %s takes the character state."), FTWorldWave::kTag, *Material->GetPathName());
		}
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("%s: built MF_WorldWaveCut, MF_WorldWaveState, PP_WorldWave (%d expressions) and %d prop twins; the parameters are in "
			 "%s. Now rebuild the masters that wire them in: -run=FTBuildSurfaces -Master -Weather, -run=FTImportPlant -Masters, "
			 "-run=FTImportVehicle -Source=<the car's export>."),
		FTWorldWave::kTag, Post->GetExpressionCollection().Expressions.Num(), Built.Num() - 1, FTWorldWave::CollectionPath);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildWaveMaterials needs an editor build."));
	return 1;
#endif
}
