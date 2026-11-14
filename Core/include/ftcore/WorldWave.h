#pragma once

// The world wave: travel between worlds as one front that starts at a point and spreads through the place, so every
// object changes where it stands and the camera never cuts.
//
// A wave starts at startTime with a prelude (a thin streak rising at the origin) of `prelude` seconds. Then a spherical
// front grows from the origin. With tau the time since the prelude ended, its radius is
//   tau <= 0          r = 0
//   r < fastRadius    r = fastSpeed * tau                          (fastRadius is reached at tau1 = fastRadius / fastSpeed)
//   after tau1        r = fastRadius + slowSpeed * u + (fastSpeed - slowSpeed) * easeTime * (1 - exp(-u / easeTime)),
//                     u = tau - tau1
// so the speed eases from fastSpeed to slowSpeed with no jump at the join, and the radius stops at maxRadius (settled).
// The front is a band, bandAtOrigin wide at r = 0, widening linearly to bandAtFar at bandFarRadius and held there.
//
// classifyWorldWave says which side of the front a point is on: beyond the front (d > r) the world the wave leaves
// (Old); behind the band's inner edge (d < r - w) the world it brings (New); between them the Band, with a value running
// 0 at the outer edge to 1 at the inner edge (in either direction; the value is geometric). Reverse swaps the sides:
// the sphere holds the Old world and New lies beyond the front. The ink material draws the New side, so a Reverse wave
// run over a settled Forward one opens the first world out again from its point.
//
// updateWorldWave steps a small state with the time and the viewer (camera and listener, one point) and returns the
// frame and the cues that fired, each once and in the order Prelude, FrontPassedListener, Settled:
//   Prelude              the first step at or after startTime
//   FrontPassedListener  the outer edge has reached the viewer (r > d)
//   Settled              the radius has reached maxRadius
// cameraInside is geometric (inside the sphere, whatever the direction): 0 until the band's inner edge passes the
// viewer, then a linear ramp to 1 over insideRamp seconds from the moment it passed (found between the two steps that
// straddle it, so a fixed-step capture and a live run agree). Once passed it stays passed. A host that switches the
// sky and fog for the New world uses cameraInside for Forward and 1 - cameraInside for Reverse.
//
// The two worlds need not match. Every placed thing carries a WorldTag: Both (the same thing in either world; never cut,
// it only takes the new world's look), OldOnly (only in the first world) or NewOnly (only in the second). "Old" and
// "New" name the worlds (the first, the second), not the travel, so a Reverse wave brings the OldOnly things back in
// and cuts the NewOnly ones away. worldTagState says, for a tag at a classified point, whether the thing is drawn there,
// how far the cut has gone (cutValue: 0 whole, 1 gone; it runs across the band) and whether its collision is on.
//
// updateWorldWaveThing tracks one tagged thing (a pivot and a footprint radius) through a wave. Its collision follows
// the tag collisionDelay seconds after the front's outer edge passes the pivot. A thing the wave brings in (drawn on the
// inner side, not on the outer) never pushes a pawn: when the front reaches the thing's near side with a lead or the
// player inside its footprint, the thing is held (not drawn, no collision) until the footprint is clear, then cuts in
// on its own front, which sweeps from the near side to the far side plus a band at slowSpeed; its collision comes on
// collisionDelay after that front passes the pivot. Collision that would come on while a pawn is in the footprint waits
// until the pawn has left it.
//
// Units: metres and seconds. Plain C++20, deterministic, no allocation.

#include "ftcore/Api.h"

#include <cstdint>

