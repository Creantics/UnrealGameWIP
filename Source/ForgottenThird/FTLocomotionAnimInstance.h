#pragma once

// Anim class for a walking body: idle, walk, run, jump start, fall loop and landing clips (and the
// moving jump and landing, and each gait's start and left- and right-foot stops), blended by
// ftcore::Locomotion from how the owner moves, with the air pose (LocoClip::AirPose, its last frame)
// laid over the arms and legs in the air. No Anim Blueprint needed.
//
//   Mesh->SetAnimInstanceClass(UFTLocomotionAnimInstance::StaticClass());
//   Anim->SetClip(ftcore::LocoClip::Walk, WalkSeq);   // measures the clip's gait once
//
// A character owner drives it from its velocity, acceleration and IsFalling; any other owner (the
// party follower) calls SetGroundSpeed, and SetIntent for starts and stops, every frame. The turn
// rate comes from the owner's yaw. Walk and run play at the rate that keeps the planted feet still
// (their ground speed is measured from the clip at SetClip), starts and stops at the point whose
// root travel matches the capsule's, the root bone's translation is held at the ref pose (the
// capsule moves the body), and the spine banks, twists and leans forward per Locomotion's lean,
// twist and pitch. At a run the elbows are held bent at least Locomotion's elbowBend.
//
// Turns (SetTurnsFromRun): a character owner steers. Its facing eases toward the way it moves
// (apply GetLocomotion().yawStep() to the actor each frame that time passes; turn
// bOrientRotationToMovement off), and pivots (a running turn or turning start well round, a turn on
// the spot (SetFace)) move and turn the capsule by the clip's root motion: the anim instance plays
// root motion from everything and hands the movement component each frame's stretch of the turn
// clip. Any other running turn or turning start rides the capsule's own motion. Any other owner turns
// itself (by GetLocomotion().turnYaw() through a pivot) and replays a lead's running turn with
// ReplayTurn. The root bone is held at its ref pose, rotation and all.
//
// -LocoLog logs the speed, the intent, the clips and their weights and rates: on each change of state,
// ten times a second through a start or stop, otherwise about once a second. -LocoNoMoves plays no
// starts or stops (SetMovesFromGait sets none), for comparing against.

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/BoneReference.h"

#include "ftcore/FootHold.h"
#include "ftcore/Locomotion.h"
#include "ftcore/MoveTuning.h"
#include "ftcore/Slide.h"
#include "ftcore/Traversal.h"

#include "FTLocomotionAnimInstance.generated.h"

class UAnimSequence;
class UCharacterMovementComponent;
class USkeletalMesh;
class USkeletalMeshComponent;

// -LocoSlide: once a second, logs how fast the planted foot moves over the ground against the body's
// speed: each plant's net slip from start to end (and how much of it is forward and sideways), and
// the frame-to-frame speed. The planted foot is the slower of the ball bones within 3 cm of the
// lowest ball height (in the mesh's space) in the previous second. Each plant also counts, for the
// whole run, by what the body did over it (speeding up, braking, holding its speed), so starts and
// stops compare across runs. -LocoPlants also logs each plant as it ends, UTC-stamped: its length, its
// net slip forward and sideways, and the forward slip over its middle half alone (so slip from the
// foot still landing or already lifting shows apart from the stance's own). Call Sample while the
// mesh's pose and placement are from the same frame: a character's Tick, or a follower before it moves.
// bOffFoot: no foot is planted now (a follower in the air or carried by a traversal says so; a character's
// movement mode tells for it).
struct FFTFootSlideProbe
{
	void Sample(const USkeletalMeshComponent& Mesh, float DeltaSeconds, float BodySpeed, bool bOffFoot = false);

private:
	FVector Last[2] = {FVector::ZeroVector, FVector::ZeroVector};
	bool bHasLast = false;
	bool bEnabled = false;
	bool bChecked = false;
	float Floor = 1e9f;    // the lower foot's lowest height last window (none yet)
	float NextFloor = 1e9f;
	double SlideSum = 0.0, SpeedSum = 0.0;
	// The current plant (the foot, where it started, how long it has lasted) and the plants' net slip.
	int32 PlantFoot = INDEX_NONE;
	FVector PlantStart = FVector::ZeroVector;
	float PlantTime = 0.f;
	float PlantSpeed = 0.f; // the body's speed as the plant began
	// -LocoPlants: the plant's path so far (a plant longer than MaxPath frames keeps its first MaxPath).
	bool bPlantLog = false;
	static constexpr int32 MaxPath = 512;
	FVector2D Path[MaxPath];
	float PathTime[MaxPath] = {};
	int32 PathLen = 0;
	// The run's plants by what the body did: speeding up, braking, holding its speed.
	static constexpr int32 NumKinds = 3;
	double KindSlip[NumKinds] = {}, KindTime[NumKinds] = {};
	int32 KindPlants[NumKinds] = {};
	double SlipSum = 0.0, AlongSum = 0.0, SideSum = 0.0, SlipTime = 0.0;
	int32 Planted = 0, Frames = 0;
	float Timer = 0.f;
};

