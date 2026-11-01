#include "ftcore/BeatChoreo.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace ftcore {

namespace {

constexpr double kEps = 1e-9;
constexpr double kPi = 3.14159265358979323846;
constexpr double kFar = 1e30;
constexpr std::size_t kMaxChoreoClipLength = 60;

constexpr std::array<const char*, kChoreoShotCount> kShotKeys{
    "wide", "over_lead", "over_boss", "close_lead", "close_boss", "low_hero", "top_down", "finisher_orbit"};
constexpr std::array<const char*, kChoreoAnchorCount> kAnchorKeys{
    "boss_head", "boss_fist_r", "boss_fist_l", "boss_chest", "lead_head", "lead_chest", "screen_center"};

constexpr std::size_t kLead = 0;
constexpr std::size_t kBoss = 1;

double wrapTime(double t, double length) {
    if (!(length > 0.0)) return 0.0;
    const double w = std::fmod(t, length);
    return w < 0.0 ? w + length : w;
}

// Answers where the lead's blow lands on the boss (the lead strikes) rather than meeting the boss's.
bool strikes(BeatBossAnswer a) {
    return a == BeatBossAnswer::Attack || a == BeatBossAnswer::Hold || a == BeatBossAnswer::Release;
}

} // namespace

// ---- names ----

const char* choreoShotKey(ChoreoShot s) { return kShotKeys[static_cast<std::size_t>(s) % kChoreoShotCount]; }

bool parseChoreoShot(std::string_view key, ChoreoShot& out) {
    for (std::size_t i = 0; i < kChoreoShotCount; ++i) {
        if (key == kShotKeys[i]) {
            out = static_cast<ChoreoShot>(i);
            return true;
        }
    }
    return false;
}

const char* choreoAnchorKey(ChoreoAnchor a) { return kAnchorKeys[static_cast<std::size_t>(a) % kChoreoAnchorCount]; }

bool parseChoreoAnchor(std::string_view key, ChoreoAnchor& out) {
    for (std::size_t i = 0; i < kChoreoAnchorCount; ++i) {
        if (key == kAnchorKeys[i]) {
            out = static_cast<ChoreoAnchor>(i);
            return true;
        }
    }
    return false;
}

int BeatClips::find(std::string_view id) const {
    for (std::size_t i = 0; i < count; ++i)
        if (items[i].id() == id) return static_cast<int>(i);
    return -1;
}

// ---- timing ----

ChoreoPlan planChoreoClip(const BeatClip& clip, double impactTime, double desiredStart, double earliest) {
    ChoreoPlan p;
    const double io = clip.impact;
    if (!(io > 0.0)) {
        p.start = std::max(desiredStart, earliest);
        return p;
    }
    // The rate that spans the windup from the wanted start to the impact, inside the clamp.
    const double span = impactTime - desiredStart;
    double rate = span > kEps ? io / span : kChoreoMaxRate;
    rate = std::clamp(rate, kChoreoMinRate, kChoreoMaxRate);
    const double start = impactTime - io / rate;
    if (start >= earliest) {
        p.start = start;
        p.rate = rate;
        return p;
    }
    // Not free that early: the windup is squeezed in, and if even the top of the clamp is too slow
    // the clip starts partway through.
    const double avail = impactTime - earliest;
    if (avail <= kEps) {
        p.start = impactTime;
        p.offset = io;
        p.rate = 1.0;
        return p;
    }
    p.start = earliest;
    const double needed = io / avail;
    if (needed <= kChoreoMaxRate) {
        p.rate = needed;
    } else {
        p.rate = kChoreoMaxRate;
        p.offset = io - avail * kChoreoMaxRate;
    }
    return p;
}

double choreoStanceRate(const BeatClip& stance, double beatSeconds) {
    if (!(beatSeconds > 0.0) || !(stance.length > 0.0)) return 1.0;
    // The whole number of beats whose rate is inside the clamp and nearest to 1.
    int best = 0;
    double bestDistance = kFar;
    for (int n = 1; n <= 64; ++n) {
        const double r = stance.length / (static_cast<double>(n) * beatSeconds);
        if (r < kChoreoMinRate - kEps || r > kChoreoMaxRate + kEps) continue;
        const double d = std::fabs(r - 1.0);
        if (d < bestDistance) {
            bestDistance = d;
            best = n;
        }
    }
    if (best == 0) best = std::max(1, static_cast<int>(std::lround(stance.length / beatSeconds)));
    return stance.length / (static_cast<double>(best) * beatSeconds);
}

// ---- the scene ----

void BeatChoreo::begin(const BeatChoreoDef& def, const BeatClips& clips, const BeatChoreoTuning& tuning) {
    def_ = def;
    clips_ = clips;
    tuning_ = tuning;
    begun_ = true;
    synced_ = false;
    unmatched_ = 0;
    hitStops_ = 0;
    retries_ = 0;
    clock_ = 0.0;
    track_ = 0.0;
    for (Actor& a : actors_) a.count = 0;
    for (PromptRec& r : recs_) r.open = false;
    barCount_ = cutCount_ = warpCount_ = 0;
    ended_ = false;
    frozen_ = true;
    running_ = false;
    frozenSince_ = -kFar;
}

void BeatChoreo::retry(const BeatBoss& fight) {
    if (!begun_) return;
    resetRun(fight, std::max(clock_, fight.now()));
}

