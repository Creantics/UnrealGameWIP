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
#include "Misc/PackageName.h"

#include "ForgottenThird.h"

namespace
{
const TCHAR* const SpineNames[FFTLocomotionAnimProxy::NumSpine] = {TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03")};

// A turn clip's side ("L" or "R"), angle and, for a running turn, foot.
struct FTurnParts
{
	const TCHAR* Side;
	int32 Angle;
	const TCHAR* Foot;
};
FTurnParts TurnParts(ftcore::LocoClip C)
{
	const int32 I = static_cast<int32>(C);
	auto Side = [](bool bRight) { return bRight ? TEXT("R") : TEXT("L"); };
	if (ftcore::isLocoRunTurn(C))
	{
		const int32 K = I - static_cast<int32>(ftcore::LocoClip::RunTurnLeft45L);
		return {Side(K >= 8), ftcore::kLocoTurnAngles[(K % 8) / 2], Side(K % 2 == 1)};
	}
	if (ftcore::isLocoReface(C))
	{
		const int32 K = I - static_cast<int32>(ftcore::LocoClip::RefaceLeft90);
		return {Side(K >= 2), ftcore::kLocoRefaceAngles[K % 2], TEXT("")};
	}
	const int32 K = I - static_cast<int32>(ftcore::LocoClip::StandTurnLeft45);
	return {Side(K >= 4), ftcore::kLocoTurnAngles[K % 4], TEXT("")};
}

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
	case ftcore::LocoClip::WalkStart: return TEXT("walk start");
	case ftcore::LocoClip::RunStart: return TEXT("run start");
	case ftcore::LocoClip::SprintStart: return TEXT("sprint start");
	case ftcore::LocoClip::WalkStopLeft: return TEXT("walk stop L");
	case ftcore::LocoClip::WalkStopRight: return TEXT("walk stop R");
	case ftcore::LocoClip::RunStopLeft: return TEXT("run stop L");
	case ftcore::LocoClip::RunStopRight: return TEXT("run stop R");
	case ftcore::LocoClip::SprintStopLeft: return TEXT("sprint stop L");
	case ftcore::LocoClip::SprintStopRight: return TEXT("sprint stop R");
	default: break;
	}
	// The turns, named once: "run turn L90 Rfoot", "reface R180", "stand turn L45".
	static const TArray<FString> Turns = []
	{
		TArray<FString> Names;
		for (int32 I = static_cast<int32>(ftcore::LocoClip::RunTurnLeft45L); I < static_cast<int32>(ftcore::LocoClip::Count); ++I)
		{
			const ftcore::LocoClip Clip = static_cast<ftcore::LocoClip>(I);
			const FTurnParts T = TurnParts(Clip);
			Names.Add(ftcore::isLocoRunTurn(Clip) ? FString::Printf(TEXT("run turn %s%d %sfoot"), T.Side, T.Angle, T.Foot)
				: ftcore::isLocoReface(Clip) ? FString::Printf(TEXT("reface %s%d"), T.Side, T.Angle)
				: FString::Printf(TEXT("stand turn %s%d"), T.Side, T.Angle));
		}
		return Names;
	}();
	const int32 Turn = static_cast<int32>(C) - static_cast<int32>(ftcore::LocoClip::RunTurnLeft45L);
	return Turns.IsValidIndex(Turn) ? *Turns[Turn] : TEXT("?");
}

// Samples per second when measuring a gait.
constexpr float kMeasureRate = 60.f;

// The clip's root and ball (or foot) bones in its component space, sampled evenly from 0 to its
// length into Frames. False (and no frames) when the clip has no skeleton, length or feet.
bool SampleFeet(const UAnimSequence& Clip, TArray<ftcore::GaitFrame>& Frames)
{
	const float Length = Clip.GetPlayLength();
	const USkeleton* Skeleton = Clip.GetSkeleton();
	if (!Skeleton || Length <= 0.f)
	{
		return false;
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
		return false;
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
	Frames.SetNum(N);
	for (int32 K = 0; K < N; ++K)
	{
		const double Time = double(Length) * K / (N - 1);
		Frames[K].root = Point(ComponentSpace(0, Time).GetTranslation());
		Frames[K].left = Point(ComponentSpace(Left, Time).GetTranslation());
		Frames[K].right = Point(ComponentSpace(Right, Time).GetTranslation());
		Frames[K].yaw = float(ComponentSpace(0, Time).Rotator().Yaw);
	}
	return true;
}
} // namespace

