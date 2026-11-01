#include "FTBeatFighterAnimInstance.h"

#include "Animation/AnimAttributes.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "AnimationRuntime.h"
#include "BonePose.h"

void UFTBeatFighterAnimInstance::SetPose(const UAnimSequence* Seq, float Time, bool bLoop, float DeltaSeconds)
{
	if (!Seq)
	{
		return;
	}
	if (Slots[0].Seq != Seq)
	{
		// The clip showing becomes the first to fade out; the one that was fading out first, the second.
		if (Slots[0].Seq)
		{
			Slots[2] = Slots[1];
			Slots[1] = Slots[0];
		}
		Slots[0] = FSlot{Seq, Time, Slots[0].Seq ? 0.f : 1.f, bLoop};
	}
	else
	{
		Slots[0].Time = Time;
		Slots[0].bLoop = bLoop;
	}

	// The new clip's weight rises; the others share what is left in the proportion they had.
	const float Step = BlendSeconds > 0.f ? DeltaSeconds / BlendSeconds : 1.f;
	Slots[0].Weight = FMath::Min(1.f, Slots[0].Weight + Step);
	const float Rest = 1.f - Slots[0].Weight;
	const float Old = Slots[1].Weight + Slots[2].Weight;
	for (int32 I = 1; I < FFTBeatFighterProxy::MaxLayers; ++I)
	{
		Slots[I].Weight = Old > 0.f ? Slots[I].Weight / Old * Rest : 0.f;
		if (Slots[I].Weight <= 0.001f)
		{
			Slots[I] = FSlot{};
		}
	}
	if (Slots[1].Seq == nullptr && Slots[2].Seq != nullptr)
	{
		Slots[1] = Slots[2];
		Slots[2] = FSlot{};
	}
}

FAnimInstanceProxy* UFTBeatFighterAnimInstance::CreateAnimInstanceProxy()
{
	return new FFTBeatFighterProxy(this);
}

void UFTBeatFighterAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete static_cast<FFTBeatFighterProxy*>(InProxy);
}

void FFTBeatFighterProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
	const UFTBeatFighterAnimInstance* Instance = static_cast<UFTBeatFighterAnimInstance*>(InAnimInstance);
	NumLayers = 0;
	float Sum = 0.f;
	for (const UFTBeatFighterAnimInstance::FSlot& Slot : Instance->Slots)
	{
		if (Slot.Seq && Slot.Weight > 0.f && NumLayers < MaxLayers)
		{
			Layers[NumLayers++] = FLayer{Slot.Seq, Slot.Time, Slot.Weight, Slot.bLoop};
			Sum += Slot.Weight;
		}
	}
	for (int32 I = 0; I < NumLayers && Sum > 0.f; ++I)
	{
		Layers[I].Weight /= Sum;
	}
	bHoldRoot = Instance->bHoldRoot;
}

bool FFTBeatFighterProxy::Evaluate(FPoseContext& Output)
{
	if (NumLayers == 0)
	{
		Output.ResetToRefPose();
		return true;
	}
	const auto Sample = [](const FLayer& L, FAnimationPoseData& Into)
	{
		L.Seq->GetAnimationPose(Into, FAnimExtractContext(double(L.Time), false, {}, L.bLoop));
	};
	if (NumLayers == 1)
	{
		FAnimationPoseData Out(Output);
		Sample(Layers[0], Out);
	}
	else
	{
		const FBoneContainer& Container = Output.Pose.GetBoneContainer();
		FCompactPose Poses[MaxLayers];
		FBlendedCurve Curves[MaxLayers];
		UE::Anim::FStackAttributeContainer Attributes[MaxLayers];
		float Weights[MaxLayers];
		for (int32 I = 0; I < NumLayers; ++I)
		{
			Poses[I].SetBoneContainer(&Container);
			Curves[I].InitFrom(Output.Curve);
			FAnimationPoseData Data(Poses[I], Curves[I], Attributes[I]);
			Sample(Layers[I], Data);
			Weights[I] = Layers[I].Weight;
		}
		FAnimationPoseData Out(Output);
		FAnimationRuntime::BlendPosesTogether(MakeArrayView(Poses, NumLayers), MakeArrayView(Curves, NumLayers),
			MakeArrayView(Attributes, NumLayers), MakeArrayView(Weights, NumLayers), Out);
	}
	if (bHoldRoot)
	{
		// The host places the actor: the root bone does not travel (its turning stays, the clips' own).
		const FCompactPoseBoneIndex Root(0);
		Output.Pose[Root].SetTranslation(Output.Pose.GetRefPose(Root).GetTranslation());
	}
	return true;
}
