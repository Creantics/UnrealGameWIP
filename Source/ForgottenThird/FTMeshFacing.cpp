#include "FTMeshFacing.h"

#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"

namespace
{
// A bone's reference-pose position in component space, or false when the bone isn't there.
bool RefBonePosition(const FReferenceSkeleton& Skeleton, const TCHAR* Name, FVector& Out)
{
	int32 Bone = Skeleton.FindBoneIndex(Name);
	if (Bone == INDEX_NONE)
	{
		return false;
	}
	FTransform Pose = FTransform::Identity;
	for (; Bone != INDEX_NONE; Bone = Skeleton.GetParentIndex(Bone))
	{
		Pose = Pose * Skeleton.GetRefBonePose()[Bone];
	}
	Out = Pose.GetLocation();
	return true;
}
}

float FTMeshFacing::FacingYaw(const USkeletalMesh& Mesh)
{
	FVector Head, EyeL, EyeR;
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	if (!RefBonePosition(Skeleton, TEXT("head"), Head) || !RefBonePosition(Skeleton, TEXT("eye_l"), EyeL)
		|| !RefBonePosition(Skeleton, TEXT("eye_r"), EyeR))
	{
		return 0.f;
	}
	FVector Forward = (EyeL + EyeR) * 0.5f - Head;
	Forward.Z = 0;
	if (Forward.IsNearlyZero())
	{
		return 0.f;
	}
	return -FMath::RadiansToDegrees(FMath::Atan2(Forward.Y, Forward.X));
}
