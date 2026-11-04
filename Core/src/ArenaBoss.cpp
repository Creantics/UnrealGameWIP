#include "ftcore/ArenaBoss.h"

#include "ftcore/Json.h"
#include "ftcore/Load.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <utility>

namespace ftcore {
namespace {

constexpr double kNever = std::numeric_limits<double>::infinity();
constexpr double kBeatSlack = 1e-6; // beats compared in the loader: rounding in the file, not the rule

float abLen(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
Vec2 abSub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
float abDot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
// The right of facing f (the host's frame, seen from above).
Vec2 abRight(Vec2 f) { return {-f.y, f.x}; }
Vec2 abUnit(Vec2 v) {
    const float l = abLen(v);
    return l > 1e-6f ? Vec2{v.x / l, v.y / l} : Vec2{};
}
float abRad(float deg) { return deg * std::numbers::pi_v<float> / 180.0f; }

} // namespace

// ---- Called blows ----------------------------------------------------------------------------------

CalledZone arenaSideOf(Vec2 pos, Vec2 facing, Vec2 p) {
    return abDot(abSub(p, pos), abRight(facing)) > 0.0f ? CalledZone::Right : CalledZone::Left;
}

bool arenaInLane(Vec2 from, Vec2 to, float width, Vec2 p) {
    const Vec2 axis = abSub(to, from);
    const float len = abLen(axis);
    if (len < 1e-3f) return false;
    const Vec2 u{axis.x / len, axis.y / len};
    const Vec2 d = abSub(p, from);
    const float along = abDot(d, u);
    if (along < 0.0f || along > len) return false;
    return std::fabs(abDot(d, abRight(u))) <= width * 0.5f;
}

CalledZone arenaZoneOf(CalledZone next, Vec2 pos, Vec2 facing, Vec2 laneEnd, float laneWidth, Vec2 p) {
    if (next == CalledZone::Path && arenaInLane(pos, laneEnd, laneWidth, p)) return CalledZone::Path;
    return arenaSideOf(pos, facing, p);
}

const char* calledOutcomeName(CalledOutcome o) {
    switch (o) {
    case CalledOutcome::Intercepted: return "INTERCEPT";
    case CalledOutcome::Guarded: return "GUARD";
    case CalledOutcome::WrongZone: return "WRONG SIDE";
    case CalledOutcome::Missed: return "MISSED";
    case CalledOutcome::Pending: break;
    }
    return "";
}

void CalledStrike::start(const CalledImpact* impacts, std::size_t count) {
    count_ = 0;
    next_ = 0;
    for (std::size_t i = 0; i < count && count_ < kMaxCalledImpacts; ++i) {
        // Insertion keeps them sorted by time (stable on ties).
        std::size_t j = count_++;
        while (j > 0 && impacts_[j - 1].at > impacts[i].at) {
            impacts_[j] = impacts_[j - 1];
            --j;
        }
        impacts_[j] = impacts[i];
    }
    outcomes_.fill(CalledOutcome::Pending);
}

void CalledStrike::cancel() { count_ = next_; }

CalledOutcome CalledStrike::judge(const CalledImpact& imp) const {
    if (hold_ == ArenaHold::None) return CalledOutcome::Missed;
    if (imp.zone == CalledZone::Path && holdZone_ != CalledZone::Path) return CalledOutcome::Missed;
    if (hold_ == ArenaHold::Guard) return CalledOutcome::Guarded;
    if (holdZone_ != imp.zone) return CalledOutcome::WrongZone;
    const bool inWindow = holdFrom_ >= imp.at - windows_.window && holdFrom_ < imp.at;
    return inWindow ? CalledOutcome::Intercepted : CalledOutcome::Guarded;
}

CalledResults CalledStrike::resolveUpTo(double t) {
    CalledResults out;
    if (std::isfinite(t) && t > now_) now_ = t;
    while (next_ < count_ && impacts_[next_].at <= now_) {
        const CalledOutcome o = judge(impacts_[next_]);
        outcomes_[next_] = o;
        out.items[out.count++] = {static_cast<std::uint8_t>(next_), o, impacts_[next_].at};
        ++next_;
    }
    return out;
}

CalledResults CalledStrike::update(double now) { return resolveUpTo(now); }

CalledResults CalledStrike::interceptDown(CalledZone zone, double t) {
    const CalledResults out = resolveUpTo(t);
    if (hold_ != ArenaHold::None || rearming(now_)) return out;
    hold_ = ArenaHold::Intercept;
    holdFrom_ = now_;
    holdZone_ = zone;
    rearmUntil_ = now_ + windows_.rearm;
    return out;
}

CalledResults CalledStrike::interceptUp(double t) {
    const CalledResults out = resolveUpTo(t);
    if (hold_ == ArenaHold::Intercept) hold_ = ArenaHold::None;
    return out;
}

CalledResults CalledStrike::guardDown(CalledZone zone, double t) {
    const CalledResults out = resolveUpTo(t);
    if (hold_ != ArenaHold::None) return out;
    hold_ = ArenaHold::Guard;
    holdFrom_ = now_;
    holdZone_ = zone;
    return out;
}

CalledResults CalledStrike::guardUp(double t) {
    const CalledResults out = resolveUpTo(t);
    if (hold_ == ArenaHold::Guard) hold_ = ArenaHold::None;
    return out;
}

// ---- Weak points -----------------------------------------------------------------------------------

bool arenaGatePasses(const ArenaWeakGate& gate, Vec2 pos, Vec2 facing, Vec2 from) {
    if (gate.arc >= 180.0f) return true;
    const Vec2 d = abSub(from, pos);
    const float len = abLen(d);
    if (len < 1e-4f) return false;
    const Vec2 f = abUnit(facing);
    const float ahead = abDot(d, f) / len;
    const float right = abDot(d, abRight(f)) / len;
    const float g = abRad(gate.facing);
    // The cosine of the angle between the body's bearing and the gate's direction, both in the boss's frame.
    return ahead * std::cos(g) + right * std::sin(g) >= std::cos(abRad(gate.arc)) - 1e-6f;
}

WeakPoints CritWindow::gated(Vec2 from, Vec2 pos, Vec2 facing, std::array<std::uint8_t, kMaxWeakPoints>& index) const {
    WeakPoints out;
    out.halfWidth = points_.points.halfWidth;
    for (std::size_t i = 0; i < points_.points.size(); ++i) {
        if (!arenaGatePasses(points_.gates[i], pos, facing, from)) continue;
        index[out.count] = static_cast<std::uint8_t>(i);
        out.add(points_.points[i]);
    }
    return out;
}

CritResult CritWindow::land(const WeakPoints& g, const std::array<std::uint8_t, kMaxWeakPoints>& index, AimPoint at) {
    CritResult r;
    const ShotResult shot = judgeShot(g, at, AimTuning{});
    if (shot.kind != ShotKind::Weak) return r;
    r.crit = true;
    r.point = index[shot.weakPoint];
    r.multiplier = shot.multiplier;
    r.first = crits_ == 0;
    ++crits_;
    return r;
}

CritResult CritWindow::judge(Vec2 from, Vec2 pos, Vec2 facing, AimPoint at, double t) {
    if (!isOpen(t)) return {};
    std::array<std::uint8_t, kMaxWeakPoints> index{};
    return land(gated(from, pos, facing, index), index, at);
}

CritResult CritWindow::judgeLockOn(Vec2 from, Vec2 pos, Vec2 facing, double t) {
    if (!isOpen(t)) return {};
    std::array<std::uint8_t, kMaxWeakPoints> index{};
    const WeakPoints g = gated(from, pos, facing, index);
    const std::size_t b = g.best();
    if (b >= g.size()) return {};
    return land(g, index, AimPoint{g[b].x, g[b].y});
}

// ---- Phases ----------------------------------------------------------------------------------------

void ArenaBossPhaseTrack::reset(const ArenaBossPhase* phases, std::size_t count) {
    count_ = std::min(count, kMaxArenaBossPhases);
    for (std::size_t i = 0; i < count_; ++i) phases_[i] = phases[i];
    phase_ = 0;
}

int ArenaBossPhaseTrack::update(float hpShare) {
    int moved = 0;
    while (phase_ + 1 < count_ && hpShare <= phases_[phase_].endAt) {
        ++phase_;
        ++moved;
    }
    return moved;
}

// ---- The guest's calls -----------------------------------------------------------------------------

double GuestCaller::nextRepeat(double start, double everyBeats, double now) const {
    const double base = start + arenaBeats(shiftBeats(), bpm_);
    const double period = arenaBeats(everyBeats, bpm_);
    if (!(period > 0.0) || now <= base) return base;
    // The slack keeps a call that `now` sits on (up to rounding) from being skipped.
    const double k = std::ceil((now - base) / period - 1e-9);
    return base + k * period;
}

double GuestCaller::nearestRepeat(double start, double everyBeats, double t) const {
    const double base = start + arenaBeats(shiftBeats(), bpm_);
    const double period = arenaBeats(everyBeats, bpm_);
    if (!(period > 0.0) || t <= base) return base;
    const double a = base + std::floor((t - base) / period) * period;
    const double b = a + period;
    return t - a <= b - t ? a : b;
}

// ---- The keep-out ----------------------------------------------------------------------------------

float ArenaKeepOut::clampArc(Vec2 origin, Vec2 dir, float arcCos, float reach) const {
    if (!(reach > 0.0f)) return 0.0f; // NaN included
    if (none()) return reach;
    const Vec2 c = abSub(centre, origin);
    const float dist = abLen(c);
    if (dist <= radius) return 0.0f;
    const Vec2 u = abUnit(dir);
    // The nearest point of the disc lies along the line to its centre; when that bearing is outside
    // the arc, the nearest point inside the arc is on one of its two edges.
    if (arcCos <= -1.0f || (u.x == 0.0f && u.y == 0.0f) || abDot(c, u) >= arcCos * dist)
        return std::min(reach, dist - radius);
    const float s = std::sqrt(std::max(0.0f, 1.0f - arcCos * arcCos));
    float best = reach;
    for (const float side : {1.0f, -1.0f}) {
        // The edge: u turned by the half-arc, either way.
        const Vec2 r = abRight(u);
        const Vec2 e{u.x * arcCos + r.x * s * side, u.y * arcCos + r.y * s * side};
        const float along = abDot(c, e);
        if (along <= 0.0f) continue;
        const float off2 = dist * dist - along * along;
        if (off2 > radius * radius) continue;
        best = std::min(best, along - std::sqrt(radius * radius - off2));
    }
    return std::max(best, 0.0f);
}

float ArenaKeepOut::clampLane(Vec2 origin, Vec2 dir, float width, float reach) const {
    if (!(reach > 0.0f)) return 0.0f; // NaN included
    if (none()) return reach;
    const Vec2 u = abUnit(dir);
    if (u.x == 0.0f && u.y == 0.0f) return clampArc(origin, dir, -1.0f, reach);
    const Vec2 c = abSub(centre, origin);
    const float along = abDot(c, u);
    const float across = std::fabs(abDot(c, abRight(u)));
    const float gap = std::max(0.0f, across - std::max(width, 0.0f) * 0.5f);
    if (gap >= radius) return reach; // the disc is wholly to one side
    const float half = std::sqrt(radius * radius - gap * gap);
    if (along + half <= 0.0f) return reach; // wholly behind
    return std::clamp(along - half, 0.0f, reach);
}

Vec2 ArenaKeepOut::clampArea(Vec2 point, float areaRadius) const {
    if (none()) return point;
    const float need = radius + std::max(areaRadius, 0.0f);
    const Vec2 d = abSub(point, centre);
    const float dist = abLen(d);
    if (dist >= need) return point;
    if (dist < 1e-6f) return {centre.x + need, centre.y};
    return {centre.x + d.x / dist * need, centre.y + d.y / dist * need};
}

ArenaKeepOut arenaKeepOutAt(Vec2 centre, float yawDeg, float bearingDeg, float distance, float radius) {
    const float a = abRad(yawDeg + bearingDeg);
    ArenaKeepOut k;
    k.centre = {centre.x + distance * std::cos(a), centre.y + distance * std::sin(a)};
    k.radius = std::max(radius, 0.0f);
    return k;
}

// ---- The boss's data -------------------------------------------------------------------------------

std::uint8_t ArenaBossDef::find(std::string_view id) const {
    for (std::uint8_t i = 0; i < moveCount; ++i)
        if (moves[i].id() == id) return i;
    return kArenaBossNoMove;
}

namespace {

bool abId(std::string_view id, std::size_t maxLen) {
    if (id.empty() || id.size() > maxLen) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

void abUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                   std::initializer_list<std::string_view> more, std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end() &&
            std::find(more.begin(), more.end(), key) == more.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

std::string abJoin(const std::string& path, const char* key) { return path.empty() ? key : path + "." + key; }
std::string abAt(const std::string& path, std::uint32_t i) { return path + "[" + std::to_string(i) + "]"; }

std::string abRange(double lo, double hi) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g..%g", lo, hi);
    return buf;
}

// A number in [lo, hi] (required when `required`, else left alone when absent).
template <typename T>
void abNum(JsonValue j, const char* key, T& out, double lo, double hi, bool required, const std::string& path,
           std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent() && !required) return;
    const double d = v.number(-1e300);
    if (!v.isNumber() || !std::isfinite(d) || d < lo || d > hi) {
        errors.push_back(abJoin(path, key) + ": expected a number in " + abRange(lo, hi));
        return;
    }
    out = static_cast<T>(d);
}

void abInt(JsonValue j, const char* key, int& out, int lo, int hi, bool required, const std::string& path,
           std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent() && !required) return;
    if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
        errors.push_back(abJoin(path, key) + ": expected an integer in " + std::to_string(lo) + ".." +
                         std::to_string(hi));
        return;
    }
    out = static_cast<int>(v.integer());
}

