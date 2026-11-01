#include "ftcore/BeatBoss.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace ftcore {

namespace {

constexpr double kBossSlack = 1e-9;
constexpr double kBossNever = 1e300;
// The events one call may process: a bound, so an absurd clock jump can't hang a frame.
constexpr int kBossMaxSteps = 1 << 20;
constexpr std::size_t kBossNone = static_cast<std::size_t>(-1);

constexpr std::array<const char*, 8> kBossAnswerKeys{"attack", "guard", "parry", "dodge",
                                                     "jump",   "hold",  "paired", "release"};

int bossRound(double v) { return static_cast<int>(std::lround(v)); }

int bossPopcount(std::uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

// The button a counter or strike is answered with.
ActionButton bossButton(const BeatBossPrompt& p) {
    switch (p.answer) {
    case BeatBossAnswer::Attack: return ActionButton::Attack;
    case BeatBossAnswer::Guard:
    case BeatBossAnswer::Parry: return ActionButton::Guard;
    case BeatBossAnswer::Dodge: return ActionButton::Dodge;
    case BeatBossAnswer::Jump: return ActionButton::Jump;
    case BeatBossAnswer::Hold:
    case BeatBossAnswer::Release:
    case BeatBossAnswer::Paired: return p.button;
    }
    return p.button;
}

int bossBeats(const BossBar& b, std::uint8_t beatEighths) {
    return (static_cast<int>(b.eighths) + beatEighths - 1) / beatEighths;
}

} // namespace

const char* beatBossAnswerKey(BeatBossAnswer a) {
    const auto i = static_cast<std::size_t>(a);
    return i < kBossAnswerKeys.size() ? kBossAnswerKeys[i] : "";
}

bool parseBeatBossAnswer(std::string_view key, BeatBossAnswer& out) {
    for (std::size_t i = 0; i < kBossAnswerKeys.size(); ++i) {
        if (key == kBossAnswerKeys[i]) {
            out = static_cast<BeatBossAnswer>(i);
            return true;
        }
    }
    return false;
}

bool parseBossPos(std::string_view text, std::uint8_t beatEighths, BossPos& out) {
    std::size_t i = 0;
    const auto number = [&](int& value) {
        const std::size_t start = i;
        value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - start < 3) {
            value = value * 10 + (text[i] - '0');
            ++i;
        }
        return i > start;
    };
    int bar = 0;
    int beat = 0;
    if (!number(bar) || i >= text.size() || text[i] != '.') return false;
    ++i;
    if (!number(beat)) return false;
    int sub = 0;
    if (i < text.size() && text[i] == '&') {
        sub = 1;
        ++i;
    } else if (i < text.size() && text[i] == 'a' && beatEighths == 3) {
        sub = 2;
        ++i;
    }
    if (i != text.size() || bar < 1 || bar > 255 || beat < 1) return false;
    const int eighth = (beat - 1) * beatEighths + sub;
    if (eighth > 255) return false;
    out.bar = static_cast<std::uint8_t>(bar);
    out.eighth = static_cast<std::uint8_t>(eighth);
    return true;
}

// ---- the fight ----

void BeatBoss::begin(const BeatBossDef& def, const BeatBossTuning& tuning, double now, const QteAssist& assist) {
    def_ = def;
    tuning_ = tuning;
    assist_ = assist;
    stats_ = BeatBossStats{};
    droppedEvents_ = 0;
    nextId_ = 1;
    now_ = now;
    track_ = 0.0;
    checkpoint_ = Checkpoint{0, def_.bossHp, 0, 0};
    if (def_.phaseCount == 0) {
        state_ = BeatBossState::NotStarted;
        return;
    }
    reset(now, 0);
    const BarMap& m = def_.phases[0];
    startCount(def_.leadInBars, m.beatSeconds(), bossBeats(m.meter, m.beatEighths));
}

void BeatBoss::retry(double now) {
    if (state_ == BeatBossState::NotStarted) return;
    ++stats_.retries;
    reset(now, checkpoint_.phase);
    const BarMap& m = def_.phases[static_cast<std::size_t>(phase_)];
    startCount(1, m.beatSeconds(), bossBeats(m.meter, m.beatEighths));
}

void BeatBoss::reset(double now, int phase) {
    if (now > now_) now_ = now;
    state_ = BeatBossState::Playing;
    phase_ = phase;
    loop_ = 1;
    barIndex_ = 0;
    finisherBar_ = -1;
    finisherBroken_ = false;
    finisherLoop_ = 0;
    finisherStrikes_ = 0;
    phaseEnd_ = false;
    skipCounters_ = false;
    started_ = false;
    barStart_ = track_;
    barSeconds_ = 0.0;
    hp_ = def_.leadHp;
    bossHp_ = checkpoint_.bossHp;
    load_.value = checkpoint_.load;
    silencesPlayed_ = checkpoint_.silencesPlayed;
    floor_ = bossRound(static_cast<double>(def_.phases[static_cast<std::size_t>(phase)].endAt) * def_.bossHp);
    streak_ = 0;
    openMask_ = 0;
    lockoutUntil_ = -1e30;
    promptCount_ = 0;
}

void BeatBoss::startCount(int bars, double beatSeconds, int beatsPerBar) {
    silentUntil_ = now_;
    countBeat_ = beatSeconds;
    countBeatsPerBar_ = beatsPerBar < 1 ? 1 : beatsPerBar;
    countBeats_ = bars * countBeatsPerBar_;
    countDone_ = 0;
    pauseEnd_ = now_ + countBeat_ * countBeats_;
    mode_ = countBeats_ > 0 ? Mode::Count : Mode::Running;
}

BeatBossEvents BeatBoss::update(double now) {
    BeatBossEvents out;
    runTo(now, out);
    return out;
}

BeatBossEvents BeatBoss::press(ActionButton input, double t) {
    BeatBossEvents out;
    if (!assist_.autoComplete) doPress(input, t, out);
    else runTo(t, out);
    return out;
}

BeatBossEvents BeatBoss::release(ActionButton input, double t) {
    BeatBossEvents out;
    if (!assist_.autoComplete) doRelease(input, t, out);
    else runTo(t, out);
    return out;
}

int BeatBoss::multiplier() const {
    const int step = def_.streakStep > 0 ? def_.streakStep : 1;
    return std::min(def_.maxMultiplier, 1 + streak_ / step);
}

int BeatBoss::openRests() const { return bossPopcount(openMask_); }

double BeatBoss::approach(const BeatBossPrompt& p) const {
    if (mode_ != Mode::Running || track_ < p.tell) return -1.0;
    if (track_ >= p.impact) return 0.0;
    const double span = p.impact - p.tell;
    return span > 0.0 ? (p.impact - track_) / span : 0.0;
}

// While the track is frozen it runs again at the pause's end.
double BeatBoss::toWall(double trackTime) const {
    return (mode_ == Mode::Running ? now_ : pauseEnd_) + (trackTime - track_);
}

void BeatBoss::push(BeatBossEvents& out, BeatBossEventType type, int amount, const BeatBossPrompt* p, QteGrade grade) {
    if (out.count >= out.items.size()) {
        ++droppedEvents_;
        return;
    }
    BeatBossEvent& e = out.items[out.count++];
    e = BeatBossEvent{};
    e.type = type;
    e.amount = amount;
    e.grade = grade;
    e.time = now_;
    if (p) {
        e.prompt = p->id;
        e.answer = p->answer;
    }
}

// ---- the clock ----

