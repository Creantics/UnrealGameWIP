#include "ftcore/FaceDirection.h"

#include "ftcore/Json.h"

#include "SeedMix.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

bool setError(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
    return false;
}

float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

// 0 at x = 0 and 1, 1 at x = 0.5; 0 outside.
float bump(float x) { return x > 0.f && x < 1.f ? 4.f * x * (1.f - x) : 0.f; }

bool isWarm(const FaceShapeSet& set, const EmotionDef* def) {
    return def && def->base >= 0 && def->base < static_cast<int>(set.expressions.size()) &&
           set.expressions[static_cast<std::size_t>(def->base)].name == "warm";
}

} // namespace

// ---- containment ------------------------------------------------------------------------------

const ContainmentProfile* ContainmentProfiles::find(std::string_view name) const {
    for (const ContainmentProfile& p : profiles)
        if (p.name == name) return &p;
    return nullptr;
}

bool importContainmentProfiles(std::string_view json, const FaceShapeSet& set, ContainmentProfiles& out,
                               std::string* error) {
    out = ContainmentProfiles{};
    const auto fail = [&](std::string msg) {
        out = ContainmentProfiles{};
        return setError(error, "containment_profiles.json: " + msg);
    };
    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) return fail(err);
    const JsonValue list = doc.root()["profiles"];
    if (!list.isArray()) return fail("profiles: expected an array");
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const JsonValue e = list[i];
        ContainmentProfile p;
        p.name = std::string(e["name"].string());
        if (p.name.empty()) return fail("profile without a name");
        if (out.find(p.name)) return fail("profile '" + p.name + "' is listed twice");
        const auto number = [&](std::string_view key, float& field) {
            const JsonValue v = e[key];
            if (v.isAbsent()) return true;
            if (!v.isNumber() || v.number() < 0.0) return false;
            field = static_cast<float>(v.number());
            return true;
        };
        for (const auto& [key, field] : {std::pair<std::string_view, float*>{"cap", &p.cap},
                                         {"blink_rate", &p.blinkRate},
                                         {"head_motion", &p.headMotion},
                                         {"micro_per_excess", &p.microPerExcess}})
            if (!number(key, *field))
                return fail("profile '" + p.name + "': " + std::string(key) + " is not a non-negative number");
        const JsonValue minor = e["minor"], add = e["add"];
        if (!minor.isAbsent() && !minor.isBool()) return fail("profile '" + p.name + "': minor is not a bool");
        if (minor.boolean(false)) p.maxIntensity = 4;
        if (!add.isAbsent() && !add.isObject()) return fail("profile '" + p.name + "': add must be an object");
        for (std::uint32_t k = 0; k < add.size(); ++k) {
            const std::string_view shape = add.key(k);
            const int s = set.find(shape);
            if (s < 0 || s >= static_cast<int>(kMaxFaceShapes))
                return fail("profile '" + p.name + "' names unknown shape '" + std::string(shape) + "'");
            if (!add.value(k).isNumber())
                return fail("profile '" + p.name + "': weight of '" + std::string(shape) + "' is not a number");
            p.added[static_cast<std::size_t>(s)] = static_cast<float>(add.value(k).number());
        }
        out.profiles.push_back(std::move(p));
    }
    return true;
}

bool resolveContainedEmotion(const FaceShapeSet& set, const EmotionMap& map, const ContainmentProfile* profile,
                             std::string_view primary, std::string_view secondary, int intensity, FacePreset& out,
                             float* excess) {
    int i = std::clamp(intensity, 1, 5);
    if (profile) i = std::min(i, std::max(1, profile->maxIntensity));
    const float cap = profile ? std::max(0.f, profile->cap) : 1.f;
    const bool ok = resolveEmotion(set, map, primary, secondary, i, cap, out);
    const float felt = 0.2f * static_cast<float>(i);
    if (profile) {
        for (std::size_t s = 0; s < kMaxFaceShapes; ++s) out.weights[s] = clamp01(out.weights[s] + profile->added[s] * felt);
    }
    if (excess) *excess = std::max(0.f, felt - cap);
    return ok;
}