ftcore::LocoClipInfo UFTLocomotionAnimInstance::MeasureClip(const UAnimSequence& Clip)
{
	TArray<ftcore::GaitFrame> Frames;
	SampleFeet(Clip, Frames);
	return ftcore::measureGait(Frames.GetData(), Frames.Num(), Clip.GetPlayLength());
}

ftcore::LocoClipInfo UFTLocomotionAnimInstance::MeasureMove(const UAnimSequence& Clip)
{
	TArray<ftcore::GaitFrame> Frames;
	SampleFeet(Clip, Frames);
	return ftcore::measureMove(Frames.GetData(), Frames.Num(), Clip.GetPlayLength());
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
		const bool bMove = ftcore::isLocoStart(Which) || ftcore::isLocoStop(Which) || ftcore::isLocoTurn(Which);
		Info = bGait ? MeasureClip(*Clip) : bMove ? MeasureMove(*Clip) : ftcore::LocoClipInfo{Clip->GetPlayLength(), 0.f, 1, 0.f};
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
		FString Detail;
		if (bGait)
		{
			Detail = FString::Printf(TEXT(", ground speed %.0f cm/s (root moves %.0f, feet %.0f, %.0f sideways), %d strides, left plant at %.2f"),
				Info.groundSpeed, RootSpeed, Info.footSpeed, Info.footSide, Info.cycles, Info.leftPlant);
		}
		else if (bMove)
		{
			Detail = FString::Printf(TEXT(", root travels %.0f cm in %.2f s"), Info.travel, Info.moveTime);
			if (ftcore::isLocoTurn(Which))
			{
				Detail += FString::Printf(TEXT(", turns %.0f deg from %.2f to %.2f s"), Info.turn, Info.turnStart, Info.turnEnd);
			}
		}
		else
		{
			Detail = FString::Printf(TEXT(", root moves %.0f cm/s"), RootSpeed);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Locomotion: %s = %s, %.2f s%s"), ClipName(Which), *Clip->GetName(), Info.length, *Detail);
	}
	Loco.setClip(Which, Info);
	Loco.reset();
}

void UFTLocomotionAnimInstance::SetMovesFromGait(ftcore::LocoClip Gait)
{
	// -LocoNoMoves leaves them out, for comparing with the gaits alone.
	static const bool bNoMoves = FParse::Param(FCommandLine::Get(), TEXT("LocoNoMoves"));
	const UAnimSequence* Loop = ftcore::isLocoGait(Gait) && !bNoMoves ? Clips[static_cast<int32>(Gait)].Get() : nullptr;
	const FString Path = Loop ? Loop->GetPathName() : FString();
	auto Set = [&](ftcore::LocoClip Which, const TCHAR* Name)
	{
		UAnimSequence* Clip = nullptr;
		if (Path.Contains(TEXT("_Loop_F")))
		{
			const FString MovePath = Path.Replace(TEXT("_Loop_F"), Name, ESearchCase::CaseSensitive);
			if (FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(MovePath)))
			{
				Clip = LoadObject<UAnimSequence>(nullptr, *MovePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			else
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Locomotion: no %s, so none."), *MovePath);
			}
		}
		SetClip(Which, Clip);
	};
	Set(ftcore::locoStart(Gait), TEXT("_Start_F_Lfoot"));
	Set(ftcore::locoStop(Gait, false), TEXT("_Stop_F_Lfoot"));
	Set(ftcore::locoStop(Gait, true), TEXT("_Stop_F_Rfoot"));
}

