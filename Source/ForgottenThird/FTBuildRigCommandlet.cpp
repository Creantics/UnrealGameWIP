#include "FTBuildRigCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Retargeter/IKRetargeter.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigAutoCharacterizer.h"
#include "RigEditor/IKRigController.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
constexpr double kMoveDegrees = 0.1;
constexpr double kMoveCm = 0.01;

bool Differs(const FTransform& A, const FTransform& B)
{
	return FMath::RadiansToDegrees(A.GetRotation().AngularDistance(B.GetRotation())) > kMoveDegrees
		|| FVector::Dist(A.GetTranslation(), B.GetTranslation()) > kMoveCm;
}

// Logs one line per clip in the mesh's folder: frames, bone tracks that move over time or differ
// from the bind pose, and float curves whose value changes. Only reports; an empty clip is a
// warning, since the rig is built from the mesh's reference pose, not from a clip.
void CheckClips(const USkeletalMesh& Mesh)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByPath(FName(FPackageName::GetLongPackagePath(Mesh.GetOutermost()->GetName())), Assets);

	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetClassPath != UAnimSequence::StaticClass()->GetClassPathName())
		{
			continue;
		}
		const UAnimSequence* Clip = Cast<UAnimSequence>(Asset.GetAsset());
		const IAnimationDataModel* Model = Clip ? Clip->GetDataModel() : nullptr;
		if (!Model)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildRig: couldn't load clip %s."), *Asset.GetObjectPathString());
			continue;
		}

		int32 Moving = 0, OffBind = 0;
		TArray<FName> Tracks;
		Model->GetBoneTrackNames(Tracks);
		TArray<FTransform> Keys;
		for (const FName Track : Tracks)
		{
			Keys.Reset();
			Model->GetBoneTrackTransforms(Track, Keys);
			if (Keys.IsEmpty())
			{
				continue;
			}
			for (const FTransform& Key : Keys)
			{
				if (Differs(Key, Keys[0]))
				{
					++Moving;
					break;
				}
			}
			const int32 Bone = Skeleton.FindBoneIndex(Track);
			if (Bone != INDEX_NONE && Differs(Keys[0], Skeleton.GetRefBonePose()[Bone]))
			{
				++OffBind;
			}
		}

		int32 CurvesChanging = 0;
		for (const FFloatCurve& Curve : Model->GetFloatCurves())
		{
			const TArray<FRichCurveKey>& CurveKeys = Curve.FloatCurve.GetConstRefOfKeys();
			for (const FRichCurveKey& Key : CurveKeys)
			{
				if (!FMath::IsNearlyEqual(Key.Value, CurveKeys[0].Value, 1e-4f))
				{
					++CurvesChanging;
					break;
				}
			}
		}

		const bool bEmpty = Moving == 0 && OffBind == 0 && CurvesChanging == 0;
		UE_LOG(LogForgottenThird, Display,
			TEXT("FTBuildRig: clip %s: %d frames, %.2f s, %d bone tracks (%d move, %d off the bind pose), %d float curves (%d change)%s"),
			*Clip->GetName(), Model->GetNumberOfFrames(), Model->GetPlayLength(), Model->GetNumBoneTracks(), Moving, OffBind,
			Model->GetNumberOfFloatCurves(), CurvesChanging, bEmpty ? TEXT(" - EMPTY") : TEXT(""));
	}
}

// A chain the rig must have. KeyBone has to lie inside some chain; when none covers it after
// auto-characterizing, the chain is added by hand from Start to End (skipped if those bones are
// absent, e.g. a skeleton without fingers).
struct FRequiredChain
{
	const TCHAR* Name;
	const TCHAR* Start;
	const TCHAR* End;
	const TCHAR* KeyBone;
};

