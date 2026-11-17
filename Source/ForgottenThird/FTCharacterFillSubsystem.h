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
// Near a lit shop bay the shop light shapes the figure: the fill keeps ftcore::nightCharacterFillShare of itself at the
// pawn's plan distance from the nearest lit bay front (the hero corner's front keys), gone within -NightCharFillBayNear=<m>
// (3) and whole from -NightCharFillBayFar=<m> (7).

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
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bActive; }

	static constexpr const TCHAR* LightTag = TEXT("FTCharacterFill");

private:
	void Spawn(UWorld& World);
	int32 MarkCharacters(UWorld& World);
	// The lit bays' fronts (the hero corner's front keys, FTNight::HeroFrontTag), looked for on the scan until found
	// and again after a level streams in.
	void FindBays(UWorld& World);
	// Metres in plan from the player's pawn to the nearest lit bay front; negative with none.
	float BayDistanceM(const UWorld& World) const;

	TWeakObjectPtr<UDirectionalLightComponent> Light;
	TArray<FVector, TInlineAllocator<8>> BayFronts;
	bool bBaysFound = false;
	int32 BayLooksLeft = 20;
	FDelegateHandle LevelAddedHandle;
	bool bActive = false;
	bool bStillNight = false;
	float ScanIn = 0.f;
	int32 Marked = 0;
	bool bLogged = false;
};
