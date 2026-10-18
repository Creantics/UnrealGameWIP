#include "ftcore/Face.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ftcore {

namespace {

bool setError(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
    return false;
}

float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }

bool fillPreset(const FaceShapeSet& set, std::string_view name, std::span<const FaceShapeWeight> weights,
                FacePreset& out, std::string* error) {
    out.name = std::string(name);
    out.weights.fill(0.f);
    for (const auto& [shape, w] : weights) {
        const int i = set.find(shape);
        if (i < 0)
            return setError(error, "face preset '" + out.name + "' names unknown shape '" + std::string(shape) + "'");
        out.weights[static_cast<std::size_t>(i)] = w;
    }
    return true;
}

} // namespace

bool faceLayerFromName(std::string_view name, FaceLayer& out) {
    if (name == "upper") out = FaceLayer::Upper;
    else if (name == "eyes") out = FaceLayer::Eyes;
    else if (name == "mouth_art") out = FaceLayer::MouthArticulation;
    else if (name == "mouth_tone") out = FaceLayer::MouthTone;
    else if (name == "corrective") out = FaceLayer::Corrective;
    else return false;
    return true;
}

// ---- shape set --------------------------------------------------------------------------------

int FaceShapeSet::find(std::string_view name) const {
    for (std::size_t i = 0; i < shapes.size(); ++i)
        if (shapes[i] == name) return static_cast<int>(i);
    return -1;
}

const FacePreset* FaceShapeSet::viseme(std::string_view name) const {
    for (const FacePreset& p : visemes)
        if (p.name == name) return &p;
    return nullptr;
}

const FacePreset* FaceShapeSet::expression(std::string_view name) const {
    for (const FacePreset& p : expressions)
        if (p.name == name) return &p;
    return nullptr;
}

bool FaceShapeSet::addShape(std::string_view name, FaceLayer layer, std::string* error) {
    if (name.empty()) return setError(error, "face shape without a name");
    if (find(name) >= 0) return setError(error, "face shape '" + std::string(name) + "' is listed twice");
    if (shapes.size() >= kMaxFaceShapes) return setError(error, "face shape set has more than 64 shapes");
    shapes.emplace_back(name);
    layers.push_back(layer);
    return true;
}

bool FaceShapeSet::addViseme(std::string_view name, std::span<const FaceShapeWeight> weights, FaceVec3 tongueDeg,
                             std::string* error) {
    FacePreset p;
    if (!fillPreset(*this, name, weights, p, error)) return false;
    p.tongueDeg = tongueDeg;
    visemes.push_back(std::move(p));
    return true;
}

bool FaceShapeSet::addExpression(std::string_view name, std::span<const FaceShapeWeight> weights, std::string* error) {
    FacePreset p;
    if (!fillPreset(*this, name, weights, p, error)) return false;
    expressions.push_back(std::move(p));
    return true;
}

bool FaceShapeSet::finish(std::string* error) {
    if (shapes.size() > kMaxFaceShapes || layers.size() != shapes.size())
        return setError(error, "face shape set has more than 64 shapes");
    correctives.clear();
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        if (layers[i] != FaceLayer::Corrective) continue;
        const std::string& n = shapes[i];
        const auto x = n.find("_x_");
        if (x == std::string::npos) return setError(error, "corrective shape '" + n + "' is not named a_x_b");
        Corrective c;
        c.shape = static_cast<int>(i);
        c.driverA = find(std::string_view(n).substr(0, x));
        const std::string b = n.substr(x + 3);
        c.driverB = find(b);
        c.driverB2 = -1;
        if (c.driverB < 0) { // side-less driver: the larger of the Left / Right shapes
            c.driverB = find(b + "Left");
            c.driverB2 = find(b + "Right");
        }
        if (c.driverA < 0 || c.driverB < 0) return setError(error, "corrective shape '" + n + "' names unknown drivers");
        correctives.push_back(c);
    }
    jawOpen = find("jawOpen");
    mouthFunnel = find("mouthFunnel");
    mouthPucker = find("mouthPucker");
    mouthClose = find("mouthClose");
    blinkLeft = find("eyeBlinkLeft");
    blinkRight = find("eyeBlinkRight");
    return true;
}

