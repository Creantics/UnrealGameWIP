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
//   lanterns  6 large, a pair flanking each lit bay's entrance as the shops hang them: the glazed front's under the eave
//             (1.0 m out, 2.6 m apart about its door, at 1.5 times the mesh), each lattice front's under its pent roof
//             (0.6 m out, 2.65 m up, 2.8 m apart about its entrance, at the mesh's own size)
//   strings   S1 at y 538.4, S2 at y 552: from the facade (x 908, 3.55 m up, under the eave) to a post's top (x 901.3,
//             3.8 m up), sag 0.4 m at the middle; nine small lanterns 0.75 m apart, centred on the span, each hung
//             under the cable
//   light     only the lamp pool's: lamp slots (warm, straight down) under each string's middle, between each pair of
//             large lanterns and over each lit entrance, close to its face; and the lit bays' shop slots, one inside
//             each lit room, deep and high, facing out through its front; the rooms' ceiling panels and the lanterns
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
    // The large lanterns: a pair flanking each lit bay's entrance (the first kHeroLargeLanterns / 2 lit bays, south to
    // north). Under the eave (a glazed front): largeOut from the shop face, hooked to the eave's underside, the mesh at
    // largeScale, largePairGap apart about the door.
    double largeOut = 1.0, largeUp = 3.3, largePairGap = 2.6;
    double largeHeight = 0.6, largeDiameter = 0.45;  // the mesh's own size; placed largeScale times it from its hook
    double largeScale = 1.25;  // tune 3 (2026-11-16): 0.75 m
    // Under a lattice front's pent roof (EAVE_PENT_09: 0.85 m out from 3.0 m at 22 degrees, its fascia's foot 2.50 m up):
    // hooked pentHookOut from the face and pentHookUp over the ground, clear of the fascia, the mesh at pentScale,
    // pentPairGap apart about the entrance (2.25 m wide).
    double pentHookOut = 0.6, pentHookUp = 2.65, pentScale = 1.0, pentPairGap = 2.8;
    // Where a bay's entrance is along the lane: a lattice front's (SHOP_KOSHI_54, its entrance 3.0..5.25 m along the
    // piece from the bay's north edge) this far south of the north edge; a glazed front's door at the bay's middle.
    double koshiEntranceFromNorth = 4.125;
    // The lanterns' paper (the night lamp head master's LampBase, LampColor and LampStrength): a deep amber paper and a
    // glow low enough that the body stays under clipping at the hero exposure and its ribs read at a metre. Tune 3's
    // values (2026-11-16, passed as the corner's lighting base).
    float lanternGlow = 0.34f;
    float lanternColour[3] = {1.0f, 0.22f, 0.02f};
    float lanternBase[3] = {0.45f, 0.16f, 0.04f};
    // The strings.
    double stringY[kHeroStrings] = {538.4, 552.0};
    double stringFacadeUp = 3.55, stringPostX = 901.3, stringPostUp = 3.8, stringSag = 0.4;
    double stringPitch = 0.75;    // m between two small lanterns, along the plan
    double smallHeight = 0.32, smallDiameter = 0.25;
    double smallDrop = 0.05;      // m from the cable to a small lantern's top (its clip)
    double postHeight = 4.0;      // a string post's top over the ground, the cable's anchor 0.2 under it
    // The pool's lamp slot under each string's middle: this far under the cable's lowest point, at this gain of a
    // street lamp's candela.
    double stringLampUnder = 0.35;
    float stringLampGain = 0.30f;
    // The pool's lamp slot for each pair of large lanterns: between the pair, this far under their hooks, so the faces,
    // the noren and the paving under them take its warm pool.
    double pairLampUnder = 0.1;
    float pairLampGain = 0.10f;
    // The pool's lamp slot over each lit entrance: this far out from the shop face (straight down, so it grazes the
    // noren, the lattice face and the threshold), under the pent or the eave at the up below.
    double entranceLampOut = 0.35, entranceLampUpPent = 2.7, entranceLampUpEave = 3.55;
    float entranceLampGain = 0.06f;
    // A lit bay's one light, its pool shop slot: this far inside the room behind its shop face and this high over the
    // ground (under a lattice room's 2.45 m ceiling and its panels), facing out and pitched down shopLightPitch degrees,
    // so it lights the room's front and falls through the front onto the lane; at shopLightGain of a shop light.
    // Tune 3: 2.0 in, 2.35 up, gain 4.5 (tune 3 ran 1.5 at -HeroShopScale=3).
    double shopLightIn = 2.0, shopLightUp = 2.35;
    float shopLightPitch = -35.f;
    float shopLightGain = 4.5f;
    // A lit bay may take a second slot facing in (east), pitched near level from just behind its front, so its room's
    // back wall takes the light (the front slot faces out: on a glazed bay it lights only the front and the floor). By
    // default (tune 3) only the glazed bay 4 has one, at gain 3 (tune 3 ran 1.0 at -HeroShopScale=3).
    bool shopBack[kHeroBays] = {false, false, false, true, false};
    double shopBackIn = 0.5, shopBackUp = 2.3;
    float shopBackPitch = -10.f;
    float shopBackGain = 3.0f;
    // The quay's lamp masts in the camera's view (both banks of the canal beside the lane, to its far end) have no lamp
    // in the pool: inside this plan box each gives way to a rail piece (heroCornerClearMasts).
    double mastClearX0 = 890.0, mastClearX1 = 903.0, mastClearY0 = 505.0, mastClearY1 = 640.0;
    // Inside the corner the light comes from the lanterns and the shops: no street lamp (a road's or a lane's) stands in
    // this plan box.
    double lampClearX0 = 897.0, lampClearX1 = 909.0, lampClearY0 = 503.0, lampClearY1 = 560.0;
    // The lane's foot carried south past the paving to here, so nothing (a parked car, a scooter, a prop) stands between
    // the camera and the corner.
    double nearClearY0 = 503.0;
    // The camera.
    double eye[3] = {904.5, 511.0, 5.78};
    double at[3] = {904.5, 551.0, 3.68};
    double lensMm = 35.0;
    // Step 2 (2026-11-16): the lane's dressing (heroCornerDressing). Hanging signs on the shop face every signPitch,
    // large and small in turn at two heights: under a lattice front's pent (its fascia foot pentFasciaUp) or under the
    // eave; none within signClear of a lantern's hook, a string's anchor or an entrance (entranceHalf either side).
    double pentFasciaUp = 2.50;
    double signPitch = 3.5, signClear = 0.7, entranceHalf = 1.15;
    double signUpPent[2] = {2.42, 2.22}, signUpEave[2] = {3.35, 2.95};
    // Banners from the eave's edge over the dark bays, two a bay, bannerDrop long.
    double bannerUp = 3.53, bannerIn = 0.1, bannerDrop = 1.6;
    // The far end closed: a gate across the lane gateBack in from its end, its four lanterns' hooks gateHookUp.
    double gateBack = 0.4, gateWidth = 7.4, gateHookUp = 3.42, gateLanternScale = 1.0;
    double gateHookX[4] = {-2.4, -0.8, 0.8, 2.4};
    // The face ruling (2026-11-16, heroCornerAreaLights): each lit bay's front a wide soft key, a rect light frontIn
    // inside its shop face, frontUp (its centre) over the paving, the size of its opening: a lattice front its room
    // (koshiRoomX0..X1 along the piece from the bay's north edge), a glazed front glazedFrontWidth about the bay's middle.
    double frontIn = 0.1, frontUp = 1.3, frontHeight = 2.2;
    double koshiRoomX0 = 0.15, koshiRoomX1 = 5.25, glazedFrontWidth = 6.0;
    // The cool floor: a rect light skyOut west of the lane (the open canal side), the lane's length, skyHeight tall from
    // skyUp (its centre) over the paving, facing the bays.
    double skyOut = 1.0, skyUp = 2.5, skyHeight = 5.0;
};

