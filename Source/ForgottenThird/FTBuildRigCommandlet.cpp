#include "FTBuildRigCommandlet.h"

#include "ForgottenThird.h"
#include "FTLocomotionAnimInstance.h"

#if WITH_EDITOR
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Retargeter/IKRetargeter.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "RetargetEditor/IKRetargeterPoseGenerator.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/RetargetOps/RootMotionGeneratorOp.h"
#include "Retargeter/RetargetOps/FKChainsOp.h"
#include "Retargeter/RetargetOps/RunIKRigOp.h"
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

// A bone's component-space transform at Key, composed up the parent chain from the clip's tracks
// (the bind pose where a bone has no track).
FTransform ComponentTransform(const IAnimationDataModel& Model, const FReferenceSkeleton& Skeleton, int32 Bone, int32 Key)
{
	FTransform Result = FTransform::Identity;
	TArray<FTransform> Keys;
	for (int32 B = Bone; B != INDEX_NONE; B = Skeleton.GetParentIndex(B))
	{
		Keys.Reset();
		Model.GetBoneTrackTransforms(Skeleton.GetBoneName(B), Keys);
		const FTransform Local = Keys.IsValidIndex(Key) ? Keys[Key] : Skeleton.GetRefBonePose()[B];
		Result = Result * Local;
	}
	return Result;
}

// Logs the clip's gait as the game measures it at load (UFTLocomotionAnimInstance::MeasureClip):
// the planted feet's ground speed relative to the root bone (the capsule speed at which the clip
// plays at rate 1 with no sliding), strides per loop and where the left foot plants; and the
// root's own speed, which for a clip with root motion should match the ground speed.
void LogFootSpeed(const UAnimSequence& Clip, const FReferenceSkeleton& Skeleton)
{
	const IAnimationDataModel* Model = Clip.GetDataModel();
	const int32 Frames = Model ? Model->GetNumberOfKeys() : 0;
	const double Length = Model ? Model->GetPlayLength() : 0.0;
	if (Frames < 3 || Length <= 0.0)
	{
		return;
	}
	const ftcore::LocoClipInfo Gait = UFTLocomotionAnimInstance::MeasureClip(Clip);
	if (Gait.groundSpeed <= 0.f)
	{
		return;
	}
	auto Travel = [&](int32 Bone)
	{
		return (ComponentTransform(*Model, Skeleton, Bone, Frames - 1).GetTranslation() - ComponentTransform(*Model, Skeleton, Bone, 0).GetTranslation())
				   .Size2D()
			/ Length;
	};
	// The pelvis carries the body: its travel should match the root's, or the root runs ahead of the feet.
	const int32 Pelvis = Skeleton.FindBoneIndex(TEXT("pelvis"));
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTBuildRig: clip %s gait: ground speed %.0f cm/s (feet %.0f, %.0f sideways), %d strides per loop, left plant at %.2f, root motion %.0f cm/s, pelvis %.0f cm/s"),
		*Clip.GetName(), Gait.groundSpeed, Gait.footSpeed, Gait.footSide, Gait.cycles, Gait.leftPlant, Travel(0),
		Pelvis != INDEX_NONE ? Travel(Pelvis) : 0.0);
}

