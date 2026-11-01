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
// actor); the root bone's translation is held at its reference pose unless SetHoldRoot(false).

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
};
