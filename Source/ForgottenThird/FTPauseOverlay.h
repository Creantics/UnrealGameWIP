#pragma once

// Canvas drawing of the pause menu (no UMG), shared by every gameplay screen's HUD: each calls it at the
// end of its DrawHUD, on every path out of it. Draws nothing unless UFTPauseSubsystem::IsOpen(): a dimmed
// screen, a centred panel with the four items (Resume, Settings, Quit to Title, Quit Game) and a hint, or
// in place of the items the Yes / No prompt of a quit item. The settings overlay opened from the menu is
// drawn here too, on top (UFTSettingsSubsystem draws nothing unless it is open), so a screen that has
// never drawn it needs nothing more. It reports each row's rectangle back to the subsystem (for clicks)
// and passes the mouse position on for hover. Drawing builds no strings: every label is a static
// FTUiText.

#include "CoreMinimal.h"

class AHUD;
class UCanvas;

namespace FT
{
// Canvas is the HUD's own (AHUD::Canvas is protected, so the HUD passes it in).
void DrawPauseOverlay(AHUD& Hud, UCanvas* Canvas);
} // namespace FT
