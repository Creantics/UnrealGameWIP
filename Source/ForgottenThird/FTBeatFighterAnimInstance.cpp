#include "FTBeatFighterAnimInstance.h"

#include "Animation/AnimAttributes.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AttributesRuntime.h"
#include "AnimationRuntime.h"
#include "BonePose.h"

void UFTBeatFighterAnimInstance::SetPose(const UAnimSequence* Seq, float Time, bool bLoop, float DeltaSeconds, float Blend, bool bAdditiveIn)
{
	if (!Seq)
	{
		return;
	}
	// The same clip begun again fades in over itself where the owner asks (SetFadeRestart), as a new clip does.
	const bool bRestart = bFadeRestart && Slots[0].Seq == Seq && !bLoop && Time < Slots[0].Time - 0.05f;
	if (Slots[0].Seq != Seq || bRestart)
	{
		// The clip showing becomes the first to fade out; the one that was fading out first, the second.
		if (Slots[0].Seq)
		{
			// An additive clip cut off mid-fade keeps the share of its motion it was showing (1 - its weight), from now on
			// in proportion to its weight, so the share goes as the clip fades out.
			Slots[0].Additive = Slots[0].bAdditiveIn && Slots[0].Weight > 0.001f ? (1.f - Slots[0].Weight) / Slots[0].Weight : 0.f;
			Slots[2] = Slots[1];
			Slots[1] = Slots[0];
		}
		Slots[0] = FSlot{Seq, Time, Slots[0].Seq ? 0.f : 1.f, bLoop, bAdditiveIn};
	}
	else
	{
		Slots[0].Time = Time;
		Slots[0].bLoop = bLoop;
	}

	// The new clip's weight rises; the others share what is left in the proportion they had.
	const float Step = Blend > 0.f ? DeltaSeconds / Blend : 1.f;
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
	for (int32 I = 0; I < MaxLayers; ++I)
	{
		const UFTBeatFighterAnimInstance::FSlot& Slot = Instance->Slots[I];
		if (Slot.Seq && Slot.Weight > 0.f && NumLayers < MaxLayers)
		{
			// The clip fading in puts on what is left of its fade; one faded out of, its kept share by its weight.
			const float Additive = I == 0 ? (Slot.bAdditiveIn ? 1.f - Slot.Weight : 0.f) : Slot.Additive * Slot.Weight;
			Layers[NumLayers++] = FLayer{Slot.Seq, Slot.Time, Slot.Weight, Slot.bLoop, FMath::Clamp(Additive, 0.f, 1.f)};
			Sum += Slot.Weight;
		}
	}
	for (int32 I = 0; I < NumLayers && Sum > 0.f; ++I)
	{
		Layers[I].Weight /= Sum;
	}
	bHoldRoot = Instance->bHoldRoot;
	BuildChest = Instance->BuildChest;
	BuildHead = Instance->BuildHead;
	BuildHands = Instance->BuildHands;
	Lean = Instance->Lean;
	GuardDown = Instance->GuardDown;
	ArmDropR = Instance->ArmDropR;
	ArmDropL = Instance->ArmDropL;
}