void composeFace(const FaceShapeSet& set, const FaceWeights& expr, const FaceWeights& vis, float speech,
                 const FaceWeights& micro, FaceWeights& out) {
    out.fill(0.f);
    const auto at = [](const FaceWeights& w, int i) { return i >= 0 ? w[static_cast<std::size_t>(i)] : 0.f; };
    const float rho = std::max(at(vis, set.mouthFunnel), at(vis, set.mouthPucker));
    const float close = at(vis, set.mouthClose);
    const float quiet = 1.f - speech;
    const std::size_t n = std::min(set.size(), kMaxFaceShapes);
    for (std::size_t s = 0; s < n; ++s) {
        const float e = expr[s], v = vis[s], m = micro[s];
        float w = 0.f;
        switch (set.layers[s]) {
        case FaceLayer::Upper: w = e + m; break;
        case FaceLayer::Eyes: w = std::max(e, std::max(v, m)); break;
        case FaceLayer::MouthArticulation:
            w = static_cast<int>(s) == set.jawOpen ? std::max(v, quiet * e) : v + quiet * e;
            break;
        case FaceLayer::MouthTone: w = e * (1.f - 0.7f * rho) * (1.f - 0.5f * close); break;
        case FaceLayer::Corrective: w = 0.f; break;
        }
        out[s] = clamp01(w);
    }
    for (const FaceShapeSet::Corrective& c : set.correctives)
        out[static_cast<std::size_t>(c.shape)] =
            std::min(1.f, at(out, c.driverA) * std::max(at(out, c.driverB), at(out, c.driverB2)));
}

// ---- .nface -----------------------------------------------------------------------------------

namespace {
constexpr char kMagic[6] = {'N', 'F', 'A', 'C', 'E', '\0'};
constexpr std::uint16_t kTrackVersion = 1;
} // namespace

bool parseFaceTrack(std::span<const std::uint8_t> bytes, FaceTrack& out, std::string* error) {
    out = FaceTrack{};
    std::size_t pos = 0;
    auto need = [&](std::size_t n) { return pos + n <= bytes.size(); };
    auto u16 = [&]() {
        const auto v = static_cast<std::uint16_t>(bytes[pos] | (bytes[pos + 1] << 8));
        pos += 2;
        return v;
    };
    auto u32 = [&]() {
        const std::uint32_t v = static_cast<std::uint32_t>(bytes[pos]) | (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
                                (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
                                (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
        pos += 4;
        return v;
    };
    if (!need(16) || std::memcmp(bytes.data(), kMagic, 6) != 0) return setError(error, "not an .nface track");
    pos = 6;
    FaceTrack t;
    if (u16() != kTrackVersion) return setError(error, ".nface version is not 1");
    const std::uint16_t rate = u16();
    t.frames = u32();
    const std::uint16_t channels = u16();
    if (rate == 0 || channels == 0 || channels > FacePlayer::kMaxTrackChannels)
        return setError(error, ".nface header is invalid");
    t.rate = static_cast<float>(rate);
    for (std::uint16_t c = 0; c < channels; ++c) {
        if (!need(1)) break;
        const std::size_t len = bytes[pos++];
        if (!need(len)) return setError(error, ".nface channel table is truncated");
        t.channels.emplace_back(reinterpret_cast<const char*>(bytes.data() + pos), len);
        pos += len;
    }
    const std::size_t dataSize = static_cast<std::size_t>(t.frames) * channels;
    if (t.channels.size() != channels || bytes.size() - pos != dataSize)
        return setError(error, ".nface data size does not match its header");
    t.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.end());
    out = std::move(t);
    return true;
}

std::vector<std::uint8_t> serializeFaceTrack(const FaceTrack& t) {
    std::vector<std::uint8_t> out(kMagic, kMagic + 6);
    auto u16 = [&](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xff));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    };
    u16(kTrackVersion);
    u16(static_cast<std::uint32_t>(std::lround(t.rate)));
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::uint8_t>((t.frames >> (8 * k)) & 0xff));
    u16(static_cast<std::uint32_t>(t.channels.size()));
    for (const std::string& c : t.channels) {
        const std::size_t len = std::min<std::size_t>(c.size(), 255);
        out.push_back(static_cast<std::uint8_t>(len));
        out.insert(out.end(), c.begin(), c.begin() + static_cast<std::ptrdiff_t>(len));
    }
    out.insert(out.end(), t.data.begin(), t.data.end());
    return out;
}