void BeatChoreo::resetRun(const BeatBoss& fight, double clock) {
    for (Actor& a : actors_) {
        a.count = 0;
        a.anchor = fight.trackTime();
    }
    for (PromptRec& r : recs_) r.open = false;
    barCount_ = cutCount_ = warpCount_ = 0;
    pending_ = false;
    ended_ = false;
    frozen_ = true;
    running_ = false;
    frozenSince_ = clock;
    clock_ = clock;
    cursorClock_ = clock;
    track_ = cursorTrack_ = fight.trackTime();
    phase_ = fight.phase();
    newPhaseBar_ = false;
    finisherNext_ = false;
    lastLeadEnd_ = lastBossContact_ = fight.trackTime();
    staggerUntil_ = -kFar;
    retries_ = fight.stats().retries;
    synced_ = true;
    const BeatBossDef& d = fight.def();
    barBeat_ = d.phaseCount > 0 ? d.phases[static_cast<std::size_t>(std::clamp(phase_, 0, d.phaseCount - 1))].beatSeconds() : 0.5;
}

double BeatChoreo::trackNow() const { return ended_ ? endTrack_ + std::max(0.0, clock_ - endClock_) : track_; }

const BeatClip* BeatChoreo::clipOf(std::int16_t clip) const {
    return clip >= 0 && static_cast<std::size_t>(clip) < clips_.count ? &clips_.items[static_cast<std::size_t>(clip)] : nullptr;
}

BeatChoreo::PromptRec* BeatChoreo::findRec(std::uint32_t id) {
    for (PromptRec& r : recs_)
        if (r.open && r.id == id) return &r;
    return nullptr;
}

const ChoreoMove* BeatChoreo::moveOf(const PromptRec& r) const {
    if (r.phase >= def_.phaseCount) return nullptr;
    const ChoreoPhase& ph = def_.phases[r.phase];
    if (r.finisher) return r.move < ph.finisherCount ? &ph.finisher[r.move] : nullptr;
    return r.move < ph.moveCount ? &ph.moves[r.move] : nullptr;
}

// ---- boundaries (track time) ----

double BeatChoreo::boundary(double t, bool atOrAfter) const {
    const ChoreoBar* b = nullptr;
    for (std::size_t i = barCount_; i-- > 0;) {
        if (bars_[i].start <= t + kEps) {
            b = &bars_[i];
            break;
        }
    }
    if (!b) {
        if (barCount_ > 0) return bars_[0].start; // before the first bar: its bar line
        const double beat = barBeat_;
        const double k = atOrAfter ? std::ceil(t / beat - kEps) : std::floor(t / beat + kEps) + 1.0;
        return std::max(0.0, k) * beat;
    }
    const double beat = b->beat;
    double k = atOrAfter ? std::ceil((t - b->start) / beat - kEps) : std::floor((t - b->start) / beat + kEps) + 1.0;
    k = std::max(0.0, k);
    const double c = b->start + k * beat;
    if (c < b->end - kEps) return c;
    // The bar's last beat is the end: its bar line.
    if (atOrAfter ? t <= b->end + kEps : t < b->end - kEps) return b->end;
    // Past the last bar seen: the beat carries on.
    k = atOrAfter ? std::ceil((t - b->end) / beat - kEps) : std::floor((t - b->end) / beat + kEps) + 1.0;
    return b->end + std::max(0.0, k) * beat;
}

double BeatChoreo::floorBoundary(double t) const {
    for (std::size_t i = barCount_; i-- > 0;) {
        if (bars_[i].start <= t + kEps) {
            const ChoreoBar& b = bars_[i];
            const double k = std::floor((t - b.start) / b.beat + kEps);
            return b.start + std::max(0.0, k) * b.beat;
        }
    }
    return t;
}

double BeatChoreo::beatAt(double t) const {
    for (std::size_t i = barCount_; i-- > 0;)
        if (bars_[i].start <= t + kEps) return bars_[i].beat;
    return barBeat_;
}

// ---- segments, cuts, warps ----

double BeatChoreo::segEnd(const Seg& s) const {
    const BeatClip* c = clipOf(s.clip);
    return c ? s.start + (c->length - s.offset) / s.rate : s.start;
}

void BeatChoreo::addSeg(Actor& a, const Seg& s) {
    if (a.count >= a.segs.size()) {
        for (std::size_t i = 1; i < a.count; ++i) a.segs[i - 1] = a.segs[i];
        --a.count;
    }
    std::size_t at = a.count;
    while (at > 0 && a.segs[at - 1].start > s.start) {
        a.segs[at] = a.segs[at - 1];
        --at;
    }
    a.segs[at] = s;
    ++a.count;
    if (&a == &actors_[kLead]) lastLeadEnd_ = segEnd(s);
}

void BeatChoreo::removeFuture(Actor& a, double t) {
    std::size_t w = 0;
    for (std::size_t i = 0; i < a.count; ++i)
        if (a.segs[i].start <= t + kEps) a.segs[w++] = a.segs[i];
    a.count = w;
}

void BeatChoreo::removePrompt(Actor& a, std::uint32_t prompt, double t) {
    std::size_t w = 0;
    for (std::size_t i = 0; i < a.count; ++i)
        if (!(a.segs[i].prompt == prompt && a.segs[i].start > t + kEps)) a.segs[w++] = a.segs[i];
    a.count = w;
}

// The track starts again (a bar line after a silence, or a first bar): what had begun before it is let
// go, so the stance carries the actors over the stop, and what is yet to come, a move told at the bar
// line included, stays.
void BeatChoreo::settle(double t) {
    for (Actor& a : actors_) {
        std::size_t w = 0;
        for (std::size_t i = 0; i < a.count; ++i)
            if (a.segs[i].start >= t - kEps) a.segs[w++] = a.segs[i];
        a.count = w;
        a.anchor = t;
    }
    std::size_t w = 0;
    for (std::size_t i = 0; i < warpCount_; ++i)
        if (warps_[i].t0 > t + kEps) warps_[w++] = warps_[i];
    warpCount_ = w;
    pending_ = false;
}

