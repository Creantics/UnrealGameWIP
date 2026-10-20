#include "ftcore/Cutscene.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace ftcore {

namespace {

constexpr float kCutPi = 3.14159265358979f;
// Push-in stops this far short of the look point.
constexpr float kPushStop = 0.3f;
// Over-the-shoulder: how far behind and beside the near subject the camera stands, and how high.
constexpr float kShoulderBack = 0.8f;
constexpr float kShoulderSide = 0.45f;
constexpr float kShoulderHeight = 1.65f;
constexpr float kShoulderLookHeight = 1.6f;

bool cutSetId(char* buffer, std::uint8_t& length, std::string_view id) {
    if (!isCutsceneId(id)) return false;
    for (std::size_t i = 0; i < id.size(); ++i) buffer[i] = id[i];
    buffer[id.size()] = '\0';
    length = static_cast<std::uint8_t>(id.size());
    return true;
}

float cutClamp01(float t) { return t < 0.f ? 0.f : (t > 1.f ? 1.f : t); }
float cutEase(float t) {
    const float c = cutClamp01(t);
    return c * c * (3.f - 2.f * c);
}

CutsceneVec3 cutAdd(const CutsceneVec3& a, const CutsceneVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
CutsceneVec3 cutSub(const CutsceneVec3& a, const CutsceneVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
CutsceneVec3 cutScale(const CutsceneVec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float cutLength(const CutsceneVec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

// The unit direction from `from` to `to` seen from above (z = 0); `fallback` when they coincide.
CutsceneVec3 cutFlatDir(const CutsceneVec3& from, const CutsceneVec3& to, const CutsceneVec3& fallback) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return fallback;
    return {dx / len, dy / len, 0.f};
}

// `p` turned about the vertical axis through `pivot` by `degrees`.
CutsceneVec3 cutTurn(const CutsceneVec3& p, const CutsceneVec3& pivot, float degrees) {
    const float a = degrees * kCutPi / 180.f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    const float dx = p.x - pivot.x;
    const float dy = p.y - pivot.y;
    return {pivot.x + dx * c - dy * s, pivot.y + dx * s + dy * c, p.z};
}

CameraPose cutFramed(const FramingRule& rule, const CutsceneVec3& spot, const CutsceneVec3& subject) {
    CameraPose pose;
    pose.lookAt = {subject.x, subject.y, subject.z + rule.lookHeight};
    const CutsceneVec3 d = cutFlatDir(subject, spot, {1.f, 0.f, 0.f});
    pose.position = {subject.x + d.x * rule.distance, subject.y + d.y * rule.distance, subject.z + rule.cameraHeight};
    return pose;
}

// ---- loader ----

std::string cutStr(std::string_view s) { return std::string(s); }

void cutUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                    std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + cutStr(key));
    }
}

bool cutInt(JsonValue j, const char* key, int& out, int lo, int hi, const std::string& path,
            std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
        errors.push_back(path + "." + key + ": expected an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
        return false;
    }
    out = static_cast<int>(v.integer());
    return true;
}

bool cutNum(JsonValue j, const char* key, float& out, float lo, float hi, const std::string& path,
            std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    const double d = v.number(-1e30);
    if (!v.isNumber() || !std::isfinite(d) || d < static_cast<double>(lo) || d > static_cast<double>(hi)) {
        errors.push_back(path + "." + key + ": expected a number in " + std::to_string(lo) + ".." + std::to_string(hi));
        return false;
    }
    out = static_cast<float>(d);
    return true;
}

bool cutFinish(std::vector<std::string>& errors, std::string* error) {
    if (errors.empty()) return true;
    if (error) {
        error->clear();
        for (const std::string& e : errors) {
            if (!error->empty()) *error += '\n';
            *error += e;
        }
    }
    return false;
}

bool cutFramingFromName(std::string_view name, ShotFraming& out) {
    for (const ShotFraming f : {ShotFraming::Wide, ShotFraming::Medium, ShotFraming::Close, ShotFraming::OverShoulder,
                                ShotFraming::TwoShot, ShotFraming::Insert}) {
        if (shotFramingName(f) == name) {
            out = f;
            return true;
        }
    }
    return false;
}

bool cutMoveFromName(std::string_view name, CameraMove& out) {
    for (const CameraMove m : {CameraMove::Static, CameraMove::PushIn, CameraMove::PullOut, CameraMove::Pan, CameraMove::Orbit}) {
        if (cameraMoveName(m) == name) {
            out = m;
            return true;
        }
    }
    return false;
}

void cutIdKey(JsonValue j, const char* key, bool required, const std::string& path, std::vector<std::string>& errors,
              CutsceneShot& shot, bool (CutsceneShot::*set)(std::string_view)) {
    const JsonValue v = j[key];
    if (v.isAbsent()) {
        if (required) errors.push_back(path + "." + key + ": expected an id");
        return;
    }
    if (!v.isString() || !(shot.*set)(v.string()))
        errors.push_back(path + "." + key + ": expected lower-case letters, digits and _ (at most 15)");
}

void cutShot(JsonValue j, const std::string& path, CutscenePlan& plan, std::vector<std::string>& errors,
             std::vector<std::string>* warnings) {
    if (!j.isObject()) {
        errors.push_back(path + ": expected an object");
        return;
    }
    cutUnknownKeys(j, path,
                   {"spot", "subject", "subject2", "framing", "duration", "move", "amount", "transition", "blend"},
                   warnings);
    CutsceneShot shot;
    cutIdKey(j, "spot", true, path, errors, shot, &CutsceneShot::setSpot);
    cutIdKey(j, "subject", true, path, errors, shot, &CutsceneShot::setSubject);
    cutIdKey(j, "subject2", false, path, errors, shot, &CutsceneShot::setSubject2);
    if (j.has("framing") && !cutFramingFromName(j["framing"].string(), shot.framing))
        errors.push_back(path + ".framing: expected wide, medium, close, over_shoulder, two_shot or insert");
    if ((shot.framing == ShotFraming::TwoShot || shot.framing == ShotFraming::OverShoulder) && !j.has("subject2"))
        errors.push_back(path + ".subject2: " + cutStr(shotFramingName(shot.framing)) + " needs a second subject");
    cutNum(j, "duration", shot.duration, CutsceneShot::kMinDuration, 60.f, path, errors);
    if (j.has("move") && !cutMoveFromName(j["move"].string(), shot.move))
        errors.push_back(path + ".move: expected static, push_in, pull_out, pan or orbit");
    switch (shot.move) {
    case CameraMove::Static: break;
    case CameraMove::PushIn:
    case CameraMove::PullOut:
        shot.moveAmount = 0.5f;
        cutNum(j, "amount", shot.moveAmount, 0.f, 20.f, path, errors);
        break;
    case CameraMove::Pan:
        shot.moveAmount = 20.f;
        cutNum(j, "amount", shot.moveAmount, -360.f, 360.f, path, errors);
        break;
    case CameraMove::Orbit:
        shot.moveAmount = 30.f;
        cutNum(j, "amount", shot.moveAmount, -360.f, 360.f, path, errors);
        break;
    }
    const std::string_view transition = j["transition"].string("cut");
    if (transition == "cut") {
        shot.transition = ShotTransition::Cut;
    } else if (transition == "blend") {
        shot.transition = ShotTransition::Blend;
        shot.blendTime = std::min(0.5f, shot.duration);
        cutNum(j, "blend", shot.blendTime, 0.f, shot.duration, path, errors);
    } else {
        errors.push_back(path + ".transition: expected cut or blend");
    }
    if (!plan.add(shot))
        errors.push_back(path + ": more than " + std::to_string(CutscenePlan::kMaxShots) + " shots");
}

void cutShots(JsonValue list, const std::string& path, CutscenePlan& plan, std::vector<std::string>& errors,
              std::vector<std::string>* warnings) {
    if (!list.isArray() || list.size() == 0) {
        errors.push_back(path + ": expected a non-empty array of shots");
        return;
    }
    if (list.size() > CutscenePlan::kMaxShots) {
        errors.push_back(path + ": more than " + std::to_string(CutscenePlan::kMaxShots) + " shots");
        return;
    }
    for (std::uint32_t i = 0; i < list.size(); ++i) cutShot(list[i], path + "[" + std::to_string(i) + "]", plan, errors, warnings);
}

} // namespace

