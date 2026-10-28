#include "FTLocomotionAnimInstance.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"

#include "ForgottenThird.h"

namespace
{
const TCHAR* const SpineNames[FFTLocomotionAnimProxy::NumSpine] = {TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03")};
const TCHAR* const NeckNames[FFTLocomotionAnimProxy::NumNeck] = {TEXT("neck_01"), TEXT("head")};
// The bones the air pose turns: the arms (from the clavicles) first, then the legs.
const TCHAR* const LimbNames[FFTLocomotionAnimProxy::NumLimbs] = {
	TEXT("clavicle_l"), TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("hand_l"),
	TEXT("clavicle_r"), TEXT("upperarm_r"), TEXT("lowerarm_r"), TEXT("hand_r"),
	TEXT("thigh_l"), TEXT("calf_l"), TEXT("foot_l"), TEXT("thigh_r"), TEXT("calf_r"), TEXT("foot_r")};

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
	const int32 K = I - static_cast<int32>(ftcore::isLocoCrouchTurn(C) ? ftcore::LocoClip::CrouchTurnLeft45 : ftcore::LocoClip::StandTurnLeft45);
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
	case ftcore::LocoClip::AirPose: return TEXT("air pose");
	case ftcore::LocoClip::CrouchIdle: return TEXT("crouch idle");
	case ftcore::LocoClip::CrouchWalk: return TEXT("crouch walk");
	case ftcore::LocoClip::CrouchDown: return TEXT("crouch down");
	case ftcore::LocoClip::CrouchUp: return TEXT("crouch up");
	default: break;
	}
	// The turns, named once: "run turn L90 Rfoot", "reface R180", "stand turn L45", "crouch turn R90".
	static const TArray<FString> Turns = []
	{
		TArray<FString> Names;
		for (int32 I = static_cast<int32>(ftcore::LocoClip::RunTurnLeft45L); I < static_cast<int32>(ftcore::LocoClip::Count); ++I)
		{
			const ftcore::LocoClip Clip = static_cast<ftcore::LocoClip>(I);
			if (!ftcore::isLocoTurn(Clip))
			{
				Names.Add(TEXT("?"));
				continue;
			}
			const FTurnParts T = TurnParts(Clip);
			Names.Add(ftcore::isLocoRunTurn(Clip) ? FString::Printf(TEXT("run turn %s%d %sfoot"), T.Side, T.Angle, T.Foot)
				: ftcore::isLocoReface(Clip) ? FString::Printf(TEXT("reface %s%d"), T.Side, T.Angle)
				: ftcore::isLocoCrouchTurn(Clip) ? FString::Printf(TEXT("crouch turn %s%d"), T.Side, T.Angle)
				: FString::Printf(TEXT("stand turn %s%d"), T.Side, T.Angle));
		}
		return Names;
	}();
	const int32 Turn = static_cast<int32>(C) - static_cast<int32>(ftcore::LocoClip::RunTurnLeft45L);
	return Turns.IsValidIndex(Turn) ? *Turns[Turn] : TEXT("?");
}

// Samples per second when measuring a gait.
constexpr float kMeasureRate = 60.f;

// The traversal clips after the run loop's prefix (M_Neutral_), in TraversalClips' order: the vaults and climbs,
// then from kFirstRoll the rolls.
constexpr int32 kFirstRoll = 18;
const TCHAR* const TraversalNames[] = {
	TEXT("_Traversal_Hurdle_1_0_stand_F_V2_Lfoot"), TEXT("_Traversal_Hurdle_1_0_stand_F_V2_Rfoot"),
	TEXT("_Traversal_Hurdle_1_0_walk_F_V2_Lfoot"), TEXT("_Traversal_Hurdle_1_0_walk_F_V2_Rfoot"),
	TEXT("_Traversal_Hurdle_1_0_run_F_V2_Lfoot"), TEXT("_Traversal_Hurdle_1_0_run_F_V2_Rfoot"),
	TEXT("_Traversal_Mantle_1_0_stand_F_Lfoot"), TEXT("_Traversal_Mantle_1_0_stand_F_Rfoot"),
	TEXT("_Traversal_Mantle_1_0_walk_F_Lfoot"), TEXT("_Traversal_Mantle_1_0_walk_F_Rfoot"),
	TEXT("_Traversal_Mantle_1_0_run_F_Lfoot"), TEXT("_Traversal_Mantle_1_0_run_F_Rfoot"),
	TEXT("_Traversal_Climb_Start_2_5_stand_F_Lfoot"), TEXT("_Traversal_Climb_Start_2_5_stand_F_Rfoot"),
	TEXT("_Traversal_Climb_Start_2_5_walk_F_Lfoot"), TEXT("_Traversal_Climb_Start_2_5_walk_F_Rfoot"),
	TEXT("_Traversal_Climb_Start_2_5_run_F_Lfoot"), TEXT("_Traversal_Climb_Start_2_5_run_F_Rfoot"),
	TEXT("_Jump_F_Land_Roll_Lfoot"), TEXT("_Jump_F_Land_Roll_Rfoot")};
// The slide's clips, each in place of the run loop's whole name (M_Neutral_Run_Loop_F), in
// ftcore::SlideState::clip's order: the drop-ins, the loop, then the get-ups in ftcore::SlideOut's order.
const TCHAR* const SlideNames[] = {
	TEXT("M_Neutral_Slide_FootOut_Into_Lfoot"), TEXT("M_Neutral_Slide_FootOut_Into_Rfoot"),
	TEXT("M_Relaxed_Slide_FootOut_Into_Sprint_Lfoot"), TEXT("M_Relaxed_Slide_FootOut_Into_Sprint_Rfoot"),
	TEXT("M_Neutral_Slide_FootOut_Loop"),
	TEXT("M_Neutral_Slide_FootOut_Out_Idle_Stand"), TEXT("M_Neutral_Slide_FootOut_Out_Idle_Crouch"),
	TEXT("M_Neutral_Slide_FootOut_Out_Moving_Walk"), TEXT("M_Neutral_Slide_FootOut_Out_Moving_Run"),
	TEXT("M_Relaxed_Slide_FootOut_Out_Moving_Sprint"), TEXT("M_Neutral_Slide_FootOut_Out_Moving_Crouch")};