// ---- player -----------------------------------------------------------------------------------

namespace {
constexpr std::int16_t kChannelNone = -1;
constexpr std::int16_t kChannelSpeech = -2;
constexpr std::int16_t kChannelTongue0 = -3; // -3, -4, -5
constexpr float kSpeechRate = 1.f / 0.12f;   // speech ramps over 120 ms
// Blink shape (seconds): close, hold, open. ~180 ms in all.
constexpr float kBlinkClose = 0.07f, kBlinkHold = 0.02f, kBlinkOpen = 0.09f;
constexpr float kBlinkMinGap = 2.f, kBlinkGapRange = 4.f;

float& component(FaceVec3& v, int k) { return k == 0 ? v.x : k == 1 ? v.y : v.z; }
} // namespace

FacePlayer::FacePlayer(const FaceShapeSet& set, std::uint32_t seed) : set_(&set), rng_(seed ? seed : 1u) {
    channelShape_.fill(kChannelNone);
    scheduleBlink();
}

float FacePlayer::nextRandom() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (1.f / 16777216.f);
}

void FacePlayer::scheduleBlink() {
    blinkClock_ = 0.f;
    nextBlink_ = (kBlinkMinGap + kBlinkGapRange * nextRandom()) / blinkRate_;
}

void FacePlayer::setBlinkRate(float scale) { blinkRate_ = std::clamp(scale, 0.05f, 20.f); }

void FacePlayer::requestBlink() {
    if (blinking_ && blinkPhase_ < 0.f && blinkHold_ <= 0.f) blinkPhase_ = 0.f;
}

void FacePlayer::holdBlinks(float seconds) { blinkHold_ = std::max(blinkHold_, seconds); }

float FacePlayer::timeToNextBlink() const { return blinkPhase_ >= 0.f ? 0.f : std::max(0.f, nextBlink_ - blinkClock_); }

void FacePlayer::setExpression(const FacePreset* preset, float intensity, float fade) {
    exprFrom_ = expr_;
    const float k = std::clamp(intensity, 0.f, 5.f) * 0.2f;
    for (std::size_t i = 0; i < kMaxFaceShapes; ++i) exprTo_[i] = preset ? clamp01(preset->weights[i] * k) : 0.f;
    fadeDuration_ = std::max(0.f, fade);
    fadeTime_ = 0.f;
    if (fadeDuration_ == 0.f) expr_ = exprTo_;
}

bool FacePlayer::play(const FaceTrack& track, float time) {
    if (track.channels.size() > kMaxTrackChannels || track.data.size() < track.channels.size() * track.frames) {
        stop(); // a refused track plays nothing, so whatever was playing ends too
        return false;
    }
    channelShape_.fill(kChannelNone);
    trackHasSpeech_ = false;
    for (std::size_t c = 0; c < track.channels.size(); ++c) {
        const std::string& name = track.channels[c];
        if (name == "@speech") {
            channelShape_[c] = kChannelSpeech;
            trackHasSpeech_ = true;
        } else if (name == "@tongue_01" || name == "@tongue_02" || name == "@tongue_03") {
            channelShape_[c] = static_cast<std::int16_t>(kChannelTongue0 - (name.back() - '1'));
        } else {
            const int s = set_->find(name);
            channelShape_[c] = static_cast<std::int16_t>(s >= 0 && s < static_cast<int>(kMaxFaceShapes) ? s : kChannelNone);
        }
    }
    track_ = &track;
    trackTime_ = std::max(0.f, time);
    sampleTrack();
    return true;
}

void FacePlayer::stop() {
    track_ = nullptr;
    vis_.fill(0.f);
    tongue_ = {};
    speechTarget_ = 0.f;
}

void FacePlayer::setMicro(int shape, float weight) {
    if (shape >= 0 && shape < static_cast<int>(kMaxFaceShapes)) micro_[static_cast<std::size_t>(shape)] = clamp01(weight);
}

void FacePlayer::clearMicro() { micro_.fill(0.f); }

void FacePlayer::setDirectedMicro(const FaceWeights& weights) { directed_ = weights; }

