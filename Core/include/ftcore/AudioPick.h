#pragma once

// Variant picker for one sound cue (docs/AUDIO_MIX.md): which of a cue's N recorded variants plays
// next, plus a small pitch and volume offset, so a footstep, impact or menu sound never plays the
// same way twice in a row. Header-only, no allocation, deterministic.
//
//   ftcore::AudioPickRule rule;          // from the cue's data: variants, avoidLast, pitchCents, ...
//   ftcore::AudioPickState state;        // one per cue, owned by the caller
//   ftcore::audioPickSeed(state, worldSeed, "SFX-STEP");
//   const ftcore::AudioPickResult r = ftcore::audioPick(rule, state);
//   // play variant r.variant at r.pitchRatio() and r.volumeDb
//
// Rules: never the same variant twice in a row when N >= 2; never one of the last two when N >= 4
// (avoidLast can raise that, up to 4, never lower it); shuffle-bag order, so over every N plays
// each variant is used once; or round robin (k, k+1, ... wrapping, from a seeded start k) when
// roundRobin is set.
// Pitch jitter is uniform in +-pitchCents cents, volume jitter uniform in +-volumeDb dB; both 0 by
// default. Each cue's state carries its own generator, so a cue's sequence depends only on its seed
// and on its own plays, never on other cues. Every play draws the same count of random numbers,
// so changing the jitter ranges never changes the variant order.

#include "ftcore/AudioCues.h" // AudioCue, for audioPickRuleFor

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr int kAudioPickMaxVariants = 16;
inline constexpr int kAudioPickMaxAvoid = 4;

struct AudioPickRule {
    int variants = 1;          // 1..16; values outside are clamped (0 counts as 1)
    int avoidLast = -1;        // -1: automatic (1 when N >= 2, 2 when N >= 4); higher values up to 4 avoid more
    bool roundRobin = false;   // true: play the variants in order instead of shuffled
    float pitchCents = 0.f;    // +- range, cents (100 = one semitone)
    float volumeDb = 0.f;      // +- range, decibels
};

struct AudioPickResult {
    int variant = 0;           // 0..N-1
    float pitchCents = 0.f;    // offset to apply
    float volumeDb = 0.f;      // offset to apply, on top of the cue's gainDb
    float pitchRatio() const { return std::exp2(pitchCents / 1200.f); }
    float volumeLinear() const { return std::pow(10.f, volumeDb / 20.f); }
};

// About 32 bytes. Zero-initialised it is valid (seed 0); call audioPickSeed for a real seed.
struct AudioPickState {
    std::uint32_t rng = 0x9E3779B9u;              // xorshift32, never 0
    std::uint8_t bag[kAudioPickMaxVariants] = {}; // this round's shuffled order
    std::uint8_t bagSize = 0;                     // N the bag was filled for (0: not yet)
    std::uint8_t bagPos = 0;                      // next slot in bag
    std::uint8_t recent[kAudioPickMaxAvoid] = {}; // recent[0] is the last variant played
    std::uint8_t recentCount = 0;
};

namespace audio_pick_detail {

inline std::uint32_t next(std::uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// Uniform in [0, 1).
inline float unit(std::uint32_t& s) { return static_cast<float>(next(s) >> 8) * (1.f / 16777216.f); }

inline std::uint32_t below(std::uint32_t& s, std::uint32_t n) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(next(s)) * n) >> 32);
}

inline std::uint32_t mix(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

inline bool isRecent(const AudioPickState& st, std::uint8_t v, int avoid) {
    for (int i = 0; i < avoid && i < st.recentCount; ++i)
        if (st.recent[i] == v) return true;
    return false;
}

inline void remember(AudioPickState& st, std::uint8_t v) {
    for (int i = kAudioPickMaxAvoid - 1; i > 0; --i) st.recent[i] = st.recent[i - 1];
    st.recent[0] = v;
    if (st.recentCount < kAudioPickMaxAvoid) ++st.recentCount;
}

inline float jitter(std::uint32_t& s, float range) {
    const float u = unit(s); // always drawn, so a range of 0 still moves the generator
    if (!(range > 0.f) || !std::isfinite(range)) return 0.f;
    return (u * 2.f - 1.f) * range;
}

} // namespace audio_pick_detail

// The rule a cue's data gives (cues.json keys variants, avoidLast, roundRobin, pitchCents, volumeDb).
inline AudioPickRule audioPickRuleFor(const AudioCue& cue) {
    AudioPickRule r;
    r.variants = cue.variants;
    r.avoidLast = cue.avoidLast;
    r.roundRobin = cue.roundRobin;
    r.pitchCents = cue.pitchCents;
    r.volumeDb = cue.volumeDb;
    return r;
}

// Clamped variant count, 1..16.
inline int audioPickVariants(const AudioPickRule& rule) {
    return rule.variants < 1 ? 1 : (rule.variants > kAudioPickMaxVariants ? kAudioPickMaxVariants : rule.variants);
}

// How many of the last plays are avoided: max(automatic, avoidLast), at most N - 1 and at most 4.
inline int audioPickAvoid(const AudioPickRule& rule) {
    const int n = audioPickVariants(rule);
    int avoid = n >= 4 ? 2 : (n >= 2 ? 1 : 0);
    if (rule.avoidLast > avoid) avoid = rule.avoidLast;
    if (avoid > kAudioPickMaxAvoid) avoid = kAudioPickMaxAvoid;
    if (avoid > n - 1) avoid = n - 1;
    return avoid;
}

