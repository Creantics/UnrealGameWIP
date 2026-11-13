#pragma once

// The water a body can stand, wade or swim in, and a camera can dip under: a world's canals, pools and its sea as a
// fixed set of bodies, each a footprint with a surface level, a bed, a flow and a kind.
//
//   ftcore::WaterBodies water;
//   for (const RoadWater& canal : build.waters) water.add(waterBodyFromCanal(canal));   // the road net's canal rows
//   water.add(waterSea(seaEdgeX, 0.f, -33.f, kWaterBuoyLine));                          // the sea past x = edge
//   const WaterState s = waterState(water, {{x, y}, feetZ, 1.8f, groundZ, true, wasSwimming}, t);
//   const WaterCamera c = water.camera({x, y}, eyeZ, t);
//
// The rules, in one place (WorldTerrain::kWadeDepth is this file's kWaterWadeDepth):
//   - The water column at a point is the surface minus the ground under it (the body's bed where no ground is given).
//   - A body whose feet are under the surface is in the water: it wades in a column up to kWaterWadeDepth (0.7 m,
//     the threshold itself still a wade), wades deep up to kWaterSwimDepth (1.2 m, itself still a deep wade), and swims
//     on the surface over more. A swimmer keeps swimming until the column is under kWaterSwimExit, so the mode doesn't
//     flicker on the line.
//   - Wading slows the walk with depth: 1 at the edge down to kWaterWadeFactor at 0.7 m, on down to kWaterDeepFactor
//     at 1.2 m. A swimmer moves at kWaterSwimSpeed and the flow carries it.
//   - Where two bodies overlap, the one with the higher surface is the water there; with equal surfaces the deeper,
//     then the first added.
//   - A sea is a half-plane. Past its buoy line (metres out from its edge) a current runs back toward the shore,
//     growing with the distance past the line until no swimmer makes way against it.
//
// One frame throughout (the caller's: plan or engine), metres and seconds. waterBodyToEngine takes a body from the plan
// frame to the engine's (y turned over). Nothing here allocates.

#include "ftcore/Api.h"
#include "ftcore/Explore.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

struct RoadWater; // ftcore/Roads.h
struct SeaWaves;  // ftcore/SeaWaves.h

inline constexpr float kWaterWadeDepth = 0.7f;   // m: a column up to this is a wade
inline constexpr float kWaterSwimDepth = 1.2f;   // m: a column over this is a swim
inline constexpr float kWaterSwimExit = 1.1f;    // m: a swimmer wades again under this
inline constexpr float kWaterWadeFactor = 0.55f; // the walk's speed share at kWaterWadeDepth
inline constexpr float kWaterDeepFactor = 0.35f; // ... at kWaterSwimDepth, the slowest
inline constexpr float kWaterSwimSpeed = 1.5f;   // m/s on the surface
inline constexpr float kWaterSwimDraft = 1.28f;  // m: a swimmer's feet hang this far under the surface
inline constexpr float kWaterBobHeight = 0.04f;  // m: the swimmer rides up and down by this
inline constexpr float kWaterBobPeriod = 2.6f;   // s
inline constexpr float kWaterBankSlack = 0.3f;   // a flow's share left at a channel's walls (1 mid-channel)
inline constexpr float kWaterBuoyLine = 60.f;    // m out from a sea's edge
inline constexpr float kWaterBuoyPull = 0.6f;    // m/s of current back for each metre past the line...
inline constexpr float kWaterBuoyPullMax = 4.f;  // ...up to this
inline constexpr std::size_t kWaterMaxBodies = 16;
inline constexpr std::size_t kWaterMaxPoints = 8;

enum class WaterKind : std::uint8_t { Canal, Pool, Lake, Sea };
enum class WaterShape : std::uint8_t {
    Box,       // centre, half sizes along its own axes, turned by (cosYaw, sinYaw)
    Polygon,   // points[0 .. pointCount), any winding, need not be convex
    HalfPlane, // every p with (p - centre) . (cosYaw, sinYaw) >= 0: a sea out from its edge
};
enum class WaterMode : std::uint8_t { Dry, Wade, DeepWade, Swim };

FTCORE_API const char* waterKindName(WaterKind kind);
FTCORE_API const char* waterModeName(WaterMode mode);

struct WaterBody {
    WaterKind kind = WaterKind::Canal;
    WaterShape shape = WaterShape::Box;
    bool waves = false;  // a sea whose surface rides the set's sea waves (WaterBodies::setWaves); else flat at level
    float level = 0.f;   // m: the surface at rest
    float bed = 0.f;     // m: the floor (a sea's: its deepest)
    Vec2 flow{};         // m/s mid-channel
    Vec2 centre{};       // a box's centre; a half-plane's point on its edge
    Vec2 half{};         // a box's half sizes
    float cosYaw = 1.f, sinYaw = 0.f; // a box's x axis; a half-plane's way out
    float buoyLine = 0.f; // m out from a half-plane's edge where the current back begins; 0: none
    std::uint8_t pointCount = 0;
    std::array<Vec2, kWaterMaxPoints> points{};

