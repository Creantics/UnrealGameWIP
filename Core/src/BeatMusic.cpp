#include "ftcore/BeatMusic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ftcore {

namespace {

std::uint8_t u8(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }

// The key is D minor; the bass lives around D2. A chord is its root (semitones above D) and its quality.
struct Chord {
    int root;
    std::uint8_t major;
};

constexpr Chord kProgressions[4][4] = {
    {{0, 0}, {8, 1}, {3, 1}, {10, 1}}, // Dm  Bb  F   C
    {{0, 0}, {5, 0}, {8, 1}, {7, 1}},  // Dm  Gm  Bb  A
    {{0, 0}, {8, 1}, {5, 0}, {7, 1}},  // Dm  Bb  Gm  A
    {{0, 0}, {3, 1}, {5, 0}, {10, 1}}, // Dm  F   Gm  C
};
constexpr int kBassBase = 38; // D2
constexpr Chord kDominant{7, 1};
constexpr Chord kTonic{0, 0};

int bassNote(int rootOffset) { return kBassBase + (rootOffset > 6 ? rootOffset - 12 : rootOffset); }

struct Planner {
    MusicPattern& p;
    int steps;

    void put(int step, MusicVoice v, int velocity, int note = 0, int length = 0, int aux = 0) {
        if (step < 0 || step >= steps || velocity <= 0) return;
        MusicHit& h = p.hits[static_cast<std::size_t>(step)][static_cast<std::size_t>(v)];
        if (velocity <= h.velocity) return;
        h.velocity = u8(std::min(velocity, 127));
        h.note = u8(note);
        h.length = u8(length);
        h.aux = u8(aux);
    }
    MusicHit& hit(int step, MusicVoice v) {
        return p.hits[static_cast<std::size_t>(step)][static_cast<std::size_t>(v)];
    }
    void clear(int step, MusicVoice v) { hit(step, v) = MusicHit{}; }
};

} // namespace

int MusicPattern::hitCount() const {
    int n = 0;
    for (int s = 0; s < steps; ++s)
        for (const MusicHit& h : hits[static_cast<std::size_t>(s)])
            if (h.velocity > 0) ++n;
    return n;
}

int MusicPattern::velocitySum() const {
    int n = 0;
    for (int s = 0; s < steps; ++s)
        for (const MusicHit& h : hits[static_cast<std::size_t>(s)]) n += h.velocity;
    return n;
}

int musicTier(const MusicBarContext& ctx) {
    if (ctx.finisher) return 3;
    if (ctx.forcedTier >= 0) return std::min<int>(ctx.forcedTier, 3);
    const float progress = std::clamp(ctx.phaseProgress, 0.0f, 0.99f);
    const float phases = static_cast<float>(std::max<int>(1, ctx.phaseCount));
    const float level = (static_cast<float>(ctx.phase) + progress) / phases;
    return std::clamp(static_cast<int>(level * 4.0f), 0, 3);
}