double BeatChoreo::bossFree(double impact) const {
    double free = -kFar;
    const Actor& b = actors_[kBoss];
    for (std::size_t i = 0; i < b.count; ++i)
        if (b.segs[i].kind == SegKind::Attack && b.segs[i].impact < impact - kEps) free = std::max(free, b.segs[i].free);
    return free;
}

void BeatChoreo::addCut(double t, ChoreoShot shot) {
    // A cut at the same time is replaced.
    std::size_t w = 0;
    for (std::size_t i = 0; i < cutCount_; ++i)
        if (std::fabs(cuts_[i].time - t) > kEps) cuts_[w++] = cuts_[i];
    cutCount_ = w;
    ChoreoShot prev = def_.opening;
    for (std::size_t i = 0; i < cutCount_; ++i)
        if (cuts_[i].time < t) prev = cuts_[i].shot;
    if (prev == shot) return;
    if (cutCount_ >= cuts_.size()) {
        for (std::size_t i = 1; i < cutCount_; ++i) cuts_[i - 1] = cuts_[i];
        --cutCount_;
    }
    std::size_t at = cutCount_;
    while (at > 0 && cuts_[at - 1].time > t) {
        cuts_[at] = cuts_[at - 1];
        --at;
    }
    cuts_[at] = ChoreoCut{t, shot};
    ++cutCount_;
}

void BeatChoreo::addStop(double t, double seconds) {
    if (!(seconds > 0.0)) return;
    // It must be over, and the animation caught up, by the next beat that leaves room: never across a
    // bar line or a move's pinned impact.
    double end = boundary(t + tuning_.catchUp * seconds - kEps, true);
    for (std::size_t i = barCount_; i-- > 0;) {
        if (bars_[i].start <= t + kEps) {
            end = std::min(end, bars_[i].end);
            break;
        }
    }
    for (const PromptRec& r : recs_)
        if (r.open && r.impact > t + kEps) end = std::min(end, r.impact);
    if (end < t + 3.0 * seconds) return;
    for (std::size_t i = 0; i < warpCount_; ++i)
        if (t < warps_[i].t1 && end > warps_[i].t0) return;
    if (warpCount_ >= warps_.size()) return;
    warps_[warpCount_++] = Warp{WarpKind::Stop, t, end, seconds};
    ++hitStops_;
}

void BeatChoreo::addDip(double t0, double t1) {
    const double slow = static_cast<double>(def_.finisherSlow);
    if (!(t1 > t0 + kEps) || slow >= 1.0 || warpCount_ >= warps_.size()) return;
    // Slowest rate = slow: the lag's steepest climb is 1 - slow.
    warps_[warpCount_++] = Warp{WarpKind::Dip, t0, t1, (1.0 - slow) * (t1 - t0) / kPi};
}

// The visuals' lag behind the track at t, and how fast it grows (slope), and whether a hit-stop holds.
double BeatChoreo::lagAt(double t, double* slope, bool* stop) const {
    double lag = 0.0;
    double ds = 0.0;
    bool held = false;
    for (std::size_t i = 0; i < warpCount_; ++i) {
        const Warp& w = warps_[i];
        if (t <= w.t0 || t >= w.t1) continue;
        if (w.kind == WarpKind::Stop) {
            const double over = w.t0 + w.amount;
            if (t < over) {
                lag += t - w.t0;
                ds += 1.0;
                held = true;
            } else {
                const double span = w.t1 - over;
                lag += w.amount * (w.t1 - t) / span;
                ds -= w.amount / span;
            }
        } else {
            const double x = kPi * (t - w.t0) / (w.t1 - w.t0);
            const double s = std::sin(x);
            lag += w.amount * s * s;
            ds += w.amount * (kPi / (w.t1 - w.t0)) * std::sin(2.0 * x);
        }
    }
    if (slope) *slope = ds;
    if (stop) *stop = held;
    return lag;
}

void BeatChoreo::prune() {
    for (Actor& a : actors_) {
        while (a.count >= 2 && a.segs[1].start < track_ - 3.0) {
            for (std::size_t i = 1; i < a.count; ++i) a.segs[i - 1] = a.segs[i];
            --a.count;
        }
    }
    std::size_t w = 0;
    for (std::size_t i = 0; i < warpCount_; ++i)
        if (warps_[i].t1 > track_ - 1.0) warps_[w++] = warps_[i];
    warpCount_ = w;
}

// ---- events ----

void BeatChoreo::observe(const BeatBoss& fight, const BeatBossEvents& events, double clock) {
    if (!begun_ || fight.def().phaseCount == 0) return;
    if (!synced_ || fight.stats().retries != retries_) resetRun(fight, std::max(clock, clock_));
    clock_ = std::max(clock_, clock);
    for (const BeatBossEvent& e : events) {
        clock_ = std::max(clock_, e.time);
        if (running_ && e.time > cursorClock_) cursorTrack_ += e.time - cursorClock_;
        cursorClock_ = std::max(cursorClock_, e.time);
        handle(fight, e, cursorTrack_);
    }

    // The fight's own track clock is the truth at the end of the call.
    track_ = cursorTrack_ = fight.trackTime();
    cursorClock_ = clock_;
    running_ = fight.trackRunning();
    if (fight.over() && !ended_) {
        ended_ = true;
        endTrack_ = track_;
        endClock_ = clock_;
        frozen_ = false;
    }
    if (!ended_) {
        if (!running_ && !frozen_) {
            frozen_ = true;
            frozenSince_ = clock_;
            settle(track_);
        } else if (running_ && frozen_) {
            frozen_ = false;
            settle(track_);
        }
    }

    // Presses that are held (a guard, a hold) are noted as they come, for the clip they start.
    for (std::size_t i = 0; i < fight.promptCount(); ++i) {
        const BeatBossPrompt& p = fight.prompt(i);
        if (p.pressA < 0.0) continue;
        if (PromptRec* r = findRec(p.id)) r->press = p.pressA;
    }
    // A prompt that is gone without a verdict was dropped (a phase ended, the boss staggered, a
    // Release nobody took): its move yet to start does not play.
    for (PromptRec& r : recs_) {
        if (!r.open) continue;
        bool there = false;
        for (std::size_t i = 0; i < fight.promptCount() && !there; ++i) there = fight.prompt(i).id == r.id;
        if (there) continue;
        r.open = false;
        removePrompt(actors_[kBoss], r.id, track_);
    }
    prune();
}

