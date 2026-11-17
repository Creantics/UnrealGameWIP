#pragma once

// Far-field sign cards for a night street: tall lit vertical cards in red, pink and a little cyan on the faces of the
// blocks beyond it, turned toward it, so its distance (and its wet ground) carries coloured accents. Each card is a thin
// lit box (/Game/Look/M_Look_Sign, SignColor and SignStrength) with a few dark bars on its face for abstract glyph marks.
// Placement is ftcore::placeFarSigns over the plan boxes of the map's tall static meshes (instances too).
//
// Off unless -FarSigns is given (game and PIE worlds). Switches (FTLookSwitches, so the demo boot row may carry them):
//   -FarSignsAt=<x,y world metres> the street's centre (404.5,-25: the Strip's lane, plan 904.5,525)   -FarSignsCount=<n> (30, at most 40)
//   -FarSignsNear=<m> (20)  -FarSignsFar=<m> (140)  -FarSignsLum=<SignStrength> (25)
// Placed once at begin play, again when a level streams in while none were placed. Nothing per tick.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "FTFarSignsSubsystem.generated.h"

UCLASS()
class FORGOTTENTHIRD_API UFTFarSignsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	static constexpr const TCHAR* SignTag = TEXT("FTFarSign");

private:
	// Returns how many cards were placed.
	int32 Place(UWorld& World);

	FDelegateHandle LevelAddedHandle;
	int32 Placed = 0;
};
