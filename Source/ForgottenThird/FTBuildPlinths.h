#pragma once

// FTBuildMassingWorld's retaining plinths (round 10): with -Plinths, the stuck-pocket check (ftcore/TerrainPocket.h) runs
// over the built boxes and the ground, and the plinths that close what it finds are planned: a skirt down to the bank
// round every building on sloping ground, and fills raising the narrow troughs left to their spill level. The commandlet
// lays them as cubes of its cells (Walk). The log gives the pockets before and after, the plinths made, and each pocket
// left ("FTBuildMassingWorld: plinths: ..."). Without the switch nothing here changes the build. Stairs (round 10 follow-up)
// lead out of the wider pockets. -PlinthExempt=X0,Y0,X1,Y1[;...] (plan m) leaves out pockets inside those rects (an area
// the lead can't enter that the boxes don't show, such as a railed canal), as ftcore_pocketcheck's --exempt-rect.

#include "CoreMinimal.h"

#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"

#include <vector>

namespace FTPlinths
{
// The plinths for the boxes Visible marks (their tops as built) on Ground, the lead's way in at the entry (EntryX, EntryY,
// plan m), appended to Out as boxes of kind "terrace" (the retaining walls' surface). False (logged) when the check
// can't run; true, with Out untouched, without -Plinths.
FORGOTTENTHIRD_API bool Plan(const ftcore::MassingGround& Ground, const std::vector<ftcore::MassingBox>& Boxes, const std::vector<bool>& Visible,
	double EntryX, double EntryY, std::vector<ftcore::MassingBox>& Out);
} // namespace FTPlinths
