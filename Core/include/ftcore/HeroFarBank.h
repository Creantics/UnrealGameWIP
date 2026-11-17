#pragma once

// The hero corner's sight line west, across the canal (CN-01 of the strip's road table: water x 892..901 from y 424 to
// 636, its surface at z 2.5; the far quay's rail at x 892.25, the near quay's at x 900.75; the far bank's road RD-01
// runs north at x 885, its west pavement's street trees at x 880.5 and the grey road-front blocks behind them at x
// 864..878). Four things, laid out here so the build and the tests read one source:
//   fronts  a row of low lean-to fronts on the far pavement (their east face frontX, their back on RD-01's kerb), one or
//           two storeys from the modular kit (ground SHOP_KOSHI_54, SHOP_GLAZED_36 or SHOP_SHUTTER_63, upper BAY_WIN_18,
//           a pent EAVE_PENT_09 between two storeys of a lattice front, a lean-to of TILE_SLOPE_54_09 and RIDGE_09 on
//           top) on a stucco body of boxes, widths 4.5, 5.4 or 6.3 m, gaps left at the far exits and the footbridge's
//           landing; lanterns flanking each entrance, a lamp slot between a lit front's pair
//   rope    a rope-and-post edge on the near quay, just inside its rail: timber posts every postPitch at most, a sagging
//           rope (a polyline per span, kRopePoints points) between them, open at the near ladder and the corner's
//           string posts
//   boats   moored by the far quay, each a hull with a pointed bow, gunwales, a barrel canopy, a stern pole with one
//           small lantern and a lamp slot under it
//   trees   modelled trees on RD-01's west pavement behind the fronts, between the street trees already there, so the
//           grey blocks behind stand mostly hidden
// Plan frame (x east, y north, metres; z absolute). heroFarBankLayout fills a fixed layout and allocates nothing.

#include "ftcore/Api.h"
#include "ftcore/HeroCorner.h"

#include <cstdint>

