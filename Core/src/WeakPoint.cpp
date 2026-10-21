#include "ftcore/WeakPoint.h"

#include "ftcore/CombatTypes.h"
#include "ftcore/Encounter.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace ftcore {

bool sameWeakPoints(const WeakPoints& a, const WeakPoints& b) {
    if (a.count != b.count || a.halfWidth != b.halfWidth) return false;
    for (std::size_t i = 0; i < a.count; ++i) {
        const WeakPoint& x = a.items[i];
        const WeakPoint& y = b.items[i];
        if (x.id() != y.id() || x.x != y.x || x.y != y.y || x.radius != y.radius || x.multiplier != y.multiplier)
            return false;
    }
    return true;
}

const char* shotKindName(ShotKind k) {
    switch (k) {
    case ShotKind::Weak: return "WEAK POINT";
    case ShotKind::Body: return "HIT";
    default: return "GRAZE";
    }
}

ShotResult judgeShot(const WeakPoints& points, AimPoint at, const AimTuning& tuning) {
    ShotResult r;
    r.at = at;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const WeakPoint& p = points[i];
        const float dx = at.x - p.x;
        const float dy = at.y - p.y;
        if (dx * dx + dy * dy > p.radius * p.radius) continue;
        if (r.kind != ShotKind::Weak || p.multiplier > r.multiplier) {
            r.kind = ShotKind::Weak;
            r.weakPoint = static_cast<std::uint8_t>(i);
            r.multiplier = p.multiplier;
        }
    }
    if (r.kind == ShotKind::Weak) return r;
    const bool onBody = std::fabs(at.x) <= points.halfWidth && at.y >= 0.0f && at.y <= 1.0f;
    r.kind = onBody ? ShotKind::Body : ShotKind::Graze;
    r.multiplier = onBody ? tuning.bodyMultiplier : tuning.grazeMultiplier;
    return r;
}

void AimShot::reset(const WeakPoints& points, double start, const AimTuning& tuning) {
    points_ = points;
    if (points_.count > kMaxWeakPoints) points_.count = static_cast<std::uint8_t>(kMaxWeakPoints);
    tuning_ = tuning;
    start_ = start;
    reticle_ = AimPoint{0.0f, 0.5f};
}

void AimShot::move(float dx, float dy) { set(reticle_.x + dx, reticle_.y + dy); }

void AimShot::set(float x, float y) {
    reticle_.x = std::clamp(x, minX(), maxX());
    reticle_.y = std::clamp(y, minY(), maxY());
}

AimPoint AimShot::sway(double t) const {
    constexpr double kTau = 2.0 * std::numbers::pi;
    const double s = t - start_;
    AimPoint out;
    out.x = tuning_.swayPeriodX > 0.0 ? tuning_.swayX * static_cast<float>(std::sin(kTau * s / tuning_.swayPeriodX)) : 0.0f;
    out.y = tuning_.swayPeriodY > 0.0 ? tuning_.swayY * static_cast<float>(std::sin(kTau * s / tuning_.swayPeriodY)) : 0.0f;
    return out;
}

AimPoint AimShot::aimPoint(double t) const {
    const AimPoint s = sway(t);
    return AimPoint{reticle_.x + s.x, reticle_.y + s.y};
}

AimPoint AimShot::reticleFor(std::size_t i, double t) const {
    const AimPoint goal = i < points_.size() ? AimPoint{points_[i].x, points_[i].y} : AimPoint{0.0f, 0.5f};
    const AimPoint s = sway(t);
    return AimPoint{goal.x - s.x, goal.y - s.y};
}

const WeakPoints* WeakPointTable::find(std::string_view enemyId) const {
    for (const WeakPointEntry& e : entries)
        if (e.enemy == enemyId) return &e.points;
    return nullptr;
}

namespace {

bool wpId(std::string_view id) {
    if (id.empty() || id.size() >= kWeakPointIdCapacity) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

void wpUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                   std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back(path + ": unknown key: " + std::string(key));
    }
}

// A number in [lo, hi] (required when `required`, else left alone when absent).
void wpNum(JsonValue j, const char* key, float& out, float lo, float hi, bool required, const std::string& path,
           std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent() && !required) return;
    const double d = v.number(-1e30);
    if (!v.isNumber() || !std::isfinite(d) || d < lo || d > hi) {
        errors.push_back(path + "." + key + ": expected a number in " + std::to_string(lo) + ".." + std::to_string(hi));
        return;
    }
    out = static_cast<float>(d);
}

