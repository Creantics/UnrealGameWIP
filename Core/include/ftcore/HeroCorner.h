#pragma once

// The hero corner: one 40 m stretch of canal-side lane built to the full look first (the slice's beauty test). Step 1 is
// the lane's stone paving, a row of five two-storey shop bays with a deep eave over the lane (bays 1, 3 and 4 lit from
// inside, 3 the eatery, 2 and 5 dark), six large paper lanterns hung under the eave in pairs, and two strings of nine
// small lanterns across the lane from the eave to a post at the canal's edge.
//
// Every number is the corner's plan (plan frame, metres, +y north; z absolute over the datum), kept here so the build,
// the tests and the meshes read one source:
//   lane      x 901..908, y 515..555, on ground at z 4.18; stone setts end to end
//   bays      x 908..916, 8.1 m each (nine of the kit's 0.9 m steps) from y 514.75, centred on the lane; their west
//             face the shopfront; built as kit lots (heroCornerLots): two storeys, the shop storey 3.6 m (the timber
//             lattice fronts of bay 1 and the eatery 3.0 m)
//   eave      1.2 m out over the lane (x 906.8..908) at 3.65 m, clear of the kit's 3.6 m shop storey, 0.12 m thick;
//             in runs between the two timber lattice fronts (bay 1 and the eatery), each with its own tiled pent roof
//             at 3.0 m
//   lanterns  6 large, 1.0 m out (x 907.0), centre 3.3 m up, a pair (1.6 m apart) about y 526, 543 and 549.5 (the
//             eatery's flanked), each under the eave
//   strings   S1 at y 536, S2 at y 552: from the facade (x 908, 3.55 m up, under the eave) to a post's top (x 901.3,
//             3.8 m up), sag 0.4 m at the middle; nine small lanterns 0.75 m apart, centred on the span, each hung
//             under the cable
//   light     only the lamp pool's: one lamp slot under each string's middle (warm, half a street lamp) and the lit
//             bays' shop slots, one inside each lit room near its front; the rooms' ceiling panels and the lanterns
//             are emissive, no lights of their own
//   camera    (904.5, 511.0, 5.78) looking north at (904.5, 551.0, 3.68), 35 mm
//
// heroCornerLayout fills a fixed layout (no allocation but the boxes' id strings). Plain C++20.

#include "ftcore/Api.h"
#include "ftcore/BuildingLots.h"
#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/StandIns.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr int kHeroBays = 5;
inline constexpr int kHeroLargeLanterns = 6;
inline constexpr int kHeroStrings = 2;
inline constexpr int kHeroStringLanterns = 9;
// The cable's polyline: this many points from the facade end to the post end, both included.
inline constexpr int kHeroCablePoints = 25;
// A bay's shopfront: boxFace order face 2 (-u, west for a box turned 0).
inline constexpr int kHeroShopFace = 2;

struct HeroCornerSpec {
    double ground = 4.18;  // the lane's ground, z
    // The lane.
    double laneX0 = 901.0, laneX1 = 908.0, laneY0 = 515.0, laneY1 = 555.0;
    double paveThick = 0.08;  // the paving slab: top paveTop over the ground, this thick
    double paveTop = 0.02;
    // The bays: from bayY0 north, bayWidth each (a whole number of kLotGrid steps).
    double bayY0 = 514.75;
    double bayDepth = 8.0, bayWidth = 8.1, bayHeight = 6.6, baySink = 0.3;
    bool bayLit[kHeroBays] = {true, false, true, true, false};
    int eateryBay = 2;  // 0-based: bay 3
    // The timber lattice (koshi) fronts, each under its own pent roof: the eatery's and bay 1's.
    bool bayKoshi[kHeroBays] = {true, false, true, false, false};
    // The eave.
    double eaveOut = 1.2, eaveUp = 3.65, eaveThick = 0.12;
    // The large lanterns.
    double largeOut = 1.0, largeUp = 3.3, largePairGap = 1.6;
    double largePairs[kHeroLargeLanterns / 2] = {526.0, 543.0, 549.5};
    double largeHeight = 0.6, largeDiameter = 0.45;
    // The strings.
    double stringY[kHeroStrings] = {536.0, 552.0};
    double stringFacadeUp = 3.55, stringPostX = 901.3, stringPostUp = 3.8, stringSag = 0.4;
    double stringPitch = 0.75;    // m between two small lanterns, along the plan
    double smallHeight = 0.32, smallDiameter = 0.25;
    double smallDrop = 0.05;      // m from the cable to a small lantern's top (its clip)
    double postHeight = 4.0;      // a string post's top over the ground, the cable's anchor 0.2 under it
    // The pool's lamp slot under each string's middle: this far under the cable's lowest point, at this gain of a
    // street lamp's candela.
    double stringLampUnder = 0.35;
    float stringLampGain = 0.5f;
    // A lit bay's one light, its pool shop slot: this far inside the room behind its shop face and this high over the
    // ground (under a lattice room's 2.45 m ceiling and its panels), facing out, so it lights the front of the room and
    // spills through the front onto the lane.
    double shopLightIn = 0.6, shopLightUp = 2.1;
    // The quay's lamp masts in the camera's view (both banks of the canal beside the lane, to its far end) have no lamp
    // in the pool: inside this plan box each gives way to a rail piece (heroCornerClearMasts).
    double mastClearX0 = 890.0, mastClearX1 = 903.0, mastClearY0 = 505.0, mastClearY1 = 640.0;
    // The camera.
    double eye[3] = {904.5, 511.0, 5.78};
    double at[3] = {904.5, 551.0, 3.68};
    double lensMm = 35.0;
};

