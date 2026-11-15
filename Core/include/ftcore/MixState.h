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
inline constexpr int kMixMaxVoices = 32;

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
        return std::isfinite(v) ? v : 0.f;
    }
    void fill() {
        for (int b = 0; b < kMixBuses; ++b) {
            gains_.db[b] = gainDb_[b];
            gains_.linear[b] = std::pow(10.f, gainDb_[b] / 20.f);
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

struct MixVoiceStops {
    std::uint8_t index[kMixMaxVoices] = {}; // indices into the voices given, in stop order
    int count = 0;
};

// Which sounds to stop so at most `cap` keep playing: the lowest priority first, then the oldest,
// then (a full tie) the lower index. Only the first 32 voices are looked at; a cap below 0 counts as 0.
inline MixVoiceStops mixVoicesToStop(std::span<const MixVoice> voices, int cap) {
    MixVoiceStops out;
    const int n = voices.size() > static_cast<std::size_t>(kMixMaxVoices) ? kMixMaxVoices : static_cast<int>(voices.size());
    if (cap < 0) cap = 0;
    if (n <= cap) return out;
    std::uint8_t order[kMixMaxVoices];
    for (int i = 0; i < n; ++i) order[i] = static_cast<std::uint8_t>(i);
    const auto stopsFirst = [&](std::uint8_t a, std::uint8_t b) {
        const MixVoice& va = voices[a];
        const MixVoice& vb = voices[b];
        if (va.priority != vb.priority) return va.priority < vb.priority;
        const float aa = std::isfinite(va.ageSeconds) ? va.ageSeconds : 0.f;
        const float ab = std::isfinite(vb.ageSeconds) ? vb.ageSeconds : 0.f;
        if (aa != ab) return aa > ab;
        return a < b;
    };
    for (int i = 1; i < n; ++i) { // insertion sort: at most 32 entries
        const std::uint8_t x = order[i];
        int j = i;
        while (j > 0 && stopsFirst(x, order[j - 1])) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = x;
    }
    out.count = n - cap;
    for (int i = 0; i < out.count; ++i) out.index[i] = order[i];
    return out;
}

} // namespace ftcore