void planBeatMusic(const MusicBarContext& ctx, MusicPattern& out) {
    out = MusicPattern{};
    const int E = std::clamp<int>(ctx.eighths, 1, static_cast<int>(kMaxBossBarEighths)); // the bar's eighths
    const int B = ctx.beatEighths < 1 ? 2 : ctx.beatEighths;                              // a beat's
    const int steps = 2 * E;
    out.steps = u8(steps);
    if (ctx.silent) return;

    const bool fin = ctx.finisher;
    const bool build = ctx.nextFinisher && !fin;
    const int tier = musicTier(ctx);
    out.tier = u8(tier);
    Planner pl{out, steps};

    const int beats = (E + B - 1) / B;
    const auto beatStep = [&](int i) { return 2 * i * B; };
    const auto beatLen = [&](int i) { return std::min(B, E - i * B); }; // in eighths

    // The harmony: a progression per phase, a chord a bar or two; the dominant in the build, the tonic in the finisher.
    const int chordBars = (ctx.phase % 2 == 0) ? 2 : 1;
    const int chordIndex = ctx.bar == 0 ? 0 : ((static_cast<int>(ctx.bar) - 1) / chordBars) % 4;
    Chord chord = kProgressions[ctx.phase % 4][chordIndex];
    if (build) chord = kDominant;
    if (fin) chord = kTonic;
    const int root = bassNote(chord.root);
    const int major = chord.major;

    // Kick: the downbeat always; the rest by tier.
    pl.put(0, MusicVoice::Kick, fin ? 127 : 118);
    for (int i = 1; i < beats; ++i) {
        const int s = beatStep(i);
        const bool odd = (i & 1) != 0;
        if (fin) pl.put(s, MusicVoice::Kick, odd ? 104 : 122);
        else if (tier >= 3) pl.put(s, MusicVoice::Kick, odd ? 72 : 104);
        else if (tier >= 1 && !odd) pl.put(s, MusicVoice::Kick, tier == 1 ? 92 : 100);
        if (tier >= 2 || fin) {
            if (odd) pl.put(s - 1, MusicVoice::Kick, 78); // a pickup into the backbeat
        }
    }

    // Snare on the second, fourth ... beat when the beat is whole.
    static constexpr int kSnareVelocity[4] = {80, 100, 108, 116};
    for (int i = 1; i < beats; i += 2) {
        if (beatLen(i) != B) continue;
        const int s = beatStep(i);
        pl.put(s, MusicVoice::Snare, fin ? 127 : kSnareVelocity[tier], 0, 0, (tier >= 2 || fin) ? 1 : 0);
        if (tier >= 3 && !fin && 3 < 2 * beatLen(i)) pl.put(s + 3, MusicVoice::Snare, 40); // a ghost note
    }

    // Hats.
    for (int e = 0; e < E; ++e) {
        const bool onBeat = e % B == 0;
        const int s = 2 * e;
        if (tier == 0) {
            if (onBeat) pl.put(s, MusicVoice::HatClosed, 44);
            continue;
        }
        if (tier == 1) {
            pl.put(s, MusicVoice::HatClosed, onBeat ? 64 : 46);
            continue;
        }
        if (tier == 2) {
            pl.put(s, MusicVoice::HatClosed, onBeat ? 70 : 50);
            if (e % 2 == 1) pl.put(s + 1, MusicVoice::HatClosed, 26);
            continue;
        }
        pl.put(s, MusicVoice::HatClosed, std::min(127, (onBeat ? 78 : 60) + (fin ? 20 : 0)));
        pl.put(s + 1, MusicVoice::HatClosed, fin ? 52 : 34);
    }
    if (tier >= 2) {
        for (int e = 0; e < E; ++e) {
            // The last eighth of an even beat: the open hat lifts into the backbeat.
            if (e % B == B - 1 && ((e / B) & 1) == 0) {
                const int s = 2 * e;
                pl.clear(s, MusicVoice::HatClosed);
                pl.put(s, MusicVoice::HatOpen, fin ? 108 : 84, 0, 2);
            }
        }
    }

    // Bass.
    switch (tier) {
    case 0: pl.put(0, MusicVoice::Bass, 100, root, steps); break;
    case 1:
        for (int i = 0; i < beats; ++i) pl.put(beatStep(i), MusicVoice::Bass, i == 0 ? 110 : 96, root, std::max(1, 2 * beatLen(i) - 1));
        break;
    default:
        for (int e = 0; e < E; ++e) {
            const bool onBeat = e % B == 0;
            const bool lift = e % B == B - 1 && ((e / B) & 1) == 1;
            pl.put(2 * e, MusicVoice::Bass, (onBeat ? 112 : 84) + (fin ? 12 : 0), lift ? root + 12 : root, 1);
        }
        if (tier >= 3) {
            for (int s = 3; s < steps; s += 4) pl.put(s, MusicVoice::Bass, fin ? 88 : 70, root + 7, 1);
        }
        break;
    }

    // Stabs and the pad.
    if (fin) {
        for (int i = 0; i < beats; ++i) pl.put(beatStep(i), MusicVoice::Stab, 122, root + 24, 3, major);
    } else if (tier == 1) {
        pl.put(0, MusicVoice::Stab, 56, root + 24, steps, major | 2);
    } else if (tier == 2) {
        pl.put(0, MusicVoice::Stab, 96, root + 24, 3, major);
        if (beats > 2) pl.put(beatStep(2), MusicVoice::Stab, 80, root + 24, 2, major);
    } else if (tier >= 3) {
        pl.put(0, MusicVoice::Stab, 100, root + 24, 3, major);
        for (int i = 1; i < beats; i += 2) pl.put(beatStep(i), MusicVoice::Stab, 88, root + 24, 2, major);
    }

    // The build: the bass, the stabs and the hats leave the last half bar for a snare roll that grows.
    if (build) {
        const int rollSteps = std::min(8, steps / 2);
        const int rollStart = steps - rollSteps;
        for (int s = rollStart; s < steps; ++s) {
            for (MusicVoice v : {MusicVoice::Kick, MusicVoice::HatClosed, MusicVoice::HatOpen, MusicVoice::Bass, MusicVoice::Stab, MusicVoice::Snare}) {
                if (s == 0 && v == MusicVoice::Kick) continue;
                pl.clear(s, v);
            }
            pl.put(s, MusicVoice::Snare, 48 + 79 * (s - rollStart + 1) / rollSteps);
        }
        pl.put(0, MusicVoice::Riser, 100, 0, steps);
    }

    // An empty beat of the bar map: the music leaves it.
    for (int i = 0; i < beats && i < 16; ++i) {
        if (((ctx.emptyBeats >> i) & 1u) == 0) continue;
        const int from = beatStep(i);
        const int to = std::min(steps, beatStep(i) + 2 * B);
        for (int s = from; s < to; ++s)
            for (MusicVoice v : {MusicVoice::Kick, MusicVoice::Snare, MusicVoice::HatClosed, MusicVoice::HatOpen, MusicVoice::Bass, MusicVoice::Stab})
                pl.clear(s, v);
    }

    // The first bar of a phase or after a silence, and the finisher: an impact (and a crash).
    if (ctx.entry || fin) {
        pl.put(0, MusicVoice::Impact, fin ? 127 : 112, 0, std::min(steps, 32));
        pl.put(0, MusicVoice::HatOpen, fin ? 127 : 104, 0, std::min(steps, 32), 1);
    }

    // Told moves: an accent on each impact, and the music leaves room for it.
    for (int s = 0; s < steps; ++s) {
        const int a = ctx.accent[static_cast<std::size_t>(s)];
        if (a == 0) continue;
        for (MusicVoice v : {MusicVoice::HatClosed, MusicVoice::HatOpen, MusicVoice::Stab, MusicVoice::Bass}) {
            if (s == 0 && v == MusicVoice::HatOpen && pl.hit(0, v).aux == 1) continue; // the crash stays
            pl.clear(s, v);
        }
        if (pl.hit(s, MusicVoice::Snare).velocity < 60) pl.clear(s, MusicVoice::Snare);
        if (s != 0) {
            for (MusicVoice v : {MusicVoice::Kick, MusicVoice::Snare}) {
                MusicHit& h = pl.hit(s, v);
                h.velocity = u8(h.velocity * 6 / 10);
            }
        }
        pl.put(s, MusicVoice::Accent, 127, 0, 2, std::min(a - 1, 2));
    }
}