// Logs how far a clip turns, on the root bone and on the pelvis (component space, first to last
// frame), and the root's travel. A turn or pivot the game plays as root motion needs the yaw on the
// root; skipped for clips that turn less than 5 degrees.
void LogTurn(const UAnimSequence& Clip, const FReferenceSkeleton& Skeleton)
{
	const IAnimationDataModel* Model = Clip.GetDataModel();
	const int32 Frames = Model ? Model->GetNumberOfKeys() : 0;
	const int32 Pelvis = Skeleton.FindBoneIndex(TEXT("pelvis"));
	if (Frames < 2 || Pelvis == INDEX_NONE)
	{
		return;
	}
	// Summed frame to frame, so a half turn keeps its sign.
	TArray<double> RootYaw;
	RootYaw.SetNumZeroed(Frames);
	double PelvisYaw = 0.0;
	for (int32 F = 1; F < Frames; ++F)
	{
		auto Step = [&](int32 Bone)
		{
			const FQuat A = ComponentTransform(*Model, Skeleton, Bone, F - 1).GetRotation();
			const FQuat B = ComponentTransform(*Model, Skeleton, Bone, F).GetRotation();
			return FMath::RadiansToDegrees((B * A.Inverse()).GetTwistAngle(FVector::UpVector));
		};
		RootYaw[F] = RootYaw[F - 1] + Step(0);
		PelvisYaw += Step(Pelvis);
	}
	const double Turn = RootYaw.Last();
	if (FMath::Abs(Turn) < 5.0 && FMath::Abs(PelvisYaw) < 5.0)
	{
		return;
	}
	// When the root has turned 10%, half and 90% of the way, and its speed over the first and last tenth.
	const double Length = Model->GetPlayLength();
	auto TimeAt = [&](double Share)
	{
		for (int32 F = 0; F < Frames; ++F)
		{
			if (FMath::Abs(RootYaw[F]) >= FMath::Abs(Turn) * Share)
			{
				return Length * F / (Frames - 1);
			}
		}
		return Length;
	};
	const int32 Tenth = FMath::Max(1, (Frames - 1) / 10);
	auto Speed = [&](int32 From)
	{
		return (ComponentTransform(*Model, Skeleton, 0, From + Tenth).GetTranslation() - ComponentTransform(*Model, Skeleton, 0, From).GetTranslation()).Size2D()
			/ (Length * Tenth / (Frames - 1));
	};
	const FVector Travel = ComponentTransform(*Model, Skeleton, 0, Frames - 1).GetTranslation() - ComponentTransform(*Model, Skeleton, 0, 0).GetTranslation();
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTBuildRig: clip %s turn: root %.0f deg (10%% at %.2f s, half at %.2f, 90%% at %.2f of %.2f), pelvis %.0f deg, root travels %.0f cm (%.0f, %.0f), %.0f cm/s at first, %.0f at last"),
		*Clip.GetName(), Turn, TimeAt(0.1), TimeAt(0.5), TimeAt(0.9), Length, PelvisYaw, Travel.Size2D(), Travel.X, Travel.Y, Speed(0),
		Speed(Frames - 1 - Tenth));
}

// Joint angles of a clip at a share of its length, from component-space bone positions (not local
// rotations, which depend on each rig's bone roll): elbow and knee bend (0 = straight), upper arm
// raised from hanging down (90 = level), and the pelvis-to-head line's lean forward (+X) and sideways.
// Also the neck_01-to-head line's lean forward, and how far the head is turned chin down from its own
// rest (its rotation against the ref pose's, applied to the way the body faces).
struct FPoseAngles
{
	double Elbow[2] = {}, Arm[2] = {}, Knee[2] = {}, Lean = 0.0, Side = 0.0, Neck = 0.0, ChinDown = 0.0;
	// Each hand from its shoulder in the root's frame (cm): forward, out to its own side, and up.
	FVector Hand[2];
	bool bValid = false;
};

