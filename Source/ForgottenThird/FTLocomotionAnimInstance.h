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
// (apply GetLocomotion().yawStep() to the actor each frame; turn bOrientRotationToMovement off), and
// running turns, turning starts and turns on the spot (SetFace) move and turn the capsule by the
// clip's root motion: the anim instance plays root motion from everything and hands the movement
// component each frame's stretch of the turn clip. Any other owner turns itself (by
// GetLocomotion().turnYaw() while turning) and replays a lead's running turn with ReplayTurn. The root
// bone is held at its ref pose, rotation and all.
//
// -LocoLog logs the speed, the intent, the clips and their weights and rates: on each change of state,
// ten times a second through a start or stop, otherwise about once a second. -LocoNoMoves plays no
// starts or stops (SetMovesFromGait sets none), for comparing against.

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/BoneReference.h"

#include "ftcore/Locomotion.h"

#include "FTLocomotionAnimInstance.generated.h"

class UAnimSequence;
class UCharacterMovementComponent;
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
struct FFTFootSlideProbe
{
	void Sample(const USkeletalMeshComponent& Mesh, float DeltaSeconds, float BodySpeed);

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

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	struct FLayer
	{
		const UAnimSequence* Seq = nullptr;
		float Time = 0.f;
		float Weight = 0.f;
		bool bLoop = false;
	};
	FLayer Layers[MaxLayers];
	int32 NumLayers = 0;
	// The spine's bend this frame, in component space: one rotation shared over the spine bones.
	FQuat SpineBend = FQuat::Identity;
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
	// A non-character owner: replay a running turn by Delta degrees on the next update.
	void ReplayTurn(float Delta) { PushedTurnBy = Delta; }
	// The owner's ground speed (cm/s) for this frame, for owners that aren't characters.
	void SetGroundSpeed(float Speed) { PushedSpeed = Speed; }
	// Such an owner following a lead: the lead's foot phase while it holds a gait, below 0 otherwise
	// (ftcore::LocoInput::leadPhase: the follower steps a quarter stride off it).
	void SetLeadPhase(float Phase) { PushedLeadPhase = Phase; }
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

	ftcore::LocoTuning& Tuning() { return Loco.tuning(); }
	const ftcore::Locomotion& GetLocomotion() const { return Loco; }

	// The clip's gait (length, ground speed, strides per loop, left plant), measured from its root and
	// ball (or foot) bones. Zero ground speed when the clip has no stride.
	static ftcore::LocoClipInfo MeasureClip(const UAnimSequence& Clip);
	// A start or stop clip's root travel and feet over the time the root moves (ftcore::measureMove).
	static ftcore::LocoClipInfo MeasureMove(const UAnimSequence& Clip);

protected:
	virtual void NativeInitializeAnimation() override;
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
	friend struct FFTLocomotionAnimProxy;
	// Game thread, once per update: reads the owner's movement and steps the locomotion.
	void Step(float DeltaSeconds);

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> Clips[47];
	static_assert(ftcore::kLocoClipCount == 47, "Clips[] holds one per LocoClip");

	ftcore::Locomotion Loco;
	// The actor's forward in the mesh component's space, for the spine bend.
	FVector ForwardCS = FVector::ForwardVector;
	float PushedSpeed = 0.f;
	float PushedWant = -1.f;
	float PushedStop = -1.f;
	float PushedTurnBy = 0.f;
	float PushedLeadPhase = -1.f;
	float PushedFaceYaw = 0.f;
	bool bPushedFace = false;
	bool bPushedAir = false;
	float PushedVerticalSpeed = 0.f;
	float LastYaw = 0.f;
	bool bHasYaw = false;
	bool bLog = false;
	float LogTimer = 0.f;
	ftcore::LocoState LastState = ftcore::LocoState::Ground;
};
