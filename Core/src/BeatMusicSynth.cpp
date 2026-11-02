#include "ftcore/BeatMusicSynth.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr float kTwoPi = 6.2831853f;
constexpr float kPi = 3.1415927f;

float noiseOf(std::uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float>(s & 0xFFFFu) / 32767.5f - 1.0f;
}

float wrap(float p) { return p - std::floor(p); }

float saw(float phase) { return 2.0f * wrap(phase) - 1.0f; }

bool isCue(MusicSound s) { return static_cast<int>(s) >= static_cast<int>(MusicSound::AccentThump); }
bool isAccent(MusicSound s) {
    return s == MusicSound::AccentThump || s == MusicSound::AccentSnare || s == MusicSound::AccentBell;
}

float midiFreq(int note) { return 440.0f * std::exp2(static_cast<float>(note - 69) / 12.0f); }

// One step of a state-variable filter; returns the low-pass, leaves the band in `band`.
float svf(float in, float fc, float q, float invRate, float& low, float& band) {
    const float g = 2.0f * std::sin(kPi * std::min(fc, 9000.0f) * invRate);
    low += g * band;
    const float high = in - low - q * band;
    band += g * high;
    return low;
}

constexpr float kMusicGain = 0.4f;
constexpr float kCueGain = 1.0f;
constexpr float kMaster = 0.97f;
constexpr float kDuckLevel = 0.2f;
constexpr float kDuckHold = 0.09f;    // seconds the music stays down
constexpr float kDuckRelease = 0.15f; // seconds it takes to come back
constexpr float kCutFade = 0.012f;    // seconds

} // namespace

void BeatMusicSynth::init(int sampleRate) {
    rate_ = std::max(8000, sampleRate);
    invRate_ = 1.0f / static_cast<float>(rate_);
    position_ = 0;
    for (Voice& v : voices_) v = Voice{};
    pendingCount_ = 0;
    started_ = 0;
    late_ = 0;
    dropped_ = 0;
    duck_ = 1.0f;
    duckHold_ = 0;
    peak_ = 0.0f;
    duckRelease_ = 1.0f - std::exp(-invRate_ / kDuckRelease);
}

int BeatMusicSynth::activeVoices() const {
    int n = 0;
    for (const Voice& v : voices_)
        if (v.active) ++n;
    return n;
}

void BeatMusicSynth::play(std::uint64_t at, MusicSound sound, std::uint8_t velocity, std::uint8_t note, std::uint8_t aux,
                          std::uint32_t lengthFrames) {
    if (velocity == 0 || sound >= MusicSound::Count) return;
    if (at < position_) {
        at = position_;
        ++late_;
    }
    if (pendingCount_ >= pending_.size()) {
        ++dropped_;
        return;
    }
    std::size_t i = pendingCount_;
    while (i > 0 && pending_[i - 1].at > at) {
        pending_[i] = pending_[i - 1];
        --i;
    }
    Pending& p = pending_[i];
    p = Pending{};
    p.at = at;
    p.sound = sound;
    p.velocity = velocity;
    p.note = note;
    p.aux = aux;
    p.length = lengthFrames;
    ++pendingCount_;
}

void BeatMusicSynth::revokeFrom(std::uint64_t at) {
    std::size_t keep = 0;
    for (std::size_t i = 0; i < pendingCount_; ++i)
        if (pending_[i].at < at) pending_[keep++] = pending_[i];
    pendingCount_ = keep;
}

void BeatMusicSynth::cut(std::uint64_t at) {
    revokeFrom(at);
    if (at < position_) at = position_;
    if (pendingCount_ >= pending_.size()) {
        ++dropped_;
        return;
    }
    std::size_t i = pendingCount_;
    while (i > 0 && pending_[i - 1].at > at) {
        pending_[i] = pending_[i - 1];
        --i;
    }
    Pending& p = pending_[i];
    p = Pending{};
    p.at = at;
    p.cut = true;
    ++pendingCount_;
}

