#pragma once

// Game mode of the look lab (/Game/Maps/Look_Lab, built by FTBuildLookLabWorld): a still scene of the look's surfaces
// (a water basin, glass, chrome, wet stone, a glowing strip and a practical lamp in fog) seen through the map's
// cameras. No story, no HUD, no pawn to steer. The world's look comes from -Look=<world> (UFTLookSubsystem).
//
//   -LookLabShot=<prefix> renders, at a fixed 1/48 s step, three stills (<prefix>_1_wide.png, _2_water.png,
//                         _3_chrome.png), each after a 3 s settle, then 192 frames of the water view a 24th of a second
//                         apart (<prefix>_loop_0000.png ...), and quits. Needs a renderer (-RenderOffScreen).
// See docs/PHOTOREAL_LOOK.md.

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "FTLookLabGameMode.generated.h"

class ACameraActor;

UCLASS()
class FORGOTTENTHIRD_API AFTLookLabGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	AFTLookLabGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// The tag of the map's view N (1 wide, 2 water and glass, 3 chrome, lamp and glow): "FTLookLab<N>".
	static FName ViewTag(int32 View);

private:
	void ShowView(int32 View);
	void Shoot(const FString& File, const TCHAR* Size);

	FString ShotPrefix;
	// The shot being saved, and the ticks spent waiting for it.
	FString Pending;
	int32 PendingTicks = 0;
	// The view to show once the pending still is saved, or 0.
	int32 NextView = 0;
	bool bAdvanced = false;
	int32 Stage = 0;
	int32 Frame = 0;
	int32 Wait = 0;
};