std::string_view shotFramingName(ShotFraming framing) {
    switch (framing) {
    case ShotFraming::Wide: return "wide";
    case ShotFraming::Medium: return "medium";
    case ShotFraming::Close: return "close";
    case ShotFraming::OverShoulder: return "over_shoulder";
    case ShotFraming::TwoShot: return "two_shot";
    case ShotFraming::Insert: return "insert";
    }
    return "medium";
}

std::string_view cameraMoveName(CameraMove move) {
    switch (move) {
    case CameraMove::Static: return "static";
    case CameraMove::PushIn: return "push_in";
    case CameraMove::PullOut: return "pull_out";
    case CameraMove::Pan: return "pan";
    case CameraMove::Orbit: return "orbit";
    }
    return "static";
}

std::string_view shotTransitionName(ShotTransition transition) {
    return transition == ShotTransition::Blend ? "blend" : "cut";
}

bool isCutsceneId(std::string_view id) {
    if (id.empty() || id.size() >= CutsceneShot::kIdCapacity) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool CutsceneShot::setSpot(std::string_view id) { return cutSetId(spotId, spotLength, id); }
bool CutsceneShot::setSubject(std::string_view id) { return cutSetId(subjectId, subjectLength, id); }
bool CutsceneShot::setSubject2(std::string_view id) {
    if (id.empty()) {
        subject2Id[0] = '\0';
        subject2Length = 0;
        return true;
    }
    return cutSetId(subject2Id, subject2Length, id);
}

// ---- plan ----

bool CutscenePlan::add(const CutsceneShot& shot) {
    if (count_ >= kMaxShots) return false;
    CutsceneShot& s = shots_[count_++];
    s = shot;
    if (!(s.duration >= CutsceneShot::kMinDuration)) s.duration = CutsceneShot::kMinDuration; // NaN too
    if (s.transition == ShotTransition::Cut || !(s.blendTime > 0.f)) s.blendTime = 0.f;
    if (s.blendTime > s.duration) s.blendTime = s.duration;
    return true;
}

float CutscenePlan::totalDuration() const {
    float total = 0.f;
    for (std::uint32_t i = 0; i < count_; ++i) total += shots_[i].duration;
    return total;
}

// ---- player ----

void CutscenePlayer::begin(const CutscenePlan& plan, bool autoPlay) {
    plan_ = plan;
    index_ = 0;
    shotTime_ = 0.0;
    elapsed_ = 0.0;
    realElapsed_ = 0.0;
    autoPlay_ = autoPlay;
    skipped_ = false;
    phase_ = plan_.empty() ? CutscenePhase::Done : CutscenePhase::Playing;
}

void CutscenePlayer::update(double gameSeconds) {
    if (phase_ != CutscenePhase::Playing || !(gameSeconds > 0.0)) return;
    elapsed_ += gameSeconds;
    shotTime_ += gameSeconds;
    while (shotTime_ >= static_cast<double>(plan_.shot(index_).duration)) {
        if (index_ + 1 >= plan_.size()) {
            shotTime_ = static_cast<double>(plan_.shot(index_).duration);
            phase_ = CutscenePhase::Done;
            return;
        }
        shotTime_ -= static_cast<double>(plan_.shot(index_).duration);
        ++index_;
    }
}

void CutscenePlayer::tickReal(double realSeconds) {
    if (phase_ != CutscenePhase::Playing || !(realSeconds > 0.0)) return;
    realElapsed_ += realSeconds;
    if (autoPlay_ && realElapsed_ >= kAutoPlayRealLimit) skip();
}

bool CutscenePlayer::skip() {
    if (phase_ != CutscenePhase::Playing) return false;
    phase_ = CutscenePhase::Done;
    skipped_ = true;
    return true;
}

bool CutscenePlayer::press() {
    if (phase_ != CutscenePhase::Playing || realElapsed_ < kInputGrace) return false;
    return skip();
}

float CutscenePlayer::shotProgress() const {
    if (plan_.empty() || phase_ == CutscenePhase::Done) return 1.f;
    if (phase_ != CutscenePhase::Playing) return 0.f;
    const double t = shotTime_ / static_cast<double>(plan_.shot(index_).duration);
    return cutClamp01(static_cast<float>(t));
}

bool CutscenePlayer::blending() const {
    if (phase_ != CutscenePhase::Playing || index_ == 0) return false;
    const CutsceneShot& s = plan_.shot(index_);
    return s.transition == ShotTransition::Blend && s.blendTime > 0.f && shotTime_ < static_cast<double>(s.blendTime);
}

float CutscenePlayer::blendWeight() const {
    if (!blending()) return 1.f;
    const double t = shotTime_ / static_cast<double>(plan_.shot(index_).blendTime);
    return cutEase(static_cast<float>(t));
}

// ---- camera ----

FramingRule framingRule(ShotFraming framing) {
    switch (framing) {
    case ShotFraming::Wide: return {8.f, 1.0f, 1.7f}; // distance and height used only when the spot sits on the subject
    case ShotFraming::Medium: return {3.f, 1.45f, 1.55f};
    case ShotFraming::Close: return {1.3f, 1.6f, 1.62f};
    case ShotFraming::OverShoulder: return {kShoulderBack, kShoulderLookHeight, kShoulderHeight};
    case ShotFraming::TwoShot: return {3.f, 1.45f, 1.55f};
    case ShotFraming::Insert: return {0.8f, 1.0f, 1.3f};
    }
    return {3.f, 1.45f, 1.55f};
}

CameraPose cutsceneCamera(const CutsceneShot& shot, float progress, const CutsceneVec3& spot, const CutsceneVec3& subject,
                          const CutsceneVec3* subject2) {
    ShotFraming framing = shot.framing;
    if ((framing == ShotFraming::TwoShot || framing == ShotFraming::OverShoulder) && !subject2) framing = ShotFraming::Medium;
    const FramingRule rule = framingRule(framing);

    CameraPose pose;
    switch (framing) {
    case ShotFraming::Wide: {
        pose.lookAt = {subject.x, subject.y, subject.z + rule.lookHeight};
        pose.position = spot;
        if (cutLength(cutSub(pose.position, pose.lookAt)) < 1e-3f) pose = cutFramed(rule, spot, subject);
        break;
    }
    case ShotFraming::TwoShot: {
        const CutsceneVec3 mid = cutScale(cutAdd(subject, *subject2), 0.5f);
        const float dx = subject2->x - subject.x;
        const float dy = subject2->y - subject.y;
        const float separation = std::sqrt(dx * dx + dy * dy);
        const FramingRule wide = {std::max(rule.distance, separation * 1.1f + 1.5f), rule.lookHeight, rule.cameraHeight};
        pose = cutFramed(wide, spot, mid);
        break;
    }
    case ShotFraming::OverShoulder: {
        const CutsceneVec3 toward = cutFlatDir(subject, spot, {1.f, 0.f, 0.f});
        const CutsceneVec3 back = cutFlatDir(subject, *subject2, toward); // from the subject past the near one
        CutsceneVec3 side = {-back.y, back.x, 0.f};
        if (side.x * (spot.x - subject2->x) + side.y * (spot.y - subject2->y) < 0.f) side = cutScale(side, -1.f);
        pose.lookAt = {subject.x, subject.y, subject.z + rule.lookHeight};
        pose.position = cutAdd(*subject2, cutAdd(cutScale(back, rule.distance), cutScale(side, kShoulderSide)));
        pose.position.z = subject2->z + rule.cameraHeight;
        break;
    }
    case ShotFraming::Medium:
    case ShotFraming::Close:
    case ShotFraming::Insert: pose = cutFramed(rule, spot, subject); break;
    }

    const float e = cutEase(progress);
    switch (shot.move) {
    case CameraMove::Static: break;
    case CameraMove::PushIn:
    case CameraMove::PullOut: {
        const CutsceneVec3 forward = cutSub(pose.lookAt, pose.position);
        const float len = cutLength(forward);
        if (len < 1e-4f) break;
        float travel = std::max(0.f, shot.moveAmount) * e;
        if (shot.move == CameraMove::PushIn) travel = std::min(travel, std::max(0.f, len - kPushStop));
        else travel = -travel;
        pose.position = cutAdd(pose.position, cutScale(forward, travel / len));
        break;
    }
    case CameraMove::Pan: pose.lookAt = cutTurn(pose.lookAt, pose.position, (e - 0.5f) * shot.moveAmount); break;
    case CameraMove::Orbit: pose.position = cutTurn(pose.position, pose.lookAt, e * shot.moveAmount); break;
    }
    return pose;
}

CameraPose lerpPose(const CameraPose& from, const CameraPose& to, float t) {
    const float c = cutClamp01(t);
    CameraPose pose;
    pose.position = cutAdd(from.position, cutScale(cutSub(to.position, from.position), c));
    pose.lookAt = cutAdd(from.lookAt, cutScale(cutSub(to.lookAt, from.lookAt), c));
    return pose;
}

// ---- table ----

const CutsceneEntry* CutsceneTable::findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    for (const CutsceneEntry& e : entries)
        if (e.beat == beat && e.step == step && e.scene == scene) return &e;
    return nullptr;
}