namespace ftcore {

struct WaveVec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

enum class WaveDirection : std::uint8_t {
    Forward,  // the world the wave brings is inside the sphere
    Reverse,  // it is outside: the sphere holds the world the wave leaves
};

struct WorldWaveParams {
    WaveVec3 origin;                              // metres
    double startTime = 0.0;                       // seconds, on the host's wave clock
    float prelude = 0.3f;                         // the streak's time before the front moves
    float fastSpeed = 40.f;                       // m/s up to fastRadius ...
    float fastRadius = 30.f;
    float slowSpeed = 25.f;                       // ... easing to this beyond
    float easeTime = 0.5f;                        // the ease's time constant (k)
    float bandAtOrigin = 1.5f;                    // the band's width at r = 0 ...
    float bandAtFar = 4.f;                        // ... widening linearly to this ...
    float bandFarRadius = 60.f;                   // ... at this radius, then held
    float maxRadius = 400.f;
    float insideRamp = 0.25f;                     // cameraInside's ramp once the inner edge has passed the viewer
    float collisionDelay = 0.2f;                  // a tagged thing's collision follows its tag this long after the front
    WaveDirection direction = WaveDirection::Forward;
};

struct WorldWaveFrame {
    float radius = 0.f;           // the outer edge, metres
    float bandWidth = 0.f;        // metres; the inner edge is radius - bandWidth
    float preludeProgress = 0.f;  // 0 at startTime to 1 when the front starts to move
    float cameraInside = 0.f;     // 0..1 (updateWorldWave only; evaluateWorldWave leaves it 0)
    WaveDirection direction = WaveDirection::Forward;
    bool active = false;          // started and not yet settled
    bool settled = false;         // the radius has reached maxRadius
};

enum class WaveSide : std::uint8_t { Old, Band, New };

struct WaveClass {
    WaveSide side = WaveSide::Old;
    float band = 0.f;  // geometric: (r - d) / w in the band, 0 beyond the front, 1 inside the inner edge
};

// The cues, as bits of the mask updateWorldWave returns.
enum class WorldWaveCue : std::uint8_t {
    Prelude = 1u << 0,
    FrontPassedListener = 1u << 1,
    Settled = 1u << 2,
};
inline constexpr int kWorldWaveCueCount = 3;

constexpr std::uint8_t worldWaveCueBit(WorldWaveCue cue) { return static_cast<std::uint8_t>(cue); }
constexpr bool hasWorldWaveCue(std::uint8_t mask, WorldWaveCue cue) { return (mask & worldWaveCueBit(cue)) != 0; }
// "Prelude", "FrontPassedListener", "Settled" (a log's name for the cue).
FTCORE_API const char* worldWaveCueName(WorldWaveCue cue);

struct WorldWaveState {
    bool preludeFired = false;
    bool frontFired = false;
    bool settledFired = false;
    bool crossed = false;      // the inner edge has passed the viewer
    bool hasLast = false;
    double crossTime = 0.0;
    double lastTime = 0.0;
    float lastMargin = 0.f;    // (r - w) - d at the last step: the inner edge's lead on the viewer
};

struct WorldWaveStep {
    WorldWaveFrame frame;
    std::uint8_t cues = 0;  // WorldWaveCue bits fired by this step
};

// The radius at tau seconds after the prelude, clamped to [0, maxRadius].
FTCORE_API float worldWaveRadiusAt(const WorldWaveParams& params, float tau);
// The band's width at a radius.
FTCORE_API float worldWaveBandWidth(const WorldWaveParams& params, float radius);
// The frame at time t (on the host's wave clock), without the viewer: cameraInside is 0.
FTCORE_API WorldWaveFrame evaluateWorldWave(const WorldWaveParams& params, double time);
// One step: the frame at time t with the viewer's cameraInside, and the cues that fired. Steps go forward in time; a
// new wave starts from a fresh state (WorldWaveState{}).
FTCORE_API WorldWaveStep updateWorldWave(WorldWaveState& state, const WorldWaveParams& params, double time,
                                         const WaveVec3& viewer);
// Which side of the front a point is on (see the note above).
FTCORE_API WaveClass classifyWorldWave(const WorldWaveFrame& frame, const WaveVec3& origin, const WaveVec3& point);

// Which world a placed thing belongs to. The values are the ones the materials read (see the host's slot constants).
enum class WorldTag : std::uint8_t {
    Both = 0,     // in both worlds: never cut, collision always on
    OldOnly = 1,  // only in the first world
    NewOnly = 2,  // only in the second world
};

struct WorldTagState {
    bool drawn = true;
    float cutValue = 0.f;  // 0 whole .. 1 cut away; between them only in the band
    bool collisionOn = true;
};

// A tagged thing at a classified point. In the band the cut runs with the share of the band the new world has taken
// (the band value for Forward, 1 - band for Reverse). OldOnly keeps collision until it is on the New side; NewOnly has
// collision only there. (A host flips collision with the delay updateWorldWaveThing adds; this is the instant rule.)
FTCORE_API WorldTagState worldTagState(WorldTag tag, const WaveClass& where, WaveDirection direction);

// One tagged thing's tracked state through a wave. Set pivot, radius and tag; leave the rest as default when the wave
// starts.
struct WorldWaveThing {
    WaveVec3 pivot;                 // metres
    float radius = 0.f;             // the footprint's bounding radius, metres
    WorldTag tag = WorldTag::Both;
    bool collisionKnown = false;    // the first step reports the collision as changed
    bool collisionOn = false;
    bool arrived = false;           // the outer edge has reached the near side (d - radius)
    bool passed = false;            // the outer edge has passed the pivot
    double passTime = 0.0;
    bool held = false;
    bool released = false;          // was held; now cuts in on its own front
    double releaseTime = 0.0;
    bool localPassed = false;
    double localPassTime = 0.0;
    bool localDone = false;
};

struct WorldWaveThingStep {
    // The host shows the thing at all (its shadows, distance field and bounce light too, which a material's clip does
    // not reach): false while held, before the front reaches a thing the wave brings in, and once a thing the wave takes
    // away is wholly behind the band's inner edge (r - w > d + radius). Between those the material's cut draws it.
    bool drawn = true;
    bool localFront = false;        // true: cut on localRadius, not the wave's radius
    float localRadius = 0.f;        // metres from the wave's origin, when localFront
    bool collisionOn = false;
    bool collisionChanged = false;  // the host flips the thing's collision this step
    bool held = false;
};

// One step for a tracked thing, after updateWorldWave for the same time. pawnInFootprint: a lead or the player overlaps
// the thing's footprint now.
FTCORE_API WorldWaveThingStep updateWorldWaveThing(WorldWaveThing& thing, const WorldWaveParams& params,
                                                   const WorldWaveFrame& frame, double time, bool pawnInFootprint);

} // namespace ftcore
