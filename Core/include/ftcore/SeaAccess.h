#pragma once

// Ways out of the sea (playtest 2026-11-17, item 2: the lead jumps in anywhere and finds a way out within 25 m).
//
// seaLadders lays ladders along the water faces (a quay, a seawall, a pier's side): one every `spacing` m along each face,
// spread evenly between `endMargin` from its ends, and none whose foot or top lands in an `avoid` rect (a crane, the slip,
// a pier's root) or within minGap of another ladder. A ladder's foot stands footOut into the water from the face, its top
// topIn back onto the deck at the face's topZ, and its rail gap runs gapHalf either side of it along the face's line
// (the quay rail at kMassingRailIn is cleared there).
//
// seaExitReach reads the swimmable water (`water` rects, less the `solid` rects in it) on a `cell` grid and returns the
// largest straight-line distance from a water cell to the nearest way out (a ladder's foot, or another exit point such as
// the slip's foot), and where it lies. Straight-line: a solid between a cell and its exit isn't walked round.
//
// Plain C++20. Allocates (build time only).

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ftcore {

struct SeaRect {
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
};

struct SeaFace {
    std::string id;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0; // the face's line, plan m
    double outX = 0.0, outY = 0.0;                 // unit, from the water to the bank
    double topZ = 0.0;                             // m, the deck at the face
};

struct SeaLadderSpec {
    double spacing = 30.0;  // m along a face
    double endMargin = 5.0; // m from a face's ends
    double footOut = 0.35;  // m into the water from the face (as a canal ladder's: a body at the wall is at its foot)
    double topIn = 1.2;     // m onto the deck from the face
    double gapHalf = 0.6;   // m, the rail gap either side of the ladder
    double minGap = 10.0;   // m: no two ladders closer than this (faces meeting at a jog)
};

struct SeaLadder {
    std::string id;         // <face id>-L<n>
    double footX = 0.0, footY = 0.0;
    double topX = 0.0, topY = 0.0, topZ = 0.0;
    double outX = 0.0, outY = 0.0;
    double gapX0 = 0.0, gapY0 = 0.0, gapX1 = 0.0, gapY1 = 0.0;
};

struct SeaPoint {
    double x = 0.0, y = 0.0;
};

// The ladders, appended to out. False (error set) on a face with no length or a spacing under 1 m.
FTCORE_API bool seaLadders(const std::vector<SeaFace>& faces, const std::vector<SeaRect>& avoid, const SeaLadderSpec& spec,
                           std::vector<SeaLadder>& out, std::string* error = nullptr);

struct SeaReach {
    std::size_t cells = 0;     // water cells read
    double worst = 0.0;        // m, the largest distance to a way out
    double worstX = 0.0, worstY = 0.0;
    std::size_t over = 0;      // cells farther than the limit
};

// The reach check. False (error set) with no exits, a cell under 0.25 m, or over 50 million cells.
FTCORE_API bool seaExitReach(const std::vector<SeaRect>& water, const std::vector<SeaRect>& solid,
                             const std::vector<SeaPoint>& exits, double cell, double limit, SeaReach& out,
                             std::string* error = nullptr);

struct SeaRailCounts {
    std::size_t cut = 0;     // rail bars cut open
    std::size_t dropped = 0; // posts and bollards taken out of a gap
};

// The quay's furniture (massingQuay's rail bars, kind "barrier", and posts and bollards, kind "post") opened at the
// ladders: a bar whose line passes within `reach` of a ladder's gap (across it, inside its length) is cut into the
// pieces either side of the gap widened by `clear` (ids <bar id>a, b, ...; a piece under 0.3 m is left out), and a
// post or bollard whose middle stands inside the widened gap along the face and within `reach` + 1 m across it is left
// out. Other pieces are kept as they are. Returns the furniture to build.
FTCORE_API std::vector<MassingBox> seaRailGaps(const std::vector<MassingBox>& furniture, const std::vector<SeaLadder>& ladders,
                                               double clear, double reach, SeaRailCounts& counts);

// The -SeaFaces table: one row per line, comma separated, '#' starts a comment.
//   face,<id>,<x0>,<y0>,<x1>,<y1>,<out x>,<out y>,<top z>   a water face (out is normalised)
//   avoid|water|solid,<min x>,<min y>,<max x>,<max y>       rects for seaLadders' avoid and seaExitReach
//   exit,<x>,<y>                                           another way out (the slip's foot, steps)
//   bed,<z> / sea,<z>                                      the ladders' foot and the water line (defaults -3 and 0)
struct SeaAccessTable {
    std::vector<SeaFace> faces;
    std::vector<SeaRect> avoid, water, solid;
    std::vector<SeaPoint> exits;
    double bedZ = -3.0;
    double seaZ = 0.0;
};

// False (error names the line) on an unknown row kind, a missing or bad number, a face with no outward direction or
// a rect whose max is under its min.
FTCORE_API bool importSeaAccess(const std::string& text, SeaAccessTable& out, std::string* error = nullptr);

} // namespace ftcore