const FRequiredChain kChains[] = {
	{ TEXT("Spine"), TEXT("spine_01"), TEXT("spine_03"), TEXT("spine_01") },
	{ TEXT("Neck"), TEXT("neck_01"), TEXT("neck_02"), TEXT("neck_01") },
	{ TEXT("Head"), TEXT("head"), TEXT("head"), TEXT("head") },
	{ TEXT("LeftClavicle"), TEXT("clavicle_l"), TEXT("clavicle_l"), TEXT("clavicle_l") },
	{ TEXT("RightClavicle"), TEXT("clavicle_r"), TEXT("clavicle_r"), TEXT("clavicle_r") },
	{ TEXT("LeftArm"), TEXT("upperarm_l"), TEXT("hand_l"), TEXT("lowerarm_l") },
	{ TEXT("RightArm"), TEXT("upperarm_r"), TEXT("hand_r"), TEXT("lowerarm_r") },
	{ TEXT("LeftLeg"), TEXT("thigh_l"), TEXT("ball_l"), TEXT("calf_l") },
	{ TEXT("RightLeg"), TEXT("thigh_r"), TEXT("ball_r"), TEXT("calf_r") },
	{ TEXT("LeftThumb"), TEXT("thumb_01_l"), TEXT("thumb_03_l"), TEXT("thumb_01_l") },
	{ TEXT("LeftIndex"), TEXT("index_01_l"), TEXT("index_03_l"), TEXT("index_01_l") },
	{ TEXT("LeftMiddle"), TEXT("middle_01_l"), TEXT("middle_03_l"), TEXT("middle_01_l") },
	{ TEXT("LeftRing"), TEXT("ring_01_l"), TEXT("ring_03_l"), TEXT("ring_01_l") },
	{ TEXT("LeftPinky"), TEXT("pinky_01_l"), TEXT("pinky_03_l"), TEXT("pinky_01_l") },
	{ TEXT("RightThumb"), TEXT("thumb_01_r"), TEXT("thumb_03_r"), TEXT("thumb_01_r") },
	{ TEXT("RightIndex"), TEXT("index_01_r"), TEXT("index_03_r"), TEXT("index_01_r") },
	{ TEXT("RightMiddle"), TEXT("middle_01_r"), TEXT("middle_03_r"), TEXT("middle_01_r") },
	{ TEXT("RightRing"), TEXT("ring_01_r"), TEXT("ring_03_r"), TEXT("ring_01_r") },
	{ TEXT("RightPinky"), TEXT("pinky_01_r"), TEXT("pinky_03_r"), TEXT("pinky_01_r") },
};

// Bone names covered by any retarget chain.
TSet<FName> CoveredBones(const UIKRigController& Rig, const UIKRigDefinition& Asset, bool& bAllValid)
{
	const TArray<FName>& BoneNames = Asset.GetSkeleton().BoneNames;
	TSet<FName> Covered;
	bAllValid = true;
	for (const FBoneChain& Chain : Rig.GetRetargetChains())
	{
		TSet<int32> Indices;
		if (!Rig.ValidateChain(Chain.ChainName, nullptr, Indices))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: chain %s (%s -> %s) is invalid."), *Chain.ChainName.ToString(),
				*Chain.StartBone.BoneName.ToString(), *Chain.EndBone.BoneName.ToString());
			bAllValid = false;
			continue;
		}
		for (const int32 Index : Indices)
		{
			Covered.Add(BoneNames[Index]);
		}
	}
	return Covered;
}

template <typename T>
T* NewAsset(const FString& PackagePath)
{
	UPackage* Package = CreatePackage(*PackagePath);
	Package->FullyLoad();
	const FName Name(FPackageName::GetShortName(PackagePath));
	if (UObject* Old = FindObject<UObject>(Package, *Name.ToString()))
	{
		// Rebuild from scratch: move the old asset out of the way so the new one takes its name.
		Old->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Old->ClearFlags(RF_Public | RF_Standalone);
	}
	T* Asset = NewObject<T>(Package, Name, RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Asset);
	return Asset;
}

bool SaveAsset(UObject* Asset)
{
	UPackage* Package = Asset->GetOutermost();
	Package->MarkPackageDirty();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Asset, *File, Save))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: saved %s"), *File);
	return true;
}
} // namespace
#endif