void BeatChoreo::handle(const BeatBoss& fight, const BeatBossEvent& e, double te) {
    switch (e.type) {
    case BeatBossEventType::PhaseStart: {
        phase_ = std::clamp(e.amount, 0, static_cast<int>(fight.def().phaseCount) - 1);
        newPhaseBar_ = true;
        barBeat_ = fight.def().phases[static_cast<std::size_t>(phase_)].beatSeconds();
        break;
    }
    case BeatBossEventType::FinisherStart: finisherNext_ = true; break;
    case BeatBossEventType::BarStart: barStart(fight, e.amount, te); break;
    case BeatBossEventType::Judged: judged(e, te); break;
    case BeatBossEventType::BossHit: bossHit(te); break;
    case BeatBossEventType::FinisherBroken: {
        // Its slow motion is let go from the strikes not yet reached.
        std::size_t w = 0;
        for (std::size_t i = 0; i < warpCount_; ++i)
            if (!(warps_[i].kind == WarpKind::Dip && warps_[i].t0 > te + kEps)) warps_[w++] = warps_[i];
        warpCount_ = w;
        break;
    }
    case BeatBossEventType::Stagger: {
        removeFuture(actors_[kBoss], te);
        std::size_t w = 0;
        for (std::size_t i = 0; i < cutCount_; ++i)
            if (cuts_[i].time <= te + kEps) cuts_[w++] = cuts_[i];
        cutCount_ = w;
        if (def_.stagger.cut) addCut(boundary(te, false), def_.stagger.shot);
        if (const BeatClip* c = clipOf(def_.stagger.boss)) {
            Seg s;
            s.clip = def_.stagger.boss;
            s.start = te;
            addSeg(actors_[kBoss], s);
            staggerUntil_ = te + c->length;
        }
        if (clipOf(def_.stagger.lead)) {
            Seg s;
            s.clip = def_.stagger.lead;
            s.start = te;
            addSeg(actors_[kLead], s);
        }
        break;
    }
    case BeatBossEventType::SilenceStart:
        settle(te);
        frozen_ = true;
        frozenSince_ = e.time;
        running_ = false;
        break;
    case BeatBossEventType::Resumed:
        running_ = true;
        frozen_ = false;
        settle(te);
        break;
    case BeatBossEventType::Victory: forceEnd(def_.victory, te, true); break;
    case BeatBossEventType::Defeat: forceEnd(def_.defeat, te, false); break;
    default: break;
    }
}

void BeatChoreo::barStart(const BeatBoss& fight, int number, double te) {
    const BarMap& m = fight.def().phases[static_cast<std::size_t>(phase_)];
    const bool finisher = finisherNext_ || number == 0;
    finisherNext_ = false;
    const double seconds = finisher ? m.finisherBarSeconds()
                                    : m.barSeconds(static_cast<std::size_t>(std::clamp(number, 1, static_cast<int>(m.loopBars))) - 1);
    if (barCount_ >= bars_.size()) {
        for (std::size_t i = 1; i < barCount_; ++i) bars_[i - 1] = bars_[i];
        --barCount_;
    }
    bars_[barCount_++] = ChoreoBar{te, m.beatSeconds(), te + seconds};
    barBeat_ = m.beatSeconds();
    if (newPhaseBar_) {
        addCut(te, def_.opening);
        newPhaseBar_ = false;
    }

    // The moves told at this bar (their prompts are open now): the boss's clips are planned, the
    // camera is cut to each move's shot at the beat its tell falls on.
    std::array<double, kMaxBossPrompts> impacts{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < fight.promptCount(); ++i) {
        const BeatBossPrompt& p = fight.prompt(i);
        if (p.finisher != finisher || p.tell < te - 1e-6 || p.tell >= te + seconds - 1e-6) continue;
        if (findRec(p.id)) continue;
        registerPrompt(p, te);
        if (n < impacts.size()) impacts[n++] = p.impact;
        if (!finisher) {
            if (const PromptRec* r = findRec(p.id))
                if (const ChoreoMove* mv = moveOf(*r)) addCut(std::max(te, floorBoundary(p.tell)), mv->shot);
        }
    }
    if (finisher) {
        addCut(te, ChoreoShot::FinisherOrbit);
        // The slow motion dips between the strikes and after the last: lag is 0 on every strike.
        std::sort(impacts.begin(), impacts.begin() + static_cast<std::ptrdiff_t>(n));
        for (std::size_t i = 0; i < n; ++i) {
            const double next = i + 1 < n ? impacts[i + 1] : std::min(te + seconds, impacts[i] + 1.0);
            addDip(impacts[i], next);
        }
    }
}