// Runs on the anim worker thread: PreUpdate (game thread) steps the locomotion and copies the
// layers out, Evaluate samples and blends them. Nothing here allocates per frame.
struct FFTLocomotionAnimProxy : public FAnimInstanceProxy
{
	FFTLocomotionAnimProxy() = default;
	explicit FFTLocomotionAnimProxy(UAnimInstance* InInstance) : FAnimInstanceProxy(InInstance) {}

	// The heaviest layers past this many are dropped (only a crossfade under way has more than a few).
	static constexpr int32 MaxLayers = 12;
	static constexpr int32 NumSpine = 3; // spine_01..03
	static constexpr int32 NumNeck = 2;  // neck_01, head
	// The air pose's bones: clavicle to hand on each side, then thigh to foot on each side.
	static constexpr int32 NumArmBones = 8;
	static constexpr int32 NumLimbs = 14;
	// The fist's bones on each hand: index, middle, ring and pinky _01.._03, then thumb_01.._03.
	static constexpr int32 NumFingerBones = 15;

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	// The feet held (ftcore::FootHold), on the pose in component space.
	void HoldFeet(FCSPose<FCompactPose>& CS, const FBoneContainer& Container);
	struct FLayer
	{
		const UAnimSequence* Seq = nullptr;
		float Time = 0.f;
		float Weight = 0.f;
		bool bLoop = false;
	};
	FLayer Layers[MaxLayers];
	int32 NumLayers = 0;
	// The layers before the actions (the locomotion's), and the actions' upper-body share (SetActionUpper): the bones
	// off UpperMask (the pelvis, the legs) blend back to those layers alone by it. UpperMask is per compact bone, made
	// with the bones.
	// LocoWeight keeps the locomotion layers' own weights (summing to 1), before the actions scale them down: an action
	// at full weight leaves them nothing in the blend, and the legs would drop to the action's clip.
	int32 NumLoco = 0;
	float LocoWeight[MaxLayers] = {};
	float UpperShare = 0.f;
	TArray<bool> UpperMask;
	// The spine's bend this frame, in component space: one rotation shared over the spine bones.
	FQuat SpineBend = FQuat::Identity;
	// The legs turned from the body's facing by this (degrees, + right) about the root (SetLegYaw).
	float LegYaw = 0.f;
	// The feet held (SetFootLock): each foot on the floor stays where it stood in the world while the body moves over
	// it, by two-bone IK on the leg, and the pelvis sinks under them (ftcore::FootHold has the rule). A foot the clip
	// lifts (its ball kLiftedAbove over its rest height) goes where the clip takes it.
	static constexpr float kLiftedAbove = 4.f;  // cm over the ball's rest height: the clip lifts the foot
	// The pelvis sinks so a moved foot is within kLegReach of its leg's length (the clips' legs stand nearly straight).
	static constexpr float kLegReach = 0.96f;
	FBoneReference PelvisBone;
	bool bFootLock = false;
	ftcore::FootHold Feet;
	uint32 FeetResetSeen = 0;
	bool bHaveLastRoot = false;
	FVector LastRoot = FVector::ZeroVector; // the component's place at the last PreUpdate, world
	float RootMoved = 0.f;                  // cm since then
	FTransform ToWorld = FTransform::Identity; // the component's, at PreUpdate
	float HoldDt = 0.f;
	FVector LegForward = FVector::ForwardVector;
	FBoneReference BallBone[2];
	float BallRest[2] = {0.f, 0.f}; // the balls' heights in the ref pose, component space
	// The head's on top of it (Locomotion's headPitch, headRoll, headYaw), shared over the neck bones.
	FQuat HeadBend = FQuat::Identity;
	// The air pose (Locomotion::limbs), and its weight on the arms and on the legs.
	FLayer LimbPose;
	float LimbWeight[2] = {0.f, 0.f};
	// The elbows held bent at least ElbowBend degrees (Locomotion::elbowHold), by this weight; a
	// straight arm bends toward ElbowForward (the body's forward, in component space).
	float ElbowHold = 0.f;
	float ElbowBend = 90.f;
	FVector ElbowForward = FVector::ForwardVector;
	// The arm aim (SetArmAim), its target in component space.
	float AimWeight = 0.f;
	int32 AimSide = 1;
	FVector AimTarget = FVector::ZeroVector;
	FVector AimAxis = FVector::ForwardVector;
	float AimReach = 0.f;
	// The other hand's support of the aimed one (SetSupportHand): by SupportWeight its wrist beside and under the
	// aimed wrist, its fingers wrapped, for a two-handed hold.
	float SupportWeight = 0.f;
	// A hand in the pistol grip shape (SetHandGrip): the index along the trigger guard, the rest wrapped round.
	bool bGrip[2] = {false, false};
	// The hands' fist curl (SetHandCurl; below 0 the clip's own fingers), and per finger bone the curl's axis in its
	// parent's space and its ref-pose rotation (bound with the bones; bFingers when a hand has them all).
	float Curl[2] = {-1.f, -1.f};
	FBoneReference Finger[2][NumFingerBones];
	FVector FingerAxis[2][NumFingerBones];
	FQuat FingerRef[2][NumFingerBones];
	bool bFingers[2] = {false, false};