// ---- the driver ----

namespace {

constexpr double kMusicEpsilon = 1e-6; // times closer than this are the same step
constexpr double kMusicRewind = 1e-3;
constexpr double kMusicStale = 0.12;   // a step this far behind the clock is dropped, not flooded

} // namespace

void BeatMusicDriver::add(MusicEvents& out, const MusicEvent& e) {
    if (out.count >= out.items.size()) {
        ++dropped_;
        return;
    }
    out.items[out.count++] = e;
}

void BeatMusicDriver::play(MusicEvents& out, MusicSound s, double time, std::uint8_t velocity, std::uint8_t note,
                           std::uint8_t aux, double length) {
    MusicEvent e;
    e.kind = MusicEventKind::Play;
    e.sound = s;
    e.time = time;
    e.velocity = velocity;
    e.note = note;
    e.aux = aux;
    e.length = length;
    add(out, e);
}

void BeatMusicDriver::reset(double now, MusicEvents& out) {
    out.count = 0;
    MusicEvent revoke;
    revoke.kind = MusicEventKind::Revoke;
    revoke.time = -1e18;
    add(out, revoke);
    MusicEvent cut;
    cut.kind = MusicEventKind::Cut;
    cut.time = now;
    add(out, cut);
    mark_ = -1e30;
    countMark_ = -1e30;
    barsSeen_ = std::numeric_limits<std::uint32_t>::max();
    lastPhase_ = -1;
    predicted_ = MusicBarKey{};
    entryCur_ = true;
    predictedValid_ = false;
    entryNext_ = true;
    riserSent_[0] = riserSent_[1] = false;
    phaseStartHp_ = 0.0f;
    tierCur_ = tierNext_ = -1;
    over_ = false;
    silencePending_ = false;
    endPending_ = false;
    lastPrompt_ = 0;
    cueCount_ = 0;
    afterSilence_ = false;
}