void BeatChoreo::registerPrompt(const BeatBossPrompt& p, double te) {
    PromptRec* slot = nullptr;
    for (PromptRec& r : recs_) {
        if (!r.open) {
            slot = &r;
            break;
        }
    }
    if (!slot) return;
    PromptRec& r = *slot;
    r = PromptRec{};
    r.open = true;
    r.id = p.id;
    r.phase = static_cast<std::uint8_t>(phase_);
    r.move = p.move;
    r.finisher = p.finisher;
    r.answer = p.answer;
    r.tell = p.tell;
    r.impact = p.impact;
    const ChoreoMove* mv = moveOf(r);
    if (!mv) return;
    r.anchor = mv->anchor;
    const BeatClip* c = clipOf(mv->boss);
    if (!c) return;
    const ChoreoPlan plan = planChoreoClip(*c, p.impact, p.tell, std::max(te, bossFree(p.impact)));
    Seg s;
    s.clip = mv->boss;
    s.kind = SegKind::Attack;
    s.start = plan.start;
    s.offset = plan.offset;
    s.rate = plan.rate;
    s.impact = p.impact;
    s.free = p.impact + std::min(tuning_.followThrough, std::max(0.0, (c->length - c->impact) / plan.rate));
    s.prompt = p.id;
    // A reaction planned to start inside this windup would cut it off (the later start wins): the
    // pinned move comes first, so that reaction is dropped.
    Actor& boss = actors_[kBoss];
    std::size_t w = 0;
    for (std::size_t i = 0; i < boss.count; ++i) {
        const Seg& o = boss.segs[i];
        if (!(o.kind == SegKind::Reaction && o.start > s.start + kEps && o.start <= s.impact)) boss.segs[w++] = o;
    }
    boss.count = w;
    addSeg(boss, s);
}

void BeatChoreo::judged(const BeatBossEvent& e, double te) {
    pending_ = false;
    PromptRec* rec = findRec(e.prompt);
    if (!rec) {
        ++unmatched_;
        return;
    }
    rec->open = false;
    const ChoreoMove* mv = moveOf(*rec);
    if (!mv) return;
    Actor& lead = actors_[kLead];
    if (e.grade != QteGrade::Perfect && e.grade != QteGrade::Good) {
        // A miss: the lead's reaction plays from the impact (the verdict comes a moment after it).
        const BeatClip* c = clipOf(mv->miss);
        if (!c) return;
        const ChoreoPlan plan = planChoreoClip(*c, rec->impact, c->impact > 0.0 ? rec->impact - c->impact : rec->impact, -kFar);
        Seg s;
        s.clip = mv->miss;
        s.start = plan.start;
        s.offset = plan.offset;
        s.rate = plan.rate;
        addSeg(lead, s);
        return;
    }

    const bool held = rec->answer == BeatBossAnswer::Guard || isBossHold(rec->answer);
    const double press = held && rec->press >= 0.0 ? rec->press : te;
    const bool blow = strikes(rec->answer);
    const double contact = blow ? press + kChoreoLeadLag : std::max(press, rec->impact);
    const BeatClip* c = clipOf(mv->ok);
    if (!c) return;
    const double desired = c->impact > 0.0 ? contact - c->impact : press;
    const ChoreoPlan plan = planChoreoClip(*c, contact, desired, press);
    Seg s;
    s.clip = mv->ok;
    s.start = plan.start;
    s.offset = plan.offset;
    s.rate = plan.rate;
    addSeg(lead, s);
    double landed = c->impact > 0.0 ? plan.start + (c->impact - plan.offset) / plan.rate : plan.start;
    const bool perfect = e.grade == QteGrade::Perfect;
    if (perfect) {
        if (const BeatClip* next = clipOf(mv->then)) {
            // The follow-up is played only if its blow lands before the lead's next answer can start (a
            // press for the next move cuts it off), so the hit-stop and the boss's reaction are on a
            // blow that shows.
            double due = kFar;
            for (const PromptRec& o : recs_)
                if (o.open && o.impact > te) due = std::min(due, o.impact);
            Seg f;
            f.clip = mv->then;
            f.start = segEnd(s);
            if (f.start + next->impact <= due - 2.0 * kChoreoLeadLag) {
                addSeg(lead, f);
                landed = f.start + next->impact;
            }
        }
    }
    pending_ = true;
    pendingPrompt_ = rec->id;
    pendingContact_ = landed;
    pendingHit_ = mv->hit;
    if (perfect) {
        if (rec->answer == BeatBossAnswer::Guard) addStop(rec->impact, tuning_.hitStopLight);
        else if (rec->answer != BeatBossAnswer::Dodge && rec->answer != BeatBossAnswer::Jump) addStop(landed, tuning_.hitStopStrong);
    }
}

// The lead's blow landed: the boss reacts at the contact, unless it is staggered, or winding up
// another move (its pinned impacts are never moved), and never before its own move's impact.
void BeatChoreo::bossHit(double te) {
    if (!pending_) return;
    pending_ = false;
    const BeatClip* c = clipOf(pendingHit_);
    if (!c || te < staggerUntil_) return;
    Actor& boss = actors_[kBoss];
    double own = -kFar;
    for (std::size_t i = 0; i < boss.count; ++i)
        if (boss.segs[i].kind == SegKind::Attack && boss.segs[i].prompt == pendingPrompt_) own = boss.segs[i].impact;
    // The reaction shows after the boss's own impact frame, never with it.
    const double after = own + kChoreoReactionLag;
    const double at = std::max(pendingContact_, after + c->impact);
    const ChoreoPlan plan = planChoreoClip(*c, at, c->impact > 0.0 ? at - c->impact : at, std::max(te, after));
    for (std::size_t i = 0; i < boss.count; ++i) {
        const Seg& a = boss.segs[i];
        if (a.kind == SegKind::Attack && a.prompt != pendingPrompt_ && a.start <= plan.start + kEps && plan.start <= a.impact) return;
    }
    Seg s;
    s.clip = pendingHit_;
    s.start = plan.start;
    s.offset = plan.offset;
    s.rate = plan.rate;
    addSeg(boss, s);
    lastBossContact_ = at;
}

