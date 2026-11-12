#include "FTFaceCurveAnimInstance.h"

#include "FTFaceComponent.h"
#include "GameFramework/Actor.h"

UFTFaceComponent* UFTFaceCurveAnimInstance::GetFace()
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

FAnimInstanceProxy* UFTFaceCurveAnimInstance::CreateAnimInstanceProxy()
{
	return new FFTFaceCurveAnimProxy(this);
}

void UFTFaceCurveAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete static_cast<FFTFaceCurveAnimProxy*>(InProxy);
}

FFTFaceCurveAnimProxy::FFTFaceCurveAnimProxy(UAnimInstance* InInstance)
	: FAnimInstanceProxy(InInstance)
{
	CopyPose.bUseAttachedParent = true; // the body the face piece is attached to
	CopyPose.bCopyCurves = false;
}

void FFTFaceCurveAnimProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
	const UFTFaceComponent* Face = static_cast<UFTFaceCurveAnimInstance*>(InAnimInstance)->GetFace();
	if (!Face || !Face->IsCurveMode())
	{
		Values.Reset();
		return;
	}
	const TArray<FName>& N = Face->GetCurveNames();
	if (Names.Num() != N.Num() || (N.Num() > 0 && Names[0] != N[0]))
	{
		Names = N; // a rebind: sized once, not per frame
	}
	Values = Face->GetCurveValues(); // same size as last frame: no allocation
}

bool FFTFaceCurveAnimProxy::Evaluate(FPoseContext& Output)
{
	CopyPose.Evaluate_AnyThread(Output);
	const int32 Count = FMath::Min(Names.Num(), Values.Num());
	for (int32 I = 0; I < Count; ++I)
	{
		Output.Curve.Set(Names[I], Values[I]);
	}
	return true;
}