void BeatBoss::runTo(double t, BeatBossEvents& out) {
    if (!(t > now_)) t = now_;
    if (!assist_.autoComplete) {
        advance(t, out);
        return;
    }
    // Auto-complete: the autopilot's moves in time order, never past a bar line (which can make
    // prompts the autopilot hasn't seen) or the end of a pause.
    for (int step = 0; step < kBossMaxSteps && state_ == BeatBossState::Playing; ++step) {
        const double stop = std::min(t, horizon());
        const QteAction a = autopilot(stop);
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

double BeatBoss::horizon() const {
    if (mode_ != Mode::Running) return pauseEnd_;
    return toWall(barStart_ + barSeconds_);
}

void BeatBoss::advance(double t, BeatBossEvents& out) {
    if (state_ != BeatBossState::Playing) return;
    if (!(t > now_)) t = now_;
    for (int step = 0; step < kBossMaxSteps && state_ == BeatBossState::Playing; ++step) {
        if (mode_ != Mode::Running) {
            // A pause: the silence, then the count's beats, then the track runs again.
            const bool beat = countDone_ < countBeats_;
            const double when = beat ? silentUntil_ + countBeat_ * countDone_ : pauseEnd_;
            if (when > t) {
                if (mode_ == Mode::Silent && t >= silentUntil_) mode_ = Mode::Count;
                now_ = t;
                return;
            }
            if (when > now_) now_ = when;
            mode_ = Mode::Count;
            if (beat) {
                ++countDone_;
                push(out, BeatBossEventType::Count, (countDone_ - 1) % countBeatsPerBar_ + 1);
            } else {
                mode_ = Mode::Running;
                push(out, BeatBossEventType::Resumed, started_ ? bar() : 1);
            }
            continue;
        }
        const double target = track_ + (t - now_);
        const double line = barStart_ + barSeconds_;
        std::size_t which = kBossNone;
        double due = kBossNever;
        for (std::size_t i = 0; i < promptCount_; ++i) {
            const double d = dueTime(prompts_[i]);
            if (d < due && isDue(prompts_[i], target)) {
                due = d;
                which = i;
            }
        }
        if (which != kBossNone && due <= line) {
            if (due > track_) {
                now_ += due - track_;
                track_ = due;
            }
            expire(which, out);
            continue;
        }
        // Within the slack the line counts as reached: a stop at horizon(), the line put in wall time,
        // can come back to track time a bit short of it, and the bar would then never turn.
        if (line <= target + kBossSlack) {
            if (line > track_) {
                now_ = std::min(now_ + (line - track_), t);
                track_ = line;
            }
            barLine(out);
            continue;
        }
        track_ = target;
        now_ = t;
        return;
    }
}

// ---- bars ----

BeatBoss::BarRef BeatBoss::nextBar() const {
    BarRef r;
    r.phase = static_cast<std::int8_t>(phase_);
    if (!started_) {
        r.newPhase = true;
        return r;
    }
    const BarMap& m = def_.phases[static_cast<std::size_t>(phase_)];
    const bool last = phase_ + 1 >= def_.phaseCount;
    const bool loopEnd = finisherBar_ < 0 && barIndex_ + 1 >= m.loopBars;
    if (!last && (phaseEnd_ || (m.loops > 0 && loopEnd && loop_ >= m.loops))) {
        r.phase = static_cast<std::int8_t>(phase_ + 1);
        r.newPhase = true;
        return r;
    }
    if (finisherBar_ >= 0 && !finisherBroken_ && finisherBar_ + 1 < m.finisherBars) {
        r.bar = static_cast<std::int8_t>(finisherBar_ + 1);
        r.finisher = true;
        r.loop = loop_;
        return r;
    }
    int nb = barIndex_ + 1;
    int nl = loop_;
    if (nb >= m.loopBars) {
        nb = 0;
        ++nl;
    }
    const double band = static_cast<double>(m.finisherAt) * def_.bossHp;
    if (finisherBar_ < 0 && m.finisherBars > 0 && m.finisherCount > 0 && bossHp_ > floor_ &&
        static_cast<double>(bossHp_ - floor_) <= band + kBossSlack && finisherLoop_ != nl) {
        r.finisher = true;
        r.loop = nl;
        return r;
    }
    r.bar = static_cast<std::int8_t>(nb);
    r.loop = nl;
    return r;
}

BossBar BeatBoss::barShape(const BarRef& r) const {
    const BarMap& m = def_.phases[static_cast<std::size_t>(r.phase)];
    return r.finisher ? m.meter : m.bars[static_cast<std::size_t>(r.bar)];
}

double BeatBoss::barSecondsOf(const BarRef& r) const {
    const BarMap& m = def_.phases[static_cast<std::size_t>(r.phase)];
    return static_cast<double>(barShape(r).eighths) * m.eighthSeconds();
}

int BeatBoss::silenceDue(const BarRef& r) const {
    const BarMap& m = def_.phases[static_cast<std::size_t>(r.phase)];
    for (std::size_t i = 0; i < m.silenceCount; ++i) {
        const std::uint32_t bit = 1u << (static_cast<std::uint32_t>(r.phase) * kMaxBossSilences + i);
        if (silencesPlayed_ & bit) continue;
        const BossSilence& s = m.silences[i];
        if (s.hp > 0.0f) {
            if (static_cast<double>(bossHp_) <= static_cast<double>(s.hp) * def_.bossHp + kBossSlack)
                return static_cast<int>(i);
        } else if (!r.finisher && r.bar + 1 == s.bar && r.loop == s.loop) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void BeatBoss::barLine(BeatBossEvents& out) {
    const BarRef r = nextBar();
    const BarMap& m = def_.phases[static_cast<std::size_t>(r.phase)];
    if (r.newPhase) {
        promptCount_ = 0; // what was open is dropped
        phase_ = r.phase;
        loop_ = 1;
        barIndex_ = 0;
        finisherBar_ = -1;
        finisherBroken_ = false;
        finisherLoop_ = 0;
        phaseEnd_ = false;
        skipCounters_ = false;
        openMask_ = 0;
        floor_ = bossRound(static_cast<double>(m.endAt) * def_.bossHp);
        checkpoint_ = Checkpoint{phase_, bossHp_, load_.value, silencesPlayed_};
        push(out, BeatBossEventType::PhaseStart, phase_);
    } else if (r.finisher) {
        if (finisherBar_ < 0) {
            finisherLoop_ = r.loop;
            finisherBroken_ = false;
            finisherStrikes_ = m.finisherCount;
            push(out, BeatBossEventType::FinisherStart);
        }
        finisherBar_ = r.bar;
    } else {
        finisherBar_ = -1;
        barIndex_ = r.bar;
        loop_ = r.loop;
    }
    started_ = true;
    barStart_ = track_;
    barSeconds_ = barSecondsOf(r);
    ++stats_.bars;
    push(out, BeatBossEventType::BarStart, r.finisher ? 0 : r.bar + 1);
    generate(out);

    const int s = silenceDue(r);
    if (s >= 0) {
        const BossSilence& sil = m.silences[static_cast<std::size_t>(s)];
        silencesPlayed_ |= 1u << (static_cast<std::uint32_t>(phase_) * kMaxBossSilences + static_cast<std::uint32_t>(s));
        ++stats_.silences;
        startCount(1, m.beatSeconds(), bossBeats(m.meter, m.beatEighths));
        silentUntil_ = now_ + sil.seconds;
        pauseEnd_ = silentUntil_ + countBeat_ * countBeats_;
        mode_ = Mode::Silent;
        push(out, BeatBossEventType::SilenceStart, bossRound(sil.seconds * 1000.0));
    }
}

void BeatBoss::generate(BeatBossEvents& out) {
    (void)out;
    const BarMap& m = def_.phases[static_cast<std::size_t>(phase_)];
    const bool skip = skipCounters_;
    skipCounters_ = false;
    if (finisherBar_ >= 0) {
        if (finisherBroken_) return;
        const double base = barStart_ - m.finisherBarSeconds() * finisherBar_;
        for (std::size_t k = 0; k < m.finisherCount; ++k)
            if (m.finisher[k].tell.bar == finisherBar_ + 1) addPrompt(m.finisher[k], static_cast<std::uint8_t>(k), true, base);
        return;
    }
    const double base = barStart_ - m.barOffset(static_cast<std::size_t>(barIndex_));
    for (std::size_t k = 0; k < m.moveCount; ++k) {
        const PinnedMove& mv = m.moves[k];
        if (mv.tell.bar != barIndex_ + 1) continue;
        if (skip && isBossCounter(mv.answer)) continue;
        if (mv.answer == BeatBossAnswer::Release && load_.value <= 0) continue;
        addPrompt(mv, static_cast<std::uint8_t>(k), false, base);
    }
}

void BeatBoss::addPrompt(const PinnedMove& m, std::uint8_t index, bool finisher, double base) {
    if (promptCount_ >= prompts_.size()) return;
    BeatBossPrompt p;
    p.id = nextId_++;
    p.move = index;
    p.finisher = finisher;
    p.answer = m.answer;
    p.button = m.button;
    p.second = m.second;
    p.stem = m.stem;
    p.missCost = m.missCost;
    p.opens = m.opens;
    p.tell = base + m.tellAt;
    p.impact = base + m.impactAt;
    p.end = base + m.endAt;
    std::size_t at = promptCount_;
    while (at > 0 && prompts_[at - 1].impact > p.impact) {
        prompts_[at] = prompts_[at - 1];
        --at;
    }
    prompts_[at] = p;
    ++promptCount_;
}

void BeatBoss::removePrompt(std::size_t i) {
    for (std::size_t k = i + 1; k < promptCount_; ++k) prompts_[k - 1] = prompts_[k];
    --promptCount_;
}

void BeatBoss::dropCounters() {
    std::size_t w = 0;
    for (std::size_t i = 0; i < promptCount_; ++i)
        if (!isBossCounter(prompts_[i].answer)) prompts_[w++] = prompts_[i];
    promptCount_ = w;
}

void BeatBoss::dropFinisher() {
    std::size_t w = 0;
    for (std::size_t i = 0; i < promptCount_; ++i)
        if (!prompts_[i].finisher) prompts_[w++] = prompts_[i];
    promptCount_ = w;
}

int BeatBoss::finisherLeft() const { return finisherStrikes_ > 0 ? finisherStrikes_ : 1; }

// ---- judging ----

double BeatBoss::dueTime(const BeatBossPrompt& p) const {
    const double half = tuning_.perfect * 0.5;
    switch (p.answer) {
    case BeatBossAnswer::Attack: return p.impact + tuning_.strikeGood;
    case BeatBossAnswer::Hold:
    case BeatBossAnswer::Release: return p.pressA >= 0.0 ? p.end : p.impact + tuning_.strikeGood;
    case BeatBossAnswer::Guard: return p.pressA >= 0.0 ? p.impact : p.impact + half;
    case BeatBossAnswer::Parry:
    case BeatBossAnswer::Dodge:
    case BeatBossAnswer::Jump:
    case BeatBossAnswer::Paired: return p.impact + half;
    }
    return p.impact;
}

bool BeatBoss::isDue(const BeatBossPrompt& p, double t) const {
    // A press at the last moment of a strike's window still counts; a counter has landed at its due
    // time, and a held hold is done at its end.
    const bool strikeLike = p.answer == BeatBossAnswer::Attack || (isBossHold(p.answer) && p.pressA < 0.0);
    return strikeLike ? dueTime(p) < t : dueTime(p) <= t;
}

void BeatBoss::expire(std::size_t i, BeatBossEvents& out) {
    const BeatBossPrompt& p = prompts_[i];
    if (p.answer == BeatBossAnswer::Release && p.pressA < 0.0) {
        removePrompt(i); // an offer not taken: the Load stays
        return;
    }
    const bool answered = (isBossHold(p.answer) || p.answer == BeatBossAnswer::Guard) && p.pressA >= 0.0;
    resolve(i, answered ? p.grade : QteGrade::Miss, out);
}

void BeatBoss::resolve(std::size_t i, QteGrade grade, BeatBossEvents& out) {
    const BeatBossPrompt p = prompts_[i];
    removePrompt(i);
    push(out, BeatBossEventType::Judged, 0, &p, grade);
    if (grade == QteGrade::Miss) {
        ++stats_.misses;
        if (isBossCounter(p.answer)) hitLead(p, out);
        if (state_ != BeatBossState::Playing) return;
        if (p.finisher) {
            finisherBroken_ = true;
            dropFinisher();
            push(out, BeatBossEventType::FinisherBroken);
        }
        breakStreak(out);
        return;
    }
    if (grade == QteGrade::Perfect) ++stats_.perfects;
    else ++stats_.goods;
    const BarMap& m = def_.phases[static_cast<std::size_t>(phase_)];
    const bool perfect = grade == QteGrade::Perfect;
    const int mult = multiplier();
    int damage = 0;
    switch (p.answer) {
    case BeatBossAnswer::Attack:
        if (p.finisher) {
            const int left = finisherLeft();
            damage = (bossHp_ - floor_ + left - 1) / left;
            if (--finisherStrikes_ <= 0) ++stats_.finishers;
        } else {
            damage = (perfect ? def_.strikeDamage : bossRound(def_.strikeDamage * tuning_.goodFactor)) * mult;
        }
        break;
    case BeatBossAnswer::Hold: {
        const double base = def_.holdDamage * (p.end - p.impact) / m.beatSeconds();
        damage = bossRound(perfect ? base : base * tuning_.goodFactor) * mult;
        break;
    }
    case BeatBossAnswer::Release: {
        const int spent = load_.release();
        ++stats_.releases;
        push(out, BeatBossEventType::Released, spent, &p);
        const double base = static_cast<double>(spent) * def_.releaseDamage;
        damage = bossRound(perfect ? base : base * tuning_.goodFactor);
        break;
    }
    case BeatBossAnswer::Parry:
    case BeatBossAnswer::Paired:
        if (perfect) damage = def_.counterDamage * mult;
        break;
    case BeatBossAnswer::Guard:
        if (perfect) {
            const int stored = load_.store(bossRound(static_cast<double>(p.missCost) * kMaxLoad));
            if (stored > 0) {
                stats_.loadStored += stored;
                push(out, BeatBossEventType::LoadStored, stored, &p);
            }
        }
        break;
    case BeatBossAnswer::Dodge:
    case BeatBossAnswer::Jump: break;
    }
    if (p.opens) openRest(out);
    addStreak(out);
    if (damage > 0) hitBoss(damage, out);
}

void BeatBoss::hitBoss(int amount, BeatBossEvents& out) {
    const int dealt = std::min(amount, bossHp_ - floor_);
    if (dealt <= 0) return;
    bossHp_ -= dealt;
    stats_.damageDealt += dealt;
    push(out, BeatBossEventType::BossHit, dealt);
    if (bossHp_ > floor_) return;
    if (floor_ <= 0) {
        state_ = BeatBossState::Victory;
        push(out, BeatBossEventType::Victory);
    } else {
        phaseEnd_ = true;
    }
}

void BeatBoss::hitLead(const BeatBossPrompt& p, BeatBossEvents& out) {
    // Rounded down, so one miss never costs more than its share.
    int damage = static_cast<int>(std::floor(static_cast<double>(p.missCost) * def_.leadHp + 1e-6));
    if (damage <= 0) return;
    damage = std::min(damage, hp_);
    hp_ -= damage;
    stats_.damageTaken += damage;
    push(out, BeatBossEventType::LeadHit, damage, &p);
    if (hp_ <= 0) {
        state_ = BeatBossState::Defeat;
        push(out, BeatBossEventType::Defeat);
    }
}

void BeatBoss::addStreak(BeatBossEvents& out) {
    const int before = multiplier();
    ++streak_;
    stats_.maxStreak = std::max(stats_.maxStreak, streak_);
    for (std::size_t r = 0; r < def_.rewardCount; ++r) {
        const BossReward& w = def_.rewards[r];
        if (w.at != streak_) continue;
        push(out, BeatBossEventType::Reward, static_cast<int>(r));
        switch (w.effect) {
        case BossRewardEffect::None: break;
        case BossRewardEffect::Open: openRest(out); break;
        case BossRewardEffect::Heal: hp_ = std::min(def_.leadHp, hp_ + w.amount); break;
        case BossRewardEffect::Load: {
            const int stored = load_.store(w.amount);
            stats_.loadStored += stored;
            if (stored > 0) push(out, BeatBossEventType::LoadStored, stored);
            break;
        }
        }
    }
    const int after = multiplier();
    if (after != before) push(out, BeatBossEventType::Multiplier, after);
}

void BeatBoss::breakStreak(BeatBossEvents& out) {
    const int before = multiplier();
    streak_ = 0;
    if (multiplier() != before) push(out, BeatBossEventType::Multiplier, multiplier());
    const int open = openRests();
    if (open > 0) {
        openMask_ = static_cast<std::uint16_t>(openMask_ & ~(1u << openOrder_[static_cast<std::size_t>(open - 1)]));
        push(out, BeatBossEventType::RestClosed, openRests());
    }
}

void BeatBoss::openRest(BeatBossEvents& out) {
    const BarMap& m = def_.phases[static_cast<std::size_t>(phase_)];
    for (std::size_t i = 0; i < m.restCount; ++i) {
        if (openMask_ & (1u << i)) continue;
        const int open = openRests();
        openOrder_[static_cast<std::size_t>(open)] = static_cast<std::uint8_t>(i);
        openMask_ = static_cast<std::uint16_t>(openMask_ | (1u << i));
        push(out, BeatBossEventType::RestOpened, open + 1);
        if (def_.staggerOpens > 0 && open + 1 >= def_.staggerOpens) {
            openMask_ = 0;
            ++stats_.staggers;
            push(out, BeatBossEventType::Stagger);
            dropCounters();
            skipCounters_ = true;
        }
        return;
    }
}

void BeatBoss::doPress(ActionButton input, double t, BeatBossEvents& out) {
    advance(t, out);
    if (state_ != BeatBossState::Playing) return;
    judgePress(input, out);
}

void BeatBoss::judgePress(ActionButton input, BeatBossEvents& out) {
    if (mode_ != Mode::Running) {
        ++stats_.strays;
        push(out, BeatBossEventType::Stray);
        return;
    }
    const double t = track_;
    const double half = tuning_.perfect * 0.5;
    bool lockedOut = false;
    bool whiff = false;
    for (std::size_t i = 0; i < promptCount_; ++i) {
        BeatBossPrompt& p = prompts_[i];
        const double d = t - p.impact;
        if (p.answer == BeatBossAnswer::Attack || isBossHold(p.answer)) {
            if (input != bossButton(p) || p.pressA >= 0.0 || std::fabs(d) > tuning_.strikeGood + kBossSlack) continue;
            const QteGrade g = std::fabs(d) <= tuning_.strikePerfect + kBossSlack ? QteGrade::Perfect : QteGrade::Good;
            if (p.answer == BeatBossAnswer::Attack) {
                resolve(i, g, out);
            } else {
                p.pressA = t;
                p.grade = g;
            }
            return;
        }
        // A counter.
        const bool paired = p.answer == BeatBossAnswer::Paired;
        const bool matches = paired ? (input == p.button || input == p.second) : input == bossButton(p);
        if (!matches) continue;
        if (p.answer == BeatBossAnswer::Guard && p.pressA >= 0.0) continue;
        if (t < p.impact - tuning_.counterEarly - kBossSlack) {
            if (t >= p.tell) whiff = true;
            continue;
        }
        if (t < lockoutUntil_) {
            lockedOut = true;
            continue;
        }
        const QteGrade g = std::fabs(d) <= half + kBossSlack ? QteGrade::Perfect : QteGrade::Good;
        switch (p.answer) {
        case BeatBossAnswer::Guard:
            if (t >= p.impact - kBossSlack) {
                resolve(i, g, out);
            } else {
                p.pressA = t;
                p.grade = g;
            }
            return;
        case BeatBossAnswer::Paired: {
            if (input == p.button) p.pressA = t;
            else p.pressB = t;
            if (p.pressA < 0.0 || p.pressB < 0.0) return;
            if (std::fabs(p.pressA - p.pressB) <= tuning_.pairWidth + kBossSlack) {
                resolve(i, g, out); // graded by this press, the later of the two
            } else if (input == p.button) {
                p.pressB = -1.0; // too far apart: the older press no longer counts
            } else {
                p.pressA = -1.0;
            }
            return;
        }
        default: resolve(i, g, out); return;
        }
    }
    if (lockedOut) {
        ++stats_.ignored;
        push(out, BeatBossEventType::LockedOut);
        return;
    }
    if (whiff) {
        const BarMap& m = def_.phases[static_cast<std::size_t>(phase_)];
        const double lockout = std::min(tuning_.counterLockout, tuning_.lockoutBeats * m.beatSeconds());
        lockoutUntil_ = t + lockout;
        ++stats_.whiffs;
        push(out, BeatBossEventType::Whiff, bossRound(lockout * 1000.0));
        return;
    }
    ++stats_.strays;
    push(out, BeatBossEventType::Stray);
}

void BeatBoss::doRelease(ActionButton input, double t, BeatBossEvents& out) {
    advance(t, out);
    if (state_ != BeatBossState::Playing || mode_ != Mode::Running || assist_.holdToggle) return;
    for (std::size_t i = 0; i < promptCount_; ++i) {
        BeatBossPrompt& p = prompts_[i];
        if (p.answer == BeatBossAnswer::Guard && input == ActionButton::Guard && p.pressA >= 0.0) {
            p.pressA = -1.0; // let go before the impact: it can be pressed again
            p.grade = QteGrade::Pending;
        }
    }
    for (std::size_t i = 0; i < promptCount_; ++i) {
        const BeatBossPrompt& p = prompts_[i];
        if (!isBossHold(p.answer) || p.pressA < 0.0 || p.button != input) continue;
        resolve(i, track_ >= p.end - tuning_.holdSlack - kBossSlack ? p.grade : QteGrade::Miss, out);
        return;
    }
}

// ---- the autopilot ----

QteAction BeatBoss::autopilot(double now) const {
    // Never past a bar line that drops the open prompts (a new phase) or stops the track (a
    // silence): a time beyond it would be wrong.
    double limit = now;
    if (state_ == BeatBossState::Playing && mode_ == Mode::Running && started_) {
        const BarRef r = nextBar();
        if (r.newPhase || silenceDue(r) >= 0) limit = std::min(limit, toWall(barStart_ + barSeconds_) - 1e-6);
    }
    return pilot(limit);
}

QteAction BeatBoss::pilot(double limit) const {
    QteAction best;
    if (state_ != BeatBossState::Playing) return best;
    double bestTime = kBossNever;
    const auto offer = [&](QteAction::Kind kind, ActionButton input, double trackTime) {
        const double wall = std::max(toWall(trackTime), now_);
        if (wall > limit + kBossSlack) return;
        // Earliest first; at the same moment a release before a press.
        const bool better = wall < bestTime - kBossSlack ||
                            (wall <= bestTime + kBossSlack && kind == QteAction::Kind::Release &&
                             best.kind == QteAction::Kind::Press);
        if (!better) return;
        bestTime = wall;
        best.kind = kind;
        best.input = input;
        best.time = wall;
    };
    for (std::size_t i = 0; i < promptCount_; ++i) {
        const BeatBossPrompt& p = prompts_[i];
        switch (p.answer) {
        case BeatBossAnswer::Hold:
        case BeatBossAnswer::Release:
            if (p.pressA < 0.0) offer(QteAction::Kind::Press, p.button, p.impact);
            else if (!assist_.holdToggle) offer(QteAction::Kind::Release, p.button, p.end);
            break;
        case BeatBossAnswer::Guard:
            if (p.pressA < 0.0) offer(QteAction::Kind::Press, ActionButton::Guard, p.impact - tuning_.perfect * 0.25);
            break;
        case BeatBossAnswer::Paired:
            if (p.pressA < 0.0) offer(QteAction::Kind::Press, p.button, p.impact);
            else if (p.pressB < 0.0) offer(QteAction::Kind::Press, p.second, p.impact);
            break;
        default: offer(QteAction::Kind::Press, bossButton(p), p.impact); break;
        }
    }
    return best;
}

// ---- the beat line ----

void BeatBoss::beatLine(BeatLine& out) const {
    out.tickCount = 0;
    out.restCount = 0;
    out.markerCount = 0;
    out.bars = {};
    out.seconds = 0.0;
    out.playhead = 0.0;
    out.playheadX = 0.0f;
    out.frozen = false;
    out.counting = false;
    if (state_ == BeatBossState::NotStarted) return;

    BarRef refs[2];
    bool isCount[2] = {false, false};
    BarRef here;
    here.phase = static_cast<std::int8_t>(phase_);
    here.finisher = finisherBar_ >= 0;
    here.bar = static_cast<std::int8_t>(here.finisher ? finisherBar_ : barIndex_);
    here.loop = loop_;
    // Open prompts sit on the line from this track time (the current bar's downbeat, or the bar a
    // pause gives back to); -1 when the line holds none of them.
    double promptsFrom = 0.0;
    double promptsAt = -1.0;
    if (mode_ != Mode::Running) {
        out.counting = true;
        out.frozen = mode_ == Mode::Silent;
        const int countBar = std::min(countDone_, std::max(countBeats_ - 1, 0)) / countBeatsPerBar_;
        const int countBars = (countBeats_ + countBeatsPerBar_ - 1) / countBeatsPerBar_;
        isCount[0] = true;
        refs[0] = here;
        if (countBar + 1 < countBars) {
            isCount[1] = true;
            refs[1] = here;
        } else {
            refs[1] = started_ ? here : nextBar();
            if (started_) {
                promptsFrom = barStart_;
                promptsAt = countBeat_ * countBeatsPerBar_;
            }
        }
        const double countStart = silentUntil_ + countBeat_ * countBeatsPerBar_ * countBar;
        out.playhead = out.frozen ? 0.0 : std::max(0.0, now_ - countStart);
    } else {
        refs[0] = here;
        refs[1] = nextBar();
        promptsFrom = barStart_;
        promptsAt = 0.0;
        out.playhead = track_ - barStart_;
    }

    double start = 0.0;
    for (int k = 0; k < 2; ++k) {
        const BarRef& r = refs[k];
        const BarMap& m = def_.phases[static_cast<std::size_t>(r.phase)];
        BeatLineBar& b = out.bars[static_cast<std::size_t>(k)];
        if (isCount[k]) {
            b.count = true;
            b.beats = static_cast<std::uint8_t>(countBeatsPerBar_);
            b.eighths = static_cast<std::uint8_t>(countBeatsPerBar_ * m.beatEighths);
            b.seconds = countBeat_ * countBeatsPerBar_;
        } else {
            const BossBar played = barShape(r);
            b.finisher = r.finisher;
            b.number = static_cast<std::uint8_t>(r.finisher ? 0 : r.bar + 1);
            b.eighths = played.eighths;
            b.beats = static_cast<std::uint8_t>(bossBeats(played, m.beatEighths));
            b.seconds = barSecondsOf(r);
            b.shortBar = played.eighths < m.meter.eighths;
            b.longBar = played.eighths > m.meter.eighths;
        }
        const double eighth = isCount[k] ? countBeat_ / m.beatEighths : m.eighthSeconds();
        const BossBar shape = isCount[k] ? BossBar{} : barShape(r);
        for (int beat = 0; beat < b.beats && out.tickCount < out.ticks.size(); ++beat) {
            BeatLineTick& tk = out.ticks[out.tickCount++];
            tk = BeatLineTick{};
            tk.at = start + eighth * beat * m.beatEighths;
            tk.bar = static_cast<std::uint8_t>(k);
            tk.beat = static_cast<std::uint8_t>(beat + 1);
            tk.kind = beat == 0 ? BeatTickKind::BarLine
                      : (!isCount[k] && (shape.emptyBeats & (1u << beat))) ? BeatTickKind::Empty
                                                                          : BeatTickKind::Beat;
            if (isCount[k]) tk.numeral = static_cast<std::uint8_t>(beat + 1);
        }
        if (!isCount[k] && !r.finisher) {
            const bool samePhase = r.phase == phase_ && !r.newPhase;
            for (std::size_t i = 0; i < m.restCount && out.restCount < out.rests.size(); ++i) {
                const BossRest& rest = m.rests[i];
                if (rest.at.bar != r.bar + 1) continue;
                BeatLineRest& lr = out.rests[out.restCount++];
                lr = BeatLineRest{};
                lr.at = start + eighth * rest.at.eighth;
                lr.open = samePhase && (openMask_ & (1u << i)) != 0;
            }
        }
        // The next bar's moves, not told yet.
        const bool told = k == 0 || (promptsAt >= 0.0 && !isCount[k] && r.bar == here.bar && r.finisher == here.finisher &&
                                     !r.newPhase && mode_ != Mode::Running);
        if (k == 1 && !isCount[k] && !told) {
            const bool fin = r.finisher;
            const std::size_t n = fin ? m.finisherCount : m.moveCount;
            const double barOff = fin ? m.finisherBarSeconds() * r.bar : m.barOffset(static_cast<std::size_t>(r.bar));
            for (std::size_t i = 0; i < n && out.markerCount < out.markers.size(); ++i) {
                const PinnedMove& mv = fin ? m.finisher[i] : m.moves[i];
                if (mv.tell.bar != r.bar + 1 || mv.impact.bar != r.bar + 1) continue;
                if (!fin && skipCounters_ && isBossCounter(mv.answer)) continue;
                if (mv.answer == BeatBossAnswer::Release && load_.value <= 0) continue;
                BeatLineMarker& mk = out.markers[out.markerCount++];
            mk = BeatLineMarker{};
                mk.at = start + (mv.impactAt - barOff);
                mk.endX = static_cast<float>(start + (mv.endAt - barOff)); // scaled below
                mk.answer = mv.answer;
                mk.stem = mv.stem;
            }
        }
        start += b.seconds;
    }
    out.seconds = start;

    if (promptsAt >= 0.0) {
        for (std::size_t i = 0; i < promptCount_ && out.markerCount < out.markers.size(); ++i) {
            const BeatBossPrompt& p = prompts_[i];
            const double at = promptsAt + (p.impact - promptsFrom);
            if (at < 0.0 || at >= out.seconds) continue;
            BeatLineMarker& mk = out.markers[out.markerCount++];
            mk = BeatLineMarker{};
            mk.at = at;
            mk.endX = static_cast<float>(promptsAt + (p.end - promptsFrom));
            mk.answer = p.answer;
            mk.stem = p.stem;
            mk.prompt = p.id;
        }
    }

    const double total = out.seconds > 0.0 ? out.seconds : 1.0;
    if (out.tickCount < out.ticks.size()) {
        BeatLineTick& end = out.ticks[out.tickCount++];
        end = BeatLineTick{};
        end.at = out.seconds;
        end.kind = BeatTickKind::End;
        end.bar = 1;
    }
    for (std::size_t i = 0; i < out.tickCount; ++i) out.ticks[i].x = static_cast<float>(out.ticks[i].at / total);
    for (std::size_t i = 0; i < out.restCount; ++i) out.rests[i].x = static_cast<float>(out.rests[i].at / total);
    for (std::size_t i = 0; i < out.markerCount; ++i) {
        BeatLineMarker& mk = out.markers[i];
        mk.x = static_cast<float>(mk.at / total);
        mk.endX = std::max(mk.x, static_cast<float>(static_cast<double>(mk.endX) / total));
    }
    out.playheadX = static_cast<float>(std::clamp(out.playhead / total, 0.0, 1.0));
}

// ---- data ----

namespace {

constexpr std::size_t kBossMaxId = 48;
constexpr std::size_t kBossMaxMusic = 64;
constexpr int kBossMaxNumber = 99999;

std::string bossNumberText(double v) {
    char text[32];
    std::snprintf(text, sizeof text, "%g", v);
    return text;
}

bool bossIdText(std::string_view s) {
    if (s.empty() || s.size() > kBossMaxId) return false;
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

struct BossLoad {
    std::vector<std::string>& errors;
    std::vector<std::string>* warnings;

    static std::string at(const std::string& path, const char* key) {
        return path.empty() ? std::string(key) : path + "." + key;
    }
    void unknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys) const {
        if (!warnings || !j.isObject()) return;
        for (std::uint32_t k = 0; k < j.size(); ++k) {
            const std::string_view key = j.key(k);
            if (std::find(keys.begin(), keys.end(), key) == keys.end())
                warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
        }
    }
    // An optional number in [lo, hi]: false (nothing written) when absent, or when bad (an error).
    bool number(JsonValue j, const char* key, double& out, double lo, double hi, const std::string& path,
                const char* why = nullptr) const {
        const JsonValue v = j[key];
        if (v.isAbsent()) return false;
        const double d = v.number(-1e30);
        if (!v.isNumber() || !std::isfinite(d) || d < lo || d > hi) {
            std::string e = at(path, key) + ": expected a number in " + bossNumberText(lo) + ".." + bossNumberText(hi);
            if (why) e += std::string(" (") + why + ")";
            errors.push_back(e);
            return false;
        }
        out = d;
        return true;
    }
    bool integer(JsonValue j, const char* key, int& out, int lo, int hi, const std::string& path) const {
        const JsonValue v = j[key];
        if (v.isAbsent()) return false;
        if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
            errors.push_back(at(path, key) + ": expected an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
            return false;
        }
        out = static_cast<int>(v.integer());
        return true;
    }
    bool count(JsonValue j, const char* key, BeatCountKind& out, const std::string& path) const {
        const JsonValue v = j[key];
        if (v.isAbsent()) return false;
        const std::string_view s = v.string();
        if (s == "click") out = BeatCountKind::Click;
        else if (s == "voice") out = BeatCountKind::Voice;
        else if (s == "silent") out = BeatCountKind::Silent;
        else {
            errors.push_back(at(path, key) + ": expected \"click\", \"voice\" or \"silent\"");
            return false;
        }
        return true;
    }
    bool button(JsonValue v, ActionButton& out, const std::string& where) const {
        if (!v.isString() || !parseActionButton(v.string(), out)) {
            errors.push_back(where + ": expected \"attack\", \"guard\", \"dodge\" or \"jump\"");
            return false;
        }
        return true;
    }
};

// "n/4" or "n/8" into a bar; false for anything else.
bool bossMeter(std::string_view s, BossBar& out) {
    const std::size_t slash = s.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash > 2) return false;
    int n = 0;
    for (std::size_t i = 0; i < slash; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        n = n * 10 + (s[i] - '0');
    }
    const std::string_view unit = s.substr(slash + 1);
    if (unit == "4") {
        if (n < 1 || n > 8) return false;
        out.eighths = static_cast<std::uint8_t>(2 * n);
    } else if (unit == "8") {
        if (n < 1 || n > static_cast<int>(kMaxBossBarEighths)) return false;
        out.eighths = static_cast<std::uint8_t>(n);
    } else {
        return false;
    }
    out.meterCount = static_cast<std::uint8_t>(n);
    out.meterUnit = static_cast<std::uint8_t>(unit == "4" ? 4 : 8);
    out.emptyBeats = 0;
    return true;
}

std::string bossMeterText(const BossBar& b) { return std::to_string(b.meterCount) + "/" + std::to_string(b.meterUnit); }

// A list of beats (1..beats) as a mask; false (and an error) when bad.
bool bossEmpty(JsonValue v, int beats, std::uint16_t& mask, const std::string& where, std::vector<std::string>& errors) {
    if (!v.isArray()) {
        errors.push_back(where + ": expected an array of beats");
        return false;
    }
    std::uint16_t m = 0;
    for (std::uint32_t i = 0; i < v.size(); ++i) {
        if (!v[i].isInteger() || v[i].integer() < 1 || v[i].integer() > beats) {
            errors.push_back(where + "[" + std::to_string(i) + "]: expected a beat in 1.." + std::to_string(beats));
            return false;
        }
        m = static_cast<std::uint16_t>(m | (1u << (v[i].integer() - 1)));
    }
    mask = m;
    return true;
}

// Where a place falls: in the bar map (and so a time), or why not.
struct PlaceCheck {
    bool ok = false;
    double time = 0.0;
};

PlaceCheck bossPlace(const BarMap& m, bool finisher, const BossPos& pos, bool allowEnd, std::string_view text,
                     const std::string& where, std::vector<std::string>& errors) {
    PlaceCheck c;
    const int bars = finisher ? m.finisherBars : m.loopBars;
    const double e8 = m.eighthSeconds();
    if (allowEnd && pos.bar == bars + 1 && pos.eighth == 0) {
        c.ok = true;
        c.time = finisher ? static_cast<double>(bars) * m.finisherBarSeconds() : m.barOffset(static_cast<std::size_t>(bars));
        return c;
    }
    if (pos.bar < 1 || pos.bar > bars) {
        errors.push_back(where + ": \"" + std::string(text) + "\" is past the " + (finisher ? "finisher's " : "loop's ") +
                         std::to_string(bars) + " bars");
        return c;
    }
    const std::size_t b = static_cast<std::size_t>(pos.bar - 1);
    const BossBar& shape = finisher ? m.meter : m.bars[b];
    if (pos.eighth >= shape.eighths) {
        errors.push_back(where + ": \"" + std::string(text) + "\" is a lost beat (bar " + std::to_string(pos.bar) + " is " +
                         bossMeterText(shape) + ")");
        return c;
    }
    const int beat = pos.eighth / m.beatEighths;
    if (shape.emptyBeats & (1u << beat)) {
        errors.push_back(where + ": \"" + std::string(text) + "\" is on an empty beat");
        return c;
    }
    c.ok = true;
    c.time = (finisher ? static_cast<double>(b) * m.finisherBarSeconds() : m.barOffset(b)) + e8 * pos.eighth;
    return c;
}

// One move. False when it is refused (its errors are in `errors`).
bool bossMove(const BossLoad& ld, JsonValue j, const std::string& path, BarMap& m, bool finisher,
              const std::vector<std::string>& stems, PinnedMove& mv, std::string& id, std::string& name) {
    const std::size_t before = ld.errors.size();
    if (!j.isObject()) {
        ld.errors.push_back(path + ": expected an object");
        return false;
    }
    ld.unknownKeys(j, path, {"id", "name", "tell", "impact", "end", "answer", "button", "pair", "stem", "miss", "opens"});
    id = std::string(j["id"].string());
    if (!bossIdText(id)) ld.errors.push_back(path + ".id: expected 1.." + std::to_string(kBossMaxId) + " letters, digits, _ and -");
    if (j.has("name")) {
        if (!j["name"].isString() || j["name"].string().size() > kBossMaxId)
            ld.errors.push_back(path + ".name: expected a string of at most " + std::to_string(kBossMaxId) + " bytes");
        else
            name = std::string(j["name"].string());
    }
    if (!parseBeatBossAnswer(j["answer"].string(), mv.answer)) {
        ld.errors.push_back(path + ".answer: expected attack, guard, parry, dodge, jump, hold, paired or release");
        return false;
    }
    if (finisher && mv.answer != BeatBossAnswer::Attack) ld.errors.push_back(path + ".answer: a finisher holds attacks only");
    const bool counter = isBossCounter(mv.answer);
    const bool hold = isBossHold(mv.answer);

    const auto place = [&](const char* key, BossPos& pos, bool allowEnd) -> PlaceCheck {
        const std::string where = path + "." + key;
        const JsonValue v = j[key];
        if (!v.isString() || !parseBossPos(v.string(), m.beatEighths, pos)) {
            ld.errors.push_back(where + ": expected a place like \"4.2\" or \"4.2&\"" +
                                std::string(m.beatEighths == 3 ? " or \"4.2a\"" : ""));
            return PlaceCheck{};
        }
        return bossPlace(m, finisher, pos, allowEnd, v.string(), where, ld.errors);
    };
    const PlaceCheck tell = place("tell", mv.tell, false);
    const PlaceCheck impact = place("impact", mv.impact, false);
    mv.end = mv.impact;
    PlaceCheck end = impact;
    if (hold) {
        end = place("end", mv.end, true);
        if (end.ok && impact.ok && end.time <= impact.time + kBossSlack)
            ld.errors.push_back(path + ".end: expected a place after the impact");
    } else if (j.has("end")) {
        ld.errors.push_back(path + ".end: only a hold or a release has an end");
    }
    if (tell.ok && impact.ok) {
        const double lead = impact.time - tell.time;
        if (lead < kMinBossTell - 1e-6)
            ld.errors.push_back(path + ".tell: " + bossNumberText(lead) + " s before the impact, under the " +
                                bossNumberText(kMinBossTell) + " s floor");
        else if (counter && lead < m.beatSeconds() - 1e-6)
            ld.errors.push_back(path + ".tell: a counter's tell spans at least one beat (" + bossNumberText(m.beatSeconds()) +
                                " s)");
    }
    mv.tellAt = tell.time;
    mv.impactAt = impact.time;
    mv.endAt = end.time;

    if (j.has("button")) {
        if (!hold) ld.errors.push_back(path + ".button: only a hold or a release takes a button");
        else ld.button(j["button"], mv.button, path + ".button");
    }
    if (mv.answer == BeatBossAnswer::Paired) {
        const JsonValue pair = j["pair"];
        if (!pair.isArray() || pair.size() != 2) {
            ld.errors.push_back(path + ".pair: expected two buttons");
        } else if (ld.button(pair[0], mv.button, path + ".pair[0]") && ld.button(pair[1], mv.second, path + ".pair[1]") &&
                   mv.button == mv.second) {
            ld.errors.push_back(path + ".pair: expected two different buttons");
        }
    } else if (j.has("pair")) {
        ld.errors.push_back(path + ".pair: only a paired answer takes a pair");
    }

    const std::string_view stem = j["stem"].string();
    bool stemFound = false;
    for (std::size_t s = 0; s < stems.size(); ++s) {
        if (stems[s] == stem) {
            mv.stem = static_cast<std::uint8_t>(s);
            stemFound = true;
        }
    }
    if (!stemFound) ld.errors.push_back(path + ".stem: expected one of the track's stems");

    double miss = 0.0;
    if (ld.number(j, "miss", miss, 0.0, kMaxBossMissCost, path, "a share of the lead's max hp")) {
        if (!counter && miss > 0.0)
            ld.errors.push_back(path + ".miss: only a counter (guard, parry, dodge, jump, paired) costs hp");
        mv.missCost = static_cast<float>(miss);
    } else if (counter && !j.has("miss")) {
        ld.errors.push_back(path + ".miss: a counter needs its miss cost");
    }
    if (j.has("opens")) {
        if (!j["opens"].isBool()) ld.errors.push_back(path + ".opens: expected true or false");
        else if (!counter) ld.errors.push_back(path + ".opens: only a counter opens a rest");
        else mv.opens = j["opens"].boolean();
    }
    return ld.errors.size() == before;
}

// No two impacts closer than kMinBossGap (around the loop's end too when `wrap`), and none inside a
// hold.
void bossSpacing(const BossLoad& ld, const std::string& path, const char* list, const PinnedMove* moves, std::size_t n,
                 double wrap) {
    for (std::size_t a = 0; a < n; ++a) {
        for (std::size_t b = a + 1; b < n; ++b) {
            double d = std::fabs(moves[a].impactAt - moves[b].impactAt);
            if (wrap > 0.0) d = std::min(d, wrap - d);
            if (d < kMinBossGap - 1e-6)
                ld.errors.push_back(path + "." + list + "[" + std::to_string(b) + "].impact: " + bossNumberText(d) +
                                    " s from " + list + "[" + std::to_string(a) + "]'s, under " +
                                    bossNumberText(kMinBossGap) + " s");
        }
    }
    for (std::size_t h = 0; h < n; ++h) {
        if (!isBossHold(moves[h].answer)) continue;
        for (std::size_t k = 0; k < n; ++k) {
            if (k == h) continue;
            if (moves[k].impactAt > moves[h].impactAt + 1e-6 && moves[k].impactAt <= moves[h].endAt + 1e-6)
                ld.errors.push_back(path + "." + list + "[" + std::to_string(k) + "].impact: inside the hold of " + list +
                                    "[" + std::to_string(h) + "]");
        }
    }
}

void bossPhase(const BossLoad& ld, JsonValue j, const std::string& path, const std::vector<std::string>& stems,
               BarMap& m, std::string& id, std::vector<std::string>& moveIds, std::vector<std::string>& moveNames) {
    if (!j.isObject()) {
        ld.errors.push_back(path + ": expected an object");
        return;
    }
    ld.unknownKeys(j, path,
                   {"id", "bpm", "meter", "loop", "bars", "empty", "endAt", "loops", "count", "rests", "moves", "finisher",
                    "silences"});
    id = std::string(j["id"].string());
    if (!bossIdText(id)) ld.errors.push_back(path + ".id: expected 1.." + std::to_string(kBossMaxId) + " letters, digits, _ and -");

    if (!j.has("bpm")) ld.errors.push_back(path + ".bpm: expected the phase's tempo");
    ld.number(j, "bpm", m.bpm, kMinBossBpm, kMaxBossBpm, path, "a beat over 150 bpm is under 0.4 s");
    if (!bossMeter(j["meter"].string(), m.meter)) {
        ld.errors.push_back(path + ".meter: expected n/4 (1..8) or n/8 (1..16)");
        return;
    }
    // A compound meter counts in dotted quarters.
    m.beatEighths = static_cast<std::uint8_t>(m.meter.meterUnit == 8 && m.meter.meterCount % 3 == 0 && m.meter.meterCount >= 6 ? 3 : 2);
    const int phaseBeats = bossBeats(m.meter, m.beatEighths);
    if (j.has("empty")) bossEmpty(j["empty"], phaseBeats, m.meter.emptyBeats, path + ".empty", ld.errors);

    const JsonValue loop = j["loop"];
    double loopSeconds = 0.0;
    int loopBars = 0;
    if (!loop.isObject() || !ld.integer(loop, "bars", loopBars, 1, static_cast<int>(kMaxBossLoopBars), path + ".loop") ||
        !ld.number(loop, "seconds", loopSeconds, 0.1, 600.0, path + ".loop")) {
        ld.errors.push_back(path + ".loop: expected {\"bars\": 1.." + std::to_string(kMaxBossLoopBars) + ", \"seconds\": s}");
        return;
    }
    ld.unknownKeys(loop, path + ".loop", {"bars", "seconds"});
    m.loopBars = static_cast<std::uint8_t>(loopBars);
    m.loopSeconds = loopSeconds;
    for (std::size_t b = 0; b < m.loopBars; ++b) {
        m.bars[b] = m.meter;
    }

    const JsonValue bars = j["bars"];
    std::uint32_t overridden = 0;
    for (std::uint32_t i = 0; i < bars.size() && bars.isArray(); ++i) {
        const JsonValue o = bars[i];
        const std::string where = path + ".bars[" + std::to_string(i) + "]";
        ld.unknownKeys(o, where, {"bar", "meter", "empty"});
        int bar = 0;
        if (!o.isObject() || !ld.integer(o, "bar", bar, 1, loopBars, where)) {
            ld.errors.push_back(where + ".bar: expected a bar of the loop, 1.." + std::to_string(loopBars));
            continue;
        }
        if (overridden & (1u << (bar - 1))) {
            ld.errors.push_back(where + ".bar: bar " + std::to_string(bar) + " is listed twice");
            continue;
        }
        overridden |= 1u << (bar - 1);
        BossBar& shape = m.bars[static_cast<std::size_t>(bar - 1)];
        if (o.has("meter") && !bossMeter(o["meter"].string(), shape)) {
            ld.errors.push_back(where + ".meter: expected n/4 (1..8) or n/8 (1..16)");
            continue;
        }
        const int beats = bossBeats(shape, m.beatEighths);
        if (beats > static_cast<int>(kMaxBossBarBeats)) {
            ld.errors.push_back(where + ".meter: more than " + std::to_string(kMaxBossBarBeats) + " beats");
            continue;
        }
        if (o.has("empty")) {
            bossEmpty(o["empty"], beats, shape.emptyBeats, where + ".empty", ld.errors);
        } else {
            shape.emptyBeats = static_cast<std::uint16_t>(m.meter.emptyBeats & ((1u << beats) - 1u));
        }
    }
    if (!bars.isAbsent() && !bars.isArray()) ld.errors.push_back(path + ".bars: expected an array");
    m.barStart[0] = 0;
    for (std::size_t b = 0; b < m.loopBars; ++b)
        m.barStart[b + 1] = static_cast<std::uint16_t>(m.barStart[b] + m.bars[b].eighths);
    const double sum = m.barOffset(m.loopBars);
    if (std::fabs(sum - loopSeconds) > 1e-3)
        ld.errors.push_back(path + ".loop: its " + std::to_string(loopBars) + " bars sum to " + bossNumberText(sum) +
                            " s, not " + bossNumberText(loopSeconds) + " s");

    double share = 0.0;
    if (ld.number(j, "endAt", share, 0.0, 0.99, path)) m.endAt = static_cast<float>(share);
    ld.integer(j, "loops", m.loops, 0, 999, path);
    ld.count(j, "count", m.count, path);

    // Moves.
    const JsonValue moves = j["moves"];
    if (!moves.isArray() || moves.size() == 0 || moves.size() > kMaxBossMoves) {
        ld.errors.push_back(path + ".moves: expected an array of 1.." + std::to_string(kMaxBossMoves) + " moves");
    } else {
        int releases = 0;
        bool good = true;
        m.moveCount = static_cast<std::uint8_t>(moves.size());
        for (std::uint32_t i = 0; i < moves.size(); ++i) {
            std::string mid, mname;
            const std::string where = path + ".moves[" + std::to_string(i) + "]";
            good = bossMove(ld, moves[i], where, m, false, stems, m.moves[i], mid, mname) && good;
            if (m.moves[i].answer == BeatBossAnswer::Release && ++releases == 2)
                ld.errors.push_back(where + ".answer: at most one release a phase");
            moveIds.push_back(mid);
            moveNames.push_back(mname);
        }
        if (good) bossSpacing(ld, path, "moves", m.moves.data(), m.moveCount, m.loopSeconds);
    }

    // Rests.
    const JsonValue rests = j["rests"];
    if (!rests.isAbsent() && (!rests.isArray() || rests.size() > kMaxBossRests))
        ld.errors.push_back(path + ".rests: expected an array of at most " + std::to_string(kMaxBossRests) + " places");
    for (std::uint32_t i = 0; i < rests.size() && rests.isArray() && rests.size() <= kMaxBossRests; ++i) {
        const std::string where = path + ".rests[" + std::to_string(i) + "]";
        BossRest& r = m.rests[i];
        if (!rests[i].isString() || !parseBossPos(rests[i].string(), m.beatEighths, r.at)) {
            ld.errors.push_back(where + ": expected a place like \"4.2\"");
            continue;
        }
        const PlaceCheck c = bossPlace(m, false, r.at, false, rests[i].string(), where, ld.errors);
        if (!c.ok) continue;
        r.time = c.time;
        for (std::size_t k = 0; k < m.moveCount; ++k) {
            const PinnedMove& mv = m.moves[k];
            const bool onImpact = std::fabs(mv.impactAt - c.time) < 1e-6;
            const bool inHold = isBossHold(mv.answer) && c.time > mv.impactAt && c.time < mv.endAt;
            if (onImpact || inHold) {
                ld.errors.push_back(where + ": a rest on the impact or inside the hold of moves[" + std::to_string(k) + "]");
                break;
            }
        }
        m.restCount = static_cast<std::uint8_t>(i + 1);
    }

    // The finisher.
    const JsonValue fin = j["finisher"];
    if (!fin.isAbsent()) {
        const std::string where = path + ".finisher";
        ld.unknownKeys(fin, where, {"at", "bars", "moves"});
        double at = 0.0;
        int fbars = 0;
        if (!fin.isObject() || !ld.number(fin, "at", at, 0.01, 1.0, where) ||
            !ld.integer(fin, "bars", fbars, 1, static_cast<int>(kMaxBossFinisherBars), where)) {
            ld.errors.push_back(where + ": expected {\"at\": share, \"bars\": 1.." + std::to_string(kMaxBossFinisherBars) +
                                ", \"moves\": [...]}");
        } else {
            m.finisherAt = static_cast<float>(at);
            m.finisherBars = static_cast<std::uint8_t>(fbars);
            const JsonValue fm = fin["moves"];
            if (!fm.isArray() || fm.size() == 0 || fm.size() > kMaxBossFinisherMoves) {
                ld.errors.push_back(where + ".moves: expected an array of 1.." + std::to_string(kMaxBossFinisherMoves) +
                                    " attacks");
            } else {
                bool good = true;
                m.finisherCount = static_cast<std::uint8_t>(fm.size());
                for (std::uint32_t i = 0; i < fm.size(); ++i) {
                    std::string mid, mname;
                    good = bossMove(ld, fm[i], where + ".moves[" + std::to_string(i) + "]", m, true, stems, m.finisher[i],
                                    mid, mname) &&
                           good;
                    moveIds.push_back(mid);
                    moveNames.push_back(mname);
                }
                if (good) bossSpacing(ld, where, "moves", m.finisher.data(), m.finisherCount, 0.0);
            }
        }
    }

    // Silences.
    const JsonValue sil = j["silences"];
    if (!sil.isAbsent() && (!sil.isArray() || sil.size() > kMaxBossSilences))
        ld.errors.push_back(path + ".silences: expected an array of at most " + std::to_string(kMaxBossSilences));
    for (std::uint32_t i = 0; i < sil.size() && sil.isArray() && sil.size() <= kMaxBossSilences; ++i) {
        const std::string where = path + ".silences[" + std::to_string(i) + "]";
        const JsonValue s = sil[i];
        ld.unknownKeys(s, where, {"hp", "bar", "loop", "seconds", "count"});
        BossSilence& out = m.silences[i];
        double hp = 0.0;
        int bar = 0;
        int loopNo = 1;
        const bool byHp = ld.number(s, "hp", hp, 0.01, 0.99, where);
        const bool byBar = ld.integer(s, "bar", bar, 1, loopBars, where);
        ld.integer(s, "loop", loopNo, 1, 999, where);
        if (byHp == byBar) ld.errors.push_back(where + ": expected either hp (a share) or bar (and loop)");
        if (!ld.number(s, "seconds", out.seconds, 0.1, 30.0, where) && !s.has("seconds"))
            ld.errors.push_back(where + ".seconds: expected how long the track stops");
        ld.count(s, "count", out.count, where);
        out.hp = static_cast<float>(byHp ? hp : 0.0);
        out.bar = static_cast<std::uint8_t>(byBar ? bar : 0);
        out.loop = static_cast<std::uint16_t>(loopNo);
        m.silenceCount = static_cast<std::uint8_t>(i + 1);
    }
}

} // namespace

bool importBeatBossTrack(std::string_view json, BeatBossTrack& out, std::string* error, std::vector<std::string>* warnings) {
    out = BeatBossTrack{};
    std::vector<std::string> errors;
    const BossLoad ld{errors, warnings};
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
        errors.push_back("beat boss file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("beat boss file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");
    ld.unknownKeys(root, "",
                   {"version", "id", "music", "boss", "lead", "damage", "leadIn", "stems", "streak", "staggerOpens",
                    "phases"});

    BeatBossTrack track;
    BeatBossDef& def = track.def;
    track.id = std::string(root["id"].string());
    if (!bossIdText(track.id)) errors.push_back("id: expected 1.." + std::to_string(kBossMaxId) + " letters, digits, _ and -");
    if (root.has("music")) {
        const JsonValue m = root["music"];
        if (!m.isString() || m.string().size() > kBossMaxMusic)
            errors.push_back("music: expected a cue id string of at most " + std::to_string(kBossMaxMusic) + " bytes");
        else
            track.music = std::string(m.string());
    }

    const JsonValue boss = root["boss"];
    if (!boss.isObject() || !ld.integer(boss, "hp", def.bossHp, 1, kBossMaxNumber, "boss"))
        errors.push_back("boss: expected {\"hp\": 1.." + std::to_string(kBossMaxNumber) + "}");
    ld.unknownKeys(boss, "boss", {"hp"});
    const JsonValue lead = root["lead"];
    if (!lead.isObject() || !ld.integer(lead, "hp", def.leadHp, 1, kBossMaxNumber, "lead"))
        errors.push_back("lead: expected {\"hp\": 1.." + std::to_string(kBossMaxNumber) + "}");
    ld.unknownKeys(lead, "lead", {"hp"});
    if (const JsonValue d = root["damage"]; !d.isAbsent()) {
        ld.unknownKeys(d, "damage", {"strike", "counter", "hold", "release"});
        ld.integer(d, "strike", def.strikeDamage, 0, kBossMaxNumber, "damage");
        ld.integer(d, "counter", def.counterDamage, 0, kBossMaxNumber, "damage");
        ld.integer(d, "hold", def.holdDamage, 0, kBossMaxNumber, "damage");
        ld.integer(d, "release", def.releaseDamage, 0, kBossMaxNumber, "damage");
    }
    if (const JsonValue li = root["leadIn"]; !li.isAbsent()) {
        ld.unknownKeys(li, "leadIn", {"bars", "count"});
        ld.integer(li, "bars", def.leadInBars, 0, 4, "leadIn");
        ld.count(li, "count", def.leadInCount, "leadIn");
    }

    const JsonValue stems = root["stems"];
    if (!stems.isArray() || stems.size() == 0 || stems.size() > kMaxBossStems) {
        errors.push_back("stems: expected an array of 1.." + std::to_string(kMaxBossStems) + " stems");
    } else {
        def.stemCount = static_cast<std::uint8_t>(stems.size());
        for (std::uint32_t i = 0; i < stems.size(); ++i) {
            const std::string where = "stems[" + std::to_string(i) + "]";
            const JsonValue s = stems[i];
            ld.unknownKeys(s, where, {"id", "size"});
            std::string sid(s["id"].string());
            if (!bossIdText(sid)) errors.push_back(where + ".id: expected 1.." + std::to_string(kBossMaxId) + " letters, digits, _ and -");
            else if (std::find(track.stemIds.begin(), track.stemIds.end(), sid) != track.stemIds.end())
                errors.push_back(where + ".id: duplicate stem " + sid);
            double size = 1.0;
            ld.number(s, "size", size, 0.1, 2.0, where);
            def.stemSize[i] = static_cast<float>(size);
            track.stemIds.push_back(sid);
        }
    }

    if (const JsonValue st = root["streak"]; !st.isAbsent()) {
        ld.unknownKeys(st, "streak", {"step", "max", "rewards"});
        ld.integer(st, "step", def.streakStep, 1, 64, "streak");
        ld.integer(st, "max", def.maxMultiplier, 1, kMaxBossMultiplier, "streak");
        const JsonValue rw = st["rewards"];
        if (!rw.isAbsent() && (!rw.isArray() || rw.size() > kMaxBossRewards))
            errors.push_back("streak.rewards: expected an array of at most " + std::to_string(kMaxBossRewards));
        for (std::uint32_t i = 0; i < rw.size() && rw.isArray() && rw.size() <= kMaxBossRewards; ++i) {
            const std::string where = "streak.rewards[" + std::to_string(i) + "]";
            const JsonValue r = rw[i];
            ld.unknownKeys(r, where, {"at", "id", "effect", "amount"});
            BossReward& w = def.rewards[i];
            if (!ld.integer(r, "at", w.at, 1, 999, where) && !r.has("at")) errors.push_back(where + ".at: expected a streak count");
            if (i > 0 && w.at <= def.rewards[i - 1].at) errors.push_back(where + ".at: expected counts in rising order");
            std::string rid(r["id"].string());
            if (!bossIdText(rid)) errors.push_back(where + ".id: expected 1.." + std::to_string(kBossMaxId) + " letters, digits, _ and -");
            const std::string_view effect = r["effect"].string("none");
            if (effect == "none") w.effect = BossRewardEffect::None;
            else if (effect == "open") w.effect = BossRewardEffect::Open;
            else if (effect == "heal") w.effect = BossRewardEffect::Heal;
            else if (effect == "load") w.effect = BossRewardEffect::Load;
            else errors.push_back(where + ".effect: expected none, open, heal or load");
            ld.integer(r, "amount", w.amount, 0, kBossMaxNumber, where);
            track.rewardIds.push_back(rid);
            def.rewardCount = static_cast<std::uint8_t>(i + 1);
        }
    }
    ld.integer(root, "staggerOpens", def.staggerOpens, 0, static_cast<int>(kMaxBossRests), "");

    const JsonValue phases = root["phases"];
    if (!phases.isArray() || phases.size() == 0 || phases.size() > kMaxBossPhases) {
        errors.push_back("phases: expected an array of 1.." + std::to_string(kMaxBossPhases) + " phases");
    } else {
        def.phaseCount = static_cast<std::uint8_t>(phases.size());
        for (std::uint32_t i = 0; i < phases.size(); ++i) {
            const std::string path = "phases[" + std::to_string(i) + "]";
            track.phaseIds.emplace_back();
            track.moveIds.emplace_back();
            track.moveNames.emplace_back();
            BarMap& m = def.phases[i];
            bossPhase(ld, phases[i], path, track.stemIds, m, track.phaseIds.back(), track.moveIds.back(),
                      track.moveNames.back());
            const bool last = i + 1 == phases.size();
            if (last && m.endAt > 0.0f) errors.push_back(path + ".endAt: the last phase ends with the boss down (0)");
            if (last && m.loops > 0) errors.push_back(path + ".loops: the last phase ends with the boss down, not a loop count");
            if (!last && !(m.endAt > 0.0f)) errors.push_back(path + ".endAt: expected the share of hp the phase ends at");
            if (i > 0 && !last && m.endAt >= def.phases[i - 1].endAt)
                errors.push_back(path + ".endAt: expected a lower share than the phase before");
        }
    }
    if (!finish()) return false;
    out = std::move(track);
    return true;
}

} // namespace ftcore
