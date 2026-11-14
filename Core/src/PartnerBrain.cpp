#include "ftcore/PartnerBrain.h"

#include "ftcore/FightSession.h"
#include "ftcore/Json.h"
#include "ftcore/StateHash.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ftcore {
namespace {

using View = PartnerEnemyView;

float dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }
// From p to the enemy's edge (0 inside it).
float edge(Vec2 p, const View& e) { return std::max(0.f, dist(p, e.pos) - e.radius); }
// A view with a position and radius that are numbers (NaN or an infinity: absent, as if not there).
bool present(const View& e) {
    return std::isfinite(e.pos.x) && std::isfinite(e.pos.y) && std::isfinite(e.radius) && e.radius >= 0.f;
}
bool standing(const View& e) { return present(e) && e.state != View::State::Down; }
bool windingAt(const View& e) {
    return present(e) && e.state == View::State::WindingUp && e.aim == View::Aim::Partner;
}
// The point `d` from c toward p (along +x when p is on c).
Vec2 toward(Vec2 c, Vec2 p, float d) {
    const float l = dist(c, p);
    if (l < 1e-4f) return {c.x + d, c.y};
    return {c.x + (p.x - c.x) / l * d, c.y + (p.y - c.y) / l * d};
}
std::uint32_t mix(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h != 0 ? h : 1u;
}
// In float, as the tuning is held (so 0.01 in the data is at its 0.01 bound). NaN is out.
bool pbWithin(float v, float lo, float hi) { return v >= lo && v <= hi; }

} // namespace

bool validPartnerTuning(const PartnerTuning& t) {
    return pbWithin(t.leashMeters, 1.f, 10.f) && pbWithin(t.returnMeters, 0.5f, t.leashMeters) &&
           pbWithin(t.reachMeters, 0.5f, 4.f) && pbWithin(t.standoffMeters, 0.f, t.reachMeters) && t.blowsMin >= 1 &&
           t.blowsMin <= 6 && t.blowsMax >= t.blowsMin && t.blowsMax <= 6 && pbWithin(t.blowSeconds, 0.1f, 2.f) &&
           pbWithin(t.pauseMin, 0.f, 10.f) && pbWithin(t.pauseMax, t.pauseMin, 10.f) &&
           pbWithin(t.guardWarning, 0.f, 2.f) && pbWithin(t.guardChance, 0.f, 1.f) && pbWithin(t.guardHold, 0.f, 2.f) &&
           pbWithin(t.downSeconds, 1.f, 600.f) && pbWithin(t.riseHealth, 0.01f, 1.f) &&
           pbWithin(t.blowCapShare, 0.f, 1.f) && pbWithin(t.overScale, 0.f, 1.f) &&
           pbWithin(t.budgetWindowSeconds, 5.f, 30.f) && pbWithin(t.budgetShare, 0.f, 1.f) &&
           pbWithin(t.budgetAllowanceSeconds, 0.f, 10.f);
}

static_assert(kMaxPartnerStep == kMaxFightStep, "the brain's step bound is the fight's");

bool PartnerBrain::begin(const PartnerTuning& t, std::uint32_t seed) {
    const bool ok = validPartnerTuning(t);
    s_ = PartnerBrainSave{};
    s_.t = ok ? t : PartnerTuning{};
    s_.rhythm = mix(seed);
    s_.defence = mix(seed ^ 0xA5C3F00Du);
    s_.begun = true;
    return ok;
}

std::uint64_t PartnerBrainSave::hash() const {
    StateHash h;
    h.add(t.leashMeters).add(t.returnMeters).add(t.reachMeters).add(t.standoffMeters).add(t.blowsMin).add(t.blowsMax);
    h.add(t.blowSeconds).add(t.pauseMin).add(t.pauseMax).add(t.guardWarning).add(t.guardChance).add(t.guardHold);
    h.add(t.downSeconds).add(t.riseHealth).add(t.blowCapShare).add(t.overScale).add(t.budgetWindowSeconds);
    h.add(t.budgetShare).add(t.budgetAllowanceSeconds);
    h.add(rhythm).add(defence).add(rhythmDraws).add(defenceDraws).add(phase).add(target).add(blows).add(pause);
    h.add(clock).add(returning).add(drawn).add(lengths).add(begunBlows).add(committedEnd);
    h.add(spentPartner).add(spentLead).add(slot).add(now).add(down).add(downFor);
    for (const Watch& w : watch) h.add(w.winding).add(w.left).add(w.guard).add(w.hold);
    h.add(out.act).add(out.why).add(out.point.x).add(out.point.y).add(out.enemy).add(out.blows).add(out.newString).add(out.blowStart).add(out.blowIndex);
    h.add(out.partnerBlow).add(out.breakValue).add(out.extendsStagger).add(out.punish).add(out.rise).add(out.riseHealth);
    h.add(begun);
    return h.value;
}

