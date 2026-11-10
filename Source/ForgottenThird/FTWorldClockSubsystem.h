#pragma once

// The time of day in a world: ticks ftcore::WorldClock and lights the map from it.
//
// It runs in game and PIE worlds whose look is Solhaven's (Data/Look/looks.json), or in any game world given
// -TimeOfDay. -TimeOfDay=<hours> sets the clock and pauses it (stills); -DayLength=<real minutes> sets the day length;
// -NoDayClock turns it off. A world-eye still (-WorldEyeShot) without -TimeOfDay leaves the map alone, so the world
// eye's own -WorldEyeNight look stands.
//
// From the clock: the map's directional lights follow the sun (and, once it is down, a cool moon opposite it), the sky
// lights fall to the night fill and take its cool tint, the height fog keeps a share of its own colour and takes the
// city's warm glow, the post-process volumes' white balance moves toward the night's, and from half night their auto
// exposure is held to the night's range (centre +- range); all by the one
// (and as the sun goes low, the dusk's magenta sky, warm sun, dim fill and lens flare under it: FTNight::FrameAt)
// night spec the world eye's night still reads too (FTNightLook.h: ftcore::NightLook and its -Night* switches), blended
// in by NightAmount (ftcore::nightFrame). The windows' Night (FTScene::WindowCollection) takes NightAmount. The street lamps and
// shop lights the massing build placed (FTScene::NightLampTag, NightShopTag) switch on as NightAmount passes 0.5, each
// at its own hashed point between 0.4 and 0.6 so a street doesn't light in one frame. The lights are touched only when
// the clock has moved a step (1/240 h) or a lamp crosses its point; nothing allocates per tick.
// On a map whose lights are data (AFTNightLights) there are no tagged lamps to switch: FTLampPoolSubsystem reads
// NightAmount and moves its pool of lights instead.
// A fresh clock (none fixed, set or loaded) starts at ftcore::kWorldClockStartHours, a low morning sun; the map builders
// bake that same sun (FTClock::StartSunRotation) and the world eye's day stills use it too.
//
// The plan frame's north is the engine's -Y, east +X (the massing tables' frame, ftcore::massingEnginePoint).
//
// Story and cutscene code set, pause and resume it; the save holds it (UFTStorySaveGame).
//
// The day's shadow floor (FTDayShade, ftcore::DayShade): by full day the sky light is scaled up and the unbound priority-0
// post-process volume's local exposure shadow contrast, Lumen diffuse colour boost and film toe move toward the day's,
// so shaded lanes read at about a third of the sunlit value. Blended by ftcore::dayShadeAmount (0 at full night and full
// dusk, which keep exactly what they had). Only in a world with a look (UFTLookSubsystem). Switches (read once):
//   -NoDayShade  off;  -DayShadeSky=<scale>  -DayShadeContrast=<0..1>  -DayShadeBoost=<1..4>  -DayShadeToe=<0..1>
// and r.FT.DayShade 0/1 at run time. A day world-eye still with no running clock gets the full day's (or
// -WorldEyeSunHour's) shade from the look subsystem once the look is written (FTDayShade::StillAmount).

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "FTNightLook.h"

#include "ftcore/DayShade.h"
#include "ftcore/SceneLook.h"
#include "ftcore/WorldClock.h"

#include "FTWorldClockSubsystem.generated.h"

class APostProcessVolume;
class UExponentialHeightFogComponent;
class UMaterialParameterCollectionInstance;
class ULightComponent;
class USkyLightComponent;

namespace FTClock
{
// The map's directional light rotation for the sun (or, once it is down, the moon opposite it) at Hours.
FORGOTTENTHIRD_API FRotator SunRotation(const ftcore::WorldClockSpec& Spec, double Hours);
// The start hour's sun with the city's spec: what the builders bake and the day stills use.
inline FRotator StartSunRotation()
{
	return SunRotation(ftcore::WorldClockSpec{}, ftcore::kWorldClockStartHours);
}
} // namespace FTClock

namespace FTDayShade
{
// The day's values: ftcore::DayShadeLook's defaults with the -DayShade* switches over them.
FORGOTTENTHIRD_API const ftcore::DayShadeLook& Look();
// False with -NoDayShade or r.FT.DayShade 0.
FORGOTTENTHIRD_API bool Enabled();
// What the world look writes (the night keeps it).
FORGOTTENTHIRD_API ftcore::DayShadeBase BaseFor(const ftcore::SceneLook& WorldLook);
// The day amount at a sky frame (0 when off).
FORGOTTENTHIRD_API float AmountAt(const FTNight::FSkyFrame& Sky);
// Writes the frame's shadow contrast, diffuse colour boost and film toe into World's unbound priority-0 post-process
// volume (the look's). False when there is none.
FORGOTTENTHIRD_API bool WritePost(UWorld& World, const ftcore::DayShadeFrame& Frame);
// A world-eye day still with no running clock (-WorldEyeShot without -TimeOfDay, -WorldEyeNight or -WorldEyeDusk): the
// day amount at the start hour or -WorldEyeSunHour. Negative when this run is no such still or the hour is not full day.
FORGOTTENTHIRD_API float StillAmount();
} // namespace FTDayShade