void BeatMusicDriver::onEvent(const BeatBossEvent& e) {
    switch (e.type) {
    case BeatBossEventType::SilenceStart:
        silencePending_ = true;
        silenceAt_ = e.time;
        silenceMs_ = e.amount;
        break;
    case BeatBossEventType::Victory:
    case BeatBossEventType::Defeat:
        endPending_ = true;
        endVictory_ = e.type == BeatBossEventType::Victory;
        endAt_ = e.time;
        break;
    default:
        break;
    }
}

void BeatMusicDriver::trackCues(const BeatBoss& fight) {
    for (std::size_t i = 0; i < fight.promptCount(); ++i) {
        const BeatBossPrompt& p = fight.prompt(i);
        if (p.id <= lastPrompt_) continue;
        lastPrompt_ = std::max(lastPrompt_, p.id);
        if (cueCount_ >= cues_.size()) continue;
        cues_[cueCount_++] = Cue{p.impact, static_cast<std::uint8_t>(p.stem % 3)};
    }
    // Impacts long past are forgotten.
    for (std::size_t i = 0; i < cueCount_;) {
        if (cues_[i].impact < fight.trackTime() - 1.0) cues_[i] = cues_[--cueCount_];
        else ++i;
    }
}

void BeatMusicDriver::fillContext(const BeatBoss& fight, const BossBarPreview& bar, double startTrack, bool entry,
                                  bool nextFinisher, MusicBarContext& ctx) const {
    const BeatBossDef& def = fight.def();
    const BarMap& m = def.phases[static_cast<std::size_t>(bar.phase)];
    const BossBar shape = bar.finisher ? m.meter : m.bars[static_cast<std::size_t>(bar.bar)];
    ctx = MusicBarContext{};
    ctx.eighths = shape.eighths;
    ctx.beatEighths = m.beatEighths;
    ctx.emptyBeats = shape.emptyBeats;
    ctx.phase = static_cast<std::uint8_t>(bar.phase);
    ctx.phaseCount = def.phaseCount;
    ctx.bar = bar.finisher ? 0 : static_cast<std::uint16_t>(bar.bar + 1);
    ctx.finisher = bar.finisher;
    ctx.nextFinisher = nextFinisher;
    ctx.entry = entry;
    if (bar.phase == fight.phase() && !bar.newPhase) {
        const float floorHp = static_cast<float>(fight.floorHp());
        if (phaseStartHp_ > floorHp)
            ctx.phaseProgress = std::clamp((phaseStartHp_ - static_cast<float>(fight.bossHp())) / (phaseStartHp_ - floorHp), 0.0f, 1.0f);
    }
    const double step = m.eighthSeconds() * 0.5;
    const int steps = 2 * shape.eighths;
    for (std::size_t i = 0; i < cueCount_; ++i) {
        const double rel = (cues_[i].impact - startTrack) / step;
        const long s = std::lround(rel);
        if (s < 0 || s >= steps || std::fabs(rel - static_cast<double>(s)) > 0.25) continue;
        ctx.accent[static_cast<std::size_t>(s)] = static_cast<std::uint8_t>(cues_[i].timbre + 1);
    }
}