// xorshift32, one value in [0, 1).
float PartnerBrain::draw(std::uint32_t& s, std::uint32_t& n) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    ++n;
    return static_cast<float>(s >> 8) * (1.f / 16777216.f);
}

void PartnerBrain::endString(bool staggered) {
    s_.clock = 0.0;
    s_.begunBlows = s_.blows; // no more blows: one still running runs out (committedEnd)
    s_.phase = staggered ? Phase::Ready : Phase::Pause; // rule 6: no pause on a staggered target
}

int PartnerBrain::chooseTarget(const PartnerInput& in) const {
    const int n = static_cast<int>(std::min(in.enemies.size(), kMaxPartnerEnemies));
    if (in.order == PartnerOrder::StayClose) {
        const int t = in.leadTarget;
        return t >= 0 && t < n && standing(in.enemies[static_cast<std::size_t>(t)]) ? t : -1;
    }
    int count = 0, all = -1, free = -1;
    float dAll = 0.f, dFree = 0.f;
    for (int i = 0; i < n; ++i) {
        const View& e = in.enemies[static_cast<std::size_t>(i)];
        if (!standing(e)) continue;
        ++count;
        const float d = edge(in.pos, e);
        if (all < 0 || d < dAll) {
            all = i;
            dAll = d;
        }
        if (!e.leadAttacking && (free < 0 || d < dFree)) {
            free = i;
            dFree = d;
        }
    }
    if (count <= 1) return all;
    return free >= 0 ? free : all; // the lead on every standing enemy: the nearest
}

// Every window [T - W, T] that would hold any of `seconds` more attack from now, the lead attacking no more. For the
// windows whose past part starts in slot b_i (the current slot is b_0): the partner's time is at most its sum over
// b_0..b_i, the lead's at least its sum over b_0..b_(i-1), and the new attack in the window at most
// min(seconds, W - s), s the length of b_0..b_(i-1). Slots before the fight hold nothing.
bool PartnerBrain::budgetFits(double seconds) const {
    const double w = static_cast<double>(s_.t.budgetWindowSeconds);
    const double share = static_cast<double>(s_.t.budgetShare);
    const double allow = static_cast<double>(s_.t.budgetAllowanceSeconds);
    double partner = 0.0, lead = 0.0, past = 0.0;
    for (std::size_t i = 0; i < kSlots && past < w; ++i) {
        const std::int64_t b = s_.slot - static_cast<std::int64_t>(i);
        if (b < 0) break;
        const std::size_t k = static_cast<std::size_t>(b % static_cast<std::int64_t>(kSlots));
        partner += static_cast<double>(s_.spentPartner[k]);
        if (partner + std::min(seconds, w - past) > share * lead + allow + 1e-9) return false;
        lead += static_cast<double>(s_.spentLead[k]);
        past = s_.now - static_cast<double>(b) * kSlot;
    }
    return true;
}

// `dt` from now: the partner's `partner` seconds of it are its first stretch (a blow begun runs from the step's start).
void PartnerBrain::account(double dt, double partner, bool lead) {
    double left = dt;
    double mine = partner;
    while (left > 1e-12) {
        const double end = static_cast<double>(s_.slot + 1) * kSlot;
        const double part = std::min(left, std::max(0.0, end - s_.now));
        const std::size_t k = static_cast<std::size_t>(s_.slot % static_cast<std::int64_t>(kSlots));
        const double p = std::min(part, mine);
        if (p > 0.0) s_.spentPartner[k] += static_cast<float>(p);
        if (lead) s_.spentLead[k] += static_cast<float>(part);
        mine -= p;
        s_.now += part;
        left -= part;
        if (s_.now >= end - 1e-12) {
            ++s_.slot;
            const std::size_t next = static_cast<std::size_t>(s_.slot % static_cast<std::int64_t>(kSlots));
            s_.spentPartner[next] = 0.f;
            s_.spentLead[next] = 0.f;
        }
    }
}

