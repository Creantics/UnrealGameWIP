#pragma once

// Anim class of a beat boss fighter (docs/BEAT_BOSS_DEMO.md, "The stage"): it plays the clip the choreography
// (ftcore::BeatChoreo) names at the clip time it names, with a short crossfade whenever the clip changes, so
// there is no pop back to the stance. No Anim Blueprint needed.
//
//   Mesh->SetAnimInstanceClass(UFTBeatFighterAnimInstance::StaticClass());
//   Anim->SetPose(Seq, Time, bLoop, DeltaSeconds);    // every frame, from ChoreoSample::lead or ::boss
//
// The clips are retargeted onto the test human's skeleton; the pose is sampled at exactly the time given (a
// hit-stop is the same time twice, the finisher's slow motion is the choreography's own times). The clip a
// pose fades out of is held at the last time it was given. Root motion is never extracted (the host places the
// actor); the root bone's translation is held at its reference pose unless SetHoldRoot(false) (a fall lets it go).
//
// A build and a lean can be put on the pose after the clips are blended (the boss's mass and its swing):
//   Anim->SetBuild(1.15f, 0.85f, 1.25f);  // net sizes of the shoulders and chest (spine_03 and all it carries), the head, the hands
//   Anim->SetLean(12.f);                  // the upper body pitched this many degrees towards where the body faces (negative: back)
//   Anim->SetGuardDown(1.f);              // 0..1: the arms lowered from a guard held at the face (the upper arms down, the elbows opened)
//   Anim->SetArmDrop(30.f, 0.f);          // degrees each upper arm (right, left) is swung down from where the clip has it: a blow aimed lower
// The scales are on the bones' local scale, divided by the parent's net size, so they do not shear a rotated limb; the
// lean turns spine_03 in component space about the line across the hips, the same way for every clip.

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"

#include "FTBeatFighterAnimInstance.generated.h"

class UAnimSequence;

// Runs on the anim worker thread: PreUpdate (game thread) copies the layers out, Evaluate samples and blends them.
// Nothing here allocates per frame.
struct FFTBeatFighterProxy : public FAnimInstanceProxy
{
	FFTBeatFighterProxy() = default;
	explicit FFTBeatFighterProxy(UAnimInstance* InInstance) : FAnimInstanceProxy(InInstance) {}

	static constexpr int32 MaxLayers = 3;

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
	bool bHoldRoot = true;
	float BuildChest = 1.f;
	float BuildHead = 1.f;
	float BuildHands = 1.f;
	float Lean = 0.f;
	float GuardDown = 0.f;
	float ArmDropR = 0.f;
	float ArmDropL = 0.f;

	void ApplyBuild(FPoseContext& Output) const;
};

UCLASS(Transient)
class FORGOTTENTHIRD_API UFTBeatFighterAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	// The crossfade between two clips, seconds.
	static constexpr float BlendSeconds = 0.12f;

	// The clip to show now, at clip time Time. Call it every frame. A different clip from last frame's fades in over
	// BlendSeconds while the last fades out; a null clip leaves the pose as it was.
	void SetPose(const UAnimSequence* Seq, float Time, bool bLoop, float DeltaSeconds);
	void SetHoldRoot(bool bHold) { bHoldRoot = bHold; }
	// With it on, the same clip begun again (its time jumps back) fades in over itself as a new clip does.
	void SetFadeRestart(bool bFade) { bFadeRestart = bFade; }
	void SetBuild(float InChest, float InHead, float InHands)
	{
		BuildChest = InChest;
		BuildHead = InHead;
		BuildHands = InHands;
	}
	void SetLean(float Degrees) { Lean = Degrees; }
	void SetGuardDown(float Amount) { GuardDown = Amount; }
	void SetArmDrop(float Right, float Left)
	{
		ArmDropR = Right;
		ArmDropL = Left;
	}
	// The clip showing now (the heaviest layer), for logs and tests.
	const UAnimSequence* GetClip() const { return Slots[0].Seq; }

	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
	friend struct FFTBeatFighterProxy;

	struct FSlot
	{
		const UAnimSequence* Seq = nullptr;
		float Time = 0.f;
		float Weight = 0.f;
		bool bLoop = false;
	};
	// Slot 0 is the clip now showing; 1 and 2 are the ones fading out of it.
	FSlot Slots[FFTBeatFighterProxy::MaxLayers];
	bool bHoldRoot = true;
	bool bFadeRestart = false;
	float BuildChest = 1.f;
	float BuildHead = 1.f;
	float BuildHands = 1.f;
	float Lean = 0.f;
	float GuardDown = 0.f;
	float ArmDropR = 0.f;
	float ArmDropL = 0.f;
};
