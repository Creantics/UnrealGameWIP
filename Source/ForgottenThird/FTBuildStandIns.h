#pragma once

// FTBuildMassingWorld's stand-in meshes (round 9): street and park trees, park shrubs, quay railings, lamp masts,
// footbridges (as their deck, landing, stair and pier pieces), pitched house roofs, glass towers and the landmark tower,
// each a composition of the engine's basic shapes (ftcore::standInParts) laid as instances of the build's cells
// (FFTCellInstancer), never actors, until kit meshes replace them. Placed from point tables (ftcore::importStandIns reads the columns by name; a kind column, where a
// table has one, overrides the switch's kind). Each switch takes one CSV or several separated by ';':
//
//   -StandIns=<csv>       rows of any kind (the kind column required)
//   -StandInTrees=<csv>   trees (kind street, or park / forecourt / precinct for the bigger park tree; height_m, canopy_m)
//   -StandInHouses=<csv>  pitched house roofs (eave_z, pitch_deg, ridge u|w; hidden = 1 for the back house's simpler roof)
//   -StandInTowers=<csv>  glass towers and the landmark (kind glass | landmark; z0 the base, height_m, floors)
//   -QuayPoints=<csv>     the quay's railing spans, lamp masts and footbridge decks, landings, stairs and piers
//
// The corridor rule: with -Streets (and -Roads) on the command line no part of a stand-in may stand in a street's or an
// alley's clear way (its carriageway where it has pavements, its whole width where it has none; a tree's crown and
// anything 2.4 m over the ground hang clear: ftcore::standInCorridorCheck). Each offending row is logged as an Error
// ("StandIns: corridors: ...") and the build fails; -StandInCorridorWarn logs them as warnings and builds on.
// ftcore_standincheck runs the same rule on the tables offline.
//
// A row without z stands on the ground's height. The log gives each kind's points and instances, in all and per cell
// ("StandIns: Cell_<i>_<j>: ..."), the census the frame budget is checked against. Without these switches nothing here
// changes the build.

#include "CoreMinimal.h"

#include "ftcore/Backdrop.h"

class FFTCellInstancer;
class UMaterialInterface;
class UPackage;

namespace FTStandIns
{
// Reads the switches' tables and adds every stand-in's parts to Cells (before its Flush). Base is the build's surface
// material (its instances are saved in Package); Field the ground the rows without z stand on; Side the world's side (m).
// False (logged) when a table doesn't load; true, with nothing to do, without the switches.
FORGOTTENTHIRD_API bool Place(UPackage* Package, FFTCellInstancer& Cells, UMaterialInterface* Base, const ftcore::HeightField& Field, double Side);
} // namespace FTStandIns