// The catches after the run loop's prefix, in TraversalClips' order from FirstCatchClip: from high up, then
// mantles from a chest's and a hip's height, each from a stand, a walk and a run.
const TCHAR* const CatchNames[] = {
	TEXT("_Traversal_Catch_Cliff_high_stand"),
	TEXT("_Traversal_Catch_Mantle_med_stand"), TEXT("_Traversal_Catch_Mantle_med_walk"), TEXT("_Traversal_Catch_Mantle_med_run"),
	TEXT("_Traversal_Catch_Mantle_low_stand"), TEXT("_Traversal_Catch_Mantle_low_walk"), TEXT("_Traversal_Catch_Mantle_low_run")};
// A traversal clip fades in over the locomotion this long (s), and out over this long before its end; one that
// takes over from another (a slide's next clip) takes this long to.
constexpr float kTraversalIn = 0.15f;
constexpr float kTraversalOut = 0.25f;
constexpr float kTraversalSwitch = 0.15f;

// The clip's root and ball (or foot) bones in its component space, sampled evenly from 0 to its
// length into Frames, and with Pelvis its pelvis bone at the same times (left at 0 without one). False
// (and no frames) when the clip has no skeleton, length or feet.
bool SampleFeet(const UAnimSequence& Clip, TArray<ftcore::GaitFrame>& Frames, TArray<ftcore::LocoPoint>* Pelvis = nullptr)
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

	const int32 PelvisBone = Pelvis ? Ref.FindBoneIndex(TEXT("pelvis")) : INDEX_NONE;

	const int32 N = FMath::Clamp(FMath::CeilToInt(Length * kMeasureRate) + 1, 3, static_cast<int32>(ftcore::kMaxGaitFrames));
	Frames.SetNum(N);
	if (Pelvis)
	{
		Pelvis->Init(ftcore::LocoPoint{}, N);
	}
	for (int32 K = 0; K < N; ++K)
	{
		const double Time = double(Length) * K / (N - 1);
		Frames[K].root = Point(ComponentSpace(0, Time).GetTranslation());
		Frames[K].left = Point(ComponentSpace(Left, Time).GetTranslation());
		Frames[K].right = Point(ComponentSpace(Right, Time).GetTranslation());
		Frames[K].yaw = float(ComponentSpace(0, Time).Rotator().Yaw);
		if (PelvisBone != INDEX_NONE)
		{
			(*Pelvis)[K] = Point(ComponentSpace(PelvisBone, Time).GetTranslation());
		}
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
			// A running turn's root speed every 0.1 s to its turn's end: where a pivot can enter at the body's speed.
			if (ftcore::isLocoRunTurn(Which))
			{
				Detail += TEXT(", root speed every 0.1 s");
				for (int32 K = 0; 0.1f * K <= Info.turnEnd + 1e-3f; ++K)
				{
					Detail += FString::Printf(TEXT(" %.0f"), ftcore::locoRootSpeed(Info, 0.1f * K));
				}
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

void UFTLocomotionAnimInstance::SetBaseClips(UAnimSequence* const (&Base)[NumBaseClips], bool bTraversal)
{
	for (int32 I = 0; I < NumBaseClips; ++I)
	{
		SetClip(static_cast<ftcore::LocoClip>(I), Base[I]);
	}
	SetMovesFromGait(ftcore::LocoClip::Walk);
	SetMovesFromGait(ftcore::LocoClip::Run);
	SetMovesFromGait(ftcore::LocoClip::Sprint);
	SetTurnsFromRun();
	SetAirPoseFromRun();
	SetCrouchFromRun();
	if (bTraversal)
	{
		SetTraversalFromRun();
	}
}

UAnimSequence* UFTLocomotionAnimInstance::FindRetargeted(ftcore::LocoClip Which, const FString& Folder, const USkeletalMesh& Body, const TCHAR* Who)
{
	// Each base clip's sources: the mocap sample's, then the template's.
	static const TCHAR* const Sources[NumBaseClips][2] = {
		{TEXT("M_Neutral_Stand_Idle_Loop"), TEXT("MM_Idle")},
		{TEXT("M_Neutral_Walk_Loop_F"), TEXT("MF_Unarmed_Walk_Fwd")},
		{TEXT("M_Neutral_Run_Loop_F"), TEXT("MF_Unarmed_Jog_Fwd")},
		{TEXT("M_Neutral_Sprint_Loop_F"), nullptr},
		{TEXT("M_Neutral_Jump_F_Start_Stand_Lfoot"), TEXT("MM_Jump")},
		{TEXT("M_Neutral_Jump_Loop_Fall"), TEXT("MM_Fall_Loop")},
		{TEXT("M_Neutral_Jump_F_Land_Stand_Light_Lfoot"), nullptr},
		{TEXT("M_Neutral_Jump_F_Start_Run_Lfoot"), nullptr},
		{TEXT("M_Neutral_Jump_F_Land_Run_Light_Lfoot"), nullptr},
	};
	const int32 Slot = static_cast<int32>(Which);
	if (Slot >= NumBaseClips || Folder.IsEmpty())
	{
		return nullptr;
	}
	const FString Dir = Folder.EndsWith(TEXT("/")) ? Folder : Folder + TEXT("/");
	for (const TCHAR* Source : Sources[Slot])
	{
		const FString Asset = Source ? FString::Printf(TEXT("%s_to_%s"), Source, *Body.GetName()) : FString();
		if (Asset.IsEmpty() || !FPackageName::DoesPackageExist(Dir + Asset))
		{
			continue;
		}
		UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *(Dir + Asset + TEXT(".") + Asset), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Clip && FitsBody(*Clip, Body, Who))
		{
			return Clip;
		}
	}
	return nullptr;
}

bool UFTLocomotionAnimInstance::FitsBody(const UAnimSequence& Clip, const USkeletalMesh& Body, const TCHAR* Who)
{
	const USkeleton* Skeleton = Clip.GetSkeleton();
	if (Skeleton && Skeleton == Body.GetSkeleton())
	{
		return true;
	}
	UE_LOG(LogForgottenThird, Warning, TEXT("%s: %s is for the skeleton %s, not %s's %s; left out."), Who, *Clip.GetName(),
		Skeleton ? *Skeleton->GetName() : TEXT("(none)"), *Body.GetName(), Body.GetSkeleton() ? *Body.GetSkeleton()->GetName() : TEXT("(none)"));
	return false;
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
			FString MovePath = Path.Replace(TEXT("_Loop_F"), Name, ESearchCase::CaseSensitive);
			// A start prefers the relaxed set's where there is one: the neutral run start flings its arms
			// back and up in a 60-degree drive.
			const FString Relaxed = MovePath.Replace(TEXT("M_Neutral_"), TEXT("M_Relaxed_"), ESearchCase::CaseSensitive);
			if (ftcore::isLocoStart(Which) && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Relaxed)))
			{
				MovePath = Relaxed;
			}
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

