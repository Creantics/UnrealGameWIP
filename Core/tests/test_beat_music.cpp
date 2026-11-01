#include "ftcore/BeatBoss.h"
#include "ftcore/BeatMusic.h"
#include "ftcore/BeatMusicSynth.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>

using namespace ftcore;

// The beat boss demo's music (docs/BEAT_BOSS_DEMO.md): the pattern sequencer for every meter, the driver that
// follows the fight, and the synth rendered offline against the fight's clock. An autoplayed fight of the test
// track is rendered in memory and checked: the beats line up with the bar lines, silences are silent, the build
// into the finisher is there, the mix does not clip and the accents stand out. Set FTCORE_MUSIC_RENDER_DIR to a
// folder and the same render is written there as WAV files to listen to.

namespace {

constexpr int kRate = 48000;

BeatBossTrack loadTrack() {
    std::string text;
    BeatBossTrack track;
    if (!ftcore_test::readProjectData("Battle/beat_boss/test_track.json", text)) {
        FAIL("Data/Battle/beat_boss/test_track.json not found");
        return track;
    }
    std::string error;
    REQUIRE_MESSAGE(importBeatBossTrack(text, track, &error), error);
    return track;
}

MusicBarContext bar(int eighths, int beatEighths, int phase = 0, int phases = 2, float progress = 0.0f) {
    MusicBarContext c;
    c.eighths = static_cast<std::uint8_t>(eighths);
    c.beatEighths = static_cast<std::uint8_t>(beatEighths);
    c.phase = static_cast<std::uint8_t>(phase);
    c.phaseCount = static_cast<std::uint8_t>(phases);
    c.phaseProgress = progress;
    return c;
}

int voiceHits(const MusicPattern& p, MusicVoice v) {
    int n = 0;
    for (int s = 0; s < p.steps; ++s)
        if (p.at(s, v).velocity > 0) ++n;
    return n;
}

bool samePattern(const MusicPattern& a, const MusicPattern& b) { return std::memcmp(&a, &b, sizeof(MusicPattern)) == 0; }

std::uint64_t sampleOf(double t) { return t <= 0.0 ? 0 : static_cast<std::uint64_t>(std::llround(t * kRate)); }

bool isMusic(MusicSound s) { return static_cast<int>(s) <= static_cast<int>(MusicSound::AccentBell); }
bool isAccentSound(MusicSound s) {
    return s == MusicSound::AccentThump || s == MusicSound::AccentSnare || s == MusicSound::AccentBell;
}

// ---- an autoplayed fight, rendered offline ----

struct Render {
    std::vector<float> audio;
    std::vector<MusicEvent> played;         // what the synth was left to play (revoked and cut sounds removed)
    std::vector<BeatBossEvent> fightEvents;
    std::uint32_t late = 0, dropped = 0, revoked = 0;
    bool won = false;
    double endTime = 0.0;
};

struct RenderOptions {
    double dt = 1.0 / 60.0;
    double maxSeconds = 120.0;
    bool noAccents = false; // leave the accent sounds out of the mix (to measure what they add)
    bool noMusic = false;   // leave the music out (the accents and counts alone)
    bool onlyKicks = false; // the kicks alone (to find their onsets)
};

Render renderFight(const BeatBossTrack& track, const RenderOptions& opt) {
    Render r;
    BeatBoss fight;
    BeatMusicDriver driver;
    BeatMusicSynth synth;
    synth.init(kRate);
    MusicEvents ev;
    const BeatBossTuning tuning{};
    BeatCountKind countKind = track.def.leadInCount;

    const auto apply = [&](const MusicEvents& events) {
        for (const MusicEvent& e : events) {
            if (e.kind == MusicEventKind::Revoke || e.kind == MusicEventKind::Cut) {
                const std::uint64_t from = sampleOf(e.time);
                if (e.kind == MusicEventKind::Revoke) synth.revokeFrom(from);
                else synth.cut(from);
                const std::uint64_t pos = synth.position();
                r.played.erase(std::remove_if(r.played.begin(), r.played.end(),
                                              [&](const MusicEvent& p) { return sampleOf(p.time) >= from && sampleOf(p.time) >= pos; }),
                               r.played.end());
                continue;
            }
            if (opt.noAccents && isAccentSound(e.sound)) continue;
            if (opt.noMusic && isMusic(e.sound) && !isAccentSound(e.sound)) continue;
            if (opt.onlyKicks && e.sound != MusicSound::Kick) continue;
            synth.play(sampleOf(e.time), e.sound, e.velocity, e.note, e.aux,
                       static_cast<std::uint32_t>(e.length * kRate + 0.5));
            r.played.push_back(e);
        }
    };

    fight.begin(track.def, tuning, 0.0);
    driver.reset(0.0, ev);
    apply(ev);
    const auto handle = [&](const BeatBossEvents& events) {
        for (const BeatBossEvent& e : events) {
            r.fightEvents.push_back(e);
            driver.onEvent(e);
            if (e.type == BeatBossEventType::SilenceStart) {
                const BarMap& m = track.def.phases[static_cast<std::size_t>(fight.phase())];
                for (std::size_t i = 0; i < m.silenceCount; ++i)
                    if (std::fabs(m.silences[i].seconds * 1000.0 - e.amount) <= 1.0) countKind = m.silences[i].count;
            }
        }
    };
    BeatLine line;
    double tail = -1.0;
    for (double now = opt.dt; now < opt.maxSeconds; now += opt.dt) {
        for (int guard = 0; guard < 64 && !fight.over(); ++guard) {
            const QteAction a = fight.autopilot(now);
            if (a.kind == QteAction::Kind::None) break;
            handle(a.kind == QteAction::Kind::Press ? fight.press(a.input, a.time) : fight.release(a.input, a.time));
        }
        handle(fight.update(now));
        fight.beatLine(line);
        driver.update(fight, line, now, countKind, ev);
        apply(ev);
        const std::uint64_t to = sampleOf(now);
        const std::size_t before = r.audio.size();
        r.audio.resize(to);
        synth.render(r.audio.data() + before, static_cast<int>(to - before));
        if (fight.over() && tail < 0.0) tail = now + 2.5;
        if (tail >= 0.0 && now >= tail) break;
    }
    r.late = synth.late();
    r.dropped = synth.dropped() + driver.dropped();
    r.revoked = driver.revoked();
    r.won = fight.state() == BeatBossState::Victory;
    r.endTime = tail - 2.5;
    return r;
}

bool writeWav(const std::string& path, const float* samples, std::size_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const auto put32 = [&](std::uint32_t v) {
        const char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF), static_cast<char>((v >> 16) & 0xFF),
                           static_cast<char>((v >> 24) & 0xFF)};
        f.write(b, 4);
    };
    const auto put16 = [&](std::uint16_t v) {
        const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
        f.write(b, 2);
    };
    f.write("RIFF", 4);
    put32(static_cast<std::uint32_t>(36 + n * 2));
    f.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(1);
    put32(kRate);
    put32(kRate * 2);
    put16(2);
    put16(16);
    f.write("data", 4);
    put32(static_cast<std::uint32_t>(n * 2));
    for (std::size_t i = 0; i < n; ++i) {
        const int v = static_cast<int>(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
        put16(static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
    }
    return static_cast<bool>(f);
}

std::string renderDir() {
    std::string value;
#if defined(_MSC_VER)
    char* raw = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&raw, &length, "FTCORE_MUSIC_RENDER_DIR") == 0 && raw != nullptr) value = raw;
    std::free(raw);