FPoseAngles MeasurePose(const UAnimSequence& Clip, double Share)
{
	FPoseAngles Pose;
	const IAnimationDataModel* Model = Clip.GetDataModel();
	const USkeleton* Skel = Clip.GetSkeleton();
	if (!Model || !Skel || Model->GetNumberOfKeys() < 1)
	{
		return Pose;
	}
	const FReferenceSkeleton& Skeleton = Skel->GetReferenceSkeleton();
	const int32 Key = FMath::RoundToInt(Share * (Model->GetNumberOfKeys() - 1));
	auto At = [&](const TCHAR* Name, FVector& Out)
	{
		const int32 Bone = Skeleton.FindBoneIndex(Name);
		if (Bone != INDEX_NONE)
		{
			Out = ComponentTransform(*Model, Skeleton, Bone, Key).GetTranslation();
		}
		return Bone != INDEX_NONE;
	};
	auto Bend = [](const FVector& A, const FVector& B, const FVector& C)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(((B - A).GetSafeNormal() | (C - B).GetSafeNormal()), -1.0, 1.0)));
	};
	const TCHAR* Sides[2] = {TEXT("_l"), TEXT("_r")};
	FVector Pelvis, Head;
	bool bOk = At(TEXT("pelvis"), Pelvis) && At(TEXT("head"), Head);
	const FTransform Root = ComponentTransform(*Model, Skeleton, 0, Key);
	for (int32 S = 0; S < 2 && bOk; ++S)
	{
		FVector Upper, Lower, Hand, Thigh, Calf, Foot;
		bOk = At(*(FString(TEXT("upperarm")) + Sides[S]), Upper) && At(*(FString(TEXT("lowerarm")) + Sides[S]), Lower)
			&& At(*(FString(TEXT("hand")) + Sides[S]), Hand) && At(*(FString(TEXT("thigh")) + Sides[S]), Thigh)
			&& At(*(FString(TEXT("calf")) + Sides[S]), Calf) && At(*(FString(TEXT("foot")) + Sides[S]), Foot);
		if (bOk)
		{
			Pose.Elbow[S] = Bend(Upper, Lower, Hand);
			Pose.Knee[S] = Bend(Thigh, Calf, Foot);
			Pose.Arm[S] = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(((Lower - Upper).GetSafeNormal() | FVector::DownVector), -1.0, 1.0)));
			// UE mannequins face +Y with their left side at +X.
			const FVector H = Root.InverseTransformVectorNoScale(Hand - Upper);
			Pose.Hand[S] = FVector(H.Y, S == 0 ? H.X : -H.X, H.Z);
		}
	}
	if (bOk)
	{
		// Measured in the root's frame at that key, so a turning clip's lean stays forward.
		const FVector Up = Root.InverseTransformVectorNoScale(Head - Pelvis).GetSafeNormal();
		Pose.Lean = FMath::RadiansToDegrees(FMath::Atan2(Up.Y, Up.Z)); // UE mannequins face +Y
		Pose.Side = FMath::RadiansToDegrees(FMath::Atan2(Up.X, Up.Z));
		Pose.bValid = true;
		FVector Neck;
		const int32 HeadBone = Skeleton.FindBoneIndex(TEXT("head"));
		if (At(TEXT("neck_01"), Neck))
		{
			const FVector N = Root.InverseTransformVectorNoScale(Head - Neck).GetSafeNormal();
			Pose.Neck = FMath::RadiansToDegrees(FMath::Atan2(N.Y, N.Z));
		}
		FQuat RefHead = FQuat::Identity;
		for (int32 Bone = HeadBone; Bone != INDEX_NONE; Bone = Skeleton.GetParentIndex(Bone))
		{
			RefHead = Skeleton.GetRefBonePose()[Bone].GetRotation() * RefHead;
		}
		const FQuat Turned = ComponentTransform(*Model, Skeleton, HeadBone, Key).GetRotation() * RefHead.Inverse();
		const FVector Face = Root.InverseTransformVectorNoScale(Turned.RotateVector(FVector::YAxisVector));
		Pose.ChinDown = FMath::RadiansToDegrees(FMath::Atan2(-Face.Z, Face.Y));
	}
	return Pose;
}

// -ComparePose=<source clip>,<retargeted clip>[,<retargeted clip>...]: logs both clips' joint angles
// at ten points through each (-PoseSteps=<n> for more), so a retarget that straightens the arms or legs
// shows as numbers.
int32 ComparePoses(const FString& List, int32 Steps)
{
	TArray<FString> Paths;
	List.ParseIntoArray(Paths, TEXT(","));
	TArray<UAnimSequence*> Clips;
	for (const FString& Path : Paths)
	{
		UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *Path);
		if (!Clip)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: couldn't load %s."), *Path);
			return 1;
		}
		Clips.Add(Clip);
	}
	Steps = FMath::Max(Steps, 1);
	for (int32 I = 0; I <= Steps; ++I)
	{
		for (const UAnimSequence* Clip : Clips)
		{
			const double Share = static_cast<double>(I) / Steps;
			const FPoseAngles P = MeasurePose(*Clip, Share);
			UE_LOG(LogForgottenThird, Display,
				TEXT("FTBuildRig: pose %.3f %-48s elbow %5.0f %5.0f  arm %5.0f %5.0f  knee %5.0f %5.0f  lean %5.0f side %5.0f  neck %5.1f chin %5.1f  hand L %4.0f %4.0f %4.0f  R %4.0f %4.0f %4.0f%s"),
				Share, *Clip->GetName(), P.Elbow[0], P.Elbow[1], P.Arm[0], P.Arm[1], P.Knee[0], P.Knee[1], P.Lean, P.Side, P.Neck, P.ChinDown,
				P.Hand[0].X, P.Hand[0].Y, P.Hand[0].Z, P.Hand[1].X, P.Hand[1].Y, P.Hand[1].Z, P.bValid ? TEXT("") : TEXT(" (missing bones)"));
		}
	}
	return 0;
}