struct HeroPoint {
    double x = 0.0, y = 0.0, z = 0.0;
};

struct HeroLantern {
    HeroPoint centre;     // the paper's centre
    HeroPoint hook;       // where it hangs from (its top)
    bool large = false;
    int string = -1;      // 0, 1: on that string; -1: under the eave
};

struct HeroCornerLayout {
    MassingBox pave;                 // the lane's paving slab (kind "paving")
    MassingBox eaves[kHeroBays / 2 + 1];  // the eave's slabs over the lane (kind "eave", ids HC-EAVE-<n> south to north):
                                          // one per run of bays between the lattice fronts
    int eaveCount = 0;
    MassingBox bays[kHeroBays];      // the shop bays (kind "kit", ids HC-BAY-<n><letter>)
    bool bayLit[kHeroBays] = {};
    int eateryBay = 0;
    HeroLantern lanterns[kHeroLargeLanterns + kHeroStrings * kHeroStringLanterns];
    std::size_t lanternCount = 0;
    HeroPoint cable[kHeroStrings][kHeroCablePoints];
    HeroPoint postFoot[kHeroStrings];  // a post's foot on the ground; it stands postHeight
    HeroPoint stringLamp[kHeroStrings];
    float stringLampGain = 1.f;
    HeroPoint eye, at;
    double lensMm = 35.0;
};

// The cable's height over the datum at t (0 the facade end, 1 the post end): the chord's height less a parabola's
// 4 * sag * t * (1 - t).
FTCORE_API double heroCableZ(const HeroCornerSpec& spec, double t);

// The corner by its plan. Each bay's id is HC-BAY-<n> followed by the first letter (a, b, ...) whose box, fronting the
// lane on face kHeroShopFace, takes the shophouse facade and a rendered, tiled, board or brick wall; ids that never do
// keep the letter 'a'.
FTCORE_API void heroCornerLayout(const HeroCornerSpec& spec, HeroCornerLayout& out);

// A lamp slot's gain on the street lamps' candela: gains[i] where the map's gain table (count entries) has one, 1
// past it. A map built before the table, or with no hero corner, has an empty table, so every lamp keeps its light.
inline float lampSlotGain(const float* gains, std::size_t count, std::size_t i) {
    return gains && i < count ? gains[i] : 1.f;
}

// Whether id is a hero corner bay's (HC-BAY-<n>...), and its 0-based bay index.
FTCORE_API bool heroCornerBay(std::string_view id, int* index);

// The bays as kit lots (BuildingLots.h, facadePlacements): a run along the bays' lane face, A at its north end (the
// left end seen from the lane), and one lot per bay, lots[i] for bay i (south to north). Two storeys each, no awnings
// (the eave covers the lane), every upper storey flat (BAY_WIN_18 and blanks, nothing out under the eave):
//   koshi bays TownHouse, ground SHOP_KOSHI_54 (the timber lattice front, a real shallow room behind it)
//   lit bays   Tiled, ground SHOP_GLAZED_81 (its glass the true panes over a real bay)
//   dark bays  WalkUp, ground SHOP_SHUTTER_63 (shut)
// Roofs, walls and palettes alternate so neighbours differ in at least two traits.
struct HeroLots {
    FrontageRun run;
    BuildingLot lots[kHeroBays];
    bool lit[kHeroBays] = {};  // lit from inside: a real room behind its front
    int eatery = -1;           // the lot whose room is the eatery's (a counter, not shelves)
};
FTCORE_API void heroCornerLots(const HeroCornerSpec& spec, HeroLots& out);

// The lit bays' lights (the lamp pool's shop slots, the only light a lit bay has): one per lit bay at the middle of its
// front, shopLightIn inside the room behind its shop face and shopLightUp over the ground, facing west (out onto the
// lane). Returns how many (at most kHeroBays).
FTCORE_API int heroCornerShopLights(const HeroCornerSpec& spec, MassingShopLight out[kHeroBays]);

// The quay's lamp masts inside the spec's mast-clear box become railing spans that close the rail's gap round the mast
// (kQuayMastPlate long, rail high, along the rail: the mast's rot turned 90 degrees, id "<mast id>-RAIL"). Returns how
// many it changed; other points are left as they are.
FTCORE_API int heroCornerClearMasts(const HeroCornerSpec& spec, std::vector<StandInPoint>& points);

} // namespace ftcore
