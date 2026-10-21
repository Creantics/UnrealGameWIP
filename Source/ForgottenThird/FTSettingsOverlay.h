#pragma once

// Canvas drawing of the settings overlay (no UMG), shared by the title HUD and the story HUD. Draws
// nothing unless UFTSettingsSubsystem::IsOpen(). It reports each row's rectangle back to the
// subsystem (for clicks) and passes the mouse position on for hover. Drawing builds no strings: the
// labels and values are cached in the subsystem and the rest are static.

#include "CoreMinimal.h"

class AHUD;
class UCanvas;
class UFTSettingsSubsystem;

namespace FT
{
// Canvas is the HUD's own (AHUD::Canvas is protected, so the HUD passes it in).
void DrawSettingsOverlay(AHUD& Hud, UCanvas* Canvas, UFTSettingsSubsystem& Settings);
} // namespace FT