// Logs one line for a clip: frames, bone tracks that move over time or differ from the bind pose,
// and float curves whose value changes. Returns false when the clip is empty (nothing moves and
// nothing is off the bind pose).
bool LogClip(const UAnimSequence& Clip, const FReferenceSkeleton& Skeleton)
{
	const IAnimationDataModel* Model = Clip.GetDataModel();
	if (!Model)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildRig: clip %s has no data model."), *Clip.GetPathName());
		return false;
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
		*Clip.GetName(), Model->GetNumberOfFrames(), Model->GetPlayLength(), Model->GetNumBoneTracks(), Moving, OffBind,
		Model->GetNumberOfFloatCurves(), CurvesChanging, bEmpty ? TEXT(" - EMPTY") : TEXT(""));
	LogFootSpeed(Clip, Skeleton);
	LogTurn(Clip, Skeleton);
	return !bEmpty;
}

// Logs every clip in the mesh's folder. Only reports; an empty clip is not an error, since the rig
// is built from the mesh's reference pose, not from a clip.
void CheckClips(const USkeletalMesh& Mesh)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByPath(FName(FPackageName::GetLongPackagePath(Mesh.GetOutermost()->GetName())), Assets);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetClassPath != UAnimSequence::StaticClass()->GetClassPathName())
		{
			continue;
		}
		if (const UAnimSequence* Clip = Cast<UAnimSequence>(Asset.GetAsset()))
		{
			LogClip(*Clip, Mesh.GetRefSkeleton());
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildRig: couldn't load clip %s."), *Asset.GetObjectPathString());
		}
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

