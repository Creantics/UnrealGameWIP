#pragma once

// A night fill on characters alone: one directional light on lighting channel 1 only, cool, from the camera's side and a
// little above (ftcore::NightLook charFill*), so a face and a silhouette read between the lamps while the street keeps
// its dark. The characters' own primitives (and their attached actors') get channel 1 on top of channel 0, so every
// other light still lights them as before. Its illuminance is ftcore::nightCharacterFillLux at the clock's NightAmount
// (1 for a -WorldEyeNight still, 0 by day and with no clock).
//
// Off unless -NightCharFill is given (game and PIE worlds). Switches (FTNightLook.h reads them once):
//   -NightCharFillStops=<stops under the lamp key>  -NightCharFillK=<kelvin>  -NightCharFillPitch=<degrees below the view>
// The light is spawned on the first tick, after the world clock and a night still have gathered the map's directional
// lights, and its intensity, colour and direction are written over theirs when they move a step. Characters are looked
// for twice a second. No allocation per tick.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "FTCharacterFillSubsystem.generated.h"

class UDirectionalLightComponent;

UCLASS()
class FORGOTTENTHIRD_API UFTCharacterFillSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bActive; }

	static constexpr const TCHAR* LightTag = TEXT("FTCharacterFill");

private:
	void Spawn(UWorld& World);
	int32 MarkCharacters(UWorld& World);

	TWeakObjectPtr<UDirectionalLightComponent> Light;
	bool bActive = false;
	bool bStillNight = false;
	float ScanIn = 0.f;
	float AppliedLux = -1.f;
	FRotator AppliedRotation = FRotator(1000.f, 0.f, 0.f);
	int32 Marked = 0;
	bool bLogged = false;
};
