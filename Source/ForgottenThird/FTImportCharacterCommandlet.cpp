#include "FTImportCharacterCommandlet.h"

#include "ForgottenThird.h"
#include "FTImportMaterials.h"

#if WITH_EDITOR
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "FTText.h"
#include "IAssetTools.h"
#include "InterchangeManager.h"
#include "InterchangePipelineBase.h"
#include "InterchangeProjectSettings.h"
#include "InterchangeSourceData.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"
#include "ftcore/Face.h"
#include "ftcore/FaceImport.h"
#include "ftcore/Glb.h"

#include <string>
#include <string_view>
#include <vector>

namespace
{
const TCHAR* const kFaceBones[] = {
	TEXT("head"), TEXT("jaw"), TEXT("tongue_01"), TEXT("tongue_02"), TEXT("tongue_03"), TEXT("eye_l"), TEXT("eye_r"),
};

// Logs the rig check; with bCheckFace, false when a face bone or a face shape's morph is missing.
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
	// Each morph at weight 1: how many LOD 0 vertices it moves, the largest move and the mean move (cm), so a morph that
	// imports empty or flat shows here. The skeleton's curve of that name must be flagged as a morph curve to drive it.
	const USkeleton* Skel = Mesh.GetSkeleton();
	for (const UMorphTarget* Morph : Mesh.GetMorphTargets())
	{
		if (!Morph)
		{
			continue;
		}
		const TConstArrayView<FMorphTargetDelta> Deltas = Morph->GetMorphTargetDeltas(0);
		double Largest = 0.0;
		FVector Mean = FVector::ZeroVector;
		int32 Moved = 0;
		for (const FMorphTargetDelta& Delta : Deltas)
		{
			const FVector Move(Delta.PositionDelta);
			if (Move.SizeSquared() > 1e-6)
			{
				++Moved;
				Mean += Move;
				Largest = FMath::Max(Largest, Move.Size());
			}
		}
		Mean = Moved > 0 ? Mean / Moved : Mean;
		const FCurveMetaData* Meta = Skel ? Skel->GetCurveMetaData(Morph->GetFName()) : nullptr;
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: morph %s moves %d vertices at weight 1, largest %.1f cm, mean move %s cm; skeleton curve %s."),
			*Morph->GetName(), Moved, Largest, *Mean.ToString(),
			Meta ? (Meta->Type.bMorphtarget ? TEXT("flagged as a morph curve") : TEXT("present, not flagged as a morph curve")) : TEXT("absent"));
	}

	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	int32 BonesFound = 0;
	for (const TCHAR* Bone : kFaceBones)
	{
		if (Skeleton.FindBoneIndex(Bone) != INDEX_NONE)
		{
			++BonesFound;
		}
		else if (bCheckFace)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: face bone '%s' is missing."), Bone);
			bOk = false;
		}
		else
		{
			// A creature with no human head (a boss) imports with -Face=0.
			UE_LOG(LogForgottenThird, Warning, TEXT("FTImportCharacter: face bone '%s' is missing."), Bone);
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

// -AnimSampleRate: false (and logs each clip) when a clip's length is not a whole number of frames at Rate.
bool ClipsOnFrames(const FString& Source, int32 Rate)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: couldn't read %s."), *Source);
		return false;
	}
	ftcore::GlbInfo Info;
	std::string Error;
	if (!ftcore::readGlb(std::string_view(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num()), Info, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: %s: %s"), *Source, *FT::ToFString(Error));
		return false;
	}
	const std::vector<ftcore::AssetIssue> Issues = ftcore::checkClipFrames(Info, Rate);
	for (const ftcore::AssetIssue& Issue : Issues)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: -AnimSampleRate=%d: %s; nothing imported."), Rate,
			*FT::ToFString(Issue.message));
	}
	if (Issues.empty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %d clips, each a whole number of frames at %d Hz."),
			static_cast<int32>(Info.animations.size()), Rate);
	}
	return Issues.empty();
}

// The pipeline holding the animation settings: the pipeline itself, or its AnimationPipeline subobject
// (the generic assets pipeline). Read by name so the module needs no Interchange plugin dependency.
UObject* AnimationPipelineOf(UObject& Pipeline)
{
	if (FindFProperty<FIntProperty>(Pipeline.GetClass(), TEXT("CustomBoneAnimationSampleRate")))
	{
		return &Pipeline;
	}
	const FObjectPropertyBase* Property =
		FindFProperty<FObjectPropertyBase>(Pipeline.GetClass(), TEXT("AnimationPipeline"));
	return Property ? Property->GetObjectPropertyValue_InContainer(&Pipeline) : nullptr;
}

