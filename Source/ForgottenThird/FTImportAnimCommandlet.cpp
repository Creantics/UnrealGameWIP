#include "FTImportAnimCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxImportUI.h"
#include "FTText.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "ftcore/AnimImport.h"

#include <string>
#include <vector>

namespace
{
namespace AI = ftcore::animimport;

// Every value of a repeatable -Key=value switch, in order (quoted values keep their spaces).
TArray<FString> AllValues(const FString& Params, const TCHAR* Key)
{
	TArray<FString> Out;
	const FString Prefix = FString(TEXT("-")) + Key + TEXT("=");
	const TCHAR* Cursor = *Params;
	FString Token;
	while (FParse::Token(Cursor, Token, false))
	{
		if (Token.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			Out.Add(Token.RightChop(Prefix.Len()).TrimQuotes());
		}
	}
	return Out;
}

bool SaveAnimAsset(UObject& Asset)
{
	UPackage* Package = Asset.GetOutermost();
	Package->MarkPackageDirty();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, &Asset, *File, Save))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportAnim: saved %s"), *File);
	return true;
}

// The bone's reference-pose transform in component space.
FTransform RefComponentSpace(const FReferenceSkeleton& Ref, int32 Bone)
{
	FTransform T = FTransform::Identity;
	for (int32 B = Bone; B != INDEX_NONE; B = Ref.GetParentIndex(B))
	{
		T = T * Ref.GetRefBonePose()[B];
	}
	return T;
}

// Puts the socket on the skeleton (replacing one of the same name). A component-space point is turned into the bone's
// frame with Ref: the reference pose of the mesh the point was measured on. False when its bone is missing.
bool PutSocket(USkeleton& Skeleton, const FReferenceSkeleton& Ref, const AI::SocketSpec& Spec)
{
	const FName Bone(FT::ToFString(Spec.bone));
	const int32 BoneIndex = Ref.FindBoneIndex(Bone);
	if (BoneIndex == INDEX_NONE)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: socket %s: bone %s is not on %s."), *FT::ToFString(Spec.name),
			*Bone.ToString(), *Skeleton.GetPathName());
		return false;
	}
	FTransform Local;
	const FTransform BoneCs = RefComponentSpace(Ref, BoneIndex);
	if (Spec.componentSpace)
	{
		const FVector Point(Spec.x, Spec.y, Spec.z);
		const FQuat Rot = Spec.hasAxis ? FRotationMatrix::MakeFromX(FVector(Spec.ax, Spec.ay, Spec.az)).ToQuat() : BoneCs.GetRotation();
		Local = FTransform(Rot, Point).GetRelativeTransform(BoneCs);
		UE_LOG(LogForgottenThird, Display,
			TEXT("FTImportAnim: socket %s on %s: component-space point %s (bone at %s, %.1f cm away) -> bone-local %s, rotation %s."),
			*FT::ToFString(Spec.name), *Bone.ToString(), *Point.ToString(), *BoneCs.GetLocation().ToString(),
			FVector::Dist(Point, BoneCs.GetLocation()), *Local.GetLocation().ToString(), *Local.Rotator().ToString());
	}
	else
	{
		Local = FTransform(FRotator(Spec.pitch, Spec.yaw, Spec.roll), FVector(Spec.x, Spec.y, Spec.z));
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportAnim: socket %s on %s: bone-local %s, rotation %s (component space %s)."),
			*FT::ToFString(Spec.name), *Bone.ToString(), *Local.GetLocation().ToString(), *Local.Rotator().ToString(),
			*(Local * BoneCs).GetLocation().ToString());
	}
	const FName Name(FT::ToFString(Spec.name));
	for (int32 I = Skeleton.Sockets.Num() - 1; I >= 0; --I)
	{
		if (Skeleton.Sockets[I] && Skeleton.Sockets[I]->SocketName == Name)
		{
			Skeleton.Sockets.RemoveAt(I);
		}
	}
	USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(&Skeleton);
	Socket->SocketName = Name;
	Socket->BoneName = Bone;
	Socket->RelativeLocation = Local.GetLocation();
	Socket->RelativeRotation = Local.Rotator();
	Socket->RelativeScale = FVector::OneVector;
	Skeleton.Sockets.Add(Socket);
	return true;
}
} // namespace
#endif