namespace ftcore {

inline constexpr int kFarClearMax = 8;
inline constexpr int kFarFrontsMax = 24;
inline constexpr int kFarPiecesMax = 320;
inline constexpr int kFarBoxesMax = 160;
inline constexpr int kFarLanternsMax = 56;
inline constexpr int kFarLampsMax = 32;
inline constexpr int kQuayPostsMax = 32;
inline constexpr int kRopePoints = 7;
inline constexpr int kFarBoatsMax = 3;
inline constexpr int kFarTreesMax = 16;
inline constexpr int kFarTreeModels = 4;

// A stretch along the plan's y kept free.
struct HeroSpan {
    double y0 = 0.0, y1 = 0.0;
};

// The kit pieces the fronts use (Data/Look/building_kit_pieces.csv; kit frame: x along the facade from its left end, -y
// out of the face, z up, origin at the wall face's bottom-left).
enum class FarKit : int { ShopKoshi54, ShopGlazed36, ShopShutter63, BayWin18, EavePent09, TileSlope54, Ridge09, Count };
struct FarKitSize {
    double sx = 0.0, sy = 0.0, sz = 0.0, minX = 0.0, minY = 0.0, minZ = 0.0;
};
FTCORE_API const char* farKitName(FarKit kit);  // the kit id, e.g. "SHOP_KOSHI_54"
FTCORE_API FarKitSize farKitSize(FarKit kit);   // the kit table's bounds

enum class FarMat : int { Stucco, Timber, Dark, Rope };
// A box (or a cylinder) in a frame of its own: out (outX, outY) its depth axis, along = out turned 90 degrees
// anticlockwise (-outY, outX), up z; tilted tiltDeg about along so its top rises towards -out (a lean-to's back).
// CylinderUp: its axis up (diameter along x depth); CylinderAlong: its axis along (depth x height its section).
enum class FarShape : int { Cube, CylinderUp, CylinderAlong };
struct FarBox {
    HeroPoint centre;
    double along = 0.0, depth = 0.0, height = 0.0;
    double outX = 1.0, outY = 0.0;
    double tiltDeg = 0.0;
    FarShape shape = FarShape::Cube;
    FarMat mat = FarMat::Stucco;
    bool solid = true;  // collides (the fronts' bodies); boats and trims don't
};

// A kit piece at its origin, its face looking (outX, outY); lit: its glass glows at night.
struct FarPiece {
    FarKit kit = FarKit::ShopKoshi54;
    HeroPoint at;
    double outX = 1.0, outY = 0.0;
    bool lit = false;
    int front = -1;
};

struct FarFront {
    double y0 = 0.0, y1 = 0.0;  // along the bank, south to north
    double ground = 0.0;        // its foot
    double eave = 0.0;          // the lean-to's foot (the facade's top)
    double ridge = 0.0;         // the lean-to's top at the back
    int storeys = 1;
    bool lit = false;
    FarKit shop = FarKit::ShopKoshi54;
};

struct FarLantern {
    HeroPoint hook;
    double scale = 1.0;
    bool large = true;
};
struct FarLamp {
    HeroPoint at;
    float gain = 0.f;
};

struct QuayPost {
    HeroPoint foot;
    double height = 0.0, diameter = 0.0;
};
struct RopeSpan {
    HeroPoint pt[kRopePoints];
};

struct FarBoat {
    HeroPoint centre;  // on the water, the hull's middle
    double length = 0.0, beam = 0.0;
    double alongX = 0.0, alongY = 1.0;  // bow direction
    HeroPoint lanternHook;
    int lamp = -1;  // its lamp slot
};

struct FarTree {
    HeroPoint foot;
    double height = 0.0;
    double yawDeg = 0.0;
    int model = 0;  // 0..kFarTreeModels-1
};

struct HeroFarBankSpec {
    // The canal (CN-01): its rectangle canalX0..canalX1 (the banks' edges), its 1 m quay walls inside it, so the water
    // runs waterX0..waterX1 (the exits' rail gaps), the surface at waterZ; the near quay's rail at nearRailX.
    double canalX0 = 892.0, canalX1 = 901.0, waterX0 = 893.0, waterX1 = 900.0, waterZ = 2.5;
    double nearRailX = 900.75, nearGround = 4.18;
    // The far bank's ground under the fronts: farGroundAt at y farGroundFromY, rising farGroundRise per metre north
    // (the far exits' tops: 4.239 at y 512.5, 4.284 at y 582.5).
    double farGroundAt = 4.239, farGroundFromY = 512.5, farGroundRise = 0.00064;
    // The fronts: their faces on frontX (a strip clear to the rail), roofDepth deep (the lean-to tile's run, so their
    // backs stand on RD-01's kerb at x 888.5), from frontY0 to frontY1 with frontClear kept open: the far ladders (y
    // 512.5, 582.5), the far steps (545..550) and the footbridge's landing (y 560.8..563.2).
    double frontX = 891.25, roofDepth = 2.7, roofRise = 1.202;
    double frontY0 = 514.0, frontY1 = 606.0;
    HeroSpan frontClear[kFarClearMax] = {{510.5, 514.5}, {544.5, 551.5}, {558.0, 566.0}, {580.5, 584.5}};
    int frontClearCount = 4;
    // The far quay's lamp masts (x 892.25): a front over one keeps to one storey, so a pent eave (0.93 m out) never
    // meets a mast.
    double mastY[kFarClearMax] = {511.5, 536.5, 558.5, 586.5};
    int mastCount = 4;
    double mastClear = 0.4;
    double bodySink = 0.3;     // the body's foot under the ground
    double groundInset = 0.45; // the ground storey's body behind its shop piece (the glass stays clear of it)
    double upperStorey = 3.0;
    std::uint32_t seed = 0x5a17u;
    // Lanterns: a pair about each front's entrance, hookOut from its face, hookUnder under its first storey's top, at
    // lanternScale; a lit front's lamp slot between them at pairGain of a street lamp.
    double hookOut = 0.55, hookUnder = 0.35, lanternScale = 1.0, pairHalf = 1.25;
    float pairGain = 0.08f;
    // The rope edge: posts on ropeX (just inside the near rail) from ropeY0 to ropeY1, postPitch apart at most
    // (postPitchMin at least), postHeight tall and postDiameter thick; the rope ropeUp over the ground at each post,
    // sagging ropeSag at a span's middle. Open over ropeClear (the near ladder's gap; the corner's string posts).
    double ropeX = 901.2, ropeY0 = 515.3, ropeY1 = 555.0;
    double postPitch = 2.5, postPitchMin = 2.0, postHeight = 0.85, postDiameter = 0.18;
    double ropeUp = 0.72, ropeSag = 0.1, ropeDiameter = 0.035;
    HeroSpan ropeClear[kFarClearMax] = {{546.8, 548.2}, {537.9, 538.9}, {551.5, 552.5}};
    int ropeClearCount = 3;
    // Boats: moored boatOff off the far quay's face, bow north, their middles at boatY (clear of the far exits and the
    // footbridge); boatLength by boatBeam, freeboard over the water, draft under it; one small lantern on a stern pole,
    // its lamp slot boatLampUnder under the hook at boatLampGain.
    int boatCount = 3;
    double boatY[kFarBoatsMax] = {570.0, 577.0, 592.0};
    double boatOff = 0.35, boatLength = 6.0, boatBeam = 1.7, freeboard = 0.5, draft = 0.3;
    double poleHeight = 1.7, boatLampUnder = 0.3, boatLanternScale = 1.0;
    float boatLampGain = 0.05f;
    // Trees: on treeX from treeY0 to treeY1 every treePitch, none within treeClear of a street tree already there
    // (oldTreeY) nor in a lane's or alley's mouth (treeMouth); treeHeight tall, give or take treeHeightVar, the foot at
    // treeGround (the street trees' own foot height in the tree table, so a trunk is never left floating).
    double treeX = 880.5, treeY0 = 533.0, treeY1 = 640.0, treePitch = 7.5, treeClear = 4.5;
    double treeHeight = 11.0, treeHeightVar = 1.5, treeGround = 4.0;
    double oldTreeY[8] = {548.0, 578.0, 598.0, 618.0, 638.0};
    int oldTreeCount = 5;
    HeroSpan treeMouth[kFarClearMax] = {{525.0, 532.0}, {541.5, 545.5}, {555.0, 559.0}, {566.5, 574.5},
                                        {586.3, 590.3}, {621.5, 629.5}};
    int treeMouthCount = 6;
};

struct HeroFarBankLayout {
    FarFront fronts[kFarFrontsMax];
    int frontCount = 0;
    FarPiece pieces[kFarPiecesMax];
    int pieceCount = 0;
    FarBox boxes[kFarBoxesMax];
    int boxCount = 0;
    FarLantern lanterns[kFarLanternsMax];
    int lanternCount = 0;
    FarLamp lamps[kFarLampsMax];
    int lampCount = 0;
    QuayPost posts[kQuayPostsMax];
    int postCount = 0;
    RopeSpan ropes[kQuayPostsMax];
    int ropeCount = 0;
    FarBoat boats[kFarBoatsMax];
    int boatCount = 0;
    FarTree trees[kFarTreesMax];
    int treeCount = 0;
    bool overflow = false;  // something didn't fit its array (left out)
};

// The far bank's ground at y.
FTCORE_API double heroFarGround(const HeroFarBankSpec& spec, double y);

// The rope's height over the datum at t (0..1) of a span whose ends are at ground + ropeUp: less 4 sag t (1 - t).
FTCORE_API double heroRopeZ(const HeroFarBankSpec& spec, double t);

// Everything above, deterministic for a spec.
FTCORE_API void heroFarBankLayout(const HeroFarBankSpec& spec, HeroFarBankLayout& out);

} // namespace ftcore