void UFTLocomotionAnimInstance::SetAirPoseFromRun()
{
	static const bool bNoAirPose = FParse::Param(FCommandLine::Get(), TEXT("LocoNoAirPose"));
	const UAnimSequence* Loop = bNoAirPose ? nullptr : Clips[static_cast<int32>(ftcore::LocoClip::Run)].Get();
	const FString Path = Loop ? Loop->GetPathName() : FString();
	UAnimSequence* Clip = nullptr;
	if (Path.Contains(TEXT("_Run_Loop_F")))
	{
		const FString PosePath = Path.Replace(TEXT("_Run_Loop_F"), TEXT("_Jump_F_Off_Run_Lfoot"), ESearchCase::CaseSensitive);
		if (FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(PosePath)))
		{
			Clip = LoadObject<UAnimSequence>(nullptr, *PosePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		else
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Locomotion: no %s, so no air pose."), *PosePath);
		}
	}
	SetClip(ftcore::LocoClip::AirPose, Clip);
}

void UFTLocomotionAnimInstance::SetTurnsFromRun()
{
	static const bool bNoTurns = FParse::Param(FCommandLine::Get(), TEXT("LocoNoTurns"));
	const UAnimSequence* Loop = bNoTurns ? nullptr : Clips[static_cast<int32>(ftcore::LocoClip::Run)].Get();
	const FString Path = Loop ? Loop->GetPathName() : FString();
	int32 Found = 0;
	for (int32 C = static_cast<int32>(ftcore::LocoClip::RunTurnLeft45L); C <= static_cast<int32>(ftcore::LocoClip::StandTurnRight180); ++C)
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
			static_cast<int32>(ftcore::LocoClip::StandTurnRight180) - static_cast<int32>(ftcore::LocoClip::RunTurnLeft45L) + 1, *Loop->GetName());
	}
}

void UFTLocomotionAnimInstance::SetCrouchFromRun()
{
	static const bool bNoCrouch = FParse::Param(FCommandLine::Get(), TEXT("LocoNoCrouch"));
	const UAnimSequence* Loop = bNoCrouch ? nullptr : Clips[static_cast<int32>(ftcore::LocoClip::Run)].Get();
	const FString Path = Loop ? Loop->GetPathName() : FString();
	auto Set = [&](ftcore::LocoClip Which, const FString& Name)
	{
		UAnimSequence* Clip = nullptr;
		if (Path.Contains(TEXT("_Run_Loop_F")))
		{
			const FString ClipPath = Path.Replace(TEXT("_Run_Loop_F"), *Name, ESearchCase::CaseSensitive);
			if (FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ClipPath)))
			{
				Clip = LoadObject<UAnimSequence>(nullptr, *ClipPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			else
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Locomotion: no %s, so none."), *ClipPath);
			}
		}
		SetClip(Which, Clip);
	};
	Set(ftcore::LocoClip::CrouchIdle, TEXT("_Crouch_Idle_Loop"));
	Set(ftcore::LocoClip::CrouchWalk, TEXT("_Crouch_Loop_F"));
	Set(ftcore::LocoClip::CrouchDown, TEXT("_Transition_Stand_to_Crouch"));
	Set(ftcore::LocoClip::CrouchUp, TEXT("_Transition_Crouch_to_Stand"));
	for (int32 C = static_cast<int32>(ftcore::LocoClip::CrouchTurnLeft45); C <= static_cast<int32>(ftcore::LocoClip::CrouchTurnRight180); ++C)
	{
		const ftcore::LocoClip Which = static_cast<ftcore::LocoClip>(C);
		const FTurnParts T = TurnParts(Which);
		Set(Which, FString::Printf(TEXT("_Crouch_Idle_Turn_%03d_%s"), T.Angle, T.Side));
	}
}

