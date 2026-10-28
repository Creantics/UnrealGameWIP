#include "ftcore/BeatFight.h"

#include "SeedMix.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace ftcore {

namespace {

// Slack for "at least" and "at or before" tests on grid arithmetic (beats and seconds that went
// through a division).
constexpr double kBeatSlack = 1e-9;
constexpr double kBeatNever = 1e300; // a time that never comes
// The events one call may process: a bound, so an absurd clock jump can't hang a frame.
constexpr int kBeatMaxSteps = 1 << 20;
constexpr std::size_t kBeatNone = static_cast<std::size_t>(-1);
constexpr double kBeatBarBeats = 4.0;  // a 4/4 bar
constexpr double kBeatSlotBeats = 0.5; // a slot is an eighth

// xorshift32, as WorldRng in WorldLayout.cpp. The state lives in the fight (BeatFight::rng_) and is
// seeded through detail::mixSeed, so a copied or restarted fight replays the same draws.
std::uint32_t beatDraw(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

bool beatArrow(char c, Arrow& out) {
    switch (c) {
    case 'L': out = Arrow::Left; return true;
    case 'D': out = Arrow::Down; return true;
    case 'U': out = Arrow::Up; return true;
    case 'R': out = Arrow::Right; return true;
    default: return false;
    }
}

// The error text is built only when the caller wants it: parsing a pattern allocates nothing else.
bool beatPatternFailSlot(std::string* error, std::size_t slot, const char* why, std::string_view token) {
    if (error) {
        *error = "slot " + std::to_string(slot) + ": " + why + " (\"";
        error->append(token.data(), token.size());
        *error += "\")";
    }
    return false;
}

// Within `perfect` of the eighth is Perfect, within `good` Good; Pending outside.
QteGrade beatStrikeGrade(double offset, const BeatWindows& w) {
    const double d = std::fabs(offset);
    if (d <= w.perfect) return QteGrade::Perfect;
    return d <= w.good ? QteGrade::Good : QteGrade::Pending;
}

bool beatHeld(const BeatPrompt& p) { return p.kind == BeatKind::Hold && p.grade != QteGrade::Pending; }

BeatEvent beatEvent(BeatEventType type, std::uint8_t enemy, std::uint32_t prompt, QteGrade grade, BeatKind kind,
                    int amount, double time) {
    BeatEvent e;
    e.type = type;
    e.enemy = enemy;
    e.prompt = prompt;
    e.grade = grade;
    e.kind = kind;
    e.amount = amount;
    e.time = time;
    return e;
}

} // namespace

// ---- patterns ----

bool parseBeatPattern(std::string_view text, BeatPattern& out, std::string* error) {
    // Split on blanks. Only the first kBeatSlots tokens are kept; the rest are only counted.
    std::array<std::string_view, kBeatSlots> tokens{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == ' ' || text[i] == '\t') {
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
        if (count < kBeatSlots) tokens[count] = text.substr(start, i - start);
        ++count;
    }
    if (count != kBeatSlots) {
        if (error) *error = "expected " + std::to_string(kBeatSlots) + " slots, found " + std::to_string(count);
        return false;
    }

    BeatPattern parsed;
    std::size_t dashes = 0; // the `-` slots the last hold still needs
    Arrow holdArrow = Arrow::Left;
    for (std::size_t s = 0; s < kBeatSlots; ++s) {
        const std::string_view t = tokens[s];
        BeatSlot& slot = parsed.slots[s];
        if (t == "-") {
            if (dashes == 0) return beatPatternFailSlot(error, s, "stray -, no hold before it", t);
            slot.kind = BeatKind::Held;
            slot.input = holdArrow;
            --dashes;
            continue;
        }
        if (dashes > 0) return beatPatternFailSlot(error, s, "a hold is missing its - slots", t);
        if (t == ".") continue; // a rest
        Arrow a = Arrow::Left;
        const bool shaped = t.size() >= 2 && (t[0] == 's' || t[0] == 'c' || t[0] == 'h') && beatArrow(t[1], a);
        if (!shaped) return beatPatternFailSlot(error, s, "bad token, expected . or s<A> c<A> h<A><n>", t);
        if (t[0] != 'h') {
            if (t.size() != 2) return beatPatternFailSlot(error, s, "bad token, expected . or s<A> c<A> h<A><n>", t);
            slot.kind = t[0] == 's' ? BeatKind::Strike : BeatKind::Counter;
            slot.input = a;
            continue;
        }
        // A hold: h<A><n>, then n - 1 `-` slots.
        if (t.size() < 3) return beatPatternFailSlot(error, s, "a hold needs its length, h<A><n>", t);
        std::size_t length = 0;
        for (std::size_t k = 2; k < t.size(); ++k) {
            if (t[k] < '0' || t[k] > '9') return beatPatternFailSlot(error, s, "bad token, a hold's length is a number", t);
            length = std::min<std::size_t>(length * 10 + static_cast<std::size_t>(t[k] - '0'), 99);
        }
        if (length < 2 || length > kBeatSlots) return beatPatternFailSlot(error, s, "a hold lasts 2..8 eighths", t);
        if (s + length > kBeatSlots) return beatPatternFailSlot(error, s, "a hold runs past the bar", t);
        slot.kind = BeatKind::Hold;
        slot.input = a;
        slot.length = static_cast<std::uint8_t>(length);
        holdArrow = a;
        dashes = length - 1;
    }
    out = parsed;
    return true;
}

BeatFightDef defaultBeatFightDef() {
    BeatFightDef def;
    def.enemyCount = 1;
    BeatEnemy& wisp = def.enemies[0];
    wisp.patternCount = 2;
    parseBeatPattern("sL . sR . cU . . .", wisp.patterns[0]);
    parseBeatPattern("cD . . . sL . hU2 -", wisp.patterns[1]);
    return def;
}

// ---- the fight: setup ----

void BeatFight::begin(const BeatFightDef& def, const BeatTuning& tuning, double now, std::uint32_t seed,
                      const QteAssist& assist) {
    def_ = def;
    tuning_ = tuning;
    assist_ = assist;
    seed_ = seed;
    stats_ = BeatStats{};
    droppedEvents_ = 0;

    // The loader refuses numbers outside their ranges; a def built in code reads as the nearest sane
    // value instead, so no divide by zero and no empty fight.
    if (!(def_.bpm > 0.0) || !std::isfinite(def_.bpm)) def_.bpm = 120.0;
    if (!std::isfinite(def_.offset)) def_.offset = 0.0;
    def_.hp = std::max(1, def_.hp);
    def_.enemyCount = static_cast<std::uint8_t>(std::clamp<std::size_t>(def_.enemyCount, 1, kMaxBeatEnemies));
    for (BeatEnemy& e : def_.enemies) {
        e.hp = std::max(1, e.hp);
        e.damage = std::max(0, e.damage);
        e.patternCount = static_cast<std::uint8_t>(std::clamp<std::size_t>(e.patternCount, 1, kMaxBeatPatterns));
    }
    tuning_.approachBeats = std::clamp(tuning_.approachBeats, 0.0, 4.0);
    tuning_.leadInBars = std::max(0, tuning_.leadInBars);
    tuning_.chainPerStep = std::max(1, tuning_.chainPerStep);
    tuning_.maxMultiplier = std::max(1, tuning_.maxMultiplier);
    tuning_.finisherLength = std::clamp(tuning_.finisherLength, 1, static_cast<int>(kBeatSlots));
    tuning_.holdSlack = std::max(0.0, tuning_.holdSlack);

    // The windows, once: the tuning's, capped for this tempo.
    strikeWindows_ = effectiveStrikeWindows(tuning_, def_.bpm);
    counterWindows_ = effectiveCounterWindows(tuning_, def_.bpm);
    counterHalf_ = counterWindows_.parry * 0.5;
    lockoutLength_ =
        std::max(0.0, std::min(tuning_.counter.parryLockout, tuning_.counterLockoutBeats * def_.secondsPerBeat()));
    approachSeconds_ = tuning_.approachBeats * def_.secondsPerBeat();
    reset(now);
}

void BeatFight::reset(double now) {
    now_ = now;
    phase_ = BeatPhase::Playing;
    hp_ = maxHp_ = def_.hp;
    for (std::size_t i = 0; i < enemyHp_.size(); ++i) enemyHp_[i] = i < def_.enemyCount ? def_.enemies[i].hp : 0;
    chain_ = 0;
    rng_ = detail::mixSeed(seed_);
    lastPattern_.fill(-1);
    turnCursor_ = 0;
    nextGenBar_ = 0;
    nextStartBar_ = 0;
    bars_.fill(Bar{});
    nextId_ = 1;
    lockoutUntil_ = -1e30;
    promptCount_ = 0;
    landed_.fill(Landed{});
    landedNext_ = 0;
    // The first bar line at least leadInBars bars after `now`.
    const double lead = static_cast<double>(tuning_.leadInBars) * kBeatBarBeats;
    firstBarBeat_ = kBeatBarBeats * std::ceil((def_.beatAt(now) + lead) / kBeatBarBeats - kBeatSlack) + 0.0;
}

void BeatFight::restart(double now) {
    if (phase_ == BeatPhase::NotStarted) return;
    reset(now);
    ++stats_.restarts;
}

// ---- the fight: what the host asks ----

BeatPhase BeatFight::phase() const { return phase_; }
double BeatFight::now() const { return now_; }
const BeatFightDef& BeatFight::def() const { return def_; }
const BeatTuning& BeatFight::tuning() const { return tuning_; }
const BeatStats& BeatFight::stats() const { return stats_; }
std::uint32_t BeatFight::droppedEvents() const { return droppedEvents_; }
double BeatFight::firstBarBeat() const { return firstBarBeat_; }

std::size_t BeatFight::promptCount() const { return promptCount_; }

const BeatPrompt& BeatFight::prompt(std::size_t i) const { return prompts_[std::min(i, prompts_.size() - 1)]; }

double BeatFight::approach(const BeatPrompt& p, double t) const {
    const double left = p.time - t;
    if (left > approachSeconds_) return -1.0; // not showing yet
    if (left <= 0.0 || approachSeconds_ <= 0.0) return 0.0;
    return left / approachSeconds_;
}

int BeatFight::hp() const { return hp_; }
int BeatFight::maxHp() const { return maxHp_; }
std::size_t BeatFight::enemyCount() const { return def_.enemyCount; }
int BeatFight::enemyHp(std::size_t i) const { return i < def_.enemyCount ? enemyHp_[i] : 0; }
int BeatFight::enemyMaxHp(std::size_t i) const { return i < def_.enemyCount ? def_.enemies[i].hp : 0; }
bool BeatFight::enemyAlive(std::size_t i) const { return enemyHp(i) > 0; }
int BeatFight::chain() const { return chain_; }

int BeatFight::multiplier() const {
    return std::min(tuning_.maxMultiplier, 1 + chain_ / tuning_.chainPerStep);
}

int BeatFight::turnEnemy() const {
    if (phase_ != BeatPhase::Playing) return -1;
    const double bars = (def_.beatAt(now_) - firstBarBeat_) / kBeatBarBeats;
    if (bars < -kBeatSlack) return -1; // the lead-in
    const std::int32_t index = static_cast<std::int32_t>(std::floor(bars + kBeatSlack));
    for (const Bar& b : bars_)
        if (b.index == index) return b.enemy >= 0 && enemyHp_[static_cast<std::size_t>(b.enemy)] > 0 ? b.enemy : -1;
    return -1;
}

bool BeatFight::holding() const {
    for (std::size_t i = 0; i < promptCount_; ++i)
        if (beatHeld(prompts_[i])) return true;
    return false;
}

double BeatFight::holdProgress() const {
    for (std::size_t i = 0; i < promptCount_; ++i) {
        const BeatPrompt& p = prompts_[i];
        if (!beatHeld(p)) continue;
        const double span = p.end - p.time;
        return span > 0.0 ? std::clamp((now_ - p.time) / span, 0.0, 1.0) : 1.0;
    }
    return 0.0;
}

bool BeatFight::counterLockedOut(double t) const { return t < lockoutUntil_; }

// ---- the fight: time ----

BeatEvents BeatFight::update(double now) {
    BeatEvents out;
    if (phase_ == BeatPhase::Playing) runTo(now, out);
    return out;
}

BeatEvents BeatFight::press(Arrow input, double t) {
    BeatEvents out;
    if (phase_ == BeatPhase::Playing && !assist_.autoComplete) doPress(input, t, out);
    return out;
}

BeatEvents BeatFight::release(Arrow input, double t) {
    BeatEvents out;
    if (phase_ == BeatPhase::Playing && !assist_.autoComplete) doRelease(input, t, out);
    return out;
}

double BeatFight::generationTime() const {
    return def_.timeAtBeat(firstBarBeat_ + kBeatBarBeats * static_cast<double>(nextGenBar_) - tuning_.approachBeats);
}

double BeatFight::startTime() const {
    return def_.timeAtBeat(firstBarBeat_ + kBeatBarBeats * static_cast<double>(nextStartBar_));
}

// When a prompt resolves if nothing answers it: a strike (or a hold nobody pressed) when the clock is
// past its window, a counter when it lands, a held hold when it ends.
double BeatFight::dueTime(const BeatPrompt& p) const {
    if (p.kind == BeatKind::Counter) return p.time + counterHalf_;
    if (beatHeld(p)) return p.end;
    return p.time + strikeWindows_.good;
}

bool BeatFight::isDue(const BeatPrompt& p, double t) const {
    const double due = dueTime(p);
    // A press at the last moment of a strike's window still counts; at the impact a counter has
    // landed, and at its end a hold is done.
    return p.kind == BeatKind::Counter || beatHeld(p) ? due <= t : due < t;
}

void BeatFight::advance(double t, BeatEvents& out) {
    if (phase_ != BeatPhase::Playing) return;
    if (!(t > now_)) t = now_;
    enum class What : std::uint8_t { None, Generate, Expire, Start };
    for (int step = 0; step < kBeatMaxSteps && phase_ == BeatPhase::Playing; ++step) {
        // The earliest thing due by t. At the same moment: a bar is generated first, then prompts
        // resolve, then a bar starts.
        What what = What::None;
        double when = kBeatNever;
        std::size_t which = kBeatNone;
        const double gen = generationTime();
        if (gen <= t) {
            what = What::Generate;
            when = gen;
        }
        for (std::size_t i = 0; i < promptCount_; ++i) {
            const double due = dueTime(prompts_[i]);
            if (due < when && isDue(prompts_[i], t)) {
                what = What::Expire;
                when = due;
                which = i;
            }
        }
        const double start = startTime();
        if (start <= t && start < when) {
            what = What::Start;
            when = start;
        }
        switch (what) {
        case What::Generate: generate(when, out); break;
        case What::Expire: expire(which, out); break;
        case What::Start:
            ++stats_.bars;
            ++nextStartBar_;
            break;
        case What::None: break;
        }
        if (what == What::None) break;
    }
    now_ = t;
}

void BeatFight::runTo(double t, BeatEvents& out) {
    if (!(t > now_)) t = now_;
    if (!assist_.autoComplete) {
        advance(t, out);
        return;
    }
    // Auto-complete: play the autopilot's moves in time order. A bar is generated (and so can hold
    // prompts the autopilot hasn't seen) before any move after its generation time.
    for (int step = 0; step < kBeatMaxSteps && phase_ == BeatPhase::Playing; ++step) {
        const double stop = std::min(t, generationTime());
        const QteAction a = pilot(stop);
        if (a.kind == QteAction::Kind::Press) {
            doPress(a.input, a.time, out);
            continue;
        }
        if (a.kind == QteAction::Kind::Release) {
            doRelease(a.input, a.time, out);
            continue;
        }
        advance(stop, out);
        if (stop >= t) break;
    }
}

void BeatFight::doPress(Arrow input, double t, BeatEvents& out) {
    advance(t, out);
    if (phase_ != BeatPhase::Playing) return;
    finishHolds(now_, out);
    if (phase_ != BeatPhase::Playing) return;
    judgePress(input, now_, out);
}

void BeatFight::doRelease(Arrow input, double t, BeatEvents& out) {
    advance(t, out);
    if (phase_ != BeatPhase::Playing || assist_.holdToggle) return; // a toggled hold latches
    finishHolds(now_, out);
    if (phase_ != BeatPhase::Playing) return;
    for (std::size_t i = 0; i < promptCount_; ++i) {
        const BeatPrompt& p = prompts_[i];
        if (beatHeld(p) && p.input == input) {
            resolve(i, QteGrade::Miss, now_, out); // let go too early: broken
            return;
        }
    }
}

// Holds within holdSlack of their end are done: a press or a release that close to the end completes
// the hold before it does anything else.
void BeatFight::finishHolds(double t, BeatEvents& out) {
    while (phase_ == BeatPhase::Playing) {
        std::size_t found = kBeatNone;
        for (std::size_t i = 0; i < promptCount_ && found == kBeatNone; ++i)
            if (beatHeld(prompts_[i]) && t >= prompts_[i].end - tuning_.holdSlack) found = i;
        if (found == kBeatNone) return;
        resolve(found, prompts_[found].grade, t, out);
    }
}

// ---- the fight: the schedule ----

void BeatFight::generate(double time, BeatEvents& out) {
    const std::int32_t index = nextGenBar_++;
    const double barBeat = firstBarBeat_ + kBeatBarBeats * static_cast<double>(index);
    const std::size_t count = def_.enemyCount;

    // Its enemy: the next living one in turn order.
    std::size_t enemy = kBeatNone;
    for (std::size_t k = 0; k < count && enemy == kBeatNone; ++k) {
        const std::size_t candidate = (static_cast<std::size_t>(turnCursor_) + k) % count;
        if (enemyHp_[candidate] > 0) enemy = candidate;
    }
    bars_[0] = bars_[1];
    bars_[1].index = index;
    bars_[1].enemy = enemy == kBeatNone ? -1 : static_cast<std::int32_t>(enemy);
    if (enemy == kBeatNone) return; // nobody left (the fight is over)
    turnCursor_ = static_cast<int>((enemy + 1) % count);
    const std::uint8_t who = static_cast<std::uint8_t>(enemy);
    const BeatEnemy& e = def_.enemies[enemy];

    // A finisher instead of a pattern when the enemy is low: at most finisherAt of its max hp.
    const double lowHp = tuning_.finisherAt * static_cast<double>(e.hp) + kBeatSlack;
    const bool finisher = tuning_.finisherAt > 0.0 && static_cast<double>(enemyHp_[enemy]) <= lowHp;
    if (finisher) {
        push(out, beatEvent(BeatEventType::FinisherStart, who, 0, QteGrade::Pending, BeatKind::Rest, 0, time));
        std::uint32_t previous = 0;
        for (int k = 0; k < tuning_.finisherLength; ++k) {
            // Random arrows, never the same one twice in a row.
            std::uint32_t arrow = 0;
            if (k == 0) {
                arrow = beatDraw(rng_) % 4u;
            } else {
                arrow = beatDraw(rng_) % 3u;
                if (arrow >= previous) ++arrow;
            }
            previous = arrow;
            addPrompt(BeatKind::Strike, static_cast<Arrow>(arrow), who, 0, true, barBeat + kBeatSlotBeats * k);
        }
        return;
    }

    // A pattern: never the same one twice in a row for an enemy that has more than one.
    const std::size_t patterns = e.patternCount;
    std::size_t pick = 0;
    if (patterns > 1) {
        const int last = lastPattern_[enemy];
        const std::size_t choices = last >= 0 ? patterns - 1 : patterns;
        pick = beatDraw(rng_) % choices;
        if (last >= 0 && pick >= static_cast<std::size_t>(last)) ++pick;
    }
    lastPattern_[enemy] = static_cast<std::int8_t>(pick);
    const BeatPattern& pattern = e.patterns[pick];
    for (std::size_t s = 0; s < kBeatSlots; ++s) {
        const BeatSlot& slot = pattern.slots[s];
        if (slot.kind == BeatKind::Strike || slot.kind == BeatKind::Counter || slot.kind == BeatKind::Hold) {
            std::uint8_t length = 0;
            if (slot.kind == BeatKind::Hold)
                length = static_cast<std::uint8_t>(std::clamp<int>(slot.length, 1, static_cast<int>(kBeatSlots)));
            addPrompt(slot.kind, slot.input, who, length, false, barBeat + kBeatSlotBeats * static_cast<double>(s));
        }
    }
}

void BeatFight::addPrompt(BeatKind kind, Arrow input, std::uint8_t enemy, std::uint8_t length, bool finisher,
                          double beat) {
    if (promptCount_ >= prompts_.size()) return; // full: not with the shipped limits
    BeatPrompt& p = prompts_[promptCount_++];
    p = BeatPrompt{};
    p.id = nextId_++;
    p.kind = kind;
    p.input = input;
    p.enemy = enemy;
    p.length = length;
    p.finisher = finisher;
    p.beat = beat;
    p.time = def_.timeAtBeat(beat);
    p.end = kind == BeatKind::Hold ? def_.timeAtBeat(beat + kBeatSlotBeats * static_cast<double>(length)) : p.time;
}

// ---- the fight: judging ----

BeatFight::Judgement BeatFight::judge(const BeatPrompt& p, double t) const {
    Judgement j;
    if (p.kind != BeatKind::Counter) {
        // A strike, and the press of a hold: symmetric windows around the eighth.
        j.grade = beatStrikeGrade(t - p.time, strikeWindows_);
        j.reach = j.grade == QteGrade::Pending ? Reach::Outside : Reach::Answers;
        return j;
    }
    if (approach(p, t) < 0.0) {
        j.reach = Reach::Hidden; // its marker isn't showing yet
        return j;
    }
    // The defence maths of DefenceTimeline, as a Narrow QTE prompt is judged: one hit whose impact is
    // half a parry window after the eighth, a parry press first, a dodge as the Good fallback.
    AttackTimeline hit;
    hit.clear();
    TimelineHit h;
    h.at = 0.0;
    h.tell = 0.0;
    hit.add(h);
    DefenceTimeline timeline;
    timeline.reset(hit, p.time + counterHalf_, counterWindows_);
    DefenceResults r = timeline.press(DefenceInput::Parry, t);
    if (r.empty()) r = timeline.press(DefenceInput::Dodge, t);
    if (r.empty() || r[0].outcome == DefenceOutcome::Hit) return j; // too early
    j.grade = r[0].outcome == DefenceOutcome::Perfect ? QteGrade::Perfect : QteGrade::Good;
    j.reach = counterLockedOut(t) ? Reach::LockedOut : Reach::Answers;
    return j;
}

void BeatFight::judgePress(Arrow input, double t, BeatEvents& out) {
    // The earliest open prompt not yet pressed, and the one after it.
    std::size_t first = kBeatNone, second = kBeatNone;
    for (std::size_t i = 0; i < promptCount_; ++i) {
        if (beatHeld(prompts_[i])) continue;
        if (first == kBeatNone) {
            first = i;
        } else {
            second = i;
            break;
        }
    }
    if (first != kBeatNone) {
        const BeatPrompt p0 = prompts_[first];
        if (p0.input == input) {
            const Judgement j = judge(p0, t);
            if (j.reach == Reach::Answers) {
                answer(first, j.grade, t, out);
                return;
            }
            if (j.reach == Reach::LockedOut) {
                ++stats_.ignored;
                push(out, beatEvent(BeatEventType::LockedOut, p0.enemy, p0.id, QteGrade::Pending, p0.kind, 0, t));
                return;
            }
        } else if (second != kBeatNone && prompts_[second].input == input) {
            // Another arrow than the earliest prompt's, but the next one's, inside its window: the
            // player skipped a note.
            const BeatPrompt p1 = prompts_[second];
            const Judgement j = judge(p1, t);
            if (j.reach == Reach::Answers) {
                resolve(first, QteGrade::Miss, t, out);
                if (phase_ != BeatPhase::Playing) return;
                const std::size_t at = indexOf(p1.id);
                if (at != kBeatNone) {
                    answer(at, j.grade, t, out);
                } else { // dropped with the broken finisher: nothing left to answer
                    ++stats_.strays;
                    push(out, beatEvent(BeatEventType::Stray, 0, 0, QteGrade::Pending, BeatKind::Rest, 0, t));
                }
                return;
            }
            if (j.reach == Reach::LockedOut) {
                ++stats_.ignored;
                push(out, beatEvent(BeatEventType::LockedOut, p1.enemy, p1.id, QteGrade::Pending, p1.kind, 0, t));
                return;
            }
        }
    }
    classify(input, t, out);
}

// A press that answered nothing.
void BeatFight::classify(Arrow input, double t, BeatEvents& out) {
    // Just after a counter of that arrow landed: LATE, nothing else.
    std::size_t late = kBeatNone;
    for (std::size_t i = 0; i < landed_.size(); ++i) {
        const Landed& l = landed_[i];
        if (!l.open || l.input != input) continue;
        const double after = t - l.impact;
        if (after < 0.0 || after >= counterWindows_.late) continue;
        if (late == kBeatNone || l.impact > landed_[late].impact) late = i;
    }
    if (late != kBeatNone) {
        Landed& landed = landed_[late];
        landed.open = false; // once per counter
        push(out, beatEvent(BeatEventType::Late, landed.enemy, landed.id, QteGrade::Pending, BeatKind::Counter, 0, t));
        return;
    }
    // The arrow of a counter that is showing, before its dodge window opened: a whiff, and counters are
    // locked out for a moment.
    for (std::size_t i = 0; i < promptCount_; ++i) {
        const BeatPrompt& p = prompts_[i];
        if (p.kind != BeatKind::Counter || p.input != input || judge(p, t).reach != Reach::Outside) continue;
        lockoutUntil_ = t + lockoutLength_;
        ++stats_.whiffs;
        push(out, beatEvent(BeatEventType::Whiff, p.enemy, p.id, QteGrade::Pending, BeatKind::Counter,
                            static_cast<int>(std::lround(lockoutLength_ * 1000.0)), t));
        return;
    }
    ++stats_.strays;
    push(out, beatEvent(BeatEventType::Stray, 0, 0, QteGrade::Pending, BeatKind::Rest, 0, t));
}

// A press answered prompt i. A hold is only pressed (it resolves at its end); the others resolve now.
void BeatFight::answer(std::size_t i, QteGrade grade, double t, BeatEvents& out) {
    if (prompts_[i].kind == BeatKind::Hold) {
        prompts_[i].grade = grade;
        return;
    }
    resolve(i, grade, t, out);
}

void BeatFight::expire(std::size_t i, BeatEvents& out) {
    const BeatPrompt p = prompts_[i];
    const double due = dueTime(p);
    if (p.kind == BeatKind::Counter) {
        Landed& l = landed_[landedNext_];
        l.id = p.id;
        l.input = p.input;
        l.enemy = p.enemy;
        l.open = true;
        l.impact = due;
        landedNext_ = (landedNext_ + 1) % landed_.size();
        resolve(i, QteGrade::Miss, due, out);
    } else if (beatHeld(p)) {
        resolve(i, p.grade, due, out); // held to its end
    } else {
        resolve(i, QteGrade::Miss, due, out);
    }
}

// Prompt i is over with `grade`: the chain, the damage and what follows (a hit lead, a broken finisher,
// a down enemy, the end of the fight).
void BeatFight::resolve(std::size_t i, QteGrade grade, double time, BeatEvents& out) {
    const BeatPrompt p = prompts_[i];
    const std::size_t left = p.finisher ? finisherLeft(p) : 0; // p included
    removePrompt(i);
    push(out, beatEvent(BeatEventType::Judged, p.enemy, p.id, grade, p.kind, 0, time));
    const int before = multiplier();

    if (grade == QteGrade::Miss) {
        ++stats_.misses;
        chain_ = 0;
        if (multiplier() != before)
            push(out, beatEvent(BeatEventType::Multiplier, 0, 0, QteGrade::Pending, BeatKind::Rest, multiplier(), time));
        if (p.kind == BeatKind::Counter) hitLead(p, time, out);
        if (phase_ != BeatPhase::Playing) return;
        if (p.finisher) {
            push(out, beatEvent(BeatEventType::FinisherBroken, p.enemy, p.id, QteGrade::Miss, p.kind, 0, time));
            dropFinisher(p);
        }
        return;
    }

    // An answer: Perfect or Good. The damage uses the multiplier from before it counts.
    if (grade == QteGrade::Perfect) ++stats_.perfects;
    else ++stats_.goods;
    if (p.kind == BeatKind::Counter) ++stats_.counters;
    ++chain_;
    stats_.maxChain = std::max(stats_.maxChain, chain_);

    int& hp = enemyHp_[p.enemy];
    int amount = 0;
    if (p.finisher) {
        // The enemy's hp over the strikes left, rounded up: the last one takes the rest.
        const int strikes = static_cast<int>(std::max<std::size_t>(left, 1));
        amount = (hp + strikes - 1) / strikes;
    } else {
        int base = def_.strikeDamage;
        if (p.kind == BeatKind::Counter) base = def_.counterDamage;
        else if (p.kind == BeatKind::Hold) base = def_.holdDamage * static_cast<int>(p.length);
        if (grade == QteGrade::Good) base = static_cast<int>(std::lround(static_cast<double>(base) * tuning_.goodFactor));
        amount = base * before;
    }
    hp = std::max(0, hp - amount);
    stats_.damageDealt += amount;
    push(out, beatEvent(BeatEventType::EnemyHit, p.enemy, p.id, grade, p.kind, amount, time));
    if (multiplier() != before)
        push(out, beatEvent(BeatEventType::Multiplier, 0, 0, QteGrade::Pending, BeatKind::Rest, multiplier(), time));
    if (p.finisher && left <= 1) ++stats_.finishers;
    if (hp > 0) return;

    push(out, beatEvent(BeatEventType::EnemyDown, p.enemy, 0, QteGrade::Pending, BeatKind::Rest, 0, time));
    dropEnemy(p.enemy);
    for (std::size_t e = 0; e < def_.enemyCount; ++e)
        if (enemyHp_[e] > 0) return;
    phase_ = BeatPhase::Victory;
    push(out, beatEvent(BeatEventType::Victory, 0, 0, QteGrade::Pending, BeatKind::Rest, 0, time));
}

void BeatFight::hitLead(const BeatPrompt& p, double time, BeatEvents& out) {
    const int damage = def_.enemies[p.enemy].damage;
    hp_ = std::max(0, hp_ - damage);
    stats_.damageTaken += damage;
    push(out, beatEvent(BeatEventType::LeadHit, p.enemy, p.id, QteGrade::Miss, p.kind, damage, time));
    if (hp_ > 0) return;
    phase_ = BeatPhase::Defeat;
    push(out, beatEvent(BeatEventType::Defeat, 0, 0, QteGrade::Pending, BeatKind::Rest, 0, time));
}

// ---- the fight: the open list ----

void BeatFight::removePrompt(std::size_t i) {
    for (std::size_t k = i + 1; k < promptCount_; ++k) prompts_[k - 1] = prompts_[k];
    --promptCount_;
}

void BeatFight::dropEnemy(std::uint8_t enemy) {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < promptCount_; ++i)
        if (prompts_[i].enemy != enemy) prompts_[kept++] = prompts_[i];
    promptCount_ = kept;
}

