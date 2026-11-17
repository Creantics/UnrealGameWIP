#include "ftcore/SpecialCine.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

namespace ftcore {

namespace {

constexpr float kTouch = 0.001f;      // a shot's in matches the last one's out within this (seconds)
constexpr float kMinShot = 0.1f;      // seconds
constexpr float kMaxLength = 20.f;    // seconds
constexpr float kMaxBlendIn = 1.f;    // seconds
constexpr float kSensorWidth = 36.f;  // mm
constexpr float kHumanHeight = 1.8f;  // a target's z offsets are written for this height
constexpr float kPi = 3.14159265358979f;

bool finite3(const CutsceneVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float ease01(SpecialCineEase ease, float x) {
    const float c = std::isfinite(x) ? std::clamp(x, 0.f, 1.f) : 0.f;
    return ease == SpecialCineEase::Linear ? c : c * c * (3.f - 2.f * c);
}

bool isIdText(std::string_view id, std::size_t most, bool upper) {
    if (id.empty() || id.size() > most) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (upper && c >= 'A' && c <= 'Z'))) return false;
    return true;
}

template <std::size_t N>
bool setText(char (&buffer)[N], std::uint8_t& length, std::string_view text, bool upper) {
    if (!isIdText(text, N - 1, upper)) return false;
    std::fill(std::begin(buffer), std::end(buffer), '\0');
    std::copy(text.begin(), text.end(), buffer);
    length = static_cast<std::uint8_t>(text.size());
    return true;
}

bool lensOk(float mm) { return std::isfinite(mm) && mm >= 10.f && mm <= 300.f; }
bool focusOk(float m) { return std::isfinite(m) && m >= 0.f && m <= 100.f; }
bool fStopOk(float f) { return std::isfinite(f) && f >= 0.7f && f <= 32.f; }

bool shotValuesOk(const SpecialCineShot& s) {
    return std::isfinite(s.in) && std::isfinite(s.out) && finite3(s.cameraFrom) && finite3(s.cameraTo) && finite3(s.lookFrom) &&
           finite3(s.lookTo) && lensOk(s.lensFrom) && lensOk(s.lensTo) && focusOk(s.focusFrom) && focusOk(s.focusTo) && fStopOk(s.fStop);
}

CutsceneVec3 sub3(const CutsceneVec3& a, const CutsceneVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
CutsceneVec3 lerp3(const CutsceneVec3& a, const CutsceneVec3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
float length3(const CutsceneVec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// The stage frame's x and y on the ground (unit), from its forward.
void stageAxes(const SpecialCineStage& stage, float& fx, float& fy) {
    fx = stage.forward.x;
    fy = stage.forward.y;
    const float n = std::sqrt(fx * fx + fy * fy);
    if (!(n > 1e-4f) || !std::isfinite(n)) {
        fx = 1.f;
        fy = 0.f;
        return;
    }
    fx /= n;
    fy /= n;
}

} // namespace

std::string_view specialCineAnchorName(SpecialCineAnchor anchor) {
    switch (anchor) {
    case SpecialCineAnchor::Lead: return "lead";
    case SpecialCineAnchor::World: return "world";
    case SpecialCineAnchor::Target: return "target";
    case SpecialCineAnchor::Bone: return "bone";
    }
    return "lead";
}

// ---- the move ----

std::uint32_t SpecialCineMove::shotAt(float t) const {
    if (count_ == 0) return 0;
    std::uint32_t i = 0;
    while (i + 1 < count_ && shots_[i + 1].in <= t) ++i;
    return i;
}

float SpecialCineMove::blendOut() const {
    if (count_ == 0) return 0.f;
    const float last = shots_[count_ - 1].out - shots_[count_ - 1].in;
    return blendOut_ < 0.f ? last : std::min(blendOut_, length());
}

float SpecialCineMove::release() const {
    if (release_ >= 0.f) return std::min(release_, length());
    for (std::uint32_t i = 0; i < count_; ++i)
        if (shots_[i].shake) return shots_[i].in;
    return count_ > 0 ? shots_[count_ - 1].in : 0.f;
}

bool SpecialCineMove::setId(std::string_view id) { return setText(id_, idLength_, id, false); }
bool SpecialCineMove::setShotFile(std::string_view file) { return setText(file_, fileLength_, file, true); }
bool SpecialCineMove::setClip(std::string_view clip) { return setText(clip_, clipLength_, clip, true); }

bool SpecialCineMove::add(const SpecialCineShot& shot) {
    if (count_ >= kMaxShots || !shotValuesOk(shot)) return false;
    const float expect = count_ == 0 ? 0.f : shots_[count_ - 1].out;
    if (std::fabs(shot.in - expect) > kTouch || shot.out - shot.in < kMinShot || shot.out > kMaxLength) return false;
    shots_[count_] = shot;
    shots_[count_].in = expect; // touching exactly
    ++count_;
    return true;
}

void SpecialCineMove::clearShots() {
    count_ = 0;
    clipLength_ = 0;
    std::fill(std::begin(clip_), std::end(clip_), '\0');
    shake_ = {};
    endFeet_ = {};
}

void SpecialCineMove::setBlends(float inSeconds, float outSeconds) {
    blendIn_ = std::isfinite(inSeconds) && inSeconds > 0.f ? std::min(inSeconds, kMaxBlendIn) : 0.f;
    blendOut_ = std::isfinite(outSeconds) && outSeconds >= 0.f ? outSeconds : -1.f;
}

void SpecialCineMove::setRelease(float seconds) { release_ = std::isfinite(seconds) && seconds >= 0.f ? seconds : -1.f; }

void SpecialCineMove::setFStop(float fStop) { fStop_ = fStopOk(fStop) ? fStop : kDefaultFStop; }

void SpecialCineMove::setShake(const SpecialCineShake& shake) {
    shake_ = shake;
    if (!finite3(shake_.amplitude) || !finite3(shake_.hz)) shake_ = {};
    if (!(std::isfinite(shake_.decay) && shake_.decay > 0.001f)) shake_.decay = 0.2f;
}

void SpecialCineMove::setEndFeet(const CutsceneVec3& feet) { endFeet_ = finite3(feet) ? feet : CutsceneVec3{}; }

void SpecialCineMove::clear() {
    clearShots();
    idLength_ = 0;
    std::fill(std::begin(id_), std::end(id_), '\0');
    fileLength_ = 0;
    std::fill(std::begin(file_), std::end(file_), '\0');
    blendIn_ = 0.f;
    blendOut_ = -1.f;
    release_ = -1.f;
    fStop_ = kDefaultFStop;
}

const SpecialCineMove* SpecialCineTable::find(std::string_view id) const {
    for (std::uint32_t i = 0; i < count_; ++i)
        if (moves_[i].id() == id) return &moves_[i];
    return nullptr;
}

bool SpecialCineTable::add(const SpecialCineMove& move) {
    if (count_ >= kMaxMoves || move.id().empty() || find(move.id()) != nullptr) return false;
    moves_[count_++] = move;
    return true;
}

// ---- the camera ----

float specialCineFov(float lensMm) {
    const float mm = std::isfinite(lensMm) ? std::max(lensMm, 10.f) : 35.f;
    return 2.f * std::atan(0.5f * kSensorWidth / mm) * 180.f / kPi;
}

CutsceneVec3 specialCinePoint(const SpecialCineStage& stage, SpecialCineAnchor anchor, const CutsceneVec3& at) {
    float fx = 1.f, fy = 0.f;
    stageAxes(stage, fx, fy);
    const float rx = -fy, ry = fx; // right of forward, Unreal's handedness
    float ax = at.x, ay = at.y, az = at.z;
    CutsceneVec3 origin = stage.origin;
    if (anchor == SpecialCineAnchor::Target) {
        origin = stage.target;
        const float r = std::isfinite(stage.targetRadius) ? std::max(stage.targetRadius, 0.f) : 0.f;
        const float h = std::sqrt(ax * ax + ay * ay);
        if (r > 0.f && h > 1e-4f) {
            ax += ax / h * r;
            ay += ay / h * r;
        }
        const float height = std::isfinite(stage.targetHeight) && stage.targetHeight > 0.1f ? stage.targetHeight : kHumanHeight;
        az *= height / kHumanHeight;
    } else if (anchor == SpecialCineAnchor::Bone) {
        origin = stage.bone;
    }
    return {origin.x + ax * fx + ay * rx, origin.y + ax * fy + ay * ry, origin.z + az};
}

SpecialCineFrame specialCineFrameAt(const SpecialCineMove& move, float t, const SpecialCineStage& stage,
                                    const SpecialCineShotFix* fixes) {
    SpecialCineFrame f;
    if (move.empty()) return f;
    const float length = move.length();
    const float tc = std::isfinite(t) ? std::clamp(t, 0.f, length) : 0.f;
    const std::uint32_t i = move.shotAt(tc);
    const SpecialCineShot& s = move.shot(i);
    const float p = ease01(s.ease, (tc - s.in) / std::max(s.out - s.in, kMinShot));
    f.shot = i;
    const SpecialCineShotFix fix = fixes != nullptr ? fixes[i] : SpecialCineShotFix{};
    CutsceneVec3 eye = lerp3(s.cameraFrom, s.cameraTo, p);
    CutsceneVec3 look = lerp3(s.lookFrom, s.lookTo, p);
    if (fix.mirror) {
        eye.y = -eye.y;
        look.y = -look.y;
    }
    f.pose.position = specialCinePoint(stage, s.cameraAnchor, eye);
    f.pose.lookAt = specialCinePoint(stage, s.lookAnchor, look);
    if (std::isfinite(fix.raise) && fix.raise > 0.f) f.pose.position.z += fix.raise;
    float pulled = 0.f;
    if (std::isfinite(fix.pull) && fix.pull > 0.f) {
        const CutsceneVec3 axis = sub3(f.pose.lookAt, f.pose.position);
        const float d = length3(axis);
        pulled = std::min(fix.pull, std::max(d - SpecialCineShotFix::kMinPullGap, 0.f));
        if (d > 1e-4f && pulled > 0.f) {
            const float k = pulled / d;
            f.pose.position = {f.pose.position.x + axis.x * k, f.pose.position.y + axis.y * k, f.pose.position.z + axis.z * k};
        }
    }
    f.fovDegrees = specialCineFov(s.lensFrom + (s.lensTo - s.lensFrom) * p);
    const float focus = s.focusFrom + (s.focusTo - s.focusFrom) * p - pulled;
    f.focus = focus > 0.f ? focus : std::max(length3(sub3(f.pose.lookAt, f.pose.position)), 0.1f);
    f.fStop = s.fStop > 0.f ? s.fStop : move.fStop();
    if (s.shake) {
        const SpecialCineShake& k = move.shake();
        const float u = tc - s.in;
        const float fall = std::exp(-u / k.decay);
        const float w = 2.f * kPi * u;
        // Fixed phases per axis, so the three do not start together.
        const float sx = k.amplitude.x * fall * std::sin(w * k.hz.x + 0.3f);
        const float sy = k.amplitude.y * fall * std::sin(w * k.hz.y + 1.7f);
        const float sz = k.amplitude.z * fall * std::sin(w * k.hz.z + 2.9f);
        float fx = 1.f, fy = 0.f;
        stageAxes(stage, fx, fy);
        f.shake = {sx * fx - sy * fy, sx * fy + sy * fx, sz};
    }
    // The blends: from the play camera over the blend in; into it over the blend out, eased by the last shot's ease.
    const float bin = std::min(move.blendIn(), 0.5f * length);
    const float bout = move.blendOut();
    f.playWeight = 0.f;
    if (bin > 0.f && tc < bin) f.playWeight = 1.f - ease01(SpecialCineEase::Smooth, tc / bin);
    if (bout > 0.f && tc > length - bout)
        f.playWeight = ease01(move.shot(move.size() - 1).ease, (tc - (length - bout)) / bout);
    return f;
}

// ---- the player ----

void SpecialCinePlayer::begin(const SpecialCineMove* move) {
    move_ = move != nullptr && !move->empty() ? move : nullptr;
    time_ = 0.f;
    framed_ = false;
    lastShot_ = 0;
    finished_ = false;
    releaseTaken_ = false;
    std::fill(std::begin(fixes_), std::end(fixes_), SpecialCineShotFix{});
}

bool SpecialCinePlayer::setFix(std::uint32_t shot, const SpecialCineShotFix& fix) {
    if (move_ == nullptr || shot >= move_->size()) return false;
    fixes_[shot] = fix;
    return true;
}

void SpecialCinePlayer::step(float seconds) {
    if (!active()) return;
    const float dt = std::isfinite(seconds) && seconds > 0.f ? seconds : 0.f;
    time_ = std::min(time_ + dt, move_->length());
    if (time_ >= move_->length()) finished_ = true;
}

void SpecialCinePlayer::stop() {
    if (move_ != nullptr) finished_ = true;
}

SpecialCineFrame SpecialCinePlayer::frame(const SpecialCineStage& stage) {
    if (move_ == nullptr) return {};
    SpecialCineFrame f = specialCineFrameAt(*move_, time_, stage, fixes_);
    f.cut = framed_ ? f.shot != lastShot_ : move_->blendIn() <= 0.f;
    if (!releaseTaken_ && time_ >= move_->release()) {
        f.release = true;
        releaseTaken_ = true;
    }
    lastShot_ = f.shot;
    framed_ = true;
    return f;
}

// ---- the files ----

namespace {

bool readVec(const JsonValue& v, CutsceneVec3& out) {
    if (!v.isArray() || v.size() != 3 || !v[0].isNumber() || !v[1].isNumber() || !v[2].isNumber()) return false;
    out = {static_cast<float>(v[0].number()), static_cast<float>(v[1].number()), static_cast<float>(v[2].number())};
    return finite3(out);
}

// { "from": [..], "to": [..] } ("to" defaults to "from").
bool readSpan(const JsonValue& v, CutsceneVec3& from, CutsceneVec3& to) {
    if (!v.isObject() || !readVec(v["from"], from)) return false;
    if (v["to"].isAbsent()) {
        to = from;
        return true;
    }
    return readVec(v["to"], to);
}

// A number, or { "from": n, "to": n } ("to" defaults to "from"); absent: the fallback.
bool readScalarSpan(const JsonValue& v, float fallback, float& from, float& to) {
    if (v.isAbsent()) {
        from = to = fallback;
        return true;
    }
    if (v.isNumber()) {
        from = to = static_cast<float>(v.number());
        return true;
    }
    if (!v.isObject() || !v["from"].isNumber()) return false;
    from = static_cast<float>(v["from"].number());
    to = v["to"].isAbsent() ? from : static_cast<float>(v["to"].number(std::nan("")));
    return v["to"].isAbsent() || v["to"].isNumber();
}

bool readAnchor(const JsonValue& v, SpecialCineAnchor& out) {
    if (v.isAbsent()) {
        out = SpecialCineAnchor::Lead;
        return true;
    }
    const std::string_view s = v.string();
    if (s == "lead") out = SpecialCineAnchor::Lead;
    else if (s == "world") out = SpecialCineAnchor::World;
    else if (s == "target") out = SpecialCineAnchor::Target;
    else if (s == "bone") out = SpecialCineAnchor::Bone;
    else return false;
    return true;
}

void warnUnknown(const JsonValue& obj, std::initializer_list<std::string_view> known, const std::string& where,
                 std::vector<std::string>* warnings) {
    if (warnings == nullptr) return;
    for (std::uint32_t i = 0; i < obj.size(); ++i) {
        const std::string_view k = obj.key(i);
        if (std::find(known.begin(), known.end(), k) == known.end()) warnings->push_back(where + ": unknown key \"" + std::string(k) + "\"");
    }
}

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
}

} // namespace

static bool readSpecialCineTable(std::string_view text, SpecialCineTable& table, std::string* error, std::vector<std::string>* warnings) {
    table.clear();
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(text, &parseError)) return fail(error, "specials: " + parseError);
    const JsonValue root = doc.root();
    if (!root.isObject()) return fail(error, "specials: the root is not an object");
    warnUnknown(root, {"version", "moves"}, "specials", warnings);
    if (root["version"].integer(1) != 1) return fail(error, "specials: version must be 1");
    const JsonValue moves = root["moves"];
    if (!moves.isArray()) return fail(error, "specials: \"moves\" is not an array");
    for (std::uint32_t m = 0; m < moves.size(); ++m) {
        const JsonValue mv = moves[m];
        const std::string where = "specials: move " + std::to_string(m);
        if (!mv.isObject()) return fail(error, where + " is not an object");
        warnUnknown(mv, {"id", "shots", "release", "fstop", "blend_in", "blend_out"}, where, warnings);
        SpecialCineMove move;
        if (!move.setId(mv["id"].string())) return fail(error, where + ": \"id\" is not an id (a-z, 0-9, _; 1..23)");
        if (!move.setShotFile(mv["shots"].string())) return fail(error, where + ": \"shots\" is not a file name (a-z, A-Z, 0-9, _; 1..39)");
        const float rel = static_cast<float>(mv["release"].number(-1.0));
        if (!mv["release"].isAbsent() && !(mv["release"].isNumber() && rel >= 0.f && rel <= kMaxLength))
            return fail(error, where + ": \"release\" is not a time in 0..20 s");
        move.setRelease(rel);
        const float stop = static_cast<float>(mv["fstop"].number(SpecialCineMove::kDefaultFStop));
        if (!fStopOk(stop)) return fail(error, where + ": \"fstop\" is outside 0.7..32");
        move.setFStop(stop);
        const float bin = static_cast<float>(mv["blend_in"].number(0.0));
        const float bout = static_cast<float>(mv["blend_out"].number(-1.0));
        if (!(bin >= 0.f && bin <= kMaxBlendIn)) return fail(error, where + ": \"blend_in\" is outside 0..1 s");
        if (!mv["blend_out"].isAbsent() && !(bout >= 0.f && bout <= kMaxLength)) return fail(error, where + ": \"blend_out\" is negative");
        move.setBlends(bin, bout);
        if (!table.add(move)) return fail(error, where + ": a duplicate id, or more than 8 moves");
    }
    return true;
}

static bool readSpecialCineShots(std::string_view text, SpecialCineMove& move, std::string* error, std::vector<std::string>* warnings) {
    move.clearShots();
    const std::string where = "special " + std::string(move.id()) + " shots";
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(text, &parseError)) return fail(error, where + ": " + parseError);
    const JsonValue root = doc.root();
    if (!root.isObject()) return fail(error, where + ": the root is not an object");
    warnUnknown(root, {"version", "clip", "duration_s", "fps", "frame", "sensor_width_mm", "aspect", "shake", "end_feet", "cuts", "shots"},
                where, warnings);
    if (root["version"].integer(1) != 1) return fail(error, where + ": version must be 1");
    if (!root["clip"].isAbsent() && !move.setClip(root["clip"].string()))
        return fail(error, where + ": \"clip\" is not an asset name (a-z, A-Z, 0-9, _; 1..39)");
    const float sensor = static_cast<float>(root["sensor_width_mm"].number(kSensorWidth));
    if (std::fabs(sensor - kSensorWidth) > 0.01f) return fail(error, where + ": \"sensor_width_mm\" must be 36");

