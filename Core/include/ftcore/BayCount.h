#pragma once

// Ground-floor shop bays per street, for planning real modelled shop bays (an offline planning aid, no game logic).
//
// The count follows what the world build draws. A windowed box is a visible, flat building box no kit mesh stands in for
// (and not a crane or boat). A face of it is a street face when massingFrontage's fronting bit is set (the face's middle
// fronts a street of the frontage street table). A street face's shop bays are the bays of its window grid: the type
// massingFacadeData picks, windowFaceGrid of the face's length and its height over its street floor (box height less
// MassingWindowData::base[f]) with the type's grid spec and window width, exactly as the shop interiors (windowShopInterior)
// and the facade master cut them. Every bay counts, lit or not; litBays counts those windowShopLit lights at night (share
// BayCountConfig::shopLit) and lights the night spill points massingShopLights sets on the face (one per
// BayCountConfig::lightSpacing of face at most, none where the shop storey doesn't fit under the box's top).
//
// A face is credited to the street massingFrontage reads for it: among the streets whose edge lies within
// kMassingFrontReach of the point 0.5 m out of the face's middle, with the nearest point of its polyline out from the face,
// the one with the nearest edge (ties to the lower index). The credit goes to the row of the same id in `streets` (the
// table carrying pavement_m); a front street missing from it is appended after the table's rows.
//
// "Walked" streets are those of `streets` with pavement_m > 0 (a footway). The densest point is found by a sweep along each
// walked street's centreline, every config.step metres (and at its end), counting the bays of every street whose centre
// lies within config.reach of the sample.
//
// Not reproduced: the build's top nudges and raises (a few cm), and a backdrop rock face wearing a box (a box FTBackdrop
// takes over has no windows). Plain C++20; allocates (offline use only).

#include "ftcore/Api.h"
#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ftcore {

struct BayCountConfig {
    double reach = 30.0;                          // m: the sweep's radius
    double step = 5.0;                            // m: the sweep's spacing along a street's centreline, > 0
    double lightSpacing = kMassingShopLightSpacing;  // m: as the build's night shop lights
    float shopLit = 0.8f;                         // share of shop bays lit at night (the build's shop interiors' value)
};

struct BayStreet {
    std::string id, kind;
    double length = 0.0;     // m along the centreline
    double width = 0.0;      // m, wall to wall
    double pavement = 0.0;   // m each side; > 0: walked
    std::size_t faces = 0;   // street faces credited to it that hold at least one bay
    std::size_t bays = 0;    // shop bays on those faces
    std::size_t litBays = 0; // of them lit at night
    std::size_t lights = 0;  // night spill points on its faces
};

struct BayTotals {
    std::size_t streets = 0;   // rows in the group
    std::size_t withBays = 0;  // of them with at least one bay
    double length = 0.0;       // m
    std::size_t faces = 0, bays = 0, litBays = 0, lights = 0;
};

struct BayWorst {
    bool found = false;         // false when no walked street was swept
    std::string street;         // the walked street the sweep point lies on
    double x = 0.0, y = 0.0;    // plan metres
    std::size_t bays = 0;       // bays within reach of it
};

struct BayCountResult {
    std::vector<BayStreet> streets;  // the `streets` rows in order, then any front street missing from them
    BayTotals all, walked;
    BayWorst worst;
    double reach = 0.0;
    std::size_t boxes = 0, hosts = 0, windowed = 0;
    std::size_t streetFaces = 0;     // street faces of windowed boxes
    std::size_t emptyFaces = 0;      // of them with no bay (too low or too narrow)
    std::size_t unattributed = 0;    // street faces no street was found for (0 unless the tables disagree)
};

// Counts the bays. `boxes`: the map's boxes; `replaced[i]`: box i is hidden under a kit mesh (the kit table's "box" ids;
// empty for none); `streets`: the street table (ids, kinds, pavement_m); `frontStreets`: the table the build's frontage
// reads (-PropStreets, else the same table); `rules`: the frontage rules (their kinds don't matter here, but with none
// no face fronts anything, which is an error); `ground`: the height field. False, with a reason, on empty rules or a
// non-positive step.
FTCORE_API bool bayCount(const std::vector<MassingBox>& boxes, const std::vector<bool>& replaced,
                         const std::vector<MassingStreet>& streets, const std::vector<MassingStreet>& frontStreets,
                         const std::vector<MassingFrontRule>& rules, const MassingGround& ground,
                         const BayCountConfig& config, BayCountResult& out, std::string* error = nullptr);

}  // namespace ftcore
