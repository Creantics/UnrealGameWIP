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
	bLive = Face && Face->IsCurveMode() && Face->GetCurveNames().Num() == Face->GetCurveValues().Num();
	if (!bLive || Face->GetCurveSerial() == Serial)
	{
		return;
	}
	Serial = Face->GetCurveSerial();
	const TArray<FName>& N = Face->GetCurveNames();
	if (Names != N)
	{
		// A rebind: build the sorted curve once and keep each name's element.
		Names = N;
		Cached.Empty();
		Cached.AppendNames(Names);
		Slots.SetNumUninitialized(Names.Num());
		for (int32 I = 0; I < Names.Num(); ++I)
		{
			Slots[I] = Cached.Find(Names[I]); // the first Find sorts; the rest are binary searches
		}
	}
	const TArray<float>& V = Face->GetCurveValues();
	for (int32 I = 0; I < Slots.Num(); ++I)
	{
		if (Slots[I])
		{
			Slots[I]->Value = V[I];
		}
	}
}

bool FFTFaceCurveAnimProxy::Evaluate(FPoseContext& Output)
{
	CopyPose.Evaluate_AnyThread(Output);
	if (bLive && Cached.Num() > 0)
	{
		Output.Curve.Combine(Cached); // one sorted merge into the memstack-backed output
	}
	return true;
}
