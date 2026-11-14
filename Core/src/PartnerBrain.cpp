#include "ftcore/PartnerBrain.h"

#include "ftcore/Json.h"

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

bool PartnerBrain::begin(const PartnerTuning& t, std::uint32_t seed) {
    const bool ok = validPartnerTuning(t);
    t_ = ok ? t : PartnerTuning{};
    rhythm_ = mix(seed);
    defence_ = mix(seed ^ 0xA5C3F00Du);
    rhythmDraws_ = 0;
    defenceDraws_ = 0;
    phase_ = Phase::Ready;
    target_ = -1;
    blows_ = 0;
    pause_ = 0.0;
    clock_ = 0.0;
    seenAttacking_ = false;
    returning_ = false;
    drawn_ = false;
    spentPartner_.fill(0.f);
    spentLead_.fill(0.f);
    slot_ = 0;
    now_ = 0.0;
    down_ = false;
    downFor_ = 0.0;
    watch_ = {};
    out_ = PartnerIntent{};
    begun_ = true;
    return ok;
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
    clock_ = 0.0;
    seenAttacking_ = false;
    phase_ = staggered ? Phase::Ready : Phase::Pause; // rule 6: no pause on a staggered target
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
    const double w = static_cast<double>(t_.budgetWindowSeconds);
    const double share = static_cast<double>(t_.budgetShare);
    const double allow = static_cast<double>(t_.budgetAllowanceSeconds);
    double partner = 0.0, lead = 0.0, past = 0.0;
    for (std::size_t i = 0; i < kSlots && past < w; ++i) {
        const std::int64_t b = slot_ - static_cast<std::int64_t>(i);
        if (b < 0) break;
        const std::size_t k = static_cast<std::size_t>(b % static_cast<std::int64_t>(kSlots));
        partner += static_cast<double>(spentPartner_[k]);
        if (partner + std::min(seconds, w - past) > share * lead + allow + 1e-9) return false;
        lead += static_cast<double>(spentLead_[k]);
        past = now_ - static_cast<double>(b) * kSlot;
    }
    return true;
}

void PartnerBrain::account(double dt, bool partner, bool lead) {
    double left = dt;
    // A step longer than any window: only its last stretch can still be in one.
    const double skip = left - (static_cast<double>(t_.budgetWindowSeconds) + 2.0 * kSlot);
    if (skip > 0.0) {
        spentPartner_.fill(0.f);
        spentLead_.fill(0.f);
        now_ += skip;
        slot_ = static_cast<std::int64_t>(std::floor(now_ / kSlot));
        left -= skip;
    }
    while (left > 1e-12) {
        const double end = static_cast<double>(slot_ + 1) * kSlot;
        const double part = std::min(left, std::max(0.0, end - now_));
        const std::size_t k = static_cast<std::size_t>(slot_ % static_cast<std::int64_t>(kSlots));
        if (partner) spentPartner_[k] += static_cast<float>(part);
        if (lead) spentLead_[k] += static_cast<float>(part);
        now_ += part;
        left -= part;
        if (now_ >= end - 1e-12) {
            ++slot_;
            const std::size_t next = static_cast<std::size_t>(slot_ % static_cast<std::int64_t>(kSlots));
            spentPartner_[next] = 0.f;
            spentLead_[next] = 0.f;
        }
    }
}

const PartnerIntent& PartnerBrain::step(double dt, const PartnerInput& in) {
    if (!(dt > 0.0)) dt = 0.0;
    decide(dt, in);
    if (begun_) account(dt, out_.act == PartnerAct::Attack || in.doing == PartnerDoing::Attacking, in.leadAttacking);
    return out_;
}

