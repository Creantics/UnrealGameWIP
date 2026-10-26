#include "FTLocomotionAnimInstance.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/CommandLine.h"

#include "ForgottenThird.h"

namespace
{
const TCHAR* const SpineNames[FFTLocomotionAnimProxy::NumSpine] = {TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03")};

const TCHAR* ClipName(ftcore::LocoClip C)
{
	switch (C)
	{
	case ftcore::LocoClip::Idle: return TEXT("idle");
	case ftcore::LocoClip::Walk: return TEXT("walk");
	case ftcore::LocoClip::Run: return TEXT("run");
	case ftcore::LocoClip::Sprint: return TEXT("sprint");
	case ftcore::LocoClip::JumpStart: return TEXT("jump");
	case ftcore::LocoClip::Fall: return TEXT("fall");
	case ftcore::LocoClip::Land: return TEXT("land");
	case ftcore::LocoClip::JumpMoving: return TEXT("moving jump");
	case ftcore::LocoClip::LandMoving: return TEXT("moving land");
	default: return TEXT("?");
	}
}

// Samples per second when measuring a gait.
constexpr float kMeasureRate = 60.f;
} // namespace

ftcore::LocoClipInfo UFTLocomotionAnimInstance::MeasureClip(const UAnimSequence& Clip)
{
	const float Length = Clip.GetPlayLength();
	const USkeleton* Skeleton = Clip.GetSkeleton();
	if (!Skeleton || Length <= 0.f)
	{
		return ftcore::measureGait(nullptr, 0, Length);
	}
	const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
	auto Find = [&Ref](const TCHAR* A, const TCHAR* B)
	{
		const int32 I = Ref.FindBoneIndex(A);
		return I != INDEX_NONE ? I : Ref.FindBoneIndex(B);
	};
	const int32 Left = Find(TEXT("ball_l"), TEXT("foot_l"));
	const int32 Right = Find(TEXT("ball_r"), TEXT("foot_r"));
	if (Left == INDEX_NONE || Right == INDEX_NONE)
	{
		return ftcore::measureGait(nullptr, 0, Length);
	}

	// A bone's transform in the clip's component space, composed up its parent chain.
	auto ComponentSpace = [&](int32 Bone, double Time)
	{
		FTransform CS = FTransform::Identity;
		const FAnimExtractContext Context(Time, false);
		for (int32 B = Bone; B != INDEX_NONE; B = Ref.GetParentIndex(B))
		{
			FTransform Local;
			Clip.GetBoneTransform(Local, FSkeletonPoseBoneIndex(B), Context, false);
			CS = CS * Local;
		}
		return CS;
	};
	auto Point = [](const FVector& V) { return ftcore::LocoPoint{float(V.X), float(V.Y), float(V.Z)}; };

	const int32 N = FMath::Clamp(FMath::CeilToInt(Length * kMeasureRate) + 1, 3, static_cast<int32>(ftcore::kMaxGaitFrames));
	TArray<ftcore::GaitFrame> Frames;
	Frames.SetNum(N);
	for (int32 K = 0; K < N; ++K)
	{
		const double Time = double(Length) * K / (N - 1);
		Frames[K].root = Point(ComponentSpace(0, Time).GetTranslation());
		Frames[K].left = Point(ComponentSpace(Left, Time).GetTranslation());
		Frames[K].right = Point(ComponentSpace(Right, Time).GetTranslation());
	}
	return ftcore::measureGait(Frames.GetData(), Frames.Num(), Length);
}

void UFTLocomotionAnimInstance::SetClip(ftcore::LocoClip Which, UAnimSequence* Clip)
{
	if (Which == ftcore::LocoClip::Count)
	{
		return;
	}
	Clips[static_cast<int32>(Which)] = Clip;
	ftcore::LocoClipInfo Info;
	if (Clip)
	{
		const bool bGait = ftcore::isLocoGait(Which);
		Info = bGait ? MeasureClip(*Clip) : ftcore::LocoClipInfo{Clip->GetPlayLength(), 0.f, 1, 0.f};
		// The root's own travel over the loop, when the clip has root motion: a check on the ground speed.
		FTransform Start, End;
		Clip->GetBoneTransform(Start, FSkeletonPoseBoneIndex(0), FAnimExtractContext(0.0, false), false);
		Clip->GetBoneTransform(End, FSkeletonPoseBoneIndex(0), FAnimExtractContext(double(Info.length), false), false);
		const float RootSpeed = Info.length > 0.f ? float(FVector::Dist2D(Start.GetTranslation(), End.GetTranslation())) / Info.length : 0.f;
		// A moving jump or landing plays at the rate its root's travel matches the capsule's.
		if (Which == ftcore::LocoClip::JumpMoving || Which == ftcore::LocoClip::LandMoving)
		{
			Info.groundSpeed = RootSpeed;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Locomotion: %s = %s, %.2f s%s"), ClipName(Which), *Clip->GetName(), Info.length,
			bGait ? *FString::Printf(TEXT(", ground speed %.0f cm/s (root moves %.0f, feet %.0f, %.0f sideways), %d strides, left plant at %.2f"),
						Info.groundSpeed, RootSpeed, Info.footSpeed, Info.footSide, Info.cycles, Info.leftPlant)
				  : *FString::Printf(TEXT(", root moves %.0f cm/s"), RootSpeed));
	}
	Loco.setClip(Which, Info);
	Loco.reset();
}

void UFTLocomotionAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	Loco.reset();
	bHasYaw = false;
	bLog = FParse::Param(FCommandLine::Get(), TEXT("LocoLog"));
	if (FParse::Param(FCommandLine::Get(), TEXT("LocoSlide")))
	{
		// The probe reads bone positions, which an unrendered mesh (-NullRHI) otherwise leaves stale.
		if (USkeletalMeshComponent* Mesh = GetSkelMeshComponent())
		{
			Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		}
	}
}