// FNV-1a of a cue id; no allocation. A 32-bit hash: two ids that collide would share a sequence
// when seeded by id alone, so seed with the cue's table index too where there is one.
inline std::uint32_t audioCueKey(std::string_view cueId) {
    std::uint32_t h = 2166136261u;
    for (const char c : cueId) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 16777619u;
    }
    return h;
}

// Resets the state and seeds its generator from a game-wide seed and the cue's key, so two cues
// with one seed still get unrelated sequences. For tests and tools: a game should call the
// four-argument form below (id plus table index), which can't be fooled by a key collision.
inline void audioPickSeed(AudioPickState& st, std::uint32_t seed, std::uint32_t cueKey) {
    st = AudioPickState{};
    std::uint32_t s = audio_pick_detail::mix(seed ^ audio_pick_detail::mix(cueKey + 0x9E3779B9u));
    st.rng = s ? s : 0x9E3779B9u;
}

// By id alone; as above, a game should call the four-argument form.
inline void audioPickSeed(AudioPickState& st, std::uint32_t seed, std::string_view cueId) {
    audioPickSeed(st, seed, audioCueKey(cueId));
}

// Every pair of cues in `cues` whose ids give the same key under `keyOf` (audioCueKey by default),
// as one error line per pair naming both ids. importAudioCues refuses a table with any. Load time
// only: allocates for the messages.
using AudioCueKeyFn = std::uint32_t (*)(std::string_view);
inline void audioCueKeyCollisions(const std::vector<AudioCue>& cues, std::vector<std::string>& errors,
                                  AudioCueKeyFn keyOf = &audioCueKey) {
    for (std::size_t i = 0; i < cues.size(); ++i) {
        const std::uint32_t ki = keyOf(cues[i].id);
        for (std::size_t j = i + 1; j < cues.size(); ++j) {
            // Equal or empty ids are reported by the importer as duplicate or missing ids instead.
            if (cues[i].id.empty() || cues[j].id == cues[i].id || keyOf(cues[j].id) != ki) continue;
            char hex[16];
            std::snprintf(hex, sizeof hex, "%08X", static_cast<unsigned>(ki));
            errors.push_back("cues[" + std::to_string(j) + "].id: " + cues[j].id + " and " + cues[i].id +
                             " (cues[" + std::to_string(i) + "]) share the cue key 0x" + hex + "; rename one");
        }
    }
}

// Seeds from the cue id and its index in the cue table (AudioCueTable::cues). Two cues whose ids
// hash alike still start from different generator states, since the index is folded in through
// bijections (a multiply by an odd constant, then an invertible mix).
inline void audioPickSeed(AudioPickState& st, std::uint32_t seed, std::string_view cueId, std::uint32_t tableIndex) {
    namespace d = audio_pick_detail;
    st = AudioPickState{};
    const std::uint32_t base = d::mix(seed ^ d::mix(audioCueKey(cueId) + 0x9E3779B9u));
    const std::uint32_t s = d::mix(base + tableIndex * 0x9E3779B9u);
    st.rng = s ? s : 0x9E3779B9u;
}

// The next play of the cue. No allocation.
inline AudioPickResult audioPick(const AudioPickRule& rule, AudioPickState& st) {
    namespace d = audio_pick_detail;
    if (st.rng == 0) st.rng = 0x9E3779B9u;
    const int n = audioPickVariants(rule);
    const int avoid = audioPickAvoid(rule);

    // The data changed under a live state (a reload with another variant count): forget old indices.
    if (st.bagSize != n) {
        int kept = 0;
        for (int i = 0; i < st.recentCount; ++i)
            if (st.recent[i] < n) st.recent[kept++] = st.recent[i];
        st.recentCount = static_cast<std::uint8_t>(kept);
        st.bagSize = static_cast<std::uint8_t>(n);
        st.bagPos = static_cast<std::uint8_t>(n); // refill now
    }

    std::uint8_t v = 0;
    if (n == 1) {
        v = 0;
    } else if (rule.roundRobin) {
        // The first play starts at a seeded offset (read from the generator without advancing it),
        // so two round-robin cues don't run in lockstep.
        v = st.recentCount ? static_cast<std::uint8_t>((st.recent[0] + 1) % n)
                           : static_cast<std::uint8_t>(d::mix(st.rng) % static_cast<std::uint32_t>(n));
    } else {
        if (st.bagPos >= n) {
            for (int i = 0; i < n; ++i) st.bag[i] = static_cast<std::uint8_t>(i);
            for (int i = n - 1; i > 0; --i) {
                const auto j = static_cast<int>(d::below(st.rng, static_cast<std::uint32_t>(i + 1)));
                const std::uint8_t t = st.bag[i];
                st.bag[i] = st.bag[j];
                st.bag[j] = t;
            }
            st.bagPos = 0;
        }
        // The first slot left in this round that isn't a recent play. One always exists: at slot p
        // at most avoid - p of the recent plays come from the last round, and N - p slots are left.
        int pick = st.bagPos;
        for (int i = st.bagPos; i < n; ++i) {
            if (!d::isRecent(st, st.bag[i], avoid)) {
                pick = i;
                break;
            }
        }
        const std::uint8_t t = st.bag[st.bagPos];
        st.bag[st.bagPos] = st.bag[pick];
        st.bag[pick] = t;
        v = st.bag[st.bagPos++];
    }
    d::remember(st, v);

    AudioPickResult r;
    r.variant = v;
    r.pitchCents = d::jitter(st.rng, rule.pitchCents);
    r.volumeDb = d::jitter(st.rng, rule.volumeDb);
    return r;
}

} // namespace ftcore