double BeatFight::barOf(const BeatPrompt& p) const {
    return std::floor((p.beat - firstBarBeat_) / kBeatBarBeats + kBeatSlack);
}

void BeatFight::dropFinisher(const BeatPrompt& p) {
    const double bar = barOf(p);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < promptCount_; ++i) {
        const BeatPrompt& q = prompts_[i];
        const bool same = q.finisher && q.enemy == p.enemy && barOf(q) == bar;
        if (!same) prompts_[kept++] = q;
    }
    promptCount_ = kept;
}

std::size_t BeatFight::finisherLeft(const BeatPrompt& p) const {
    const double bar = barOf(p);
    std::size_t n = 0;
    for (std::size_t i = 0; i < promptCount_; ++i) {
        const BeatPrompt& q = prompts_[i];
        if (q.finisher && q.enemy == p.enemy && barOf(q) == bar) ++n;
    }
    return n;
}

std::size_t BeatFight::indexOf(std::uint32_t id) const {
    for (std::size_t i = 0; i < promptCount_; ++i)
        if (prompts_[i].id == id) return i;
    return kBeatNone;
}

void BeatFight::push(BeatEvents& out, const BeatEvent& e) {
    if (out.count >= out.items.size()) {
        ++droppedEvents_;
        return;
    }
    out.items[out.count++] = e;
}