const PartnerIntent& PartnerBrain::step(double dt, const PartnerInput& in) {
    // As FightSession's tick: not a finite number or under 0 is 0; a long step counts as kMaxPartnerStep.
    dt = std::isfinite(dt) && dt > 0.0 ? std::min(dt, kMaxPartnerStep) : 0.0;
    decide(dt, in);
    if (s_.begun) {
        // Its attack this step: what runs of the blows it began, or, with none running, the whole step when the host
        // says it attacks anyway.
        const double running = std::clamp(s_.committedEnd - s_.now, 0.0, dt);
        const double mine = running > 0.0 ? running : (in.doing == PartnerDoing::Attacking ? dt : 0.0);
        account(dt, mine, in.leadAttacking);
    }
    return s_.out;
}

const PartnerIntent& PartnerBrain::decide(double dt, const PartnerInput& in) {
    PartnerIntent& out = s_.out;
    out = PartnerIntent{};
    if (!s_.begun) return out;
    const PartnerTuning& t = s_.t;
    const std::size_t n = std::min(in.enemies.size(), kMaxPartnerEnemies);

    // Rule 5: down at zero, waits, rises after downSeconds (every step until the host stands it).
    if (!(in.health > 0.f)) {
        if (!s_.down) {
            s_.down = true;
            s_.downFor = 0.0;
            s_.phase = Phase::Ready;
            s_.target = -1;
            s_.returning = false;
            s_.watch = {};
        } else {
            s_.downFor += dt;
        }
        out.why = PartnerWhy::Down;
        if (s_.downFor + 1e-9 >= static_cast<double>(t.downSeconds)) {
            out.rise = true;
            out.riseHealth = t.riseHealth;
        }
        return out;
    }
    s_.down = false;
    s_.downFor = 0.0;

    // Rule 3: one defence draw per windup aimed at it, when first seen with enough warning.
    bool guard = false;
    for (std::size_t i = 0; i < kMaxPartnerEnemies; ++i) {
        Watch& w = s_.watch[i];
        if (i < n && windingAt(in.enemies[i])) {
            const float left = in.enemies[i].windupLeft;
            if (!w.winding || left > w.left + 1e-4f) { // a new windup
                w.guard = left >= t.guardWarning && draw(s_.defence, s_.defenceDraws) < t.guardChance;
                w.hold = t.guardHold;
            }
            w.winding = true;
            w.left = left;
            if (w.guard) guard = true;
        } else {
            w.winding = false;
            w.left = 0.f;
            if (w.guard) {
                if (w.hold > 1e-6) {
                    guard = true;
                    w.hold -= dt;
                } else {
                    w.guard = false;
                }
            }
        }
    }

    const bool haveTarget = s_.target >= 0 && static_cast<std::size_t>(s_.target) < n &&
                            standing(in.enemies[static_cast<std::size_t>(s_.target)]);
    const bool staggered =
        haveTarget && in.enemies[static_cast<std::size_t>(s_.target)].state == View::State::Staggered;
    // The blows begun have run out (a blow begun counts to its end, whatever follows).
    const bool blowOver = s_.now + 1e-9 >= s_.committedEnd;

    // The string's clock is its blows' lengths; the pause's is the brain's.
    if (s_.phase == Phase::String) {
        if (!haveTarget) { // gone (down, or past the span): pick again, no pause on nothing
            s_.phase = Phase::Ready;
            s_.clock = 0.0;
            s_.begunBlows = s_.blows;
        } else if (blowOver && s_.begunBlows >= s_.blows) {
            endString(staggered);
        }
    } else if (s_.phase == Phase::Pause) {
        s_.clock += dt;
        if (s_.clock + 1e-9 >= s_.pause || staggered || !haveTarget) {
            s_.phase = Phase::Ready;
            s_.clock = 0.0;
        }
    }

    if (!in.canAct) {
        if (s_.phase == Phase::String) endString(staggered);
        out.why = PartnerWhy::CannotAct;
        return out;
    }
    if (guard) {
        if (s_.phase == Phase::String) endString(staggered);
        out.act = PartnerAct::Guard;
        out.why = PartnerWhy::Guard;
        return out;
    }

    // Rule 1, StayClose: past the leash, back to the lead until within returnMeters.
    if (in.order == PartnerOrder::StayClose) {
        const float d = dist(in.pos, in.leadPos);
        if (d > t.leashMeters) s_.returning = true;
        else if (d <= t.returnMeters) s_.returning = false;
        if (s_.returning) {
            if (s_.phase == Phase::String) endString(staggered);
            out.act = PartnerAct::MoveTo;
            out.why = PartnerWhy::Leash;
            out.point = in.leadPos;
            return out;
        }
    } else {
        s_.returning = false;
    }

    const auto attack = [&](int enemy, bool fresh) {
        out.act = PartnerAct::Attack;
        out.enemy = enemy;
        out.blows = s_.blows;
        out.newString = fresh;
        out.partnerBlow = true;
        out.breakValue = 0.f;
        out.extendsStagger = false;
        out.punish = in.enemies[static_cast<std::size_t>(enemy)].state == View::State::Staggered;
    };
    // What is left of the blows begun: every budget check counts it with the blow it decides.
    const double running = blowOver ? 0.0 : s_.committedEnd - s_.now;

    if (s_.phase == Phase::String) {
        if (blowOver) { // the next blow, decided before it starts on its whole length
            const double len = s_.lengths[static_cast<std::size_t>(s_.begunBlows)];
            if (!budgetFits(running + len)) { // rule 2's budget: it would break it
                endString(staggered);
                out.why = PartnerWhy::Budget;
                out.enemy = s_.target;
                return out;
            }
            out.blowStart = true;
            out.blowIndex = s_.begunBlows;
            ++s_.begunBlows;
            s_.committedEnd = s_.now + len;
        }
        attack(s_.target, false);
        out.why = PartnerWhy::String;
        return out;
    }
    if (s_.phase == Phase::Pause) {
        out.why = PartnerWhy::Pause;
        const View& e = in.enemies[static_cast<std::size_t>(s_.target)];
        if (edge(in.pos, e) < t.standoffMeters) {
            out.act = PartnerAct::StepBack;
            out.enemy = s_.target;
            out.point = toward(e.pos, in.pos, e.radius + t.standoffMeters);
        }
        return out;
    }

    // Ready: pick, close in, or start a string.
    const int tgt = chooseTarget(in);
    s_.target = tgt;
    if (tgt < 0) {
        out.why = PartnerWhy::NoTarget;
        return out;
    }
    const View& e = in.enemies[static_cast<std::size_t>(tgt)];
    if (edge(in.pos, e) > t.reachMeters) {
        Vec2 p = toward(e.pos, in.pos, e.radius + 0.5f * t.reachMeters);
        if (in.order == PartnerOrder::StayClose && dist(in.leadPos, p) > t.returnMeters)
            p = toward(in.leadPos, p, t.returnMeters);
        out.act = PartnerAct::MoveTo;
        out.why = PartnerWhy::Approach;
        out.enemy = tgt;
        out.point = p;
        return out;
    }
    if (windingAt(e)) { // rule 2: never into a windup aimed at it
        out.why = PartnerWhy::HeldOff;
        out.enemy = tgt;
        return out;
    }
    if (!blowOver) { // a blow begun before its string ended still runs out
        out.why = PartnerWhy::Committed;
        out.enemy = tgt;
        return out;
    }
    if (!s_.drawn) {
        const int span = t.blowsMax - t.blowsMin + 1;
        s_.blows = std::min(t.blowsMax,
                            t.blowsMin + static_cast<int>(draw(s_.rhythm, s_.rhythmDraws) * static_cast<float>(span)));
        s_.pause = static_cast<double>(t.pauseMin) +
                   static_cast<double>(draw(s_.rhythm, s_.rhythmDraws)) * static_cast<double>(t.pauseMax - t.pauseMin);
        s_.drawn = true;
        // The lengths the host will run (read once, so a held string keeps them).
        for (std::size_t i = 0; i < kMaxPartnerBlows; ++i) {
            const float l = in.blowSeconds[i];
            s_.lengths[i] = static_cast<double>(l >= 0.1f && l <= 2.f ? l : t.blowSeconds);
        }
    }
    if (!budgetFits(running + s_.lengths[0])) { // rule 2's budget: hold (each later blow is decided as it starts)
        out.why = PartnerWhy::Budget;
        out.enemy = tgt;
        return out;
    }
    s_.drawn = false;
    s_.phase = Phase::String;
    s_.clock = 0.0;
    s_.begunBlows = 1;
    s_.committedEnd = s_.now + s_.lengths[0];
    attack(tgt, true);
    out.blowStart = true;
    out.blowIndex = 0;
    out.why = PartnerWhy::Start;
    return out;
}