FAnimInstanceProxy* UFTLocomotionAnimInstance::CreateAnimInstanceProxy()
{
	return new FFTLocomotionAnimProxy(this);
}

void UFTLocomotionAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete static_cast<FFTLocomotionAnimProxy*>(InProxy);
}

void UFTLocomotionAnimInstance::Step(float DeltaSeconds)
{
	const AActor* Owner = GetOwningActor();
	if (!Owner)
	{
		return;
	}
	ftcore::LocoInput In;
	In.dt = DeltaSeconds;
	if (const ACharacter* Character = Cast<ACharacter>(Owner))
	{
		const FVector V = Character->GetVelocity();
		In.groundSpeed = static_cast<float>(V.Size2D());
		In.verticalSpeed = static_cast<float>(V.Z);
		const UCharacterMovementComponent* Move = Character->GetCharacterMovement();
		In.falling = Move && Move->IsFalling();
	}
	else
	{
		In.groundSpeed = PushedSpeed;
	}
	const float Yaw = static_cast<float>(Owner->GetActorRotation().Yaw);
	In.turnRate = bHasYaw && DeltaSeconds > 0.f ? FMath::FindDeltaAngleDegrees(LastYaw, Yaw) / DeltaSeconds : 0.f;
	LastYaw = Yaw;
	bHasYaw = true;
	Loco.update(In);

	if (const USkeletalMeshComponent* Mesh = GetSkelMeshComponent())
	{
		ForwardCS = Mesh->GetComponentTransform().InverseTransformVectorNoScale(Owner->GetActorForwardVector()).GetSafeNormal2D();
		if (ForwardCS.IsNearlyZero())
		{
			ForwardCS = FVector::ForwardVector;
		}
	}

	if (bLog && (LogTimer -= DeltaSeconds) <= 0.f)
	{
		LogTimer = 1.f;
		FString Layers;
		for (std::size_t I = 0; I < Loco.layerCount(); ++I)
		{
			const ftcore::LocoLayer& L = Loco.layer(I);
			Layers += FString::Printf(TEXT(" %s %.2f@x%.2f"), ClipName(L.clip), L.weight, Loco.rate(L.clip));
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Locomotion %s: speed %.0f turn %.0f falling %d state %d lean %.1f twist %.1f pitch %.1f |%s"),
			*Owner->GetName(), In.groundSpeed, In.turnRate, In.falling ? 1 : 0, static_cast<int32>(Loco.state()), Loco.lean(), Loco.twist(),
			Loco.pitch(), *Layers);
	}
}