#else
    if (const char* raw = std::getenv("FTCORE_MUSIC_RENDER_DIR")) value = raw;
#endif
    return value;
}

float peakOf(const std::vector<float>& a, std::size_t from = 0, std::size_t to = static_cast<std::size_t>(-1)) {
    float p = 0.0f;
    for (std::size_t i = from; i < a.size() && i < to; ++i) p = std::max(p, std::fabs(a[i]));
    return p;
}

double rmsOf(const std::vector<float>& a, std::size_t from, std::size_t to) {
    double s = 0.0;
    std::size_t n = 0;
    for (std::size_t i = from; i < a.size() && i < to; ++i, ++n) s += static_cast<double>(a[i]) * a[i];
    return n ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}

// The bar lines of the music: each BarStart, or the Resumed that follows when a silence takes the line. A bar
// that starts as the fight is won is not one (the victory cuts the music).
std::vector<double> musicBarLines(const Render& r) {
    std::vector<double> lines;
    double won = 1e30;
    for (const BeatBossEvent& e : r.fightEvents)
        if (e.type == BeatBossEventType::Victory) won = e.time;
    for (std::size_t i = 0; i < r.fightEvents.size(); ++i) {
        const BeatBossEvent& e = r.fightEvents[i];
        if (e.type != BeatBossEventType::BarStart || e.time >= won - 1e-9) continue;
        double at = e.time;
        for (std::size_t j = i + 1; j < r.fightEvents.size() && r.fightEvents[j].time <= e.time + 1e-9; ++j) {
            if (r.fightEvents[j].type == BeatBossEventType::SilenceStart) {
                at = -1.0;
                for (std::size_t k = j + 1; k < r.fightEvents.size(); ++k)
                    if (r.fightEvents[k].type == BeatBossEventType::Resumed) {
                        at = r.fightEvents[k].time;
                        break;
                    }
            }
        }
        if (at >= 0.0) lines.push_back(at);
    }
    return lines;
}

const MusicEvent* playedAt(const Render& r, MusicSound s, double t, double tol = 1e-6) {
    for (const MusicEvent& e : r.played)
        if (e.sound == s && std::fabs(e.time - t) <= tol) return &e;
    return nullptr;
}

} // namespace

// ---- the pattern sequencer ----

