#pragma once

// The first Arena boss's effects (docs/VFX_UE.md's code-built systems): a small pool of AFTVfxActor spawned once, each slot
// keeping one effect, played and moved by UFTArenaRunner from the fight's state. Five effects:
//   Tell    looping at a blow's landing point through its startup (a lane: marks along it), stopped when it turns active;
//   Impact  one-shot there when the blow turns active;
//   Chips   one-shot on the boss's surface toward the lead when a lead's blow lands on it;
//   Crit    one-shot at the weak point when the boss's rules score a crit;
//   Glow    looping at the weak point while the weak points are open, moved there each frame.
// Which effect each plays, its user parameters and the offsets the runner places them by are read from
// Data/Battle/arena_boss/stone_bearer_vfx.json (the defaults below when it is missing). Every play logs one Display line
// ("Arena VFX: ...").
//
// Nothing here allocates after Spawn: a play moves a slot and restarts its Niagara component.

#include "CoreMinimal.h"

#include "FTArenaBossVfx.generated.h"

class AFTVfxActor;
class UWorld;

USTRUCT()
struct FFTArenaBossVfx
{
	GENERATED_BODY()

	enum class EEffect : uint8 { Tell, Impact, Chips, Crit, Glow };
	static constexpr int32 EffectCount = 5;
	// Slots: MaxTellMarks tells (a lane's marks), then 2 impacts, 3 chips, 2 crits and 1 glow (SlotCount in the .cpp).
	static constexpr int32 MaxTellMarks = 4;
	static constexpr int32 MaxSlots = 12;

	// One effect: its Data/Vfx name and user parameters.
	struct FLook
	{
		FName Effect;
		float SpeedScale = 1.f;
		float ShapeScale = 1.f;
	};

	struct FTuning
	{
		FLook Looks[EffectCount];
		float Lift = 0.05f;       // m: tells and impacts sit this far over the ground
		float CalledAhead = 1.2f; // m: a called impact to a side lands this far ahead of the boss ...
		float CalledSide = 1.8f;  // ... and this far to that side
		float ReachShare = 0.8f;  // an arc blow lands at this share of its reach
		int32 LaneMarks = 3;      // tells along a lane (1..MaxTellMarks)
		float AreaScale = 2.5f;   // an all-round blow's tell and impact: their ShapeScale times this
		float Surface = 0.9f;     // a hit's chips: this share of the boss's body radius out from its centre
		float WeakDepth = 0.85f;  // no weak-point bone: the point this share of the body radius out, on its gate's side
		float FlashUp = 0.3f;       // m: the crit flash and the weak-point glow sit this far above the weak point ...
		float FlashToCamera = 0.5f; // ... and this far from it toward the camera, clear of the carried piece
	};

	// Reads the tunables (File, relative to the project), loads the systems and spawns the pool in World. Once: later calls
	// do nothing until Destroy. An effect whose system doesn't load is off, with one warning. With -ArenaNoBossFx: nothing,
	// with one note.
	void Spawn(UWorld* World, const FString& File);
	void Destroy();
	bool IsSpawned() const { return bSpawned; }
	const FTuning& GetTuning() const { return Tuning; }

	// Stops everything (a restart, the fight's end).
	void StopAll();
	// Plays Effect at At, its ShapeScale times ShapeMul, on the effect's next slot. Why (null: no log line) says what set it
	// off. False when the effect is off.
	bool Play(EEffect Effect, const FVector& At, float ShapeMul, const TCHAR* Why);
	// The looping tells: stops every playing one.
	void StopTells();
	bool AnyTells() const { return bTells; }
	// The weak point's glow: on at At, off (its live particles die at once), or moved to At while on.
	void SetGlow(bool bOn, const FVector& At);
	void MoveGlow(const FVector& At);
	bool IsGlowing() const { return bGlowing; }

	UPROPERTY()
	TObjectPtr<AFTVfxActor> Slots[12]; // MaxSlots (a literal for UHT; checked in the .cpp)

private:
	void ReadTuning(const FString& File);
	static int32 FirstSlot(EEffect Effect);
	AFTVfxActor* NextSlot(EEffect Effect);

	FTuning Tuning;
	int32 Next[EffectCount] = {};
	bool bOn[EffectCount] = {};
	bool bSpawned = false;
	bool bTells = false;
	bool bGlowing = false;
};
