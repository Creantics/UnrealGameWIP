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
class UFont;
class UFTArenaRunner;
class AFTExploreCharacter;
class AFTExploreDirector;
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

// Explore's HUD (the r19 language out of a fight): the party's names over their thin lines, the objective under them,
// the prompt at the focused spot and the notices at the right edge; nothing drawn while either getter gives null.
TSharedRef<SWidget> MakeExploreHudWidget(TFunction<const AFTExploreDirector*()> GetDirector, TFunction<const AFTExploreCharacter*()> GetLead);

// The Colours row: the new UI's palette (ftcore::UiPalette) from now on. The settings subsystem calls it on every change.
void SetUiPalette(uint8 Palette);

// The new UI's pause menu and settings screen (item B): mounts one widget on the game viewport when the new UI is on and
// it is not mounted yet (again after a map load). True while it is: the Canvas overlays then draw nothing. Called from
// DrawPauseOverlay and DrawSettingsOverlay, so every screen that drew them gets it.
bool EnsureMenuScreens();

// The UI's faces as Canvas fonts (the runtime cache; each made once and kept for the process), for the Canvas drawers
// such as the map overlay (FTMapOverlay): Face is an ftcore::UiFont. Size them with FSlateFontInfo(Font, points).
// Null when the face's file is missing (the caller keeps an engine font).
UFont* UiCanvasFont(uint8 Face);

// The text size row (ftcore::Settings::textScale, 1..2) for the fight HUD's prompt stack (with the special's row) and the
// map overlay's names and where-am-I line, behind -UiHudTextScale: without it 1, every size as it was. Outside Shipping
// -UiTextScale=<100..200> holds it, as it holds the menus'.
float HudTextScale();
}
