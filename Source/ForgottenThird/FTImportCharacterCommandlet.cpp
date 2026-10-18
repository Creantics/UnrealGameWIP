#include "FTImportCharacterCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "FTText.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ftcore/Face.h"
#include "ftcore/FaceImport.h"

namespace
{
const TCHAR* const kFaceBones[] = {
	TEXT("head"), TEXT("jaw"), TEXT("tongue_01"), TEXT("tongue_02"), TEXT("tongue_03"), TEXT("eye_l"), TEXT("eye_r"),
};

// Interchange gives each glTF material an instance of a plugin master without the skeletal mesh and morph target
// usage. The editor adds them in memory, but a game run draws the default material instead. Plugin content can't be
// saved, and characters use flat glTF factors only, so each instance is moved onto a small project master that takes
// the same values and has both usages.
const TCHAR* const kCharacterMaster = TEXT("/Game/Characters/Shared/Materials/M_FTCharacter");

template <typename T>
T* AddParameter(UMaterial* Material, FName Name, int32 Y)
{
	T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = -400;
	Expression->MaterialExpressionEditorY = Y;
	Expression->ParameterName = Name;
	Material->GetExpressionCollection().AddExpression(Expression);
	return Expression;
}

UMaterial* CharacterMaster(TArray<UPackage*>& Dirty)
{
	const FString Name = FPackageName::GetShortName(kCharacterMaster);
	if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, *(FString(kCharacterMaster) + TEXT(".") + Name), nullptr,
			LOAD_NoWarn | LOAD_Quiet))
	{
		return Existing;
	}
	UPackage* Package = CreatePackage(kCharacterMaster);
	UMaterial* Material = NewObject<UMaterial>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	Material->bUsedWithSkeletalMesh = true;
	Material->bUsedWithMorphTargets = true;
	Material->TwoSided = true;

	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	UMaterialExpressionVectorParameter* Color = AddParameter<UMaterialExpressionVectorParameter>(Material, TEXT("BaseColor"), 0);
	Color->DefaultValue = FLinearColor::White;
	Inputs->BaseColor.Expression = Color;
	UMaterialExpressionScalarParameter* Metallic = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Metallic"), 200);
	Metallic->DefaultValue = 0.0f;
	Inputs->Metallic.Expression = Metallic;
	UMaterialExpressionScalarParameter* Roughness = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Roughness"), 300);
	Roughness->DefaultValue = 0.6f;
	Inputs->Roughness.Expression = Roughness;
	UMaterialExpressionVectorParameter* Emissive = AddParameter<UMaterialExpressionVectorParameter>(Material, TEXT("Emissive"), 400);
	Emissive->DefaultValue = FLinearColor::Black;
	UMaterialExpressionScalarParameter* Strength = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("EmissiveStrength"), 600);
	Strength->DefaultValue = 1.0f;
	UMaterialExpressionMultiply* Glow = NewObject<UMaterialExpressionMultiply>(Material, NAME_None, RF_Transactional);
	Glow->Material = Material;
	Glow->MaterialExpressionEditorX = -200;
	Glow->MaterialExpressionEditorY = 450;
	Glow->A.Expression = Emissive;
	Glow->B.Expression = Strength;
	Material->GetExpressionCollection().AddExpression(Glow);
	Inputs->EmissiveColor.Expression = Glow;

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	FAssetRegistryModule::AssetCreated(Material);
	Material->MarkPackageDirty();
	Dirty.AddUnique(Package);
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: made %s."), *Material->GetPathName());
	return Material;
}