// ---- director ---------------------------------------------------------------------------------

FaceDirector::FaceDirector(const FaceShapeSet& set, const EmotionMap& map, FacePlayer& player, std::uint32_t seed,
                           const FaceDirectionTuning& tuning)
    : set_(&set), map_(&map), player_(&player), tuning_(tuning), rng_(detail::mixSeed(seed)) {
    const auto shape = [&](std::string_view name) {
        const int s = set.find(name);
        return s < static_cast<int>(kMaxFaceShapes) ? s : -1;
    };
    browInner_ = shape("browInnerUp");
    browOuterLeft_ = shape("browOuterUpLeft");
    browOuterRight_ = shape("browOuterUpRight");
}

float FaceDirector::nextRandom() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (1.f / 16777216.f);
}

void FaceDirector::setProfile(const ContainmentProfile* profile) {
    profile_ = profile;
    player_->setBlinkRate(profile ? profile->blinkRate : 1.f);
}

float FaceDirector::headMotion() const { return profile_ ? profile_->headMotion : 1.f; }

void FaceDirector::scheduleFlash() { nextFlash_ = microRate_ > 0.f ? (0.5f + nextRandom()) / microRate_ : 0.f; }

void FaceDirector::startFlash() {
    if (hasFlashPose_ && flashTime_ < 0.f) flashTime_ = 0.f;
}

bool FaceDirector::speak(std::string_view primary, std::string_view secondary, int intensity,
                         std::span<const FaceCue> cues, float fade) {
    listening_ = false;
    const bool ok = resolveContainedEmotion(*set_, *map_, profile_, primary, secondary, intensity, pose_, &excess_);
    player_->setExpression(&pose_, 5.f, fade);

    const EmotionDef* p = primary.empty() ? nullptr : map_->find(primary);
    const EmotionDef* s = secondary.empty() ? nullptr : map_->find(secondary);
    lastPrimary_ = p;
    // The flash shows what leaks through: the secondary, or with none, the held back primary.
    flashPose_.fill(0.f);
    const EmotionDef* leak = s ? s : (excess_ > 0.f ? p : nullptr);
    hasFlashPose_ = leak != nullptr;
    if (leak) {
        addEmotionPose(*set_, *leak, 1.f, true, flashPose_);
        for (float& w : flashPose_) w = clamp01(w);
    }
    const float perExcess = profile_ ? profile_->microPerExcess : 0.f;
    microRate_ = hasFlashPose_ ? (s ? tuning_.microBaseRate : 0.f) + perExcess * excess_ : 0.f;
    flashTime_ = -1.f;
    scheduleFlash();

    cues_ = cues;
    cueNext_ = guardNext_ = 0;
    lineTime_ = 0.f;
    runCues(0.f);
    return ok;
}

void FaceDirector::runCues(float now) {
    // Blink guards run ahead of their emphasis word so that no blink is mid-close on it.
    while (guardNext_ < cues_.size()) {
        const FaceCue& c = cues_[guardNext_];
        if (c.kind == FaceCueKind::Emphasis) {
            if (now < c.time - tuning_.blinkGuard) break;
            player_->holdBlinks(c.time + std::max(0.f, c.duration) - now);
        }
        ++guardNext_;
    }
    while (cueNext_ < cues_.size() && cues_[cueNext_].time <= now) {
        const FaceCue& c = cues_[cueNext_++];
        const float late = now - c.time; // events start where they would have been at their time
        if (c.kind == FaceCueKind::Emphasis) {
            flickTime_ = late;
            nodTime_ = late;
            continue;
        }
        const bool longPause = c.kind == FaceCueKind::LongPause;
        const bool coin = roll(longPause ? tuning_.longPauseBlinkChance : tuning_.shortPauseBlinkChance);
        if (coin || (!longPause && player_->timeToNextBlink() <= tuning_.shortPausePull)) player_->requestBlink();
        if (hasFlashPose_ && roll(tuning_.pauseMicroChance)) {
            startFlash();
            if (flashTime_ >= 0.f && flashTime_ < late) flashTime_ = late;
        }
    }
}