    const JsonValue shake = root["shake"];
    if (!shake.isAbsent()) {
        if (!shake.isObject()) return fail(error, where + ": \"shake\" is not an object");
        warnUnknown(shake, {"profile", "amp_m", "hz", "decay_s"}, where + " shake", warnings);
        if (!shake["profile"].isAbsent() && shake["profile"].string() != "decaying")
            return fail(error, where + ": shake \"profile\" must be \"decaying\"");
        SpecialCineShake k;
        if (!readVec(shake["amp_m"], k.amplitude) || !readVec(shake["hz"], k.hz)) return fail(error, where + ": shake \"amp_m\" and \"hz\" are [x, y, z]");
        k.decay = static_cast<float>(shake["decay_s"].number(0.2));
        const bool ampOk = k.amplitude.x >= 0.f && k.amplitude.y >= 0.f && k.amplitude.z >= 0.f && k.amplitude.x <= 0.5f &&
                           k.amplitude.y <= 0.5f && k.amplitude.z <= 0.5f;
        const bool hzOk = k.hz.x >= 0.f && k.hz.y >= 0.f && k.hz.z >= 0.f && k.hz.x <= 60.f && k.hz.y <= 60.f && k.hz.z <= 60.f;
        if (!ampOk || !hzOk || !(k.decay > 0.001f && k.decay <= 10.f))
            return fail(error, where + ": shake out of range (amp 0..0.5 m, hz 0..60, decay 0.001..10 s)");
        move.setShake(k);
    }
    if (!root["end_feet"].isAbsent()) {
        CutsceneVec3 feet;
        if (!readVec(root["end_feet"], feet)) return fail(error, where + ": \"end_feet\" is not [x, y, z]");
        if (std::sqrt(feet.x * feet.x + feet.y * feet.y) > 5.f) return fail(error, where + ": \"end_feet\" travels over 5 m");
        move.setEndFeet(feet);
    }