void UFTLocomotionAnimInstance::SetTraversalFromRun()
{
	static_assert(UE_ARRAY_COUNT(TraversalNames) == FirstSlideClip, "one name per traversal clip");
	static_assert(NumTraversalClips <= static_cast<int32>(ftcore::kMaxTraversalClips), "the traversal clips fit the TraversalSet");
	static_assert(UE_ARRAY_COUNT(SlideNames) == ftcore::kSlideClips, "one name per slide clip");
	static_assert(UE_ARRAY_COUNT(CatchNames) == NumCatchClips, "one name per catch clip");
	static const bool bNoTraversal = FParse::Param(FCommandLine::Get(), TEXT("LocoNoTraversal"));
	const UAnimSequence* Loop = bNoTraversal ? nullptr : Clips[static_cast<int32>(ftcore::LocoClip::Run)].Get();
	const FString Path = Loop ? Loop->GetPathName() : FString();
	TArray<ftcore::GaitFrame> Gait;
	TArray<ftcore::LocoPoint> Pelvis;
	TArray<ftcore::TraversalFrame> Frames;
	int32 Found = 0;
	for (int32 I = 0; I < NumTraversalClips; ++I)
	{
		// A traversal or catch clip's name stands in for the run loop's _Run_Loop_F, a slide clip's for its whole name.
		const bool bSlideClip = I >= FirstSlideClip && I < FirstCatchClip;
		const bool bCatchClip = I >= FirstCatchClip;
		const TCHAR* const RunName = bSlideClip ? TEXT("M_Neutral_Run_Loop_F") : TEXT("_Run_Loop_F");
		const TCHAR* const ClipName = bCatchClip ? CatchNames[I - FirstCatchClip] : bSlideClip ? SlideNames[I - FirstSlideClip] : TraversalNames[I];
		UAnimSequence* Clip = nullptr;
		if (Path.Contains(RunName))
		{
			const FString ClipPath = Path.Replace(RunName, ClipName, ESearchCase::CaseSensitive);
			if (FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ClipPath)))
			{
				Clip = LoadObject<UAnimSequence>(nullptr, *ClipPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
		}
		TraversalClips[I] = Clip;
		const bool bSampled = Clip && SampleFeet(*Clip, Gait, &Pelvis);
		if (bSampled)
		{
			// Core's x is the way the clip goes (its root's net travel), y its left, z up; the clip's own
			// component space may face +Y.
			const ftcore::LocoPoint& A = Gait[0].root;
			const ftcore::LocoPoint& B = Gait.Last().root;
			FVector2D Dir(B.x - A.x, B.y - A.y);
			Dir = Dir.SizeSquared() > 1.0 ? Dir.GetSafeNormal() : FVector2D(1.0, 0.0);
			auto Point = [&Dir](const ftcore::LocoPoint& P)
			{
				// Left of Dir in UE's left-handed ground plane (X forward, Y right) is (Dir.Y, -Dir.X).
				return ftcore::TravPoint{float(P.x * Dir.X + P.y * Dir.Y), float(P.x * Dir.Y - P.y * Dir.X), P.z};
			};
			Frames.SetNum(Gait.Num());
			for (int32 K = 0; K < Gait.Num(); ++K)
			{
				Frames[K] = {Point(Gait[K].root), Point(Gait[K].left), Point(Gait[K].right), Point(Pelvis[K])};
			}
			++Found;
		}
		if (bSlideClip)
		{
			// The slide's drop-ins, its loop and its get-ups, each left unmeasured when missing.
			const int32 K = I - FirstSlideClip;
			const float ClipLength = bSampled ? Clip->GetPlayLength() : 0.f;
			if (K < ftcore::kSlideLoop)
			{
				const ftcore::SlideIntoInfo Into = bSampled ? ftcore::measureSlideInto(Frames.GetData(), Frames.Num(), ClipLength) : ftcore::SlideIntoInfo();
				Slide.setInto(static_cast<std::size_t>(K), Into);
				if (bSampled)
				{
					UE_LOG(LogForgottenThird, Display,
						TEXT("Locomotion: slide %d = %s, %.2f s: %s, take-off %.2f off foot %d, down at %.2f, enters at %.0f cm/s, slides at %.0f cm/s"),
						K, *Clip->GetName(), ClipLength, Into.length <= 0.f ? TEXT("NOT MEASURED") : TEXT("drop-in"), Into.takeoff, Into.takeoffFoot,
						Into.down, Into.entrySpeed, Into.slideSpeed);
				}
			}
			else if (K == ftcore::kSlideLoop)
			{
				Slide.setLoop(ClipLength);
				if (bSampled)
				{
					UE_LOG(LogForgottenThird, Display, TEXT("Locomotion: slide %d = %s, %.2f s: loop"), K, *Clip->GetName(), ClipLength);
				}
			}
			else
			{
				const ftcore::SlideOutInfo GetUp = bSampled ? ftcore::measureSlideOut(Frames.GetData(), Frames.Num(), ClipLength) : ftcore::SlideOutInfo();
				Slide.setOut(static_cast<ftcore::SlideOut>(K - ftcore::kSlideLoop - 1), GetUp);
				if (bSampled)
				{
					UE_LOG(LogForgottenThird, Display,
						TEXT("Locomotion: slide %d = %s, %.2f s: %s, first plant %.2f, up %.2f, settled %.2f, ends at %.0f cm/s"),
						K, *Clip->GetName(), ClipLength, GetUp.length <= 0.f ? TEXT("NOT MEASURED") : TEXT("get-up"), GetUp.plant, GetUp.up,
						GetUp.settle, GetUp.endSpeed);
				}
			}
			continue;
		}
		ftcore::TraversalClipInfo Info;
		if (bSampled)
		{
			if (bCatchClip)
			{
				// A catch is read from its root, from the catch (its lowest point) on: the clip comes down onto the ledge
				// from far higher than a jump comes from.
				Info = ftcore::measureCatch(Frames.GetData(), Frames.Num(), Clip->GetPlayLength());
				UE_LOG(LogForgottenThird, Display,
					TEXT("Locomotion: traversal %d = %s, %.2f s: %s, catch %.2f land %.2f, edge %.0f cm ahead %.0f up, ends %.0f up, goes on at %.0f cm/s"),
					I, *Clip->GetName(), Clip->GetPlayLength(), Info.length <= 0.f ? TEXT("NOT MEASURED") : TEXT("catch"), Info.contact, Info.land,
					Info.ledgeAhead, Info.ledgeHeight, Info.rise, Info.exitSpeed);
			}
			else if (I >= kFirstRoll)
			{
				// A roll is read from the pelvis going down and back up; its root stays on the floor (a land roll's from
				// its touchdown, where it is played from).
				Info = ftcore::measureRoll(Frames.GetData(), Frames.Num(), Clip->GetPlayLength());
				UE_LOG(LogForgottenThird, Display,
					TEXT("Locomotion: traversal %d = %s, %.2f s: %s, down at %.2f, tucked %.2f back up %.2f, pelvis down %.0f cm, ends %.0f up, enters at %.0f cm/s off foot %d"),
					I, *Clip->GetName(), Clip->GetPlayLength(), Info.length <= 0.f ? TEXT("NOT MEASURED") : TEXT("roll"), Info.touchdown, Info.contact,
					Info.land, Info.dip, Info.rise, Info.entrySpeed, Info.takeoffFoot);
			}
			else
			{
				Info = ftcore::measureTraversal(Frames.GetData(), Frames.Num(), Clip->GetPlayLength());
				UE_LOG(LogForgottenThird, Display,
					TEXT("Locomotion: traversal %d = %s, %.2f s: %s, contact %.2f leave %.2f land %.2f, edge %.0f cm ahead %.0f up, top %.0f deep, ends %.0f up, enters at %.0f cm/s off foot %d"),
					I, *Clip->GetName(), Clip->GetPlayLength(), Info.length <= 0.f ? TEXT("NOT MEASURED") : Info.move == ftcore::TraversalMove::Up ? TEXT("up") : TEXT("over"),
					Info.contact, Info.leave, Info.land, Info.ledgeAhead, Info.ledgeHeight, Info.topDepth, Info.rise, Info.entrySpeed, Info.takeoffFoot);
			}
		}
		Traversal.setClip(static_cast<std::size_t>(I), Info);
	}
	if (Loop)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Locomotion: %d of %d traversal, slide and catch clips next to %s."), Found, NumTraversalClips, *Loop->GetName());
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
	// A character's turn clips move and turn its capsule; nothing else here extracts root motion. Anything
	// else is a party follower, which the party trail brakes more gently.
	const bool bLead = Cast<ACharacter>(GetOwningActor()) != nullptr;
	if (bLead)
	{
		RootMotionMode = ERootMotionMode::RootMotionFromEverything;
	}
	else
	{
		ftcore::tuneForFollower(Loco.tuning());
	}
	bHasYaw = false;
	bLog = FParse::Param(FCommandLine::Get(), TEXT("LocoLog"));
	// -LocoRunPitch=<deg> tries another run lean for a capture (negative pulls the mocap upright).
	FParse::Value(FCommandLine::Get(), TEXT("LocoRunPitch="), Loco.tuning().runPitch);
	// -LocoBrakePitch=<deg> and -LocoBrakeDecel=<cm/s^2> try another stop lean on both bodies (negative leans
	// back), -LocoLeadBrakePitch=... or -LocoFollowerBrakePitch=... on one.
	const TCHAR* Body = bLead ? TEXT("Lead") : TEXT("Follower");
	FParse::Value(FCommandLine::Get(), TEXT("LocoBrakePitch="), Loco.tuning().brakePitch);
	FParse::Value(FCommandLine::Get(), *FString::Printf(TEXT("Loco%sBrakePitch="), Body), Loco.tuning().brakePitch);
	FParse::Value(FCommandLine::Get(), TEXT("LocoBrakeDecel="), Loco.tuning().brakeDecel);
	FParse::Value(FCommandLine::Get(), *FString::Printf(TEXT("Loco%sBrakeDecel="), Body), Loco.tuning().brakeDecel);
	// -LocoGaitDeadBand=<share> drops a gait with less of the mix than this (lead 0.03, followers 0.08: at 0.03 Duy holds ~6% walk at the run).
	FParse::Value(FCommandLine::Get(), TEXT("LocoGaitDeadBand="), Loco.tuning().gaitDeadBand);
	// -LocoPhaseTrim=<share> caps how far a follower's stride rate bends to keep its step off the lead's.
	FParse::Value(FCommandLine::Get(), TEXT("LocoPhaseTrim="), Loco.tuning().phaseTrim);
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
		// Carried by a traversal the capsule stands, but a body sliding or getting up crouched wants it down: its
		// clip hands back to a crouch.
		In.crouch = Character->bIsCrouched || (TravClip >= 0 && !bTravOut && TravWeight >= 1.f && Move && Move->bWantsToCrouch);
	}
	else
	{
		In.groundSpeed = PushedSpeed;
		In.wantSpeed = PushedWant;
		In.stopDistance = PushedStop;
		In.turnBy = PushedTurnBy;
		PushedTurnBy = 0.f;
		In.leadPhase = PushedLeadPhase;
		In.leadStride = PushedLeadStride;
		In.falling = bPushedAir;
		In.verticalSpeed = PushedVerticalSpeed;
		In.crouch = bPushedCrouch;
		In.yaw = static_cast<float>(Owner->GetActorRotation().Yaw);
		In.face = bPushedFace;
		In.faceYaw = PushedFaceYaw;
	}
	const float Yaw = static_cast<float>(Owner->GetActorRotation().Yaw);
	In.turnRate = bHasYaw && DeltaSeconds > 0.f ? FMath::FindDeltaAngleDegrees(LastYaw, Yaw) / DeltaSeconds : 0.f;
	// Only over time that passes: a character with root motion poses again with no time passed once its move
	// has turned it, and taking the yaw there would hide the clip's turn from the next frame's rate: the body
	// would never bank into a running turn. The phase and stride the frame began with stay too
	// (LeadPhaseAtFrameStart).
	if (DeltaSeconds > 0.f)
	{
		LastYaw = Yaw;
		bHasYaw = true;
		SteppedFrame = GFrameCounter;
		PhaseBefore = Loco.phase();
		StrideBefore = Loco.strideLength();
	}
	// A traversal's weight: in over its first kTraversalIn, out over the kTraversalOut before its end (unless it
	// loops or its owner goes on from it). Let go by its owner, it plays on at its own pace and fades over
	// kTraversalOut from the weight it had (sooner by the end of one that does not loop), then is gone.
	auto PlayOn = [DeltaSeconds](float Time, float Length, bool bLoop)
	{
		return bLoop && Length > 0.f ? FMath::Fmod(Time + DeltaSeconds, Length) : FMath::Min(Time + DeltaSeconds, Length);
	};
	const UAnimSequence* Trav = TravClip >= 0 && TravClip < NumTraversalClips ? TraversalClips[TravClip].Get() : nullptr;
	const float TravLength = Trav ? Trav->GetPlayLength() : 0.f;
	if (Trav && bTravOut)
	{
		TravOutFor += DeltaSeconds;
		TravTime = PlayOn(TravTime, TravLength, bTravLoop);
		TravWeight = FMath::Clamp(FMath::Min(TravOutFrom * (1.f - TravOutFor / kTraversalOut), bTravLoop ? 1.f : (TravLength - TravTime) / kTraversalOut), 0.f, 1.f);
	}
	else if (Trav)
	{
		TravElapsed += DeltaSeconds;
		TravWeight = FMath::Clamp(FMath::Min(TravElapsed / kTraversalIn, bTravEndFade && !bTravLoop ? (TravLength - TravTime) / kTraversalOut : 1.f), 0.f, 1.f);
	}
	else
	{
		TravWeight = 0.f;
	}
	if (bTravOut && TravWeight <= 0.f)
	{
		TravClip = -1;
		bTravOut = false;
	}
	// The clip it is taking over from plays on at its own pace until kTraversalSwitch has passed.
	const UAnimSequence* PrevSeq = PrevClip >= 0 && PrevClip < NumTraversalClips ? TraversalClips[PrevClip].Get() : nullptr;
	PrevFor += DeltaSeconds;
	if (!PrevSeq || TravClip < 0 || PrevFor >= kTraversalSwitch)
	{
		PrevClip = -1;
	}
	else
	{
		PrevTime = PlayOn(PrevTime, PrevSeq->GetPlayLength(), bPrevLoop);
	}
	// A body a catch carries up from the air stays in the air to its locomotion until the catch plays at full
	// weight, so the landing it would play then does so under the catch rather than as the catch fades in.
	if (TravClip >= FirstCatchClip && !bTravOut && TravWeight < 1.f)
	{
		In.falling = true;
	}
	// A body a traversal carries (fading in, playing or fading out) isn't asked to turn, nor to replay a turn: a run
	// turn begun under it would turn the body by its root as the traversal fades out, the planted foot sliding sideways.
	if (TravClip >= 0)
	{
		In.wantYaw = In.yaw;
		In.turnBy = 0.f;
	}
	Loco.update(In);

	if (const USkeletalMeshComponent* Mesh = GetSkelMeshComponent())
	{
		ForwardCS = Mesh->GetComponentTransform().InverseTransformVectorNoScale(Owner->GetActorForwardVector()).GetSafeNormal2D();
		if (ForwardCS.IsNearlyZero())
		{
			ForwardCS = FVector::ForwardVector;
		}
	}

	// Each change of state is logged too, and starts, stops, turns and stance changes ten times a second.
	const bool bChanged = Loco.state() != LastState;
	LastState = Loco.state();
	if (bLog && ((LogTimer -= DeltaSeconds) <= 0.f || bChanged))
	{
		const bool bMoving = Loco.state() == ftcore::LocoState::Starting || Loco.state() == ftcore::LocoState::Stopping || Loco.turning()
			|| Loco.state() == ftcore::LocoState::StanceChange;
		LogTimer = LogTimer <= 0.f ? (bMoving ? 0.1f : 1.f) : LogTimer;
		FString Layers;
		for (std::size_t I = 0; I < Loco.layerCount(); ++I)
		{
			const ftcore::LocoLayer& L = Loco.layer(I);
			Layers += FString::Printf(TEXT(" %s %.2f@x%.2f"), ClipName(L.clip), L.weight, Loco.rate(L.clip));
		}
		if (Loco.turning())
		{
			Layers += FString::Printf(TEXT(" | turning %s x%.2f yaw %.0f (facing %.0f, heading %.0f) from %.2f s at %.0f cm/s"), ClipName(Loco.turnClip()),
				Loco.turnWarp(), Loco.turnYaw(), In.yaw, In.wantYaw, Loco.turnEnter(), ftcore::locoRootSpeed(Loco.clip(Loco.turnClip()), Loco.turnEnter()));
		}
		if (Loco.limbs().arms > 0.f || Loco.limbs().legs > 0.f)
		{
			Layers += FString::Printf(TEXT(" | air pose arms %.2f legs %.2f"), Loco.limbs().arms, Loco.limbs().legs);
		}
		UE_LOG(LogForgottenThird, Display,
			TEXT("Locomotion %s: speed %.0f want %.0f stop in %.0f turn %.0f falling %d state %d lean %.1f twist %.1f pitch %.1f |%s"),
			*Owner->GetName(), In.groundSpeed, In.wantSpeed, In.stopDistance, In.turnRate, In.falling ? 1 : 0, static_cast<int32>(Loco.state()),
			Loco.lean(), Loco.twist(), Loco.pitch(), *Layers);
	}

	// -LocoAngles logs the posed body every frame, stamped with the UTC time so captures can be matched to it:
	// the trunk (pelvis to neck) and chest (spine_03 to neck) tipped forward and to the right of upright, the
	// head the same from the neck, each elbow's bend (0 straight), and the body (pelvis to head) forward, the
	// lean a side view shows, then the stride phase (two party members' phases give their step offset), a
	// follower's stride rate trim, and the ground speed Core was given with how hard it slows (a kinematic
	// follower's own velocity reads 0). The pose is last frame's.
	static const bool bAngles = FParse::Param(FCommandLine::Get(), TEXT("LocoAngles"));
	const USkeletalMeshComponent* Posed = bAngles ? GetSkelMeshComponent() : nullptr;
	if (Posed)
	{
		const FVector Fwd = Owner->GetActorForwardVector().GetSafeNormal2D();
		const FVector Side = FVector::CrossProduct(FVector::UpVector, Fwd);
		auto At = [Posed](const TCHAR* Bone) { return Posed->GetBoneLocation(FName(Bone), EBoneSpaces::WorldSpace); };
		auto Tip = [](const FVector& V, const FVector& Axis)
		{
			return FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(FVector::DotProduct(V, Axis)), static_cast<float>(V.Z)));
		};
		auto Elbow = [&At](const TCHAR* Upper, const TCHAR* Lower, const TCHAR* Hand)
		{
			const FVector A = (At(Lower) - At(Upper)).GetSafeNormal();
			const FVector B = (At(Hand) - At(Lower)).GetSafeNormal();
			return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(static_cast<float>(FVector::DotProduct(A, B)), -1.f, 1.f)));
		};
		const FVector Neck = At(TEXT("neck_01"));
		const FVector Trunk = Neck - At(TEXT("pelvis"));
		const FVector Chest = Neck - At(TEXT("spine_03"));
		const FVector Head = At(TEXT("head")) - Neck;
		const FDateTime Now = FDateTime::UtcNow();
		UE_LOG(LogForgottenThird, Display,
			TEXT("LocoAngles %s utc %lld state %d speed %.0f | trunk pitch %.1f roll %.1f | chest pitch %.1f roll %.1f | head pitch %.1f roll %.1f | elbow l %.0f r %.0f | loco pitch %.1f lean %.1f twist %.1f | body pitch %.1f | phase %.3f trim %.3f | ground %.0f decel %.0f"),
			*Owner->GetName(), Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond(), static_cast<int32>(Loco.state()),
			static_cast<float>(Owner->GetVelocity().Size2D()), Tip(Trunk, Fwd), Tip(Trunk, Side), Tip(Chest, Fwd), Tip(Chest, Side),
			Tip(Head, Fwd), Tip(Head, Side), Elbow(TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("hand_l")),
			Elbow(TEXT("upperarm_r"), TEXT("lowerarm_r"), TEXT("hand_r")), Loco.pitch(), Loco.lean(), Loco.twist(),
			Tip(At(TEXT("head")) - At(TEXT("pelvis")), Fwd), Loco.phase(), Loco.phaseTrim(), In.groundSpeed, Loco.decel());
	}
}

