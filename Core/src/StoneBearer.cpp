#include "ftcore/StoneBearer.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace ftcore {

// ---- The pedestal ----------------------------------------------------------------------------------

PedestalRelease PedestalPhase::release(const GuestCaller& guest, double t, int load) {
    if (!holding_) return PedestalRelease::NotHolding;
    if (load <= kArenaReleaseOpenLoad) return PedestalRelease::LowLoad;
    if (std::fabs(t - nearestCall(guest, t)) > tuning_.releaseWindow) return PedestalRelease::OffBeat;
    holding_ = false;
    lifted_ = true;
    downUntil_ = t + tuning_.downTime;
    return PedestalRelease::Lifted;
}

// ---- The loader ------------------------------------------------------------------------------------

namespace {

std::string sbRange(double lo, double hi) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g..%g", lo, hi);
    return buf;
}

void sbUnknownKeys(JsonValue j, const char* path, std::initializer_list<std::string_view> keys,
                   std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back(std::string(path) + ": unknown key: " + std::string(key));
    }
}

template <typename T>
void sbNum(JsonValue j, const char* path, const char* key, T& out, double lo, double hi,
           std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    const double d = v.number(-1e300);
    if (!v.isNumber() || !std::isfinite(d) || d < lo || d > hi) {
        errors.push_back(std::string(path) + "." + key + ": expected a number in " + sbRange(lo, hi));
        return;
    }
    out = static_cast<T>(d);
}

void sbInt(JsonValue j, const char* path, const char* key, int& out, int lo, int hi,
           std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
        errors.push_back(std::string(path) + "." + key + ": expected an integer in " + std::to_string(lo) + ".." +
                         std::to_string(hi));
        return;
    }
    out = static_cast<int>(v.integer());
}

void sbWeight(JsonValue v, WeightTuning& w, std::vector<std::string>& errors, std::vector<std::string>* warnings) {
    sbUnknownKeys(v, "weight",
                  {"cap", "damagePerStack", "defencePerStack", "shockRingAt", "poiseFreezeAt", "guard", "soloLift",
                   "interceptShed", "critShed"},
                  warnings);
    const std::size_t before = errors.size();
    sbInt(v, "weight", "cap", w.cap, 1, 10, errors);
    sbNum(v, "weight", "damagePerStack", w.damagePerStack, 0.0, 0.5, errors);
    sbNum(v, "weight", "defencePerStack", w.defencePerStack, 0.0, 0.5, errors);
    sbInt(v, "weight", "shockRingAt", w.shockRingAt, 1, 10, errors);
    sbInt(v, "weight", "poiseFreezeAt", w.poiseFreezeAt, 1, 10, errors);
    sbInt(v, "weight", "guard", w.guard, 0, 5, errors);
    sbInt(v, "weight", "soloLift", w.soloLift, 0, 5, errors);
    sbInt(v, "weight", "interceptShed", w.interceptShed, 0, 5, errors);
    sbInt(v, "weight", "critShed", w.critShed, 0, 5, errors);
    if (errors.size() != before) return; // the cross-checks need every value
    if (w.shockRingAt > w.cap) errors.push_back("weight.shockRingAt: expected at most weight.cap");
    if (w.poiseFreezeAt > w.cap) errors.push_back("weight.poiseFreezeAt: expected at most weight.cap");
    if (!(w.defencePerStack * static_cast<float>(w.cap) < 1.0f))
        errors.push_back("weight.defencePerStack: expected weight.cap x it below 1");
}

void sbPedestal(JsonValue v, double bpm, PedestalTuning& p, std::vector<std::string>& errors,
                std::vector<std::string>* warnings) {
    sbUnknownKeys(v, "pedestal", {"at", "firstCallBeats", "callEveryBeats", "releaseWindow", "downTime"}, warnings);
    const std::size_t before = errors.size();
    sbNum(v, "pedestal", "at", p.at, 0.0, 1.0, errors);
    if (errors.size() == before && !(p.at > 0.0f && p.at < 1.0f))
        errors.push_back("pedestal.at: expected a number above 0 and below 1");
    sbNum(v, "pedestal", "firstCallBeats", p.firstCallBeats, 0.0, 16.0, errors);
    sbNum(v, "pedestal", "callEveryBeats", p.callEveryBeats, 1.0, 16.0, errors);
    sbNum(v, "pedestal", "releaseWindow", p.releaseWindow, 0.02, 0.5, errors);
    sbNum(v, "pedestal", "downTime", p.downTime, 0.0, 10.0, errors);
    if (errors.size() != before) return;
    // The windows either side of two calls may not meet, or a Release would sit in both.
    if (!(p.releaseWindow < 0.5 * arenaBeats(p.callEveryBeats, bpm)))
        errors.push_back("pedestal.releaseWindow: expected below half of pedestal.callEveryBeats");
}

} // namespace

