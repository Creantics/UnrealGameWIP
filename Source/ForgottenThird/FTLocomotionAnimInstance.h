#pragma once

// Anim class for a walking body: idle, walk, run, jump start, fall loop and landing clips (and the
// moving jump and landing, and each gait's start and left- and right-foot stops), blended by
// ftcore::Locomotion from how the owner moves. No Anim Blueprint needed.
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
// twist and pitch.
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
// stops compare across runs. Call Sample while the mesh's pose and placement are from the same
// frame: a character's Tick, or a follower before it moves.
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

	static constexpr int32 MaxLayers = static_cast<int32>(ftcore::kLocoClipCount);
	static constexpr int32 NumSpine = 3; // spine_01..03

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

	FBoneReference Spine[NumSpine];
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
	// M_Neutral_Run_Loop_F_to_x, M_Neutral_Run_Start_F_Lfoot_to_x and M_Neutral_Run_Stop_F_Lfoot_to_x
	// (and _Rfoot). Call after the gait's SetClip.
	void SetMovesFromGait(ftcore::LocoClip Gait);
	// The owner's ground speed (cm/s) for this frame, for owners that aren't characters.
	void SetGroundSpeed(float Speed) { PushedSpeed = Speed; }
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
	TObjectPtr<UAnimSequence> Clips[18];
	static_assert(ftcore::kLocoClipCount == 18, "Clips[] holds one per LocoClip");

	ftcore::Locomotion Loco;
	// The actor's forward in the mesh component's space, for the spine bend.
	FVector ForwardCS = FVector::ForwardVector;
	float PushedSpeed = 0.f;
	float PushedWant = -1.f;
	float PushedStop = -1.f;
	float LastYaw = 0.f;
	bool bHasYaw = false;
	bool bLog = false;
	float LogTimer = 0.f;
	ftcore::LocoState LastState = ftcore::LocoState::Ground;
};
