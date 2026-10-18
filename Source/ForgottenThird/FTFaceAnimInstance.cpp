#include "FTFaceAnimInstance.h"

#include "FTFaceComponent.h"
#include "ForgottenThird.h"
#include "GameFramework/Actor.h"

namespace
{
	const TCHAR* const BoneNames[FFTFaceAnimProxy::NumBones] = {
		TEXT("jaw"), TEXT("tongue_01"), TEXT("tongue_02"), TEXT("tongue_03"),
	};
}

UFTFaceComponent* UFTFaceAnimInstance::GetFace()
{
	if (!Face.IsValid())
	{
		if (AActor* Owner = GetOwningActor())
		{
			Face = Owner->FindComponentByClass<UFTFaceComponent>();
		}
	}
	return Face.Get();
}

FAnimInstanceProxy* UFTFaceAnimInstance::CreateAnimInstanceProxy()
{
	return new FFTFaceAnimProxy(this);
}

void UFTFaceAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete static_cast<FFTFaceAnimProxy*>(InProxy);
}

void FFTFaceAnimProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);

	const UFTFaceComponent* Face = static_cast<UFTFaceAnimInstance*>(InAnimInstance)->GetFace();
	if (!Face)
	{
		for (FQuat& Q : Delta)
		{
			Q = FQuat::Identity;
		}
		return;
	}
	Delta[0] = Face->GetJawRotation().Quaternion();
	for (int32 i = 1; i < NumBones; ++i)
	{
		Delta[i] = Face->GetTongueRotation(i - 1).Quaternion();
	}
}

bool FFTFaceAnimProxy::Evaluate(FPoseContext& Output)
{
	Output.ResetToRefPose();

	const FBoneContainer& Container = Output.Pose.GetBoneContainer();
	if (!bBound || BoundSerial != Container.GetSerialNumber())
	{
		for (int32 i = 0; i < NumBones; ++i)
		{
			Bones[i].BoneName = BoneNames[i];
			Bones[i].Initialize(Container);
			if (!Bones[i].IsValidToEvaluate(Container) && !bLoggedMissing[i])
			{
				bLoggedMissing[i] = true;
				UE_LOG(LogForgottenThird, Warning, TEXT("Face anim: bone '%s' not found on the skeleton; skipped."),
					BoneNames[i]);
			}
		}
		BoundSerial = Container.GetSerialNumber();
		bBound = true;
	}

	for (int32 i = 0; i < NumBones; ++i)
	{
		if (Bones[i].IsValidToEvaluate(Container))
		{
			FTransform& T = Output.Pose[Bones[i].GetCompactPoseIndex(Container)];
			T.SetRotation((T.GetRotation() * Delta[i]).GetNormalized());
		}
	}
	return true;
}
