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
	FVector Head, EyeL, EyeR, FootL, FootR, BallL, BallR;
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	FVector Forward;
	if (RefBonePosition(Skeleton, TEXT("head"), Head) && RefBonePosition(Skeleton, TEXT("eye_l"), EyeL)
		&& RefBonePosition(Skeleton, TEXT("eye_r"), EyeR))
	{
		Forward = (EyeL + EyeR) * 0.5f - Head;
	}
	else if (RefBonePosition(Skeleton, TEXT("foot_l"), FootL) && RefBonePosition(Skeleton, TEXT("foot_r"), FootR)
		&& RefBonePosition(Skeleton, TEXT("ball_l"), BallL) && RefBonePosition(Skeleton, TEXT("ball_r"), BallR))
	{
		// A MetaHuman body keeps its eyes on the face mesh; the toes point the same way.
		Forward = (BallL + BallR) - (FootL + FootR);
	}
	else
	{
		return 0.f;
	}
	Forward.Z = 0;
	if (Forward.IsNearlyZero())
	{
		return 0.f;
	}
	return -FMath::RadiansToDegrees(FMath::Atan2(Forward.Y, Forward.X));
}
