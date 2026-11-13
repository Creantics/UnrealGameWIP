#pragma once

// The new game UI (production/ui plan): Slate widgets built in code, painted from Core view-models, added to the game
// viewport by the screen's HUD actor. Off by default: -FTNewUi on the command line or the console variable ft.NewUi 1
// turns it on; with it off every HUD draws exactly as before.

#include "CoreMinimal.h"

class SWidget;
class UFTArenaRunner;

namespace FT
{
// True when the new UI is on (-FTNewUi, or ft.NewUi 1).
bool NewUiEnabled();

// The arena HUD's Slate layer, reading the given runner each paint (the runner may be null until the fight begins).
TSharedRef<SWidget> MakeArenaHudWidget(TFunction<const UFTArenaRunner*()> GetRunner);
}
