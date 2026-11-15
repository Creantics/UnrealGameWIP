#pragma once

// Table-level hard rejects for a generated slice (the layout rules' sec. 3), run on the finished box table before any
// build:
//   lone      a building with no other building within kBlockLoneGap m (footprint gap) whose base is within
//             kBlockLoneDz m of its own: alone on its tread;
//   depth     a building whose block, street edge to street edge (alleys and streets under 3.5 m left out) along the
//             shorter of x and y, is under kBlockMinDepth m;
//   frontage  a road (kerbed, 10 m or wider) whose side, probed 1 m behind its footway's back edge (half its width
//             out: a street's width holds its footways) every 1 m inside the slice, runs more than kBlockFrontGap m
//             past no building, street mouth or exempt rect;
//   paving    open paving across a street beyond its footway edges (facade or yard wall kBlockWallClose m or higher to
//             the same across, less the street's width), at
//             stations every 2 m inside the slice (both sides built within 30 m; crossings skipped), over
//             kBlockOpenRoad m on a road 10 m or wider or kBlockOpenLane m on a narrower street, outside the exempt
//             rects (named squares, parks, forecourts, precincts, courts);
//   sink      a building whose ground, sampled every 1 m under its footprint, stands over kBlockMaxSink m above its z0.
// Route rules (with routes on; roles as blockStreetRole):
//   main      a main street through the slice (a point within 20 m of it; kBlockLinkMax m long or longer, a shorter one
//             is a junction link) with no straight run of kBlockMainStraight m;
//   chain     a lane end inside the slice (or 20 m from it) on another lane, or a lane crossing another lane;
//   deadend   a lane end touching no street (1-2 m grace) and no reason rect (park, precinct, gate, grown 3 m);
//   walk      a lane or alley point in the slice (2 m samples) more than kBlockWalkMax m along the streets from a main
//             street (junctions where a sample lies on another street);
//   section   a street through the slice off its class: a road (10 m or wider, kerbed) 10-14 m with footways of
//             2-3 m; a lane (over kBlockAlleyMax m) 4-6 m with kerbed footways of 1-1.5 m (no kerbless shared lanes);
//             an alley kBlockAlleyMax m or narrower with none;
//   sight     a main street through the slice from none of whose points (10 m apart, eye kBlockEyeZ over the ground)
//             a sight target (the landmark, the sea) is clear of every building's box and the ground. Skipped with no
//             targets. Junctions (walk) and lane ends touch a street within 2 m of its edge (lane mouths stop short).
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
// m of open paving beyond a street's footway edges, both sides together: the r9 facade-to-facade limits (18 m, 8 m)
// less the narrowest street of each (a 10 m road, a 3 m alley), so the r9 verdicts stand; pending a ruling.
inline constexpr double kBlockOpenRoad = 8.0;
inline constexpr double kBlockOpenLane = 5.0; // on a lane or alley
inline constexpr double kBlockMaxSink = 4.0;
inline constexpr double kBlockWalkMax = 80.0;
inline constexpr double kBlockEyeZ = 1.7;
inline constexpr double kBlockWallClose = 1.8; // m: a wall this high closes a street side (paving)
inline constexpr double kBlockLinkMax = 50.0; // a road shorter than this is a junction link, not a main street

// A point a main street must see (the landmark's upper part, a point on the sea).
struct BlockSightTarget {
    std::string id;
    double x = 0.0, y = 0.0, z = 0.0;
};

struct BlockCheckInput {
    const std::vector<MassingBox>* boxes = nullptr;
    std::vector<MassingStreet> streets; // the street table plus the loop roads
    std::vector<BlockRect> exempt;
    BlockRect rect;
    const BlockGround* ground = nullptr;
    bool routes = false;                    // the route rules
    std::vector<BlockSightTarget> sights;   // sight targets
    std::vector<BlockRect> reasons;         // where a lane may end (park, precinct, gate)
    bool sea = false;                       // also the sea: water (ground under 0) due east of x seaX
    double seaX = 0.0;
    std::vector<MassingBox> occluders;      // more boxes that block sight only (towers kept in their own table)
};

struct BlockCheckIssue {
    std::string rule, id;
    double x = 0.0, y = 0.0, value = 0.0;
};

struct BlockCheckReport {
    std::size_t buildings = 0, stations = 0, probes = 0;
    std::size_t lone = 0, depth = 0, frontage = 0, paving = 0, sink = 0;
    double minDepth = 0.0, maxPaveRoad = 0.0, maxPaveLane = 0.0, maxSink = 0.0;
    bool routes = false;
    std::size_t mains = 0, links = 0, lanes = 0, alleys = 0, walkSamples = 0, walkOver = 0, sightRays = 0;
    std::size_t mainShort = 0, chain = 0, deadEnd = 0, walk = 0, sight = 0, section = 0;
    double maxWalk = 0.0;
    std::vector<BlockCheckIssue> issues;
    bool ok() const { return lone + depth + frontage + paving + sink + mainShort + chain + deadEnd + walk + sight + section == 0; }
};

// Whether a box counts as a building for the checks.
FTCORE_API bool blockIsBuilding(const MassingBox& b);
// Runs the checks. True when nothing is rejected.
FTCORE_API bool blockCheck(const BlockCheckInput& in, BlockCheckReport& out);
// The report as text, its last line "BLOCKCHECK OK" or "BLOCKCHECK FAIL".
FTCORE_API std::string blockCheckText(const BlockCheckReport& r, std::size_t listMax = 200);

} // namespace ftcore