void abBool(JsonValue j, const char* key, bool& out, const std::string& path, std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return;
    if (!v.isBool()) {
        errors.push_back(abJoin(path, key) + ": expected true or false");
        return;
    }
    out = v.boolean();
}

// An object (or absent when !required): false when it is absent or wrong (wrong is an error).
bool abObject(JsonValue v, const std::string& path, bool required, std::vector<std::string>& errors) {
    if (v.isObject()) return true;
    if (!v.isAbsent() || required) errors.push_back(path + ": expected an object");
    return false;
}

bool abAnswer(std::string_view s, ArenaBossAnswer& out) {
    constexpr std::pair<std::string_view, ArenaBossAnswer> names[] = {
        {"intercept", ArenaBossAnswer::Intercept}, {"dodge", ArenaBossAnswer::Dodge}, {"jump", ArenaBossAnswer::Jump},
        {"tile", ArenaBossAnswer::Tile},           {"cover", ArenaBossAnswer::Cover}, {"any", ArenaBossAnswer::Any}};
    for (const auto& [name, a] : names)
        if (s == name) {
            out = a;
            return true;
        }
    return false;
}

bool abZone(std::string_view s, CalledZoneRule& out) {
    if (s == "side") out = CalledZoneRule::Side;
    else if (s == "other") out = CalledZoneRule::Other;
    else if (s == "path") out = CalledZoneRule::Path;
    else return false;
    return true;
}