    // Whether p lies in the footprint (its edge included).
    FTCORE_API bool covers(Vec2 p) const;
    // How far p is out from a half-plane's edge (negative on the land side); 0 for the other shapes.
    FTCORE_API float out(Vec2 p) const;
};

// A canal's water (the road net's row) as a body in the plan frame.
FTCORE_API WaterBody waterBodyFromCanal(const RoadWater& canal);
// A sea over x >= edgeX (the plan frame's east), flat at `level`.
FTCORE_API WaterBody waterSea(float edgeX, float level, float bed, float buoyLine = kWaterBuoyLine);
// The body in the engine's frame for a square `side` metres across (massingEnginePoint: x - side/2, side/2 - y).
FTCORE_API WaterBody waterBodyToEngine(const WaterBody& planBody, float side);

struct WaterCapsule {
    Vec2 p{};
    float feetZ = 0.f;
    float height = 1.8f;
    float groundZ = 0.f;     // the ground under p, when hasGround
    bool hasGround = false;  // else the body's bed is the ground
    bool swimming = false;   // it swam the step before
};

struct WaterState {
    WaterMode mode = WaterMode::Dry;
    int body = -1;           // the body it is in, -1 dry
    float surface = 0.f;     // m: the surface over p
    float column = 0.f;      // m: surface minus ground
    float immersion = 0.f;   // m: surface minus feet, 0 or more
    float submerged = 0.f;   // 0..1 of the capsule's height under the surface
    float speedFactor = 1.f; // the walk's speed share (wading); kWaterDeepFactor swimming
    Vec2 flow{};             // m/s: the push on it there (the flow, and a sea's current back)
};

struct WaterCamera {
    bool under = false;
    int body = -1;
    float depth = 0.f;   // m under the surface (0 when not under)
    float surface = 0.f; // m: the surface over the eye
};

class WaterBodies {
public:
    // False (and nothing added) when the set is full or the body has no footprint.
    FTCORE_API bool add(const WaterBody& body);
    void clear() { count_ = 0; }
    std::size_t count() const { return count_; }
    const WaterBody& body(std::size_t i) const { return bodies_[i]; }
    // The waves a body with `waves` rides (not owned; null: flat). SeaWaves works in centimetres from this set's
    // origin; the heights come back in metres over the body's level.
    void setWaves(const SeaWaves* waves) { waves_ = waves; }

    // The body that is the water at p (the overlap rule above), -1 where none is.
    FTCORE_API int bodyAt(Vec2 p) const;
    // The surface over p at time t; false where no body is.
    FTCORE_API bool surfaceHeight(Vec2 p, float t, float& out) const;
    // Body i's surface over p at t (its level, or its waves').
    FTCORE_API float surfaceOf(std::size_t i, Vec2 p, float t) const;
    // The column over the body's own bed at p (at rest), 0 where no body is.
    FTCORE_API float depth(Vec2 p) const;
    // The push at p, m/s: the body's flow (slack toward a box's walls along the flow) and a sea's current back past
    // its buoy line. Zero where no body is.
    FTCORE_API Vec2 flowAt(Vec2 p) const;
    // Whether ground at groundZ under p is water's: a body covers p and stands over it by more than -margin.
    FTCORE_API bool overWater(Vec2 p, float groundZ, float margin = 0.f) const;
    // An eye at (p, z): under when a body covers p, z is below its surface plus `margin` and not under its bed. Of
    // several, the one it is deepest in.
    FTCORE_API WaterCamera camera(Vec2 p, float z, float t, float margin = 0.f) const;

private:
    std::size_t count_ = 0;
    std::array<WaterBody, kWaterMaxBodies> bodies_{};
    const SeaWaves* waves_ = nullptr;
};

// The walk's speed share in a column `depth` m deep (not swimming): 1 at 0, kWaterWadeFactor at kWaterWadeDepth,
// kWaterDeepFactor from kWaterSwimDepth on.
FTCORE_API float waterWadeFactor(float depth);
// The capsule's state at time t.
FTCORE_API WaterState waterState(const WaterBodies& water, const WaterCapsule& capsule, float t);
// Where a swimmer's feet ride at time t under a surface at `surface`: the draft, and a gentle bob.
FTCORE_API float waterSwimFeetZ(float surface, float t);

} // namespace ftcore