// The default stack for Source, duplicated, with the one animation pipeline's rate set to Rate, as a task
// override; null (logged) when the stack can't take the rate as its only change. Rooted collects every
// object added to the root set, for the caller to release after the import.
UInterchangePipelineStackOverride* RateOverride(const FString& Source, int32 Rate, TArray<UObject*>& Rooted)
{
	const UInterchangeSourceData* SourceData = UInterchangeManager::CreateSourceData(Source);
	const TArray<UInterchangePipelineBase*> Stack =
		UInterchangeProjectSettingsScript::GetPipelineStackFromSourceData(/*bIsSceneImport*/ false, SourceData);
	if (Stack.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: no default pipeline stack for %s."), *Source);
		return nullptr;
	}
	int32 RatesSet = 0;
	for (UInterchangePipelineBase* Pipeline : Stack)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: stack pipeline %s (%s)"),
			*Pipeline->OriginalPipelinePath.ToString(), *Pipeline->GetClass()->GetName());
		UObject* Animation = AnimationPipelineOf(*Pipeline);
		if (!Animation)
		{
			continue;
		}
		UClass* Class = Animation->GetClass();
		const FIntProperty* RateField = FindFProperty<FIntProperty>(Class, TEXT("CustomBoneAnimationSampleRate"));
		const FBoolProperty* Fixed30 = FindFProperty<FBoolProperty>(Class, TEXT("bUse30HzToBakeBoneAnimation"));
		const FEnumProperty* Alignment = FindFProperty<FEnumProperty>(Class, TEXT("FrameAlignment"));
		if (!RateField || !Fixed30 || !Alignment)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: %s lacks a rate, 30 Hz or alignment field."),
				*Class->GetName());
			return nullptr;
		}
		if (Fixed30->GetPropertyValue_InContainer(Animation))
		{
			UE_LOG(LogForgottenThird, Error,
				TEXT("FTImportCharacter: the default stack bakes at a fixed 30 Hz; the rate can't apply."));
			return nullptr;
		}
		const int64 Align = Alignment->GetUnderlyingProperty()->GetSignedIntPropertyValue(
			Alignment->ContainerPtrToValuePtr<void>(Animation));
		if (Align != 0)
		{
			UE_LOG(LogForgottenThird, Error,
				TEXT("FTImportCharacter: the default stack snaps clips to frames (alignment %lld)."), Align);
			return nullptr;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %s CustomBoneAnimationSampleRate %d -> %d."),
			*Animation->GetPathName(), RateField->GetPropertyValue_InContainer(Animation), Rate);
		RateField->SetPropertyValue_InContainer(Animation, Rate);
		++RatesSet;
	}
	if (RatesSet != 1)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: the default stack has %d animation pipelines, not 1."),
			RatesSet);
		return nullptr;
	}
	UInterchangePipelineStackOverride* Override = NewObject<UInterchangePipelineStackOverride>();
	for (UInterchangePipelineBase* Pipeline : Stack)
	{
		Override->AddPipeline(Pipeline); // a path to the in-memory pipeline, which the manager duplicates
		Pipeline->AddToRoot();
		Rooted.Add(Pipeline);
	}
	Override->AddToRoot();
	Rooted.Add(Override);
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: importing on the default stack (%d pipelines) at %d Hz."),
		Stack.Num(), Rate);
	return Override;
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
	FString Pack, Role;
	FParse::Value(*Params, TEXT("Pack="), Pack);
	FParse::Value(*Params, TEXT("Role="), Role);
	int32 AnimSampleRate = 0;
	FParse::Value(*Params, TEXT("AnimSampleRate="), AnimSampleRate);
	if (AnimSampleRate < 0 || AnimSampleRate > 48000)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: -AnimSampleRate=%d is outside 0-48000."), AnimSampleRate);
		return 1;
	}
	const ftcore::AssetRules Rules = ftcore::characterRules(
		ftcore::assetPackFromName(TCHAR_TO_UTF8(*Pack)), ftcore::assetRoleFromName(TCHAR_TO_UTF8(*Role)));
	if (FPaths::IsRelative(Source))
	{
		Source = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Source);
	}
	if (!FPaths::FileExists(Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: %s doesn't exist."), *Source);
		return 1;
	}
	if (FPaths::GetExtension(Source).Equals(TEXT("glb"), ESearchCase::IgnoreCase) && !FT::CheckGlbFile(TEXT("FTImportCharacter"), Source, Rules))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: %s breaks the asset rules (docs/ASSET_INTAKE.md); nothing imported."),
			*Source);
		return 1;
	}
	TArray<UObject*> Rooted;
	UInterchangePipelineStackOverride* RateStack = nullptr;
	if (AnimSampleRate > 0)
	{
		if (!FPaths::GetExtension(Source).Equals(TEXT("glb"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogForgottenThird, Error,
				TEXT("FTImportCharacter: -AnimSampleRate needs a .glb (its clip lengths are checked)."));
			return 1;
		}
		if (!ClipsOnFrames(Source, AnimSampleRate))
		{
			return 1;
		}
		RateStack = RateOverride(Source, AnimSampleRate, Rooted);
		if (!RateStack)
		{
			for (UObject* Object : Rooted)
			{
				Object->RemoveFromRoot();
			}
			return 1;
		}
	}

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Source;
	Task->DestinationPath = Dest;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bSave = true;
	Task->bAsync = false;
	if (RateStack)
	{
		Task->Options = RateStack;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.ImportAssetTasks({ Task });
	for (UObject* Object : Rooted)
	{
		Object->RemoveFromRoot();
	}

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
	TArray<UMaterialInterface*> Materials;
	for (const FSkeletalMaterial& Slot : Mesh->GetMaterials())
	{
		Materials.Add(Slot.MaterialInterface);
	}
	const bool bMaterialsOk = FT::UseProjectMasters(TEXT("FTImportCharacter"), Materials);
	const bool bRigOk = CheckRig(*Mesh, bCheckFace);
	return bMaterialsOk && bRigOk ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter needs an editor build."));
	return 1;
#endif
}