// ---- the autopilot ----

QteAction BeatFight::autopilot(double now) const { return pilot(now); }

QteAction BeatFight::pilot(double horizon) const {
    QteAction a;
    if (phase_ != BeatPhase::Playing) return a;
    const BeatPrompt* held = nullptr; // the hold that ends first
    const BeatPrompt* next = nullptr; // the earliest prompt not pressed yet
    for (std::size_t i = 0; i < promptCount_; ++i) {
        const BeatPrompt& p = prompts_[i];
        if (beatHeld(p)) {
            if (!held || p.end < held->end) held = &p;
        } else if (!next) {
            next = &p;
        }
    }
    // Every prompt is pressed on its beat (Perfect) and a hold let go of at its end. A release goes
    // before a press at the same time. A move is never in the past of the fight's clock, and a press
    // that couldn't answer its prompt (the clock is already past its window) is never made: the prompt
    // runs out, and the autopilot never strays.
    const double releaseAt = held ? std::max(held->end, now_) : kBeatNever;
    double pressAt = next ? std::max(next->time, now_) : kBeatNever;
    if (next && judge(*next, pressAt).reach != Reach::Answers) {
        next = nullptr;
        pressAt = kBeatNever;
    }
    if (held && releaseAt <= pressAt) {
        if (releaseAt <= horizon) {
            a.kind = QteAction::Kind::Release;
            a.input = held->input;
            a.time = releaseAt;
        }
    } else if (next && pressAt <= horizon) {
        a.kind = QteAction::Kind::Press;
        a.input = next->input;
        a.time = pressAt;
    }
    return a;
}