	FBoneReference Spine[NumSpine];
	FBoneReference Neck[NumNeck];
	FBoneReference Limb[NumLimbs];
	uint16 BoundSerial = 0;
	bool bBound = false;
};

UCLASS(Transient)
class FORGOTTENTHIRD_API UFTLocomotionAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	// Sets (or with nullptr clears) one of the clips, measuring a walk or run clip's gait.
	void SetClip(ftcore::LocoClip Which, UAnimSequence* Clip);
	// The clips a look sets (LocoClip order, Idle to LandMoving); the rest are found beside the gaits and the run.
	static constexpr int32 NumBaseClips = static_cast<int32>(ftcore::LocoClip::LandMoving) + 1;
	// Sets the base clips (a null one clears its slot), then each gait's start and stops, the turns, the air pose,
	// the crouch and, with bTraversal, the climbs and vaults, from the clips found beside them (below).
	void SetBaseClips(UAnimSequence* const (&Base)[NumBaseClips], bool bTraversal);
	// The base clip retargeted onto Body in Folder, as FTBuildRig names one (<source>_to_<mesh>): the mocap
	// sample's (M_Neutral_Run_Loop_F_to_x), else the template's (MF_Unarmed_Jog_Fwd_to_x). Null when there is none
	// made for Body's skeleton.
	static UAnimSequence* FindRetargeted(ftcore::LocoClip Which, const FString& Folder, const USkeletalMesh& Body, const TCHAR* Who);
	// Whether Clip was made for Body's skeleton (a clip for another one can't pose it); if not, warns, naming Who.
	static bool FitsBody(const UAnimSequence& Clip, const USkeletalMesh& Body, const TCHAR* Who);
	// Sets a gait's start and stops from the clips named after its loop, where there are any: next to
	// M_Neutral_Run_Loop_F_to_x, M_Neutral_Run_Start_F_Lfoot_to_x (M_Relaxed_Run_Start_F_Lfoot_to_x
	// first) and M_Neutral_Run_Stop_F_Lfoot_to_x (and _Rfoot). Call after the gait's SetClip.
	void SetMovesFromGait(ftcore::LocoClip Gait);
	// Sets the air pose from the clip named after the run loop, where there is one: next to
	// M_Neutral_Run_Loop_F_to_x, M_Neutral_Jump_F_Off_Run_Lfoot_to_x (its last frame: arms bent
	// forward and down, knees tucked). Call after SetClip(Run). -LocoNoAirPose leaves it out.
	void SetAirPoseFromRun();
	// Sets the running turns, turning starts and turns on the spot from the clips named after the run
	// loop, where there are any: next to M_Neutral_Run_Loop_F_to_x, M_Neutral_Run_Turn_L_090_Lfoot_to_x,
	// M_Neutral_Run_Reface_Start_F_L_090_to_x, M_Neutral_Stand_Turn_090_L_to_x and so on. Call after
	// SetClip(Run). -LocoNoTurns leaves them out.
	void SetTurnsFromRun();
	// Sets the crouch from the clips named after the run loop, where there are any: next to
	// M_Neutral_Run_Loop_F_to_x, M_Neutral_Crouch_Loop_F_to_x (the crouch walk), M_Neutral_Crouch_Idle_Loop_to_x,
	// M_Neutral_Transition_Stand_to_Crouch_to_x and _Crouch_to_Stand, and M_Neutral_Crouch_Idle_Turn_090_L_to_x
	// and so on. Call after SetClip(Run). -LocoNoCrouch leaves them out.
	void SetCrouchFromRun();
	// Sets the traversal clips (a hurdle over a low wall, a mantle or a climb up onto a ledge, each from a stand, a
	// walk and a run off either foot, then a roll off either foot) from the clips named after the run loop, where
	// there are any: next to M_Neutral_Run_Loop_F_to_x, M_Neutral_Traversal_Hurdle_1_0_run_F_V2_Lfoot_to_x,
	// ..._Mantle_1_0_run_F_Lfoot_to_x, ..._Climb_Start_2_5_run_F_Lfoot_to_x, M_Neutral_Jump_F_Land_Roll_Lfoot_to_x
	// and so on, each measured into GetTraversal() (index = TraversalNames order; the rolls from their pelvis, by
	// ftcore::measureRoll). The slide's clips follow from FirstSlideClip, in ftcore::SlideState::clip's order
	// (M_Neutral_Slide_FootOut_Into_Lfoot_to_x, ..._Loop_to_x, ..._Out_Moving_Run_to_x and so on), each measured into
	// GetSlide(). The catches (a jump onto a ledge, its edge caught by the hands: from high up, then from a chest's and a
	// hip's height, each from a stand, a walk and a run) follow from FirstCatchClip
	// (M_Neutral_Traversal_Catch_Cliff_high_stand_to_x, ..._Catch_Mantle_med_stand_to_x and so on), each measured into
	// GetTraversal() by ftcore::measureCatch. Call after SetClip(Run). -LocoNoTraversal leaves them out.
	void SetTraversalFromRun();
	const ftcore::TraversalSet& GetTraversal() const { return Traversal; }
	const ftcore::SlideSet& GetSlide() const { return Slide; }
	// The movement tunables, read once from Data/Movement/movement.json (the built-in defaults without it, or with
	// one it refuses: logged). Every body's slide, traversals and landings take them as it initialises.
	static const ftcore::MoveTuning& GetMoveTuning();
	// Slide clip k (ftcore::SlideState::clip) plays as traversal clip FirstSlideClip + k.
	static constexpr int32 FirstSlideClip = 20;
	// The catches are traversal clips FirstCatchClip on, in the TraversalSet with the vaults and climbs.
	static constexpr int32 FirstCatchClip = FirstSlideClip + static_cast<int32>(ftcore::kSlideClips);
	static constexpr int32 NumCatchClips = 7;
	// Plays traversal clip Clip (a TraversalPlan's) at clip time Time over the locomotion, fading in over its first
	// moments and out by its end; the owner moves the body along TraversalSet::rootAt and calls this every frame,
	// then with Clip -1 (or LetGoTraversal) when it hands the body back: the clip plays on at its own pace while it
	// fades out over kTraversalOut (sooner by its end), and the locomotion takes the body back with the clip's feet
	// where it let go (ftcore::Locomotion::handBack). The clip's root is held like the locomotion's: the owner carries it.
	// A clip asked for while another plays takes over from it over kTraversalSwitch, the other playing on at its own
	// pace as it fades (a slide's clips, one after another). bLoop loops the clip, and neither a looping clip nor one
	// asked for with bEndFade false fades by its end: the owner goes on from it into another, or lets it go first.
	void SetTraversal(int32 Clip, float Time, bool bLoop = false, bool bEndFade = true)
	{
		if (Clip < 0)
		{
			LetGoTraversal(1.f, false);
			return;
		}
		if (Clip != TravClip && TravClip >= 0 && !bTravOut)
		{
			PrevClip = TravClip;
			PrevTime = TravTime;
			bPrevLoop = bTravLoop;
			PrevFor = 0.f;
		}
		else if (Clip != TravClip || bTravOut)
		{
			TravElapsed = 0.f;
			PrevClip = -1;
		}
		bTravOut = false;
		bTravHandBack = false;
		TravClip = Clip;
		TravTime = Time;
		bTravLoop = bLoop;
		bTravEndFade = bEndFade;
	}
	// Lets the traversal playing go, as SetTraversal(-1, ...) does, but the clip plays on at Rate (the pace the owner
	// played it at as it let go: its feet are handed back at that pace too) while it fades out, and with bTurn the
	// locomotion may start a turn under it (a roll's: its body is back on its feet, and may want another way).
	void LetGoTraversal(float Rate, bool bTurn)
	{
		if (TravClip >= 0 && !bTravOut)
		{
			bTravOut = true;
			bTravHandBack = true;
			TravOutFor = 0.f;
			TravOutFrom = TravWeight;
			TravOutRate = Rate;
			bTravOutTurns = bTurn;
		}
	}
	// The owner standing still turns on the spot to face Yaw (degrees, world) while bFace holds.
	void SetFace(bool bFace, float Yaw)
	{
		bPushedFace = bFace;
		PushedFaceYaw = Yaw;
	}
	// A non-character owner in the air (a replayed jump), rising while VerticalSpeed (cm/s) is above 0.
	void SetAir(bool bAir, float VerticalSpeed)
	{
		bPushedAir = bAir;
		PushedVerticalSpeed = VerticalSpeed;
	}
	// The arm aim, for a strike, shot or cast: by Weight (0..1, 0 off), the arm on Side (0 left, 1 right) is turned
	// by two-bone IK (upper arm, forearm, hand) so its wrist lies on the line from the shoulder to Target (world, cm),
	// Reach cm short of it, the arm no straighter than nearly straight, and the hand then turns so its local Axis
	// (the knuckles' or the barrel's way, in the hand bone's space) points at Target. On top of everything else.
	void SetArmAim(int32 Side, const FVector& Target, float Weight, const FVector& Axis, float Reach)
	{
		PushedAimSide = Side;
		PushedAimTarget = Target;
		PushedAimWeight = Weight;
		PushedAimAxis = Axis;
		PushedAimReach = Reach;
	}
	// The hands closed into fists by Left and Right (0..1; 1 a full fist, 0.5 half closed, below 0 the clip's own
	// fingers): each finger bone at its ref pose turned toward the palm (the thumb's side) by its share of the fist
	// (index to pinky 75-80, 90, 50 degrees from the knuckle out; thumb 25, 35, 45), after the clips and the air pose.
	void SetHandCurl(float Left, float Right)
	{
		PushedCurl[0] = Left;
		PushedCurl[1] = Right;
	}
	// A hand shaped round a pistol's grip in place of the fist (the index finger nearly straight along the trigger
	// guard, the other fingers wrapped, the thumb over): wins over SetHandCurl for that hand.
	void SetHandGrip(bool bLeft, bool bRight)
	{
		bPushedGrip[0] = bLeft;
		bPushedGrip[1] = bRight;
	}
	// While the right arm is aimed (SetArmAim, side 1): the left hand joins it by Weight (0..1), wrist under and beside
	// the right one, for a two-handed hold.
	void SetSupportHand(float Weight) { PushedSupport = Weight; }
	// The legs turned from the facing by Yaw degrees (+ right), the chest turned back by as much over the spine: a
	// body with forward gaits only walks along the way it moves while it faces elsewhere (a strafe). Any owner.
	void SetLegYaw(float Yaw) { PushedLegYaw = Yaw; }
	// The feet held where they stand while bLock holds (a strike whose clip has the legs; FFTLocomotionAnimProxy::
	// ftcore::FootHold), stepping when the body has gone too far over one. Any owner.
	void SetFootLock(bool bLock) { bPushedFootLock = bLock; }
	// Drops the held feet and the pelvis's sink at once, for a body put somewhere else (a teleport, a respawn, a death,
	// a hand-off): the old world anchors would stretch the legs. A jump of over 1.5 m in a frame does the same itself.
	void ResetFeet() { ++PushedFeetReset; }
	// A non-character owner crouches while bCrouch holds (a character's own crouch is read from it).
	void SetCrouch(bool bCrouch) { bPushedCrouch = bCrouch; }
	// A roll asked for and still kept (Locomotion's roll input): touching down with it, a moving body rolls out of
	// the landing. Any owner.
	void SetRollAsked(bool bAsked) { bPushedRoll = bAsked; }
	// A non-character owner: replay a running turn by Delta degrees on the next update.
	void ReplayTurn(float Delta) { PushedTurnBy = Delta; }
	// The owner's ground speed (cm/s) for this frame, for owners that aren't characters.
	void SetGroundSpeed(float Speed) { PushedSpeed = Speed; }
	// Such an owner following a lead: the lead's foot phase while it holds a gait, below 0 otherwise, and its
	// stride (ftcore::LocoInput::leadPhase and leadStride: the follower steps at the lead's rate, a quarter
	// stride off it), both as the frame began (LeadPhaseAtFrameStart).
	void SetLead(float Phase, float Stride)
	{
		PushedLeadPhase = Phase;
		PushedLeadStride = Stride;
	}
	// The foot phase and the stride as this frame began, whether this instance has updated yet or not: what a
	// follower, updating in either order, compares with its own phase before its update.
	float LeadPhaseAtFrameStart() const { return SteppedFrame == GFrameCounter ? PhaseBefore : Loco.phase(); }
	float LeadStrideAtFrameStart() const { return SteppedFrame == GFrameCounter ? StrideBefore : Loco.strideLength(); }
	// Where such an owner is heading, as ftcore::LocoInput's wantSpeed and stopDistance: the speed it
	// accelerates toward (0 while braking, below 0 unknown, which plays no starts or stops) and, while
	// braking, how far it will slide before it stands.
	void SetIntent(float WantSpeed, float StopDistance)
	{
		PushedWant = WantSpeed;
		PushedStop = StopDistance;
	}

	// A walking character's intent, for SetIntent: with input, the speed it accelerates toward; without,
	// 0 and the distance it brakes over (v^2 / 2b at its steady braking deceleration). Unknown (-1)
	// while it falls.
	static void MovementIntent(const UCharacterMovementComponent& Move, float& WantSpeed, float& StopDistance);

	// Whole-body action clips over the locomotion (a fight's strikes): a host sets clips in slots once, then every
	// frame clears the actions and adds the ones playing, each a slot, a time into its clip and a weight (summing to
	// at most 1; the locomotion keeps the rest, and the bends and the air pose give way by the same share). At most
	// MaxActions a frame (more are ignored); a slot without a clip adds nothing. Game thread.
	static constexpr int32 MaxActionClips = 20;
	static constexpr int32 MaxActions = 4;
	// Sets (or with nullptr clears) the clip in Slot (0..MaxActionClips-1); false for a slot out of range.
	bool SetActionClip(int32 Slot, UAnimSequence* Clip);
	UAnimSequence* GetActionClip(int32 Slot) const { return Slot >= 0 && Slot < MaxActionClips ? ActionClips[Slot].Get() : nullptr; }
	void ClearActions() { NumActions = 0; }
	// The share (0..1) of the actions that plays on the upper body only (ftcore::attackUpperShare): spine_01 and the
	// bones under it take the action, the pelvis and the legs keep the locomotion by that share (0: the whole body).
	void SetActionUpper(float Share) { ActionUpper = FMath::Clamp(Share, 0.f, 1.f); }
	void AddAction(int32 Slot, float Time, float Weight)
	{
		if (NumActions < MaxActions && Slot >= 0 && Slot < MaxActionClips && Weight > 0.f)
		{
			Actions[NumActions++] = {Slot, Time, Weight};
		}
	}

	ftcore::LocoTuning& Tuning() { return Loco.tuning(); }
	const ftcore::Locomotion& GetLocomotion() const { return Loco; }

	// The clip's gait (length, ground speed, strides per loop, left plant), measured from its root and
	// ball (or foot) bones. Zero ground speed when the clip has no stride.
	static ftcore::LocoClipInfo MeasureClip(const UAnimSequence& Clip);
	// A start or stop clip's root travel and feet over the time the root moves (ftcore::measureMove).
	static ftcore::LocoClipInfo MeasureMove(const UAnimSequence& Clip);
	// A take-off's or landing's times off and onto the ground, from its root's height (ftcore::measureAir).
	static ftcore::LocoClipInfo MeasureAir(const UAnimSequence& Clip);

