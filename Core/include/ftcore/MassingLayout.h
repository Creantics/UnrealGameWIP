#pragma once

// A massing's lanes laid out as streets: buildings brought to a build-to line along each narrow lane, the gaps between
// them filled with infill buildings, and the lane's surface dressed flush with the ground (a centre gutter, drain covers,
// patches and give-way bars at its mouths). Plain C++20; plan frame, metres (MassingPlan.h). Every function here runs at
// build time only and may allocate.
//
//   ftcore::MassingBuildTo opts;  opts.area = {690, 260, 940, 720};  opts.keepClear.push_back({878, 410, 892, 650});
//   ftcore::MassingBuildToCounts counts = ftcore::massingBuildTo(boxes, streets, ground, opts);
//   ftcore::massingLaneDress(streets, ground, dressOpts, boxes);
//
// A lane is a street no wider than laneUpTo whose segment runs along x or along y (to within 0.01 m). Other streets are
// left as they are, but no box is ever moved into one.

#include "ftcore/Api.h"
#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ftcore {

struct MassingRect {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0; // plan frame, x0 <= x1, y0 <= y1
};

// The lanes' build-to line and infill.
struct MassingBuildTo {
    MassingRect area;               // only lanes (clipped) and boxes inside it are touched
    double laneUpTo = 6.0;          // m: a street no wider is a lane
    std::vector<std::string> kinds; // and, when not empty, of one of these kinds (stairs stay out); other streets stay obstacles
    double reach = 12.0;            // m: a building face up to this far behind a lane's edge is pulled out to it
    double minGap = 4.0;            // m: a gap along a lane's frontage at least this long is filled
    double lotMax = 8.0;            // m: an infill lot's frontage, at most
    double depthMin = 4.0;          // m: a lot shallower than this is left empty
    double depthMax = 12.0;         // m
    double storey = 3.0;            // m
    double sink = 0.1;              // m a moved or added building reaches under the lowest ground beneath it
    std::vector<MassingRect> keepClear; // nothing is moved or added into these (a road corridor, a square)
    bool oblique = false; // also fill the frontages of lane segments at any angle (infill only, turned to the segment)
    double groundMin = -1.0e300; // m: no infill on ground lower than this anywhere under it (the sea floor, a quay's water)
    double obliqueRise = 5.0; // m: an oblique lot is only as deep as keeps the ground under it within this (no cliff lots)
};

struct MassingBuildToCounts {
    std::size_t lanes = 0;   // lane segments inside the area
    std::size_t pulled = 0;  // faces pulled out to a build-to line
    std::size_t blocked = 0; // faces in reach left where they were (a box or a street between, or the pull would enter
                             // a street or a keep-clear rect)
    std::size_t trimmed = 0; // boxes cut back out of a lane
    std::size_t infill = 0;  // infill buildings added
    std::size_t oblique = 0; // of them, along segments at an angle (MassingBuildTo::oblique)
};

// Brings `boxes` to the lanes' build-to lines, then fills each frontage's gaps. A building here is a visible box
// (massingVisible) wearing a building finish (massingLookIsBuilding), level (not sloped, no tilt) and turned by a
// multiple of 90 degrees. For each lane segment in the area and each of its sides, its line lies width/2 from the
// centre line. A building whose near face is no more than `reach` behind the line, whose span along the lane overlaps
// the segment by a metre or more, and with nothing between (no visible box, no street, no keep-clear rect) has that
// face moved out to the line; its far face stays, and its bottom drops to the lowest ground under its new footprint
// less `sink`. A building reaching into a lane is cut back to the lane's line. Then each frontage (a lane side, along
// the segment) is walked: a stretch at least minGap long with no building, street or keep-clear rect just behind the
// line is split into lots of at most lotMax and each lot gets a building of kind "kit", source Invented, id
// "<lane id>-IN<n>", as deep as the first obstacle behind it allows (depthMin..depthMax), 2 or 3 storeys (now and then
// 4) by its id's hash over the highest ground under it. Boxes are only changed or appended, never erased; the appended
// ones come last in `boxes`.
// With `oblique`, the lane segments at an angle (terraces on the hills) get infill too, after the rest: each side is
// probed every metre just behind its line (a 1 m cell, turned with the segment) against every visible standing box
// (turned or not), every street's strip (its distance from the segment under half its width, the lane's own bends
// included) and the keep-clear rects; stretches at least minGap long become lots as above, each a box turned to the
// segment (rot = its heading), its near face on the line, as deep as is free and keeps the ground under it within
// obliqueRise (depthMin..depthMax). Nothing is moved.
FTCORE_API MassingBuildToCounts massingBuildTo(std::vector<MassingBox>& boxes, const std::vector<MassingStreet>& streets,
                                               const MassingGround& ground, const MassingBuildTo& opts);

// The distance along the normal between the two building lines of a lane, sampled every `step` m along each of its
// segments inside the area, ignoring sample points where one side has no building within `reach` (a mouth, a crossing):
// min and max over the samples that see both sides, and how many samples see both. For the report on the built lanes.
struct MassingLaneWidth {
    double minWall = 0.0, maxWall = 0.0; // m, wall to wall; 0, 0 when no sample sees both sides
    std::size_t samples = 0, bothSides = 0;
};
FTCORE_API MassingLaneWidth massingLaneWidth(const MassingStreet& lane, const std::vector<MassingBox>& boxes,
                                             const MassingRect& area, double reach, double step);