TEST_CASE("beat music: every meter fits the bar, the kick holds the downbeat and nothing leaves the grid") {
    for (int beatEighths = 2; beatEighths <= 3; ++beatEighths) {
        for (int eighths = 1; eighths <= static_cast<int>(kMaxBossBarEighths); ++eighths) {
            for (int phase = 0; phase < 4; ++phase) {
                for (int variant = 0; variant < 4; ++variant) {
                    MusicBarContext c = bar(eighths, beatEighths, phase, 4, variant == 3 ? 0.9f : 0.2f);
                    c.finisher = variant == 1;
                    c.nextFinisher = variant == 2;
                    c.entry = variant == 3;
                    c.bar = static_cast<std::uint16_t>(c.finisher ? 0 : 1 + eighths % 5);
                    MusicPattern p;
                    planBeatMusic(c, p);
                    INFO("eighths " << eighths << " beat " << beatEighths << " phase " << phase << " variant " << variant);
                    CHECK(p.steps == 2 * eighths);
                    CHECK(p.at(0, MusicVoice::Kick).velocity > 0);
                    CHECK(p.hitCount() > 0);
                    for (int s = 0; s < kMusicMaxSteps; ++s) {
                        for (std::size_t v = 0; v < kMusicVoices; ++v) {
                            const MusicHit& h = p.hits[static_cast<std::size_t>(s)][v];
                            if (s >= p.steps) CHECK(h.velocity == 0);
                            CHECK(h.velocity <= 127);
                            if (h.velocity > 0 && (v == static_cast<std::size_t>(MusicVoice::Bass) || v == static_cast<std::size_t>(MusicVoice::Stab)))
                                CHECK(h.note >= 30);
                            if (h.velocity > 0 && h.length > 0) CHECK(h.length <= kMusicMaxSteps);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("beat music: the bars the loop breaks keep the kick and lay the rest out by their own beats") {
    // 5/4: five beats. 3/4. 7/8 in a quarter-note phase (the last beat one eighth long); 7/8 in a compound phase.
    MusicPattern p;
    planBeatMusic(bar(10, 2, 1, 2, 0.5f), p);
    CHECK(p.steps == 20);
    CHECK(p.at(0, MusicVoice::Kick).velocity > 0);
    CHECK(p.at(4, MusicVoice::Snare).velocity > 0);  // the second beat
    CHECK(p.at(12, MusicVoice::Snare).velocity > 0); // the fourth
    CHECK(p.at(8, MusicVoice::Snare).velocity == 0);
    planBeatMusic(bar(6, 2, 0, 2, 0.1f), p);
    CHECK(p.steps == 12);
    CHECK(p.at(4, MusicVoice::Snare).velocity > 0);
    planBeatMusic(bar(7, 2, 1, 2, 0.5f), p);
    CHECK(p.steps == 14);
    CHECK(p.at(4, MusicVoice::Snare).velocity > 0);
    CHECK(p.at(12, MusicVoice::Snare).velocity == 0); // the short last beat takes no backbeat
    CHECK(p.at(0, MusicVoice::Kick).velocity > 0);
    planBeatMusic(bar(6, 3, 1, 2, 0.5f), p); // 6/8: two beats of six sixteenths
    CHECK(p.at(6, MusicVoice::Snare).velocity > 0);
    planBeatMusic(bar(7, 3, 1, 2, 0.5f), p); // 7/8 in 6/8: the second beat whole, the third one eighth
    CHECK(p.steps == 14);
    CHECK(p.at(6, MusicVoice::Snare).velocity > 0);
    CHECK(p.at(12, MusicVoice::Snare).velocity == 0);
}

TEST_CASE("beat music: a silent bar is empty, and an empty beat is left empty") {
    MusicBarContext c = bar(8, 2, 1, 2, 0.8f);
    c.finisher = true;
    c.entry = true;
    c.silent = true;
    MusicPattern p;
    planBeatMusic(c, p);
    CHECK(p.hitCount() == 0);
    CHECK(p.steps == 16);

    c = bar(12, 3, 1, 2, 0.8f);
    c.emptyBeats = 1u << 1; // 6/8 in 12/8: beat 2 empty
    planBeatMusic(c, p);
    for (int s = 6; s < 12; ++s)
        for (std::size_t v = 0; v < kMusicVoices; ++v) {
            if (v == static_cast<std::size_t>(MusicVoice::Riser) || v == static_cast<std::size_t>(MusicVoice::Impact)) continue;
            CHECK(p.hits[static_cast<std::size_t>(s)][v].velocity == 0);
        }
    CHECK(p.at(0, MusicVoice::Kick).velocity > 0);
    CHECK(p.at(12, MusicVoice::Kick).velocity > 0);
}

TEST_CASE("beat music: the intensity follows the phase and the boss's hp") {
    MusicPattern first, mid, late;
    planBeatMusic(bar(8, 2, 0, 2, 0.0f), first);
    planBeatMusic(bar(8, 2, 0, 2, 0.95f), mid);
    planBeatMusic(bar(8, 2, 1, 2, 0.9f), late);
    CHECK(first.tier == 0);
    CHECK(mid.tier == 1);
    CHECK(late.tier == 3);
    CHECK(first.hitCount() < mid.hitCount());
    CHECK(mid.hitCount() < late.hitCount());
    CHECK(first.velocitySum() < late.velocitySum());
    // Sparse: no stabs, no open hat, no sixteenth hats.
    CHECK(voiceHits(first, MusicVoice::Stab) == 0);
    CHECK(voiceHits(first, MusicVoice::HatOpen) == 0);
    CHECK(voiceHits(first, MusicVoice::Bass) == 1);
    CHECK(voiceHits(late, MusicVoice::Stab) > 0);
    CHECK(voiceHits(late, MusicVoice::HatClosed) > voiceHits(first, MusicVoice::HatClosed));
    // Four phases: one tier each.
    for (int phase = 0; phase < 4; ++phase) CHECK(musicTier(bar(8, 2, phase, 4, 0.3f)) == phase);
    // A single phase climbs with its hp.
    CHECK(musicTier(bar(8, 2, 0, 1, 0.0f)) == 0);
    CHECK(musicTier(bar(8, 2, 0, 1, 0.9f)) == 3);
}

TEST_CASE("beat music: the bar before a finisher builds, and the finisher bar hits hardest") {
    MusicBarContext c = bar(8, 2, 1, 2, 0.6f);
    MusicPattern plain, build, fin;
    planBeatMusic(c, plain);
    c.nextFinisher = true;
    planBeatMusic(c, build);
    c.nextFinisher = false;
    c.finisher = true;
    c.bar = 0;
    planBeatMusic(c, fin);

    CHECK(voiceHits(plain, MusicVoice::Riser) == 0);
    REQUIRE(build.at(0, MusicVoice::Riser).velocity > 0);
    CHECK(build.at(0, MusicVoice::Riser).length == 16); // the whole bar
    // The roll: a snare on every sixteenth of the last half bar, each harder than the one before.
    int last = 0;
    for (int s = 8; s < 16; ++s) {
        const int v = build.at(s, MusicVoice::Snare).velocity;
        CHECK(v > last);
        last = v;
        CHECK(build.at(s, MusicVoice::Bass).velocity == 0);
        CHECK(build.at(s, MusicVoice::Stab).velocity == 0);
    }
    CHECK(last >= 120);
    CHECK(build.at(0, MusicVoice::Kick).velocity > 0);
    // The build's chord is the dominant (A), the finisher's the tonic (D).
    CHECK(build.at(0, MusicVoice::Bass).note == 33);
    CHECK(fin.at(0, MusicVoice::Bass).note == 38);

    // The finisher bar: a kick on every beat, an impact, a crash, the most velocity of any bar of its shape.
    for (int beat = 0; beat < 4; ++beat) CHECK(fin.at(beat * 4, MusicVoice::Kick).velocity >= 104);
    CHECK(fin.at(0, MusicVoice::Impact).velocity == 127);
    CHECK(fin.at(0, MusicVoice::HatOpen).velocity == 127);
    CHECK(fin.at(0, MusicVoice::HatOpen).aux == 1);
    CHECK(fin.at(4, MusicVoice::Snare).velocity == 127);
    for (int phase = 0; phase < 2; ++phase) {
        for (int p10 = 0; p10 <= 9; p10 += 3) {
            MusicBarContext other = bar(8, 2, phase, 2, static_cast<float>(p10) / 10.0f);
            other.entry = true;
            MusicPattern o;
            planBeatMusic(other, o);
            CHECK(o.velocitySum() < fin.velocitySum());
            other.nextFinisher = true;
            planBeatMusic(other, o);
            CHECK(o.velocitySum() < fin.velocitySum());
        }
    }
}

TEST_CASE("beat music: told moves get an accent hit and the music leaves their step") {
    MusicBarContext c = bar(8, 2, 1, 2, 0.8f);
    MusicPattern base, p;
    planBeatMusic(c, base);
    REQUIRE(base.at(6, MusicVoice::HatClosed).velocity + base.at(6, MusicVoice::Bass).velocity > 0);
    c.accent[6] = 3;  // a bell on step 6
    c.accent[10] = 1; // a thump
    planBeatMusic(c, p);
    CHECK(p.at(6, MusicVoice::Accent).velocity == 127);
    CHECK(p.at(6, MusicVoice::Accent).aux == 2);
    CHECK(p.at(10, MusicVoice::Accent).aux == 0);
    for (const int s : {6, 10}) {
        CHECK(p.at(s, MusicVoice::HatClosed).velocity == 0);
        CHECK(p.at(s, MusicVoice::HatOpen).velocity == 0);
        CHECK(p.at(s, MusicVoice::Stab).velocity == 0);
        CHECK(p.at(s, MusicVoice::Bass).velocity == 0);
    }
    CHECK(voiceHits(p, MusicVoice::Accent) == 2);
    // The rest of the bar is as it was.
    CHECK(p.at(0, MusicVoice::Kick).velocity == base.at(0, MusicVoice::Kick).velocity);
    CHECK(p.at(4, MusicVoice::Snare).velocity == base.at(4, MusicVoice::Snare).velocity);
}

TEST_CASE("beat music: the same bar gives the same pattern, and the harmony moves every bar or two") {
    MusicBarContext c = bar(8, 2, 1, 3, 0.4f);
    c.accent[5] = 2;
    MusicPattern a, b;
    planBeatMusic(c, a);
    planBeatMusic(c, b);
    CHECK(samePattern(a, b));
    // Dirty the output first: planning fills every field.
    std::memset(&b, 0xAB, sizeof(b));
    planBeatMusic(c, b);
    CHECK(samePattern(a, b));

    const auto bassOf = [](int phase, int barNumber) {
        MusicBarContext x = bar(8, 2, phase, 4, 0.5f);
        x.bar = static_cast<std::uint16_t>(barNumber);
        MusicPattern p;
        planBeatMusic(x, p);
        return static_cast<int>(p.at(0, MusicVoice::Bass).note);
    };
    // Phase 0 changes chord every two bars, phase 1 every bar; the loop of four comes round.
    CHECK(bassOf(0, 1) == bassOf(0, 2));
    CHECK(bassOf(0, 2) != bassOf(0, 3));
    CHECK(bassOf(1, 1) != bassOf(1, 2));
    CHECK(bassOf(1, 2) != bassOf(1, 3));
    CHECK(bassOf(1, 1) == bassOf(1, 5));
}

TEST_CASE("beat music: planning and the synth allocate nothing") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    BeatMusicSynth synth;
    synth.init(kRate);
    std::vector<float> buffer(4096);
    MusicPattern p;
    MusicBarContext c = bar(8, 2, 1, 2, 0.5f);
    c.accent[5] = 2;
    int sink = 0;
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (int round = 0; round < 40; ++round) {
        c.nextFinisher = round % 3 == 0;
        c.phaseProgress = static_cast<float>(round % 10) / 10.0f;
        planBeatMusic(c, p);
        sink += p.hitCount();
        for (int s = 0; s < p.steps; ++s) {
            for (std::size_t v = 0; v < kMusicVoices; ++v) {
                const MusicHit& h = p.hits[static_cast<std::size_t>(s)][v];
                if (h.velocity == 0) continue;
                const auto sound = static_cast<MusicSound>(v < static_cast<std::size_t>(MusicVoice::Accent) ? v : static_cast<std::size_t>(MusicSound::AccentBell));
                synth.play(synth.position() + static_cast<std::uint64_t>(s) * 500, sound, h.velocity, h.note, h.aux, 6000);
            }
        }
        synth.render(buffer.data(), static_cast<int>(buffer.size()));
        synth.revokeFrom(synth.position() + 100);
        synth.cut(synth.position() + 4000);
        sink += static_cast<int>(synth.pending());
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink > 0);
}

// ---- the synth ----

TEST_CASE("beat music synth: a sound starts on its frame, can be revoked, and a cut fades it out") {
    BeatMusicSynth synth;
    synth.init(kRate);
    std::vector<float> out(48000, 0.0f);
    synth.play(1234, MusicSound::Kick, 127);
    synth.play(30000, MusicSound::Snare, 127);
    synth.play(30010, MusicSound::HatClosed, 100);
    synth.revokeFrom(30000);
    CHECK(synth.pending() == 1);
    synth.render(out.data(), 4000);
    std::size_t first = 0;
    while (first < 4000 && out[first] == 0.0f) ++first;
    CHECK(first == 1234);
    CHECK(synth.position() == 4000);
    // A cut at 10000 fades a long sound out within a few milliseconds and drops what is due after it.
    synth.play(5000, MusicSound::Impact, 127);
    synth.play(12000, MusicSound::Kick, 127);
    synth.cut(10000);
    synth.render(out.data() + 4000, 20000);
    CHECK(peakOf(out, 10000 + 700, 24000) == 0.0f);
    CHECK(peakOf(out, 5000, 10000) > 0.1f);
    // Late: a sound due before the position is started at once and counted.
    synth.play(10, MusicSound::Kick, 100);
    CHECK(synth.late() == 1);
    // Velocity 0 is nothing.
    synth.play(synth.position() + 10, MusicSound::Kick, 0);
    CHECK(synth.pending() == 1);
}

TEST_CASE("beat music synth: no sound clips alone, and everything at once stays under full scale") {
    for (int s = 0; s < static_cast<int>(MusicSound::Count); ++s) {
        BeatMusicSynth synth;
        synth.init(kRate);
        std::vector<float> out(kRate * 3, 0.0f);
        synth.play(100, static_cast<MusicSound>(s), 127, 38, 1, 24000);
        synth.render(out.data(), kRate * 3);
        INFO("sound " << s);
        CHECK(peakOf(out) > 0.02f);
        CHECK(peakOf(out) < 1.0f);
    }
    BeatMusicSynth synth;
    synth.init(kRate);
    std::vector<float> out(kRate, 0.0f);
    for (int s = 0; s < static_cast<int>(MusicSound::Count); ++s) {
        synth.play(100, static_cast<MusicSound>(s), 127, 38, 0, 24000);
        synth.play(100, static_cast<MusicSound>(s), 127, 50, 3, 24000);
    }
    synth.render(out.data(), kRate);
    CHECK(peakOf(out) < 1.0f);
    CHECK(synth.dropped() == 0);
}

// ---- the driver, against a whole fight ----

TEST_CASE("beat music: upcomingBar names the bar the next bar line plays") {
    const BeatBossTrack track = loadTrack();
    BeatBoss f;
    f.begin(track.def, BeatBossTuning{}, 0.0);
    BossBarPreview up = f.upcomingBar();
    CHECK(up.newPhase);
    CHECK(up.phase == 0);
    int checked = 0;
    for (double now = 1.0 / 60.0; !f.over() && now < 120.0; now += 1.0 / 60.0) {
        for (int guard = 0; guard < 64; ++guard) {
            const QteAction a = f.autopilot(now);
            if (a.kind == QteAction::Kind::None) break;
            if (a.kind == QteAction::Kind::Press) f.press(a.input, a.time);
            else f.release(a.input, a.time);
        }
        const std::uint32_t bars = static_cast<std::uint32_t>(f.stats().bars);
        const BossBarPreview before = f.upcomingBar();
        const bool running = f.trackRunning();
        f.update(now);
        if (static_cast<std::uint32_t>(f.stats().bars) != bars + 1 || !running) continue;
        // One bar line was crossed this frame: what the preview said is what is playing (a new bar can only
        // differ if the boss's hp changed in between, which a frame's update can do; the last frame's view stands).
        if (f.over() || !f.trackRunning()) continue;
        if (before.finisher == f.inFinisher() && !before.newPhase) {
            if (!before.finisher) CHECK(before.bar == f.bar() - 1);
            CHECK(before.phase == f.phase());
            ++checked;
        }
    }
    CHECK(checked >= 8);
}

TEST_CASE("beat music: an autoplayed fight is scheduled on time and on the bar grid") {
    const BeatBossTrack track = loadTrack();
    const Render r = renderFight(track, RenderOptions{});
    REQUIRE(r.won);
    CHECK(r.dropped == 0);
    CHECK(r.late <= 4); // the count's first blips, which a frame can only learn of as they sound

    // Every bar's downbeat has its kick on the bar line, to the microsecond, and every music sound sits on a
    // sixteenth of the bar it is in.
    const std::vector<double> lines = musicBarLines(r);
    REQUIRE(lines.size() >= 15);
    int kicks = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        INFO("bar line " << i << " at " << lines[i]);
        CHECK(playedAt(r, MusicSound::Kick, lines[i]) != nullptr);
        ++kicks;
    }
    CHECK(kicks == static_cast<int>(lines.size()));
    for (const MusicEvent& e : r.played) {
        if (!isMusic(e.sound) || e.time < lines.front() - 1e-9) continue;
        double start = lines.front();
        for (const double l : lines)
            if (l <= e.time + 1e-6) start = l;
        // Both phases of the test track have a sixteenth of 0.125 s.
        const double steps = (e.time - start) / 0.125;
        INFO("sound " << static_cast<int>(e.sound) << " at " << e.time << " in the bar from " << start);
        CHECK(std::fabs(steps - std::round(steps)) < 1e-6);
    }
}

TEST_CASE("beat music: a silence cuts the music and leaves the count, then the track comes back with an impact") {
    const BeatBossTrack track = loadTrack();
    const Render r = renderFight(track, RenderOptions{});
    REQUIRE(r.won);
    int silences = 0;
    for (std::size_t i = 0; i < r.fightEvents.size(); ++i) {
        const BeatBossEvent& s = r.fightEvents[i];
        if (s.type != BeatBossEventType::SilenceStart) continue;
        ++silences;
        double resumed = -1.0;
        for (std::size_t k = i + 1; k < r.fightEvents.size(); ++k)
            if (r.fightEvents[k].type == BeatBossEventType::Resumed) {
                resumed = r.fightEvents[k].time;
                break;
            }
        REQUIRE(resumed > s.time);
        const double over = s.time + s.amount * 0.001; // the count starts
        int counts = 0;
        for (const MusicEvent& e : r.played) {
            if (e.time < s.time - 1e-9 || e.time >= resumed - 1e-9) continue;
            // Between the silence and the track's return: the count's blips and nothing else.
            INFO("sound " << static_cast<int>(e.sound) << " at " << e.time << " in the silence from " << s.time);
            CHECK((e.sound == MusicSound::CountClick || e.sound == MusicSound::CountVoice));
            CHECK(e.time >= over - 1e-6);
            ++counts;
        }
        // The count back is the silence's own: clicks, or nothing for a "silent" count.
        bool silentCount = false;
        for (std::size_t p = 0; p < track.def.phaseCount; ++p) {
            const BarMap& phase = track.def.phases[p];
            for (std::size_t k = 0; k < phase.silenceCount; ++k)
                if (std::fabs(phase.silences[k].seconds * 1000.0 - s.amount) <= 1.0) silentCount = phase.silences[k].count == BeatCountKind::Silent;
        }
        CHECK((silentCount ? counts == 0 : counts >= 1));
        // Nothing of the old bar rings on after the line, and the bar that returns opens with an impact.
        CHECK(playedAt(r, MusicSound::Impact, resumed) != nullptr);
        CHECK(playedAt(r, MusicSound::Kick, resumed) != nullptr);
        // The audio itself is quiet through the silence's first part.
        const std::size_t from = sampleOf(s.time) + 3000;
        const std::size_t to = sampleOf(over) - 1;
        if (to > from) CHECK(peakOf(r.audio, from, to) < 0.001f);
    }
    CHECK(silences >= 2);
}

TEST_CASE("beat music: the bar before a finisher has its riser and snare roll, and the finisher bar its impact") {
    const BeatBossTrack track = loadTrack();
    const Render r = renderFight(track, RenderOptions{});
    REQUIRE(r.won);
    int finishers = 0;
    const std::vector<double> lines = musicBarLines(r);
    for (std::size_t i = 0; i < r.fightEvents.size(); ++i) {
        const BeatBossEvent& f = r.fightEvents[i];
        if (f.type != BeatBossEventType::FinisherStart) continue;
        ++finishers;
        const double fin = f.time;
        INFO("finisher at " << fin);
        CHECK(playedAt(r, MusicSound::Impact, fin) != nullptr);
        // (The last phase's finisher opens as the boss falls, and the victory takes its bar.)
        if (fin < r.endTime - 1.0) CHECK(playedAt(r, MusicSound::Kick, fin) != nullptr);
        // The bar before it: a riser inside it and snares crescendoing through its last half.
        double prev = -1.0;
        for (const double l : lines)
            if (l < fin - 1e-9) prev = l;
        REQUIRE(prev >= 0.0);
        const double len = fin - prev;
        const MusicEvent* riser = nullptr;
        for (const MusicEvent& e : r.played)
            if (e.sound == MusicSound::Riser && e.time >= prev - 1e-6 && e.time < fin) riser = &e;
        REQUIRE(riser != nullptr);
        CHECK(riser->time + riser->length >= fin - 0.2);
        int rolls = 0;
        int last = 0;
        for (const MusicEvent& e : r.played)
            if (e.sound == MusicSound::Snare && e.time >= prev + len / 2.0 - 1e-6 && e.time < fin) {
                CHECK(e.velocity >= last);
                last = e.velocity;
                ++rolls;
            }
        // The finisher opens when a hit takes the boss into its band, which can be late in the bar: the build
        // joins then, with whatever is left of the bar.
        CHECK(rolls >= 3);
        CHECK(last >= 120);
    }
    CHECK(finishers >= 1);
}

TEST_CASE("beat music: the render is deterministic, does not clip, and the move accents stand out") {
    const BeatBossTrack track = loadTrack();
    const Render a = renderFight(track, RenderOptions{});
    const Render b = renderFight(track, RenderOptions{});
    REQUIRE(a.won);
    REQUIRE(a.audio.size() == b.audio.size());
    CHECK(std::memcmp(a.audio.data(), b.audio.data(), a.audio.size() * sizeof(float)) == 0);
    REQUIRE(a.played.size() == b.played.size());

    // The whole fight: loud enough, never at full scale.
    const float peak = peakOf(a.audio);
    const double loudness = rmsOf(a.audio, sampleOf(a.endTime * 0.2), sampleOf(a.endTime * 0.9));
    MESSAGE("peak " << peak << ", rms " << loudness << ", " << a.played.size() << " sounds, " << a.revoked << " revoked, "
                    << a.late << " late");
    CHECK(peak < 0.99f);
    CHECK(peak > 0.5f);
    CHECK(loudness > 0.05);

    // An accent over the music: its first 60 ms carry clearly more than the same stretch without accents.
    RenderOptions quiet;
    quiet.noAccents = true;
    const Render without = renderFight(track, quiet);
    REQUIRE(without.won);
    int accents = 0;
    int clear = 0;
    for (const MusicEvent& e : a.played) {
        if (!isAccentSound(e.sound)) continue;
        const std::size_t from = sampleOf(e.time);
        const std::size_t to = from + 2880;
        if (to >= a.audio.size() || to >= without.audio.size()) continue;
        ++accents;
        const double with = rmsOf(a.audio, from, to);
        const double bare = rmsOf(without.audio, from, to);
        if (with > 1.25 * bare && with > 0.1) ++clear;
    }
    MESSAGE(accents << " accents, " << clear << " clear over the music");
    CHECK(accents >= 20);
    CHECK(clear * 10 >= accents * 9);

    const std::string dir = renderDir();
    if (!dir.empty()) {
        const std::size_t thirty = std::min<std::size_t>(a.audio.size(), 30 * kRate);
        CHECK(writeWav(dir + "/test_track_30s.wav", a.audio.data(), thirty));
        CHECK(writeWav(dir + "/test_track_full.wav", a.audio.data(), a.audio.size()));
        RenderOptions accentsOnly;
        accentsOnly.noMusic = true;
        const Render cues = renderFight(track, accentsOnly);
        CHECK(writeWav(dir + "/test_track_cues_only.wav", cues.audio.data(), cues.audio.size()));
    }
}

TEST_CASE("beat music: the fight is scored at other frame rates, with nothing dropped and little late") {
    const BeatBossTrack track = loadTrack();
    for (const double dt : {1.0 / 30.0, 1.0 / 144.0, 1.0 / 240.0}) {
        RenderOptions o;
        o.dt = dt;
        const Render r = renderFight(track, o);
        INFO("dt " << dt);
        CHECK(r.won);
        CHECK(r.dropped == 0);
        CHECK(r.late <= 6);
        CHECK(peakOf(r.audio) < 0.99f);
        const std::vector<double> lines = musicBarLines(r);
        for (const double l : lines) CHECK(playedAt(r, MusicSound::Kick, l) != nullptr);
    }
}

TEST_CASE("beat music: a lost fight cuts the music, and a retry starts it again") {
    const BeatBossTrack track = loadTrack();
    BeatBoss fight;
    BeatMusicDriver driver;
    BeatLine line;
    MusicEvents ev;
    fight.begin(track.def, BeatBossTuning{}, 0.0);
    driver.reset(0.0, ev);
    int plays = 0;
    bool cutAfterDefeat = false;
    double now = 0.0;
    for (; !fight.over() && now < 200.0; now += 1.0 / 60.0) {
        for (const BeatBossEvent& e : fight.update(now)) driver.onEvent(e);
        fight.beatLine(line);
        driver.update(fight, line, now, BeatCountKind::Click, ev);
        for (const MusicEvent& e : ev) {
            plays += e.kind == MusicEventKind::Play ? 1 : 0;
            cutAfterDefeat |= fight.over() && e.kind == MusicEventKind::Cut;
        }
    }
    REQUIRE(fight.state() == BeatBossState::Defeat);
    CHECK(plays > 50);
    CHECK(cutAfterDefeat);
    driver.update(fight, line, now + 0.1, BeatCountKind::Click, ev);
    CHECK(ev.empty());

    fight.retry(now);
    driver.reset(now, ev);
    CHECK(ev.size() == 2);
    int after = 0;
    for (double t = now; t < now + 12.0; t += 1.0 / 60.0) {
        for (const BeatBossEvent& e : fight.update(t)) driver.onEvent(e);
        fight.beatLine(line);
        driver.update(fight, line, t, BeatCountKind::Click, ev);
        for (const MusicEvent& e : ev) after += e.kind == MusicEventKind::Play && e.sound == MusicSound::Kick ? 1 : 0;
    }
    CHECK(after >= 4);
}

TEST_CASE("beat music: driving a whole fight allocates nothing") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const BeatBossTrack track = loadTrack();
    BeatBoss fight;
    BeatMusicDriver driver;
    BeatMusicSynth synth;
    synth.init(kRate);
    MusicEvents ev;
    BeatLine line;
    std::vector<float> buffer(2048);
    int sink = 0;
    const std::uint64_t before = ftcore_test::allocStats().count;
    fight.begin(track.def, BeatBossTuning{}, 0.0);
    driver.reset(0.0, ev);
    for (double now = 1.0 / 60.0; !fight.over() && now < 120.0; now += 1.0 / 60.0) {
        for (int guard = 0; guard < 16; ++guard) {
            const QteAction a = fight.autopilot(now);
            if (a.kind == QteAction::Kind::None) break;
            const BeatBossEvents e = a.kind == QteAction::Kind::Press ? fight.press(a.input, a.time) : fight.release(a.input, a.time);
            for (const BeatBossEvent& x : e) driver.onEvent(x);
        }
        for (const BeatBossEvent& e : fight.update(now)) driver.onEvent(e);
        fight.beatLine(line);
        driver.update(fight, line, now, BeatCountKind::Click, ev);
        for (const MusicEvent& e : ev) {
            if (e.kind == MusicEventKind::Play) synth.play(sampleOf(e.time), e.sound, e.velocity, e.note, e.aux, static_cast<std::uint32_t>(e.length * kRate));
            else if (e.kind == MusicEventKind::Cut) synth.cut(sampleOf(e.time));
            else synth.revokeFrom(sampleOf(e.time));
        }
        const std::uint64_t target = sampleOf(now);
        while (synth.position() < target) {
            const int n = static_cast<int>(std::min<std::uint64_t>(buffer.size(), target - synth.position()));
            synth.render(buffer.data(), n);
            sink += static_cast<int>(buffer[0] * 1000.0f);
        }
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(fight.state() == BeatBossState::Victory);
    (void)sink;
}

TEST_CASE("beat music: the kicks in the audio start on the bar lines, to the sample") {
    const BeatBossTrack track = loadTrack();
    RenderOptions o;
    o.onlyKicks = true;
    const Render r = renderFight(track, o);
    REQUIRE(r.won);
    const std::vector<double> lines = musicBarLines(r);
    REQUIRE(lines.size() >= 15);
    int exact = 0;
    for (const double l : lines) {
        const std::size_t at = sampleOf(l);
        // A kick opens with a click, a jump from one sample to the next that a sine's tail never makes: the first
        // such jump near the line is on the line itself (a click that happens to start near zero is a sample or two on).
        std::size_t first = at - 40;
        while (first <= at + 10 && std::fabs(r.audio[first] - r.audio[first - 1]) < 0.04f) ++first;
        CHECK(first >= at);
        if (first == at) ++exact;
    }
    CHECK(exact * 10 >= static_cast<int>(lines.size()) * 8);
}

TEST_CASE("beat music: a bar keeps the tier it was planned at, whatever the hp does in the middle of it") {
    // A forced tier overrides the hp, and a finisher is always the top.
    MusicBarContext ctx;
    ctx.phase = 0;
    ctx.phaseCount = 2;
    ctx.phaseProgress = 0.0f;
    CHECK(musicTier(ctx) == 0);
    ctx.forcedTier = 2;
    CHECK(musicTier(ctx) == 2);
    MusicPattern p;
    planBeatMusic(ctx, p);
    CHECK(p.tier == 2);
    ctx.finisher = true;
    CHECK(musicTier(ctx) == 3);

    // A whole fight: the tier never changes between the first and the last frame of a bar (the boss loses hp
    // all the way through them), changes between bars, and climbs through every tier on the way.
    for (const double dt : {1.0 / 60.0, 1.0 / 30.0}) {
        const BeatBossTrack track = loadTrack();
        BeatBoss fight;
        BeatMusicDriver driver;
        MusicEvents ev;
        BeatLine line;
        fight.begin(track.def, BeatBossTuning{}, 0.0);
        driver.reset(0.0, ev);
        int bars = -1;
        int tier = -1;
        int changes = 0;
        int hpDuringBar = 0; // bars in which the boss lost hp after the tier was taken
        int bossHpAtBar = 0;
        bool lostHp = false;
        std::array<bool, 4> seen{};
        int frames = 0;
        for (double now = dt; !fight.over() && now < 120.0; now += dt) {
            for (int guard = 0; guard < 16; ++guard) {
                const QteAction a = fight.autopilot(now);
                if (a.kind == QteAction::Kind::None) break;
                for (const BeatBossEvent& x : a.kind == QteAction::Kind::Press ? fight.press(a.input, a.time) : fight.release(a.input, a.time)) driver.onEvent(x);
            }
            for (const BeatBossEvent& e : fight.update(now)) driver.onEvent(e);
            fight.beatLine(line);
            driver.update(fight, line, now, BeatCountKind::Click, ev);
            if (!fight.trackRunning() || driver.tier() < 0) continue;
            ++frames;
            if (fight.stats().bars != bars) {
                if (lostHp) ++hpDuringBar;
                bars = fight.stats().bars;
                if (driver.tier() != tier) ++changes;
                tier = driver.tier();
                bossHpAtBar = fight.bossHp();
                lostHp = false;
                seen[static_cast<std::size_t>(tier)] = true;
            } else {
                CHECK_MESSAGE(driver.tier() == tier, "bar " << bars << " at " << now);
                if (fight.bossHp() < bossHpAtBar) lostHp = true;
            }
        }
        CHECK(fight.state() == BeatBossState::Victory);
        CHECK(frames > 500);
        CHECK(hpDuringBar >= 3); // the boss really was hit inside bars that kept their tier
        CHECK(changes >= 3);
        for (int k = 0; k < 4; ++k) CHECK_MESSAGE(seen[static_cast<std::size_t>(k)], "tier " << k);
    }
}