void BeatMusicSynth::apply(const Pending& p) {
    if (p.cut) {
        const std::uint32_t fade = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(kCutFade * static_cast<float>(rate_)));
        for (Voice& v : voices_) {
            if (v.active && v.fadeLeft == 0) v.fadeLeft = v.fadeTotal = fade;
        }
        return;
    }
    start(p);
}

void BeatMusicSynth::start(const Pending& p) {
    // A free voice, or the oldest.
    std::size_t slot = 0;
    for (std::size_t i = 0; i < voices_.size(); ++i) {
        if (!voices_[i].active) {
            slot = i;
            break;
        }
        if (voices_[i].age > voices_[slot].age) slot = i;
    }
    Voice& v = voices_[slot];
    v = Voice{};
    v.active = true;
    v.sound = p.sound;
    v.aux = p.aux;
    v.velocity = static_cast<float>(std::min<int>(p.velocity, 127)) / 127.0f;
    ++started_;
    v.rng = 2463534242u ^ (started_ * 2654435761u);
    if (v.rng == 0) v.rng = 1;
    const float fr = static_cast<float>(rate_);
    const auto frames = [fr](float seconds) { return static_cast<std::uint32_t>(seconds * fr); };

    switch (p.sound) {
    case MusicSound::Kick: v.length = frames(0.45f); break;
    case MusicSound::Snare: v.length = frames(0.28f); break;
    case MusicSound::HatClosed: v.length = frames(0.07f); break;
    case MusicSound::HatOpen:
        if (p.aux == 1) v.length = std::min(p.length > 0 ? p.length : frames(1.6f), frames(1.6f));
        else v.length = std::clamp(p.length > 0 ? p.length : frames(0.3f), frames(0.08f), frames(0.4f));
        v.hold = v.length > frames(0.012f) ? v.length - frames(0.012f) : 0;
        break;
    case MusicSound::Bass:
        v.freq = midiFreq(p.note);
        v.hold = std::max(p.length, frames(0.06f));
        v.length = v.hold + frames(0.012f);
        break;
    case MusicSound::Stab: {
        v.freq = midiFreq(p.note);
        const bool major = (p.aux & 1) != 0;
        v.ratio = {1.0f, std::exp2((major ? 4.0f : 3.0f) / 12.0f), std::exp2(7.0f / 12.0f)};
        v.hold = std::max(p.length, frames(0.05f));
        v.length = v.hold + ((p.aux & 2) != 0 ? frames(0.25f) : frames(0.06f));
        break;
    }
    case MusicSound::Riser:
        v.hold = std::max(p.length, frames(0.2f));
        v.length = v.hold + frames(0.005f);
        break;
    case MusicSound::Impact: v.length = frames(1.6f); break;
    case MusicSound::AccentThump: v.length = frames(0.32f); break;
    case MusicSound::AccentSnare: v.length = frames(0.2f); break;
    case MusicSound::AccentBell: v.length = frames(0.45f); break;
    case MusicSound::CountClick: v.length = frames(0.09f); break;
    case MusicSound::CountVoice: v.length = frames(0.16f); break;
    case MusicSound::Good: v.length = frames(0.07f); break;
    case MusicSound::Miss: v.length = frames(0.24f); break;
    case MusicSound::TellTick:
        v.aux = p.aux;
        v.length = p.aux == 8 ? frames(0.34f) : p.aux == 5 ? frames(0.2f) : frames(0.16f);
        break;
    case MusicSound::Pickup: v.length = frames(0.06f); break;
    case MusicSound::PerfectChime:
        v.freq = midiFreq(p.note);
        v.length = frames(0.34f);
        break;
    case MusicSound::GoodCue:
        v.freq = midiFreq(p.note);
        v.length = frames(0.16f);
        break;
    case MusicSound::Count: v.active = false; return;
    }
    if (v.length == 0) v.length = 1;
    if (isAccent(p.sound)) {
        duck_ = kDuckLevel;
        duckHold_ = frames(kDuckHold);
    }
}

