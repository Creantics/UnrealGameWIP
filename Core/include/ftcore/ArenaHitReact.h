#pragma once

// A lead's body reaction to the blows Core stamped this tick (ArenaEvents::hits, ArenaLeadHit): tier, push direction,
// impulse strength, struck ragdoll part and hit-stop frames. Pure and allocation free: resolve an ArenaHitSetup once
// from the ragdoll data, then call per tick.
//
//   ftcore::ArenaHitSetup setup;
//   ftcore::arenaHitSetup(map, profile, setup);          // once
//   ftcore::ArenaHitReactions r;
//   ftcore::arenaHitReactions(fight.output().events, setup, r);  // per tick
//   for (const auto& e : r) host.react(e.target, e.tier, e.dir, e.strength, e.part, e.hitStopFrames);
//
// Strength is raw N*s: the host's profile scales (hitImpulseScale, koImpulseScale) still apply after it.

#include "ftcore/ArenaFight.h"
#include "ftcore/Api.h"
#include "ftcore/Physics.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

inline constexpr std::size_t kMaxArenaHitParts = 24; // ragdoll parts ArenaHitSetup keeps masses for

// The ragdoll facts the reaction needs, read once (no per-tick lookups by name).
struct ArenaHitSetup {
    int chest = -1, spine = -1, head = -1, pelvis = -1; // part indices in the RagdollMap, -1 when absent
    int shinL = -1, shinR = -1, thighL = -1, thighR = -1;
    int root = 0;                              // part 0: the map lists parents first
    std::array<float, kMaxArenaHitParts> partMass{}; // kg, by part index
    std::size_t partCount = 0;
    float upperMass = 0.f;                     // kg: the parts marked upper
    float totalMass = 0.f;                     // kg
    float maxLinearVelocity = 7.5f;            // m/s (RagdollProfile)
    float koMaxLocalDeltaV = 3.f;              // m/s (RagdollProfile)
    std::array<int, 4> tierFrames{0, 0, 4, 8}; // hit-stop frames by ArenaHitTier (None, Light, Stagger, KO), from
                                               // the profile's hitStop*Frames
    float hitStopFps = 60.f;                   // frames per s for a move's own hitStop (RagdollProfile::hitStopFps)
    float minShare = 0.25f;                    // least share of a tier's cap a blow gives
};

// Reads the map's part masses and named parts, and the profile's speeds and per-tier hit-stop frames. False (setup left at defaults) when the map
// has no parts or more than kMaxArenaHitParts.
FTCORE_API bool arenaHitSetup(const RagdollMap& map, const RagdollProfile& profile, ArenaHitSetup& out);

struct ArenaHitReaction {
    ArenaHitTier tier = ArenaHitTier::None;
    std::uint8_t target = kArenaNobody;  // kArenaPlayer or kArenaAlly
    std::uint8_t source = kArenaNobody;  // the strongest blow's attacker
    Vec2 dir{1.f, 0.f};                  // unit, attacker -> target on the ground plane
    float strength = 0.f;                // N*s, raw
    int part = -1;                       // struck RagdollMap part
    int hitStopFrames = 0;               // display only (the host's HitStop): the move's own hitStop x fps when it
                                         // is set (at least 1), else the tier's frames from the profile. Never fed
                                         // back to Core. A set hitStop is also the tick's own pause (ArenaAttack::
                                         // hitStop) when the blow lands on the player, so the two agree there;
                                         // other blows and the tier frames pause the host's display alone.
    std::uint8_t blows = 0;              // blows merged into this one
    bool tookOver = false;               // a TookOver tick: target names the body before the swap. The host applies
                                         // the player's reaction to the fallen body (in the ally's place after the
                                         // swap) and the ally's to the body that now plays.
};

// One blow's reaction.
FTCORE_API ArenaHitReaction arenaHitReaction(const ArenaLeadHit& hit, const ArenaHitSetup& setup);

// The tick's reactions, one per struck body (player first, then ally), several blows on one body merged:
// the highest tier; the weights summed, then capped by that tier; dir the weighted sum's (the strongest blow's when it
// cancels); part and source the strongest blow's (tier, then weight, then earliest); hit-stop the longest.
struct ArenaHitReactions {
    std::array<ArenaHitReaction, 2> items{};
    std::size_t count = 0;

    const ArenaHitReaction* begin() const { return items.data(); }
    const ArenaHitReaction* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const ArenaHitReaction& operator[](std::size_t i) const { return items[i]; }
};
FTCORE_API void arenaHitReactions(const ArenaEvents& events, const ArenaHitSetup& setup, ArenaHitReactions& out);

// The tier stamped on events[eventIndex], or None when no record names it (Evaded, Parried, Blocked, a chip, ...).
FTCORE_API ArenaHitTier arenaHitTierOf(const ArenaEvents& events, std::size_t eventIndex);

// The cap on a tier's strength (N*s): Light the struck part's mass x koMaxLocalDeltaV, Stagger the upper body's,
// Knockout the whole body's mass x maxLinearVelocity. None: 0.
FTCORE_API float arenaHitCap(ArenaHitTier tier, int part, const ArenaHitSetup& setup);

// ---- the leg the teeth are at (a bite on the legs) ----
// Core picks a leg part by which side of the struck body's facing the attacker stands (hitPart). Dead ahead that sign
// flips on a fraction of a degree, so the host, which has both bodies' bones, re-picks the side in world space.
// A leg's chain (ArenaPoint3 from ArenaFight.h, in world units, any one unit): hip (thigh), knee (calf), ankle
// (foot).
using ArenaLegChain = std::array<ArenaPoint3, 3>;

// Which leg the attacker's teeth are at: the snout ray (from `snout`, along `forward` on the ground) carried to the
// legs' depth (the mean of both chains along `forward`), and the leg whose chain (hip-knee, knee-ankle segments) lies
// nearer that point. 0 the left, 1 the right; -1 when the two distances are within `tie` (same unit), `forward` has
// no length or a place is not finite, so the caller keeps its own pick. Pure, no allocation.
FTCORE_API int arenaLegAtTeeth(ArenaPoint3 snout, Vec2 forward, const ArenaLegChain& left, const ArenaLegChain& right,
                               float tie);
// The snout ray's point at the legs' depth (as arenaLegAtTeeth carries it) and its distance to each chain, for logs.
struct ArenaLegReach {
    ArenaPoint3 contact;
    float left = 0.f, right = 0.f;
    bool valid = false; // forward had a length and both distances are finite
};
FTCORE_API ArenaLegReach arenaLegReach(ArenaPoint3 snout, Vec2 forward, const ArenaLegChain& left,
                                       const ArenaLegChain& right);

// `part` moved to `side` (0 left, 1 right) when it is one of the setup's thigh or shin parts and that side has the same
// part; anything else (another part, side -1, a missing part) comes back unchanged.
FTCORE_API int arenaLegPartOnSide(int part, int side, const ArenaHitSetup& setup);

} // namespace ftcore