// ---- data ----

const BeatEntry* BeatTable::findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    for (const BeatEntry& e : entries)
        if (e.beat == beat && e.step == step && e.scene == scene) return &e;
    return nullptr;
}

const BeatSetup& BeatTable::find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    const BeatEntry* e = findEntry(scene, beat, step);
    return e ? e->setup : fallback;
}

namespace {

// Number ranges the loader checks (anything outside is an error with its path):
//   fight: bpm 40..240, offset -60..600 (seconds), hp, strikeDamage, counterDamage and holdDamage 1..9999,
//          enemies 1..kMaxBeatEnemies ids, music at most 64 bytes;
//   tuning: strike.perfect and strike.good 0.005..0.5 (perfect <= good), counter.dodge and counter.parry
//          0.02..1 (parry <= dodge), counter.parryLockout 0..2, counter.late 0..1, holdSlack 0..0.5
//          (all seconds);
//   enemy: hp and damage 1..9999, 1..kMaxBeatPatterns patterns, each with at least one counter;
//   entry: beat 0..1000000, step 0..1000.
constexpr double kBeatMinBpm = 40.0, kBeatMaxBpm = 240.0;
constexpr double kBeatMinOffset = -60.0, kBeatMaxOffset = 600.0;
constexpr int kBeatMaxNumber = 9999;
constexpr std::size_t kBeatMaxMusic = 64;
constexpr std::size_t kBeatMaxId = 48;

std::string beatNumberText(double v) {
    char text[32];
    std::snprintf(text, sizeof text, "%g", v);
    return text;
}

// Letters, digits, '_' and '-'; not empty.
bool beatIdText(std::string_view s) {
    if (s.empty() || s.size() > kBeatMaxId) return false;
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

void beatUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                     std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

// An optional number in [lo, hi]: false (nothing written) when absent, or when bad (and an error).
bool beatNumber(JsonValue j, const char* key, double& out, double lo, double hi, const std::string& path,
                std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return false;
    const double d = v.number(-1e30);
    if (!v.isNumber() || !std::isfinite(d) || d < lo || d > hi) {
        errors.push_back(path + "." + key + ": expected a number in " + beatNumberText(lo) + ".." + beatNumberText(hi));
        return false;
    }
    out = d;
    return true;
}

bool beatInteger(JsonValue j, const char* key, int& out, int lo, int hi, const std::string& path,
                 std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return false;
    if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
        errors.push_back(path + "." + key + ": expected an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
        return false;
    }
    out = static_cast<int>(v.integer());
    return true;
}

int beatEnemyIndex(const BeatTable& table, std::string_view id) {
    for (std::size_t i = 0; i < table.enemyIds.size(); ++i)
        if (table.enemyIds[i] == id) return static_cast<int>(i);
    return -1;
}

void beatParseEnemy(JsonValue j, const std::string& path, BeatTable& table, std::vector<std::string>& errors,
                    std::vector<std::string>* warnings) {
    if (!j.isObject()) {
        errors.push_back(path + ": expected an object");
        return;
    }
    beatUnknownKeys(j, path, {"id", "hp", "damage", "patterns"}, warnings);
    BeatEnemy e;
    const std::string id(j["id"].string());
    bool idOk = true;
    if (!beatIdText(id)) {
        errors.push_back(path + ".id: expected 1.." + std::to_string(kBeatMaxId) + " letters, digits, _ and -");
        idOk = false;
    } else if (beatEnemyIndex(table, id) >= 0) {
        errors.push_back(path + ".id: duplicate enemy " + id);
        idOk = false;
    }
    beatInteger(j, "hp", e.hp, 1, kBeatMaxNumber, path, errors);
    beatInteger(j, "damage", e.damage, 1, kBeatMaxNumber, path, errors);

    const JsonValue patterns = j["patterns"];
    if (!patterns.isArray() || patterns.size() < 1 || patterns.size() > kMaxBeatPatterns) {
        errors.push_back(path + ".patterns: expected an array of 1.." + std::to_string(kMaxBeatPatterns) + " patterns");
    } else {
        e.patternCount = static_cast<std::uint8_t>(patterns.size());
        for (std::uint32_t k = 0; k < patterns.size(); ++k) {
            const std::string where = path + ".patterns[" + std::to_string(k) + "]";
            if (!patterns[k].isString()) {
                errors.push_back(where + ": expected a pattern string");
                continue;
            }
            std::string why;
            if (!parseBeatPattern(patterns[k].string(), e.patterns[k], &why))
                errors.push_back(where + ": " + why);
            else if (!e.patterns[k].hasCounter())
                errors.push_back(where + ": a pattern needs at least one counter (c<A>), so every bar threatens");
        }
    }
    if (!idOk) return;
    table.enemyIds.push_back(id);
    table.enemies.push_back(e);
}

// One fight's keys, applied over `setup` (the default fight's values, or the struct defaults for the
// default fight itself). `needEnemies`: the default fight must name its line-up.
void beatParseFight(JsonValue j, const std::string& path, const BeatTable& table, bool needEnemies, BeatSetup& setup,
                    std::vector<std::string>& errors, std::vector<std::string>* warnings) {
    BeatFightDef& def = setup.def;
    BeatTuning& tuning = setup.tuning;
    double d = 0.0;
    int n = 0;
    if (beatNumber(j, "bpm", d, kBeatMinBpm, kBeatMaxBpm, path, errors)) def.bpm = d;
    if (beatNumber(j, "offset", d, kBeatMinOffset, kBeatMaxOffset, path, errors)) def.offset = d;
    if (beatInteger(j, "hp", n, 1, kBeatMaxNumber, path, errors)) def.hp = n;
    if (beatInteger(j, "strikeDamage", n, 1, kBeatMaxNumber, path, errors)) def.strikeDamage = n;
    if (beatInteger(j, "counterDamage", n, 1, kBeatMaxNumber, path, errors)) def.counterDamage = n;
    if (beatInteger(j, "holdDamage", n, 1, kBeatMaxNumber, path, errors)) def.holdDamage = n;
    if (beatNumber(j, "holdSlack", d, 0.0, 0.5, path, errors)) tuning.holdSlack = d;

    const JsonValue list = j["enemies"];
    if (list.isAbsent()) {
        if (needEnemies)
            errors.push_back(path + ".enemies: expected an array of 1.." + std::to_string(kMaxBeatEnemies) + " enemy ids");
    } else if (!list.isArray() || list.size() < 1 || list.size() > kMaxBeatEnemies) {
        errors.push_back(path + ".enemies: expected an array of 1.." + std::to_string(kMaxBeatEnemies) + " enemy ids");
    } else {
        def.enemyCount = static_cast<std::uint8_t>(list.size());
        for (std::uint32_t k = 0; k < list.size(); ++k) {
            const std::string_view id = list[k].string();
            const int at = beatEnemyIndex(table, id);
            if (at < 0)
                errors.push_back(path + ".enemies[" + std::to_string(k) + "]: unknown enemy \"" + std::string(id) + "\"");
            else
                def.enemies[k] = table.enemies[static_cast<std::size_t>(at)];
        }
    }

    if (j.has("music")) {
        const JsonValue m = j["music"];
        if (!m.isString() || m.string().size() > kBeatMaxMusic)
            errors.push_back(path + ".music: expected a cue id string of at most " + std::to_string(kBeatMaxMusic) +
                             " bytes");
        else
            setup.music = std::string(m.string());
    }

    if (const JsonValue s = j["strike"]; !s.isAbsent()) {
        if (!s.isObject()) {
            errors.push_back(path + ".strike: expected an object");
        } else {
            beatUnknownKeys(s, path + ".strike", {"perfect", "good"}, warnings);
            if (beatNumber(s, "perfect", d, 0.005, 0.5, path + ".strike", errors)) tuning.strike.perfect = d;
            if (beatNumber(s, "good", d, 0.005, 0.5, path + ".strike", errors)) tuning.strike.good = d;
            if (tuning.strike.perfect > tuning.strike.good) errors.push_back(path + ".strike: perfect must not exceed good");
        }
    }
    if (const JsonValue c = j["counter"]; !c.isAbsent()) {
        if (!c.isObject()) {
            errors.push_back(path + ".counter: expected an object");
        } else {
            beatUnknownKeys(c, path + ".counter", {"dodge", "parry", "parryLockout", "late"}, warnings);
            if (beatNumber(c, "dodge", d, 0.02, 1.0, path + ".counter", errors)) tuning.counter.dodge = d;
            if (beatNumber(c, "parry", d, 0.02, 1.0, path + ".counter", errors)) tuning.counter.parry = d;
            if (beatNumber(c, "parryLockout", d, 0.0, 2.0, path + ".counter", errors)) tuning.counter.parryLockout = d;
            if (beatNumber(c, "late", d, 0.0, 1.0, path + ".counter", errors)) tuning.counter.late = d;
            if (tuning.counter.parry > tuning.counter.dodge)
                errors.push_back(path + ".counter: parry must not exceed dodge");
        }
    }
}

} // namespace

bool importBeatTable(std::string_view json, BeatTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = BeatTable{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
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
        errors.push_back("beat file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("beat file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    beatUnknownKeys(root, "", {"version", "enemies", "default", "fights"}, warnings);

    BeatTable table;
    const JsonValue enemies = root["enemies"];
    if (!enemies.isArray() || enemies.size() == 0) errors.push_back("enemies: expected a non-empty array");
    for (std::uint32_t i = 0; i < enemies.size() && enemies.isArray(); ++i)
        beatParseEnemy(enemies[i], "enemies[" + std::to_string(i) + "]", table, errors, warnings);

    const JsonValue def = root["default"];
    if (!def.isObject()) {
        errors.push_back("default: expected an object");
    } else {
        beatUnknownKeys(def, "default",
                        {"bpm", "offset", "hp", "strikeDamage", "counterDamage", "holdDamage", "enemies", "music", "strike",
                         "counter", "holdSlack"},
                        warnings);
        beatParseFight(def, "default", table, true, table.fallback, errors, warnings);
    }

    const JsonValue list = root["fights"];
    if (!list.isAbsent() && !list.isArray()) errors.push_back("fights: expected an array");
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "fights[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        beatUnknownKeys(j, path,
                        {"scene", "beat", "step", "bpm", "offset", "hp", "strikeDamage", "counterDamage", "holdDamage",
                         "enemies", "music", "strike", "counter", "holdSlack"},
                        warnings);
        BeatEntry e;
        e.scene = std::string(j["scene"].string());
        if (e.scene.empty()) errors.push_back(path + ".scene: expected a scene id");
        const JsonValue b = j["beat"];
        const JsonValue s = j["step"];
        bool good = !e.scene.empty();
        if (!b.isInteger() || b.integer() < 0 || b.integer() > 1000000) {
            errors.push_back(path + ".beat: expected an integer in 0..1000000");
            good = false;
        } else {
            e.beat = static_cast<std::uint32_t>(b.integer());
        }
        if (!s.isAbsent() && (!s.isInteger() || s.integer() < 0 || s.integer() > 1000)) {
            errors.push_back(path + ".step: expected an integer in 0..1000");
            good = false;
        } else {
            e.step = static_cast<std::uint32_t>(s.integer(0));
        }
        e.setup = table.fallback; // a missing key keeps the default fight's value
        beatParseFight(j, path, table, false, e.setup, errors, warnings);
        if (good && table.findEntry(e.scene, e.beat, e.step))
            errors.push_back(path + ": duplicate entry for " + e.scene + " beat " + std::to_string(e.beat) + " step " +
                             std::to_string(e.step));
        table.entries.push_back(std::move(e));
    }
    if (!finish()) return false;
    out = std::move(table);
    return true;
}

} // namespace ftcore
