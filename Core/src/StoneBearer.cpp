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
    if (!(std::fabs(t - nearestCall(guest, t)) <= tuning_.releaseWindow)) return PedestalRelease::OffBeat;
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
    if (errors.size() != before || !(bpm > 0.0)) return; // a bad bpm is the generic loader's error
    // The windows either side of two calls may not meet, or a Release would sit in both.
    if (!(p.releaseWindow < 0.5 * arenaBeats(p.callEveryBeats, bpm)))
        errors.push_back("pedestal.releaseWindow: expected below half of pedestal.callEveryBeats");
}

void sbBreakable(JsonValue v, float pedestalAt, BreakableTuning& b, std::vector<std::string>& errors,
                 std::vector<std::string>* warnings) {
    sbUnknownKeys(v, "breakable", {"at"}, warnings);
    const std::size_t before = errors.size();
    sbNum(v, "breakable", "at", b.at, 0.0, 1.0, errors);
    if (errors.size() == before && !(b.at > 0.0f && b.at < 1.0f))
        errors.push_back("breakable.at: expected a number above 0 and below 1");
    // pedestalAt is 0 when the pedestal was refused: the cross-check is then skipped.
    if (errors.size() == before && pedestalAt > 0.0f && !(b.at > pedestalAt))
        errors.push_back("breakable.at: expected above pedestal.at");
}

} // namespace