// A list of 1..max numbers in (0, hi], strictly rising. Returns how many were read (0 on any error).
std::uint8_t abBeats(JsonValue v, const std::string& path, std::array<float, kMaxCalledImpacts>& out, int hi,
                     std::vector<std::string>& errors) {
    if (!v.isArray() || v.size() < 1 || v.size() > kMaxCalledImpacts) {
        errors.push_back(path + ": expected an array of 1.." + std::to_string(kMaxCalledImpacts) + " beats");
        return 0;
    }
    bool ok = true;
    for (std::uint32_t i = 0; i < v.size(); ++i) {
        const double d = v[i].number(-1.0);
        if (!v[i].isNumber() || !std::isfinite(d) || !(d > 0.0) || d > hi) {
            errors.push_back(abAt(path, i) + ": expected a beat in (0, " + std::to_string(hi) + "]");
            ok = false;
            continue;
        }
        out[i] = static_cast<float>(d);
        if (i > 0 && ok && !(out[i] > out[i - 1])) {
            errors.push_back(abAt(path, i) + ": expected beats strictly rising");
            ok = false;
        }
    }
    return ok ? static_cast<std::uint8_t>(v.size()) : 0;
}

void abMove(JsonValue m, const std::string& path, const ArenaBossDef& def, ArenaBossMove& out,
            std::vector<std::string>& errors, std::vector<std::string>* warnings) {
    if (!abObject(m, path, true, errors)) return;
    abUnknownKeys(m, path,
                  {"id", "answer", "impacts", "calls", "zones", "sheds", "active", "recovery", "range", "damage",
                   "damageMax", "floor", "marked", "guardBreak", "guardStacks", "missStacks", "opens", "load",
                   "contact"},
                  {}, warnings);
    const std::string_view id = m["id"].string();
    if (!m["id"].isString() || !abId(id, kMaxArenaBossMoveId))
        errors.push_back(path + ".id: expected 1.." + std::to_string(kMaxArenaBossMoveId) +
                         " characters of a-z, 0-9 or _");
    else std::copy(id.begin(), id.end(), out.idChars.begin());

    if (!abAnswer(m["answer"].string(), out.answer))
        errors.push_back(path + ".answer: expected intercept, dodge, jump, tile, cover or any");
    const bool icpt = out.intercept();

    out.impactCount = abBeats(m["impacts"], path + ".impacts", out.impacts, 32, errors);
    const std::size_t n = out.impactCount;

    const JsonValue calls = m["calls"];
    if (!calls.isAbsent() && !(calls.isArray() && calls.size() == 0)) {
        const std::string cp = path + ".calls";
        if (!calls.isArray() || (n > 0 && calls.size() != n)) {
            errors.push_back(cp + ": expected one call per impact");
        } else if (n > 0) {
            for (std::uint32_t i = 0; i < calls.size(); ++i) {
                const double c = calls[i].number(-1.0);
                const std::string ip = abAt(cp, i);
                if (!calls[i].isNumber() || !std::isfinite(c)) {
                    errors.push_back(ip + ": expected a number of beats");
                    continue;
                }
                if (c < def.guestCall.fromBeats - kBeatSlack)
                    errors.push_back(ip + ": a call lands at least guestCall.fromBeats into the tell");
                if (out.impacts[i] - c < def.guestCall.minLeadBeats - kBeatSlack)
                    errors.push_back(ip + ": a call lands at least guestCall.minLeadBeats before its impact");
                if (i > 0 && !(c > out.calls[i - 1])) errors.push_back(ip + ": expected calls strictly rising");
                out.calls[i] = static_cast<float>(c);
            }
            out.callCount = static_cast<std::uint8_t>(n);
        }
    }

    const JsonValue zones = m["zones"];
    if (icpt) {
        if (!zones.isArray() || (n > 0 && zones.size() != n)) {
            errors.push_back(path + ".zones: expected one zone (side, other or path) per impact");
        } else {
            for (std::uint32_t i = 0; i < zones.size() && i < kMaxCalledImpacts; ++i)
                if (!abZone(zones[i].string(), out.zones[i]))
                    errors.push_back(abAt(path + ".zones", i) + ": expected side, other or path");
        }
    }
    const JsonValue sheds = m["sheds"];
    if (!sheds.isAbsent()) {
        if (!sheds.isArray() || (n > 0 && sheds.size() != n)) {
            errors.push_back(path + ".sheds: expected one true or false per impact");
        } else {
            for (std::uint32_t i = 0; i < sheds.size() && i < kMaxCalledImpacts; ++i) {
                if (!sheds[i].isBool()) errors.push_back(abAt(path + ".sheds", i) + ": expected true or false");
                out.sheds[i] = sheds[i].boolean();
            }
        }
    }

    abNum(m, "active", out.active, 0.05, 5.0, true, path, errors);
    abNum(m, "recovery", out.recovery, 0.0, 10.0, true, path, errors);
    abNum(m, "range", out.range, 0.0, 50.0, true, path, errors);
    // Both read as written, so a damageMax equal to damage passes (a float damage can round above it).
    double damage = 0.0;
    abNum(m, "damage", damage, 0.0, 1.0, true, path, errors);
    double damageMax = std::min(1.0, damage * static_cast<double>(std::max<std::size_t>(n, 1)));
    abNum(m, "damageMax", damageMax, damage, 1.0, false, path, errors);
    out.damage = static_cast<float>(damage);
    out.damageMax = static_cast<float>(damageMax);
    abBool(m, "floor", out.floor, path, errors);
    abBool(m, "marked", out.marked, path, errors);
    abNum(m, "guardBreak", out.guardBreak, 0.0, 5.0, false, path, errors);
    int stacks = 0;
    abInt(m, "guardStacks", stacks, 0, 5, false, path, errors);
    out.guardStacks = static_cast<std::uint8_t>(stacks);
    stacks = 0;
    abInt(m, "missStacks", stacks, 0, 5, false, path, errors);
    out.missStacks = static_cast<std::uint8_t>(stacks);
    abNum(m, "opens", out.opens, 0.0, 10.0, false, path, errors);
    abInt(m, "load", out.load, 0, kMaxLoad, false, path, errors);

    const JsonValue contact = m["contact"];
    const std::string cp = abJoin(path, "contact");
    if (abObject(contact, cp, false, errors)) {
        abUnknownKeys(contact, cp, {"socket", "ahead", "side", "up"}, {}, warnings);
        const std::string_view socket = contact["socket"].string();
        const std::size_t before = errors.size();
        if (!contact["socket"].isString() || !abId(socket, kMaxArenaBossMoveId))
            errors.push_back(cp + ".socket: expected 1.." + std::to_string(kMaxArenaBossMoveId) +
                             " characters of a-z, 0-9 or _");
        abNum(contact, "ahead", out.contactAhead, -20.0, 20.0, true, cp, errors);
        abNum(contact, "side", out.contactSide, -20.0, 20.0, true, cp, errors);
        abNum(contact, "up", out.contactUp, -5.0, 20.0, true, cp, errors);
        if (errors.size() == before) {
            std::copy(socket.begin(), socket.end(), out.contactSocketChars.begin());
            out.hasContact = true;
        }
    }

    if (!icpt) {
        for (const char* key : {"zones", "sheds", "marked", "opens", "load", "guardStacks", "missStacks"})
            if (m.has(key)) errors.push_back(abJoin(path, key) + ": only an intercept move has it");
    } else if (n > 1) {
        // Each impact must be answerable: the rearm runs out before the next one's window.
        for (std::size_t i = 1; i < n; ++i)
            if (!(arenaBeats(out.impacts[i] - out.impacts[i - 1], def.bpm) > def.intercept.rearm))
                errors.push_back(abAt(path + ".impacts", static_cast<std::uint32_t>(i)) +
                                 ": an intercept impact comes more than intercept.rearm after the one before");
    }
}