float BeatMusicSynth::voiceSample(Voice& v) {
    const float t = static_cast<float>(v.age) * invRate_;
    float x = 0.0f;
    switch (v.sound) {
    case MusicSound::Kick: {
        const float f = 46.0f + 150.0f * std::exp(-t * 45.0f);
        v.tone = wrap(v.tone + f * invRate_);
        const float body = std::sin(kTwoPi * v.tone) * std::exp(-t * 7.0f);
        const float click = noiseOf(v.rng) * std::exp(-t * 800.0f) * 0.45f;
        x = std::tanh(1.8f * (body + click)) * 0.9f;
        break;
    }
    case MusicSound::Snare: {
        const float n = noiseOf(v.rng);
        const float hp = n - v.prev;
        v.prev = n;
        const float f = 175.0f + 90.0f * std::exp(-t * 50.0f);
        v.tone = wrap(v.tone + f * invRate_);
        float clap = 0.0f;
        if (v.aux == 1) {
            for (int k = 0; k < 3; ++k) {
                const float dt = t - 0.011f * static_cast<float>(k);
                if (dt >= 0.0f) clap += std::exp(-dt * 130.0f);
            }
        }
        x = hp * (0.3f * std::exp(-t * 20.0f) + 0.15f * clap) + 0.5f * std::sin(kTwoPi * v.tone) * std::exp(-t * 26.0f);
        break;
    }
    case MusicSound::HatClosed: {
        const float n = noiseOf(v.rng);
        const float hp = n - v.prev;
        v.prev = n;
        x = hp * 0.3f * std::exp(-t * 110.0f);
        break;
    }
    case MusicSound::HatOpen: {
        const float n = noiseOf(v.rng);
        const float hp = n - v.prev;
        v.prev = n;
        const bool crash = v.aux == 1;
        x = hp * (crash ? 0.22f : 0.2f) * std::exp(-t * (crash ? 3.2f : 13.0f));
        if (v.hold > 0 && v.age > v.hold) x *= static_cast<float>(v.length - v.age) / static_cast<float>(v.length - v.hold);
        break;
    }
    case MusicSound::Bass: {
        v.phase[0] = wrap(v.phase[0] + v.freq * invRate_);
        v.phase[1] = wrap(v.phase[1] + v.freq * 1.006f * invRate_);
        const float raw = 0.5f * std::sin(kTwoPi * v.phase[0]) + 0.28f * (saw(v.phase[0]) + saw(v.phase[1]));
        const float fc = 160.0f + 1500.0f * std::exp(-t * 13.0f) * (0.35f + 0.65f * v.velocity);
        float env = std::min(1.0f, t / 0.002f);
        if (v.age > v.hold) env *= static_cast<float>(v.length - v.age) / static_cast<float>(v.length - v.hold);
        x = std::tanh(1.4f * svf(raw, fc, 0.9f, invRate_, v.low, v.band)) * env * 0.5f;
        break;
    }
    case MusicSound::Stab: {
        static constexpr float kDetune[3] = {0.9948f, 1.0f, 1.0052f};
        const bool pad = (v.aux & 2) != 0;
        float sum = 0.0f;
        for (int n = 0; n < 3; ++n) {
            for (int d = 0; d < 3; ++d) {
                float& ph = v.phase[static_cast<std::size_t>(n * 3 + d)];
                ph = wrap(ph + v.freq * v.ratio[static_cast<std::size_t>(n)] * kDetune[d] * invRate_);
                sum += saw(ph);
            }
        }
        const float fc = pad ? 500.0f + 800.0f * (1.0f - std::exp(-t * 4.0f)) : 600.0f + 2800.0f * std::exp(-t * 10.0f);
        float env = std::min(1.0f, t / (pad ? 0.08f : 0.004f));
        if (v.age > v.hold) env *= static_cast<float>(v.length - v.age) / static_cast<float>(v.length - v.hold);
        x = svf(sum, fc, 0.8f, invRate_, v.low, v.band) * env * (pad ? 0.035f : 0.06f);
        break;
    }
    case MusicSound::Riser: {
        const float t01 = std::min(1.0f, static_cast<float>(v.age) / static_cast<float>(v.hold));
        const float fc = 300.0f * std::exp(3.0f * t01);
        const float n = noiseOf(v.rng);
        svf(n, fc, 0.35f, invRate_, v.low, v.band);
        const float f = 180.0f * std::exp(2.2f * t01);
        v.tone = wrap(v.tone + f * invRate_);
        const float amp = 0.1f + 0.9f * t01 * t01;
        x = amp * (v.band * 0.16f + 0.2f * std::sin(kTwoPi * v.tone));
        if (v.age > v.hold) x *= static_cast<float>(v.length - v.age) / static_cast<float>(v.length - v.hold);
        break;
    }
    case MusicSound::Impact: {
        const float f = 32.0f + 45.0f * std::exp(-t * 3.0f);
        v.tone = wrap(v.tone + f * invRate_);
        const float n = noiseOf(v.rng);
        v.low += 0.12f * (n - v.low);
        x = std::tanh(0.9f * std::sin(kTwoPi * v.tone) * std::exp(-t * 2.4f) + 1.6f * v.low * std::exp(-t * 6.0f)) * 0.85f;
        break;
    }
    case MusicSound::AccentThump: {
        const float f = 70.0f + 150.0f * std::exp(-t * 40.0f);
        v.tone = wrap(v.tone + f * invRate_);
        x = std::tanh(1.5f * (std::sin(kTwoPi * v.tone) * std::exp(-t * 10.0f) * 0.9f +
                              std::sin(kTwoPi * 1800.0f * t) * std::exp(-t * 300.0f) * 0.5f)) * 0.8f;
        break;
    }
    case MusicSound::AccentSnare: {
        const float n = noiseOf(v.rng);
        const float hp = n - v.prev;
        v.prev = n;
        x = 0.3f * hp * std::exp(-t * 24.0f) + 0.5f * std::sin(kTwoPi * 220.0f * t) * std::exp(-t * 35.0f) +
            0.12f * std::sin(kTwoPi * 3200.0f * t) * std::exp(-t * 90.0f);
        break;
    }
    case MusicSound::AccentBell:
        x = 0.58f * std::exp(-t * 7.0f) *
            (std::sin(kTwoPi * 1320.0f * t) + 0.5f * std::sin(kTwoPi * 1980.0f * t) + 0.3f * std::sin(kTwoPi * 2640.0f * t));
        break;
    case MusicSound::CountClick: x = 0.45f * std::exp(-t * 45.0f) * std::sin(kTwoPi * 1000.0f * t); break;
    case MusicSound::CountVoice: {
        const float s = std::sin(kTwoPi * 440.0f * t);
        x = 0.35f * std::exp(-t * 18.0f) * (0.6f * s + (s >= 0.0f ? 0.4f : -0.4f));
        break;
    }
    case MusicSound::Good: x = 0.22f * std::exp(-t * 55.0f) * std::sin(kTwoPi * 1760.0f * t); break;
    case MusicSound::Miss: x = 0.3f * std::exp(-t * 13.0f) * saw(98.0f * t); break;
    case MusicSound::TellTick: {
        // A move told. Each answer has its own voice, so the ear can tell a Guard from a Parry: the amplitudes are
        // the spec's -18 dB (a strike) and -16 dB (a counter or a hold) under the accent's peak.
        struct Voice2 {
            float f1, f2, gap, decay, amp;
        };
        static constexpr Voice2 kVoices[9] = {
            {1500.0f, 0.0f, 0.0f, 90.0f, 0.125f}, // attack: a dry tock
            {330.0f, 440.0f, 0.045f, 38.0f, 0.16f},  // guard: a low rise
            {392.0f, 523.0f, 0.045f, 38.0f, 0.16f},  // parry: the rise a fourth higher
            {880.0f, 0.0f, 0.0f, 55.0f, 0.15f},      // dodge: one clean blip
            {1175.0f, 0.0f, 0.0f, 55.0f, 0.15f},     // jump: the blip higher
            {392.0f, 0.0f, 0.0f, 14.0f, 0.16f},      // hold: a note that stays a moment
            {392.0f, 587.0f, 0.0f, 38.0f, 0.16f},    // paired: two notes together
            {330.0f, 0.0f, 0.0f, 14.0f, 0.16f},      // release: a lower held note
            {500.0f, 1000.0f, 0.0f, 9.0f, 0.2f},     // a finisher's tell: a shimmer that climbs
        };
        const Voice2& vc = kVoices[std::min<int>(v.aux, 8)];
        if (v.aux == 8) {
            const float f = vc.f1 * std::exp2(t * 3.0f);
            v.tone = wrap(v.tone + f * invRate_);
            x = vc.amp * std::sin(kTwoPi * v.tone) * std::min(1.0f, t / 0.02f) * std::exp(-std::max(0.0f, t - 0.22f) * 40.0f);
        } else if (v.aux == 5 || v.aux == 7) {
            x = vc.amp * std::sin(kTwoPi * vc.f1 * t) * std::min(1.0f, t / 0.012f) * std::exp(-t * vc.decay);
        } else if (v.aux == 6) {
            x = vc.amp * 0.6f * (std::sin(kTwoPi * vc.f1 * t) + std::sin(kTwoPi * vc.f2 * t)) * std::exp(-t * vc.decay);
        } else {
            x = vc.amp * std::sin(kTwoPi * vc.f1 * t) * std::exp(-t * vc.decay);
            if (vc.f2 > 0.0f && t >= vc.gap) {
                const float u = t - vc.gap;
                x += vc.amp * std::sin(kTwoPi * vc.f2 * u) * std::exp(-u * vc.decay);
            }
        }
        break;
    }
    case MusicSound::Pickup: {
        // A quiet ride-top tick: a high noise band and a thin tone, -14 dB under the accent.
        const float n = noiseOf(v.rng);
        const float hp = n - v.prev;
        v.prev = n;
        x = 0.2f * (0.7f * hp * std::exp(-t * 90.0f) + 0.3f * std::sin(kTwoPi * 3100.0f * t) * std::exp(-t * 110.0f));
        break;
    }
    case MusicSound::PerfectChime: {
        // A tone of the playing chord with a bright shimmer over it (-6 dB, the spec's PRF).
        const float e = std::exp(-t * 14.0f) * std::min(1.0f, t / 0.002f);
        x = 0.17f * e *
            (std::sin(kTwoPi * v.freq * t) + 0.45f * std::sin(kTwoPi * v.freq * 2.0f * t) + 0.25f * std::sin(kTwoPi * v.freq * 3.0f * t));
        break;
    }
    case MusicSound::GoodCue:
        // The Perfect's dull cousin: one soft tone of the chord, no shimmer, four dB lower.
        x = 0.17f * std::sin(kTwoPi * v.freq * t) * std::min(1.0f, t / 0.004f) * std::exp(-t * 26.0f);
        break;
    case MusicSound::Count: break;
    }
    x *= v.velocity;
    if (v.fadeLeft > 0) {
        x *= static_cast<float>(v.fadeLeft) / static_cast<float>(v.fadeTotal);
        if (--v.fadeLeft == 0) v.active = false;
    }
    return x;
}

void BeatMusicSynth::render(float* out, int frames) {
    for (int i = 0; i < frames; ++i) {
        const std::uint64_t pos = position_ + static_cast<std::uint64_t>(i);
        while (pendingCount_ > 0 && pending_[0].at <= pos) {
            const Pending p = pending_[0];
            for (std::size_t k = 1; k < pendingCount_; ++k) pending_[k - 1] = pending_[k];
            --pendingCount_;
            apply(p);
        }
        float music = 0.0f;
        float cue = 0.0f;
        for (Voice& v : voices_) {
            if (!v.active) continue;
            const float s = voiceSample(v);
            if (isCue(v.sound)) cue += s;
            else music += s;
            if (++v.age >= v.length) v.active = false;
        }
        if (duckHold_ > 0) --duckHold_;
        else duck_ += (1.0f - duck_) * duckRelease_;
        const float mix = kMusicGain * duck_ * music + kCueGain * cue;
        peak_ = std::max(peak_, std::fabs(mix));
        out[i] = std::tanh(mix) * kMaster;
    }
    position_ += static_cast<std::uint64_t>(frames);
}

} // namespace ftcore
