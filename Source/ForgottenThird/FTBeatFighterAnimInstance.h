#pragma once

// Anim class of a beat boss fighter (docs/BEAT_BOSS_DEMO.md, "The stage"): it plays the clip the choreography
// (ftcore::BeatChoreo) names at the clip time it names, with a short crossfade whenever the clip changes, so
// there is no pop back to the stance. No Anim Blueprint needed.
//
//   Mesh->SetAnimInstanceClass(UFTBeatFighterAnimInstance::StaticClass());
//   Anim->SetPose(Seq, Time, bLoop, DeltaSeconds);    // every frame, from ChoreoSample::lead or ::boss
//
// A gait mix (ftcore::gaitMixStep) is one crossfade slot of two looping clips at the times and shares the caller gives,
// faded in at the weight the caller gives (SetMix); changes inside it never start a new crossfade.
//
// The clips are retargeted onto the test human's skeleton; the pose is sampled at exactly the time given (a
// hit-stop is the same time twice, the finisher's slow motion is the choreography's own times). The clip a
// pose fades out of is held at the last time it was given. Root motion is never extracted (the host places the
// actor); the root bone's translation is held at its reference pose unless SetHoldRoot(false) (a fall lets it go).
//
// A clip set to fade in additively (bAdditiveIn, a short hit reaction) also puts its own motion since its first frame on
// top of the blend while it fades in, by what is left of the fade: the pose it cuts in from takes the jolt at its full
// size at once, then settles into the clip. Cut off before its fade is done, it keeps that share and lets it go as it
// fades out, so a second blow mid-fade does not pop.
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

	static constexpr int32 MaxSlots = 3;               // the clip showing and two fading out of it
	static constexpr int32 MaxLayers = 2 * MaxSlots;   // a slot that is a gait mix samples two clips

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	struct FLayer
	{
		const UAnimSequence* Seq = nullptr;
		float Time = 0.f;
		float Weight = 0.f;
		bool bLoop = false;
		float Additive = 0.f; // the clip's own motion since its first frame, put on top of the blend at this weight
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
	// The crossfade between two clips, seconds, unless the caller gives its own.
	static constexpr float BlendSeconds = 0.12f;

	// The clip to show now, at clip time Time. Call it every frame. A different clip from last frame's fades in over
	// Blend seconds while the last fades out; a null clip leaves the pose as it was. bAdditiveIn: it fades in additively.
	void SetPose(const UAnimSequence* Seq, float Time, bool bLoop, float DeltaSeconds, float Blend = BlendSeconds, bool bAdditiveIn = false);
	// A gait mix to show now: SeqA at TimeA and SeqB at TimeB (both looping), SeqB taking ShareB of the slot, the slot at
	// Weight over the clips it fades in over (ftcore::GaitMixFrame: the layers and the slot). Call it every frame it shows.
	// Coming after a clip it starts a crossfade over it (at the weight given, not a blend time); anything inside the
	// mix changing (a gait, a share, a reverse) stays in the one slot. A null SeqA leaves the pose as it was.
	void SetMix(const UAnimSequence* SeqA, float TimeA, const UAnimSequence* SeqB, float TimeB, float ShareB, float Weight);
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
	// The clip showing now (the heavier of a gait mix's two), for logs and tests.
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
		bool bAdditiveIn = false;
		// Once faded out of: the share of its own motion still put on top, per unit of its weight.
		float Additive = 0.f;
		// A gait mix (SetMix): Seq2 at Time2 takes Share2 of the slot (Seq, at Time, the rest).
		bool bMix = false;
		const UAnimSequence* Seq2 = nullptr;
		float Time2 = 0.f;
		float Share2 = 0.f;
	};
	// Slot 0 is the clip now showing; 1 and 2 are the ones fading out of it.
	FSlot Slots[FFTBeatFighterProxy::MaxSlots];

	// The clip showing becomes the first to fade out of; the one that was, the second.
	void PushSlot();
	// Slot 0's weight set: the ones fading out share what is left in the proportion they had.
	void ShareRest();
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