void BeatChoreo::forceEnd(const ChoreoBeat& beat, double te, bool victory) {
    ended_ = true;
    endTrack_ = te;
    endClock_ = cursorClock_;
    frozen_ = false;
    for (PromptRec& r : recs_) r.open = false;
    pending_ = false;
    std::size_t w = 0;
    for (std::size_t i = 0; i < warpCount_; ++i)
        if (warps_[i].t1 <= te + kEps) warps_[w++] = warps_[i];
    warpCount_ = w;
    removeFuture(actors_[kLead], te);
    removeFuture(actors_[kBoss], te);
    w = 0;
    for (std::size_t i = 0; i < cutCount_; ++i)
        if (cuts_[i].time <= te + kEps) cuts_[w++] = cuts_[i];
    cutCount_ = w;
    if (beat.cut) addCut(boundary(te, false), beat.shot);
    // The lead's last blow plays out before its victory or its fall; the boss answers it.
    const double leadAt = std::max(te, lastLeadEnd_);
    if (clipOf(beat.lead)) {
        Seg s;
        s.clip = beat.lead;
        s.start = leadAt;
        addSeg(actors_[kLead], s);
    }
    if (clipOf(beat.boss)) {
        Seg s;
        s.clip = beat.boss;
        s.start = victory ? std::max(te, lastBossContact_) : leadAt;
        addSeg(actors_[kBoss], s);
    }
}

// ---- sampling ----

void BeatChoreo::stancePose(std::int16_t stance, double clockTime, double beat, ChoreoPose& out) const {
    const BeatClip* c = clipOf(stance);
    if (!c) {
        out = ChoreoPose{};
        return;
    }
    const double rate = choreoStanceRate(*c, beat);
    out.clip = stance;
    out.rate = static_cast<float>(rate);
    out.time = wrapTime(rate * std::max(0.0, clockTime), c->length);
}

void BeatChoreo::poseOf(const Actor& a, std::int16_t stance, double v, ChoreoPose& out) const {
    const Seg* s = nullptr;
    for (std::size_t i = a.count; i-- > 0;) {
        if (a.segs[i].start <= v + kEps) {
            s = &a.segs[i];
            break;
        }
    }
    const BeatClip* c = s ? clipOf(s->clip) : nullptr;
    if (!c) {
        stancePose(stance, v - a.anchor, beatAt(a.anchor), out);
        return;
    }
    const double time = s->offset + s->rate * (v - s->start);
    out.clip = s->clip;
    out.rate = static_cast<float>(s->rate);
    if (c->loop) {
        out.time = wrapTime(time, c->length);
        return;
    }
    if (time < c->length) {
        out.time = std::max(0.0, time);
        return;
    }
    // Finished: it holds its last frame until the next beat, then the stance takes over (not at the end).
    if (!ended_) {
        const double beat = boundary(s->start + (c->length - s->offset) / s->rate, true);
        if (v >= beat - kEps) {
            stancePose(stance, v - beat, beatAt(beat), out);
            return;
        }
    }
    out.time = c->length;
    out.rate = 0.0f;
}

void BeatChoreo::sample(double trackTime, ChoreoSample& out) const {
    out = ChoreoSample{};
    if (!begun_) return;
    out.over = ended_;
    ChoreoShot shot = def_.opening;
    double cutAt = 0.0;
    for (std::size_t i = 0; i < cutCount_; ++i) {
        if (cuts_[i].time <= trackTime + kEps) {
            shot = cuts_[i].shot;
            cutAt = cuts_[i].time;
        }
    }
    out.shot = shot;
    out.sinceCut = std::max(0.0, trackTime - cutAt);

    if (frozen_ && !ended_) {
        // The track is stopped (a count, a silence): the shot holds and the actors loop their stance.
        const double idle = std::max(0.0, clock_ - frozenSince_);
        out.frozen = true;
        out.sinceCut += idle;
        stancePose(def_.stanceLead, idle, barBeat_, out.lead);
        stancePose(def_.stanceBoss, idle, barBeat_, out.boss);
        return;
    }

    double slope = 0.0;
    bool stop = false;
    const double lag = lagAt(trackTime, &slope, &stop);
    const double v = trackTime - lag;
    poseOf(actors_[kLead], def_.stanceLead, v, out.lead);
    poseOf(actors_[kBoss], def_.stanceBoss, v, out.boss);
    const double bend = std::max(0.0, 1.0 - slope);
    out.visualRate = static_cast<float>(bend);
    out.hitStop = stop;
    if (!stop && bend < 1.0 - kEps) out.slowMo = static_cast<float>(bend);

    // The open moves whose tell has come, soonest impact first.
    std::array<const PromptRec*, kMaxBossPrompts> open{};
    std::size_t n = 0;
    for (const PromptRec& r : recs_) {
        if (!r.open || r.tell > trackTime + kEps || ended_) continue;
        std::size_t at = n++;
        while (at > 0 && open[at - 1]->impact > r.impact) {
            open[at] = open[at - 1];
            --at;
        }
        open[at] = &r;
    }
    out.promptCount = std::min(n, kChoreoPrompts);
    for (std::size_t i = 0; i < out.promptCount; ++i) {
        const PromptRec& r = *open[i];
        ChoreoPromptView& pv = out.prompts[i];
        pv.id = r.id;
        pv.phase = r.phase;
        pv.move = r.move;
        pv.finisher = r.finisher;
        pv.answer = r.answer;
        pv.anchor = r.anchor;
        const double span = r.impact - r.tell;
        pv.progress = static_cast<float>(span > kEps ? std::clamp((trackTime - r.tell) / span, 0.0, 1.0) : 1.0);
    }
}

