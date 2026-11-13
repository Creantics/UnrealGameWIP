#pragma once

// The new game UI (production/ui plan): Slate widgets built in code, painted from Core view-models (ftcore/UiViews.h)
// in the style table (Data/UI/style.json, ftcore/UiStyle.h), added to the game viewport by the screen's HUD actor.
// Off by default: -FTNewUi on the command line or the console variable ft.NewUi 1 turns it on. Each HUD reads it once,
// at BeginPlay: with it on the HUD adds its widget and skips only the Canvas parts that widget replaces (it checks the
// widget, not the flag, so a change mid-map does nothing until the next map); with it off every HUD draws as before.
// The widgets honour the HUD's bShowHUD (clean-plate stills hide them too).
//
// The placeholder look is the style table plus its meter painter (UiStyle::meter). A direction with curved gauges or new
// marks needs new painters and view sections, not only a table swap.

#include "CoreMinimal.h"

class SWidget;
class UFTArenaRunner;
class AFTStoryDirector;

namespace FT
{
// True when the new UI is on (-FTNewUi, or ft.NewUi 1). Read by the HUDs at BeginPlay only.
bool NewUiEnabled();

// The arena HUD's Slate layer (the lead's block, the partner, the third place's reserved slot, the target and the boss
// bar), reading the given runner each tick (the runner may be null until the fight begins).
TSharedRef<SWidget> MakeArenaHudWidget(TFunction<const UFTArenaRunner*()> GetRunner);

// The story's subtitle block (speaker inline, text revealed as the presenter types it, options, continue marker),
// reading the given director each tick (null: nothing drawn).
TSharedRef<SWidget> MakeDialogueWidget(TFunction<const AFTStoryDirector*()> GetDirector);
}