// The lanes' surface. Every piece is a thin level box of a dressing kind ("gutter", "drain", "patch", "marking":
// massingLookSurface gives each its finish), turned 0 (u along x), its top kTop over the ground at its middle and its
// bottom kSink under the lowest ground beneath it; where the ground runs along x, the top follows it (slope "x A to B",
// with a thickness), so the piece lies flush. A piece whose ground strays from that plane by more than `fit` (a step
// under it) is left out. Ids are "<lane id>-G<n>" (gutter), "-D<n>" (drain), "-P<n>" (patch), "-M<n>" (marking).
struct MassingLaneDress {
    MassingRect area;
    double laneUpTo = 6.0;
    std::vector<std::string> kinds; // as MassingBuildTo::kinds
    double gutterWidth = 0.3;   // m, along the centre line
    double gutterRun = 8.0;     // m: the gutter is cut into runs at most this long
    double endGap = 1.0;        // m left bare at each end of a segment and around each keep-clear rect
    double drainEvery = 16.0;   // m between drain covers on the gutter
    double drainSize = 0.6;     // m square
    double patchEvery = 45.0;   // m: one patch per this much lane, on average (hash placed)
    double markWidth = 0.3;     // m: a give-way bar across a lane's mouth, this deep
    double fit = 0.02;          // m
    std::vector<MassingRect> keepClear;
};
inline constexpr double kMassingLaneDressTop = 0.01;  // m over the ground
inline constexpr double kMassingLaneDressSink = 0.08; // m under it
struct MassingLaneDressCounts {
    std::size_t gutters = 0, drains = 0, patches = 0, marks = 0, uneven = 0;
};
// Appends the pieces to `out` (kept). The mouth bars go across each segment end that meets a street wider than
// laneUpTo (the lane gives way to the road). No piece enters a keep-clear rect.
FTCORE_API MassingLaneDressCounts massingLaneDress(const std::vector<MassingStreet>& streets, const MassingGround& ground,
                                                   const MassingLaneDress& opts, std::vector<MassingBox>& out);

// -LanePave: the road mask's lanes. Each street no wider than upTo (> 0) is widened by `margin` each side and loses its
// pavements, so the ground's road layer runs wall to wall across it and its feathered edge falls under the walls on the
// build-to line. Returns how many it widened. No allocation.
FTCORE_API std::size_t massingLanePave(std::vector<MassingStreet>& streets, double upTo, double margin);

// -LanePave: drops the paving pads (massingFrontage's outPads) standing on a lane: a pad whose centre is within a lane's
// width/2 + reach of its centre line, for streets no wider than upTo. The piece above stays at its lowest floor
// (massingFrontage), so nothing floats. Returns how many it dropped. No allocation (erases in place).
FTCORE_API std::size_t massingDropLanePads(std::vector<MassingBox>& pads, const std::vector<MassingStreet>& streets, double upTo,
                                           double reach);

// ---- Yards on the open plains ----

// Fills the large empty flat ground in an area (a paved plain between blocks, a road side with nothing along it) with
// fenced yards, each with a shed or warehouse.
struct MassingYards {
    MassingRect area;                   // only ground inside it is filled
    double cell = 2.0;                  // m: the grid the room is read on
    double streetBack = 1.0;            // m a yard's fence keeps off a street's edge and off each `taken` foot
    double boxGap = 6.0;                // m a yard keeps off every standing box (so only open plains fill, never a gap)
    double gap = 4.0;                   // m between two yards (a way through)
    double groundMin = 0.5;             // m: no yard on ground lower than this ...
    double waterKeep = 12.0;            // m: ... nor within this of it (a quay's edge, the sea)
    double maxRise = 1.5;               // m: the ground under a yard rises no more than this
    double fenceHigh = 1.8, fenceThick = 0.25, fencePiece = 40.0; // m; a fence side is laid in pieces up to fencePiece
    double gateWide = 5.0;              // m: the gate's gap, in the side nearest a street
    double storey = 3.5;                // m: a shed's storey
    std::vector<MassingRect> keepClear; // nothing goes into these (an entry, a goal, a square)
    std::string prefix = "YD";          // ids "<prefix>-<n>" (the shed) and "<prefix>-<n>-F<k>" (its fence pieces)
};

struct MassingYardCounts {
    std::size_t yards = 0, sheds = 0, fences = 0;
    double area = 0.0; // m2 of yards
};

// Reads the room in the area on a `cell` grid: a cell is taken by a standing box (any role but a zone or a hidden one,
// grown by boxGap), a sloped zone, a street's strip (its width plus streetBack), a `taken` foot (grown by streetBack), a
// keep-clear rect, or ground under groundMin within waterKeep. A flat zone's top counts as the ground over it. Then
// lots are tried west to east, south to north, largest first (36x28, 28x22, 22x18, 16x14 m, either way round): a lot
// with every cell free, its ground within maxRise, and `gap` clear of the yards before it, becomes a yard: a shed (kind
// "kit", source Invented, 1-3 storeys by its id's hash) across the half away from the nearest street, and a fence
// (kind "wall", fenceHigh over the highest ground under each piece, its bottom 0.1 m under the lowest) round the lot
// with a gateWide gap in the side nearest a street. Each box's terr_min/terr_max are left 0 (the table's own columns
// are the caller's). Appends to `boxes` (the rest unchanged).
FTCORE_API MassingYardCounts massingYardFill(std::vector<MassingBox>& boxes, const std::vector<MassingStreet>& streets,
                                             const std::vector<MassingFoot>& taken, const MassingGround& ground,
                                             const MassingYards& opts);

} // namespace ftcore