void abTable(JsonValue v, const std::string& path, const ArenaBossDef& def, ArenaBossMoveTable& out,
             std::vector<std::string>& errors) {
    if (!v.isArray() || v.size() < 1 || v.size() > kMaxArenaPhaseMoves) {
        errors.push_back(path + ": expected an array of 1.." + std::to_string(kMaxArenaPhaseMoves) + " move ids");
        return;
    }
    for (std::uint32_t i = 0; i < v.size(); ++i) {
        const std::uint8_t m = def.find(v[i].string());
        if (!v[i].isString() || m == kArenaBossNoMove) errors.push_back(abAt(path, i) + ": no such move");
        else if (out.contains(m)) errors.push_back(abAt(path, i) + ": the move is already in the table");
        else out.add(m);
    }
}

} // namespace

bool importArenaWeakPoints(const JsonValue& v, const std::string& path, ArenaWeakPoints& out,
                           std::vector<std::string>& errors, std::vector<std::string>* warnings) {
    out = ArenaWeakPoints{};
    const std::size_t before = errors.size();
    if (!abObject(v, path, true, errors)) return false;
    abUnknownKeys(v, path, {"halfWidth", "points"}, {}, warnings);
    WeakPoints& wp = out.points;
    abNum(v, "halfWidth", wp.halfWidth, 0.0, 1.0, false, path, errors);
    if (!(wp.halfWidth > 0.0f)) errors.push_back(abJoin(path, "halfWidth") + ": expected a number above 0");
    const JsonValue list = v["points"];
    const std::string lp = abJoin(path, "points");
    if (!list.isAbsent() && (!list.isArray() || list.size() > kMaxWeakPoints)) {
        errors.push_back(lp + ": expected an array of 0.." + std::to_string(kMaxWeakPoints) + " points");
    } else {
        for (std::uint32_t i = 0; i < list.size(); ++i) {
            const JsonValue p = list[i];
            const std::string pp = abAt(lp, i);
            if (!abObject(p, pp, true, errors)) continue;
            abUnknownKeys(p, pp, {"id", "x", "y", "radius", "multiplier", "facing", "arc"}, {}, warnings);
            WeakPoint w;
            const std::string_view id = p["id"].string();
            if (!p["id"].isString() || !abId(id, kWeakPointIdCapacity - 1))
                errors.push_back(pp + ".id: expected 1.." + std::to_string(kWeakPointIdCapacity - 1) +
                                 " characters of a-z, 0-9 or _");
            for (const WeakPoint& other : wp)
                if (other.id() == id) errors.push_back(pp + ".id: duplicate id " + std::string(id));
            w.setId(id);
            abNum(p, "x", w.x, -1.0, 1.0, true, pp, errors);
            abNum(p, "y", w.y, 0.0, 1.0, true, pp, errors);
            abNum(p, "radius", w.radius, kMinWeakRadius, kMaxWeakRadius, true, pp, errors);
            abNum(p, "multiplier", w.multiplier, kMinWeakMultiplier, kMaxWeakMultiplier, true, pp, errors);
            ArenaWeakGate& g = out.gates[wp.size()];
            abNum(p, "facing", g.facing, 0.0, 360.0, false, pp, errors);
            abNum(p, "arc", g.arc, 0.0, 180.0, false, pp, errors);
            wp.add(w);
        }
    }
    return errors.size() == before;
}