    const JsonValue shots = root["shots"];
    if (!shots.isArray() || shots.size() == 0) return fail(error, where + ": \"shots\" is not a list of shots");
    if (shots.size() > SpecialCineMove::kMaxShots) return fail(error, where + ": more than 12 shots");
    for (std::uint32_t i = 0; i < shots.size(); ++i) {
        const JsonValue sv = shots[i];
        const std::string at = where + " " + std::to_string(i + 1);
        if (!sv.isObject()) {
            move.clearShots();
            return fail(error, at + " is not an object");
        }
        warnUnknown(sv, {"id", "name", "in", "out", "anchor", "offset", "look_anchor", "look_at", "ease", "focal_mm", "focus_m", "fstop",
                         "shake", "follow_bone", "note"},
                    at, warnings);
        SpecialCineShot s;
        bool ok = sv["in"].isNumber() && sv["out"].isNumber();
        s.in = static_cast<float>(sv["in"].number());
        s.out = static_cast<float>(sv["out"].number());
        ok = ok && readAnchor(sv["anchor"], s.cameraAnchor) && readAnchor(sv["look_anchor"], s.lookAnchor);
        ok = ok && readSpan(sv["offset"], s.cameraFrom, s.cameraTo) && readSpan(sv["look_at"], s.lookFrom, s.lookTo);
        if (!ok) {
            move.clearShots();
            return fail(error, at + ": needs \"in\", \"out\", \"offset\" and \"look_at\" ({from, to}), and anchors lead, world, target or bone");
        }
        const std::string_view ease = sv["ease"].string("smoothstep");
        if (ease == "smoothstep") s.ease = SpecialCineEase::Smooth;
        else if (ease == "linear") s.ease = SpecialCineEase::Linear;
        else {
            move.clearShots();
            return fail(error, at + ": \"ease\" is not \"smoothstep\" or \"linear\"");
        }
        if (!readScalarSpan(sv["focal_mm"], 35.f, s.lensFrom, s.lensTo) || !readScalarSpan(sv["focus_m"], 0.f, s.focusFrom, s.focusTo)) {
            move.clearShots();
            return fail(error, at + ": \"focal_mm\" or \"focus_m\" is not a number or {from, to}");
        }
        s.fStop = sv["fstop"].isAbsent() ? 0.f : static_cast<float>(sv["fstop"].number());
        if (s.fStop == 0.f) s.fStop = move.fStop();
        s.shake = sv["shake"].boolean(false);
        const std::string_view name = sv["name"].string();
        const std::size_t keep = std::min<std::size_t>(name.size(), SpecialCineShot::kNameCapacity - 1);
        std::copy(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(keep), s.nameText);
        s.nameLength = static_cast<std::uint8_t>(keep);
        const std::string_view bone = sv["follow_bone"].string();
        const bool needsBone = s.cameraAnchor == SpecialCineAnchor::Bone || s.lookAnchor == SpecialCineAnchor::Bone;
        if (needsBone && !isIdText(bone, SpecialCineShot::kBoneCapacity - 1, true)) {
            move.clearShots();
            return fail(error, at + ": a \"bone\" anchor needs \"follow_bone\" (a bone name, 1..15)");
        }
        std::copy(bone.begin(), bone.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(bone.size(), SpecialCineShot::kBoneCapacity - 1)),
                  s.boneText);
        s.boneLength = static_cast<std::uint8_t>(std::min<std::size_t>(bone.size(), SpecialCineShot::kBoneCapacity - 1));
        if (!move.add(s)) {
            move.clearShots();
            return fail(error, at + ": does not start at the last shot's out, is under 0.1 s, ends past 20 s, or has a value out of range");
        }
    }
    if (move.length() < 0.5f) {
        move.clearShots();
        return fail(error, where + ": shorter than 0.5 s");
    }
    const JsonValue duration = root["duration_s"];
    if (!duration.isAbsent() && std::fabs(static_cast<float>(duration.number(-1.0)) - move.length()) > kTouch) {
        move.clearShots();
        return fail(error, where + ": \"duration_s\" is not the last shot's out");
    }
    if (move.blendIn() > move.shot(0).out - move.shot(0).in) {
        move.clearShots();
        return fail(error, where + ": the blend in is longer than the first shot");
    }
    const JsonValue cuts = root["cuts"];
    if (!cuts.isAbsent()) {
        bool same = cuts.isArray() && cuts.size() + 1 == move.size();
        for (std::uint32_t i = 0; same && i < cuts.size(); ++i)
            same = cuts[i].isNumber() && std::fabs(static_cast<float>(cuts[i].number()) - move.shot(i + 1).in) <= kTouch;
        if (!same && warnings != nullptr) warnings->push_back(where + ": \"cuts\" are not the shots' inner starts (the shots win)");
    }
    return true;
}

bool importSpecialCineTable(std::string_view text, SpecialCineTable& table, std::string* error, std::vector<std::string>* warnings) {
    if (readSpecialCineTable(text, table, error, warnings)) return true;
    table.clear(); // an error leaves no half table
    return false;
}

bool importSpecialCineShots(std::string_view text, SpecialCineMove& move, std::string* error, std::vector<std::string>* warnings) {
    if (readSpecialCineShots(text, move, error, warnings)) return true;
    move.clearShots(); // an error leaves no half list
    return false;
}

} // namespace ftcore
