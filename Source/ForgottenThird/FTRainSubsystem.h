#pragma once

// Falling rain around the camera, on Solhaven's maps (ftcore::lookWorldForMap == "solhaven").
//
// -Rain=<0..1> turns it on (game and PIE worlds; 0 or no switch: off, nothing loaded). It spawns two Niagara systems
// once, built by FTBuildVfx from Data/Vfx:
// - NS_rain: lit streaks falling from a box 30 m wide that rides the camera (Lightweight emitters simulate in local
//   space, so the streaks move with it). Its top sits ftcore::RainFallSpec::aboveCamM over the camera, capped at columnM
//   over the floor found by a 30 m trace straight down (ECC_Visibility).
// - NS_rain_splash: ripple rings and small drops on that floor, out to 13 m, shown while the camera is near it.
// The intensity is a ftcore::RainState: settled at -Rain at the start, then ramped (ftcore::rainStep) toward
// r.FT.Rain when that is 0..1 (default -1: the switch). Rates and the streak opacity come from ftcore::rainFallFrame and
// are written only when the intensity moves.
//
// Not here: the ground's wetness and puddles (pair -Rain with -WorldEyeWet for now), the lead's wetness, mist, sound,
// shelter (the streaks fall through roofs and awnings), and cooking (the systems are loaded by name).
//
// Logs once: "Rain: on at <amount>, NS_rain loaded|missing". Missing NS_rain: a warning and nothing else.

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "ftcore/RainFall.h"
#include "ftcore/Weather.h"

#include "FTRainSubsystem.generated.h"

class UNiagaraComponent;

UCLASS()
class FORGOTTENTHIRD_API UFTRainSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bActive; }

	// 0..1 now (0 when off).
	float GetIntensity() const { return bActive ? State.intensity : 0.f; }
	// The streaks' and splashes' own clock (1: normal; a special's freeze slows them to a hang). Not the intensity.
	void SetTimeScale(float Scale);

private:
	void WriteParameters();

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> Streaks;
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> Splash;

	ftcore::RainSpec Spec;
	ftcore::RainFallSpec Fall;
	ftcore::RainState State;
	float SwitchTarget = 0.f; // -Rain=
	bool bActive = false;
	bool bSystemsOn = false; // activated by WriteParameters (spawned inactive)
	bool bHasFloor = false;
	bool bSplashShown = true;
	bool bTopSet = false;
	float FloorZ = 0.f;     // cm
	float TopZ = 0.f;       // cm, smoothed
	float WrittenIntensity = -1.f;

	// The floor trace's query, rebuilt only when the pawn or the view target it ignores changes (not every tick).
	FCollisionQueryParams TraceParams;
	TWeakObjectPtr<const AActor> TracedPawn;
	TWeakObjectPtr<const AActor> TracedView;
	bool bTraceParamsSet = false;
};
