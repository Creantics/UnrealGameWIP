#pragma once

// The new game UI (production/ui plan): Slate widgets built in code, painted from Core view-models (ftcore/UiViews.h)
// in the style table (Data/UI/style.json, ftcore/UiStyle.h), added to the game viewport by the screen's HUD actor.
// Always on in a Shipping build (no flag parsed, no ft.NewUi). Outside Shipping it is on by default: the console
// variable ft.NewUi 0 turns it off, back to the old Canvas HUD, for one more round, and so does -FTOldUi on the command
// line (which wins); -FTNewUi still parses but does nothing. Each HUD reads it once, at BeginPlay
// (NewUiEnabled, the one switch): with it on the HUD adds its widget and skips only the Canvas parts that widget
// replaces (it checks the widget, not the flag, so a change mid-map does nothing until the next map); with it off every
// HUD draws as before.
// The widgets honour the HUD's bShowHUD (clean-plate stills hide them too).
//
// The placeholder look is the style table plus its meter painter (UiStyle::meter). A direction with curved gauges or new
// marks needs new painters and view sections, not only a table swap.

#include "CoreMinimal.h"

class SWidget;
class UFTArenaRunner;
class AFTExploreCharacter;
class AFTStoryDirector;

namespace FT
{
// True when the new UI is on: always in Shipping; outside it the default, unless -FTOldUi is on the command line (which
// wins) or ft.NewUi is 0. Read by the HUDs at BeginPlay only.
bool NewUiEnabled();

// The arena HUD's Slate layer (the lead's block, the partner, the third place's reserved slot, the target and the boss
// bar), reading the given runner each tick (the runner may be null until the fight begins).
TSharedRef<SWidget> MakeArenaHudWidget(TFunction<const UFTArenaRunner*()> GetRunner);

// The story's subtitle block (speaker inline, text revealed as the presenter types it, options, continue marker),
// reading the given director each tick (null: nothing drawn).
TSharedRef<SWidget> MakeDialogueWidget(TFunction<const AFTStoryDirector*()> GetDirector);

// Explore's party screen (the fight HUD's party rows, standing alone while the character's screen is open), reading the
// given character each tick (null: nothing drawn).
TSharedRef<SWidget> MakeExplorePartyWidget(TFunction<const AFTExploreCharacter*()> GetLead);
}