UFTBuildRigCommandlet::UFTBuildRigCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildRigCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MeshPath = TEXT("/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head");
	FString Dest = TEXT("/Game/Characters/ProofHead/Rig");
	FParse::Value(*Params, TEXT("Mesh="), MeshPath);
	FParse::Value(*Params, TEXT("Dest="), Dest);

	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: couldn't load %s."), *MeshPath);
		return 1;
	}
	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: mesh %s, %d bones."), *Mesh->GetPathName(), Skeleton.GetNum());

	CheckClips(*Mesh);

	// IK Rig.
	UIKRigDefinition* RigAsset = NewAsset<UIKRigDefinition>(Dest / (TEXT("IK_") + Mesh->GetName()));
	UIKRigController* Rig = UIKRigController::GetController(RigAsset);
	if (!Rig->SetSkeletalMesh(Mesh))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: the IK Rig rejected the mesh."));
		return 1;
	}

	FAutoCharacterizeResults Auto;
	Rig->AutoGenerateRetargetDefinition(Auto);
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: auto-characterize: template %s (%s), %d bones matched, score %.2f, %d missing."),
		*Auto.BestTemplateName.ToString(), Auto.bUsedTemplate ? TEXT("used") : TEXT("not used"), Auto.BestNumMatchingBones,
		Auto.BestPercentageOfTemplateScore, Auto.MissingBones.Num());
	Rig->ApplyAutoGeneratedRetargetDefinition();

	if (Skeleton.FindBoneIndex(TEXT("pelvis")) != INDEX_NONE)
	{
		Rig->SetRetargetRoot(TEXT("pelvis"));
	}
	if (Skeleton.FindBoneIndex(TEXT("root")) != INDEX_NONE)
	{
		Rig->SetRootMotionBone(TEXT("root"));
	}

	bool bAllValid = true;
	TSet<FName> Covered = CoveredBones(*Rig, *RigAsset, bAllValid);
	for (const FRequiredChain& Chain : kChains)
	{
		if (Covered.Contains(Chain.KeyBone))
		{
			continue;
		}
		if (Skeleton.FindBoneIndex(Chain.Start) == INDEX_NONE || Skeleton.FindBoneIndex(Chain.End) == INDEX_NONE)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: no %s chain (the skeleton has no %s/%s)."), Chain.Name, Chain.Start, Chain.End);
			continue;
		}
		const FName Added = Rig->AddRetargetChain(Chain.Name, Chain.Start, Chain.End, NAME_None);
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: added %s by hand (the template missed it)."), *Added.ToString());
	}
	Rig->SortRetargetChains();
	Covered = CoveredBones(*Rig, *RigAsset, bAllValid);

	bool bOk = bAllValid;
	const FName Root = Rig->GetRetargetRoot();
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: retarget root %s, root motion bone %s, %d chains:"), *Root.ToString(),
		*Rig->GetRootMotionBone().ToString(), Rig->GetRetargetChains().Num());
	for (const FBoneChain& Chain : Rig->GetRetargetChains())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig:   %-14s %s -> %s, goal %s"), *Chain.ChainName.ToString(),
			*Chain.StartBone.BoneName.ToString(), *Chain.EndBone.BoneName.ToString(), *Chain.IKGoalName.ToString());
	}
	if (Skeleton.FindBoneIndex(TEXT("pelvis")) != INDEX_NONE && Root != TEXT("pelvis"))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: the retarget root is %s, not pelvis."), *Root.ToString());
		bOk = false;
	}
	for (const FRequiredChain& Chain : kChains)
	{
		if (Skeleton.FindBoneIndex(Chain.KeyBone) != INDEX_NONE && !Covered.Contains(Chain.KeyBone))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: no chain covers %s."), Chain.KeyBone);
			bOk = false;
		}
	}
	if (!bOk || !SaveAsset(RigAsset))
	{
		return 1;
	}

	// Retargeter, source side only.
	UIKRetargeter* RetargeterAsset = NewAsset<UIKRetargeter>(Dest / (TEXT("RTG_") + Mesh->GetName()));
	UIKRetargeterController* Retargeter = UIKRetargeterController::GetController(RetargeterAsset);
	Retargeter->SetIKRig(ERetargetSourceOrTarget::Source, RigAsset);
	Retargeter->SetPreviewMesh(ERetargetSourceOrTarget::Source, Mesh);
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: retargeter source is %s; no target (no stock mannequin in Engine/Content)."),
		*RigAsset->GetPathName());
	return SaveAsset(RetargeterAsset) ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig needs an editor build."));
	return 1;
#endif
}