// The boss's mass and swing, put on the blended pose.
void FFTBeatFighterProxy::ApplyBuild(FPoseContext& Output) const
{
	const FBoneContainer& Container = Output.Pose.GetBoneContainer();
	const auto Find = [&Container](const TCHAR* Name)
	{
		const int32 Mesh = Container.GetPoseBoneIndexForBoneName(FName(Name));
		return Mesh == INDEX_NONE ? FCompactPoseBoneIndex(INDEX_NONE) : Container.MakeCompactPoseIndex(FMeshPoseBoneIndex(Mesh));
	};
	const FCompactPoseBoneIndex Chest = Find(TEXT("spine_03"));
	const FCompactPoseBoneIndex Head = Find(TEXT("head"));
	const FCompactPoseBoneIndex HandR = Find(TEXT("hand_r"));
	const FCompactPoseBoneIndex HandL = Find(TEXT("hand_l"));

	// The lean first: the upper body pitched about the line across the hips, in component space, so every clip
	// swings the same way whatever its own bone axes are. Forward is across x up, as the host finds it.
	if (FMath::Abs(Lean) > 0.01f && Chest.IsValid())
	{
		const FCompactPoseBoneIndex ThighR = Find(TEXT("thigh_r"));
		const FCompactPoseBoneIndex ThighL = Find(TEXT("thigh_l"));
		if (ThighR.IsValid() && ThighL.IsValid())
		{
			FCSPose<FCompactPose> Component;
			Component.InitPose(Output.Pose);
			FVector Across = Component.GetComponentSpaceTransform(ThighR).GetLocation() - Component.GetComponentSpaceTransform(ThighL).GetLocation();
			Across.Z = 0.0;
			if (Across.Normalize())
			{
				FTransform Spine = Component.GetComponentSpaceTransform(Chest);
				Spine.SetRotation((FQuat(Across, FMath::DegreesToRadians(static_cast<double>(Lean))) * Spine.GetRotation()).GetNormalized());
				Component.SetComponentSpaceTransform(Chest, Spine);
				FCSPose<FCompactPose>::ConvertComponentPosesToLocalPoses(Component, Output.Pose);
			}
		}
	}

	// The guard comes down: from hands held at the face to a guard at the chest. Each upper arm swings down a little, and
	// each elbow opens about its own hinge (the line across the arm's bend), in component space, the same for any clip.
	if (GuardDown > 0.001f)
	{
		FCSPose<FCompactPose> Component;
		Component.InitPose(Output.Pose);
		for (const TCHAR* Side : {TEXT("l"), TEXT("r")})
		{
			const FCompactPoseBoneIndex Upper = Find(*FString::Printf(TEXT("upperarm_%s"), Side));
			const FCompactPoseBoneIndex Lower = Find(*FString::Printf(TEXT("lowerarm_%s"), Side));
			const FCompactPoseBoneIndex Hand = Find(*FString::Printf(TEXT("hand_%s"), Side));
			if (!Upper.IsValid() || !Lower.IsValid() || !Hand.IsValid())
			{
				continue;
			}
			// The upper arm's direction (shoulder to elbow) swung towards straight down.
			FTransform UpperCS = Component.GetComponentSpaceTransform(Upper);
			const FVector ToElbow = UpperCS.GetRotation().RotateVector(Output.Pose[Lower].GetTranslation()).GetSafeNormal();
			const FVector Down(0.0, 0.0, -1.0);
			FVector Swing = FVector::CrossProduct(ToElbow, Down);
			if (Swing.Normalize())
			{
				UpperCS.SetRotation((FQuat(Swing, FMath::DegreesToRadians(24.0 * static_cast<double>(GuardDown))) * UpperCS.GetRotation()).GetNormalized());
				Component.SetComponentSpaceTransform(Upper, UpperCS);
			}
			// The elbow: the forearm turned about the hinge towards the upper arm's line (straighter).
			FTransform LowerCS = Component.GetComponentSpaceTransform(Lower);
			const FVector Upward = UpperCS.GetRotation().RotateVector(Output.Pose[Lower].GetTranslation()).GetSafeNormal();
			const FVector Forearm = LowerCS.GetRotation().RotateVector(Output.Pose[Hand].GetTranslation()).GetSafeNormal();
			FVector Hinge = FVector::CrossProduct(Upward, Forearm);
			if (Hinge.Normalize())
			{
				LowerCS.SetRotation((FQuat(Hinge, FMath::DegreesToRadians(-48.0 * static_cast<double>(GuardDown))) * LowerCS.GetRotation()).GetNormalized());
				Component.SetComponentSpaceTransform(Lower, LowerCS);
			}
		}
		FCSPose<FCompactPose>::ConvertComponentPosesToLocalPoses(Component, Output.Pose);
	}

	// A blow aimed lower: the upper arm swung down about the line across it, so the fist of a swing made at the boss's own
	// height comes down onto an opponent who stands a head shorter.
	if (ArmDropR > 0.01f || ArmDropL > 0.01f)
	{
		FCSPose<FCompactPose> Component;
		Component.InitPose(Output.Pose);
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const float Degrees = Side == 0 ? ArmDropR : ArmDropL;
			if (Degrees <= 0.01f)
			{
				continue;
			}
			const FCompactPoseBoneIndex Upper = Find(Side == 0 ? TEXT("upperarm_r") : TEXT("upperarm_l"));
			const FCompactPoseBoneIndex Lower = Find(Side == 0 ? TEXT("lowerarm_r") : TEXT("lowerarm_l"));
			if (!Upper.IsValid() || !Lower.IsValid())
			{
				continue;
			}
			FTransform UpperCS = Component.GetComponentSpaceTransform(Upper);
			const FVector ToElbow = UpperCS.GetRotation().RotateVector(Output.Pose[Lower].GetTranslation()).GetSafeNormal();
			FVector Swing = FVector::CrossProduct(ToElbow, FVector(0.0, 0.0, -1.0));
			if (Swing.Normalize())
			{
				UpperCS.SetRotation((FQuat(Swing, FMath::DegreesToRadians(static_cast<double>(Degrees))) * UpperCS.GetRotation()).GetNormalized());
				Component.SetComponentSpaceTransform(Upper, UpperCS);
			}
		}
		FCSPose<FCompactPose>::ConvertComponentPosesToLocalPoses(Component, Output.Pose);
	}

	// The build: net sizes, so each bone's own scale is its size over its parent's.
	const auto Scale = [&Output](const FCompactPoseBoneIndex Bone, float Local)
	{
		if (Bone.IsValid() && FMath::Abs(Local - 1.f) > 0.001f)
		{
			Output.Pose[Bone].SetScale3D(Output.Pose[Bone].GetScale3D() * static_cast<double>(Local));
		}
	};
	Scale(Chest, BuildChest);
	Scale(Head, BuildChest > 0.f ? BuildHead / BuildChest : 1.f);
	Scale(HandR, BuildChest > 0.f ? BuildHands / BuildChest : 1.f);
	Scale(HandL, BuildChest > 0.f ? BuildHands / BuildChest : 1.f);
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
	// An additive fade-in: the clip's motion since its own first frame, put on top of the blend (local space).
	for (int32 I = 0; I < NumLayers; ++I)
	{
		const FLayer& L = Layers[I];
		if (L.Additive <= 0.001f)
		{
			continue;
		}
		const FBoneContainer& Container = Output.Pose.GetBoneContainer();
		FCompactPose Motion, First;
		FBlendedCurve MotionCurve, FirstCurve;
		UE::Anim::FStackAttributeContainer MotionAttributes, FirstAttributes;
		Motion.SetBoneContainer(&Container);
		First.SetBoneContainer(&Container);
		MotionCurve.InitFrom(Output.Curve);
		FirstCurve.InitFrom(Output.Curve);
		FAnimationPoseData MotionData(Motion, MotionCurve, MotionAttributes);
		FAnimationPoseData FirstData(First, FirstCurve, FirstAttributes);
		Sample(L, MotionData);
		L.Seq->GetAnimationPose(FirstData, FAnimExtractContext(0.0, false, {}, L.bLoop));
		FAnimationRuntime::ConvertPoseToAdditive(Motion, First);
		MotionCurve.ConvertToAdditive(FirstCurve);
		UE::Anim::Attributes::ConvertToAdditive(FirstAttributes, MotionAttributes);
		FAnimationPoseData Out(Output);
		FAnimationRuntime::AccumulateAdditivePose(Out, MotionData, L.Additive, AAT_LocalSpaceBase);
		Output.Pose.NormalizeRotations();
	}
	if (bHoldRoot)
	{
		// The host places the actor: the root bone does not travel (its turning stays, the clips' own).
		const FCompactPoseBoneIndex Root(0);
		Output.Pose[Root].SetTranslation(Output.Pose.GetRefPose(Root).GetTranslation());
	}
	if (FMath::Abs(BuildChest - 1.f) > 0.001f || FMath::Abs(BuildHead - 1.f) > 0.001f || FMath::Abs(BuildHands - 1.f) > 0.001f || FMath::Abs(Lean) > 0.01f
		|| GuardDown > 0.001f || ArmDropR > 0.01f || ArmDropL > 0.01f)
	{
		ApplyBuild(Output);
	}
	return true;
}
