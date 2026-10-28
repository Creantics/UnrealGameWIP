#pragma once

// Canvas UI of the cutscene screen (no UMG): letterbox bars (2.39:1, at least 8% of the height each),
// the step's title and "Step i of n" in the top bar, "Shot i of n" and the skip hint in the bottom
// bar. All text comes from the AFTCutsceneDirector, built once per step; no story text. Drawing builds
// no strings. FT::DrawCutsceneHUD draws it for any HUD: this one on the stage, AFTExploreHUD for a
// cutscene played in place, where the bars grow in and out with the camera's blend and only the skip
// hint shows.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTCutsceneHUD.generated.h"

class AFTCutsceneDirector;
class UCanvas;

namespace FT
{
// Nothing before the director's shots start. Canvas is the HUD's own (AHUD::Canvas is protected, so the
// HUD passes it in).
void DrawCutsceneHUD(AHUD& Hud, UCanvas* Canvas, const AFTCutsceneDirector& Director);
} // namespace FT

UCLASS()
class FORGOTTENTHIRD_API AFTCutsceneHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