void FFTFootSlideProbe::Sample(const USkeletalMeshComponent& Mesh, float DeltaSeconds, float BodySpeed, bool bOffFoot)
{
	if (!bChecked)
	{
		bEnabled = FParse::Param(FCommandLine::Get(), TEXT("LocoSlide"));
		bPlantLog = bEnabled && FParse::Param(FCommandLine::Get(), TEXT("LocoPlants"));
		bChecked = true;
	}
	if (!bEnabled || DeltaSeconds <= 0.f)
	{
		return;
	}
	// In the air no foot is planted, nor while a traversal or a slide carries the body (its custom movement mode; a
	// follower says so itself): drop the plant under way and start again once back on foot.
	const ACharacter* Character = Cast<ACharacter>(Mesh.GetOwner());
	const UCharacterMovementComponent* Move = Character ? Character->GetCharacterMovement() : nullptr;
	if (bOffFoot || (Move && (Move->IsFalling() || Move->MovementMode == MOVE_Custom)))
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
			if (bPlantLog && PathLen >= 4)
			{
				// Where the foot was at a share of the plant's time, from its path.
				auto At = [this](float Share)
				{
					const float T = Share * PathTime[PathLen - 1];
					int32 I = 1;
					while (I < PathLen - 1 && PathTime[I] < T)
					{
						++I;
					}
					const float Span = PathTime[I] - PathTime[I - 1];
					return FMath::Lerp(Path[I - 1], Path[I], Span > 0.f ? FMath::Clamp((T - PathTime[I - 1]) / Span, 0.f, 1.f) : 1.f);
				};
				const FVector2D Mid = At(0.75f) - At(0.25f);
				const FDateTime Utc = FDateTime::UtcNow();
				UE_LOG(LogForgottenThird, Display,
					TEXT("LocoPlant %s utc %lld body %.0f | %.3f s, net %.1f forward %.1f right | middle half %.1f forward (cm/s), foot %s"),
					Mesh.GetOwner() ? *Mesh.GetOwner()->GetName() : TEXT("?"), Utc.ToUnixTimestamp() * 1000 + Utc.GetMillisecond(), BodySpeed,
					PlantTime, (Slip.X * Fwd.X + Slip.Y * Fwd.Y) / PlantTime, (Slip.Y * Fwd.X - Slip.X * Fwd.Y) / PlantTime,
					(Mid.X * Fwd.X + Mid.Y * Fwd.Y) / (0.5f * PathTime[PathLen - 1]), PlantFoot == 0 ? TEXT("l") : TEXT("r"));
			}
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
		PathLen = 0;
	}
	if (Foot != INDEX_NONE)
	{
		if (bPlantLog && PathLen < MaxPath)
		{
			Path[PathLen] = FVector2D(Now[Foot]);
			PathTime[PathLen++] = PlantTime;
		}
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
	// A traversal clip over them by its weight, sharing it with the one it is taking over from (by smoothstep
	// over kTraversalSwitch); when full, the lightest locomotion layers make room.
	const float TravWeight = Instance->TravWeight;
	if (TravWeight > 0.f)
	{
		const UAnimSequence* From = Instance->PrevClip >= 0 ? Instance->TraversalClips[Instance->PrevClip].Get() : nullptr;
		const float Share = From ? FMath::SmoothStep(0.f, 1.f, Instance->PrevFor / kTraversalSwitch) : 1.f;
		const int32 Room = MaxLayers - (Share < 1.f ? 2 : 1);
		float Sum = 0.f;
		for (int32 I = 0; I < NumLayers; ++I)
		{
			Sum += Layers[I].Weight;
		}
		while (NumLayers > Room)
		{
			int32 Lightest = 0;
			for (int32 K = 1; K < NumLayers; ++K)
			{
				Lightest = Layers[K].Weight < Layers[Lightest].Weight ? K : Lightest;
			}
			Sum -= Layers[Lightest].Weight;
			Layers[Lightest] = Layers[--NumLayers];
		}
		for (int32 I = 0; I < NumLayers; ++I)
		{
			Layers[I].Weight = Sum > 0.f ? Layers[I].Weight / Sum * (1.f - TravWeight) : 0.f;
		}
		Layers[NumLayers++] = {Instance->TraversalClips[Instance->TravClip].Get(), Instance->TravTime, TravWeight * Share, Instance->bTravLoop};
		if (Share < 1.f)
		{
			Layers[NumLayers++] = {From, Instance->PrevTime, TravWeight * (1.f - Share), Instance->bPrevLoop};
		}
	}

	// The air pose over the limbs.
	const ftcore::LocoLimbs& Limbs = Loco.limbs();
	LimbPose = {Instance->Clips[static_cast<int32>(Limbs.clip)], Limbs.time, 1.f, false};
	LimbWeight[0] = LimbPose.Seq ? Limbs.arms : 0.f;
	LimbWeight[1] = LimbPose.Seq ? Limbs.legs : 0.f;
	ElbowHold = Loco.elbowHold();
	ElbowBend = Loco.tuning().elbowBend;
	ElbowForward = Instance->ForwardCS;

	// A turn driving the capsule: this frame's stretch of its root motion, its yaw scaled to the turn
	// asked for, for the movement component to apply.
	// (None while a traversal plays: its owner carries the body until it lets go.)
	if ((Instance->TravClip < 0 || Instance->bTravOut) && Loco.turning() && Loco.turnRootDriven() && Loco.turnTo() > Loco.turnFrom())
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
	// The head keeps level against the pitch and the lean, and looks into a turn ahead of the chest.
	HeadBend = FQuat(FVector::UpVector, FMath::DegreesToRadians(Loco.headYaw()))
		* FQuat(Right, FMath::DegreesToRadians(Loco.headPitch())) * FQuat(F, FMath::DegreesToRadians(-Loco.headRoll()));
	// -LocoNoBend leaves the clips' own trunk, head and elbows, for measuring what the bend adds.
	static const bool bNoBend = FParse::Param(FCommandLine::Get(), TEXT("LocoNoBend"));
	if (bNoBend)
	{
		SpineBend = HeadBend = FQuat::Identity;
		ElbowHold = 0.f;
	}
	// A traversal clip has its own trunk, head, arms and legs: the bends and the air pose give way to it.
	if (TravWeight > 0.f)
	{
		const float Keep = 1.f - TravWeight;
		SpineBend = FQuat::Slerp(FQuat::Identity, SpineBend, Keep);
		HeadBend = FQuat::Slerp(FQuat::Identity, HeadBend, Keep);
		ElbowHold *= Keep;
		LimbWeight[0] *= Keep;
		LimbWeight[1] *= Keep;
	}
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
		int32 NeckFound = 0;
		for (int32 I = 0; I < NumNeck; ++I)
		{
			Neck[I].BoneName = NeckNames[I];
			Neck[I].Initialize(Container);
			NeckFound += Neck[I].IsValidToEvaluate(Container) ? 1 : 0;
		}
		if (NeckFound < NumNeck)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTLocomotion: %d of %d neck bones (neck_01, head) found; the head's level and look use those."),
				NeckFound, NumNeck);
		}
		for (int32 I = 0; I < NumLimbs; ++I)
		{
			Limb[I].BoneName = LimbNames[I];
			Limb[I].Initialize(Container);
		}
		BoundSerial = Container.GetSerialNumber();
		bBound = true;
	}

	// The air pose: each limb bone's local rotation turned toward the pose's by the arms' or legs' weight.
	if (LimbPose.Seq && (LimbWeight[0] > 0.f || LimbWeight[1] > 0.f))
	{
		FCompactPose Pose;
		Pose.SetBoneContainer(&Container);
		FBlendedCurve Curve;
		Curve.InitFrom(Output.Curve);
		UE::Anim::FStackAttributeContainer Attributes;
		FAnimationPoseData Data(Pose, Curve, Attributes);
		Sample(LimbPose, Data);
		for (int32 I = 0; I < NumLimbs; ++I)
		{
			const float W = LimbWeight[I < NumArmBones ? 0 : 1];
			if (W <= 0.f || !Limb[I].IsValidToEvaluate(Container))
			{
				continue;
			}
			const FCompactPoseBoneIndex Index = Limb[I].GetCompactPoseIndex(Container);
			Output.Pose[Index].SetRotation(FQuat::Slerp(Output.Pose[Index].GetRotation(), Pose[Index].GetRotation(), W).GetNormalized());
		}
	}
	// Each chain's bones turn by an equal share of its bend about their own origins, in component
	// space; the bones above follow, so the head's bend comes on top of the spine's.
	auto CountValid = [&Container](const FBoneReference* Bones, int32 Num)
	{
		int32 Valid = 0;
		for (int32 I = 0; I < Num; ++I)
		{
			Valid += Bones[I].IsValidToEvaluate(Container) ? 1 : 0;
		}
		return Valid;
	};
	const int32 SpineValid = CountValid(Spine, NumSpine);
	const int32 NeckValid = CountValid(Neck, NumNeck);
	const bool bSpine = SpineValid > 0 && !SpineBend.Equals(FQuat::Identity, 1e-5);
	const bool bNeck = NeckValid > 0 && !HeadBend.Equals(FQuat::Identity, 1e-5);
	const bool bElbows = ElbowHold > 0.002f && ElbowBend > 0.f;
	if (!bSpine && !bNeck && !bElbows)
	{
		return true;
	}
	FCSPose<FCompactPose> CS;
	// A copy (on the anim stack): the conversion back writes into Output.Pose before taking CS's.
	CS.InitPose(Output.Pose);
	auto Bend = [&Container, &CS](const FBoneReference* Bones, int32 Num, int32 Valid, const FQuat& By)
	{
		const FQuat Share = FQuat::Slerp(FQuat::Identity, By, 1.f / static_cast<float>(Valid));
		for (int32 I = 0; I < Num; ++I)
		{
			if (!Bones[I].IsValidToEvaluate(Container))
			{
				continue;
			}
			const FCompactPoseBoneIndex Index = Bones[I].GetCompactPoseIndex(Container);
			FTransform T = CS.GetComponentSpaceTransform(Index);
			T.SetRotation((Share * T.GetRotation()).GetNormalized());
			CS.SetComponentSpaceTransform(Index, T);
		}
	};
	if (bSpine)
	{
		Bend(Spine, NumSpine, SpineValid, SpineBend);
	}
	if (bNeck)
	{
		Bend(Neck, NumNeck, NeckValid, HeadBend);
	}
	// The elbows: a forearm bent less than ElbowBend from its upper arm turns about the elbow toward
	// it, by ElbowHold of the difference; one held straight bends toward the body's front.
	for (int32 Side = 0; bElbows && Side < 2; ++Side)
	{
		const FBoneReference& Upper = Limb[Side * 4 + 1];
		const FBoneReference& Lower = Limb[Side * 4 + 2];
		const FBoneReference& Hand = Limb[Side * 4 + 3];
		if (!Upper.IsValidToEvaluate(Container) || !Lower.IsValidToEvaluate(Container) || !Hand.IsValidToEvaluate(Container))
		{
			continue;
		}
		// The wrist from the forearm and the hand's local offset: reading the hand through CS would
		// fix its component-space place, and it would stay put as the forearm turns.
		const FCompactPoseBoneIndex LowerIndex = Lower.GetCompactPoseIndex(Container);
		const FTransform LowerCS = CS.GetComponentSpaceTransform(LowerIndex);
		const FVector Elbow = LowerCS.GetTranslation();
		const FVector Wrist = LowerCS.TransformPosition(Output.Pose[Hand.GetCompactPoseIndex(Container)].GetTranslation());
		const FVector A = (Elbow - CS.GetComponentSpaceTransform(Upper.GetCompactPoseIndex(Container)).GetTranslation()).GetSafeNormal();
		const FVector B = (Wrist - Elbow).GetSafeNormal();
		const float BentBy = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(A, B), -1.f, 1.f)));
		if (A.IsZero() || B.IsZero() || BentBy >= ElbowBend)
		{
			continue;
		}
		FVector Axis = FVector::CrossProduct(A, B);
		if (Axis.SizeSquared() < 1e-4)
		{
			Axis = FVector::CrossProduct(A, ElbowForward);
		}
		if (!Axis.Normalize())
		{
			continue;
		}
		FTransform T = LowerCS;
		T.SetRotation((FQuat(Axis, FMath::DegreesToRadians((ElbowBend - BentBy) * ElbowHold)) * T.GetRotation()).GetNormalized());
		CS.SetComponentSpaceTransform(LowerIndex, T);
	}
	FCSPose<FCompactPose>::ConvertComponentPosesToLocalPoses(MoveTemp(CS), Output.Pose);
	return true;
}