void BeatChoreo::sampleNow(ChoreoSample& out) const { sample(trackNow(), out); }

// ---- data ----

namespace {

std::string choreoNumberText(double v) {
    char text[32];
    std::snprintf(text, sizeof text, "%g", v);
    return text;
}

bool choreoIdText(std::string_view s) {
    if (s.empty() || s.size() > kMaxClipId) return false;
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

struct ChoreoLoad {
    std::vector<std::string>& errors;
    std::vector<std::string>* warnings;

    void unknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys) const {
        if (!warnings || !j.isObject()) return;
        for (std::uint32_t k = 0; k < j.size(); ++k) {
            const std::string_view key = j.key(k);
            if (std::find(keys.begin(), keys.end(), key) == keys.end())
                warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
        }
    }
    // A clip id member into an index: absent or "" is none (an error when `required`).
    void clip(JsonValue j, const char* key, const BeatClips& clips, const std::string& path, bool required,
              std::int16_t& out) const {
        const std::string where = path + "." + key;
        const JsonValue v = j[key];
        out = kNoClip;
        if (v.isAbsent() || (v.isString() && v.string().empty())) {
            if (required) errors.push_back(where + ": expected a clip id");
            return;
        }
        if (!v.isString()) {
            errors.push_back(where + ": expected a clip id string");
            return;
        }
        const int at = clips.find(v.string());
        if (at < 0) errors.push_back(where + ": unknown clip " + std::string(v.string()));
        else out = static_cast<std::int16_t>(at);
    }
    void shot(JsonValue j, const char* key, const std::string& path, bool required, ChoreoShot& out, bool* given) const {
        const JsonValue v = j[key];
        if (given) *given = false;
        if (v.isAbsent()) {
            if (required) errors.push_back(path + "." + key + ": expected a shot");
            return;
        }
        if (!v.isString() || !parseChoreoShot(v.string(), out)) {
            errors.push_back(path + "." + key + ": expected one of wide, over_lead, over_boss, close_lead, close_boss, low_hero, top_down, finisher_orbit");
            return;
        }
        if (given) *given = true;
    }
};

void choreoMove(const ChoreoLoad& ld, JsonValue j, const std::string& path, const std::string& expect,
                const BeatClips& clips, ChoreoMove& out) {
    ld.unknownKeys(j, path, {"id", "boss", "ok", "then", "miss", "hit", "shot", "anchor"});
    if (!j.isObject()) {
        ld.errors.push_back(path + ": expected an object");
        return;
    }
    if (j["id"].string() != expect) ld.errors.push_back(path + ".id: expected " + expect + " (the track's move at this index)");
    ld.clip(j, "boss", clips, path, false, out.boss);
    ld.clip(j, "ok", clips, path, true, out.ok);
    ld.clip(j, "then", clips, path, false, out.then);
    ld.clip(j, "miss", clips, path, true, out.miss);
    ld.clip(j, "hit", clips, path, false, out.hit);
    ld.shot(j, "shot", path, true, out.shot, nullptr);
    const JsonValue a = j["anchor"];
    if (!a.isString() || !parseChoreoAnchor(a.string(), out.anchor))
        ld.errors.push_back(path + ".anchor: expected one of boss_head, boss_fist_r, boss_fist_l, boss_chest, lead_head, lead_chest, screen_center");
}

void choreoBeat(const ChoreoLoad& ld, JsonValue root, const char* key, const BeatClips& clips, ChoreoBeat& out) {
    const JsonValue j = root[key];
    if (j.isAbsent()) return;
    ld.unknownKeys(j, key, {"lead", "boss", "shot"});
    if (!j.isObject()) {
        ld.errors.push_back(std::string(key) + ": expected an object");
        return;
    }
    ld.clip(j, "lead", clips, key, false, out.lead);
    ld.clip(j, "boss", clips, key, false, out.boss);
    ld.shot(j, "shot", key, false, out.shot, &out.cut);
}

} // namespace

