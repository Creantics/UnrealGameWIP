#pragma once

// The first Arena boss's effects (docs/VFX_UE.md's code-built systems): a small pool of AFTVfxActor spawned once, each slot
// keeping one effect, played and moved by UFTArenaRunner from the fight's state. Seven effects:
//   Tell        looping at a blow's landing point through its startup (a lane: marks along it), stopped when it turns active;
//   Impact      one-shot there when the blow turns active (a blow in the air: at its contact, over the tell);
//   Chips       one-shot on the boss's surface toward the lead when a lead's light blow lands on it;
//   Crit        one-shot at the weak point when the boss's rules score a crit;
//   Glow        looping at the weak point while the weak points are open, moved there each frame; at shut its live
//               particles finish their short lives (a fade of a few frames);
//   ChipsHeavy  Chips for a heavy blow, a riposte, a Release or a counter-stagger;
//   TellArea    Tell for an all-round blow (its ring drawn at the area's size).
// Which effect each plays, its user parameters and the offsets the runner places them by are read from
// Data/Battle/arena_boss/stone_bearer_vfx.json (the defaults below when it is missing). Every play logs one Display line
// ("Arena VFX: ...").
//
// Besides the pool, the marked move's tell light: one rect light (URectLightComponent on its own actor, made once in Spawn
// and reused), a bar of warm white over the floor along the move's lane, facing down, casting no shadows. The runner turns
// it on and off through the window Core gives (ftcore::tellLightWindow): LightOn ramps it in over a few frames, LightOff
// ramps it out at the impact or cuts it at once on an interrupt, and TickLight steps the ramp once a frame. Its tunables
// are the json's optional "tellLight" object (the defaults below when it is missing).
//
// Nothing here allocates after Spawn: a play moves a slot and restarts its Niagara component; the light only moves and
// changes its intensity.

#include "CoreMinimal.h"

#include "FTArenaBossVfx.generated.h"

class AActor;
class AFTVfxActor;
class URectLightComponent;
class UWorld;

USTRUCT()
struct FFTArenaBossVfx
{
	GENERATED_BODY()

	enum class EEffect : uint8 { Tell, Impact, Chips, Crit, Glow, ChipsHeavy, TellArea };
	static constexpr int32 EffectCount = 7;
	// Slots: MaxTellMarks tells (a lane's marks), then 2 impacts, 3 chips, 2 crits, 1 glow, 2 heavy chips and 1 area tell
	// (SlotCount in the .cpp).
	static constexpr int32 MaxTellMarks = 4;
	static constexpr int32 MaxSlots = 15;

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
		// The tell light ("tellLight": {"up", "across", "candelaPerMetre", "kelvin", "past", "barnDoorDeg", "barnDoorLength",
		// "rampIn", "rampOut"}): a bar over the lane from the boss's edge to its end, LightUp m over the floor, LightAcross m
		// across the lane; its intensity LightCandelaPerMetre cd for each metre of its length (about 23 lux on the floor
		// under it at the defaults: pi * 8 / (2 * 0.55)); its reach LightPast m past the bar's ends; barn doors LightBarnDeg
		// degrees open (0: straight down, 88: none) and LightBarnLength m long, so its pool on the floor stays narrow across
		// the lane; full over LightRampIn frames, out over LightRampOut.
		float LightUp = 0.55f;
		float LightAcross = 0.3f;
		float LightCandelaPerMetre = 8.f;
		float LightKelvin = 3500.f;
		float LightPast = 0.75f;
		float LightBarnDeg = 25.f;
		float LightBarnLength = 0.2f;
		int32 LightRampIn = 3;  // 2..3
		int32 LightRampOut = 2; // 1..2
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
	// The looping tells (Tell and TellArea): stops every playing one.
	void StopTells();
	bool AnyTells() const { return bTells; }
	// The weak point's glow: on at At, off (no new particles; the live ones finish their short lives), or moved to At while on.
	void SetGlow(bool bOn, const FVector& At);
	void MoveGlow(const FVector& At);
	bool IsGlowing() const { return bGlowing; }

	// The tell light: on along the floor from From to To (world, cm; the bar spans them, lifted LightUp over From's
	// height), ramping in from the next TickLight. False (nothing changes) without the light or with From and To under a
	// centimetre apart.
	bool LightOn(const FVector& From, const FVector& To, const TCHAR* Why);
	// Out: ramped over LightRampOut frames (the impact), or bAtOnce out and hidden this frame (an interrupt). Nothing when
	// it is out already.
	void LightOff(bool bAtOnce, const TCHAR* Why);
	// One step of the ramp; once a frame, after anything that turns the light on or off that frame.
	void TickLight();
	// On (ramping in or full), and its level now (0..1 of full).
	bool IsLightOn() const { return LightGoal > 0.f; }
	float GetLightLevel() const { return LightLevel; }

	UPROPERTY()
	TObjectPtr<AFTVfxActor> Slots[15]; // MaxSlots (a literal for UHT; checked in the .cpp)
	// The tell light and the actor that holds it (made once in Spawn).
	UPROPERTY()
	TObjectPtr<AActor> LightHost;
	UPROPERTY()
	TObjectPtr<URectLightComponent> TellLight;

private:
	void ReadTuning(const FString& File);
	static int32 FirstSlot(EEffect Effect);
	AFTVfxActor* NextSlot(EEffect Effect);
	void SpawnLight(UWorld* World);
	// The light's intensity at LightLevel, hidden at 0.
	void ApplyLight();

	FTuning Tuning;
	int32 Next[EffectCount] = {};
	bool bOn[EffectCount] = {};
	bool bSpawned = false;
	bool bTells = false;
	bool bGlowing = false;
	// The ramp: the level now and the one it heads for (0..1), its full intensity (cd) and the steps per frame.
	float LightLevel = 0.f;
	float LightGoal = 0.f;
	float LightFull = 0.f;
	float LightStepIn = 1.f;
	float LightStepOut = 1.f;
};