void BeatMusicDriver::scheduleBar(const MusicBarContext& ctx, double start, double stepSeconds, double until,
                                  bool& riserSent, MusicEvents& out) {
    MusicPattern p;
    planBeatMusic(ctx, p);
    const int steps = p.steps;
    int first = -1;
    for (int k = 0; k < steps; ++k) {
        const double t = start + static_cast<double>(k) * stepSeconds;
        if (t <= mark_ + kMusicEpsilon) continue;
        if (t > until) break;
        if (first < 0) {
            first = k;
            // The build began after the bar's first step: the riser joins late and covers what is left.
            if (k > 0 && ctx.nextFinisher && !ctx.finisher && !riserSent) {
                play(out, MusicSound::Riser, t, 100, 0, 0, static_cast<double>(steps - k) * stepSeconds);
                riserSent = true;
            }
        }
        mark_ = t;
        for (std::size_t v = 0; v < kMusicVoices; ++v) {
            const MusicHit& h = p.hits[static_cast<std::size_t>(k)][v];
            if (h.velocity == 0) continue;
            const double len = static_cast<double>(h.length) * stepSeconds;
            switch (static_cast<MusicVoice>(v)) {
            case MusicVoice::Kick: play(out, MusicSound::Kick, t, h.velocity); break;
            case MusicVoice::Snare: play(out, MusicSound::Snare, t, h.velocity, 0, h.aux); break;
            case MusicVoice::HatClosed: play(out, MusicSound::HatClosed, t, h.velocity); break;
            case MusicVoice::HatOpen: play(out, MusicSound::HatOpen, t, h.velocity, 0, h.aux, len); break;
            case MusicVoice::Bass: play(out, MusicSound::Bass, t, h.velocity, h.note, 0, len); break;
            case MusicVoice::Stab: play(out, MusicSound::Stab, t, h.velocity, h.note, h.aux, len); break;
            case MusicVoice::Riser:
                play(out, MusicSound::Riser, t, h.velocity, 0, 0, len);
                riserSent = true;
                break;
            case MusicVoice::Impact: play(out, MusicSound::Impact, t, h.velocity, 0, 0, len); break;
            case MusicVoice::Accent:
                play(out, static_cast<MusicSound>(static_cast<int>(MusicSound::AccentThump) + std::min<int>(h.aux, 2)), t, h.velocity);
                break;
            case MusicVoice::Count: break;
            }
        }
    }
}

void BeatMusicDriver::sendCountBlip(MusicEvents& out, BeatCountKind kind, double time) {
    if (kind == BeatCountKind::Click) play(out, MusicSound::CountClick, time, 127);
    else if (kind == BeatCountKind::Voice) play(out, MusicSound::CountVoice, time, 127);
}

namespace {
MusicBarKey keyOf(const BossBarPreview& b) {
    MusicBarKey k;
    k.phase = b.phase;
    k.bar = static_cast<std::int8_t>(b.finisher ? 0 : b.bar);
    k.loop = b.finisher ? 0 : b.loop;
    k.finisher = b.finisher;
    return k;
}
} // namespace

