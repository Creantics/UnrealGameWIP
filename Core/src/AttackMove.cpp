#include "ftcore/AttackMove.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

const char* const kKindNames[3] = {"focus", "fire", "cast"};

bool readTune(const JsonValue& v, AttackMoveTune& t, std::string& why, std::string& where) {
    if (!v.isObject()) {
        why = "not an object";
        return false;
    }
    struct Field {
        const char* name;
        float* value;
        double lo, hi;
    };
    const Field fields[] = {{"startupScale", &t.startupScale, 0.0, 1.0}, {"activeScale", &t.activeScale, 0.0, 1.0},
                            {"recoveryScale", &t.recoveryScale, 0.0, 1.0}, {"turnRate", &t.turnRate, 0.5, 60.0},
                            {"commit", &t.commit, 0.0, 2.0}};
    for (std::uint32_t i = 0; i < v.size(); ++i) {
        const std::string_view k = v.key(i);
        bool known = k == "recoveryRamp";
        for (const Field& f : fields) known = known || k == f.name;
        if (!known) {
            where = std::string(k);
            why = "not a known tunable";
            return false;
        }
    }
    for (const Field& f : fields) {
        const JsonValue x = v[f.name];
        if (x.isAbsent()) continue;
        where = f.name;
        if (!x.isNumber()) {
            why = "not a number";
            return false;
        }
        const double d = x.number();
        if (!(d >= f.lo && d <= f.hi)) {
            why = "out of range";
            return false;
        }
        *f.value = static_cast<float>(d);
    }
    const JsonValue ramp = v["recoveryRamp"];
    if (!ramp.isAbsent()) {
        where = "recoveryRamp";
        if (!ramp.isBool()) {
            why = "not a bool";
            return false;
        }
        t.recoveryRamp = ramp.boolean();
    }
    return true;
}

} // namespace

bool importAttackMoveRules(std::string_view json, AttackMoveRules& out, std::string* error) {
    JsonDocument doc;
    if (!doc.parse(json, error)) return false;
    const JsonValue root = doc.root();
    const auto fail = [&](const std::string& key, const std::string& why) {
        if (error) *error = "attack_move" + (key.empty() ? std::string() : "." + key) + ": " + why;
        return false;
    };
    if (!root.isObject()) return fail("", "not an object");
    AttackMoveRules r = out;
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "light" && k != "heavy" && k != "upperNone" && k != "upperFull")
            return fail(std::string(k), "not a known tunable");
    }
    for (int h = 0; h < 2; ++h) {
        const char* group = h ? "heavy" : "light";
        const JsonValue g = root[group];
        if (g.isAbsent()) continue;
        if (!g.isObject()) return fail(group, "not an object");
        for (std::uint32_t i = 0; i < g.size(); ++i) {
            const std::string_view k = g.key(i);
            if (k != kKindNames[0] && k != kKindNames[1] && k != kKindNames[2])
                return fail(std::string(group) + "." + std::string(k), "not a weapon (focus, fire, cast)");
        }
        for (int kind = 0; kind < 3; ++kind) {
            const JsonValue v = g[kKindNames[kind]];
            if (v.isAbsent()) continue;
            std::string why, where;
            AttackMoveTune& t = h ? r.heavy[kind] : r.light[kind];
            if (!readTune(v, t, why, where))
                return fail(std::string(group) + "." + kKindNames[kind] + (where.empty() ? "" : "." + where), why);
        }
    }
    const char* const edges[2] = {"upperNone", "upperFull"};
    float* const edgeValues[2] = {&r.upperNone, &r.upperFull};
    for (int i = 0; i < 2; ++i) {
        const JsonValue v = root[edges[i]];
        if (v.isAbsent()) continue;
        if (!v.isNumber()) return fail(edges[i], "not a number");
        const double d = v.number();
        if (!(d >= 0.0 && d <= 10.0)) return fail(edges[i], "out of range");
        *edgeValues[i] = static_cast<float>(d);
    }
    if (!(r.upperNone < r.upperFull)) return fail("upperFull", "not above upperNone");
    out = r;
    return true;
}

void attackMoveInput(const AttackMoveRules& rules, const ArenaFight& fight, ArenaInput& in) {
    const ArenaPlayerOut& last = fight.output().player;
    const bool light = last.action == ArenaAction::Light;
    if (!light && last.action != ArenaAction::Heavy && last.action != ArenaAction::Riposte && last.action != ArenaAction::Release)
        return;
    if (light && last.move == kArenaAirMove) return; // an air light moves on the jump's air control, not a walk
    const ArenaAttack* attack = arenaPlayerAttack(fight.def().fighter, last.move);
    if (!attack) return;
    const AttackMoveOut m = attackMove(rules, aimKindOfMove(fight.def().fighter, last.move), !light, *attack, last.actionTime);
    in.attackSpeed = m.speedScale;
    in.attackTurn = m.turnRate;
}

AttackMoveOut attackMove(const AttackMoveRules& rules, AimKind kind, bool heavy, const ArenaAttack& attack, float t) {
    AttackMoveOut o;
    const int k = static_cast<int>(kind) < 3 ? static_cast<int>(kind) : 0;
    const AttackMoveTune& tune = heavy ? rules.heavy[k] : rules.light[k];
    const float activeEnd = attack.startup + attack.active;
    const float total = attack.total();
    if (!(t >= 0.f) || t >= total) return o;
    // The cancel point: when another action may start (the `cancel` share of the recovery); the walk is free from it.
    const float cancelAt = activeEnd + std::clamp(attack.cancel, 0.f, 1.f) * attack.recovery;
    o.turnRate = std::max(tune.turnRate, 0.5f);
    if (tune.recoveryRamp && t >= activeEnd) {
        const float s = attack.recovery > 0.f ? (t - activeEnd) / attack.recovery : 1.f;
        o.speedScale = tune.recoveryScale + (1.f - tune.recoveryScale) * s;
    }
    else if (t >= cancelAt) o.speedScale = 1.f;
    else if (t >= activeEnd) o.speedScale = tune.recoveryScale;
    else if (t >= attack.startup - tune.commit) o.speedScale = tune.activeScale;
    else o.speedScale = tune.startupScale;
    o.speedScale = std::clamp(o.speedScale, 0.f, 1.f);
    return o;
}

float attackUpperShare(const AttackMoveRules& rules, float speed) {
    if (!(speed > rules.upperNone)) return 0.f;
    if (speed >= rules.upperFull) return 1.f;
    const float s = (speed - rules.upperNone) / (rules.upperFull - rules.upperNone);
    return s * s * (3.f - 2.f * s);
}

Vec2 turnCapped(Vec2 from, Vec2 to, float maxRad) {
    const float tl = std::sqrt(to.x * to.x + to.y * to.y);
    if (!(tl > 1e-6f)) return from;
    const Vec2 want{to.x / tl, to.y / tl};
    const float a0 = std::atan2(from.y, from.x);
    const float a1 = std::atan2(want.y, want.x);
    float d = a1 - a0;
    const float pi = 3.14159265358979f;
    while (d > pi) d -= 2.f * pi;
    while (d < -pi) d += 2.f * pi;
    const float cap = maxRad > 0.f ? maxRad : 0.f;
    if (std::fabs(d) <= cap) return want;
    const float a = a0 + (d > 0.f ? cap : -cap);
    return Vec2{std::cos(a), std::sin(a)};
}

} // namespace ftcore
