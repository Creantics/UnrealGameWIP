#pragma once

// The new game UI (production/ui plan): Slate widgets built in code, painted from Core view-models (ftcore/UiViews.h)
// in the style table (Data/UI/style.json, ftcore/UiStyle.h), added to the game viewport by the screen's HUD actor.
// Off by default: -FTNewUi on the command line or the console variable ft.NewUi 1 turns it on. With it on, each HUD
// skips only the Canvas parts these widgets replace; with it off every HUD draws exactly as before.
//
// The look is the style table plus its meter painter (UiStyle::meter): the direction's pick swaps those two and no code.

#include "CoreMinimal.h"

class SWidget;
class UFTArenaRunner;
class AFTStoryDirector;

namespace FT
{
// True when the new UI is on (-FTNewUi, or ft.NewUi 1).
bool NewUiEnabled();

// The arena HUD's Slate layer (the lead's block, the partner, the third place's reserved slot, the target and the boss
// bar), reading the given runner each tick (the runner may be null until the fight begins).
TSharedRef<SWidget> MakeArenaHudWidget(TFunction<const UFTArenaRunner*()> GetRunner);

// The story's subtitle block (speaker inline, text revealed as the presenter types it, options, continue marker),
// reading the given director each tick (null: nothing drawn).
TSharedRef<SWidget> MakeDialogueWidget(TFunction<const AFTStoryDirector*()> GetDirector);
}
