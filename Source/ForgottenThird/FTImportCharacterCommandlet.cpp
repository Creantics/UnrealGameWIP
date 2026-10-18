#include "FTImportCharacterCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
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
	return CheckRig(*Mesh, bCheckFace) ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter needs an editor build."));
	return 1;
#endif
}
