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
// their own CustomTimeDilation (Factor, about 0.001); the world's own clock keeps running, so the performer, its
// effects and the camera play at full speed. What that dilation does not reach is held by hand on the same actors:
//   * Niagara: a batched system ticks on the world's clock, not its owner's, so each is made solo (its component
//     tick, which the owner's dilation scales). Its own dilation is left as found: the owner's already gives Factor
//     (scaling it as well gave Factor squared).
//   * Sounds playing on them are paused.
//   * Bodies simulating physics (ragdolls, loose props): gravity off, velocities zeroed, put to sleep. A contact can
//     still wake one; nothing in the frozen fight moves into them (the performer's capsule only on its own path).
//   * Cloth on their skeletal meshes is suspended (it steps on the physics clock).
//   * An actor spawned while frozen is held as it comes (the performer's own spawns, owned or instigated by it, are
//     not).
// End gives back every value it changed, as it found it, and only those. The lists are built at Begin and on a spawn,
// never per frame. Rain is not an actor here: the runner slows it on its own (UFTRainSubsystem).

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "FTSpecialCine.generated.h"

class AActor;
class UAudioComponent;
class UNiagaraComponent;
class UPrimitiveComponent;
class USkeletalMeshComponent;
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
	// Gives everything back. Safe to call when not frozen, and on a world being torn down.
	void End();
	bool IsOn() const { return bOn; }
	int32 ActorCount() const { return Actors.Num(); }
	int32 EffectCount() const { return Effects.Num(); }
	int32 SoundCount() const { return Sounds.Num(); }
	int32 BodyCount() const { return BodiesHeld.Num(); }
	int32 ClothCount() const { return Cloths.Num(); }
	int32 SpawnCount() const { return Spawned; }
	// Held actors whose dilation is no longer the factor Begin set (something else wrote it while frozen).
	int32 CountChanged() const;

private:
	// One actor held: its dilation, its effects, sounds, bodies and cloth.
	void Hold(AActor* A);
	void OnSpawned(AActor* A);

	struct FActorWas { TWeakObjectPtr<AActor> Actor; float Dilation = 1.f; };
	struct FEffectWas { TWeakObjectPtr<UNiagaraComponent> Effect; bool bSolo = false; };
	// A body by its component and index (a skeletal mesh's Bodies; INDEX_NONE: the component's own body), found
	// again at End (a recreated physics state frees the old pointers).
	struct FBodyWas
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		int32 Index = INDEX_NONE;
		FVector Linear = FVector::ZeroVector;
		FVector Angular = FVector::ZeroVector; // radians a second
		bool bGravity = true;
	};
	TArray<FActorWas> Actors;
	TArray<FEffectWas> Effects;
	TArray<TWeakObjectPtr<UAudioComponent>> Sounds;
	TArray<FBodyWas> BodiesHeld;
	TArray<TWeakObjectPtr<USkeletalMeshComponent>> Cloths;
	TWeakObjectPtr<UWorld> HeldWorld;
	TWeakObjectPtr<const AActor> HeldPerformer;
	FDelegateHandle SpawnHandle;
	int32 Spawned = 0;
	float HeldAt = 1.f;
	bool bOn = false;
};