bool importArenaBoss(std::string_view json, ArenaBossDef& out, std::string* error, std::vector<std::string>* warnings,
                     std::initializer_list<std::string_view> ownKeys) {
    out = ArenaBossDef{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("arena boss: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("arena boss: expected an object");
    } else {
        const JsonValue root = doc.root();
        if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
            errors.push_back("version: expected 1");
        abUnknownKeys(root, "",
                      {"version", "bpm", "hp", "poise", "staggerTime", "openTime", "intercept", "guestCall",
                       "weakPoints", "keepOut", "moves", "phases"},
                      ownKeys, warnings);
        abNum(root, "bpm", out.bpm, 40.0, 240.0, true, "", errors);
        abInt(root, "hp", out.hp, 1, 100000, true, "", errors);
        abNum(root, "poise", out.poise, 1.0, 10000.0, true, "", errors);
        abNum(root, "staggerTime", out.staggerTime, 0.1, 10.0, true, "", errors);
        abNum(root, "openTime", out.openTime, 0.1, 10.0, true, "", errors);

        if (const JsonValue v = root["intercept"]; abObject(v, "intercept", false, errors)) {
            abUnknownKeys(v, "intercept", {"window", "rearm"}, {}, warnings);
            abNum(v, "window", out.intercept.window, 0.05, 0.5, false, "intercept", errors);
            abNum(v, "rearm", out.intercept.rearm, 0.0, 2.0, false, "intercept", errors);
        }
        if (const JsonValue v = root["guestCall"]; abObject(v, "guestCall", false, errors)) {
            abUnknownKeys(v, "guestCall", {"fromBeats", "minLeadBeats", "missShiftBeats"}, {}, warnings);
            abNum(v, "fromBeats", out.guestCall.fromBeats, 0.0, 8.0, false, "guestCall", errors);
            abNum(v, "minLeadBeats", out.guestCall.minLeadBeats, 0.5, 8.0, false, "guestCall", errors);
            abNum(v, "missShiftBeats", out.guestCall.missShiftBeats, 0.0, 8.0, false, "guestCall", errors);
        }
        if (!(out.guestCall.missShiftBeats < out.guestCall.minLeadBeats))
            errors.push_back("guestCall.missShiftBeats: expected below guestCall.minLeadBeats");

        if (const JsonValue v = root["weakPoints"]; !v.isAbsent())
            importArenaWeakPoints(v, "weakPoints", out.weakPoints, errors, warnings);
        if (const JsonValue v = root["keepOut"]; abObject(v, "keepOut", false, errors)) {
            abUnknownKeys(v, "keepOut", {"bearing", "distance", "radius"}, {}, warnings);
            abNum(v, "bearing", out.keepOutBearing, 0.0, 360.0, true, "keepOut", errors);
            abNum(v, "distance", out.keepOutDistance, 0.0, 100.0, true, "keepOut", errors);
            abNum(v, "radius", out.keepOutRadius, 0.0, 20.0, true, "keepOut", errors);
        }

        const JsonValue moves = root["moves"];
        if (!moves.isArray() || moves.size() < 1 || moves.size() > kMaxArenaBossMoves) {
            errors.push_back("moves: expected an array of 1.." + std::to_string(kMaxArenaBossMoves) + " moves");
        } else {
            for (std::uint32_t i = 0; i < moves.size(); ++i) {
                ArenaBossMove m;
                const std::string mp = abAt("moves", i);
                abMove(moves[i], mp, out, m, errors, warnings);
                if (!m.id().empty() && out.find(m.id()) != kArenaBossNoMove)
                    errors.push_back(mp + ".id: duplicate id " + std::string(m.id()));
                out.moves[out.moveCount++] = m;
            }
        }

        const JsonValue phases = root["phases"];
        if (!phases.isArray() || phases.size() < 1 || phases.size() > kMaxArenaBossPhases) {
            errors.push_back("phases: expected an array of 1.." + std::to_string(kMaxArenaBossPhases) + " phases");
        } else {
            for (std::uint32_t i = 0; i < phases.size(); ++i) {
                const JsonValue p = phases[i];
                const std::string pp = abAt("phases", i);
                ArenaBossPhase& ph = out.phases[out.phaseCount++];
                if (!abObject(p, pp, true, errors)) continue;
                abUnknownKeys(p, pp, {"endAt", "moves", "brokenMoves"}, {}, warnings);
                ph.endAt = -1.0f;
                abNum(p, "endAt", ph.endAt, 0.0, 0.999999, true, pp, errors);
                if (i > 0 && ph.endAt >= 0.0f && !(ph.endAt < out.phases[i - 1].endAt))
                    errors.push_back(pp + ".endAt: expected below the phase before");
                if (i + 1 == phases.size() && ph.endAt != 0.0f)
                    errors.push_back(pp + ".endAt: the last phase ends at 0");
                abTable(p["moves"], pp + ".moves", out, ph.whole, errors);
                if (p.has("brokenMoves")) abTable(p["brokenMoves"], pp + ".brokenMoves", out, ph.broken, errors);
                else ph.broken = ph.whole;
            }
        }
    }
    if (errors.empty()) return true;
    out = ArenaBossDef{};
    if (error) {
        error->clear();
        for (const std::string& e : errors) {
            if (!error->empty()) *error += '\n';
            *error += e;
        }
    }
    return false;
}

bool importArenaWeakPoints(std::string_view json, ArenaWeakPoints& out, std::string* error,
                           std::vector<std::string>* warnings) {
    out = ArenaWeakPoints{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("arena weak points: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("arena weak points: expected an object");
    } else {
        const JsonValue root = doc.root();
        if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
            errors.push_back("version: expected 1");
        abUnknownKeys(root, "", {"version", "weakPoints"}, {}, warnings);
        importArenaWeakPoints(root["weakPoints"], "weakPoints", out, errors, warnings);
    }
    if (errors.empty()) return true;
    out = ArenaWeakPoints{};
    if (error) {
        error->clear();
        for (const std::string& e : errors) {
            if (!error->empty()) *error += '\n';
            *error += e;
        }
    }
    return false;
}

// ---- The run ---------------------------------------------------------------------------------------

void ArenaBossRun::begin(const ArenaBossDef& def, double now) {
    def_ = &def;
    phases_.reset(def.phases.data(), def.phaseCount);
    breakable_ = {};
    strike_ = CalledStrike{};
    now_ = std::isfinite(now) ? now : 0.0;
    strike_.setWindows(def.intercept);
    strike_.update(now_);
    guest_.reset(def.bpm, def.guestCall);
    crit_.reset(def.weakPoints);
    events_.clear();
    live_ = kArenaBossNoMove;
    liveStart_ = now_;
    cueCount_ = cueNext_ = 0;
    critWasOpen_ = false;
    missed_ = false;
}

void ArenaBossRun::cancelMove() {
    strike_.cancel();
    cueNext_ = cueCount_;
    live_ = kArenaBossNoMove;
}

void ArenaBossRun::absorb(const CalledResults& results) {
    for (const CalledResult& r : results) {
        ArenaBossEvent e;
        e.type = ArenaBossEventType::Answered;
        e.move = live_;
        e.impact = r.impact;
        e.outcome = r.outcome;
        e.at = r.at;
        const bool miss = r.outcome == CalledOutcome::Missed || r.outcome == CalledOutcome::WrongZone;
        const bool firstMiss = miss && !missed_;
        if (firstMiss) {
            missed_ = true;
            e.value = 1;
        }
        events_.push(e);
        if (live_ == kArenaBossNoMove) continue;
        const ArenaBossMove& m = def_->moves[live_];
        if (firstMiss && m.marked && guest_.onMarkedMiss())
            events_.push({ArenaBossEventType::CallsShifted, live_, r.impact, r.outcome, 0, 0, r.at, 0.0});
        if (r.impact + 1u != strike_.size()) continue;
        // The move's last impact: every impact turned opens the window.
        bool all = true;
        for (std::size_t i = 0; i < strike_.size(); ++i) all = all && strike_.outcome(i) == CalledOutcome::Intercepted;
        if (all) {
            if (m.opens > 0.0f) {
                crit_.open(r.at, m.opens);
                critWasOpen_ = true;
                events_.push(
                    {ArenaBossEventType::CritOpened, live_, r.impact, r.outcome, 0, 0, r.at, crit_.closesAt()});
            }
            if (m.load > 0)
                events_.push({ArenaBossEventType::LoadGained, live_, r.impact, r.outcome, m.load, 0, r.at, 0.0});
        }
    }
}

void ArenaBossRun::advance(double t) {
    t = timeOf(t);
    for (;;) {
        const double call = cueNext_ < cueCount_ ? cues_[cueNext_] : kNever;
        const double close = critWasOpen_ ? crit_.closesAt() : kNever;
        const double hit = strike_.complete() ? kNever : strike_.impact(strike_.nextIndex()).at;
        const double first = std::min({call, close, hit});
        if (!(first <= t)) break; // nothing due (kNever and NaN included)
        if (call == first) {
            events_.push({ArenaBossEventType::Call, live_, cueNext_, CalledOutcome::Pending, 0, 0, call, 0.0});
            ++cueNext_;
        } else if (close == first) {
            critWasOpen_ = false;
            events_.push(
                {ArenaBossEventType::CritClosed, kArenaBossNoMove, 0, CalledOutcome::Pending, 0, 0, close, 0.0});
        } else {
            absorb(strike_.update(hit));
        }
    }
    strike_.update(t);
    now_ = t;
}

const ArenaBossEvents& ArenaBossRun::startMove(std::uint8_t move, double t, CalledZone side, bool* started) {
    events_.clear();
    advance(t);
    const bool ok = move < def_->moveCount && table().contains(move);
    if (started) *started = ok;
    if (!ok) return events_;
    cancelMove();
    const ArenaBossMove& m = def_->moves[move];
    live_ = move;
    liveStart_ = now_;
    missed_ = false;
    cueCount_ = m.callCount;
    cueNext_ = 0;
    for (std::size_t k = 0; k < m.callCount; ++k) cues_[k] = guest_.cue(now_, m.calls[k]);
    std::array<CalledImpact, kMaxCalledImpacts> impacts{};
    std::size_t n = 0;
    if (m.intercept()) {
        const CalledZone other = side == CalledZone::Left ? CalledZone::Right
                                 : side == CalledZone::Right ? CalledZone::Left
                                                             : CalledZone::Path;
        for (; n < m.impactCount; ++n) {
            impacts[n].at = now_ + arenaBeats(m.impacts[n], def_->bpm);
            impacts[n].zone = m.zones[n] == CalledZoneRule::Side    ? side
                              : m.zones[n] == CalledZoneRule::Other ? other
                                                                    : CalledZone::Path;
            impacts[n].sheds = m.sheds[n];
        }
    }
    strike_.start(impacts.data(), n);
    return events_;
}

const ArenaBossEvents& ArenaBossRun::update(double now, float hpShare) {
    events_.clear();
    now = timeOf(now);
    if (phases_.update(hpShare) > 0) {
        guest_.onPhaseChange();
        events_.push({ArenaBossEventType::PhaseChanged, kArenaBossNoMove, 0, CalledOutcome::Pending,
                      static_cast<int>(phases_.phase()), 0, now, 0.0});
    }
    advance(now);
    return events_;
}

const ArenaBossEvents& ArenaBossRun::interceptDown(CalledZone zone, double t) {
    events_.clear();
    advance(t);
    absorb(strike_.interceptDown(zone, now_));
    return events_;
}

const ArenaBossEvents& ArenaBossRun::interceptUp(double t) {
    events_.clear();
    advance(t);
    absorb(strike_.interceptUp(now_));
    return events_;
}

const ArenaBossEvents& ArenaBossRun::guardDown(CalledZone zone, double t) {
    events_.clear();
    advance(t);
    absorb(strike_.guardDown(zone, now_));
    return events_;
}

const ArenaBossEvents& ArenaBossRun::guardUp(double t) {
    events_.clear();
    advance(t);
    absorb(strike_.guardUp(now_));
    return events_;
}

const ArenaBossEvents& ArenaBossRun::onStagger(double t) {
    events_.clear();
    advance(t);
    staggerNow();
    return events_;
}

void ArenaBossRun::staggerNow() {
    if (breakable_.onStagger(now_))
        events_.push({ArenaBossEventType::Broke, pending() ? live_ : kArenaBossNoMove, 0, CalledOutcome::Pending, 0,
                      0, now_, 0.0});
    cancelMove();
}

bool ArenaBossRun::breakNow() {
    if (!breakable_.onStagger(now_)) return false;
    events_.push({ArenaBossEventType::Broke, kArenaBossNoMove, 0, CalledOutcome::Pending, 0, 0, now_, 0.0});
    return true;
}

const ArenaBossEvents& ArenaBossRun::onHit(Vec2 from, Vec2 pos, Vec2 facing, double t, const AimPoint* at,
                                           CritResult* result) {
    events_.clear();
    advance(t);
    const CritResult r = at ? crit_.judge(from, pos, facing, *at, now_) : crit_.judgeLockOn(from, pos, facing, now_);
    if (r.crit)
        events_.push({ArenaBossEventType::Crit, kArenaBossNoMove, r.point, CalledOutcome::Pending, r.first ? 1 : 0, 0,
                      now_, 0.0});
    if (result) *result = r;
    return events_;
}

} // namespace ftcore