UCLASS()
class FORGOTTENTHIRD_API UFTWorldClockSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bActive; }

	// For story and cutscene code. Hours 0..24 (wrapped); SetTimeOfDay keeps the day count, AdvanceTo counts the days
	// it passes.
	UFUNCTION(BlueprintCallable, Category = "FT|Time")
	void SetTimeOfDay(float Hours);
	UFUNCTION(BlueprintCallable, Category = "FT|Time")
	void AdvanceTo(float Hours);
	UFUNCTION(BlueprintCallable, Category = "FT|Time")
	void PauseClock();
	UFUNCTION(BlueprintCallable, Category = "FT|Time")
	void ResumeClock();
	UFUNCTION(BlueprintCallable, Category = "FT|Time")
	void SetDayLength(float RealMinutes);

	UFUNCTION(BlueprintPure, Category = "FT|Time")
	float GetHours() const { return static_cast<float>(Clock.hours); }
	UFUNCTION(BlueprintPure, Category = "FT|Time")
	int32 GetDay() const { return static_cast<int32>(FMath::Min<uint32>(Clock.day, MAX_int32)); }
	UFUNCTION(BlueprintPure, Category = "FT|Time")
	bool IsClockPaused() const { return Clock.paused; }
	// True when -TimeOfDay= fixed the clock for a capture: that pause is not saved, and a load keeps it.
	bool IsFixedByCommandLine() const { return bFixedByCommandLine; }
	UFUNCTION(BlueprintPure, Category = "FT|Time")
	float GetNightAmount() const { return Night; }
	// The time curves (Data/time_curves.csv): the share of the daytime crowd out now, and how dangerous the streets are.
	UFUNCTION(BlueprintPure, Category = "FT|Time")
	float GetCrowdDensity() const;
	UFUNCTION(BlueprintPure, Category = "FT|Time")
	float GetDanger() const;

	// The whole clock, for the save.
	const ftcore::WorldClock& GetClock() const { return Clock; }
	void SetClock(const ftcore::WorldClock& InClock);
	bool IsRunning() const { return bActive; }

private:
	void Gather(UWorld& World);
	void Apply(bool bForce);
	// MPC_Look Wetness: the world look's wetness times the night frame's wetShare.
	void WriteWetness();

	struct FLit
	{
		TWeakObjectPtr<ULightComponent> Light;
		float Intensity = 0.f;
		FLinearColor Color = FLinearColor::White;
	};
	struct FSky
	{
		TWeakObjectPtr<USkyLightComponent> Light;
		float Intensity = 0.f;
		FLinearColor Color = FLinearColor::White;
		bool bRealTime = false;
	};
	struct FExposure
	{
		TWeakObjectPtr<APostProcessVolume> Volume;
		bool bMin = false, bMax = false;
		float Min = 0.f, Max = 0.f;
		bool bWhite = false;
		float White = 6500.f;
	};
	struct FFog
	{
		TWeakObjectPtr<UExponentialHeightFogComponent> Fog;
		FLinearColor Color = FLinearColor::White;
	};
	struct FLamp
	{
		TWeakObjectPtr<ULightComponent> Light;
		float OnAt = 0.5f;
		bool bOn = false;
	};

	ftcore::WorldClock Clock;
	ftcore::WorldClockSpec Spec;
	ftcore::TimeCurves Curves;
	bool bCurves = false;
	bool bFixedByCommandLine = false;
	bool bActive = false;
	double AppliedHours = -1.0;
	float Night = 0.f;
	bool bExposureLocked = false;
	float AppliedWhiteMix = 0.f;
	bool bClockGiven = false;

	TArray<FLit> Suns;
	TArray<FSky> Skies;
	TArray<FFog> Fogs;
	TArray<FExposure> Exposures;
	TArray<FLamp> Lamps;
	TWeakObjectPtr<UMaterialParameterCollectionInstance> Windows;
	TWeakObjectPtr<UMaterialParameterCollectionInstance> LookParams;
	float WetShare = 1.f;
	// Written again once the look subsystem has settled, so its own full-value write doesn't stand.
	bool bWetPending = true;
	// The dusk's sky and lens (FTNightLook.h), written as the sun goes low.
	FTNight::FDuskScene DuskScene;
	// The day's shadow floor last written to the post-process volume (-1: write on the next step).
	float AppliedDayShade = -1.f;
};
