#pragma once

// A special move's cinematic in the arena (ftcore/SpecialCine.h): the camera that plays its shot list, and the
// freeze that holds everything but the performer while it plays.
//
// UFTSpecialCineModifier: the runner hands it each frame's ftcore::SpecialCineFrame (positions already in Unreal
// units). It runs after every other camera modifier (priority 255) and blends from the camera as drawn (the close
// camera keeps computing under it), moved by PlayShift (the lead's travel: the play camera at the move's end
// position), toward the shot by 1 - playWeight: place, facing and field of view; the shot's shake (a place offset)
// is added by the same weight. Its depth of field (focal distance, f-stop, a 36 mm sensor) is blended likewise. A
// hard cut marks the camera manager's cut flag for that frame (no motion blur or history across it).
// Idle (Clear): it leaves the view and the post process alone.
//
// FFTSpecialFreeze: Begin pauses every actor in the world but the performer, the actors attached to it and the
// world's bookkeeping actors (controllers, camera managers, HUDs, AInfo: game mode, states, world settings) by
// their own CustomTimeDilation (Factor, about 0.001), and every Niagara component they own likewise; the world's
// own clock keeps running, so the performer, its effects and the camera play at full speed. End gives back every
// value it changed, as it found it. The lists are built once at Begin (no per-frame allocation).

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "FTSpecialCine.generated.h"

class AActor;
class UNiagaraComponent;
class UWorld;

UCLASS()
class FORGOTTENTHIRD_API UFTSpecialCineModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	UFTSpecialCineModifier();

	// This frame's shot camera (cm, degrees). PlayWeight 1: the play camera untouched; 0: the shot's.
	struct FShot
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float FovDegrees = 60.f;
		float FocusCm = 100.f;
		float FStop = 2.8f;
		FVector ShakeCm = FVector::ZeroVector;     // added to the shot's place (world axes)
		FVector PlayShiftCm = FVector::ZeroVector; // added to the play camera's place before the blend
		float PlayWeight = 1.f;
		bool bCut = false;
	};
	void SetShot(const FShot& InShot) { Shot = InShot; bOn = true; }
	void Clear() { bOn = false; }

	// The last view this modifier wrote and the one it was handed (the no-pop check reads both).
	const FMinimalViewInfo& LastIn() const { return In; }
	const FMinimalViewInfo& LastOut() const { return Out; }

	virtual bool ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV) override;

protected:
	virtual void ModifyPostProcess(float DeltaTime, float& PostProcessBlendWeight, FPostProcessSettings& PostProcessSettings) override;

private:
	FShot Shot;
	bool bOn = false;
	FMinimalViewInfo In;
	FMinimalViewInfo Out;
};

struct FFTSpecialFreeze
{
	// Pauses the world but `Performer` (and what hangs off it). A second Begin while frozen: ignored.
	void Begin(UWorld* World, const AActor* Performer, float Factor);
	void End();
	bool IsOn() const { return bOn; }
	int32 ActorCount() const { return Actors.Num(); }
	int32 EffectCount() const { return Effects.Num(); }
	// Held actors whose dilation is no longer the factor Begin set (something else wrote it while frozen).
	int32 CountChanged() const;

private:
	struct FActorWas { TWeakObjectPtr<AActor> Actor; float Dilation = 1.f; };
	struct FEffectWas { TWeakObjectPtr<UNiagaraComponent> Effect; float Dilation = 1.f; };
	TArray<FActorWas> Actors;
	TArray<FEffectWas> Effects;
	float HeldAt = 1.f;
	bool bOn = false;
};