protected:
	virtual void NativeInitializeAnimation() override;
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
	friend struct FFTLocomotionAnimProxy;
	// Game thread, once per update: reads the owner's movement and steps the locomotion.
	void Step(float DeltaSeconds);

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> Clips[59];
	static_assert(ftcore::kLocoClipCount == 59, "Clips[] holds one per LocoClip");
	// The traversal clips (the TraversalSet's), then the slide's, then the catches (the TraversalSet's again).
	static constexpr int32 NumTraversalClips = FirstCatchClip + NumCatchClips;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> TraversalClips[NumTraversalClips];
	// The action clips by slot (SetActionClip) and this frame's actions (AddAction).
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> ActionClips[MaxActionClips];
	struct FAction
	{
		int32 Slot = 0;
		float Time = 0.f;
		float Weight = 0.f;
	};
	FAction Actions[MaxActions];
	int32 NumActions = 0;
	float ActionUpper = 0.f;
	ftcore::TraversalSet Traversal;
	ftcore::SlideSet Slide;
	// The traversal playing (-1 none), its clip time, how long it has played, its weight over the locomotion and
	// whether it loops or fades by its end; once its owner has let it go (bTravOut), how long since, the weight it had
	// then, the pace it plays on at and whether the locomotion may turn under it, and until the next update
	// (bTravHandBack) that the locomotion has its feet to take the body back with.
	// The one it is taking over from (-1 none): its clip time, how long since and whether it loops.
	int32 TravClip = -1;
	float TravTime = 0.f;
	float TravElapsed = 0.f;
	float TravWeight = 0.f;
	bool bTravLoop = false;
	bool bTravEndFade = true;
	bool bTravOut = false;
	bool bTravHandBack = false;
	float TravOutFor = 0.f;
	float TravOutFrom = 0.f;
	float TravOutRate = 1.f;
	bool bTravOutTurns = false;
	// The run loop's root yaw at its first frame (degrees): the gaits' feet are measured turned to theirs, and a
	// traversal's are handed back turned to this.
	float GaitYaw0 = 0.f;
	int32 PrevClip = -1;
	float PrevTime = 0.f;
	float PrevFor = 0.f;
	bool bPrevLoop = false;

	ftcore::Locomotion Loco;
	// The actor's forward in the mesh component's space, for the spine bend.
	FVector ForwardCS = FVector::ForwardVector;
	float PushedSpeed = 0.f;
	float PushedWant = -1.f;
	float PushedStop = -1.f;
	float PushedTurnBy = 0.f;
	float PushedLeadPhase = -1.f;
	float PushedLeadStride = 0.f;
	// The phase and stride before the last update over time that passed, and the frame it ran in.
	uint64 SteppedFrame = MAX_uint64;
	float PhaseBefore = 0.f;
	float StrideBefore = 0.f;
	float PushedFaceYaw = 0.f;
	int32 PushedAimSide = 1;
	FVector PushedAimTarget = FVector::ZeroVector;
	float PushedAimWeight = 0.f;
	FVector PushedAimAxis = FVector::ForwardVector;
	float PushedAimReach = 0.f;
	float PushedCurl[2] = {-1.f, -1.f};
	bool bPushedGrip[2] = {false, false};
	float PushedSupport = 0.f;
	float PushedLegYaw = 0.f;
	bool bPushedFootLock = false;
	uint32 PushedFeetReset = 0;
	bool bPushedFace = false;
	bool bPushedAir = false;
	bool bPushedCrouch = false;
	bool bPushedRoll = false;
	float PushedVerticalSpeed = 0.f;
	float LastYaw = 0.f;
	bool bHasYaw = false;
	bool bLog = false;
	float LogTimer = 0.f;
	ftcore::LocoState LastState = ftcore::LocoState::Ground;
	// For the log: the body's highest point in its time in the air (so far, or its last) and its height last frame (cm).
	float AirTop = 0.f;
	float LastZ = 0.f;
	bool bLastAir = false;
};