bool importStoneBearer(std::string_view json, StoneBearerDef& out, std::string* error,
                       std::vector<std::string>* warnings) {
    out = StoneBearerDef{};
    std::vector<std::string> errors;
    std::string bossError;
    if (!importArenaBoss(json, out.boss, &bossError, warnings, {"weight", "pedestal"})) {
        std::size_t from = 0;
        while (from <= bossError.size()) {
            const std::size_t nl = std::min(bossError.find('\n', from), bossError.size());
            if (nl > from) errors.push_back(bossError.substr(from, nl - from));
            from = nl + 1;
        }
    }
    JsonDocument doc;
    if (doc.parse(json, nullptr) && doc.root().isObject()) {
        const JsonValue root = doc.root();
        const JsonValue bpm = root["bpm"];
        const double beatsPerMinute = bpm.isNumber() && bpm.number() > 0.0 ? bpm.number() : 120.0;
        if (const JsonValue v = root["weight"]; v.isObject()) sbWeight(v, out.weight, errors, warnings);
        else errors.push_back("weight: expected an object");
        if (const JsonValue v = root["pedestal"]; v.isObject()) sbPedestal(v, beatsPerMinute, out.pedestal, errors, warnings);
        else errors.push_back("pedestal: expected an object");
    }
    if (errors.empty()) return true;
    out = StoneBearerDef{};
    if (error) {
        error->clear();
        for (const std::string& e : errors) {
            if (!error->empty()) *error += '\n';
            *error += e;
        }
    }
    return false;
}

// ---- The boss --------------------------------------------------------------------------------------

void StoneBearer::begin(const StoneBearerDef& def, double now) {
    def_ = &def;
    run_.begin(def.boss, now);
    weight_.reset(def.weight);
    pedestal_.reset(def.pedestal);
    hpShare_ = 1.0f;
    nextCall_ = 0.0;
    recoverDue_ = false;
}

void StoneBearer::weigh(int n, double at) {
    const bool ring = weight_.shockRing();
    const bool frozen = weight_.poiseFrozen();
    const int changed = weight_.change(n);
    if (changed == 0) return;
    run_.push({ArenaBossEventType::StacksChanged, kArenaBossNoMove, 0, CalledOutcome::Pending, weight_.stacks(),
               changed, at, 0.0});
    if (ring != weight_.shockRing())
        run_.push({ArenaBossEventType::ShockRing, kArenaBossNoMove, 0, CalledOutcome::Pending,
                   weight_.shockRing() ? 1 : 0, 0, at, 0.0});
    if (frozen != weight_.poiseFrozen())
        run_.push({ArenaBossEventType::PoiseFrozen, kArenaBossNoMove, 0, CalledOutcome::Pending,
                   weight_.poiseFrozen() ? 1 : 0, 0, at, 0.0});
}

// Turns the run's events into the boss's own, then plays the pedestal up to now.
const ArenaBossEvents& StoneBearer::finish() {
    const ArenaBossEvents& ev = run_.events();
    const WeightTuning& w = def_->weight;
    const std::size_t n = ev.size(); // weigh() appends: only the run's own events are read
    for (std::size_t i = 0; i < n; ++i) {
        const ArenaBossEvent e = ev[i];
        if (e.type == ArenaBossEventType::Crit && e.value == 1) {
            weigh(-w.critShed, e.at);
        } else if (e.type == ArenaBossEventType::Answered && e.move < def_->boss.moveCount) {
            const ArenaBossMove& m = def_->boss.moves[e.move];
            if (e.outcome == CalledOutcome::Intercepted && m.sheds[e.impact]) weigh(-w.interceptShed, e.at);
            else if (e.outcome == CalledOutcome::Guarded) weigh(m.guardStacks, e.at);
            else if (e.outcome == CalledOutcome::Missed || e.outcome == CalledOutcome::WrongZone)
                weigh(m.missStacks, e.at);
        }
    }
    const double now = run_.now();
    if (pedestal_.holding()) {
        const PedestalTuning& p = def_->pedestal;
        const GuestCaller& guest = run_.guest();
        const double series = pedestal_.from() + arenaBeats(p.firstCallBeats, guest.bpm());
        while (nextCall_ <= now) {
            run_.push({ArenaBossEventType::Call, kArenaBossNoMove, 0, CalledOutcome::Pending, 0, 0, nextCall_, 0.0});
            nextCall_ = guest.nextRepeat(series, p.callEveryBeats, nextCall_ + 0.5 * arenaBeats(p.callEveryBeats, guest.bpm()));
        }
    }
    if (recoverDue_ && now >= pedestal_.downUntil()) {
        recoverDue_ = false;
        run_.push({ArenaBossEventType::Recovered, kArenaBossNoMove, 0, CalledOutcome::Pending, 0, 0,
                   pedestal_.downUntil(), 0.0});
    }
    return ev;
}