bool importPartnerBrain(std::string_view json, PartnerTuning& out, std::string* error) {
    out = PartnerTuning{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        out = PartnerTuning{};
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("partner brain file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("partner brain file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");

    PartnerTuning t;
    const auto real = [&](const char* key, float& to, double lo, double hi) {
        const JsonValue v = root[key];
        if (v.isAbsent()) return;
        if (!v.isNumber() || !(v.number() >= lo && v.number() <= hi)) {
            errors.push_back(std::string(key) + ": expected a number " + std::to_string(lo) + ".." + std::to_string(hi));
            return;
        }
        to = static_cast<float>(v.number());
    };
    const auto whole = [&](const char* key, int& to, int lo, int hi) {
        const JsonValue v = root[key];
        if (v.isAbsent()) return;
        if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
            errors.push_back(std::string(key) + ": expected a whole " + std::to_string(lo) + ".." + std::to_string(hi));
            return;
        }
        to = static_cast<int>(v.integer());
    };
    static constexpr const char* kKeys[] = {"version",     "leashMeters", "returnMeters", "reachMeters",  "standoffMeters",
                                            "blowsMin",    "blowsMax",    "blowSeconds",  "pauseMin",     "pauseMax",
                                            "guardWarning", "guardChance", "guardHold",   "downSeconds",  "riseHealth",
                                            "blowCapShare", "overScale",  "budgetWindowSeconds", "budgetShare",
                                            "budgetAllowanceSeconds"};
    real("leashMeters", t.leashMeters, 1, 10);
    real("returnMeters", t.returnMeters, 0.5, 10);
    real("reachMeters", t.reachMeters, 0.5, 4);
    real("standoffMeters", t.standoffMeters, 0, 4);
    whole("blowsMin", t.blowsMin, 1, 6);
    whole("blowsMax", t.blowsMax, 1, 6);
    real("blowSeconds", t.blowSeconds, 0.1, 2);
    real("pauseMin", t.pauseMin, 0, 10);
    real("pauseMax", t.pauseMax, 0, 10);
    real("guardWarning", t.guardWarning, 0, 2);
    real("guardChance", t.guardChance, 0, 1);
    real("guardHold", t.guardHold, 0, 2);
    real("downSeconds", t.downSeconds, 1, 600);
    real("riseHealth", t.riseHealth, 0.01, 1);
    real("blowCapShare", t.blowCapShare, 0, 1);
    real("overScale", t.overScale, 0, 1);
    real("budgetWindowSeconds", t.budgetWindowSeconds, 5, 30);
    real("budgetShare", t.budgetShare, 0, 1);
    real("budgetAllowanceSeconds", t.budgetAllowanceSeconds, 0, 10);

    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (std::find(std::begin(kKeys), std::end(kKeys), k) == std::end(kKeys))
            errors.push_back(std::string(k) + ": unknown field");
    }
    if (errors.empty() && !validPartnerTuning(t))
        errors.push_back("partner brain: returnMeters over leashMeters, standoffMeters over reachMeters, or a max under its min");
    if (!errors.empty()) return finish();
    out = t;
    return true;
}

} // namespace ftcore