struct HeroPoint {
    double x = 0.0, y = 0.0, z = 0.0;
};

struct HeroLantern {
    HeroPoint centre;     // the paper's centre
    HeroPoint hook;       // where it hangs from (its top)
    double scale = 1.0;   // the mesh's scale about its hook
    bool large = false;
    bool pent = false;    // a large lantern under a lattice front's pent roof (else under the eave)
    int string = -1;      // 0, 1: on that string; -1: a large one
    int bay = -1;         // a large one: the lit bay whose entrance it flanks
};

// A lamp slot of the pool's (a warm street-lamp spot, straight down) at gain times a street lamp's candela.
enum class HeroLampKind { String, Pair, Entrance };
struct HeroLampSlot {
    HeroPoint at;
    float gain = 1.f;
    HeroLampKind kind = HeroLampKind::String;
    int index = 0;  // the string's, or the pair's (0 the southmost lit bay's)
};
inline constexpr int kHeroLampSlots = kHeroStrings + 2 * (kHeroLargeLanterns / 2);

struct HeroCornerLayout {
    MassingBox pave;                 // the lane's paving slab (kind "paving")
    MassingBox nearClear;            // the lane's width from nearClearY0 to the paving's south end (kind "clear"): kept
                                     // free of cars, scooters and props, never built
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
    // Every lamp slot: the strings' first, then per pair its pair slot and its entrance slot.
    HeroLampSlot lamps[kHeroLampSlots];
    int lampCount = 0;
    // Per pair (the lit bays' in order, south to north): its bay and the middle of that bay's entrance along the lane.
    int pairBay[kHeroLargeLanterns / 2] = {-1, -1, -1};
    double entranceY[kHeroLargeLanterns / 2] = {};
    int pairCount = 0;
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

// The night lamp pool's slots, merged from every night-light set in the world (the strip's and a streamed sub-level's).
// A set is one actor's lamps or shop lights: count points and its own gain table (lampSlotGain within the set).
struct LampPoolPoint {
    double x = 0.0, y = 0.0, z = 0.0;
};
struct LampPoolSet {
    const LampPoolPoint* at = nullptr;
    std::size_t count = 0;
    const float* gains = nullptr;
    std::size_t gainCount = 0;
};
struct LampPoolSlot {
    LampPoolPoint at;
    float gain = 1.f;
    int set = 0;            // which set it came from
    std::size_t index = 0;  // its index in that set
};

// Every set's slots in set order, then point order: one set gives exactly its points with lampSlotGain's gains, as the
// pool had before it merged sets.
FTCORE_API void lampPoolMerge(const LampPoolSet* sets, std::size_t setCount, std::vector<LampPoolSlot>& out);

// Slot pairs from two different sets closer than tolerance (in the points' units): the same light claimed twice, e.g. a
// corner left in the strip and also built in its sub-level. Returns the number of pairs; the first pair's slot indices
// go to *firstA < *firstB when there is one.
FTCORE_API std::size_t lampPoolClashes(const std::vector<LampPoolSlot>& slots, double tolerance, std::size_t* firstA,
                                       std::size_t* firstB);

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

// Every shop slot with its own aim: each lit bay's front slot (heroCornerShopLights, at shopLightPitch and shopLightGain)
// and, where shopBack is set on a lit bay, its back slot shopBackIn inside the shop face and shopBackUp over the ground,
// facing east (into the room) at shopBackPitch and shopBackGain. Fronts first in bay order, then backs. Returns how many.
constexpr int kHeroShopSlots = 2 * kHeroBays;
struct HeroShopSlot {
    MassingShopLight at;
    float pitch = 0.f;
    float gain = 0.f;
    int bay = 0;
    bool back = false;
};
FTCORE_API int heroCornerShopSlots(const HeroCornerSpec& spec, HeroShopSlot out[kHeroShopSlots]);

// The plan points a still's luma probe reads (the build writes them out, so the probe never keeps its own copy of the
// layout): each large lantern's body; per lit bay the shop face either side of its entrance at 1.0 and 2.0 m, the lane
// under its pair, its room's back wall (probeRoomIn behind the face, in the room's top third) and, on a lattice front,
// its noren and lattice; per dark bay its face at 1.5 m. Returns how many (at most max).
struct HeroProbe {
    char name[32] = {};
    HeroPoint at;
};
inline constexpr int kHeroMaxProbes = 64;
FTCORE_API int heroCornerProbes(const HeroCornerSpec& spec, const HeroCornerLayout& layout, HeroProbe* out, int max);

// The quay's lamp masts inside the spec's mast-clear box become railing spans that close the rail's gap round the mast
// (kQuayMastPlate long, rail high, along the rail: the mast's rot turned 90 degrees, id "<mast id>-RAIL"). Returns how
// many it changed; other points are left as they are.
FTCORE_API int heroCornerClearMasts(const HeroCornerSpec& spec, std::vector<StandInPoint>& points);

// Whether a street lamp at plan (x, y) is kept out of the corner (inside the spec's lamp-clear box, edges included).
FTCORE_API bool heroCornerKeepsLampOut(const HeroCornerSpec& spec, double x, double y);

// Step 2 (2026-11-16): the hero corner's furnishing and dressing as pieces by role. The Unreal side maps each piece to
// its mesh (/Game/Props/HeroCorner/SM_HC_<heroPieceName>) and leaves out a piece whose mesh isn't imported.
enum class HeroPiece : int {
    Shelf, Counter, HangGoods, Lantern, Crate, Bucket, Figure, StockPot, Steam, Jars, Sacks, Bottles, GoodsHanging,
    UmbrellaStand, SignLarge, SignSmall, Banner, Bicycle, Gate, GateLantern, Scooter, Count
};
FTCORE_API const char* heroPieceName(HeroPiece piece);

// A piece's place. Room terms: x along the front from its north end, y in from the front, z over the floor. Plan
// terms: the plan's x, y and z. face is the way its front (the mesh's -Y) looks, a unit vector in the same terms
// (room: (0, -1) to the lane; plan: (0, -1) down the lane to the camera, (-1, 0) off the shop face).
struct HeroItem {
    HeroPiece piece = HeroPiece::Crate;
    HeroPoint at;
    double faceX = 0.0, faceY = -1.0;
    double scale = 1.0;
    int bay = -1;
    bool marked = false;  // a lantern carrying an invented mark (the marked mesh)
};

// A standing piece's footprint (its box in its own terms, turned by its face, whatever its z); false for a piece that
// hangs or sits on a counter.
FTCORE_API bool heroItemFootprint(const HeroItem& item, double& x0, double& x1, double& y0, double& y1);

// A lit lattice room (room terms): width along the front, depth in, height to the ceiling; the entrance's span along
// the front. A shop takes two shelf units on its back wall, a counter behind the lattice with jars and bottles, sacks
// by the door, hanging goods, a crate and one paper lamp; the eatery two counters across the room (the cook's side
// behind), two shelf units on the back wall, the figure behind the counter turned three-quarter to its stock pot (45
// degrees off the lane's line), the pot's steam, a lamp. Floor
// pieces keep out of each other and of the entrance's first doorClear in. Returns how many (at most max).
struct HeroRoomSpec {
    double width = 5.1, depth = 2.8, height = 2.45;
    bool eatery = false;
    double doorX0 = 2.85, doorX1 = 5.1, doorClear = 1.2;
};
inline constexpr int kHeroRoomMax = 16;
// SM_HC_Counter (2.4 m) carries its own dressing along its middle (tools/blender/hero_corner_props.py counter()): a
// packet stack at local x -1.0, a bowl tray at -0.05 and a cash box at +0.98. A piece set on the counter stands at one of
// these two slots between them (local x, from the counter's centre), clear for a 0.6 m stock pot or a 0.42 m tray.
inline constexpr double kHeroCounterSlotLeft = -0.56, kHeroCounterSlotRight = 0.47;
FTCORE_API int heroRoomFurnish(const HeroRoomSpec& room, HeroItem* out, int max);

// The lane's dressing (plan terms): hanging signs and banners, clutter by the walls (crates, buckets, the bicycle, an
// umbrella stand), the gate across the far end with its lanterns and two parked scooters. Returns how many (at most
// max).
inline constexpr int kHeroDressingMax = 48;
FTCORE_API int heroCornerDressing(const HeroCornerSpec& spec, const HeroCornerLayout& layout, HeroItem* out, int max);

// Whether the n-th large lantern (0-based, in layout order) carries an invented mark: one in three.
inline bool heroLanternMarked(int largeIndex) { return largeIndex % 3 == 2; }

// The face ruling's area lights (plan metres): a wide soft key in each lit bay's front (sky false, bay its index) and
// the cool floor west of the lane in kHeroSkyFloors equal lengths end to end (sky true, bay -1; a light's attenuation
// is a sphere about its centre, so one 40 m source would light only the lane's middle). Each a rect light at (x, y, z),
// width along the lane (plan y), height upright, emitting along plan (fx, fy): the fronts west onto the lane, the
// floor east onto the bays; reach its attenuation radius, past the far corners of what it lights (the lane's far side
// for a front, its length of the shop faces for the floor).
struct HeroAreaLight {
    double x = 0.0, y = 0.0, z = 0.0, width = 0.0, height = 0.0, fx = 0.0, fy = 0.0, reach = 0.0;
    bool sky = false;
    int bay = -1;
};
inline constexpr int kHeroSkyFloors = 4;
inline constexpr int kHeroAreaLights = kHeroBays + kHeroSkyFloors;
FTCORE_API int heroCornerAreaLights(const HeroCornerSpec& spec, HeroAreaLight out[kHeroAreaLights]);

} // namespace ftcore