const ArenaBossEvents& StoneBearer::tick(double t) {
    run_.update(t, hpShare_);
    if (pedestal_.update(hpShare_, run_.now())) {
        run_.cancelMove();
        run_.push({ArenaBossEventType::Invulnerable, kArenaBossNoMove, 0, CalledOutcome::Pending, 1, 0, run_.now(), 0.0});
        nextCall_ = pedestal_.firstCall(run_.guest());
    }
    return finish();
}

const ArenaBossEvents& StoneBearer::startMove(std::uint8_t move, double t, CalledZone side, bool* started) {
    if (pedestal_.holding() || pedestal_.down(t)) {
        if (started) *started = false;
        return tick(t);
    }
    run_.startMove(move, t, side, started);
    return finish();
}

const ArenaBossEvents& StoneBearer::update(double now, float hpShare) {
    hpShare_ = hpShare;
    return tick(now);
}

const ArenaBossEvents& StoneBearer::interceptDown(CalledZone zone, double t) {
    run_.interceptDown(zone, t);
    return finish();
}

const ArenaBossEvents& StoneBearer::interceptUp(double t) {
    run_.interceptUp(t);
    return finish();
}

const ArenaBossEvents& StoneBearer::guardDown(CalledZone zone, double t) {
    run_.guardDown(zone, t);
    return finish();
}

const ArenaBossEvents& StoneBearer::guardUp(double t) {
    run_.guardUp(t);
    return finish();
}

const ArenaBossEvents& StoneBearer::onBlocked(std::uint8_t move, double t) {
    tick(t);
    if (move < def_->boss.moveCount && !def_->boss.moves[move].intercept()) weigh(def_->weight.guard, run_.now());
    return run_.events();
}

const ArenaBossEvents& StoneBearer::onSoloLift(double t) {
    tick(t);
    weigh(def_->weight.soloLift, run_.now());
    return run_.events();
}

const ArenaBossEvents& StoneBearer::onStagger(double t) {
    // A frozen poise or the pedestal: the host should not stagger the boss; one it reports anyway is lost.
    if (weight_.poiseFrozen() || pedestal_.holding()) return tick(t);
    run_.onStagger(t);
    return finish();
}

const ArenaBossEvents& StoneBearer::onHit(Vec2 from, Vec2 pos, Vec2 facing, double t, const AimPoint* at,
                                          StoneBearerHit* hit) {
    StoneBearerHit h;
    if (pedestal_.holding()) {
        tick(t);
        h.damage = 0.0f;
        h.poise = 0.0f;
    } else {
        // The blow lands with the stacks it meets: a first crit's shed counts from the next blow.
        const float taken = weight_.takenScale();
        const bool frozen = weight_.poiseFrozen();
        run_.onHit(from, pos, facing, t, at, &h.crit);
        const float mult = h.crit.crit ? h.crit.multiplier : 1.0f;
        h.damage = mult * taken;
        h.poise = frozen ? 0.0f : mult;
        finish();
    }
    if (hit) *hit = h;
    return run_.events();
}

const ArenaBossEvents& StoneBearer::onRelease(double t, int load, PedestalRelease* result) {
    tick(t);
    const PedestalRelease r = pedestal_.release(run_.guest(), run_.now(), load);
    if (r == PedestalRelease::Lifted) {
        recoverDue_ = true;
        run_.push({ArenaBossEventType::Invulnerable, kArenaBossNoMove, 0, CalledOutcome::Pending, 0, 0, run_.now(), 0.0});
        run_.push({ArenaBossEventType::Lifted, kArenaBossNoMove, 0, CalledOutcome::Pending, 0, 0, run_.now(),
                   pedestal_.downUntil()});
    }
    if (result) *result = r;
    return run_.events();
}

} // namespace ftcore
