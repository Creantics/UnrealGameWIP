#pragma once

// Stuck pockets: places the lead can get into (walking, or dropping off something) but not out of on foot, and the
// retaining plinths and trough fills that close them.
//
// The check lays a grid (PocketSpec::cell) over a plan rect and reads one floor per cell: the ground (massingGroundAt),
// raised to the top of every box `include` marks that holds the cell's middle and whose bottom stands under floor +
// capsuleHeight (taken lowest bottom first, so a box on a box counts; one higher up is overhead and the capsule passes
// under it). A cell is standable when its floor's normal is at least walkableZ up (the ground's slope from the cells
// around it; a box top by its own slope). A cell is out of play when its floor lies under seaLevel (water), or squeezed
// when the cells on both sides of it along x, or along y, rise more than maxStep over it (a slot under a cell and a half
// wide that the capsule doesn't fit).
//
// The lead moves to a side neighbour b from a when b is in play and not squeezed, and its floor rises no more than
// maxStep over a's (jumpRise when a and b are standable and jumpRise > maxStep); a rise of more than kPocketFlatRise needs
// b standable; from a cell that isn't standable the lead slides: down or across, never up. A drop of any height is a move. The lead comes
// in at the anchor points (at the rect's standable edge cells when there are none) and gets out at an anchor point, a
// standable edge cell or the water (it drops into the sea). A pocket cell is reached from the way in but reaches no way
// out; pockets are their side-connected groups.
//
// planPlinths closes them in two steps, as boxes (kind "plinth", source Invented, no cover) for the build to lay:
//   skirts  each building (kind kit, box or slab, at least kPocketBuildingHeight tall, level) on sloping ground (the
//           ground within `band` of its footprint spanning more than maxStep): its faces in pieces `segment` long and
//           `band` deep, a piece wherever the ground in it is steeper than walkable, or dips at the wall (the ground at
//           the wall more than maxStep under the ground further out), and nothing taller already stands there. Its top is
//           level at the highest ground in it, its bottom `sink` under the lowest: a retaining plinth with no gap between
//           the building's base and the bank. Runs of pieces whose tops are within kPocketSkirtMerge join.
//           So do the skirts of different buildings standing in one line, end to end (PlinthSpec::joinSkirts).
//   fills   then, up to fillPasses times, the check on the boxes and the plinths so far: each pocket no wider than
//           narrowWidth (Pocket::width) whose spill level, the lowest floor of a cell next to it that still reaches a way
//           out, stands no more than fillMaxRise over its lowest floor is raised to that level, as grid-aligned boxes
//           over its cells under it (its banks included: a level terrace out to the ground it spills onto).
//   stairs  in the same passes, each other pocket with a spill gets one stair out: from the side cell with a way out
//           (lowest first) straight in, a step of stairRise every cell, three cells wide, down to a floor of the pocket;
//           where none fits (a pocket too thin for its rise) it is filled as above if its rise is within fillMaxRise.
// The check runs once more after; pockets with no way out, or that no stair fits, stay, reported.
//
// Plain C++20. Allocates (build time only): the grid is about 10 bytes a cell.

#include "ftcore/Api.h"
#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ftcore {

// A rise of up to this (m) onto a cell that isn't standable is still a move (bumps across a sampled slope).
inline constexpr double kPocketFlatRise = 0.05;
// The skirts' buildings: at least this tall (m).
inline constexpr double kPocketBuildingHeight = 2.5;
// Skirt pieces along a face whose tops lie within this (m) join into one.
inline constexpr double kPocketSkirtMerge = 0.05;

// The lead as the check moves it. The defaults are AFTExploreCharacter's: a capsule 0.34 m across the radius and 1.76 m
// tall (0.35 and 1.8 here), UCharacterMovementComponent's default MaxStepHeight 45 cm and WalkableFloorAngle 44.765
// degrees (WalkableFloorZ 0.71), and a jump of JumpZVelocity 428 cm/s under 980 cm/s2, 0.93 m at its top.
struct PocketRect {
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
};