bool importStoneBearer(std::string_view json, StoneBearerDef& out, std::string* error,
                       std::vector<std::string>* warnings) {
    out = StoneBearerDef{};
    std::vector<std::string> errors;
    std::string bossError;
    if (!importArenaBoss(json, out.boss, &bossError, warnings, {"weight", "breakable", "pedestal"})) {
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
        // The tempo the generic loader accepted (0 when it refused it: the cross-check is then skipped).
        const JsonValue bpm = root["bpm"];
        const double beatsPerMinute =
            bpm.isNumber() && bpm.number() >= 40.0 && bpm.number() <= 240.0 ? bpm.number() : 0.0;
        if (const JsonValue v = root["weight"]; v.isObject()) sbWeight(v, out.weight, errors, warnings);
        else errors.push_back("weight: expected an object");
        const std::size_t beforePedestal = errors.size();
        if (const JsonValue v = root["pedestal"]; v.isObject())
            sbPedestal(v, beatsPerMinute, out.pedestal, errors, warnings);
        else errors.push_back("pedestal: expected an object");
        const float pedestalAt = errors.size() == beforePedestal ? out.pedestal.at : 0.0f;
        if (const JsonValue v = root["breakable"]; v.isObject())
            sbBreakable(v, pedestalAt, out.breakable, errors, warnings);
        else errors.push_back("breakable: expected an object");
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
    breakAt_ = ArenaHpTrigger{def.breakable.at};
    hpShare_ = 1.0f;
    nextCall_ = 0.0;
    recoverDue_ = false;
}

// finish() reads the stack gains back from the run's events, so no call may drop one. The most a tick
// emits: the run's phase change, a call and an answer per impact, the shift, two crit windows closing
// (a long frame passes the old one's close and a new one's) and one opening, the load; up to three
// events per stack change (one change per answer); the hp break; and the pedestal's switch, its calls
// and the recovery. A call adds at most four to a tick: a Crit and its shed (onHit), a block's or a
// lift's stack change (onBlocked, onSoloLift), Invulnerable and Lifted (onRelease), or Broke
// (onStagger; never a second one after the tick's hp break).
constexpr std::size_t kSbTickEvents =
    1 + 2 * kMaxCalledImpacts + 1 + 2 + 1 + 1 + 3 * kMaxCalledImpacts + 1 + 1 + kPedestalMaxCalls + 1;
static_assert(kSbTickEvents + 1 + 3 <= kMaxArenaBossEvents);

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
const ArenaBossEvents& StoneBearer::finish(float* takenAtHit, bool* frozenAtHit) {
    const ArenaBossEvents& ev = run_.events();
    const WeightTuning& w = def_->weight;
    const auto readHit = [&] {
        if (!takenAtHit) return;
        *takenAtHit = weight_.takenScale();
        *frozenAtHit = weight_.poiseFrozen();
        takenAtHit = nullptr;
    };
    const std::size_t n = ev.size(); // weigh() appends: only the run's own events are read
    for (std::size_t i = 0; i < n; ++i) {
        const ArenaBossEvent e = ev[i];
        if (e.type == ArenaBossEventType::Crit) readHit(); // the blow itself: after every answer before it
        if (e.type == ArenaBossEventType::Crit && e.value == 1) {
            weigh(-w.critShed, e.at);
        } else if (e.type == ArenaBossEventType::Answered && e.move < def_->boss.moveCount) {
            const ArenaBossMove& m = def_->boss.moves[e.move];
            if (e.outcome == CalledOutcome::Intercepted && m.sheds[e.impact]) weigh(-w.interceptShed, e.at);
            else if (e.outcome == CalledOutcome::Guarded) weigh(m.guardStacks, e.at);
            else if (e.value == 1) weigh(m.missStacks, e.at); // the use's first miss: once per use
        }
    }
    readHit(); // no crit: the blow meets the stacks after every answer
    const double now = run_.now();
    if (pedestal_.holding()) {
        const PedestalTuning& p = def_->pedestal;
        const GuestCaller& guest = run_.guest();
        const double series = pedestal_.from() + arenaBeats(p.firstCallBeats, guest.bpm());
        const double period = arenaBeats(p.callEveryBeats, guest.bpm());
        if (period > 0.0) {
            // A frame that jumped further plays only the calls of the last kPedestalCatchUp periods.
            if (nextCall_ < now - kPedestalCatchUp * period)
                nextCall_ = guest.nextRepeat(series, p.callEveryBeats, now - kPedestalCatchUp * period);
            // Bounded too where a period is below the clock's rounding (a clock past ~1e15 s).
            for (int k = 0; nextCall_ <= now && k < kPedestalMaxCalls; ++k) {
                run_.push(
                    {ArenaBossEventType::Call, kArenaBossNoMove, 0, CalledOutcome::Pending, 0, 0, nextCall_, 0.0});
                nextCall_ = guest.nextRepeat(series, p.callEveryBeats, nextCall_ + 0.5 * period);
            }
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
    // The hp break after the phase (PhaseChanged first) and before the pedestal (Broke before
    // Invulnerable). Not a stagger: the live move plays on, and neither the freeze nor the pedestal stops
    // it. Already broken by a Stagger, it does nothing.
    if (breakAt_.update(hpShare_)) run_.breakNow();
    if (pedestal_.update(hpShare_, run_.now())) {
        run_.cancelMove();
        run_.push(
            {ArenaBossEventType::Invulnerable, kArenaBossNoMove, 0, CalledOutcome::Pending, 1, 0, run_.now(), 0.0});
        nextCall_ = pedestal_.firstCall(run_.guest());
    }
    return finish();
}

const ArenaBossEvents& StoneBearer::startMove(std::uint8_t move, double t, CalledZone side, bool* started) {
    if (pedestal_.holding() || pedestal_.down(run_.timeOf(t))) {
        if (started) *started = false;
        return tick(t);
    }
    run_.startMove(move, t, side, started);
    return finish();
}

const ArenaBossEvents& StoneBearer::update(double now, float hpShare) {
    if (!std::isnan(hpShare)) hpShare_ = hpShare;
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
    // The stacks first follow every answer up to t. Then a frozen poise or the pedestal loses the
    // stagger: the host should not stagger the boss then; one it reports anyway is ignored.
    tick(t);
    if (!weight_.poiseFrozen() && !pedestal_.holding()) run_.staggerNow();
    return run_.events();
}

const ArenaBossEvents& StoneBearer::onHit(Vec2 from, Vec2 pos, Vec2 facing, double t, const AimPoint* at,
                                          StoneBearerHit* hit) {
    StoneBearerHit h;
    if (pedestal_.holding()) {
        tick(t);
        h.damage = 0.0f;
        h.poise = 0.0f;
    } else {
        // The blow lands with the stacks it meets at t: the answers up to t count, a first crit's shed
        // counts from the next blow.
        run_.onHit(from, pos, facing, t, at, &h.crit);
        float taken = 1.0f;
        bool frozen = false;
        finish(&taken, &frozen);
        const float mult = h.crit.crit ? h.crit.multiplier : 1.0f;
        h.damage = mult * taken;
        h.poise = frozen ? 0.0f : mult;
    }
    if (hit) *hit = h;
    return run_.events();
}

const ArenaBossEvents& StoneBearer::onRelease(double t, int load, PedestalRelease* result) {
    tick(t);
    const PedestalRelease r = pedestal_.release(run_.guest(), run_.now(), load);
    if (r == PedestalRelease::Lifted) {
        recoverDue_ = true;
        run_.push(
            {ArenaBossEventType::Invulnerable, kArenaBossNoMove, 0, CalledOutcome::Pending, 0, 0, run_.now(), 0.0});
        run_.push({ArenaBossEventType::Lifted, kArenaBossNoMove, 0, CalledOutcome::Pending, 0, 0, run_.now(),
                   pedestal_.downUntil()});
    }
    if (result) *result = r;
    return run_.events();
}

// ---- The boss as ArenaFight moves ------------------------------------------------------------------

namespace {

constexpr float kStoneArcCos = 0.5f;     // the arc of intercept and plain moves, and of every move without a template
constexpr float kStoneMinStartup = 0.05f; // a chained startup never falls below this (a tile move's lands this late)
constexpr float kStonePickSlack = 0.4f;  // a move's pick range reaches this far past its reach (m), as arena.json's do
constexpr float kStoneGuardBreakExtra = 1.f; // a guard-breaking blow takes this much more than the lead's stamina

bool sbLaneMove(const ArenaBossMove& move, std::size_t impact) {
    return move.answer == ArenaBossAnswer::Cover
        || (move.intercept() && impact < move.impactCount && move.zones[impact] == CalledZoneRule::Path);
}

} // namespace

std::string_view stoneBearerClipOf(std::string_view id) {
    for (const std::string_view tail : {std::string_view("_twice"), std::string_view("_halves")}) {
        if (id.size() > tail.size() && id.substr(id.size() - tail.size()) == tail) return id.substr(0, id.size() - tail.size());
    }
    return id;
}

float stoneBearerContactPlanar(const ArenaBossMove& move) {
    return std::sqrt(move.contactAhead * move.contactAhead + move.contactSide * move.contactSide);
}

bool stoneBearerUsableContact(const ArenaBossMove& move, float bodyRadius) {
    if (!move.hasContact || move.answer == ArenaBossAnswer::Tile || move.answer == ArenaBossAnswer::Cover) return false;
    for (std::size_t k = 0; k < move.impactCount; ++k) {
        if (sbLaneMove(move, k)) return false;
    }
    return stoneBearerContactPlanar(move) > bodyRadius;
}

bool stoneBearerLayout(const StoneBearerDef& def, StoneBearerLayout& out, std::string* error) {
    out = StoneBearerLayout{};
    std::size_t count = 0;
    for (std::uint8_t m = 0; m < def.boss.moveCount; ++m) count += def.boss.moves[m].impactCount;
    if (count > kMaxArenaMoves) {
        if (error) *error = "stone_bearer: " + std::to_string(count) + " impacts in all, more than kMaxArenaMoves";
        return false;
    }
    for (std::uint8_t m = 0; m < def.boss.moveCount; ++m) {
        const ArenaBossMove& src = def.boss.moves[m];
        for (std::uint8_t k = 0; k < src.impactCount; ++k, ++out.count) {
            out.moveOf[out.count] = m;
            out.impactOf[out.count] = k;
            out.runsOn[out.count] = k > 0 && src.answer == ArenaBossAnswer::Tile;
        }
    }
    return true;
}

bool buildStoneBearerKind(const StoneBearerDef& def, const ArenaFighter& lead, ArenaEnemyKind& kind,
                          StoneBearerLayout* layout, std::string* error) {
    StoneBearerLayout lay;
    if (!stoneBearerLayout(def, lay, error)) return false;
    ArenaEnemyKind out = kind;
    out.hp = def.boss.hp;
    out.poise = def.boss.poise;
    out.staggerTime = def.boss.staggerTime;
    out.openTime = def.boss.openTime;
    const double beat = def.boss.beat();
    for (std::size_t next = 0; next < lay.count; ++next) {
        const ArenaBossMove& src = def.boss.moves[lay.moveOf[next]];
        const std::uint8_t k = lay.impactOf[next];
        const std::string_view clip = stoneBearerClipOf(src.id());
        // The arena move of the same clip tunes the pick and the look; else the kind's first move does.
        const ArenaEnemyMove* tmpl = nullptr;
        for (std::uint8_t t = 0; t < kind.moveCount && !tmpl; ++t) {
            if (kind.moves[t].clip() == clip) tmpl = &kind.moves[t];
        }
        const bool ownTemplate = tmpl != nullptr;
        if (!tmpl) tmpl = &kind.moves[0];
        ArenaEnemyMove& mv = out.moves[next];
        mv = *tmpl;
        ArenaAttack& a = mv.attack;
        const double gap = k == 0 ? src.impacts[0] : src.impacts[k] - src.impacts[k - 1];
        // A chained move starts when the one before it ends, so its startup is the gap less that move's active part.
        const float startup = static_cast<float>(gap * beat) - (k == 0 ? 0.f : src.active);
        a.startup = startup > kStoneMinStartup ? startup : kStoneMinStartup;
        a.active = src.active;
        a.recovery = k + 1 == src.impactCount ? src.recovery : 0.f;
        // The boss file's reach is from the boss's body; ArenaFight's from its centre. A clip's contact point outside
        // the body is the reach instead: the blow lands where the part strikes.
        a.range = stoneBearerUsableContact(src, kind.bodyRadius) ? stoneBearerContactPlanar(src) : src.range + kind.bodyRadius;
        a.damage = static_cast<int>(std::lround(static_cast<double>(src.damage) * lead.hp));
        a.width = 0.f;
        if (src.answer == ArenaBossAnswer::Tile) {
            a.arcCos = -1.f;
        } else if (sbLaneMove(src, k)) {
            a.width = kStoneBearerLaneWidth;
        } else if (src.intercept() || src.answer == ArenaBossAnswer::Any || !ownTemplate) {
            a.arcCos = kStoneArcCos;
        }
        if (src.guardBreak > 0.f) a.guardDamage = lead.stamina + kStoneGuardBreakExtra;
        mv.minRange = 0.f;
        mv.maxRange = a.range + kStonePickSlack;
        mv.chain = k + 1 < src.impactCount ? static_cast<std::int8_t>(next + 1) : -1;
        mv.weight = k == 0 ? tmpl->weight : 0.f;
        mv.cooldown = k == 0 ? tmpl->cooldown : 0.f;
        // The run answers an intercept move (ArenaFight only lands it); a dodge, jump, tile or cover move is not one a
        // guard stops.
        mv.parryable = tmpl->parryable && src.answer == ArenaBossAnswer::Any;
        mv.unblockable = src.answer == ArenaBossAnswer::Dodge || src.answer == ArenaBossAnswer::Jump
            || src.answer == ArenaBossAnswer::Tile || src.answer == ArenaBossAnswer::Cover;
        mv.clipText = {};
        for (std::size_t c = 0; c < clip.size() && c < kMaxArenaClipId; ++c) mv.clipText[c] = clip[c];
    }
    out.moveCount = static_cast<std::uint8_t>(lay.count);
    kind = out;
    if (layout) *layout = lay;
    return true;
}

double stoneBearerClipEntry(const ArenaEnemyKind& kind, const StoneBearerLayout& layout, std::size_t move, double impact) {
    if (move >= kind.moveCount) return 0.0;
    // A move that runs on starts where the one before it ended (chained moves are laid out next to each other).
    std::size_t root = move;
    double ran = 0.0;
    while (root > 0 && root < layout.count && layout.runsOn[root]) {
        --root;
        const ArenaAttack& a = kind.moves[root].attack;
        ran += static_cast<double>(a.startup) + static_cast<double>(a.active) + static_cast<double>(a.recovery);
    }
    return impact - static_cast<double>(kind.moves[root].attack.startup) + ran;
}

} // namespace ftcore
