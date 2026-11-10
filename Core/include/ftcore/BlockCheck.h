#pragma once

// Table-level hard rejects for a generated slice (the layout rules' sec. 3), run on the finished box table before any
// build:
//   lone      a building with no other building within kBlockLoneGap m (footprint gap) whose base is within
//             kBlockLoneDz m of its own: alone on its tread;
//   depth     a building whose block, street edge to street edge (alleys and streets under 3.5 m left out) along the
//             shorter of x and y, is under kBlockMinDepth m;
//   frontage  a kerbed street (pavement > 0, 10 m or wider) whose side, probed 1 m behind its wall line every 1 m inside
//             the slice, runs more than kBlockFrontGap m past no building, street mouth or exempt rect;
//   paving    facade to facade across a street, at stations every 2 m inside the slice (both sides built within
//             30 m; crossings skipped), over 18 m on a road 10 m or wider or 8 m on a narrower street, outside the
//             exempt rects (named squares, parks, forecourts, precincts, courts);
//   sink      a building whose ground, sampled every 1 m under its footprint, stands over kBlockMaxSink m above its z0.
// Buildings: boxes of kind kit, box or slab at least 2.5 m tall whose footprint meets the slice rect.
//
// Plain C++20. Allocates (build time only).

#include "ftcore/Api.h"
#include "ftcore/BlockGen.h"
#include "ftcore/MassingPlan.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ftcore {

inline constexpr double kBlockLoneGap = 6.0;
inline constexpr double kBlockLoneDz = 3.0;
inline constexpr double kBlockMinDepth = 24.0;
inline constexpr double kBlockFrontGap = 2.0;
inline constexpr double kBlockPaveRoad = 18.0;
inline constexpr double kBlockPaveLane = 8.0;
inline constexpr double kBlockMaxSink = 4.0;

struct BlockCheckInput {
    const std::vector<MassingBox>* boxes = nullptr;
    std::vector<MassingStreet> streets; // the street table plus the loop roads
    std::vector<BlockRect> exempt;
    BlockRect rect;
    const BlockGround* ground = nullptr;
};

struct BlockCheckIssue {
    std::string rule, id;
    double x = 0.0, y = 0.0, value = 0.0;
};

struct BlockCheckReport {
    std::size_t buildings = 0, stations = 0, probes = 0;
    std::size_t lone = 0, depth = 0, frontage = 0, paving = 0, sink = 0;
    double minDepth = 0.0, maxPaveRoad = 0.0, maxPaveLane = 0.0, maxSink = 0.0;
    std::vector<BlockCheckIssue> issues;
    bool ok() const { return lone + depth + frontage + paving + sink == 0; }
};

// Whether a box counts as a building for the checks.
FTCORE_API bool blockIsBuilding(const MassingBox& b);
// Runs the checks. True when nothing is rejected.
FTCORE_API bool blockCheck(const BlockCheckInput& in, BlockCheckReport& out);
// The report as text, its last line "BLOCKCHECK OK" or "BLOCKCHECK FAIL".
FTCORE_API std::string blockCheckText(const BlockCheckReport& r, std::size_t listMax = 200);

} // namespace ftcore
