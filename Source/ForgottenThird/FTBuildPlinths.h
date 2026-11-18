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
#include "ftcore/TerrainPocket.h"

#include <vector>

namespace FTPlinths
{
// Playtest item 3's fix pass (-FloatFix): what FixFloats found and planned, for the laying and the check after it.
struct FFloatFix
{
	bool bOn = false;
	ftcore::FloatSpec Spec;          // maxEave from -FloatEave= (m), else the default
	ftcore::FloatReport Before;      // the check before the fix (the -FloatCheck report)
	ftcore::FoundationPlan Plan;     // added: laid after the boxes, each in its wears box's surface
	ftcore::FoundationStats Stats;
};

// With -FloatFix: ftcore::planFoundations over the boxes Visible marks, their tops as built (z1 + Nudges, by box). Each
// edit is made to Boxes in place (the top kept as the table's, the nudge taken back off), so the laying, the dressing and
// the plinths see the fixed boxes; the added boxes stay in Out.Plan.added. Keep marks the boxes a kit mesh is fitted to
// (their shape is kept). Logs the counts and every float left by name. True, Out off, without the switch; false (logged)
// when the check can't run.
FORGOTTENTHIRD_API bool FixFloats(const ftcore::MassingGround& Ground, std::vector<ftcore::MassingBox>& Boxes, const std::vector<double>& Nudges,
	const std::vector<bool>& Visible, const std::vector<bool>& Keep, FFloatFix& Out);

// With -FloatFix, after the plinths: the check again (spanRule and chainCheck on) over Boxes (the built boxes, the fix's
// added boxes and the plinths, as Include marks), and ftcore::classifyFloats against Fix.Before. Logs "floats after the
// fix: N = fixed + spanned + failing; new floats", each spanned row's span and overhang, each failing row and new float by
// id, and with -FloatFixReport=<csv> writes every row. True without the switch; false (logged) when the check can't run
// (a float still failing is logged, not fatal: the ruling names them).
FORGOTTENTHIRD_API bool ReportFixed(const ftcore::MassingGround& Ground, const std::vector<ftcore::MassingBox>& Boxes,
	const std::vector<bool>& Include, const FFloatFix& Fix);

// The plinths for the boxes Visible marks (their tops as built) on Ground, the lead's way in at the entry (EntryX, EntryY,
// plan m), appended to Out as boxes of kind "terrace" (the retaining walls' surface). False (logged) when the check
// can't run; true, with Out untouched, without -Plinths.
FORGOTTENTHIRD_API bool Plan(const ftcore::MassingGround& Ground, const std::vector<ftcore::MassingBox>& Boxes, const std::vector<bool>& Visible,
	double EntryX, double EntryY, std::vector<ftcore::MassingBox>& Out);

// Playtest item 3, report only for now: with -FloatCheck, ftcore::findFloats over the boxes Visible marks. It logs the
// totals and each float (id, kind, gap, where, building, failing, crouch-under), and with -FloatReport=<csv> writes them
// there too. Nothing is sunk, snapped or failed yet. True without the switch; false (logged) when the check can't run.
// With Fix on, it reports Fix->Before (the boxes as they stood before the fix) instead of checking Boxes again.
FORGOTTENTHIRD_API bool ReportFloats(const ftcore::MassingGround& Ground, const std::vector<ftcore::MassingBox>& Boxes,
	const std::vector<bool>& Visible, const FFloatFix* Fix = nullptr);
} // namespace FTPlinths