// Writes RTG_<source mesh>_to_<target mesh> in Dest (source = SourceRig, target = TargetRig, the
// default op stack, chains mapped by exact name), then retargets each clip in Clips into
// Dest/Retargeted and checks that every result has frames and moving tracks. With bAlignPose the
// target gets a new retarget pose with its chains turned to the source's directions (for a T-pose
// source onto an A-pose target). bNoAlign keeps the target's own rest pose as its retarget pose (no
// chain turned), for comparing against the aligned result; chains named in KeepChains (Pelvis: the rig's pelvis bone)
// are left at rest and the others aligned under them. False when a chain is unmapped, a clip can't be loaded, a result
// is empty or a save fails.
bool RetargetClips(UIKRigDefinition& SourceRig, UIKRigDefinition& TargetRig, USkeletalMesh& TargetMesh, const FString& Dest,
	const TArray<FString>& Clips, bool bAlignPose, bool bNoAlign, const TArray<FString>& KeepChains)
{
	USkeletalMesh* SourceMesh = SourceRig.GetPreviewMesh();
	if (!SourceMesh || SourceMesh == &TargetMesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: the source rig %s needs a preview mesh other than the target."),
			*SourceRig.GetPathName());
		return false;
	}

	UIKRetargeter* Asset = NewAsset<UIKRetargeter>(Dest / FString::Printf(TEXT("RTG_%s_to_%s"), *SourceMesh->GetName(), *TargetMesh.GetName()));
	const UIKRetargeterController* Retargeter = UIKRetargeterController::GetController(Asset);
	Retargeter->SetIKRig(ERetargetSourceOrTarget::Source, &SourceRig);
	Retargeter->SetIKRig(ERetargetSourceOrTarget::Target, &TargetRig);
	Retargeter->SetPreviewMesh(ERetargetSourceOrTarget::Source, SourceMesh);
	Retargeter->SetPreviewMesh(ERetargetSourceOrTarget::Target, &TargetMesh);
	Retargeter->AddDefaultOps();
	Retargeter->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, &SourceRig);
	Retargeter->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, &TargetRig);
	Retargeter->AutoMapChains(EAutoMapChainType::Exact, true);
	if (bAlignPose && !bNoAlign)
	{
		const FName Pose = Retargeter->CreateRetargetPose(TEXT("AlignedToSource"), ERetargetSourceOrTarget::Target);
		Retargeter->AutoAlignAllBones(ERetargetSourceOrTarget::Target, ERetargetAutoAlignMethod::ChainToChain);
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: target retarget pose %s aligned to the source chains."), *Pose.ToString());
	}

	// Other rigs' names for the same chain (the UEFN mannequin's foot chain is its toe).
	static const TCHAR* const Aliases[][2] = {{TEXT("LeftFoot"), TEXT("LeftToe")}, {TEXT("RightFoot"), TEXT("RightToe")}};
	auto HasSourceChain = [&SourceRig](FName Name)
	{
		return SourceRig.GetRetargetChains().ContainsByPredicate([Name](const FBoneChain& C) { return C.ChainName == Name; });
	};
	bool bOk = true;
	int32 Mapped = 0;
	for (const FBoneChain& Chain : TargetRig.GetRetargetChains())
	{
		FName Source = Retargeter->GetSourceChain(Chain.ChainName);
		for (const auto& Alias : Aliases)
		{
			if (Source == NAME_None && Chain.ChainName == FName(Alias[0]) && HasSourceChain(Alias[1]))
			{
				Retargeter->SetSourceChain(Alias[1], Chain.ChainName);
				Source = Retargeter->GetSourceChain(Chain.ChainName);
			}
		}
		for (const FBoneChain& C : SourceRig.GetRetargetChains())
		{
			if (C.ChainName == Source)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig:   %-14s %s -> %s  <-  source %s %s -> %s"), *Chain.ChainName.ToString(),
					*Chain.StartBone.BoneName.ToString(), *Chain.EndBone.BoneName.ToString(), *C.ChainName.ToString(),
					*C.StartBone.BoneName.ToString(), *C.EndBone.BoneName.ToString());
			}
		}
		if (Source == NAME_None)
		{
			FString Names;
			for (const FBoneChain& C : SourceRig.GetRetargetChains())
			{
				Names += TEXT(" ") + C.ChainName.ToString();
			}
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: target chain %s has no source chain (source chains:%s)."),
				*Chain.ChainName.ToString(), *Names);
			bOk = false;
		}
		else
		{
			++Mapped;
		}
	}
	// Turn the target's chains to point the way the source's do in its retarget pose. Rigs whose ref poses differ (the UEFN
	// mannequin's against a Tripo body) otherwise swing each leg in a turned plane, and the planted feet sweep sideways.
	if (!bNoAlign)
	{
		Retargeter->AutoAlignAllBones(ERetargetSourceOrTarget::Target, ERetargetAutoAlignMethod::ChainToChain);
		if (!KeepChains.IsEmpty())
		{
			const FReferenceSkeleton& Skeleton = TargetMesh.GetRefSkeleton();
			TArray<FName> Kept;
			TArray<FName> Aligned;
			if (KeepChains.Contains(TEXT("Pelvis")))
			{
				Kept.Add(TargetRig.GetPelvis());
			}
			for (const FBoneChain& Chain : TargetRig.GetRetargetChains())
			{
				TArray<FName>& Bones = KeepChains.Contains(Chain.ChainName.ToString()) ? Kept : Aligned;
				const int32 Start = Skeleton.FindBoneIndex(Chain.StartBone.BoneName);
				for (int32 Bone = Skeleton.FindBoneIndex(Chain.EndBone.BoneName); Bone != INDEX_NONE; Bone = Skeleton.GetParentIndex(Bone))
				{
					Bones.Add(Skeleton.GetBoneName(Bone));
					if (Bone == Start)
					{
						break;
					}
				}
			}
			// The kept bones go back to rest, which turns the chains under them; those are aligned again.
			Retargeter->ResetRetargetPose(Retargeter->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target), Kept,
				ERetargetSourceOrTarget::Target);
			Retargeter->AutoAlignBones(Aligned, ERetargetAutoAlignMethod::ChainToChain, ERetargetSourceOrTarget::Target);
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: %d bones of %s kept at rest, %d aligned."), Kept.Num(),
				*FString::Join(KeepChains, TEXT(",")), Aligned.Num());
		}
	}

	// The root motion op takes its roots from the rigs, and the source rig's root is the mannequin's pelvis: the target
	// root would copy the pelvis (hip height, bob and sway) and the game, which holds the root still, would lose them.
	// Copy the skeletons' own roots, which carry only the ground motion.
	if (FIKRetargetRootMotionOp* RootOp = Retargeter->GetFirstRetargetOpOfType<FIKRetargetRootMotionOp>())
	{
		RootOp->Settings.SourceRoot.BoneName = SourceMesh->GetRefSkeleton().GetBoneName(0);
		RootOp->Settings.TargetRoot.BoneName = TargetMesh.GetRefSkeleton().GetBoneName(0);
	}
	// The arms take whichever FK mode keeps the mocap's elbow bend on this body. Interpolated samples the source chain at
	// the same share of its length, so where the arms' proportions differ the elbow takes a blend of the source's upper
	// arm and forearm (the Third's run: 16 degrees straighter); matching the chain's shape fixes that body but straightened
	// Duy's by 16, whose interpolated arms were within 3. Both are tried on a probe clip below. Matched arms are FK only:
	// the hand goals would pull the elbow straight again. The legs keep their IK (planting).
	auto IsArm = [](FName Chain) { return Chain == TEXT("LeftArm") || Chain == TEXT("RightArm"); };
	auto SetArms = [&](EFKChainRotationMode Mode)
	{
		if (FIKRetargetFKChainsOp* FkOp = Retargeter->GetFirstRetargetOpOfType<FIKRetargetFKChainsOp>())
		{
			for (FRetargetFKChainSettings& Chain : FkOp->Settings.ChainsToRetarget)
			{
				if (IsArm(Chain.TargetChainName))
				{
					Chain.RotationMode = Mode;
				}
			}
		}
		if (FIKRetargetRunIKRigOp* IkOp = Retargeter->GetFirstRetargetOpOfType<FIKRetargetRunIKRigOp>())
		{
			for (FIKRetargetRunIKRigChainSettings& Chain : IkOp->Settings.Chains)
			{
				if (IsArm(Chain.TargetChainName))
				{
					Chain.bEnableIK = Mode != EFKChainRotationMode::MatchChain;
				}
			}
		}
	};

	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: retargeter %s: %d ops, %d/%d target chains mapped (%s -> %s)."),
		*Asset->GetName(), Retargeter->GetNumRetargetOps(), Mapped, TargetRig.GetRetargetChains().Num(), *SourceMesh->GetName(),
		*TargetMesh.GetName());

	TArray<UAnimSequence*> Sources;
	for (const FString& Path : Clips)
	{
		UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *Path);
		if (!Clip)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: couldn't load clip %s."), *Path);
			return false;
		}
		Sources.Add(Clip);
	}
	if (Sources.IsEmpty())
	{
		SetArms(EFKChainRotationMode::Interpolated);
		return SaveAsset(Asset) && bOk;
	}
	auto Run = [&](TArrayView<UAnimSequence* const> These)
	{
		FIKRetargetBatchOperationInputs Inputs;
		for (UAnimSequence* Clip : These)
		{
			Inputs.AssetsToRetarget.Add(FAssetData(Clip));
		}
		Inputs.SourceMesh = SourceMesh;
		Inputs.TargetMesh = &TargetMesh;
		Inputs.IKRetargetAsset = Asset;
		Inputs.Suffix = TEXT("_to_") + TargetMesh.GetName();
		Inputs.TargetPath = Dest / TEXT("Retargeted") / TEXT("");
		Inputs.bIncludeReferencedAssets = false;
		Inputs.bOverwriteExistingFiles = true;
		return UIKRetargetBatchOperation::RunBatchRetarget(Inputs);
	};

	// The probe: the run loop if there is one (its elbows bend the most and swing through the whole range), else the first clip.
	UAnimSequence* const* Probe = Sources.FindByPredicate([](const UAnimSequence* Clip) { return Clip->GetName().Contains(TEXT("Run_Loop_F")); });
	Probe = Probe ? Probe : &Sources[0];
	auto ElbowError = [&](EFKChainRotationMode Mode)
	{
		SetArms(Mode);
		const TArray<FAssetData> Out = Run(TArrayView<UAnimSequence* const>(Probe, 1));
		const UAnimSequence* Result = Out.Num() == 1 ? Cast<UAnimSequence>(Out[0].GetAsset()) : nullptr;
		double Error = 0.0;
		for (int32 I = 0; I <= 10 && Result; ++I)
		{
			const FPoseAngles From = MeasurePose(**Probe, I / 10.0), To = MeasurePose(*Result, I / 10.0);
			Error += From.bValid && To.bValid ? FMath::Abs(From.Elbow[0] - To.Elbow[0]) + FMath::Abs(From.Elbow[1] - To.Elbow[1]) : 1e6;
		}
		return Result ? Error / 22.0 : 1e6;
	};
	const double Matched = ElbowError(EFKChainRotationMode::MatchChain);
	const double Interpolated = ElbowError(EFKChainRotationMode::Interpolated);
	const EFKChainRotationMode Arms = Matched < Interpolated ? EFKChainRotationMode::MatchChain : EFKChainRotationMode::Interpolated;
	SetArms(Arms);
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildRig: arms %s (%s elbows off the mocap by %.1f degrees on average, matched %.1f, interpolated %.1f)."),
		Arms == EFKChainRotationMode::MatchChain ? TEXT("match the source chain, FK only") : TEXT("interpolated, with IK"), *(*Probe)->GetName(),
		FMath::Min(Matched, Interpolated), Matched, Interpolated);
	if (!SaveAsset(Asset))
	{
		return false;
	}

	const TArray<FAssetData> Results = Run(Sources);
	if (Results.Num() != Sources.Num())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: retargeting %d clips gave %d results."), Sources.Num(), Results.Num());
		bOk = false;
	}
	for (const FAssetData& Result : Results)
	{
		UAnimSequence* Clip = Cast<UAnimSequence>(Result.GetAsset());
		if (!Clip || !SaveAsset(Clip))
		{
			bOk = false;
			continue;
		}
		if (!LogClip(*Clip, TargetMesh.GetRefSkeleton()))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: retargeted clip %s is empty."), *Clip->GetName());
			bOk = false;
		}
	}
	return bOk;
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
	FString CompareList;
	if (FParse::Value(*Params, TEXT("ComparePose="), CompareList, false))
	{
		int32 Steps = 10;
		FParse::Value(*Params, TEXT("PoseSteps="), Steps);
		return ComparePoses(CompareList, Steps);
	}
	FString MeshPath =TEXT("/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head");
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
	if (!SaveAsset(RetargeterAsset))
	{
		return 1;
	}

	// Optional: retarget clips from another rig onto this mesh.
	FString FromRigPath;
	if (!FParse::Value(*Params, TEXT("RetargetFrom="), FromRigPath))
	{
		return 0;
	}
	UIKRigDefinition* FromRig = LoadObject<UIKRigDefinition>(nullptr, *FromRigPath);
	if (!FromRig)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig: couldn't load the IK Rig %s."), *FromRigPath);
		return 1;
	}
	FString ClipList;
	FParse::Value(*Params, TEXT("Clips="), ClipList, false);
	TArray<FString> Clips;
	ClipList.ParseIntoArray(Clips, TEXT(","));
	FString KeepList;
	// A MetaHuman body (the only skeleton with the pectoral corrective) keeps its spine by default; an explicit
	// -KeepChains= (even empty) wins.
	if (!FParse::Value(*Params, TEXT("KeepChains="), KeepList, false)
		&& Mesh->GetRefSkeleton().FindBoneIndex(TEXT("clavicle_pec_l")) != INDEX_NONE)
	{
		KeepList = TEXT("Pelvis,Spine,Neck,Head");
	}
	TArray<FString> KeepChains;
	KeepList.ParseIntoArray(KeepChains, TEXT(","));
	return RetargetClips(*FromRig, *RigAsset, *Mesh, Dest, Clips, FParse::Param(*Params, TEXT("AlignPose")),
		FParse::Param(*Params, TEXT("NoAlign")), KeepChains) ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildRig needs an editor build."));
	return 1;
#endif
}
