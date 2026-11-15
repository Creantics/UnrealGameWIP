#include "FTPoseCorrectiveComponent.h"
#include "AnimationRuntime.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ForgottenThird.h"
#include "FTText.h"
#include "ftcore/PoseCorrective.h"

namespace
{
// Data/Anim/pose_correctives.json, read once, with each morph's name made an FName once.
struct FCorrectiveTable
{
	ftcore::PoseCorrectiveTable Table;
	TArray<FName> Morphs;
	TArray<FName> Bones;
	TArray<FName> Children;
};

const FCorrectiveTable& Correctives()
{
	static const FCorrectiveTable Loaded = []
	{
		FCorrectiveTable T;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Anim/pose_correctives.json");
		TArray<uint8> Bytes;
		std::string Error;
		std::vector<std::string> Warnings;
		if (!IFileManager::Get().FileExists(*File) || !FFileHelper::LoadFileToArray(Bytes, *File)
			|| !ftcore::importPoseCorrectives(FT::BytesAsText(Bytes), T.Table, &Error, &Warnings))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Pose correctives: couldn't read %s (%s); no garment correctives."), *File,
				*FT::ToFString(Error));
			T.Table.entries.clear();
		}
		for (const std::string& W : Warnings)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Pose correctives: %s"), *FT::ToFString(W));
		}
		for (const ftcore::PoseCorrective& E : T.Table.entries)
		{
			T.Morphs.Add(FName(*FT::ToFString(E.morph)));
			T.Bones.Add(FName(*FT::ToFString(E.bone)));
			T.Children.Add(FName(*FT::ToFString(E.child)));
		}
		return T;
	}();
	return Loaded;
}

bool HasListedMorph(const USkeletalMesh* Mesh)
{
	if (!Mesh)
	{
		return false;
	}
	for (const FName& Morph : Correctives().Morphs)
	{
		if (Mesh->FindMorphTarget(Morph))
		{
			return true;
		}
	}
	return false;
}

ftcore::Dir3 ToDir(const FVector& V)
{
	return {static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z)};
}
} // namespace

UFTPoseCorrectiveComponent::UFTPoseCorrectiveComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

int32 UFTPoseCorrectiveComponent::Bind(USkeletalMeshComponent& Body, USkeletalMeshComponent& Piece, const TCHAR* Who)
{
	AActor* Owner = Body.GetOwner();
	if (!Owner)
	{
		return 0;
	}
	UFTPoseCorrectiveComponent* Driver = nullptr;
	TInlineComponentArray<UFTPoseCorrectiveComponent*> Existing(Owner);
	for (UFTPoseCorrectiveComponent* C : Existing)
	{
		Driver = C && C->Body == &Body ? C : Driver;
	}
	if (!Driver)
	{
		if (!HasListedMorph(Piece.GetSkeletalMeshAsset()))
		{
			return 0; // nothing to drive and nothing bound before: no component
		}
		Driver = NewObject<UFTPoseCorrectiveComponent>(Owner);
		Driver->Body = &Body;
#if !UE_BUILD_SHIPPING
		float Forced = -1.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("FTPoseCorrectiveWeight="), Forced))
		{
			Driver->ForcedWeight = FMath::Clamp(Forced, 0.f, 1.f);
		}
		Driver->bLog = FParse::Param(FCommandLine::Get(), TEXT("FTPoseCorrectiveLog"));
#endif
		Driver->AddTickPrerequisiteComponent(&Body);
		Driver->RegisterComponent();
	}
	return Driver->BindPiece(Piece, Who);
}

int32 UFTPoseCorrectiveComponent::JointFor(int32 Entry)
{
	for (int32 J = 0; J < Joints.Num(); ++J)
	{
		if (Joints[J].Entry == Entry)
		{
			return J;
		}
	}
	const USkeletalMesh* Mesh = Body ? Body->GetSkeletalMeshAsset() : nullptr;
	if (!Mesh)
	{
		return INDEX_NONE;
	}
	const FCorrectiveTable& T = Correctives();
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	const int32 Bone = Ref.FindBoneIndex(T.Bones[Entry]);
	const int32 Child = Ref.FindBoneIndex(T.Children[Entry]);
	if (Bone == INDEX_NONE || Child == INDEX_NONE)
	{
		return INDEX_NONE;
	}
	FJoint J;
	J.Entry = Entry;
	J.Bone = Bone;
	J.Parent = Ref.GetParentIndex(Bone);
	const FTransform BoneRef = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Bone);
	J.RefParent = J.Parent == INDEX_NONE ? FQuat::Identity : FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, J.Parent).GetRotation();
	J.RefChildLocal = BoneRef.InverseTransformPosition(FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Child).GetLocation());
	J.RefDir = BoneRef.GetRotation().RotateVector(J.RefChildLocal);
	return Joints.Add(J);
}

