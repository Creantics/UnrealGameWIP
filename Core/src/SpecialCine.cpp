#include "ftcore/SpecialCine.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
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

float dot3(const CutsceneVec3& a, const CutsceneVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
CutsceneVec3 cross3(const CutsceneVec3& a, const CutsceneVec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
CutsceneVec3 scale3(const CutsceneVec3& v, float k) { return {v.x * k, v.y * k, v.z * k}; }
CutsceneVec3 add3(const CutsceneVec3& a, const CutsceneVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

constexpr float kChest = 0.6f; // a figure's chest, of its height

float heightOr(float h) { return std::isfinite(h) && h > 0.1f ? h : kHumanHeight; }

// The lead's feet `along` (0..1) its travel, world metres.
CutsceneVec3 leadFeet(const SpecialCineMove& move, const SpecialCineStage& stage, float along) {
    const CutsceneVec3& end = move.endFeet();
    along *= std::isfinite(stage.travel) ? std::clamp(stage.travel, 0.f, 1.f) : 1.f;
    return specialCinePoint(stage, SpecialCineAnchor::Lead, {end.x * along, end.y * along, end.z * along});
}

bool bigTarget(const SpecialCineStage& stage) {
    return std::isfinite(stage.targetRadius) && stage.targetRadius > SpecialCineFraming::kBigTarget;
}

// A big target's strike point: on its side toward the lead's feet `lead`, at the lead's chest height.
CutsceneVec3 strikePoint(const SpecialCineStage& stage, const CutsceneVec3& lead) {
    CutsceneVec3 d = sub3(lead, stage.target);
    d.z = 0.f;
    const float n = length3(d);
    CutsceneVec3 p = stage.target;
    if (n > 1e-4f) p = add3(p, scale3(d, std::min(stage.targetRadius, n) / n));
    p.z = lead.z + kChest * heightOr(stage.leadHeight);
    return p;
}

// The contact: on the target's surface (radius `radius`) toward the lead's feet `lead`, out the fist's depth
// (kContactOut), at the lead's chest height but no higher than the target's chest.
CutsceneVec3 contactPoint(const SpecialCineStage& stage, const CutsceneVec3& lead, float radius) {
    CutsceneVec3 d = sub3(lead, stage.target);
    d.z = 0.f;
    const float n = length3(d);
    CutsceneVec3 p = stage.target;
    const float out = (std::isfinite(radius) ? std::max(radius, 0.f) : 0.f) + SpecialCineFraming::kContactOut;
    if (n > 1e-4f) p = add3(p, scale3(d, std::min(out, n) / n));
    p.z = std::min(lead.z + kChest * heightOr(stage.leadHeight), stage.target.z + kChest * heightOr(stage.targetHeight));
    return p;
}

// The point an aimed shot looks at, at `t`: the subject's chest (the lead's where its travel puts it then; both: halfway;
// a big target's: its strike point, tilted up kBigTilt).
CutsceneVec3 aimPoint(const SpecialCineMove& move, const SpecialCineShot& s, const SpecialCineStage& stage, float t,
                      const CutsceneVec3& fallback) {
    const float length = move.length();
    const CutsceneVec3 feet = leadFeet(move, stage, length > 0.f ? std::clamp(t / length, 0.f, 1.f) : 0.f);
    CutsceneVec3 lead = feet;
    lead.z += kChest * heightOr(stage.leadHeight);
    const bool big = bigTarget(stage);
    CutsceneVec3 target = big ? strikePoint(stage, feet) : stage.target;
    if (!big) target.z += kChest * heightOr(stage.targetHeight);
    CutsceneVec3 at = fallback;
    switch (s.subject) {
    case SpecialCineSubject::Lead: return lead;
    case SpecialCineSubject::Target: at = target; break;
    case SpecialCineSubject::Both: at = lerp3(lead, target, 0.5f); break;
    case SpecialCineSubject::None: return fallback;
    }
    if (big) at.z += SpecialCineFraming::kBigTilt;
    return at;
}

} // namespace

std::string_view specialCineSubjectName(SpecialCineSubject subject) {
    switch (subject) {
    case SpecialCineSubject::Lead: return "lead";
    case SpecialCineSubject::Target: return "target";
    case SpecialCineSubject::Both: return "both";
    case SpecialCineSubject::None: return "none";
    }
    return "lead";
}

std::string_view specialCineProblemName(SpecialCineProblem problem) {
    switch (problem) {
    case SpecialCineProblem::None: return "none";
    case SpecialCineProblem::InsideBody: return "inside a body";
    case SpecialCineProblem::BodyInLine: return "a body in the line";
    case SpecialCineProblem::World: return "the world";
    case SpecialCineProblem::OffFrame: return "off frame";
    case SpecialCineProblem::TooSmall: return "too small";
    case SpecialCineProblem::ContactHidden: return "the contact hidden";
    case SpecialCineProblem::LeadHidden: return "the lead hidden";
    case SpecialCineProblem::AlikeShot: return "like another shot";
    }
    return "none";
}

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
    reach_ = {};
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

void SpecialCineMove::setReach(const SpecialCineReach& reach) {
    const bool ok = std::isfinite(reach.radius) && std::isfinite(reach.from) && std::isfinite(reach.to) && reach.radius >= 0.f &&
                    reach.radius <= 5.f && reach.from >= 0.f && reach.from < reach.to;
    reach_ = ok ? reach : SpecialCineReach{};
}

bool SpecialCineMove::setNear(std::string_view file, float within) {
    if (file.empty()) {
        nearFileLength_ = 0;
        std::fill(std::begin(nearFile_), std::end(nearFile_), '\0');
        nearWithin_ = 0.f;
        return true;
    }
    if (!(std::isfinite(within) && within >= 0.1f && within <= 50.f)) return false;
    if (!setText(nearFile_, nearFileLength_, file, true)) return false;
    nearWithin_ = within;
    return true;
}

void SpecialCineMove::clear() {
    clearShots();
    setNear({}, 0.f);
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

const SpecialCineMove* SpecialCineTable::find(std::string_view id, float distance) const {
    for (std::uint32_t i = 0; i < count_; ++i) {
        if (moves_[i].id() != id) continue;
        const bool near = moves_[i].nearWithin() > 0.f && !near_[i].empty() && std::isfinite(distance) && distance < moves_[i].nearWithin();
        return near ? &near_[i] : &moves_[i];
    }
    return nullptr;
}

bool SpecialCineTable::add(const SpecialCineMove& move) {
    if (count_ >= kMaxMoves || move.id().empty() || find(move.id()) != nullptr) return false;
    moves_[count_] = move;
    SpecialCineMove& near = near_[count_];
    near = move;
    near.clearShots();
    near.setNear({}, 0.f);
    if (!move.nearFile().empty()) near.setShotFile(move.nearFile());
    ++count_;
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

float specialCineDistance(const SpecialCineStage& stage) {
    const float dx = stage.target.x - stage.origin.x, dy = stage.target.y - stage.origin.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    return std::isfinite(d) ? d : 0.f;
}

SpecialCineFrame specialCineFrameAt(const SpecialCineMove& move, float t, const SpecialCineStage& stage,
                                    const SpecialCineShotFix* fixes) {
    SpecialCineFrame f;
    if (move.empty()) return f;
    const float length = move.length();
    const float tc = std::isfinite(t) ? std::clamp(t, 0.f, length) : 0.f;
    const std::uint32_t timed = move.shotAt(tc);
    const std::uint32_t i = specialCineShownShot(move, timed, fixes); // a skipped shot shows its cover, clamped to its keys
    const SpecialCineShot& s = move.shot(i);
    const float p = ease01(s.ease, (tc - s.in) / std::max(s.out - s.in, kMinShot));
    f.shot = i;
    const SpecialCineShotFix fix = fixes != nullptr ? fixes[i] : SpecialCineShotFix{};
    CutsceneVec3 eye = lerp3(s.cameraFrom, s.cameraTo, p);
    CutsceneVec3 look = lerp3(s.lookFrom, s.lookTo, p);
    CutsceneVec3 lookA = s.lookFrom, lookB = s.lookTo;
    if (fix.mirror) {
        eye.y = -eye.y;
        look.y = -look.y;
        lookA.y = -lookA.y;
        lookB.y = -lookB.y;
    }
    f.pose.position = specialCinePoint(stage, s.cameraAnchor, eye);
    // Two anchors: each end placed from its own, eased between in the world.
    f.pose.lookAt = s.lookToAnchor() != s.lookAnchor
                        ? lerp3(specialCinePoint(stage, s.lookAnchor, lookA), specialCinePoint(stage, s.lookToAnchor(), lookB), p)
                        : specialCinePoint(stage, s.lookAnchor, look);
    if (std::isfinite(fix.raise) && fix.raise > 0.f) f.pose.position.z += fix.raise;
    if (std::isfinite(fix.orbit) && fix.orbit != 0.f) {
        // Camera and look-at turned together about the upright line through the subject.
        const CutsceneVec3 feet = leadFeet(move, stage, length > 0.f ? std::clamp(tc / length, 0.f, 1.f) : 0.f);
        const CutsceneVec3 contact = contactPoint(stage, feet, stage.targetRadius);
        CutsceneVec3 pivot = f.pose.lookAt;
        switch (s.subject) {
        case SpecialCineSubject::Lead: pivot = feet; break;
        case SpecialCineSubject::Target: pivot = contact; break;
        case SpecialCineSubject::Both: pivot = lerp3(feet, contact, 0.5f); break;
        case SpecialCineSubject::None: break;
        }
        const float a = fix.orbit * kPi / 180.f, ca = std::cos(a), sa = std::sin(a);
        const auto turn = [&pivot, ca, sa](CutsceneVec3& q) {
            const float x = q.x - pivot.x, y = q.y - pivot.y;
            q.x = pivot.x + x * ca - y * sa;
            q.y = pivot.y + x * sa + y * ca;
        };
        turn(f.pose.position);
        turn(f.pose.lookAt);
    }
    const bool aimed = fix.aim && s.subject != SpecialCineSubject::None;
    if (aimed) f.pose.lookAt = aimPoint(move, s, stage, tc, f.pose.lookAt);
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
    const float focus = aimed ? 0.f : s.focusFrom + (s.focusTo - s.focusFrom) * p - pulled;
    f.focus = focus > 0.f ? focus : std::max(length3(sub3(f.pose.lookAt, f.pose.position)), 0.1f);
    f.fStop = s.fStop > 0.f ? s.fStop : move.fStop();
    const SpecialCineShot& beat = move.shot(timed); // the shake belongs to the beat: a skipped shot's still plays
    if (beat.shake) {
        const SpecialCineShake& k = move.shake();
        const float u = tc - beat.in;
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

// ---- the framing check ----

namespace {

// The camera's axes: forward to the look-at; right and up across it (up along z; straight down: the stage's right).
struct View {
    CutsceneVec3 eye, fwd, right, up;
    float tanX = 1.f, tanY = 1.f;
};

View viewOf(const SpecialCineFrame& f, const SpecialCineStage& stage) {
    View v;
    v.eye = f.pose.position;
    CutsceneVec3 d = sub3(f.pose.lookAt, f.pose.position);
    const float n = length3(d);
    v.fwd = n > 1e-4f ? scale3(d, 1.f / n) : CutsceneVec3{1.f, 0.f, 0.f};
    CutsceneVec3 r = cross3(v.fwd, {0.f, 0.f, 1.f});
    float rn = length3(r);
    if (rn < 1e-3f) {
        float fx = 1.f, fy = 0.f;
        stageAxes(stage, fx, fy);
        r = {-fy, fx, 0.f};
        rn = 1.f;
    }
    v.right = scale3(r, 1.f / rn);
    v.up = cross3(v.right, v.fwd);
    v.tanX = std::tan(0.5f * f.fovDegrees * kPi / 180.f);
    v.tanY = v.tanX / SpecialCineFraming::kAspect;
    return v;
}

// The part of the 2D segment a..b inside the box |x|, |y| <= h (Liang-Barsky): false when none.
bool clipBox(float& ax, float& ay, float& bx, float& by, float h) {
    float t0 = 0.f, t1 = 1.f;
    const float dx = bx - ax, dy = by - ay;
    const float pq[4][2] = {{-dx, ax + h}, {dx, h - ax}, {-dy, ay + h}, {dy, h - ay}};
    for (const auto& e : pq) {
        if (std::fabs(e[0]) < 1e-9f) {
            if (e[1] < 0.f) return false;
            continue;
        }
        const float r = e[1] / e[0];
        if (e[0] < 0.f) t0 = std::max(t0, r);
        else t1 = std::min(t1, r);
        if (t0 > t1) return false;
    }
    const float x0 = ax + dx * t0, y0 = ay + dy * t0;
    bx = ax + dx * t1;
    by = ay + dy * t1;
    ax = x0;
    ay = y0;
    return true;
}

// The framing of one upright figure from `feet`, `height` tall.
SpecialCineProblem figureFraming(const View& v, const CutsceneVec3& feet, float height) {
    CutsceneVec3 a = sub3(feet, v.eye);
    CutsceneVec3 b = add3(a, {0.f, 0.f, height});
    float za = dot3(a, v.fwd), zb = dot3(b, v.fwd);
    constexpr float kNear = 0.05f;
    if (za < kNear && zb < kNear) return SpecialCineProblem::OffFrame;
    if (za < kNear || zb < kNear) { // clip the part behind the lens
        const float k = (kNear - za) / (zb - za);
        const CutsceneVec3 c = lerp3(a, b, k);
        if (za < kNear) a = c;
        else b = c;
        za = dot3(a, v.fwd);
        zb = dot3(b, v.fwd);
    }
    float ax = dot3(a, v.right) / (za * v.tanX), ay = dot3(a, v.up) / (za * v.tanY);
    float bx = dot3(b, v.right) / (zb * v.tanX), by = dot3(b, v.up) / (zb * v.tanY);
    float sx0 = ax, sy0 = ay, sx1 = bx, sy1 = by;
    if (!clipBox(sx0, sy0, sx1, sy1, SpecialCineFraming::kSafe)) return SpecialCineProblem::OffFrame;
    if (!clipBox(ax, ay, bx, by, 1.f)) return SpecialCineProblem::OffFrame;
    const float shown = 0.5f * std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)); // of the frame's height
    const CutsceneVec3 mid = add3(feet, {0.f, 0.f, 0.5f * height});
    const bool close = length3(sub3(mid, v.eye)) < SpecialCineFraming::kCloseUpRange;
    return shown >= (close ? SpecialCineFraming::kMinCloseUp : SpecialCineFraming::kMinFigure) ? SpecialCineProblem::None
                                                                                               : SpecialCineProblem::TooSmall;
}

// A point inside the safe frame, in front of the lens.
SpecialCineProblem pointFraming(const View& v, const CutsceneVec3& p) {
    const CutsceneVec3 a = sub3(p, v.eye);
    const float z = dot3(a, v.fwd);
    if (z < 0.05f) return SpecialCineProblem::OffFrame;
    const float x = dot3(a, v.right) / (z * v.tanX), y = dot3(a, v.up) / (z * v.tanY);
    return std::fabs(x) <= SpecialCineFraming::kSafe && std::fabs(y) <= SpecialCineFraming::kSafe ? SpecialCineProblem::None
                                                                                                 : SpecialCineProblem::OffFrame;
}

bool insideCylinder(const CutsceneVec3& p, const SpecialCineBody& b, float margin) {
    const float dx = p.x - b.feet.x, dy = p.y - b.feet.y;
    const float r = std::max(b.radius, 0.f) + margin;
    return dx * dx + dy * dy < r * r && p.z >= b.feet.z - margin && p.z <= b.feet.z + std::max(b.height, 0.f) + margin;
}

// A body between `from` and `to` (sampled every 5 cm or so; a body is at least that wide).
bool bodyInLine(const CutsceneVec3& from, const CutsceneVec3& to, const SpecialCineBody* bodies, std::uint32_t count, bool skipTarget) {
    const float d = length3(sub3(to, from));
    const int steps = std::clamp(static_cast<int>(d / 0.05f), 1, 400);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (skipTarget && bodies[i].target) continue;
        for (int k = 1; k <= steps; ++k)
            if (insideCylinder(lerp3(from, to, static_cast<float>(k) / static_cast<float>(steps)), bodies[i], 0.f)) return true;
    }
    return false;
}

// The line from the eye to a subject's chest, stopped kLineStop short of it.
SpecialCineProblem lineTo(const View& v, CutsceneVec3 chest, const SpecialCineBody* bodies, std::uint32_t count, bool skipTarget,
                          SpecialCineWorldTest world, void* user) {
    const CutsceneVec3 d = sub3(chest, v.eye);
    const float n = length3(d);
    if (n <= SpecialCineFraming::kLineStop) return SpecialCineProblem::None;
    chest = add3(v.eye, scale3(d, (n - SpecialCineFraming::kLineStop) / n));
    if (bodyInLine(v.eye, chest, bodies, count, skipTarget)) return SpecialCineProblem::BodyInLine;
    if (world != nullptr && world(v.eye, chest, user)) return SpecialCineProblem::World;
    return SpecialCineProblem::None;
}

// The segment a..b crosses body `body`'s upright cylinder (exact: the part of the segment inside its circle, then
// its heights there against the body's).
bool segmentHitsBody(const CutsceneVec3& a, const CutsceneVec3& b, const SpecialCineBody& body) {
    const float r = std::max(body.radius, 0.f), h = std::max(body.height, 0.f);
    const float bottom = body.feet.z, top = body.feet.z + h;
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float ox = a.x - body.feet.x, oy = a.y - body.feet.y;
    const float qa = dx * dx + dy * dy, qb = 2.f * (ox * dx + oy * dy), qc = ox * ox + oy * oy - r * r;
    float t0 = 0.f, t1 = 1.f;
    if (qa < 1e-10f) { // upright: inside the circle or not
        if (qc >= 0.f) return false;
    } else {
        const float disc = qb * qb - 4.f * qa * qc;
        if (disc <= 0.f) return false;
        const float root = std::sqrt(disc);
        t0 = std::max(t0, (-qb - root) / (2.f * qa));
        t1 = std::min(t1, (-qb + root) / (2.f * qa));
        if (t0 >= t1) return false;
    }
    const float z0 = a.z + dz * t0, z1 = a.z + dz * t1;
    return std::max(z0, z1) >= bottom && std::min(z0, z1) <= top;
}

bool segmentHitsAny(const CutsceneVec3& a, const CutsceneVec3& b, const SpecialCineBody* bodies, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i)
        if (segmentHitsBody(a, b, bodies[i])) return true;
    return false;
}

// The share of the lead's sample points (feet `feet`, `height` tall) the eye sees past the bodies.
float leadShare(const CutsceneVec3& eye, const CutsceneVec3& feet, float height, const SpecialCineStage& stage,
                const SpecialCineBody* bodies, std::uint32_t count) {
    constexpr std::uint32_t axis = SpecialCineFraming::kLeadAxisPoints;
    std::uint32_t seen = 0;
    for (std::uint32_t k = 0; k < axis; ++k) {
        const float z = feet.z + height * static_cast<float>(k) / static_cast<float>(axis - 1);
        if (!segmentHitsAny(eye, {feet.x, feet.y, z}, bodies, count)) ++seen;
    }
    // The shoulders: square to the eye's line on the ground (from straight above: the stage's right).
    float sx = -(feet.y - eye.y), sy = feet.x - eye.x;
    const float n = std::sqrt(sx * sx + sy * sy);
    if (n > 1e-3f) {
        sx /= n;
        sy /= n;
    } else {
        float fx = 1.f, fy = 0.f;
        stageAxes(stage, fx, fy);
        sx = -fy;
        sy = fx;
    }
    const float z = feet.z + SpecialCineFraming::kShoulderHeight * height, w = SpecialCineFraming::kShoulderHalf;
    if (!segmentHitsAny(eye, {feet.x + sx * w, feet.y + sy * w, z}, bodies, count)) ++seen;
    if (!segmentHitsAny(eye, {feet.x - sx * w, feet.y - sy * w, z}, bodies, count)) ++seen;
    return static_cast<float>(seen) / static_cast<float>(axis + 2);
}

// The radius the contact sits out from: the stage's, or the target body's when wider.
float contactRadius(const SpecialCineStage& stage, const SpecialCineBody* bodies, std::uint32_t count) {
    float r = std::isfinite(stage.targetRadius) ? std::max(stage.targetRadius, 0.f) : 0.f;
    for (std::uint32_t i = 0; i < count; ++i)
        if (bodies[i].target && std::isfinite(bodies[i].radius)) r = std::max(r, bodies[i].radius);
    return r;
}

} // namespace

SpecialCineHit specialCineHit(const SpecialCineMove& move, const SpecialCineStage& stage, float impactTime, float radius,
                              const SpecialCineShotFix* fixes) {
    SpecialCineHit hit;
    const float length = move.length();
    const float t = std::isfinite(impactTime) ? impactTime : move.release();
    hit.time = std::clamp(t, 0.f, std::max(length, 0.f));
    hit.stillAt = std::min(hit.time + SpecialCineHit::kStillLag, std::max(length, 0.f));
    hit.shot = move.empty() ? 0 : specialCineShownShot(move, move.shotAt(hit.time), fixes);
    const CutsceneVec3 feet = leadFeet(move, stage, length > 0.f ? hit.time / length : 0.f);
    const float own = std::isfinite(stage.targetRadius) ? std::max(stage.targetRadius, 0.f) : 0.f;
    hit.at = contactPoint(stage, feet, std::isfinite(radius) ? std::max(own, radius) : own);
    return hit;
}

std::uint32_t specialCineHitStills(const SpecialCineMove& move, const SpecialCineHit& hit, float* out, std::uint32_t capacity) {
    if (out == nullptr || move.empty()) return 0;
    const float length = move.length();
    std::uint32_t n = 0;
    for (std::uint32_t k = 0; k < SpecialCineHit::kStills && n < capacity; ++k) {
        const float at = hit.time + SpecialCineHit::kStillLags[k];
        if (std::isfinite(at) && at < length) out[n++] = at;
    }
    return n;
}

SpecialCineProblem specialCineHitProblem(const SpecialCineMove& move, const SpecialCineStage& stage, const SpecialCineHit& hit,
                                         const SpecialCineShotFix* fixes, const SpecialCineBody* bodies, std::uint32_t count) {
    if (move.empty()) return SpecialCineProblem::None;
    if (bodies == nullptr) count = 0;
    const View v = viewOf(specialCineFrameAt(move, hit.time, stage, fixes), stage);
    if (pointFraming(v, hit.at) != SpecialCineProblem::None) return SpecialCineProblem::OffFrame;
    if (segmentHitsAny(v.eye, hit.at, bodies, count)) return SpecialCineProblem::ContactHidden;
    return SpecialCineProblem::None;
}

CutsceneVec3 specialCinePartnerBlow(const CutsceneVec3& target, float radius, float targetHeight, const CutsceneVec3& toward) {
    CutsceneVec3 d = sub3(toward, target);
    d.z = 0.f;
    const float n = length3(d);
    CutsceneVec3 p = target;
    const float out = (std::isfinite(radius) ? std::max(radius, 0.f) : 0.f) + SpecialCineFraming::kContactOut;
    if (n > 1e-4f) p = add3(p, scale3(d, std::min(out, n) / n));
    p.z = std::min(toward.z, target.z + kChest * heightOr(targetHeight));
    return p;
}

SpecialCinePartnerStrike specialCinePartnerStrike(const SpecialCineMove& move, const SpecialCineStage& stage,
                                                  const SpecialCineHit& hit, const CutsceneVec3& partnerChest, float radius) {
    SpecialCinePartnerStrike strike;
    const float own = std::isfinite(stage.targetRadius) ? std::max(stage.targetRadius, 0.f) : 0.f;
    const float out = std::isfinite(radius) ? std::max(own, radius) : own;
    strike.at = specialCinePartnerBlow(stage.target, out, stage.targetHeight, partnerChest);
    const float length = move.length();
    CutsceneVec3 lead = leadFeet(move, stage, length > 0.f ? std::clamp(hit.time / length, 0.f, 1.f) : 0.f);
    // The partner's bearing round the target's axis from the lead's side (the ground plane).
    {
        const float lx = lead.x - stage.target.x, ly = lead.y - stage.target.y;
        const float px = partnerChest.x - stage.target.x, py = partnerChest.y - stage.target.y;
        const float ln = std::sqrt(lx * lx + ly * ly), pn = std::sqrt(px * px + py * py);
        if (ln > 1e-4f && pn > 1e-4f)
            strike.bearing = std::acos(std::clamp((lx * px + ly * py) / (ln * pn), -1.f, 1.f)) * 180.f / kPi;
    }
    lead.z += kChest * heightOr(stage.leadHeight);
    const CutsceneVec3 line = sub3(hit.at, lead);
    const float span = length3(line);
    const CutsceneVec3 rel = sub3(strike.at, lead);
    if (span < 1e-4f) {
        strike.offLine = length3(rel);
        return strike;
    }
    strike.along = (rel.x * line.x + rel.y * line.y + rel.z * line.z) / (span * span);
    strike.offLine = length3(sub3(rel, scale3(line, strike.along)));
    return strike;
}

SpecialCineProblem specialCineFrameProblem(const SpecialCineMove& move, float t, const SpecialCineStage& stage,
                                           const SpecialCineShotFix* fixes, const SpecialCineBody* bodies, std::uint32_t count,
                                           SpecialCineWorldTest world, void* user) {
    if (move.empty()) return SpecialCineProblem::None;
    if (bodies == nullptr) count = 0;
    const SpecialCineFrame f = specialCineFrameAt(move, t, stage, fixes);
    const View v = viewOf(f, stage);
    for (std::uint32_t i = 0; i < count; ++i)
        if (insideCylinder(v.eye, bodies[i], SpecialCineFraming::kBodyMargin)) return SpecialCineProblem::InsideBody;
    const float length = move.length();
    const CutsceneVec3 leadAt = leadFeet(move, stage, length > 0.f ? std::clamp(t / length, 0.f, 1.f) : 0.f);
    const float leadH = heightOr(stage.leadHeight), targetH = heightOr(stage.targetHeight);
    const float tc = std::isfinite(t) ? std::clamp(t, 0.f, length) : 0.f; // the clock specialCineFrameAt used
    if (move.reach().at(tc) && move.shot(f.shot).cameraAnchor != SpecialCineAnchor::Bone) {
        SpecialCineBody held; // the lead's cylinder, grown by what it holds
        held.feet = leadAt;
        held.radius = move.reach().radius;
        held.height = leadH;
        if (insideCylinder(v.eye, held, SpecialCineFraming::kBodyMargin)) return SpecialCineProblem::InsideBody;
    }
    if (world != nullptr && world(v.eye, v.eye, user)) return SpecialCineProblem::World;
    const SpecialCineSubject subject = move.shot(f.shot).subject;
    if (subject == SpecialCineSubject::None) return SpecialCineProblem::None;
    const bool lead = subject == SpecialCineSubject::Lead || subject == SpecialCineSubject::Both;
    const bool target = subject == SpecialCineSubject::Target || subject == SpecialCineSubject::Both;
    const bool big = bigTarget(stage);
    const CutsceneVec3 strike = big ? strikePoint(stage, leadAt) : CutsceneVec3{};
    // Framing first (a shot that shows nothing has nothing to block), then the lines.
    SpecialCineProblem framing = SpecialCineProblem::None;
    const auto worse = [&framing](SpecialCineProblem p) {
        if (p == SpecialCineProblem::OffFrame || (p == SpecialCineProblem::TooSmall && framing == SpecialCineProblem::None)) framing = p;
    };
    if (lead) worse(figureFraming(v, leadAt, leadH));
    if (target) worse(big ? pointFraming(v, strike) : figureFraming(v, stage.target, targetH));
    if (framing != SpecialCineProblem::None) return framing;
    if (lead) {
        CutsceneVec3 chest = leadAt;
        chest.z += kChest * leadH;
        const SpecialCineProblem p = lineTo(v, chest, bodies, count, target, world, user); // a two-shot may overlap them
        if (p != SpecialCineProblem::None) return p;
    }
    if (target) {
        CutsceneVec3 chest = stage.target;
        chest.z += kChest * targetH;
        const SpecialCineProblem p = lineTo(v, big ? strike : chest, bodies, count, true, world, user);
        if (p != SpecialCineProblem::None) return p;
    }
    // Last, so either means every check above passed: the contact and the lead must read past every body.
    if (target && segmentHitsAny(v.eye, contactPoint(stage, leadAt, contactRadius(stage, bodies, count)), bodies, count))
        return SpecialCineProblem::ContactHidden;
    if (lead && leadShare(v.eye, leadAt, leadH, stage, bodies, count) < SpecialCineFraming::kMinLeadShare)
        return SpecialCineProblem::LeadHidden;
    return SpecialCineProblem::None;
}

namespace {

// The first problem over shot `shot`'s window at its samples under `fixes` (one per shot), and when.
SpecialCineProblem windowProblem(const SpecialCineMove& move, std::uint32_t shot, const SpecialCineStage& stage,
                                 const SpecialCineShotFix* fixes, const SpecialCineBody* bodies, std::uint32_t count,
                                 SpecialCineWorldTest world, void* user, float& at) {
    const SpecialCineShot& s = move.shot(shot);
    constexpr std::uint32_t n = SpecialCineFraming::kSamples;
    for (std::uint32_t k = 0; k < n; ++k) {
        // Spread over the shot, 2% in from each end (the frame at a cut belongs to the next shot).
        const float u = 0.02f + 0.96f * static_cast<float>(k) / static_cast<float>(n - 1);
        const float t = s.in + (s.out - s.in) * u;
        const SpecialCineProblem p = specialCineFrameProblem(move, t, stage, fixes, bodies, count, world, user);
        if (p != SpecialCineProblem::None) {
            at = t;
            return p;
        }
    }
    return SpecialCineProblem::None;
}

// The first problem of shot `shot` under `fix` (every other shot as written) over its samples, and when.
SpecialCineProblem shotProblem(const SpecialCineMove& move, std::uint32_t shot, const SpecialCineStage& stage, const SpecialCineShotFix& fix,
                               const SpecialCineBody* bodies, std::uint32_t count, SpecialCineWorldTest world, void* user, float& at) {
    SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
    fixes[shot] = fix;
    return windowProblem(move, shot, stage, fixes, bodies, count, world, user, at);
}

// Whether shot `shot` under `fix` stands where another shot of the move stands as written (one that frames someone:
// a "none" shot is composed on its own, top-down or on a bone).
bool alikeAnother(const SpecialCineMove& move, std::uint32_t shot, const SpecialCineStage& stage, const SpecialCineShotFix& fix) {
    SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
    fixes[shot] = fix;
    const SpecialCineShotView mine = specialCineShotView(move, shot, stage, fixes);
    for (std::uint32_t i = 0; i < move.size(); ++i) {
        if (i == shot || move.shot(i).subject == SpecialCineSubject::None) continue;
        if (specialCineViewsAlike(mine, specialCineShotView(move, i, stage))) return true;
    }
    return false;
}

} // namespace

SpecialCineShotView specialCineShotView(const SpecialCineMove& move, std::uint32_t shot, const SpecialCineStage& stage,
                                        const SpecialCineShotFix* fixes) {
    SpecialCineShotView v;
    if (shot >= move.size()) {
        v.azimuth = v.distance = std::numeric_limits<float>::quiet_NaN();
        return v;
    }
    const SpecialCineShot& s = move.shot(shot);
    const float t = 0.5f * (s.in + s.out);
    const float length = move.length();
    const CutsceneVec3 eye = specialCineFrameAt(move, t, stage, fixes).pose.position;
    const CutsceneVec3 feet = leadFeet(move, stage, length > 0.f ? std::clamp(t / length, 0.f, 1.f) : 0.f);
    float fx = 1.f, fy = 0.f;
    stageAxes(stage, fx, fy);
    const float dx = eye.x - feet.x, dy = eye.y - feet.y;
    v.azimuth = std::atan2(dx * -fy + dy * fx, dx * fx + dy * fy) * 180.f / kPi;
    v.distance = std::sqrt(dx * dx + dy * dy);
    return v;
}

bool specialCineViewsAlike(const SpecialCineShotView& a, const SpecialCineShotView& b) {
    if (!std::isfinite(a.azimuth) || !std::isfinite(b.azimuth) || !std::isfinite(a.distance) || !std::isfinite(b.distance)) return false;
    float turn = std::fabs(a.azimuth - b.azimuth);
    turn = std::fmod(turn, 360.f);
    if (turn > 180.f) turn = 360.f - turn;
    const float farther = std::max(a.distance, b.distance);
    const float apart = farther > 1e-4f ? std::fabs(a.distance - b.distance) / farther : 0.f;
    return turn < SpecialCineShotView::kAlikeAzimuth && apart < SpecialCineShotView::kAlikeDistance;
}

SpecialCineShotCheck specialCineFixShot(const SpecialCineMove& move, std::uint32_t shot, const SpecialCineStage& stage,
                                        const SpecialCineBody* bodies, std::uint32_t count, SpecialCineWorldTest world, void* user) {
    SpecialCineShotCheck c;
    if (shot >= move.size()) return c;
    float at = 0.f;
    c.tries = 1;
    c.first = c.last = shotProblem(move, shot, stage, {}, bodies, count, world, user, at);
    c.firstAt = at;
    if (c.first == SpecialCineProblem::None) return c;
    const auto attempt = [&](const SpecialCineShotFix& fix) {
        ++c.tries;
        float when = 0.f;
        c.last = shotProblem(move, shot, stage, fix, bodies, count, world, user, when);
        if (c.last != SpecialCineProblem::None) return false;
        if (alikeAnother(move, shot, stage, fix)) {
            c.last = SpecialCineProblem::AlikeShot;
            ++c.alike;
            return false;
        }
        c.fix = fix;
        return true;
    };
    constexpr float kRaises[] = {0.4f, 0.8f, 1.2f};
    if (attempt({true, 0.f, 0.f, false})) return c;
    for (const float r : kRaises)
        if (attempt({false, r, 0.f, false})) return c;
    for (const float r : kRaises)
        if (attempt({true, r, 0.f, false})) return c;
    const bool aimable = move.shot(shot).subject != SpecialCineSubject::None;
    if (aimable) {
        if (attempt({false, 0.f, 0.f, true}) || attempt({true, 0.f, 0.f, true})) return c;
        for (const float r : kRaises)
            if (attempt({false, r, 0.f, true})) return c;
    }
    for (int k = 1; k <= 12; ++k) {
        const float pull = 0.5f * static_cast<float>(k);
        if (attempt({false, 0.f, pull, false})) return c;
        if (aimable && attempt({false, 0.f, pull, true})) return c;
    }
    // The reframe: swung about the subject each way (each plain, then aimed), then raised and swung again; last, the
    // reverse angle the same way. A turned look-at that rode past the subject looks away; the aim brings it back.
    const auto orbited = [&](float raise, float orbit) {
        return attempt({false, raise, 0.f, false, orbit}) || (aimable && attempt({false, raise, 0.f, true, orbit}));
    };
    for (const float raise : {0.f, SpecialCineFraming::kOrbitRaise})
        for (int k = 1; static_cast<float>(k) * SpecialCineFraming::kOrbitStep <= SpecialCineFraming::kOrbitMax + 0.01f; ++k)
            for (const float sign : {1.f, -1.f})
                if (orbited(raise, sign * SpecialCineFraming::kOrbitStep * static_cast<float>(k))) return c;
    for (const float raise : {0.f, SpecialCineFraming::kOrbitRaise})
        if (orbited(raise, SpecialCineFraming::kOrbitReverse)) return c;
    // The reverse swung off the line, as written and mirrored, for a reverse that stands where another shot stands.
    for (const float raise : {0.f, SpecialCineFraming::kOrbitRaise})
        for (const float swing : SpecialCineFraming::kReverseSwings)
            for (const float sign : {1.f, -1.f})
                for (const bool mirror : {false, true}) {
                    const float orbit = SpecialCineFraming::kOrbitReverse + sign * swing;
                    if (attempt({mirror, raise, 0.f, false, orbit}) || (aimable && attempt({mirror, raise, 0.f, true, orbit}))) return c;
                }
    // The reverse pulled in (inside the cap), mirrored first, for the smallest kinds far out.
    for (const float raise : {0.f, SpecialCineFraming::kOrbitRaise})
        for (const float pull : SpecialCineFraming::kReversePulls)
            for (const bool mirror : {true, false}) {
                const float orbit = SpecialCineFraming::kOrbitReverse;
                if (attempt({mirror, raise, pull, false, orbit}) || (aimable && attempt({mirror, raise, pull, true, orbit}))) return c;
            }
    c.ok = false;
    return c;
}

std::uint32_t specialCineShownShot(const SpecialCineMove& move, std::uint32_t shot, const SpecialCineShotFix* fixes) {
    if (fixes == nullptr || shot >= move.size()) return shot;
    switch (fixes[shot].cover) {
    case SpecialCineCover::Next:
        return shot + 1 < move.size() && fixes[shot + 1].cover == SpecialCineCover::None ? shot + 1 : shot;
    case SpecialCineCover::Previous:
        return shot > 0 && fixes[shot - 1].cover == SpecialCineCover::None ? shot - 1 : shot;
    case SpecialCineCover::None: break;
    }
    return shot;
}

SpecialCineCover specialCineCoverShot(const SpecialCineMove& move, std::uint32_t shot, const SpecialCineStage& stage,
                                      const SpecialCineShotFix* fixes, const SpecialCineBody* bodies, std::uint32_t count,
                                      SpecialCineWorldTest world, void* user) {
    if (shot >= move.size()) return SpecialCineCover::None;
    SpecialCineShotFix trial[SpecialCineMove::kMaxShots]{};
    if (fixes != nullptr) std::copy(fixes, fixes + move.size(), trial);
    for (const SpecialCineCover cover : {SpecialCineCover::Next, SpecialCineCover::Previous}) {
        trial[shot] = SpecialCineShotFix{};
        trial[shot].cover = cover;
        const std::uint32_t shown = specialCineShownShot(move, shot, trial);
        if (shown == shot || move.shot(shown).subject == SpecialCineSubject::None) continue;
        float at = 0.f;
        if (windowProblem(move, shot, stage, trial, bodies, count, world, user, at) == SpecialCineProblem::None) return cover;
    }
    return SpecialCineCover::None;
}

std::uint32_t specialCineCutList(const SpecialCineMove& move, const SpecialCineShotFix* fixes, SpecialCineCut* out,
                                 std::uint32_t capacity) {
    std::uint32_t n = 0;
    SpecialCineCut run;
    for (std::uint32_t i = 0; i < move.size(); ++i) {
        const SpecialCineShot& s = move.shot(i);
        const std::uint32_t shown = specialCineShownShot(move, i, fixes);
        if (n > 0 && run.shot == shown) {
            run.out = s.out;
        } else {
            if (n > 0 && out != nullptr && n <= capacity) out[n - 1] = run;
            run = SpecialCineCut{shown, s.in, s.out, 0};
            ++n;
        }
        if (shown != i) ++run.skipped;
    }
    if (n > 0 && out != nullptr && n <= capacity) out[n - 1] = run;
    return n;
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
        warnUnknown(mv, {"id", "shots", "release", "fstop", "blend_in", "blend_out", "near", "near_within"}, where, warnings);
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
        if (mv["near"].isAbsent() != mv["near_within"].isAbsent()) return fail(error, where + ": \"near\" and \"near_within\" go together");
        if (!mv["near"].isAbsent() &&
            (mv["near"].string() == mv["shots"].string() || !move.setNear(mv["near"].string(), static_cast<float>(mv["near_within"].number(-1.0)))))
            return fail(error, where + ": \"near\" is not another file name, or \"near_within\" is outside 0.1..50 m");
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
    warnUnknown(root, {"version", "clip", "duration_s", "fps", "frame", "sensor_width_mm", "aspect", "shake", "end_feet", "reach", "cuts", "shots"},
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
    if (!root["reach"].isAbsent()) {
        const JsonValue r = root["reach"];
        warnUnknown(r, {"m", "from", "to", "note"}, where + " reach", warnings);
        SpecialCineReach reach;
        reach.radius = static_cast<float>(r["m"].number(-1.0));
        reach.from = static_cast<float>(r["from"].number(-1.0));
        reach.to = static_cast<float>(r["to"].number(-1.0));
        if (!r.isObject() || !(reach.radius >= 0.f && reach.radius <= 5.f) || !(reach.from >= 0.f && reach.from < reach.to) ||
            !std::isfinite(reach.to))
            return fail(error, where + ": \"reach\" is not { \"m\": 0..5, \"from\": s, \"to\": s after it }");
        move.setReach(reach);
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
        warnUnknown(sv, {"id", "name", "in", "out", "anchor", "offset", "look_anchor", "look_at", "look_anchor_to", "ease", "focal_mm", "focus_m", "fstop",
                         "shake", "follow_bone", "subject", "note"},
                    at, warnings);
        SpecialCineShot s;
        bool ok = sv["in"].isNumber() && sv["out"].isNumber();
        s.in = static_cast<float>(sv["in"].number());
        s.out = static_cast<float>(sv["out"].number());
        ok = ok && readAnchor(sv["anchor"], s.cameraAnchor) && readAnchor(sv["look_anchor"], s.lookAnchor);
        ok = ok && readSpan(sv["offset"], s.cameraFrom, s.cameraTo) && readSpan(sv["look_at"], s.lookFrom, s.lookTo);
        s.lookEndOwn = !sv["look_anchor_to"].isAbsent();
        ok = ok && (!s.lookEndOwn || readAnchor(sv["look_anchor_to"], s.lookEndAnchor));
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
        const std::string_view subject = sv["subject"].string(s.lookAnchor == SpecialCineAnchor::Target ? "target" : "lead");
        if (subject == "lead") s.subject = SpecialCineSubject::Lead;
        else if (subject == "target") s.subject = SpecialCineSubject::Target;
        else if (subject == "both") s.subject = SpecialCineSubject::Both;
        else if (subject == "none") s.subject = SpecialCineSubject::None;
        else {
            move.clearShots();
            return fail(error, at + ": \"subject\" is not \"lead\", \"target\", \"both\" or \"none\"");
        }
        const std::string_view name = sv["name"].string();
        const std::size_t keep = std::min<std::size_t>(name.size(), SpecialCineShot::kNameCapacity - 1);
        std::copy(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(keep), s.nameText);
        s.nameLength = static_cast<std::uint8_t>(keep);
        const std::string_view bone = sv["follow_bone"].string();
        const bool needsBone = s.cameraAnchor == SpecialCineAnchor::Bone || s.lookAnchor == SpecialCineAnchor::Bone ||
                               s.lookToAnchor() == SpecialCineAnchor::Bone;
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
