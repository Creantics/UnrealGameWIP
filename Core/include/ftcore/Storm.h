#pragma once

// The storm (playtest item 4): the overcast the rain brings, and seeded lightning with its thunder.
//
// Sky. stormSky(spec, rainIntensity) gives the cloud cover (0 clear .. 1 full overcast, smoothstep from overcastFrom to
// overcastFull of the rain), the day sun's and sky light's scales under it, how much of the stars and the moon show, and
// the wind. The host multiplies sunScale and skyScale into the DAY sun and sky only, after the night look's fit: the
// night's moon share, lift and floor (NightLook.h, item 5's floor) are never changed by the cover; a cloudy night reads
// by its hidden stars and moon, not by a darker street.
//
// Lightning. Strikes are scheduled in storm time (the clock advances only while not held), each next gap drawn when a
// strike fires, so a seed gives the same strikes at any frame rate. Heads come at an exponential gap whose mean runs from
// meanGapLightS (rain at strikeFrom) to meanGapHeavyS (rain at 1), never under headGapMinS; a head starts a cluster of up
// to clusterMax strikes clusterGapMinS..clusterGapMaxS apart. No strike while the rain is under strikeFrom. Each strike
// has a distance (uniform over the ring's area), a bearing and a brightness; its thunder comes distance / 343 s later in
// one of three tiers (Near under nearM: a crack; Mid under midM: a roll; Far: a rumble).
//
// Hold (a special move's cinematic): stormStep with held true stops the storm clock. No strike, no flash and no thunder
// while held; strikes and thunder already scheduled keep their storm times and play out after the release, in order,
// with no burst (the clock only resumes).
//
// Flashes. stormFlash gives the flash's added light 0..1 now: a 30 ms rise and a flashS fall per strike. With reduce
// (the "Reduce flashes" setting) only a cluster's head shows, as one slow swell (reducedRiseS, reducedFallS) at
// reducedScale. The spacing rules keep any 1 s at 3 flashes or fewer (1 with reduce). The host adds the flash after the
// night lift and keeps it out of exposure metering.
//
// Plain C++20, no allocation; all state is in StormState.

#include "ftcore/Api.h"

#include <cstdint>

namespace ftcore {

struct StormSpec {
    // Sky.
    float overcastFrom = 0.15f;  // rain intensity where the cloud starts to close
    float overcastFull = 0.6f;   // rain intensity from which the sky is fully overcast
    float sunAtOvercast = 0.2f;  // the day sun's share under full overcast
    float skyAtOvercast = 0.6f;  // the day sky light's share under full overcast
    float starsGoneAt = 0.35f;   // cover from which no star shows (all show at 0)
    float moonGoneAt = 0.75f;    // cover from which the moon's disc is hidden
    float windCalmMps = 2.f;     // wind at cover 0
    float windStormMps = 14.f;   // wind at cover 1
    // Lightning.
    float strikeFrom = 0.5f;     // rain intensity from which lightning strikes
    float meanGapLightS = 40.f;  // mean head gap at strikeFrom
    float meanGapHeavyS = 12.f;  // mean head gap at rain 1
    float headGapMinS = 2.f;     // no head sooner than this after the last strike
    float clusterChance = 0.45f; // chance a head starts a cluster
    int clusterMax = 3;          // strikes in a cluster at most, its head included (1..3)
    float clusterGapMinS = 0.35f;
    float clusterGapMaxS = 1.6f;
    float minDistM = 400.f;
    float maxDistM = 8000.f;
    float nearM = 1000.f;        // under this: Near thunder
    float midM = 3000.f;         // under this: Mid thunder; Far beyond
    float flashS = 0.25f;        // a flash's fall
    float reducedRiseS = 0.4f;
    float reducedFallS = 0.9f;
    float reducedScale = 0.4f;
};

struct StormSky {
    float cover = 0.f;     // 0 clear .. 1 overcast
    float sunScale = 1.f;  // day sun multiplier
    float skyScale = 1.f;  // day sky light multiplier
    float starAlpha = 1.f; // 0..1 of the stars shown
    float moonAlpha = 1.f; // 0..1 of the moon's disc shown
    float windMps = 0.f;
};

FTCORE_API StormSky stormSky(const StormSpec& spec, float rainIntensity);

enum class ThunderTier : std::uint8_t { Near, Mid, Far };

// The speed of sound the thunder's delay uses (m/s).
inline constexpr float kThunderMps = 343.f;
// The thunder a distance gets.
FTCORE_API ThunderTier thunderTier(const StormSpec& spec, float distanceM);

struct StormStrike {
    double atS = 0.0;        // storm time of the flash
    double thunderAtS = 0.0; // storm time the thunder arrives: atS + distanceM / kThunderMps
    float distanceM = 0.f;
    float bearingDeg = 0.f;  // 0..360, the host's own north
    float brightness = 0.f;  // 0..1 of a full flash
    ThunderTier tier = ThunderTier::Far;
    bool head = true;        // a cluster's first strike (the one a reduced flash shows)
};

inline constexpr int kStormMaxThunder = 12;     // thunder in flight at once (23 s for the farthest); past it the farthest is dropped
inline constexpr int kStormMaxStepEvents = 4;   // strikes or thunders one step reports; the rest wait a step
inline constexpr float kStormMaxStepS = 0.5f;   // a longer dt is clamped to it

struct StormState {
    std::uint32_t rng = 0x9E3779B9u; // xorshift32, never 0
    double clockS = 0.0;             // storm time
    double nextS = -1.0;             // the next strike's storm time; < 0: none drawn yet
    int clusterLeft = 0;             // strikes left in the cluster after the next
    float clusterDistM = 0.f;        // the cluster's distance (its members stay near it)
    float clusterBearingDeg = 0.f;
    double lastFlashS = -1.0e9;      // the last strike's storm time and brightness
    float lastFlash = 0.f;
    double lastHeadS = -1.0e9;       // the last head's, for the reduced swell
    float lastHead = 0.f;
    StormStrike thunder[kStormMaxThunder]; // thunder in flight, unordered
    int thunderCount = 0;
    int dropped = 0;                 // thunder lost to a full queue since the start
};

struct StormEvents {
    StormStrike strikes[kStormMaxStepEvents]; // flashes this step, in time order
    int strikeCount = 0;
    StormStrike thunders[kStormMaxStepEvents]; // thunder arriving this step, in time order
    int thunderCount = 0;
};

// A storm seeded with `seed` (0 is moved off zero), its clock at 0.
FTCORE_API StormState stormStart(std::uint32_t seed);
// One step of dtS seconds (clamped to kStormMaxStepS; 0 or less or not finite: nothing moves) at this rain intensity.
// held: a cinematic holds the storm (see the header). The step's flashes and arriving thunder go to `out`.
FTCORE_API void stormStep(const StormSpec& spec, StormState& state, float dtS, float rainIntensity, bool held, StormEvents& out);
// The flash's added light now, 0..1 (0 while held). reduce: the "Reduce flashes" setting.
FTCORE_API float stormFlash(const StormSpec& spec, const StormState& state, bool reduce, bool held = false);

} // namespace ftcore