UFTImportAnimCommandlet::UFTImportAnimCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTImportAnimCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString Source, SkeletonPath, Dest, Name;
	if (!FParse::Value(*Params, TEXT("Source="), Source) || !FParse::Value(*Params, TEXT("Skeleton="), SkeletonPath) ||
		!FParse::Value(*Params, TEXT("Dest="), Dest) || !FParse::Value(*Params, TEXT("Name="), Name))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: pass -Source=<fbx> -Skeleton=<skeleton or mesh> -Dest=<folder> -Name=<clip>."));
		return 1;
	}
	int32 Fps = 0;
	FParse::Value(*Params, TEXT("Fps="), Fps);
	double MinShare = 0.9;
	FParse::Value(*Params, TEXT("MinTrackShare="), MinShare);
	const bool bRootLock = FParse::Param(*Params, TEXT("RootLock"));

	std::vector<AI::SocketSpec> Sockets;
	for (const FString& Text : AllValues(Params, TEXT("Socket")))
	{
		AI::SocketSpec Spec;
		std::string Error;
		if (!AI::ParseSocketSpec(TCHAR_TO_UTF8(*Text), Spec, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: -Socket=%s: %s"), *Text, *FT::ToFString(Error));
			return 1;
		}
		Sockets.push_back(Spec);
	}

	if (FPaths::IsRelative(Source))
	{
		Source = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Source);
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: can't read %s."), *Source);
		return 1;
	}

	UObject* Loaded = LoadObject<UObject>(nullptr, *SkeletonPath);
	USkeleton* Skeleton = Cast<USkeleton>(Loaded);
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(Loaded);
	if (!Skeleton && Mesh)
	{
		Skeleton = Mesh->GetSkeleton();
	}
	if (!Skeleton)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: %s is not a skeleton or a skeletal mesh with one."), *SkeletonPath);
		return 1;
	}
	const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
	// Socket points are measured on a mesh: its own reference pose (a shared skeleton's proportions may not be its).
	const FReferenceSkeleton& SocketRef = Mesh ? Mesh->GetRefSkeleton() : Ref;
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportAnim: skeleton %s, %d bones; sockets measured on %s."), *Skeleton->GetPathName(),
		Ref.GetNum(), Mesh ? *Mesh->GetPathName() : TEXT("the skeleton"));

	// The file's bones against the skeleton's: a bone the skeleton lacks would be dropped without a word by the importer.
	const std::vector<std::string> FileBones = AI::FbxLimbNames(Bytes.GetData(), static_cast<std::size_t>(Bytes.Num()));
	std::vector<std::string> SkeletonBones;
	SkeletonBones.reserve(Ref.GetNum());
	for (int32 B = 0; B < Ref.GetNum(); ++B)
	{
		SkeletonBones.push_back(TCHAR_TO_UTF8(*Ref.GetBoneName(B).ToString()));
	}
	const std::vector<std::string> Missing = AI::MissingNames(FileBones, SkeletonBones);
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportAnim: %s has %d bones; %d of them not on the skeleton."), *Source,
		static_cast<int32>(FileBones.size()), static_cast<int32>(Missing.size()));
	if (FileBones.empty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: no bones (LimbNode models) found in %s."), *Source);
		return 1;
	}
	if (!Missing.empty())
	{
		for (const std::string& Bone : Missing)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: bone %s is in the file but not on the skeleton."), *FT::ToFString(Bone));
		}
		return 1;
	}

	// Sockets first: a bad bone fails before anything is written.
	for (const AI::SocketSpec& Spec : Sockets)
	{
		if (!PutSocket(*Skeleton, SocketRef, Spec))
		{
			return 1;
		}
	}
	if (!Sockets.empty() && !SaveAnimAsset(*Skeleton))
	{
		return 1;
	}

	// A clip of the same name goes first, so the new one takes its name.
	const FString ObjectPath = Dest / Name + TEXT(".") + Name;
	if (UObject* Old = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		TArray<UObject*> Doomed = {Old};
		const int32 Deleted = ObjectTools::ForceDeleteObjects(Doomed, false);
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportAnim: replaced the old %s (%d deleted)."), *ObjectPath, Deleted);
	}

	UFbxImportUI* Ui = NewObject<UFbxImportUI>();
	Ui->bIsObjImport = false;
	Ui->bAutomatedImportShouldDetectType = false;
	Ui->MeshTypeToImport = FBXIT_Animation;
	Ui->OriginalImportType = FBXIT_SkeletalMesh;
	Ui->bImportAsSkeletal = true;
	Ui->bImportMesh = false;
	Ui->bImportAnimations = true;
	Ui->bImportMaterials = false;
	Ui->bImportTextures = false;
	Ui->bCreatePhysicsAsset = false;
	Ui->Skeleton = Skeleton;
	Ui->bOverrideFullName = true;
	UFbxAnimSequenceImportData* AnimData = Ui->AnimSequenceImportData;
	AnimData->AnimationLength = FBXALIT_ExportedTime;
	AnimData->bImportBoneTracks = true;
	AnimData->bRemoveRedundantKeys = false;
	AnimData->bImportCustomAttribute = false;
	AnimData->bImportMeshesInBoneHierarchy = false;
	AnimData->bSnapToClosestFrameBoundary = true;
	// Rates are per clip: never the engine's default rate (it resamples to 30). A custom rate of 0 keeps the file's.
	AnimData->bUseDefaultSampleRate = false;
	AnimData->CustomSampleRate = FMath::Max(Fps, 0);

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Source;
	Task->DestinationPath = Dest;
	Task->DestinationName = Name;
	Task->Options = Ui;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bSave = false;
	Task->bAsync = false;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.ImportAssetTasks({Task});

	UAnimSequence* Anim = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportAnim: the import gave %s (%s)."), *GetPathNameSafe(Object),
			Object ? *Object->GetClass()->GetName() : TEXT("null"));
		if (UAnimSequence* Found = Cast<UAnimSequence>(Object))
		{
			Anim = Anim ? Anim : Found;
		}
	}
	if (!Anim)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: no clip came out of %s."), *Source);
		return 1;
	}
	if (Anim->GetName() != Name)
	{
		// The importer named it after the file's take: give it the asked name.
		TArray<FAssetRenameData> Renames;
		Renames.Emplace(Anim, Dest, Name);
		if (!AssetTools.RenameAssets(Renames))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: couldn't rename %s to %s."), *Anim->GetPathName(), *Name);
			return 1;
		}
	}
	if (Anim->GetSkeleton() != Skeleton)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: %s came in on %s, not %s."), *Anim->GetPathName(),
			*GetPathNameSafe(Anim->GetSkeleton()), *Skeleton->GetPathName());
		return 1;
	}
	if (bRootLock)
	{
		Anim->bEnableRootMotion = false;
		Anim->bForceRootLock = true;
		Anim->RootMotionRootLock = ERootMotionRootLock::AnimFirstFrame;
	}

	const IAnimationDataModel* Model = Anim->GetDataModel();
	const int32 Tracks = Model ? Model->GetNumBoneTracks() : 0;
	const int32 Keys = Model ? Model->GetNumberOfKeys() : 0;
	const double Rate = Model ? Model->GetFrameRate().AsDecimal() : 0.0;
	const double Length = Model ? Model->GetPlayLength() : 0.0;
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportAnim: %s: %d keys at %.3f fps, %.4f s, %d bone tracks of the file's %d bones, root %s."),
		*Anim->GetPathName(), Keys, Rate, Length, Tracks, static_cast<int32>(FileBones.size()), bRootLock ? TEXT("locked") : TEXT("free"));
	if (!AI::TrackShareOk(Tracks, static_cast<int32>(FileBones.size()), MinShare))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim: %d tracks is under %.2f of the file's %d bones."), Tracks, MinShare,
			static_cast<int32>(FileBones.size()));
		return 1;
	}
	for (const TObjectPtr<USkeletalMeshSocket>& Socket : Skeleton->Sockets)
	{
		if (Socket)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTImportAnim: skeleton socket %s on %s at %s."), *Socket->SocketName.ToString(),
				*Socket->BoneName.ToString(), *Socket->RelativeLocation.ToString());
		}
	}
	return SaveAnimAsset(*Anim) ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTImportAnim needs the editor."));
	return 1;
#endif
}