struct PocketSpec {
    double cell = 0.5;          // m, the grid's spacing
    double capsuleHeight = 1.8; // m: a box whose bottom stands this far over a floor is overhead
    double maxStep = 0.45;      // m, a step up the lead walks
    double jumpRise = 0.0;      // m, a rise a jump makes from a standable cell to a standable cell (0: no jumps)
    double walkableZ = 0.71;    // the least normal z standable
    double seaLevel = 0.2;      // m: a floor under this is water, out of play
    double narrowWidth = 2.0;   // m: planPlinths fills pockets no wider than this
    std::vector<PocketRect> exempt; // pockets lying inside one of these (0.5 m slack) aren't reported (counted in exempted)
};

struct PocketPoint {
    double x = 0.0, y = 0.0;
};

struct Pocket {
    double cx = 0.0, cy = 0.0;                       // the middle of its cells, plan m
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    std::size_t cells = 0;
    double area = 0.0;     // m2
    double width = 0.0;    // m: twice its floor's deepest cell's distance from the floor's edge (its floor: the cells
                           // the lead stands on, or all of them when it stands on none)
    double floorMin = 0.0; // m, its lowest floor
    double spill = 0.0;    // m, the lowest floor of a cell next to it that reaches a way out (when hasSpill)
    bool hasSpill = false;
    int nearBox = -1;      // a building box (planPlinths' kinds) within 1 m of one of its cells, or -1
};

struct PocketReport {
    std::size_t cellsX = 0, cellsY = 0;
    std::size_t inPlay = 0;       // cells neither water nor squeezed
    std::size_t reached = 0;      // cells reached from the way in
    std::size_t pocketCells = 0;
    std::size_t exempted = 0;     // pockets inside an exempt rect, left out
    std::vector<Pocket> pockets;  // largest first
};

// Parses "X0,Y0,X1,Y1[;X0,Y0,X1,Y1]" into rects (each made min..max); false on a malformed list.
FTCORE_API bool parsePocketRects(const std::string& text, std::vector<PocketRect>& out);

// The rect the boxes `include` marks cover, grown by margin and cut to the ground's extent (an empty ground: not cut).
FTCORE_API PocketRect pocketRectOf(const MassingGround& ground, const std::vector<MassingBox>& boxes,
                                   const std::vector<bool>& include, double margin);

// Whether a box is a building the skirts stand against.
FTCORE_API bool pocketBuilding(const MassingBox& box);

// The check. False (error set) on a rect with no area or over 100 million cells.
FTCORE_API bool findPockets(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                            const PocketRect& rect, const std::vector<PocketPoint>& anchors, const PocketSpec& spec,
                            PocketReport& out, std::string* error = nullptr);

struct PlinthSpec {
    PocketSpec pocket;
    double band = 2.0;     // m, a skirt piece's depth out from the wall
    double segment = 1.0;  // m, a skirt piece's length along the wall
    double sink = 0.3;     // m, a plinth's bottom under the lowest ground it covers
    double fillMaxRise = 8.0; // m: a pocket whose spill stands higher over its floor isn't filled
    double stairRise = 0.3;   // m, a stair's riser (under maxStep)
    int fillPasses = 16;      // passes of fills and stairs
    bool skirts = true;
    bool joinSkirts = true; // skirts of different buildings in one line, end to end, join as a face's own runs do
    bool fills = true;
    bool stairs = true;
};

struct PlinthStats {
    std::size_t slopedBuildings = 0; // buildings on sloping ground
    std::size_t skirts = 0;          // skirt boxes (after joining)
    std::size_t fills = 0;           // fill boxes
    std::size_t filledPockets = 0;
    std::size_t wideFilled = 0;      // of them, wider than narrowWidth (no stair fitted)
    std::size_t stairs = 0;          // stair step boxes
    std::size_t stairedPockets = 0;
    int passes = 0;                // fill/stair passes that placed something
    std::size_t pocketsBefore = 0, pocketCellsBefore = 0;
    std::size_t pocketsAfter = 0, pocketCellsAfter = 0;
    std::size_t narrowLeft = 0, wideLeft = 0; // pockets after, no wider than narrowWidth / wider
    PocketReport after;                       // the last check
};

// The plinths for the boxes `include` marks, appended to `out` (ids PL-S-<n> for skirts, PL-F-<n> for fills, PL-X-<n>
// for stair steps).
FTCORE_API bool planPlinths(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                            const PocketRect& rect, const std::vector<PocketPoint>& anchors, const PlinthSpec& spec,
                            std::vector<MassingBox>& out, PlinthStats& stats, std::string* error = nullptr);

} // namespace ftcore