int32 UFTPoseCorrectiveComponent::BindPiece(USkeletalMeshComponent& Piece, const TCHAR* Who)
{
	Bound.RemoveAll([&Piece](const FBound& B) { return B.Piece.Get() == &Piece || !B.Piece.IsValid(); });
	const USkeletalMesh* Mesh = Piece.GetSkeletalMeshAsset();
	if (!Mesh)
	{
		return 0;
	}
	const FCorrectiveTable& T = Correctives();
	int32 Added = 0;
	for (int32 E = 0; E < T.Morphs.Num(); ++E)
	{
		if (!Mesh->FindMorphTarget(T.Morphs[E]))
		{
			continue;
		}
		const int32 J = JointFor(E);
		if (J == INDEX_NONE)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: piece %s has morph %s but the body has no %s/%s; not driven."), Who,
				*Mesh->GetName(), *T.Morphs[E].ToString(), *T.Bones[E].ToString(), *T.Children[E].ToString());
			continue;
		}
		Bound.Add({&Piece, T.Morphs[E], J, -1.f});
		++Added;
	}
	if (Added > 0)
	{
		// This frame's weight reaches the piece: it updates from its leader in its own tick, after this one.
		Piece.PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
		UE_LOG(LogForgottenThird, Display, TEXT("%s: piece %s: %d pose corrective(s) bound."), Who, *Mesh->GetName(), Added);
	}
	return Added;
}

void UFTPoseCorrectiveComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Body || Bound.IsEmpty())
	{
		return;
	}
	const TArray<FTransform>& Pose = Body->GetComponentSpaceTransforms();
	const ftcore::PoseCorrectiveTable& Table = Correctives().Table;
	LogClock += DeltaTime;
	const bool bLogNow = bLog && LogClock >= 1.f;
	LogClock = bLogNow ? 0.f : LogClock;
	for (FJoint& J : Joints)
	{
		const ftcore::PoseCorrective& E = Table.entries[static_cast<size_t>(J.Entry)];
		if (!Pose.IsValidIndex(J.Bone) || (J.Parent != INDEX_NONE && !Pose.IsValidIndex(J.Parent)))
		{
			J.Weight = 0.f;
			continue;
		}
		// The bone's rotation under its parent, with the parent put back at its reference: pelvis tilt alone is no flexion.
		const FQuat Parent = J.Parent == INDEX_NONE ? FQuat::Identity : Pose[J.Parent].GetRotation();
		const FQuat Local = Parent.Inverse() * Pose[J.Bone].GetRotation();
		const FVector Dir = (J.RefParent * Local).RotateVector(J.RefChildLocal);
		const float Deg = ftcore::flexionDegrees(ToDir(J.RefDir), ToDir(Dir), E.lateral);
		J.Weight = ForcedWeight >= 0.f ? ForcedWeight : ftcore::correctiveWeight(Deg, E.startDeg, E.endDeg);
		if (bLogNow)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Pose corrective %s: flexion %.1f deg, weight %.3f"),
				*Correctives().Morphs[J.Entry].ToString(), Deg, J.Weight);
		}
	}
	for (FBound& B : Bound)
	{
		USkeletalMeshComponent* Piece = B.Piece.Get();
		const float W = Joints[B.Joint].Weight;
		if (!Piece || (FMath::Abs(W - B.Sent) < 1e-3f && (W == 0.f) == (B.Sent == 0.f)))
		{
			continue;
		}
		Piece->SetMorphTarget(B.Morph, W, /*bRemoveZeroWeight=*/true); // at 0 the piece carries no active morph
		B.Sent = W;
	}
}