const CutscenePlan& CutsceneTable::find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    const CutsceneEntry* e = findEntry(scene, beat, step);
    return e ? e->plan : fallback;
}

bool importCutsceneTable(std::string_view json, CutsceneTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = CutsceneTable{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("cutscenes file: " + parseError);
        return cutFinish(errors, error);
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("cutscenes file: expected an object");
        return cutFinish(errors, error);
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    cutUnknownKeys(root, "", {"version", "default", "cutscenes"}, warnings);

    CutsceneTable table;
    const JsonValue def = root["default"];
    if (!def.isObject()) {
        errors.push_back("default: expected an object");
    } else {
        cutUnknownKeys(def, "default", {"shots"}, warnings);
        cutShots(def["shots"], "default.shots", table.fallback, errors, warnings);
    }
    const JsonValue list = root["cutscenes"];
    if (!list.isAbsent() && !list.isArray()) errors.push_back("cutscenes: expected an array");
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "cutscenes[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        cutUnknownKeys(j, path, {"scene", "beat", "step", "shots"}, warnings);
        CutsceneEntry e;
        e.scene = cutStr(j["scene"].string());
        if (e.scene.empty()) errors.push_back(path + ".scene: expected a scene id");
        int beat = -1, step = 0;
        if (!j.has("beat")) errors.push_back(path + ".beat: expected an integer in 0..1000000");
        cutInt(j, "beat", beat, 0, 1000000, path, errors);
        cutInt(j, "step", step, 0, 1000, path, errors);
        e.beat = beat < 0 ? 0u : static_cast<std::uint32_t>(beat);
        e.step = static_cast<std::uint32_t>(step);
        cutShots(j["shots"], path + ".shots", e.plan, errors, warnings);
        if (beat >= 0 && !e.scene.empty() && table.findEntry(e.scene, e.beat, e.step))
            errors.push_back(path + ": duplicate entry for " + e.scene + " beat " + std::to_string(e.beat) + " step " +
                             std::to_string(e.step));
        table.entries.push_back(std::move(e));
    }
    if (!cutFinish(errors, error)) return false;
    out = std::move(table);
    return true;
}

} // namespace ftcore