const PartnerIntent& PartnerBrain::decide(double dt, const PartnerInput& in) {
    out_ = PartnerIntent{};
    if (!begun_) return out_;
    const std::size_t n = std::min(in.enemies.size(), kMaxPartnerEnemies);

    // Rule 5: down at zero, waits, rises after downSeconds (every step until the host stands it).
    if (!(in.health > 0.f)) {
        if (!down_) {
            down_ = true;
            downFor_ = 0.0;
            phase_ = Phase::Ready;
            target_ = -1;
            returning_ = false;
            watch_ = {};
        } else {
            downFor_ += dt;
        }
        out_.why = PartnerWhy::Down;
        if (downFor_ + 1e-9 >= static_cast<double>(t_.downSeconds)) {
            out_.rise = true;
            out_.riseHealth = t_.riseHealth;
        }
        return out_;
    }
    down_ = false;
    downFor_ = 0.0;

    // Rule 3: one defence draw per windup aimed at it, when first seen with enough warning.
    bool guard = false;
    for (std::size_t i = 0; i < kMaxPartnerEnemies; ++i) {
        Watch& w = watch_[i];
        if (i < n && windingAt(in.enemies[i])) {
            const float left = in.enemies[i].windupLeft;
            if (!w.winding || left > w.left + 1e-4f) { // a new windup
                w.guard = left >= t_.guardWarning && draw(defence_, defenceDraws_) < t_.guardChance;
                w.hold = t_.guardHold;
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

    const bool haveTarget = target_ >= 0 && static_cast<std::size_t>(target_) < n &&
                            standing(in.enemies[static_cast<std::size_t>(target_)]);
    const bool staggered =
        haveTarget && in.enemies[static_cast<std::size_t>(target_)].state == View::State::Staggered;

    // The string's clock is the host's `doing`; the pause's is the brain's.
    if (phase_ != Phase::Ready) clock_ += dt;
    if (phase_ == Phase::String) {
        if (in.doing == PartnerDoing::Attacking) seenAttacking_ = true;
        const double full = static_cast<double>(blows_) * static_cast<double>(t_.blowSeconds);
        const bool over = !haveTarget || (seenAttacking_ ? (in.doing != PartnerDoing::Attacking || clock_ >= 2.0 * full)
                                                         : clock_ + 1e-9 >= full);
        if (over && !haveTarget) { // gone (down, or past the span): pick again, no pause on nothing
            phase_ = Phase::Ready;
            clock_ = 0.0;
            seenAttacking_ = false;
        } else if (over) {
            endString(staggered);
        }
    } else if (phase_ == Phase::Pause) {
        if (clock_ + 1e-9 >= pause_ || staggered || !haveTarget) {
            phase_ = Phase::Ready;
            clock_ = 0.0;
        }
    }

    if (!in.canAct) {
        if (phase_ == Phase::String) endString(staggered);
        out_.why = PartnerWhy::CannotAct;
        return out_;
    }
    if (guard) {
        if (phase_ == Phase::String) endString(staggered);
        out_.act = PartnerAct::Guard;
        out_.why = PartnerWhy::Guard;
        return out_;
    }

    // Rule 1, StayClose: past the leash, back to the lead until within returnMeters.
    if (in.order == PartnerOrder::StayClose) {
        const float d = dist(in.pos, in.leadPos);
        if (d > t_.leashMeters) returning_ = true;
        else if (d <= t_.returnMeters) returning_ = false;
        if (returning_) {
            if (phase_ == Phase::String) endString(staggered);
            out_.act = PartnerAct::MoveTo;
            out_.why = PartnerWhy::Leash;
            out_.point = in.leadPos;
            return out_;
        }
    } else {
        returning_ = false;
    }

    const auto attack = [&](int enemy, bool fresh) {
        out_.act = PartnerAct::Attack;
        out_.enemy = enemy;
        out_.blows = blows_;
        out_.newString = fresh;
        out_.partnerBlow = true;
        out_.breakValue = 0.f;
        out_.extendsStagger = false;
        out_.punish = in.enemies[static_cast<std::size_t>(enemy)].state == View::State::Staggered;
    };

    if (phase_ == Phase::String) {
        if (!budgetFits(dt)) { // rule 2's budget: this step would break it
            endString(staggered);
            out_.why = PartnerWhy::Budget;
            out_.enemy = target_;
            return out_;
        }
        attack(target_, false);
        out_.why = PartnerWhy::String;
        return out_;
    }
    if (phase_ == Phase::Pause) {
        out_.why = PartnerWhy::Pause;
        const View& e = in.enemies[static_cast<std::size_t>(target_)];
        if (edge(in.pos, e) < t_.standoffMeters) {
            out_.act = PartnerAct::StepBack;
            out_.enemy = target_;
            out_.point = toward(e.pos, in.pos, e.radius + t_.standoffMeters);
        }
        return out_;
    }

    // Ready: pick, close in, or start a string.
    const int tgt = chooseTarget(in);
    target_ = tgt;
    if (tgt < 0) {
        out_.why = PartnerWhy::NoTarget;
        return out_;
    }
    const View& e = in.enemies[static_cast<std::size_t>(tgt)];
    if (edge(in.pos, e) > t_.reachMeters) {
        Vec2 p = toward(e.pos, in.pos, e.radius + 0.5f * t_.reachMeters);
        if (in.order == PartnerOrder::StayClose && dist(in.leadPos, p) > t_.returnMeters)
            p = toward(in.leadPos, p, t_.returnMeters);
        out_.act = PartnerAct::MoveTo;
        out_.why = PartnerWhy::Approach;
        out_.enemy = tgt;
        out_.point = p;
        return out_;
    }
    if (windingAt(e)) { // rule 2: never into a windup aimed at it
        out_.why = PartnerWhy::HeldOff;
        out_.enemy = tgt;
        return out_;
    }
    if (!drawn_) {
        const int span = t_.blowsMax - t_.blowsMin + 1;
        blows_ = std::min(t_.blowsMax,
                          t_.blowsMin + static_cast<int>(draw(rhythm_, rhythmDraws_) * static_cast<float>(span)));
        pause_ = static_cast<double>(t_.pauseMin) +
                 static_cast<double>(draw(rhythm_, rhythmDraws_)) * static_cast<double>(t_.pauseMax - t_.pauseMin);
        drawn_ = true;
    }
    if (!budgetFits(static_cast<double>(blows_) * static_cast<double>(t_.blowSeconds))) { // rule 2's budget: hold
        out_.why = PartnerWhy::Budget;
        out_.enemy = tgt;
        return out_;
    }
    drawn_ = false;
    phase_ = Phase::String;
    clock_ = 0.0;
    seenAttacking_ = false;
    attack(tgt, true);
    out_.why = PartnerWhy::Start;
    return out_;
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
