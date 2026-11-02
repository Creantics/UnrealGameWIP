#pragma once

// The beat boss demo's synth (docs/BEAT_BOSS_DEMO.md): every sound of BeatMusic.h made in code, mono, one float
// per frame. No sample, no file. A kick is a sine that drops in pitch with a click; a snare noise and a tone
// (a clap layered on later tiers); hats filtered noise; the bass saws and a sine through a low-pass that closes
// with each note; the stabs detuned saws in a chord; the riser a noise band that climbs with a sine; the impact
// a falling sub with a noise burst. The move accents (thump, snare, bell) and the count's blips sit on their own
// bus above the music, and each accent ducks the music for a moment so the player's cue reads over it.
//
//   ftcore::BeatMusicSynth synth;
//   synth.init(48000);
//   synth.play(sample, ftcore::MusicSound::Kick, 127);      // at an absolute frame of the synth's own count
//   synth.render(buffer, 512);                                // advances position() by 512
//
// Sounds are timed in frames of the synth's own clock (position() counts what render has produced), so a host
// that knows how a clock second maps to a frame places every sound exactly. A sound due before position() starts
// at the next frame (late()). revokeFrom drops sounds not yet started; cut also fades what is sounding.
//
// The player's cues sit on the cue bus with the accents: a tick when a move is told (a voice per answer), a quiet pickup,
// a chime on a Perfect (a tone of the playing chord) and a duller one on a Good.
//
// One thread (the host's audio thread drains its own queue into it). Output is soft-clipped: it never reaches 1.
// Fixed capacity (24 voices, 256 pending), no allocation after init; deterministic.

#include "ftcore/Api.h"
#include "ftcore/BeatMusic.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

class BeatMusicSynth {
public:
    static constexpr int kMaxVoices = 24;
    static constexpr std::size_t kMaxPending = 256;

    FTCORE_API void init(int sampleRate);
    // Queues a sound at absolute frame `at`. note: MIDI (Bass, Stab). aux as in MusicHit. lengthFrames 0: the sound's own.
    FTCORE_API void play(std::uint64_t at, MusicSound sound, std::uint8_t velocity, std::uint8_t note = 0,
                         std::uint8_t aux = 0, std::uint32_t lengthFrames = 0);
    // Drops the sounds queued for frame `at` or later.
    FTCORE_API void revokeFrom(std::uint64_t at);
    // Drops the queued sounds from `at` and fades everything sounding out at `at`.
    FTCORE_API void cut(std::uint64_t at);
    // Writes `frames` samples and advances the position.
    FTCORE_API void render(float* out, int frames);

    std::uint64_t position() const { return position_; }
    int sampleRate() const { return rate_; }
    int activeVoices() const;
    std::size_t pending() const { return pendingCount_; }
    std::uint32_t late() const { return late_; }       // sounds queued after their frame
    std::uint32_t dropped() const { return dropped_; } // sounds that found the queue full
    // The largest the mix has been before the soft clip since init() or resetPeak(): the output itself never
    // reaches 1, so only this says whether the mix is asking too much of it. Past about 1.2 the clip is bending
    // the sound audibly.
    float preClipPeak() const { return peak_; }
    void resetPeak() { peak_ = 0.0f; }

private:
    struct Pending {
        std::uint64_t at = 0;
        MusicSound sound = MusicSound::Kick;
        bool cut = false;
        std::uint8_t velocity = 0;
        std::uint8_t note = 0;
        std::uint8_t aux = 0;
        std::uint32_t length = 0;
    };
    struct Voice {
        bool active = false;
        MusicSound sound = MusicSound::Kick;
        std::uint8_t aux = 0;
        std::uint32_t age = 0;
        std::uint32_t length = 0; // frames the voice sounds, release included
        std::uint32_t hold = 0;   // frames before its release (notes)
        std::uint32_t fadeLeft = 0;
        std::uint32_t fadeTotal = 0;
        std::uint32_t rng = 1;
        float velocity = 0.0f;
        float freq = 0.0f;
        std::array<float, 9> phase{};
        float low = 0.0f, band = 0.0f; // state-variable filter
        float prev = 0.0f;             // the noise's high-pass
        float tone = 0.0f;             // a swept oscillator's phase
        std::array<float, 3> ratio{};  // a stab's chord
    };

    void start(const Pending& p);
    float voiceSample(Voice& v);
    void apply(const Pending& p);

    int rate_ = 48000;
    float invRate_ = 1.0f / 48000.0f;
    std::uint64_t position_ = 0;
    std::array<Voice, kMaxVoices> voices_{};
    std::array<Pending, kMaxPending> pending_{};
    std::size_t pendingCount_ = 0;
    std::uint32_t started_ = 0; // sounds started so far: seeds each voice's noise
    std::uint32_t late_ = 0;
    std::uint32_t dropped_ = 0;
    float peak_ = 0.0f;
    float duck_ = 1.0f;
    std::uint32_t duckHold_ = 0;
    float duckRelease_ = 0.0f;
};

} // namespace ftcore