void BeatMusicDriver::update(const BeatBoss& fight, const BeatLine& line, double now, BeatCountKind kind, MusicEvents& out) {
    out.count = 0;
    if (over_ || fight.state() == BeatBossState::NotStarted) return;

    if (endPending_ || fight.over()) {
        const double at = endPending_ ? endAt_ : now;
        const bool victory = endPending_ ? endVictory_ : fight.state() == BeatBossState::Victory;
        MusicEvent revoke;
        revoke.kind = MusicEventKind::Revoke;
        revoke.time = at;
        add(out, revoke);
        MusicEvent cut;
        cut.kind = MusicEventKind::Cut;
        cut.time = at;
        add(out, cut);
        if (victory) play(out, MusicSound::Impact, at, 127, 0, 0, 1.2);
        over_ = true;
        endPending_ = false;
        return;
    }

    const BeatBossDef& def = fight.def();
    const double until = now + kMusicLookahead;

    if (silencePending_) {
        silencePending_ = false;
        MusicEvent revoke;
        revoke.kind = MusicEventKind::Revoke;
        revoke.time = silenceAt_;
        add(out, revoke);
        MusicEvent cut;
        cut.kind = MusicEventKind::Cut;
        cut.time = silenceAt_;
        add(out, cut);
        ++revoked_;
        predictedValid_ = false;
        afterSilence_ = true;
        // The count back: one bar of the phase's beats, once the silence is over.
        const BarMap& m = def.phases[static_cast<std::size_t>(std::clamp(fight.phase(), 0, static_cast<int>(def.phaseCount) - 1))];
        const int beats = (static_cast<int>(m.meter.eighths) + m.beatEighths - 1) / m.beatEighths;
        const double first = silenceAt_ + static_cast<double>(silenceMs_) * 0.001;
        for (int j = 0; j < beats; ++j) {
            const double t = first + m.beatSeconds() * j;
            sendCountBlip(out, kind, t);
            countMark_ = std::max(countMark_, t);
        }
        return;
    }

    if (fight.trackRunning()) {
        const int phase = std::clamp(fight.phase(), 0, static_cast<int>(def.phaseCount) - 1);
        const BarMap& m = def.phases[static_cast<std::size_t>(phase)];
        const double stepSeconds = m.eighthSeconds() * 0.5;
        const double barStart = fight.now() - line.playhead;
        const double startTrack = fight.trackTime() - line.playhead;
        BossBarPreview cur;
        cur.phase = static_cast<std::int8_t>(phase);
        cur.finisher = fight.inFinisher();
        cur.bar = static_cast<std::int8_t>(cur.finisher ? 0 : std::max(0, fight.bar() - 1));
        cur.loop = fight.loop();
        const MusicBarKey curKey = keyOf(cur);

        if (barsSeen_ != static_cast<std::uint32_t>(fight.stats().bars)) {
            const bool changedPhase = phase != lastPhase_;
            if (predictedValid_ && predicted_ == curKey) {
                riserSent_[0] = riserSent_[1];
                riserSent_[1] = false;
                entryCur_ = entryNext_;
                tierCur_ = tierNext_;
            } else {
                tierCur_ = -1;
                // A bar nobody planned for (or not the one planned): what was sent from its line is dropped.
                if (predictedValid_) ++revoked_;
                MusicEvent revoke;
                revoke.kind = MusicEventKind::Revoke;
                revoke.time = barStart - kMusicRewind;
                add(out, revoke);
                mark_ = barStart - kMusicRewind;
                riserSent_[0] = riserSent_[1] = false;
                entryCur_ = changedPhase || afterSilence_;
            }
            afterSilence_ = false;
            if (changedPhase) {
                phaseStartHp_ = static_cast<float>(fight.bossHp());
                // The prompts a new phase drops took their accents with them.
                for (std::size_t i = 0; i < cueCount_;) {
                    if (cues_[i].impact > startTrack) cues_[i] = cues_[--cueCount_];
                    else ++i;
                }
            }
            lastPhase_ = phase;
            barsSeen_ = static_cast<std::uint32_t>(fight.stats().bars);
            predictedValid_ = false;
        }
        if (mark_ > -1e29) mark_ = std::max(mark_, now - kMusicStale);
        trackCues(fight);

        const BossBarPreview up = fight.upcomingBar();
        const bool nextFin = up.finisher && !up.newPhase && up.silence < 0 && !cur.finisher;
        MusicBarContext ctx;
        fillContext(fight, cur, startTrack, entryCur_, nextFin, ctx);
        if (tierCur_ < 0) tierCur_ = musicTier(ctx); // a bar nobody planned: its tier is read now, once
        ctx.forcedTier = static_cast<std::int8_t>(tierCur_);
        scheduleBar(ctx, barStart, stepSeconds, until, riserSent_[0], out);

        // The next bar's first steps go out before its bar line.
        const double nextStart = barStart + line.bars[0].seconds;
        if (predictedValid_ && (up.silence >= 0 || !(keyOf(up) == predicted_))) {
            MusicEvent revoke;
            revoke.kind = MusicEventKind::Revoke;
            revoke.time = nextStart - kMusicRewind;
            add(out, revoke);
            ++revoked_;
            mark_ = std::min(mark_, nextStart - kMusicRewind);
            riserSent_[1] = false;
            predictedValid_ = false;
        }
        if (up.silence < 0 && nextStart <= until && up.phase < def.phaseCount) {
            MusicBarContext nctx;
            const bool planned = predictedValid_ && keyOf(up) == predicted_;
            fillContext(fight, up, startTrack + line.bars[0].seconds, up.newPhase, false, nctx);
            if (!planned) tierNext_ = musicTier(nctx); // read at the first plan of the bar, then kept
            nctx.forcedTier = static_cast<std::int8_t>(tierNext_);
            const BarMap& nm = def.phases[static_cast<std::size_t>(up.phase)];
            scheduleBar(nctx, nextStart, nm.eighthSeconds() * 0.5, until, riserSent_[1], out);
            predicted_ = keyOf(up);
            predictedValid_ = true;
            entryNext_ = up.newPhase;
        }
        return;
    }

    if (fight.counting() && line.counting && !line.frozen) {
        const double countStart = fight.now() - line.playhead;
        for (std::size_t i = 0; i < line.tickCount; ++i) {
            const BeatLineTick& tk = line.ticks[i];
            if (tk.numeral == 0 || tk.kind == BeatTickKind::End) continue;
            if (tk.bar == 1 && !line.bars[1].count) continue;
            const double t = countStart + tk.at;
            if (t <= countMark_ + kMusicEpsilon || t > until) continue;
            if (t < now - kMusicStale) continue; // a blip the stream was not up for is not played late
            sendCountBlip(out, kind, t);
            countMark_ = t;
        }
        // The last count bar: the bar it gives back to is planned now, an impact on its downbeat.
        if (!line.bars[1].count) {
            const double resumeAt = countStart + line.bars[0].seconds;
            if (resumeAt <= until) {
                const BossBarPreview up = fight.upcomingBar();
                if (up.phase < def.phaseCount) {
                    if (!predictedValid_ || !(keyOf(up) == predicted_)) {
                        mark_ = resumeAt - kMusicRewind;
                        predicted_ = keyOf(up);
                        predictedValid_ = true;
                        riserSent_[0] = riserSent_[1] = false;
                        entryNext_ = true;
                        tierNext_ = -1;
                    }
                    trackCues(fight);
                    MusicBarContext ctx;
                    fillContext(fight, up, fight.trackTime(), true, false, ctx);
                    if (tierNext_ < 0) tierNext_ = musicTier(ctx);
                    ctx.forcedTier = static_cast<std::int8_t>(tierNext_);
                    const BarMap& nm = def.phases[static_cast<std::size_t>(up.phase)];
                    scheduleBar(ctx, resumeAt, nm.eighthSeconds() * 0.5, until, riserSent_[1], out);
                }
            }
        }
    }
}

} // namespace ftcore