void FFTFootSlideProbe::Sample(const USkeletalMeshComponent& Mesh, float DeltaSeconds, float BodySpeed)
{
	if (!bChecked)
	{
		bEnabled = FParse::Param(FCommandLine::Get(), TEXT("LocoSlide"));
		bChecked = true;
	}
	if (!bEnabled || DeltaSeconds <= 0.f)
	{
		return;
	}
	// In the air no foot is planted: drop the plant under way and start again on landing.
	const ACharacter* Character = Cast<ACharacter>(Mesh.GetOwner());
	if (Character && Character->GetCharacterMovement() && Character->GetCharacterMovement()->IsFalling())
	{
		PlantFoot = INDEX_NONE;
		bHasLast = false;
		return;
	}
	static const FName Balls[2] = {TEXT("ball_l"), TEXT("ball_r")};
	FVector Now[2];
	for (int32 I = 0; I < 2; ++I)
	{
		Now[I] = Mesh.GetBoneLocation(Balls[I], EBoneSpaces::WorldSpace);
	}
	// The planted foot is the slower of the feet near the floor: in a clean stride it stands still while
	// the other swings, so its speed over the ground is the slide.
	const FTransform& ToWorld = Mesh.GetComponentTransform();
	int32 Foot = INDEX_NONE;
	float Slowest = 0.f;
	for (int32 I = 0; I < 2; ++I)
	{
		const float Height = static_cast<float>(ToWorld.InverseTransformPosition(Now[I]).Z);
		NextFloor = FMath::Min(NextFloor, Height);
		const float Speed = static_cast<float>(FVector::Dist2D(Now[I], Last[I]) / DeltaSeconds);
		if (bHasLast && Floor < 1e8f && Height < Floor + 3.f && (Foot == INDEX_NONE || Speed < Slowest))
		{
			Foot = I;
			Slowest = Speed;
		}
	}
	if (Foot != PlantFoot)
	{
		// A plant ends: its net slip, start to end, leaves out the frame-to-frame jitter.
		if (PlantFoot != INDEX_NONE)
		{
			const FVector Slip = Last[PlantFoot] - PlantStart;
			const FVector Fwd = Mesh.GetOwner() ? Mesh.GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
			SlipSum += Slip.Size2D();
			AlongSum += Slip.X * Fwd.X + Slip.Y * Fwd.Y;
			SideSum += Slip.Y * Fwd.X - Slip.X * Fwd.Y;
			SlipTime += PlantTime;
		}
		PlantFoot = Foot;
		PlantTime = 0.f;
		if (Foot != INDEX_NONE)
		{
			PlantStart = Now[Foot];
		}
	}
	if (Foot != INDEX_NONE)
	{
		PlantTime += DeltaSeconds;
		SlideSum += Slowest;
		++Planted;
	}
	SpeedSum += BodySpeed;
	++Frames;
	Last[0] = Now[0];
	Last[1] = Now[1];
	bHasLast = true;
	if ((Timer += DeltaSeconds) >= 1.f)
	{
		UE_LOG(LogForgottenThird, Display,
			TEXT("LocoSlide %s: body %.0f cm/s, planted foot slips %.1f cm/s net (%.1f forward, %.1f right), %.1f cm/s frame to frame, over %d of %d frames"),
			Mesh.GetOwner() ? *Mesh.GetOwner()->GetName() : TEXT("?"), Frames ? SpeedSum / Frames : 0.0, SlipTime > 0.0 ? SlipSum / SlipTime : 0.0,
			SlipTime > 0.0 ? AlongSum / SlipTime : 0.0, SlipTime > 0.0 ? SideSum / SlipTime : 0.0, Planted ? SlideSum / Planted : 0.0, Planted,
			Frames);
		Floor = NextFloor;
		NextFloor = 1e9f;
		SlideSum = SpeedSum = AlongSum = SideSum = SlipSum = SlipTime = 0.0;
		Planted = Frames = 0;
		Timer = 0.f;
	}
}

void FFTLocomotionAnimProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);

	UFTLocomotionAnimInstance* Instance = static_cast<UFTLocomotionAnimInstance*>(InAnimInstance);
	Instance->Step(DeltaSeconds);
	const ftcore::Locomotion& Loco = Instance->Loco;
	NumLayers = 0;
	for (std::size_t I = 0; I < Loco.layerCount(); ++I)
	{
		const ftcore::LocoLayer& L = Loco.layer(I);
		const UAnimSequence* Seq = Instance->Clips[static_cast<int32>(L.clip)];
		if (Seq)
		{
			Layers[NumLayers++] = {Seq, L.time, L.weight, ftcore::isLocoLoop(L.clip)};
		}
	}

	// Lean rolls the upper body about the forward axis into the turn (a positive turn rate turns
	// right, toward +Y), twist yaws it toward the turn, pitch tips it forward.
	const FVector F = Instance->ForwardCS;
	const FVector Right(-F.Y, F.X, 0.0);
	const FQuat Lean(F, FMath::DegreesToRadians(-Loco.lean()));
	const FQuat Pitch(Right, FMath::DegreesToRadians(Loco.pitch()));
	const FQuat Twist(FVector::UpVector, FMath::DegreesToRadians(Loco.twist()));
	SpineBend = Twist * Pitch * Lean;
}

bool FFTLocomotionAnimProxy::Evaluate(FPoseContext& Output)
{
	if (NumLayers == 0)
	{
		Output.ResetToRefPose();
		return true;
	}

	auto Sample = [](const FLayer& L, FAnimationPoseData& Into)
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

	const FBoneContainer& Container = Output.Pose.GetBoneContainer();
	// The capsule carries the body: hold the root where the ref pose has it.
	const FCompactPoseBoneIndex Root(0);
	Output.Pose[Root].SetTranslation(Output.Pose.GetRefPose(Root).GetTranslation());

	if (!bBound || BoundSerial != Container.GetSerialNumber())
	{
		for (int32 I = 0; I < NumSpine; ++I)
		{
			Spine[I].BoneName = SpineNames[I];
			Spine[I].Initialize(Container);
		}
		BoundSerial = Container.GetSerialNumber();
		bBound = true;
	}
	int32 Valid = 0;
	for (const FBoneReference& B : Spine)
	{
		Valid += B.IsValidToEvaluate(Container) ? 1 : 0;
	}
	if (Valid == 0 || SpineBend.Equals(FQuat::Identity, 1e-5))
	{
		return true;
	}
	// Each spine bone turns by an equal share of the bend about its own origin, in component space;
	// the bones above it follow.
	const FQuat Share = FQuat::Slerp(FQuat::Identity, SpineBend, 1.f / static_cast<float>(Valid));
	FCSPose<FCompactPose> CS;
	// A copy (on the anim stack): the conversion back writes into Output.Pose before taking CS's.
	CS.InitPose(Output.Pose);
	for (const FBoneReference& B : Spine)
	{
		if (!B.IsValidToEvaluate(Container))
		{
			continue;
		}
		const FCompactPoseBoneIndex Index = B.GetCompactPoseIndex(Container);
		FTransform T = CS.GetComponentSpaceTransform(Index);
		T.SetRotation((Share * T.GetRotation()).GetNormalized());
		CS.SetComponentSpaceTransform(Index, T);
	}
	FCSPose<FCompactPose>::ConvertComponentPosesToLocalPoses(MoveTemp(CS), Output.Pose);
	return true;
}