void UseCharacterMaster(const USkeletalMesh& Mesh)
{
	TArray<UPackage*> Dirty;
	UMaterial* Master = nullptr;
	int32 Moved = 0;
	for (const FSkeletalMaterial& Slot : Mesh.GetMaterials())
	{
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Slot.MaterialInterface);
		if (!Instance || !Instance->GetPathName().StartsWith(TEXT("/Game/")))
		{
			continue;
		}
		if (!Master)
		{
			Master = CharacterMaster(Dirty);
		}
		if (Instance->Parent == Master)
		{
			continue;
		}
		FLinearColor Color = FLinearColor::White, Emissive = FLinearColor::Black;
		float Metallic = 0.0f, Roughness = 0.6f, Strength = 1.0f;
		Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColorFactor")), Color);
		Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("EmissiveFactor")), Emissive);
		Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("MetallicFactor")), Metallic);
		Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("RoughnessFactor")), Roughness);
		Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("EmissiveStrength")), Strength);

		Instance->ClearParameterValuesEditorOnly();
		Instance->SetParentEditorOnly(Master);
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BaseColor")), Color);
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Emissive")), Emissive);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Metallic")), Metallic);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), Roughness);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("EmissiveStrength")), Strength);
		Instance->PostEditChange();
		Instance->MarkPackageDirty();
		Dirty.AddUnique(Instance->GetOutermost());
		++Moved;
	}
	if (!Dirty.IsEmpty())
	{
		UEditorLoadingAndSavingUtils::SavePackages(Dirty, false);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: moved %d materials onto %s."), Moved, kCharacterMaster);
}

// Logs the rig check; false when a face bone or (with bCheckFace) a face shape's morph is missing.
bool CheckRig(const USkeletalMesh& Mesh, bool bCheckFace)
{
	bool bOk = true;

	TSet<FName> Morphs;
	for (const UMorphTarget* Morph : Mesh.GetMorphTargets())
	{
		if (Morph)
		{
			Morphs.Add(Morph->GetFName());
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %s has %d morph targets."), *Mesh.GetPathName(), Morphs.Num());

	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	int32 BonesFound = 0;
	for (const TCHAR* Bone : kFaceBones)
	{
		if (Skeleton.FindBoneIndex(Bone) != INDEX_NONE)
		{
			++BonesFound;
		}
		else
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: face bone '%s' is missing."), Bone);
			bOk = false;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %d/%d face bones, %d bones in all."), BonesFound,
		UE_ARRAY_COUNT(kFaceBones), Skeleton.GetNum());

	if (!bCheckFace)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: -Face=0, skipping the face shape check."));
		return bOk;
	}

	const FString PresetsPath = FPaths::ProjectDir() / TEXT("Data/Rig/face_presets.json");
	TArray<uint8> Bytes;
	ftcore::FaceShapeSet Shapes;
	std::string Error;
	if (!FFileHelper::LoadFileToArray(Bytes, *PresetsPath) || !ftcore::importFaceShapeSet(FT::BytesAsText(Bytes), Shapes, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: couldn't load %s: %s"), *PresetsPath, *FT::ToFString(Error));
		return false;
	}
	int32 Bound = 0;
	for (const std::string& Shape : Shapes.shapes)
	{
		const FName Name(FT::ToFString(Shape));
		if (Morphs.Contains(Name))
		{
			++Bound;
		}
		else
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: no morph target for face shape '%s'."), *Name.ToString());
			bOk = false;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %d/%d face shapes bound."), Bound, static_cast<int32>(Shapes.shapes.size()));
	return bOk;
}
} // namespace
#endif

UFTImportCharacterCommandlet::UFTImportCharacterCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTImportCharacterCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString Source, Dest = TEXT("/Game/Characters/ProofHead");
	if (!FParse::Value(*Params, TEXT("Source="), Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: pass -Source=<file>."));
		return 1;
	}
	FParse::Value(*Params, TEXT("Dest="), Dest);
	bool bCheckFace = true;
	FParse::Bool(*Params, TEXT("Face="), bCheckFace);
	if (FPaths::IsRelative(Source))
	{
		Source = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Source);
	}
	if (!FPaths::FileExists(Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: %s doesn't exist."), *Source);
		return 1;
	}

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Source;
	Task->DestinationPath = Dest;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bSave = true;
	Task->bAsync = false;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.ImportAssetTasks({ Task });

	const USkeletalMesh* Mesh = nullptr;
	for (const UObject* Object : Task->GetObjects())
	{
		if (!Object)
		{
			continue;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: imported %s (%s)"), *Object->GetPathName(),
			*Object->GetClass()->GetName());
		if (!Mesh)
		{
			Mesh = Cast<USkeletalMesh>(Object);
		}
	}
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: no skeletal mesh came out of %s."), *Source);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: mesh is %s"), *Mesh->GetPathName());
	UseCharacterMaster(*Mesh);
	return CheckRig(*Mesh, bCheckFace) ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter needs an editor build."));
	return 1;
#endif
}