void FaceDirector::listen(std::string_view speakerPrimary, int speakerIntensity) {
    if (!listening_) {
        listening_ = true;
        listenFrom_ = pose_.weights;
        decay_ = 1.f;
        mirror_.fill(0.f);
        mirrorFrom_.fill(0.f);
        mirrorEase_ = 0.f;
    }
    // The mirrored part eases from where it is now to the new speaker's.
    for (std::size_t i = 0; i < kMaxFaceShapes; ++i) mirrorFrom_[i] += (mirror_[i] - mirrorFrom_[i]) * mirrorEase_;
    mirrorEase_ = 0.f;
    mirror_.fill(0.f);
    const EmotionDef* def = speakerPrimary.empty() ? nullptr : map_->find(speakerPrimary);
    const bool mirrored = def && def->mirrors;
    if (mirrored) addEmotionPose(*set_, *def, tuning_.mirrorWeight * emotionIntensityWeight(speakerIntensity), false, mirror_);
    listenerWarm_ = isWarm(*set_, lastPrimary_) || (mirrored && isWarm(*set_, def));
    cues_ = {};
    cueNext_ = guardNext_ = 0;
    microRate_ = 0.f;
    hasFlashPose_ = false;
    tick(0.f);
}

bool FaceDirector::speakerPhrase() { return listening_ && roll(tuning_.glanceAwayChance); }

void FaceDirector::speakerPause(FaceCueKind kind) {
    if (listening_ && kind == FaceCueKind::LongPause && listenerWarm_) nodTime_ = 0.f;
}

void FaceDirector::tick(float dt) {
    dt = std::max(0.f, dt);
    if (flickTime_ >= 0.f) flickTime_ += dt;
    if (nodTime_ >= 0.f) nodTime_ += dt;
    if (flashTime_ >= 0.f) flashTime_ += dt;

    if (listening_) {
        decay_ *= std::exp2(-dt / std::max(1e-3f, tuning_.listenHalfLife));
        mirrorEase_ = tuning_.mirrorFade > 0.f ? std::min(1.f, mirrorEase_ + dt / tuning_.mirrorFade) : 1.f;
        for (std::size_t i = 0; i < kMaxFaceShapes; ++i)
            pose_.weights[i] = clamp01(listenFrom_[i] * decay_ + mirrorFrom_[i] + (mirror_[i] - mirrorFrom_[i]) * mirrorEase_);
        player_->setExpression(&pose_, 5.f, 0.f);
    } else {
        lineTime_ += dt;
        runCues(lineTime_);
        if (microRate_ > 0.f) {
            nextFlash_ -= dt;
            if (nextFlash_ <= 0.f) {
                startFlash();
                scheduleFlash();
            }
        }
    }

    micro_.fill(0.f);
    if (flickTime_ >= 0.f) {
        const float x = flickTime_ / std::max(1e-3f, tuning_.flickDuration);
        if (x >= 1.f) flickTime_ = -1.f;
        const float w = tuning_.flickWeight * bump(x);
        for (const int s : {browInner_, browOuterLeft_, browOuterRight_})
            if (s >= 0) micro_[static_cast<std::size_t>(s)] += w;
    }
    if (flashTime_ >= 0.f) {
        const float t = flashTime_, a = tuning_.microAttack, h = tuning_.microHold, r = tuning_.microRelease;
        float env = 0.f;
        if (t < a) env = t / a;
        else if (t < a + h) env = 1.f;
        else if (t < a + h + r) env = 1.f - (t - a - h) / r;
        else flashTime_ = -1.f;
        const float w = tuning_.microWeight * env;
        for (std::size_t s = 0; s < kMaxFaceShapes; ++s) micro_[s] += w * flashPose_[s];
    }
    for (float& w : micro_) w = clamp01(w);
    player_->setDirectedMicro(micro_);

    nod_ = 0.f;
    if (nodTime_ >= 0.f) {
        const float x = nodTime_ / std::max(1e-3f, tuning_.nodDuration);
        if (x >= 1.f) nodTime_ = -1.f;
        nod_ = bump(x) * headMotion();
    }
}

} // namespace ftcore