void wpEntry(JsonValue e, const std::string& path, WeakPoints& out, std::vector<std::string>& errors,
             std::vector<std::string>* warnings) {
    if (!e.isObject()) {
        errors.push_back(path + ": expected an object");
        return;
    }
    wpUnknownKeys(e, path, {"halfWidth", "points"}, warnings);
    out.halfWidth = kDefaultHalfWidth;
    wpNum(e, "halfWidth", out.halfWidth, 0.0f, 1.0f, false, path, errors);
    if (!(out.halfWidth > 0.0f)) errors.push_back(path + ".halfWidth: expected a number above 0");
    const JsonValue list = e["points"];
    if (list.isAbsent()) return;
    const std::string lp = path + ".points";
    if (!list.isArray() || list.size() > kMaxWeakPoints) {
        errors.push_back(lp + ": expected an array of 0.." + std::to_string(kMaxWeakPoints) + " points");
        return;
    }
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const JsonValue p = list[i];
        const std::string pp = lp + "[" + std::to_string(i) + "]";
        if (!p.isObject()) {
            errors.push_back(pp + ": expected an object");
            continue;
        }
        wpUnknownKeys(p, pp, {"id", "x", "y", "radius", "multiplier"}, warnings);
        WeakPoint w;
        const std::string_view id = p["id"].string();
        if (!wpId(id)) errors.push_back(pp + ".id: expected 1.." + std::to_string(kWeakPointIdCapacity - 1) +
                                        " characters of a-z, 0-9 or _");
        for (const WeakPoint& other : out)
            if (other.id() == id) errors.push_back(pp + ".id: duplicate id " + std::string(id));
        w.setId(id);
        wpNum(p, "x", w.x, -1.0f, 1.0f, true, pp, errors);
        wpNum(p, "y", w.y, 0.0f, 1.0f, true, pp, errors);
        wpNum(p, "radius", w.radius, kMinWeakRadius, kMaxWeakRadius, true, pp, errors);
        wpNum(p, "multiplier", w.multiplier, kMinWeakMultiplier, kMaxWeakMultiplier, true, pp, errors);
        out.add(w);
    }
}

} // namespace

bool importWeakPointTable(std::string_view json, const EnemyRoster& roster, WeakPointTable& out, std::string* error,
                          std::vector<std::string>* warnings) {
    out.entries.clear();
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("weakpoints: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("weakpoints: expected an object");
    } else {
        const JsonValue root = doc.root();
        if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
            errors.push_back("version: expected 1");
        wpUnknownKeys(root, "weakpoints", {"version", "weakpoints"}, warnings);
        const JsonValue table = root["weakpoints"];
        if (!table.isObject()) {
            errors.push_back("weakpoints: expected an object keyed by enemy id");
        } else {
            out.entries.reserve(table.size());
            for (std::uint32_t i = 0; i < table.size(); ++i) {
                const std::string_view enemy = table.key(i);
                const std::string path = "weakpoints." + std::string(enemy);
                if (out.find(enemy)) errors.push_back(path + ": duplicate enemy id");
                if (roster.find(enemy) < 0 && warnings) warnings->push_back(path + ": no such enemy in the roster");
                WeakPointEntry& entry = out.entries.emplace_back();
                entry.enemy = std::string(enemy);
                wpEntry(table.value(i), path, entry.points, errors, warnings);
            }
        }
    }
    if (errors.empty()) return true;
    out.entries.clear();
    if (error) {
        error->clear();
        for (const std::string& e : errors) {
            if (!error->empty()) *error += '\n';
            *error += e;
        }
    }
    return false;
}

void applyWeakPoints(const WeakPointTable& table, const EnemyRoster& roster, const EncounterLineup& lineup,
                     std::vector<Combatant>& foes) {
    const std::size_t n = std::min(foes.size(), lineup.enemies.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t index = lineup.enemies[i];
        const WeakPoints* points = index < roster.enemies.size() ? table.find(roster.enemies[index].id) : nullptr;
        foes[i].weakPoints = points ? *points : WeakPoints{};
    }
}

} // namespace ftcore