void UFTLocomotionAnimInstance::SetTurnsFromRun()
{
	static const bool bNoTurns = FParse::Param(FCommandLine::Get(), TEXT("LocoNoTurns"));
	const UAnimSequence* Loop = bNoTurns ? nullptr : Clips[static_cast<int32>(ftcore::LocoClip::Run)].Get();
	const FString Path = Loop ? Loop->GetPathName() : FString();
	int32 Found = 0;
	for (int32 C = static_cast<int32>(ftcore::LocoClip::RunTurnLeft45L); C < static_cast<int32>(ftcore::LocoClip::Count); ++C)
	{
		const ftcore::LocoClip Which = static_cast<ftcore::LocoClip>(C);
		UAnimSequence* Clip = nullptr;
		if (Path.Contains(TEXT("_Run_Loop_F")))
		{
			const FTurnParts T = TurnParts(Which);
			const FString Name = ftcore::isLocoRunTurn(Which) ? FString::Printf(TEXT("_Run_Turn_%s_%03d_%sfoot"), T.Side, T.Angle, T.Foot)
				: ftcore::isLocoReface(Which) ? FString::Printf(TEXT("_Run_Reface_Start_F_%s_%03d"), T.Side, T.Angle)
				: FString::Printf(TEXT("_Stand_Turn_%03d_%s"), T.Angle, T.Side);
			const FString TurnPath = Path.Replace(TEXT("_Run_Loop_F"), *Name, ESearchCase::CaseSensitive);
			if (FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(TurnPath)))
			{
				Clip = LoadObject<UAnimSequence>(nullptr, *TurnPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
		}
		Found += Clip ? 1 : 0;
		SetClip(Which, Clip);
	}
	if (Loop)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Locomotion: %d of %d turn clips next to %s."), Found,
			static_cast<int32>(ftcore::LocoClip::Count) - static_cast<int32>(ftcore::LocoClip::RunTurnLeft45L), *Loop->GetName());
	}
}

void UFTLocomotionAnimInstance::MovementIntent(const UCharacterMovementComponent& Move, float& WantSpeed, float& StopDistance)
{
	WantSpeed = -1.f;
	StopDistance = -1.f;
	if (Move.IsFalling())
	{
		return;
	}
	if (!Move.GetCurrentAcceleration().IsNearlyZero())
	{
		WantSpeed = static_cast<float>(Move.GetMaxSpeed()) * FMath::Max(Move.GetAnalogInputModifier(), 0.f);
		return;
	}
	WantSpeed = 0.f;
	const float Brake = Move.GetMaxBrakingDeceleration();
	if (Brake > 0.f)
	{
		const float Speed = static_cast<float>(Move.Velocity.Size2D());
		StopDistance = Speed * Speed / (2.f * Brake);
	}
}

void UFTLocomotionAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	Loco.reset();
	// A character's turn clips move and turn its capsule; nothing else here extracts root motion.
	if (Cast<ACharacter>(GetOwningActor()))
	{
		RootMotionMode = ERootMotionMode::RootMotionFromEverything;
	}
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
		if (Move)
		{
			MovementIntent(*Move, In.wantSpeed, In.stopDistance);
		}
		// It steers: Locomotion eases its facing toward the way it moves and turns it by root motion.
		const float Facing = static_cast<float>(Owner->GetActorRotation().Yaw);
		const FVector A = Move ? Move->GetCurrentAcceleration() : FVector::ZeroVector;
		In.steer = In.rootMotion = true;
		In.yaw = Facing;
		In.moveYaw = V.SizeSquared2D() > 1.0 ? static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(V.Y, V.X))) : Facing;
		In.wantYaw = A.SizeSquared2D() > 1.0 ? static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(A.Y, A.X))) : Facing;
		In.face = bPushedFace;
		In.faceYaw = PushedFaceYaw;
	}
	else
	{
		In.groundSpeed = PushedSpeed;
		In.wantSpeed = PushedWant;
		In.stopDistance = PushedStop;
		In.turnBy = PushedTurnBy;
		PushedTurnBy = 0.f;
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

	// Each change of state is logged too, and starts, stops and turns ten times a second.
	const bool bChanged = Loco.state() != LastState;
	LastState = Loco.state();
	if (bLog && ((LogTimer -= DeltaSeconds) <= 0.f || bChanged))
	{
		const bool bMoving = Loco.state() == ftcore::LocoState::Starting || Loco.state() == ftcore::LocoState::Stopping || Loco.turning();
		LogTimer = LogTimer <= 0.f ? (bMoving ? 0.1f : 1.f) : LogTimer;
		FString Layers;
		for (std::size_t I = 0; I < Loco.layerCount(); ++I)
		{
			const ftcore::LocoLayer& L = Loco.layer(I);
			Layers += FString::Printf(TEXT(" %s %.2f@x%.2f"), ClipName(L.clip), L.weight, Loco.rate(L.clip));
		}
		if (Loco.turning())
		{
			Layers += FString::Printf(TEXT(" | turning %s x%.2f yaw %.0f (facing %.0f, heading %.0f)"), ClipName(Loco.turnClip()), Loco.turnWarp(),
				Loco.turnYaw(), In.yaw, In.wantYaw);
		}
		UE_LOG(LogForgottenThird, Display,
			TEXT("Locomotion %s: speed %.0f want %.0f stop in %.0f turn %.0f falling %d state %d lean %.1f twist %.1f pitch %.1f |%s"),
			*Owner->GetName(), In.groundSpeed, In.wantSpeed, In.stopDistance, In.turnRate, In.falling ? 1 : 0, static_cast<int32>(Loco.state()),
			Loco.lean(), Loco.twist(), Loco.pitch(), *Layers);
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
		// Shorter than 0.08 s it is a flip between the feet at a high frame rate, not a plant.
		if (PlantFoot != INDEX_NONE && PlantTime >= 0.08f)
		{
			const FVector Slip = Last[PlantFoot] - PlantStart;
			const FVector Fwd = Mesh.GetOwner() ? Mesh.GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
			SlipSum += Slip.Size2D();
			AlongSum += Slip.X * Fwd.X + Slip.Y * Fwd.Y;
			SideSum += Slip.Y * Fwd.X - Slip.X * Fwd.Y;
			SlipTime += PlantTime;
			// A body moving at either end; 20 cm/s of change either way counts as speeding up or braking.
			if (FMath::Max(PlantSpeed, BodySpeed) >= 50.f)
			{
				const int32 Kind = BodySpeed > PlantSpeed + 20.f ? 0 : BodySpeed < PlantSpeed - 20.f ? 1 : 2;
				KindSlip[Kind] += Slip.Size2D();
				KindTime[Kind] += PlantTime;
				++KindPlants[Kind];
			}
		}
		PlantFoot = Foot;
		PlantTime = 0.f;
		PlantSpeed = BodySpeed;
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
		auto Kind = [this](int32 K) { return KindTime[K] > 0.0 ? KindSlip[K] / KindTime[K] : 0.0; };
		UE_LOG(LogForgottenThird, Display, TEXT("LocoSlide %s so far: speeding up %.1f cm/s (%d plants), braking %.1f (%d), steady %.1f (%d)"),
			Mesh.GetOwner() ? *Mesh.GetOwner()->GetName() : TEXT("?"), Kind(0), KindPlants[0], Kind(1), KindPlants[1], Kind(2), KindPlants[2]);
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
	float Kept = 0.f;
	for (std::size_t I = 0; I < Loco.layerCount(); ++I)
	{
		const ftcore::LocoLayer& L = Loco.layer(I);
		const UAnimSequence* Seq = Instance->Clips[static_cast<int32>(L.clip)];
		if (!Seq)
		{
			continue;
		}
		const FLayer Layer{Seq, L.time, L.weight, ftcore::isLocoLoop(L.clip)};
		if (NumLayers < MaxLayers)
		{
			Layers[NumLayers++] = Layer;
			Kept += L.weight;
			continue;
		}
		// Full: it replaces the lightest, if heavier.
		int32 Lightest = 0;
		for (int32 K = 1; K < NumLayers; ++K)
		{
			Lightest = Layers[K].Weight < Layers[Lightest].Weight ? K : Lightest;
		}
		if (L.weight > Layers[Lightest].Weight)
		{
			Kept += L.weight - Layers[Lightest].Weight;
			Layers[Lightest] = Layer;
		}
	}
	for (int32 I = 0; I < NumLayers && Kept > 0.f; ++I)
	{
		Layers[I].Weight /= Kept;
	}

	// A turn driving the capsule: this frame's stretch of its root motion, its yaw scaled to the turn
	// asked for, for the movement component to apply.
	if (Loco.turning() && Loco.turnRootDriven() && Loco.turnTo() > Loco.turnFrom())
	{
		if (const UAnimSequence* Seq = Instance->Clips[static_cast<int32>(Loco.turnClip())])
		{
			FTransform Delta = Seq->ExtractRootMotionFromRange(Loco.turnFrom(), Loco.turnTo(), FAnimExtractContext());
			const float Yaw = Delta.GetRotation().GetTwistAngle(FVector::UpVector);
			Delta.SetRotation(FQuat(FVector::UpVector, Yaw * Loco.turnWarp()));
			GetExtractedRootMotion().Accumulate(Delta);
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
	// The capsule carries the body, turns included: hold the root where the ref pose has it.
	const FCompactPoseBoneIndex Root(0);
	Output.Pose[Root].SetTranslation(Output.Pose.GetRefPose(Root).GetTranslation());
	Output.Pose[Root].SetRotation(Output.Pose.GetRefPose(Root).GetRotation());

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