bool importBeatClips(std::string_view json, BeatClips& out, std::string* error, std::vector<std::string>* warnings) {
    out = BeatClips{};
    std::vector<std::string> errors;
    const ChoreoLoad ld{errors, warnings};
    const auto finish = [&]() {
        if (errors.empty()) return true;
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        out = BeatClips{};
        return false;
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("beat clips file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("beat clips file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");
    ld.unknownKeys(root, "", {"version", "clips"});
    const JsonValue list = root["clips"];
    if (!list.isArray() || list.size() == 0 || list.size() > kMaxBeatClips) {
        errors.push_back("clips: expected an array of 1.." + std::to_string(kMaxBeatClips) + " clips");
        return finish();
    }
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const std::string where = "clips[" + std::to_string(i) + "]";
        const JsonValue c = list[i];
        ld.unknownKeys(c, where, {"id", "length", "impact", "loop"});
        BeatClip clip;
        const std::string_view id = c["id"].string();
        if (!choreoIdText(id)) {
            errors.push_back(where + ".id: expected 1.." + std::to_string(kMaxClipId) + " letters, digits, _ and -");
        } else {
            std::copy(id.begin(), id.end(), clip.idText.begin());
            for (std::size_t k = 0; k < out.count; ++k)
                if (out.items[k].id() == id) errors.push_back(where + ".id: duplicate clip " + std::string(id));
        }
        const double length = c["length"].number(-1.0);
        if (!c["length"].isNumber() || !std::isfinite(length) || !(length > 0.0) || length > static_cast<double>(kMaxChoreoClipLength))
            errors.push_back(where + ".length: expected seconds over 0 and at most " + std::to_string(kMaxChoreoClipLength));
        else
            clip.length = length;
        if (const JsonValue iv = c["impact"]; !iv.isAbsent()) {
            const double impact = iv.number(-1.0);
            if (!iv.isNumber() || !std::isfinite(impact) || impact < 0.0 || impact > clip.length)
                errors.push_back(where + ".impact: expected seconds within the clip (0: none), 0.." + choreoNumberText(clip.length));
            else
                clip.impact = impact;
        }
        if (const JsonValue lv = c["loop"]; !lv.isAbsent()) {
            if (!lv.isBool()) errors.push_back(where + ".loop: expected true or false");
            else clip.loop = lv.boolean();
        }
        out.items[out.count++] = clip;
    }
    return finish();
}

bool importBeatChoreo(std::string_view json, const BeatBossTrack& track, const BeatClips& clips, BeatChoreoDef& out,
                      std::string* error, std::vector<std::string>* warnings) {
    out = BeatChoreoDef{};
    std::vector<std::string> errors;
    const ChoreoLoad ld{errors, warnings};
    const auto finish = [&]() {
        if (errors.empty()) return true;
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        out = BeatChoreoDef{};
        return false;
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("beat choreography file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("beat choreography file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");
    ld.unknownKeys(root, "", {"version", "track", "stance", "opening", "finisher", "stagger", "victory", "defeat", "phases"});
    if (root["track"].string() != track.id) errors.push_back("track: expected " + track.id);

    BeatChoreoDef def;
    if (const JsonValue st = root["stance"]; st.isObject()) {
        ld.unknownKeys(st, "stance", {"lead", "boss"});
        ld.clip(st, "lead", clips, "stance", true, def.stanceLead);
        ld.clip(st, "boss", clips, "stance", true, def.stanceBoss);
        for (const std::int16_t sc : {def.stanceLead, def.stanceBoss})
            if (sc >= 0 && !clips[static_cast<std::size_t>(sc)].loop)
                errors.push_back("stance: " + std::string(clips.id(static_cast<std::size_t>(sc))) + " must be a looping clip");
    } else {
        errors.push_back("stance: expected {\"lead\": <clip>, \"boss\": <clip>}");
    }
    if (root.has("opening")) ld.shot(root, "opening", "", false, def.opening, nullptr);
    if (const JsonValue f = root["finisher"]; !f.isAbsent()) {
        ld.unknownKeys(f, "finisher", {"slow"});
        const JsonValue s = f["slow"];
        if (!s.isAbsent()) {
            const double slow = s.number(-1.0);
            if (!s.isNumber() || !(slow >= 0.2) || slow > 1.0) errors.push_back("finisher.slow: expected a speed in 0.2..1");
            else def.finisherSlow = static_cast<float>(slow);
        }
    }
    choreoBeat(ld, root, "stagger", clips, def.stagger);
    choreoBeat(ld, root, "victory", clips, def.victory);
    choreoBeat(ld, root, "defeat", clips, def.defeat);

    const JsonValue phases = root["phases"];
    if (!phases.isArray() || phases.size() != track.def.phaseCount) {
        errors.push_back("phases: expected " + std::to_string(track.def.phaseCount) + " phases, one for each of the track's");
    } else {
        def.phaseCount = track.def.phaseCount;
        for (std::uint32_t i = 0; i < phases.size(); ++i) {
            const std::string path = "phases[" + std::to_string(i) + "]";
            const JsonValue ph = phases[i];
            ld.unknownKeys(ph, path, {"id", "moves", "finisher"});
            const BarMap& bm = track.def.phases[i];
            if (ph["id"].string() != track.phaseIds[i]) errors.push_back(path + ".id: expected " + track.phaseIds[i]);
            ChoreoPhase& out_ph = def.phases[i];
            const struct { const char* key; std::uint8_t count; std::size_t first; } lists[2] = {
                {"moves", bm.moveCount, 0}, {"finisher", bm.finisherCount, bm.moveCount}};
            for (int l = 0; l < 2; ++l) {
                const JsonValue arr = ph[lists[l].key];
                const std::string lpath = path + "." + lists[l].key;
                if (lists[l].count == 0 && arr.isAbsent()) continue;
                if (!arr.isArray() || arr.size() != lists[l].count) {
                    errors.push_back(lpath + ": expected " + std::to_string(lists[l].count) + " entries, the track's moves in order");
                    continue;
                }
                for (std::uint32_t k = 0; k < arr.size(); ++k) {
                    const std::string mpath = lpath + "[" + std::to_string(k) + "]";
                    const std::size_t idAt = lists[l].first + k;
                    const std::string expect = idAt < track.moveIds[i].size() ? track.moveIds[i][idAt] : std::string();
                    ChoreoMove& mv = l == 0 ? out_ph.moves[k] : out_ph.finisher[k];
                    choreoMove(ld, arr[k], mpath, expect, clips, mv);
                }
                if (l == 0) out_ph.moveCount = lists[l].count;
                else out_ph.finisherCount = lists[l].count;
            }
        }
    }
    if (!finish()) return false;
    out = def;
    return true;
}

} // namespace ftcore
