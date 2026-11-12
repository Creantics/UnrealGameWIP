#pragma once

// The aim's centre reticle, shared by the field and arena HUDs: a dot and a thin ring at the screen's centre (where
// the camera's line points), about 1.5% of the screen's height across, white over a dark outline so it reads on
// bright and dark ground. Alpha is the aim stance's fade (0 draws nothing). Out of reach (a gun's crosshair on a body
// past the shot's range) it is drawn faint and without its dot.

#include "CoreMinimal.h"

class AHUD;
class UCanvas;

namespace FT
{
void DrawAimReticle(AHUD& Hud, UCanvas* Canvas, float Alpha, bool bOutOfReach = false);
} // namespace FT