void FacePlayer::sampleTrack() {
    vis_.fill(0.f);
    tongue_ = {};
    const FaceTrack& t = *track_;
    if (t.frames == 0) return;
    const float f = std::min(trackTime_ * t.rate, static_cast<float>(t.frames - 1));
    const auto f0 = static_cast<std::size_t>(f);
    const std::size_t f1 = std::min<std::size_t>(f0 + 1, t.frames - 1);
    const float a = f - static_cast<float>(f0);
    const std::size_t nc = t.channels.size();
    float speech = 1.f;
    for (std::size_t c = 0; c < nc; ++c) {
        const float v = (static_cast<float>(t.data[f0 * nc + c]) * (1.f - a) + static_cast<float>(t.data[f1 * nc + c]) * a) *
                        (1.f / 255.f);
        const std::int16_t dst = channelShape_[c];
        if (dst >= 0) vis_[static_cast<std::size_t>(dst)] = v;
        else if (dst == kChannelSpeech) speech = v;
        else if (dst <= kChannelTongue0 && dst > kChannelTongue0 - 3)
            component(tongue_, kChannelTongue0 - dst) = (v * 2.f - 1.f) * kTongueTrackRangeDeg;
    }
    speechTarget_ = trackHasSpeech_ ? speech : 1.f;
}

void FacePlayer::update(float dt) {
    dt = std::max(0.f, dt);
    // expression crossfade (smoothstep)
    if (fadeTime_ < fadeDuration_) {
        fadeTime_ = std::min(fadeDuration_, fadeTime_ + dt);
        const float s = fadeTime_ / fadeDuration_;
        const float k = s * s * (3.f - 2.f * s);
        for (std::size_t i = 0; i < kMaxFaceShapes; ++i) expr_[i] = exprFrom_[i] + (exprTo_[i] - exprFrom_[i]) * k;
    }
    // lip-sync track
    if (track_) {
        trackTime_ += dt;
        if (trackTime_ > track_->duration()) stop();
        else sampleTrack();
    }
    const float step = kSpeechRate * dt;
    speech_ = speech_ < speechTarget_ ? std::min(speechTarget_, speech_ + step) : std::max(speechTarget_, speech_ - step);
    // blinks
    float blink = 0.f;
    if (blinking_) {
        blinkClock_ += dt;
        if (blinkPhase_ < 0.f && blinkClock_ >= nextBlink_ && blinkHold_ <= 0.f) blinkPhase_ = 0.f;
        if (blinkPhase_ >= 0.f) {
            blinkPhase_ += dt;
            const float p = blinkPhase_;
            if (p < kBlinkClose) blink = p / kBlinkClose;
            else if (p < kBlinkClose + kBlinkHold) blink = 1.f;
            else if (p < kBlinkClose + kBlinkHold + kBlinkOpen) blink = 1.f - (p - kBlinkClose - kBlinkHold) / kBlinkOpen;
            else {
                blinkPhase_ = -1.f;
                scheduleBlink();
            }
        }
    }
    blinkHold_ = std::max(0.f, blinkHold_ - dt);
    for (std::size_t i = 0; i < kMaxFaceShapes; ++i) microAll_[i] = clamp01(micro_[i] + directed_[i]);
    for (const int b : {set_->blinkLeft, set_->blinkRight})
        if (b >= 0) microAll_[static_cast<std::size_t>(b)] = std::max(microAll_[static_cast<std::size_t>(b)], blink);
    composeFace(*set_, expr_, vis_, speech_, microAll_, out_);
}

float FacePlayer::jawDeg() const {
    return set_->jawOpen >= 0 ? out_[static_cast<std::size_t>(set_->jawOpen)] * set_->jawMaxDeg : 0.f;
}

void FacePlayer::bindMesh(const FaceShapeSet& set, std::span<const std::string_view> targetNames, std::vector<int>& map) {
    map.assign(targetNames.size(), -1);
    for (std::size_t t = 0; t < targetNames.size(); ++t) {
        const int s = set.find(targetNames[t]);
        map[t] = s < static_cast<int>(kMaxFaceShapes) ? s : -1;
    }
}

void FacePlayer::writeMeshWeights(std::span<const int> map, std::span<float> out) const {
    const std::size_t n = std::min(map.size(), out.size());
    for (std::size_t t = 0; t < n; ++t) out[t] = map[t] >= 0 ? out_[static_cast<std::size_t>(map[t])] : 0.f;
}

} // namespace ftcore
