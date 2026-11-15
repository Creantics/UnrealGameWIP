#pragma once

// Game-wide mix states (docs/AUDIO_MIX.md): which mix the game is in (Explore, Combat, Dialogue,
// Cutscene, Menu), the gain each bus should sit at in that state, a ramp toward it with separate
// attack (falling) and release (rising) times, and a voice cap per state with the helper that
// picks which playing sounds to stop. Header-only, no allocation, deterministic.
//
//   ftcore::MixState mix;                                      // built-in defaults
//   const ftcore::MixGains& g = mix.step(ftcore::mixRequest(ftcore::MixMode::Dialogue), dt);
//   // set each bus's volume to g.linear[bus]
//
// Several states can be requested at once (a bit mask); the highest-priority one wins:
// Menu > Cutscene > Dialogue > Combat > Explore, and no request at all means Explore.
// Ramp law: linear in dB. When a bus's target changes, the bus moves from where it is to the new
// target in exactly attackSeconds (when it falls) or releaseSeconds (when it rises), whatever the
// distance, and stops on the target (no overshoot). The result is the same at any step rate as long
// as the requests change at the same moments.

#include "ftcore/AudioCues.h" // AudioBus, for mixBusFor

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ftcore {

enum class MixMode : std::uint8_t { Explore, Combat, Dialogue, Cutscene, Menu, Count };
enum class MixBus : std::uint8_t { Music, Ambience, Sfx, Footsteps, Voice, Ui, Count };

inline constexpr int kMixModes = static_cast<int>(MixMode::Count);
inline constexpr int kMixBuses = static_cast<int>(MixBus::Count);
inline constexpr int kMixMaxVoices = 32;     // the most voices a cap can keep
inline constexpr float kMixFloorDb = -120.f; // a bus at or below this is silent (linear gain exactly 0)
inline constexpr float kMixCeilingDb = 24.f; // table values above this count as this

using MixRequests = std::uint8_t; // bit (1 << mode) per requested state

inline constexpr MixRequests mixRequest(MixMode m) { return static_cast<MixRequests>(1u << static_cast<unsigned>(m)); }

inline constexpr const char* kMixModeNames[kMixModes] = {"explore", "combat", "dialogue", "cutscene", "menu"};
inline constexpr const char* kMixBusNames[kMixBuses] = {"music", "ambience", "sfx", "footsteps", "voice", "ui"};

// The highest-priority requested state; Explore when none is.
inline constexpr MixMode activeMixMode(MixRequests requests) {
    for (int m = kMixModes - 1; m > 0; --m)
        if (requests & (1u << m)) return static_cast<MixMode>(m);
    return MixMode::Explore;
}

// The cue manifest's four buses on the mix buses. Footsteps and Ui have no cue bus yet: the caller
// routes those sounds itself (docs/AUDIO_MIX.md).
inline constexpr MixBus mixBusFor(AudioBus bus) {
    switch (bus) {
    case AudioBus::Music: return MixBus::Music;
    case AudioBus::Voice: return MixBus::Voice;
    case AudioBus::Ambience: return MixBus::Ambience;
    default: return MixBus::Sfx;
    }
}

// The mix bus a cue plays on: its mixBus key when given, else mixBusFor(bus).
inline MixBus mixBusForCue(const AudioCue& cue) {
    return cue.mixBus >= 0 && cue.mixBus < kMixBuses ? static_cast<MixBus>(cue.mixBus) : mixBusFor(cue.bus);
}

struct MixTable {
    // Target gain in dB per state and bus, indexed [MixMode][MixBus].
    float gainDb[kMixModes][kMixBuses] = {
        //  Music  Ambience  Sfx    Footsteps  Voice  Ui
        {0.f, 0.f, 0.f, 0.f, 0.f, 0.f},              // Explore
        {0.f, -6.f, 0.f, -3.f, 0.f, 0.f},            // Combat: the fight is the foreground
        {-6.f, -4.f, -3.f, -3.f, 0.f, 0.f},          // Dialogue: everything steps back from the voice
        {-3.f, -6.f, -2.f, -6.f, 0.f, -6.f},         // Cutscene
        {-12.f, -12.f, -12.f, -12.f, -12.f, 0.f},    // Menu: only the menu at full level
    };
    float attackSeconds = 0.15f;  // time for a falling bus to reach its target
    float releaseSeconds = 0.6f;  // time for a rising bus to reach its target
    // Sounds playing at once, per state.
    int voiceCap[kMixModes] = {24, 16, 16, 20, 8};
};

inline MixTable defaultMixTable() { return MixTable{}; }

struct MixGains {
    float linear[kMixBuses] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
    float db[kMixBuses] = {};
    float operator[](MixBus b) const { return linear[static_cast<int>(b)]; }
};

class MixState {
public:
    MixState() { reset(); }
    explicit MixState(const MixTable& table) : table_(table) { reset(); }

    // Every bus at the Explore target, nothing moving.
    void reset() {
        mode_ = MixMode::Explore;
        for (int b = 0; b < kMixBuses; ++b) {
            gainDb_[b] = target(MixMode::Explore, b);
            target_[b] = gainDb_[b];
            rate_[b] = 0.f;
        }
        fill();
    }

    // Moves every bus toward the active state's target by dt seconds (non-finite or negative: 0).
    const MixGains& step(MixRequests requests, float dt) {
        if (!std::isfinite(dt) || dt < 0.f) dt = 0.f;
        mode_ = activeMixMode(requests);
        for (int b = 0; b < kMixBuses; ++b) {
            const float t = target(mode_, b);
            if (t != target_[b]) {
                target_[b] = t;
                const float span = t - gainDb_[b];
                const float time = span < 0.f ? table_.attackSeconds : table_.releaseSeconds;
                rate_[b] = time > 0.f && std::isfinite(time) ? std::fabs(span) / time : 0.f; // 0: jump
            }
            const float diff = target_[b] - gainDb_[b];
            if (diff == 0.f) continue;
            const float move = rate_[b] * dt;
            if (rate_[b] <= 0.f || std::fabs(diff) <= move) gainDb_[b] = target_[b];
            else gainDb_[b] += diff > 0.f ? move : -move;
        }
        fill();
        return gains_;
    }

    MixMode active() const { return mode_; }
    const MixGains& gains() const { return gains_; }
    float gainDb(MixBus b) const { return gainDb_[static_cast<int>(b)]; }
    float targetDb(MixBus b) const { return target_[static_cast<int>(b)]; }
    int voiceCap() const { return voiceCapFor(mode_); }
    int voiceCapFor(MixMode m) const {
        const int i = static_cast<int>(m);
        return i >= 0 && i < kMixModes ? table_.voiceCap[i] : kMixMaxVoices;
    }
    const MixTable& table() const { return table_; }

private:
    float target(MixMode m, int b) const {
        const float v = table_.gainDb[static_cast<int>(m)][b];
        if (std::isnan(v) || v > 1e30f) return 0.f; // NaN and +infinity: unity
        if (v > kMixCeilingDb) return kMixCeilingDb;  // a finite boost past the ceiling: clamped
        return v <= kMixFloorDb ? kMixFloorDb : v;   // -infinity and anything at or below the floor: silence
    }
    void fill() {
        for (int b = 0; b < kMixBuses; ++b) {
            gains_.db[b] = gainDb_[b];
            gains_.linear[b] = gainDb_[b] <= kMixFloorDb ? 0.f : std::pow(10.f, gainDb_[b] / 20.f);
        }
    }

    MixTable table_{};
    MixMode mode_ = MixMode::Explore;
    float gainDb_[kMixBuses] = {};
    float target_[kMixBuses] = {};
    float rate_[kMixBuses] = {}; // dB per second toward target_
    MixGains gains_{};
};

// One playing sound, as the voice cap sees it.
struct MixVoice {
    int priority = 0;        // higher is kept longer (the cue's prio, 0..100)
    float ageSeconds = 0.f;  // time since it started
};

// The voices that keep playing; every other voice given is to be stopped.
struct MixVoiceStops {
    std::uint32_t keep[kMixMaxVoices] = {}; // indices into the voices given, best first
    int keepCount = 0;
    std::size_t count = 0;                  // voices to stop: the input length minus keepCount
    bool stops(std::size_t i) const {
        for (int k = 0; k < keepCount; ++k)
            if (keep[k] == i) return false;
        return true;
    }
};

// Which sounds to stop so at most `cap` keep playing, for any number of voices: the lowest
// priority stops first, then the oldest, then (a full tie) the lower index. At most 32 voices are
// kept (a cap above 32 counts as 32, below 0 as 0). Fixed-capacity selection, O(n * cap).
inline MixVoiceStops mixVoicesToStop(std::span<const MixVoice> voices, int cap) {
    MixVoiceStops out;
    if (cap < 0) cap = 0;
    if (cap > kMixMaxVoices) cap = kMixMaxVoices;
    const auto age = [](const MixVoice& v) { return std::isfinite(v.ageSeconds) ? v.ageSeconds : 0.f; };
    // True when voice a is kept in preference to voice b.
    const auto keptBefore = [&](std::size_t a, std::size_t b) {
        const MixVoice& va = voices[a];
        const MixVoice& vb = voices[b];
        if (va.priority != vb.priority) return va.priority > vb.priority;
        if (age(va) != age(vb)) return age(va) < age(vb);
        return a > b;
    };
    for (std::size_t i = 0; i < voices.size() && cap > 0; ++i) {
        int at = out.keepCount;
        if (out.keepCount == cap) {
            if (!keptBefore(i, out.keep[cap - 1])) continue; // no better than the worst kept
            at = cap - 1;                                    // drop the worst
        } else {
            ++out.keepCount;
        }
        while (at > 0 && keptBefore(i, out.keep[at - 1])) {
            out.keep[at] = out.keep[at - 1];
            --at;
        }
        out.keep[at] = static_cast<std::uint32_t>(i);
    }
    out.count = voices.size() - static_cast<std::size_t>(out.keepCount);
    return out;
}

} // namespace ftcore
