#include "ftcore/BeatFight.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

// The beat fight (docs/COMBAT_SYSTEMS.md): patterns, the loader, the windows, the schedule, judging,
// the chain, the finisher, the end states, determinism, balance and allocation.
//
// Most tests run at 120 bpm: a beat is 0.5 s and an eighth 0.25 s. A fight begun at 0 with the default
// one-bar lead-in has its first bar line at beat 4 (2.0 s), so slot s of bar b is at 2.0 + 2.0 b + 0.25 s,
// and with approachBeats 2 a bar is generated 1.0 s before it starts. The default windows: a strike is
// Perfect within 0.05 s of its eighth and Good within 0.12 s; a counter's impact is 0.06 s after its
// eighth, its parry window is the 0.12 s before that (Perfect) and its dodge window the 0.30 s before
// that (Good), so it opens 0.24 s before the eighth.

namespace {

// ---- helpers ----

// The beat.json spelling of a pattern, for round trips.
std::string formatPattern(const BeatPattern& p) {
    std::string out;
    for (const BeatSlot& s : p.slots) {
        if (!out.empty()) out += ' ';
        const char arrow = "LDUR"[static_cast<int>(s.input)];
        switch (s.kind) {
        case BeatKind::Rest: out += '.'; break;
        case BeatKind::Strike: out += 's'; out += arrow; break;
        case BeatKind::Counter: out += 'c'; out += arrow; break;
        case BeatKind::Hold: out += 'h'; out += arrow; out += std::to_string(s.length); break;
        case BeatKind::Held: out += '-'; break;
        }
    }
    return out;
}

BeatPattern patternOf(const char* text) {
    BeatPattern p;
    std::string error;
    REQUIRE_MESSAGE(parseBeatPattern(text, p, &error), error);
    return p;
}

struct EnemySpec {
    int hp = 60;
    int damage = 10;
    std::vector<const char*> patterns;
};

BeatFightDef defOf(std::initializer_list<EnemySpec> enemies, double bpm = 120.0) {
    BeatFightDef def;
    def.bpm = bpm;
    def.enemyCount = static_cast<std::uint8_t>(enemies.size());
    std::size_t i = 0;
    for (const EnemySpec& spec : enemies) {
        BeatEnemy& e = def.enemies[i++];
        e.hp = spec.hp;
        e.damage = spec.damage;
        e.patternCount = static_cast<std::uint8_t>(spec.patterns.size());
        for (std::size_t k = 0; k < spec.patterns.size(); ++k) e.patterns[k] = patternOf(spec.patterns[k]);
    }
    return def;
}

// One enemy with these patterns.
BeatFightDef oneEnemy(std::initializer_list<const char*> patterns, int hp = 60, int damage = 10) {
    EnemySpec spec;
    spec.hp = hp;
    spec.damage = damage;
    spec.patterns.assign(patterns.begin(), patterns.end());
    return defOf({spec});
}

// The default tuning with the finisher off, so a test sees only its pattern prompts.
BeatTuning plain() {
    BeatTuning t;
    t.finisherAt = 0.0;
    return t;
}

// A fight of one enemy (a lot of hp) against a lead with a lot of hp, begun at 0 and updated to the
// moment the first bar is generated (1.0 s), so its prompts are open.
BeatFight beginFight(std::initializer_list<const char*> patterns, const BeatTuning& tuning = plain(),
                     const QteAssist& assist = {}, std::uint32_t seed = 1) {
    BeatFightDef def = oneEnemy(patterns, 1000, 10);
    def.hp = 1000;
    BeatFight f;
    f.begin(def, tuning, 0.0, seed, assist);
    f.update(1.0);
    return f;
}

int countOf(const BeatEvents& e, BeatEventType type) {
    int n = 0;
    for (const BeatEvent& ev : e) n += ev.type == type ? 1 : 0;
    return n;
}

// The nth event of a type (null when there are fewer). The pointer lives as long as `e`.
const BeatEvent* findEvent(const BeatEvents& e, BeatEventType type, int nth = 0) {
    for (const BeatEvent& ev : e)
        if (ev.type == type && nth-- == 0) return &ev;
    return nullptr;
}

// Every event a test collected, over many calls.
struct Record {
    std::vector<BeatEvent> events;

    void add(const BeatEvents& e) { events.insert(events.end(), e.begin(), e.end()); }
    int count(BeatEventType type) const {
        int n = 0;
        for (const BeatEvent& ev : events) n += ev.type == type ? 1 : 0;
        return n;
    }
};

bool sameEvent(const BeatEvent& a, const BeatEvent& b) {
    return a.type == b.type && a.enemy == b.enemy && a.prompt == b.prompt && a.grade == b.grade && a.kind == b.kind &&
           a.amount == b.amount && a.time == b.time;
}

bool sameStats(const BeatStats& a, const BeatStats& b) {
    return a.perfects == b.perfects && a.goods == b.goods && a.misses == b.misses && a.strays == b.strays &&
           a.whiffs == b.whiffs && a.ignored == b.ignored && a.counters == b.counters && a.maxChain == b.maxChain &&
           a.damageDealt == b.damageDealt && a.damageTaken == b.damageTaken && a.finishers == b.finishers &&
           a.bars == b.bars && a.restarts == b.restarts;
}

// When bar `bar` (0 is the first) starts on the fight's clock.
double barStart(const BeatFight& f, int bar) { return f.timeAtBeat(f.firstBarBeat() + 4.0 * bar); }

// The open prompts of one bar as text ("sL0 cU4 "): kind, arrow and slot of each.
std::string barSignature(const BeatFight& f, int bar) {
    std::string s;
    const double from = barStart(f, bar);
    const double to = barStart(f, bar + 1);
    for (std::size_t i = 0; i < f.promptCount(); ++i) {
        const BeatPrompt& p = f.prompt(i);
        if (p.time < from - 1e-6 || p.time >= to - 1e-6) continue;
        s += p.kind == BeatKind::Strike ? 's' : (p.kind == BeatKind::Counter ? 'c' : 'h');
        s += "LDUR"[static_cast<int>(p.input)];
        s += std::to_string(static_cast<int>(std::lround((p.time - from) / (f.secondsPerBeat() * 0.5))));
        s += ' ';
    }
    return s;
}

// Frames of 1/60 s from the fight's clock to `to`, answering what the autopilot would, except the
// prompts `skip` picks (it gets the earliest open prompt; the autopilot waits for that one to run out).
template <class Skip>
void playFrames(BeatFight& f, Record& rec, double to, Skip skip) {
    constexpr double kFrame = 1.0 / 60.0;
    for (double now = f.now() + kFrame; !f.over() && now < to + kFrame * 0.5; now += kFrame) {
        for (int guard = 0; guard < 64; ++guard) {
            const QteAction a = f.autopilot(now);
            if (a.kind == QteAction::Kind::None) break;
            if (a.kind == QteAction::Kind::Press && skip(f.prompt(0))) break;
            rec.add(a.kind == QteAction::Kind::Press ? f.press(a.input, a.time) : f.release(a.input, a.time));
        }
        rec.add(f.update(now));
    }
}

bool noSkip(const BeatPrompt&) { return false; }

struct Outcome {
    BeatPhase phase = BeatPhase::NotStarted;
    int bars = 0;
    int hpLeft = 0;
    BeatStats stats;
};

// Plays the setup with the autopilot (frames of `frame` seconds) until the fight ends or runs past 64 bars.
Outcome playAutopilot(const BeatSetup& setup, std::uint32_t seed, double frame = 1.0 / 60.0) {
    BeatFight f;
    f.begin(setup.def, setup.tuning, 0.0, seed);
    double now = 0.0;
    for (int frames = 0; !f.over() && f.stats().bars <= 64 && frames < 400000; ++frames) {
        now += frame;
        for (int guard = 0; guard < 64; ++guard) {
            const QteAction a = f.autopilot(now);
            if (a.kind == QteAction::Kind::None) break;
            if (a.kind == QteAction::Kind::Press) f.press(a.input, a.time);
            else f.release(a.input, a.time);
        }
        f.update(now);
    }
    Outcome o;
    o.phase = f.phase();
    o.bars = f.stats().bars;
    o.hpLeft = f.hp();
    o.stats = f.stats();
    return o;
}

// The same with no input at all.
Outcome playNothing(const BeatSetup& setup, std::uint32_t seed, double frame = 1.0 / 60.0) {
    BeatFight f;
    f.begin(setup.def, setup.tuning, 0.0, seed);
    double now = 0.0;
    for (int frames = 0; !f.over() && f.stats().bars <= 64 && frames < 400000; ++frames) {
        now += frame;
        f.update(now);
    }
    Outcome o;
    o.phase = f.phase();
    o.bars = f.stats().bars;
    o.hpLeft = f.hp();
    o.stats = f.stats();
    return o;
}

} // namespace

// ---- patterns ----

TEST_CASE("beat pattern: valid patterns parse and round-trip") {
    const char* valid[] = {
        "sL . sR . cU . . .",
        "cD . . . sL . hU2 -",
        ". . . . . . . .",
        "sL sD sU sR cL cD cU cR",
        "hL8 - - - - - - -", // a hold may fill the bar...
        "hD2 - hU2 - hR2 - hL2 -",
        ". . . . . . hR2 -", // ...or end on the next bar line
        "hU3 - - cD . . sL .",
        "cR . . . hD4 - - -",
    };
    for (const char* text : valid) {
        CAPTURE(text);
        BeatPattern p;
        std::string error;
        REQUIRE_MESSAGE(parseBeatPattern(text, p, &error), error);
        CHECK(formatPattern(p) == text);
    }
}

TEST_CASE("beat pattern: slots, holds and counters") {
    const BeatPattern a = patternOf("sL . sR . cU . . .");
    CHECK(a.slots[0].kind == BeatKind::Strike);
    CHECK(a.slots[0].input == Arrow::Left);
    CHECK(a.slots[1].kind == BeatKind::Rest);
    CHECK(a.slots[2].kind == BeatKind::Strike);
    CHECK(a.slots[2].input == Arrow::Right);
    CHECK(a.slots[4].kind == BeatKind::Counter);
    CHECK(a.slots[4].input == Arrow::Up);
    CHECK(a.hasCounter());

    const BeatPattern b = patternOf("cD . . . sL . hU2 -");
    CHECK(b.slots[0].input == Arrow::Down);
    CHECK(b.slots[6].kind == BeatKind::Hold);
    CHECK(b.slots[6].input == Arrow::Up);
    CHECK(b.slots[6].length == 2);
    CHECK(b.slots[7].kind == BeatKind::Held);

    const BeatPattern c = patternOf("hL8 - - - - - - -");
    CHECK(c.slots[0].length == 8);
    for (std::size_t i = 1; i < kBeatSlots; ++i) CHECK(c.slots[i].kind == BeatKind::Held);
    CHECK_FALSE(c.hasCounter());
    CHECK_FALSE(patternOf("sL sD sU sR sL sD sU sR").hasCounter());
    CHECK_FALSE(patternOf(". . . . . . . .").hasCounter());

    // Blanks of any width and tabs separate the slots.
    CHECK(formatPattern(patternOf("  sL\t.  sR . cU . . .  ")) == "sL . sR . cU . . .");
}

TEST_CASE("beat pattern: bad patterns are errors and leave the output alone") {
    struct Bad {
        const char* text;
        const char* why; // a piece of the error text
    };
    const Bad bad[] = {
        {"", "expected 8 slots, found 0"},
        {"sL . sR", "expected 8 slots, found 3"},
        {"sL . sR . cU . . . .", "expected 8 slots, found 9"},
        {"sX . . . . . . .", "bad token"},
        {"x . . . . . . .", "bad token"},
        {"sl . . . . . . .", "bad token"},
        {"s . . . . . . .", "bad token"},
        {"sLL . . . . . . .", "bad token"},
        {"cU2 . . . . . . .", "bad token"},
        {"hUx . . . . . . .", "bad token"},
        {"hU1 . . . . . . .", "2..8"},
        {"hU0 . . . . . . .", "2..8"},
        {"hU9 - - - - - - -", "2..8"},
        {"hU10 - - - - - - -", "2..8"},
        {"hU . . . . . . .", "length"},
        {"hU2 . . . . . . .", "missing"},
        {"hU3 - . . . . . .", "missing"},
        {"hU4 - - . . . . .", "missing"},
        {"- . . . . . . .", "stray"},
        {"sL - . . . . . .", "stray"},
        {"hU2 - - . . . . .", "stray"},
        {". . . . . . . hU2", "past the bar"},
        {". . . . . . hU3 -", "past the bar"},
        {". . . . . hU8 - -", "past the bar"},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.text);
        BeatPattern p = patternOf("sD . . . . . . cU");
        std::string error;
        CHECK_FALSE(parseBeatPattern(b.text, p, &error));
        CHECK(error.find(b.why) != std::string::npos);
        CHECK(formatPattern(p) == "sD . . . . . . cU"); // untouched
        CHECK_FALSE(parseBeatPattern(b.text, p));       // no error text wanted
    }
}

TEST_CASE("beat: the built-in fight is the header's example at 120 bpm") {
    const BeatFightDef d = defaultBeatFightDef();
    CHECK(d.bpm == 120.0);
    CHECK(d.offset == 0.0);
    CHECK(d.enemyCount == 1);
    CHECK(d.hp == 100);
    CHECK(d.enemies[0].hp == 60);
    CHECK(d.enemies[0].damage == 10);
    REQUIRE(d.enemies[0].patternCount == 2);
    CHECK(formatPattern(d.enemies[0].patterns[0]) == "sL . sR . cU . . .");
    CHECK(formatPattern(d.enemies[0].patterns[1]) == "cD . . . sL . hU2 -");
    CHECK(d.enemies[0].patterns[0].hasCounter());
    CHECK(d.enemies[0].patterns[1].hasCounter());
}

// ---- the data file ----

namespace {

constexpr const char* kTable = R"J({
  "version": 1,
  "enemies": [
    { "id": "wisp", "hp": 60, "damage": 10, "patterns": [ "sL . sR . cU . . .", "cD . . . sL . hU2 -" ] },
    { "id": "brute", "hp": 140, "damage": 22, "patterns": [ "cL . . . cR . . ." ] }
  ],
  "default": { "bpm": 100, "offset": 0.5, "hp": 80, "strikeDamage": 12, "counterDamage": 16, "holdDamage": 5,
               "enemies": ["wisp"], "music": "",
               "strike": { "perfect": 0.04, "good": 0.1 },
               "counter": { "dodge": 0.28, "parry": 0.1, "parryLockout": 0.3, "late": 0.15 },
               "holdSlack": 0.1 },
  "fights": [
    { "scene": "SC-T1", "beat": 4, "step": 1, "bpm": 96, "offset": 0.42,
      "enemies": ["wisp", "brute", "wisp"], "music": "cue_a" },
    { "scene": "SC-T1", "beat": 4, "hp": 50 }
  ]
})J";

// A table file around pieces of JSON (each without its trailing comma).
std::string tableJson(const std::string& enemies, const std::string& def, const std::string& fights = "[]") {
    return "{ \"version\": 1, \"enemies\": " + enemies + ", \"default\": " + def + ", \"fights\": " + fights + " }";
}

const char* const kTwoEnemies = R"J([ { "id": "wisp", "hp": 60, "damage": 10, "patterns": [ "sL . sR . cU . . .", "cD . . . sL . hU2 -" ] },
                                      { "id": "brute", "hp": 140, "damage": 22, "patterns": [ "cL . . . cR . . ." ] } ])J";
const char* const kPlainDefault = R"J({ "bpm": 120, "enemies": ["wisp"] })J";

bool hasWarning(const std::vector<std::string>& warnings, const std::string& piece) {
    for (const std::string& w : warnings)
        if (w.find(piece) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("beat table: a fixture loads; a fight inherits the default fight's keys; find falls back to it") {
    BeatTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importBeatTable(kTable, t, &error, &warnings), error);
    CHECK(warnings.empty());

    REQUIRE(t.enemyIds.size() == 2);
    CHECK(t.enemyIds[0] == "wisp");
    CHECK(t.enemyIds[1] == "brute");
    REQUIRE(t.enemies.size() == 2);
    CHECK(t.enemies[0].hp == 60);
    CHECK(t.enemies[0].patternCount == 2);
    CHECK(t.enemies[1].damage == 22);
    CHECK(t.enemies[1].patternCount == 1);

    // The default fight: its own numbers (and the struct defaults for what it leaves out).
    const BeatSetup& d = t.fallback;
    CHECK(d.def.bpm == doctest::Approx(100.0));
    CHECK(d.def.offset == doctest::Approx(0.5));
    CHECK(d.def.hp == 80);
    CHECK(d.def.strikeDamage == 12);
    CHECK(d.def.counterDamage == 16);
    CHECK(d.def.holdDamage == 5);
    REQUIRE(d.def.enemyCount == 1);
    CHECK(d.def.enemies[0].hp == 60);
    CHECK(d.music.empty());
    CHECK(d.tuning.strike.perfect == doctest::Approx(0.04));
    CHECK(d.tuning.strike.good == doctest::Approx(0.1));
    CHECK(d.tuning.counter.dodge == doctest::Approx(0.28));
    CHECK(d.tuning.counter.parry == doctest::Approx(0.1));
    CHECK(d.tuning.counter.parryLockout == doctest::Approx(0.3));
    CHECK(d.tuning.counter.late == doctest::Approx(0.15));
    CHECK(d.tuning.holdSlack == doctest::Approx(0.1));
    CHECK(d.tuning.approachBeats == BeatTuning{}.approachBeats); // not in the data: untouched

    REQUIRE(t.entries.size() == 2);
    // Entry 1 overrides the tempo, the line-up and the cue, and inherits everything else.
    const BeatSetup& a = t.find("SC-T1", 4, 1);
    CHECK(a.def.bpm == doctest::Approx(96.0));
    CHECK(a.def.offset == doctest::Approx(0.42));
    REQUIRE(a.def.enemyCount == 3);
    CHECK(a.def.enemies[0].damage == 10);
    CHECK(a.def.enemies[1].damage == 22);
    CHECK(a.def.enemies[2].damage == 10);
    CHECK(a.music == "cue_a");
    CHECK(a.def.hp == 80);
    CHECK(a.def.strikeDamage == 12);
    CHECK(a.def.counterDamage == 16);
    CHECK(a.def.holdDamage == 5);
    CHECK(a.tuning.strike.perfect == doctest::Approx(0.04));
    CHECK(a.tuning.counter.parry == doctest::Approx(0.1));
    CHECK(a.tuning.holdSlack == doctest::Approx(0.1));
    // Entry 2 (its step is left out: 0) changes only the lead's hp.
    const BeatSetup& b = t.find("SC-T1", 4, 0);
    CHECK(b.def.hp == 50);
    CHECK(b.def.bpm == doctest::Approx(100.0));
    CHECK(b.def.enemyCount == 1);
    CHECK(b.music.empty());

    // Anything not listed plays the default fight.
    CHECK(&t.find("SC-T1", 5, 0) == &t.fallback);
    CHECK(&t.find("SC-T1", 4, 2) == &t.fallback);
    CHECK(&t.find("SC-OTHER", 4, 1) == &t.fallback);
    CHECK(t.findEntry("SC-T1", 4, 2) == nullptr);
    const BeatEntry* e = t.findEntry("SC-T1", 4, 1);
    REQUIRE(e != nullptr);
    CHECK(e->scene == "SC-T1");
    CHECK(e->beat == 4);
    CHECK(e->step == 1);
    CHECK(&e->setup == &a);
}

TEST_CASE("beat table: bad data is an error with its path, and leaves the table empty") {
    struct Bad {
        std::string json;
        const char* path; // each error names where
    };
    const std::string two = kTwoEnemies;
    const std::string def = kPlainDefault;
    const std::vector<Bad> bad = {
        // an unknown enemy id
        {tableJson(two, def, R"J([ { "scene": "A", "beat": 1, "enemies": ["wisp", "ghost"] } ])J"), "fights[0].enemies[1]"},
        {tableJson(two, R"J({ "enemies": ["ghost"] })J"), "default.enemies[0]"},
        // a duplicate id
        {tableJson(R"J([ { "id": "wisp", "patterns": [ "cL . . . . . . ." ] },
                         { "id": "wisp", "patterns": [ "cL . . . . . . ." ] } ])J", def),
         "enemies[1].id"},
        // a duplicate (scene, beat, step): the step defaults to 0
        {tableJson(two, def, R"J([ { "scene": "A", "beat": 1 }, { "scene": "A", "beat": 1, "step": 0 } ])J"), "fights[1]"},
        {tableJson(two, def, R"J([ { "scene": "A", "beat": 1, "step": 3 }, { "scene": "B", "beat": 1 },
                                   { "scene": "A", "beat": 1, "step": 3 } ])J"), "fights[2]"},
        // a pattern without a counter
        {tableJson(R"J([ { "id": "wisp", "patterns": [ "cL . . . . . . .", "sL . sR . . . . ." ] } ])J", def),
         "enemies[0].patterns[1]"},
        // a bpm out of range
        {tableJson(two, R"J({ "bpm": 39, "enemies": ["wisp"] })J"), "default.bpm"},
        {tableJson(two, R"J({ "bpm": 241, "enemies": ["wisp"] })J"), "default.bpm"},
        {tableJson(two, def, R"J([ { "scene": "A", "beat": 1, "bpm": 300 } ])J"), "fights[0].bpm"},
        // perfect > good, parry > dodge: in the default fight, and in a fight that overrides one of them
        {tableJson(two, R"J({ "enemies": ["wisp"], "strike": { "perfect": 0.2, "good": 0.1 } })J"), "default.strike"},
        {tableJson(two, def, R"J([ { "scene": "A", "beat": 1, "strike": { "perfect": 0.2 } } ])J"), "fights[0].strike"},
        {tableJson(two, R"J({ "enemies": ["wisp"], "counter": { "dodge": 0.1, "parry": 0.2 } })J"), "default.counter"},
        {tableJson(two, def, R"J([ { "scene": "A", "beat": 1, "counter": { "parry": 0.5 } } ])J"), "fights[0].counter"},
        // and the rest of the schema
        {tableJson("[]", def), "enemies"},
        {tableJson(R"J([ { "id": "wisp", "patterns": [ "sL . sR" ] } ])J", def), "enemies[0].patterns[0]"},
        {tableJson(R"J([ { "id": "wisp", "patterns": [] } ])J", def), "enemies[0].patterns"},
        {tableJson(R"J([ { "id": "wisp", "patterns": [ "cL . . . . . . .", "cL . . . . . . .", "cL . . . . . . .",
                         "cL . . . . . . .", "cL . . . . . . .", "cL . . . . . . .", "cL . . . . . . ." ] } ])J", def),
         "enemies[0].patterns"},
        {tableJson(R"J([ { "id": "wisp", "hp": 0, "patterns": [ "cL . . . . . . ." ] } ])J", def), "enemies[0].hp"},
        {tableJson(R"J([ { "id": "bad id", "patterns": [ "cL . . . . . . ." ] } ])J", def), "enemies[0].id"},
        {tableJson(two, R"J({ "bpm": 120 })J"), "default.enemies"},
        {tableJson(two, R"J({ "enemies": ["wisp", "wisp", "wisp", "wisp", "wisp"] })J"), "default.enemies"},
        {tableJson(two, R"J({ "enemies": ["wisp"], "hp": 0 })J"), "default.hp"},
        {tableJson(two, R"J({ "enemies": ["wisp"], "music": 3 })J"), "default.music"},
        {tableJson(two, def, R"J([ { "beat": 1 } ])J"), "fights[0].scene"},
        {tableJson(two, def, R"J([ { "scene": "A" } ])J"), "fights[0].beat"},
        {"{ \"version\": 2, \"enemies\": " + two + ", \"default\": " + def + " }", "version"},
        {"{ \"enemies\": " + two + " }", "default"},
        {"not json", "beat file"},
        {"[]", "beat file"},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.json);
        BeatTable t;
        std::string error;
        CHECK_FALSE(importBeatTable(b.json, t, &error));
        CHECK(error.find(b.path) != std::string::npos);
        CHECK(t.enemyIds.empty());
        CHECK(t.enemies.empty());
        CHECK(t.entries.empty());
    }

    // Each problem is on its own line.
    BeatTable t;
    std::string error;
    CHECK_FALSE(importBeatTable(tableJson(two, R"J({ "bpm": 1, "enemies": ["ghost"] })J"), t, &error));
    CHECK(error.find("default.bpm") != std::string::npos);
    CHECK(error.find("default.enemies[0]") != std::string::npos);
    CHECK(error.find('\n') != std::string::npos);

    // The same data without the mistakes loads.
    CHECK(importBeatTable(tableJson(two, def, R"J([ { "scene": "A", "beat": 1 }, { "scene": "A", "beat": 1, "step": 1 } ])J"),
                          t, &error));
    CHECK(t.entries.size() == 2);
}

TEST_CASE("beat table: an unknown key is a warning with its path") {
    const std::string json = R"J({
      "version": 1, "color": 3,
      "enemies": [ { "id": "wisp", "hp": 60, "damage": 10, "speed": 2, "patterns": [ "cL . . . . . . ." ] } ],
      "default": { "bpm": 120, "enemies": ["wisp"], "mood": 1, "strike": { "perfect": 0.05, "wide": 1 },
                   "counter": { "dodge": 0.3, "narrow": 1 } },
      "fights": [ { "scene": "A", "beat": 1, "volume": 2 } ]
    })J";
    BeatTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importBeatTable(json, t, &error, &warnings), error);
    CHECK(warnings.size() == 6);
    CHECK(hasWarning(warnings, "unknown key: color"));
    CHECK(hasWarning(warnings, "enemies[0]: unknown key: speed"));
    CHECK(hasWarning(warnings, "default: unknown key: mood"));
    CHECK(hasWarning(warnings, "default.strike: unknown key: wide"));
    CHECK(hasWarning(warnings, "default.counter: unknown key: narrow"));
    CHECK(hasWarning(warnings, "fights[0]: unknown key: volume"));
    CHECK(t.entries.size() == 1);
}

TEST_CASE("beat table: Data/Battle/beat.json loads without warnings") {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/beat.json", text)) {
        FAIL("Data/Battle/beat.json not found");
    }
    BeatTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importBeatTable(text, t, &error, &warnings), error);
    CHECK(warnings.empty());

    CHECK(t.enemies.size() >= 2);
    REQUIRE(t.enemyIds.size() == t.enemies.size());
    for (std::size_t i = 0; i < t.enemies.size(); ++i) {
        CAPTURE(t.enemyIds[i]);
        CHECK_FALSE(t.enemyIds[i].empty());
        for (std::size_t k = i + 1; k < t.enemies.size(); ++k) CHECK(t.enemyIds[i] != t.enemyIds[k]);
        const BeatEnemy& e = t.enemies[i];
        CHECK(e.hp > 0);
        CHECK(e.damage > 0);
        REQUIRE(e.patternCount >= 1);
        REQUIRE(e.patternCount <= kMaxBeatPatterns);
        for (std::size_t p = 0; p < e.patternCount; ++p) CHECK(e.patterns[p].hasCounter());
    }
    // The default fight: 120 bpm, no offset, no cue; and the file lists no fights of its own yet.
    CHECK(t.fallback.def.bpm == doctest::Approx(120.0));
    CHECK(t.fallback.def.offset == doctest::Approx(0.0));
    CHECK(t.fallback.music.empty());
    CHECK(t.fallback.def.enemyCount >= 1);
    CHECK(t.entries.empty());
}

// ---- windows ----

TEST_CASE("beat windows: unchanged at 120 bpm, capped at fast tempos") {
    const BeatTuning t{};
    // 120 bpm: an eighth is 0.25 s, so neither cap (0.125 s for a strike, 0.25 s for a counter) bites.
    const BeatWindows s120 = effectiveStrikeWindows(t, 120.0);
    CHECK(s120.perfect == doctest::Approx(0.050));
    CHECK(s120.good == doctest::Approx(0.120));
    const DefenceWindows c120 = effectiveCounterWindows(t, 120.0);
    CHECK(c120.parry == doctest::Approx(0.12));
    CHECK(c120.dodge == doctest::Approx(0.30));
    // The other fields are the tuning's.
    CHECK(c120.perfectDodge == t.counter.perfectDodge);
    CHECK(c120.parryLockout == t.counter.parryLockout);
    CHECK(c120.late == t.counter.late);
    CHECK(c120.dodgeLockout == 0.0);
    CHECK(c120.jumpLockout == 0.0);

    // 180 bpm: an eighth is 1/6 s. A strike's good window reaches half of it (1/12 s); a counter's dodge
    // window opens at most one eighth (1/6 s) before the beat, the parry window staying as it was.
    const BeatWindows s180 = effectiveStrikeWindows(t, 180.0);
    CHECK(s180.perfect == doctest::Approx(0.050));
    CHECK(s180.good == doctest::Approx(1.0 / 12.0));
    const DefenceWindows c180 = effectiveCounterWindows(t, 180.0);
    CHECK(c180.parry == doctest::Approx(0.12));
    CHECK(c180.dodge == doctest::Approx(1.0 / 6.0 + 0.06));

    // 240 bpm: an eighth is 0.125 s.
    const BeatWindows s240 = effectiveStrikeWindows(t, 240.0);
    CHECK(s240.perfect == doctest::Approx(0.050));
    CHECK(s240.good == doctest::Approx(0.0625));
    const DefenceWindows c240 = effectiveCounterWindows(t, 240.0);
    CHECK(c240.parry == doctest::Approx(0.12));
    CHECK(c240.dodge == doctest::Approx(0.125 + 0.06));

    // A very wide parry window is capped too, and stays centred on the beat (the impact is half of it
    // after the eighth): at 120 bpm a half window reaches 0.25 s.
    BeatTuning wide;
    wide.counter.parry = 0.8;
    wide.counter.dodge = 1.0;
    const DefenceWindows cw = effectiveCounterWindows(wide, 120.0);
    CHECK(cw.parry == doctest::Approx(0.5));
    CHECK(cw.dodge == doctest::Approx(0.5));

    // A fight judges with them: at 240 bpm a beat is 0.25 s and the first bar line is beat 4 (1.0 s);
    // the strike is at 1.0 s and the counter an eighth (0.125 s) later.
    BeatFightDef def = oneEnemy({"sL cU . . . . . ."}, 1000, 10);
    def.hp = 1000;
    def.bpm = 240.0;
    BeatFight f;
    f.begin(def, plain(), 0.0, 1);
    CHECK(f.tuning().strike.good == doctest::Approx(0.12)); // the tuning itself is as given
    f.update(0.5); // approachBeats 2 is 0.5 s: the bar is generated now
    REQUIRE(f.promptCount() == 2);
    const double strike = f.prompt(0).time;
    CHECK(strike == doctest::Approx(1.0));
    CHECK(f.prompt(1).time == doctest::Approx(1.125));
    // A strike is Good within 0.0625 s: 0.07 s early answers nothing, 0.06 s early is Good.
    const BeatEvents early = f.press(Arrow::Left, strike - 0.07);
    CHECK(countOf(early, BeatEventType::Stray) == 1);
    const BeatEvents good = f.press(Arrow::Left, strike - 0.06);
    REQUIRE(findEvent(good, BeatEventType::Judged) != nullptr);
    CHECK(findEvent(good, BeatEventType::Judged)->grade == QteGrade::Good);
    // The counter's dodge window (0.185 s before its impact, 0.125 s before its eighth) opens at 1.0 s:
    // 0.02 s before that it is a whiff.
    const BeatEvents whiff = f.press(Arrow::Up, strike - 0.02);
    CHECK(countOf(whiff, BeatEventType::Whiff) == 1);
}

// ---- the schedule ----

TEST_CASE("beat schedule: the first bar line comes after the lead-in") {
    const BeatFightDef def = oneEnemy({"cU . . . . . . ."});
    BeatFight f;
    // 120 bpm, begun at 0 with one lead-in bar: beat 4 is 2.0 s.
    f.begin(def, plain(), 0.0, 1);
    CHECK(f.firstBarBeat() == doctest::Approx(4.0));
    CHECK(f.timeAtBeat(f.firstBarBeat()) == doctest::Approx(2.0));
    CHECK(f.phase() == BeatPhase::Playing);
    CHECK(f.promptCount() == 0); // nothing is open until the clock runs
    CHECK(f.now() == 0.0);
    // Begun inside a bar: the first bar line at least a bar later (0.3 s is beat 0.6; 4.6 -> 8).
    f.begin(def, plain(), 0.3, 1);
    CHECK(f.firstBarBeat() == doctest::Approx(8.0));
    // Begun on a bar line (2.0 s is beat 4): a whole bar later.
    f.begin(def, plain(), 2.0, 1);
    CHECK(f.firstBarBeat() == doctest::Approx(8.0));
    BeatTuning t = plain();
    t.leadInBars = 2;
    f.begin(def, t, 0.0, 1);
    CHECK(f.firstBarBeat() == doctest::Approx(8.0));
    t.leadInBars = 0; // none: the first bar line at or after the start
    f.begin(def, t, 0.0, 1);
    CHECK(f.firstBarBeat() == doctest::Approx(0.0));
    f.begin(def, t, 0.1, 1);
    CHECK(f.firstBarBeat() == doctest::Approx(4.0));
    // An offset: beat 0 at 0.5 s and 96 bpm (0.625 s a beat); begun at 0 the first line a bar on is beat 4.
    BeatFightDef shifted = def;
    shifted.bpm = 96.0;
    shifted.offset = 0.5;
    f.begin(shifted, plain(), 0.0, 1);
    CHECK(f.firstBarBeat() == doctest::Approx(4.0));
    CHECK(f.timeAtBeat(f.firstBarBeat()) == doctest::Approx(3.0));
    CHECK(f.secondsPerBeat() == doctest::Approx(0.625));
    CHECK(f.beatAt(3.0) == doctest::Approx(4.0));
}

TEST_CASE("beat schedule: a bar is generated approachBeats before it starts, and bars follow back to back") {
    const BeatFightDef def = oneEnemy({"sL . . . . . . ."});
    for (const double approach : {0.5, 2.0, 4.0}) {
        CAPTURE(approach);
        BeatTuning t = plain();
        t.approachBeats = approach;
        t.leadInBars = 2; // the first bar is beat 8 (4.0 s): room for a 4-beat approach
        BeatFight f;
        f.begin(def, t, 0.0, 1);
        CHECK(f.firstBarBeat() == doctest::Approx(8.0));
        const auto lastId = [&f]() { return f.promptCount() == 0 ? 0u : f.prompt(f.promptCount() - 1).id; };
        for (int bar = 0; bar < 4; ++bar) {
            CAPTURE(bar);
            const std::uint32_t id = static_cast<std::uint32_t>(bar + 1);
            const double start = f.timeAtBeat(8.0 + 4.0 * bar);
            const double generated = f.timeAtBeat(8.0 + 4.0 * bar - approach);
            f.update(generated - 0.01);
            CHECK(lastId() != id); // not there yet
            f.update(generated + 0.001);
            REQUIRE(lastId() == id);
            const BeatPrompt p = f.prompt(f.promptCount() - 1);
            CHECK(p.time == doctest::Approx(start));
            CHECK(p.beat == doctest::Approx(8.0 + 4.0 * bar));
            // Its marker appears approachBeats (half as many seconds at 120 bpm) before its eighth.
            CHECK(f.approach(p, start - approach * 0.5 - 0.01) == -1.0);
            CHECK(f.approach(p, start - approach * 0.5 + 0.01) > 0.9);
        }
    }

    // The bars start on the bar lines (stats().bars counts them as they start).
    BeatFight f;
    f.begin(oneEnemy({". . . . . . . ."}), plain(), 0.0, 1);
    f.update(1.999);
    CHECK(f.stats().bars == 0);
    f.update(2.0);
    CHECK(f.stats().bars == 1);
    f.update(3.999);
    CHECK(f.stats().bars == 1);
    f.update(4.0);
    CHECK(f.stats().bars == 2);
    f.update(20.0); // one long frame: bars at 2, 4, ..., 20
    CHECK(f.stats().bars == 10);
}

TEST_CASE("beat schedule: a bar's prompts are its pattern's slots, in time order") {
    BeatFight f = beginFight({"sL sD sU sR cL cD cU cR"});
    REQUIRE(f.promptCount() == 8);
    const BeatKind kinds[] = {BeatKind::Strike,  BeatKind::Strike,  BeatKind::Strike,  BeatKind::Strike,
                              BeatKind::Counter, BeatKind::Counter, BeatKind::Counter, BeatKind::Counter};
    for (std::size_t i = 0; i < 8; ++i) {
        const BeatPrompt& p = f.prompt(i);
        CHECK(p.kind == kinds[i]);
        CHECK(p.input == kArrows[i & 3u]);
        CHECK(p.id == i + 1);
        CHECK(p.enemy == 0);
        CHECK(p.length == 0);
        CHECK_FALSE(p.finisher);
        CHECK(p.grade == QteGrade::Pending);
        CHECK(p.time == doctest::Approx(2.0 + 0.25 * static_cast<double>(i)));
        CHECK(p.beat == doctest::Approx(4.0 + 0.5 * static_cast<double>(i)));
        CHECK(p.end == p.time);
    }
    // A hold's prompt knows its length and its end; its `-` slots are no prompts.
    BeatFight h = beginFight({"hU3 - - cD . . sL ."});
    REQUIRE(h.promptCount() == 3);
    CHECK(h.prompt(0).kind == BeatKind::Hold);
    CHECK(h.prompt(0).length == 3);
    CHECK(h.prompt(0).time == doctest::Approx(2.0));
    CHECK(h.prompt(0).end == doctest::Approx(2.75));
    CHECK(h.prompt(1).kind == BeatKind::Counter);
    CHECK(h.prompt(1).time == doctest::Approx(2.75));
    CHECK(h.prompt(2).kind == BeatKind::Strike);
    CHECK(h.prompt(2).time == doctest::Approx(3.5));
}

TEST_CASE("beat schedule: the marker closes in from 1 to 0 over approachBeats, then rests at 0") {
    BeatFight f = beginFight({"sL . . . . . . ."});
    const BeatPrompt p = f.prompt(0); // at 2.0 s; approachBeats 2 is 1.0 s
    CHECK(f.approach(p, p.time - 1.5) == -1.0);
    CHECK(f.approach(p, p.time - 1.0) == doctest::Approx(1.0));
    CHECK(f.approach(p, p.time - 0.5) == doctest::Approx(0.5));
    CHECK(f.approach(p, p.time - 0.1) == doctest::Approx(0.1));
    CHECK(f.approach(p, p.time) == 0.0);
    CHECK(f.approach(p, p.time + 0.1) == 0.0);

    BeatTuning t = plain();
    t.approachBeats = 0.0; // no lead: it shows at its eighth
    BeatFight g = beginFight({"sL . . . . . . ."}, t);
    const BeatPrompt q = g.prompt(0);
    CHECK(g.approach(q, q.time - 0.01) == -1.0);
    CHECK(g.approach(q, q.time) == 0.0);
}

TEST_CASE("beat schedule: enemies take turns, round robin over the living ones") {
    // Three enemies whose bars are one strike each, a different arrow each (L, D, U); enemy 1 has 10 hp.
    BeatFightDef def = defOf({{60, 10, {"sL . . . . . . ."}}, {10, 10, {"sD . . . . . . ."}}, {60, 10, {"sU . . . . . . ."}}});
    def.hp = 1000;
    BeatFight f;
    f.begin(def, plain(), 0.0, 1);
    // Before the first bar line nobody has the turn.
    f.update(1.5);
    CHECK(f.turnEnemy() == -1);
    // Bars 0, 1, 2 belong to enemies 0, 1, 2. Enemy 1's strike is answered: it falls.
    const int owner[] = {0, 1, 2};
    const Arrow arrow[] = {Arrow::Left, Arrow::Down, Arrow::Up};
    for (int bar = 0; bar < 3; ++bar) {
        CAPTURE(bar);
        const double start = 2.0 + 2.0 * bar;
        f.update(start - 0.5);
        REQUIRE(f.promptCount() == 1);
        CHECK(f.prompt(0).enemy == owner[bar]);
        CHECK(f.prompt(0).input == arrow[bar]);
        CHECK(f.update(start + 0.01).empty());
        CHECK(f.turnEnemy() == owner[bar]); // the bar is playing
        if (bar == 1) {
            const BeatEvents hit = f.press(arrow[bar], start + 0.01);
            CHECK(countOf(hit, BeatEventType::EnemyDown) == 1);
            CHECK_FALSE(f.enemyAlive(1));
            CHECK(f.turnEnemy() == -1); // a down enemy has no turn
        }
    }
    // Enemy 1 is down: the bars that follow go 0, 2, 0, 2.
    const int later[] = {0, 2, 0, 2};
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        const double start = 2.0 + 2.0 * (3 + i);
        f.update(start - 0.5);
        REQUIRE(f.promptCount() == 1);
        CHECK(f.prompt(0).enemy == later[i]);
        f.update(start + 0.01);
        CHECK(f.turnEnemy() == later[i]);
    }
}

TEST_CASE("beat schedule: an enemy never plays the same pattern twice in a row") {
    // One enemy, three patterns told apart by the arrow of their one strike.
    BeatFight f = beginFight({"sL . . . . . . .", "sD . . . . . . .", "sU . . . . . . ."});
    std::vector<int> picks;
    int seen[3] = {0, 0, 0};
    for (int bar = 0; bar < 150; ++bar) {
        f.update(barStart(f, bar) - 0.5);
        const std::string sig = barSignature(f, bar);
        REQUIRE(sig.size() == 4); // one strike: kind, arrow, slot, a blank
        const int arrow = sig[1] == 'L' ? 0 : (sig[1] == 'D' ? 1 : 2);
        if (!picks.empty()) CHECK(arrow != picks.back());
        picks.push_back(arrow);
        ++seen[arrow];
    }
    CHECK(seen[0] > 20);
    CHECK(seen[1] > 20);
    CHECK(seen[2] > 20);

    // Two patterns can only alternate; each enemy has its own memory.
    BeatFightDef def = defOf({{60, 10, {"sL . . . . . . .", "sD . . . . . . ."}}, {60, 10, {"sU . . . . . . .", "sR . . . . . . ."}}});
    def.hp = 100000;
    BeatFight g;
    g.begin(def, plain(), 0.0, 4);
    char last[2] = {'?', '?'};
    for (int bar = 0; bar < 40; ++bar) {
        g.update(barStart(g, bar) - 0.5);
        const std::string sig = barSignature(g, bar);
        REQUIRE(sig.size() == 4);
        CHECK(sig[1] != last[bar & 1]);
        last[bar & 1] = sig[1];
    }

    // The seed picks: different seeds start differently somewhere.
    bool differ = false;
    char firstPick = '?';
    for (std::uint32_t seed = 1; seed <= 16; ++seed) {
        BeatFight h = beginFight({"sL . . . . . . .", "sD . . . . . . .", "sU . . . . . . ."}, plain(), {}, seed);
        const char pick = barSignature(h, 0)[1];
        if (firstPick == '?') firstPick = pick;
        differ = differ || pick != firstPick;
    }
    CHECK(differ);
}

TEST_CASE("beat schedule: a bar whose enemy went down before it starts plays as a rest") {
    // Two enemies. Enemy 0's bar is one hold that fills it, ends on the bar line and kills it. With
    // approachBeats 4 a bar is generated a whole bar early, and at the moment that hold completes (a bar
    // is generated first when both fall at the same moment) the bar that goes to enemy 0 again is made
    // with enemy 0 still alive: it is a rest.
    BeatTuning t = plain();
    t.approachBeats = 4.0;
    BeatFightDef def = defOf({{12, 10, {"hU8 - - - - - - -"}}, {60, 10, {"cL . . . . . . ."}}});
    def.hp = 1000;
    BeatFight f;
    f.begin(def, t, 0.0, 1);
    f.update(0.0); // bar 0 is generated now (beat 0)
    REQUIRE(f.promptCount() == 1);
    CHECK(f.prompt(0).kind == BeatKind::Hold);
    CHECK(f.prompt(0).time == doctest::Approx(2.0));
    CHECK(f.prompt(0).end == doctest::Approx(4.0));
    f.update(1.0);
    CHECK(f.press(Arrow::Up, 2.0).empty()); // pressed: held
    CHECK(f.holding());
    f.update(3.0);
    CHECK(f.turnEnemy() == 0);
    f.update(3.9);
    // At 4.0 the hold completes (48 damage to 12 hp).
    const BeatEvents e = f.update(4.0);
    CHECK(countOf(e, BeatEventType::Judged) == 1);
    CHECK(countOf(e, BeatEventType::EnemyDown) == 1);
    CHECK(countOf(e, BeatEventType::Victory) == 0); // enemy 1 is alive
    CHECK_FALSE(f.enemyAlive(0));
    CHECK(f.enemyAlive(1));
    CHECK(f.turnEnemy() == 1); // bar 1, enemy 1's, is playing
    for (std::size_t i = 0; i < f.promptCount(); ++i) CHECK(f.prompt(i).enemy == 1); // nothing of enemy 0's is open
    // Bar 2 (6.0 s to 8.0 s) is a rest: no turn, no prompt of its own. Bar 3 goes to enemy 1.
    f.update(6.5);
    CHECK(f.turnEnemy() == -1);
    for (std::size_t i = 0; i < f.promptCount(); ++i) {
        const double time = f.prompt(i).time;
        CHECK((time < 6.0 - 0.01 || time > 8.0 - 0.01));
    }
    f.update(8.5);
    CHECK(f.turnEnemy() == 1);
}

// ---- judging: strikes ----

TEST_CASE("beat judging: a strike is Perfect, Good or a Miss around its eighth, and an early press is a stray") {
    struct Case {
        double offset;
        QteGrade grade;
        int damage;
    };
    // Perfect within 0.05 s of the eighth, Good within 0.12 s, on both sides; a Good hit deals round(10 * 0.6).
    const Case cases[] = {
        {-0.045, QteGrade::Perfect, 10}, {0.045, QteGrade::Perfect, 10}, {0.0, QteGrade::Perfect, 10},
        {-0.055, QteGrade::Good, 6},     {0.055, QteGrade::Good, 6},     {-0.115, QteGrade::Good, 6},
        {0.115, QteGrade::Good, 6},
    };
    for (const Case& c : cases) {
        CAPTURE(c.offset);
        BeatFight f = beginFight({"sL . . . . . . ."});
        const BeatEvents e = f.press(Arrow::Left, 2.0 + c.offset);
        REQUIRE(e.size() == 2);
        CHECK(e[0].type == BeatEventType::Judged);
        CHECK(e[0].grade == c.grade);
        CHECK(e[0].kind == BeatKind::Strike);
        CHECK(e[0].prompt == 1);
        CHECK(e[0].time == doctest::Approx(2.0 + c.offset));
        CHECK(e[1].type == BeatEventType::EnemyHit);
        CHECK(e[1].amount == c.damage);
        CHECK(f.enemyHp(0) == 1000 - c.damage);
        CHECK(f.promptCount() == 0);
        CHECK(f.stats().damageDealt == c.damage);
    }

    // Before the window opens the press answers nothing: a stray, and the prompt is still there.
    BeatFight f = beginFight({"sL . . . . . . ."});
    const BeatEvents stray = f.press(Arrow::Left, 2.0 - 0.13);
    REQUIRE(stray.size() == 1);
    CHECK(stray[0].type == BeatEventType::Stray);
    CHECK(f.stats().strays == 1);
    CHECK(f.promptCount() == 1);
    CHECK(f.press(Arrow::Left, 2.0).size() == 2); // it answers when its time comes

    // Past the window with nothing pressed: a Miss at the window's end; a late press is a stray.
    BeatFight g = beginFight({"sL . . . . . . ."});
    CHECK(g.update(2.12).empty()); // a press at the last moment would still count
    const BeatEvents miss = g.update(2.121);
    REQUIRE(miss.size() == 1);
    CHECK(miss[0].type == BeatEventType::Judged);
    CHECK(miss[0].grade == QteGrade::Miss);
    CHECK(miss[0].time == doctest::Approx(2.12));
    CHECK(g.stats().misses == 1);
    CHECK(g.hp() == 1000); // only a missed counter hurts the lead
    const BeatEvents late = g.press(Arrow::Left, 2.13);
    REQUIRE(late.size() == 1);
    CHECK(late[0].type == BeatEventType::Stray);

    // A wrong arrow answers nothing.
    BeatFight h = beginFight({"sL . . . . . . ."});
    CHECK(h.press(Arrow::Right, 2.0)[0].type == BeatEventType::Stray);
    CHECK(h.promptCount() == 1);
}

// ---- judging: counters ----

TEST_CASE("beat judging: a counter is Perfect in the parry window around its beat and Good earlier in the dodge window") {
    struct Case {
        double offset; // from the counter's eighth
        QteGrade grade;
    };
    // The impact is 0.06 s after the eighth: Perfect from 0.06 s before to 0.06 s after the eighth; Good
    // from 0.24 s before.
    const Case cases[] = {
        {-0.055, QteGrade::Perfect}, {0.0, QteGrade::Perfect}, {0.055, QteGrade::Perfect}, {-0.065, QteGrade::Good},
        {-0.15, QteGrade::Good},     {-0.23, QteGrade::Good},
    };
    for (const Case& c : cases) {
        CAPTURE(c.offset);
        BeatFight f = beginFight({"cU . . . . . . ."});
        const BeatEvents e = f.press(Arrow::Up, 2.0 + c.offset);
        REQUIRE(e.size() == 2);
        CHECK(e[0].type == BeatEventType::Judged);
        CHECK(e[0].grade == c.grade);
        CHECK(e[0].kind == BeatKind::Counter);
        CHECK(e[1].type == BeatEventType::EnemyHit);
        CHECK(e[1].amount == (c.grade == QteGrade::Perfect ? 14 : 8)); // 14, and round(14 * 0.6)
        CHECK(f.hp() == 1000); // an answered counter costs the lead nothing
        CHECK(f.stats().counters == 1);
        f.update(3.0);
        CHECK(f.hp() == 1000); // and the prompt is gone: nothing lands later
    }

    // Unanswered: a Miss at the impact (0.06 s after the eighth), and the lead takes the enemy's damage.
    BeatFight f = beginFight({"cU . . . . . . ."});
    CHECK(f.update(2.059).empty());
    const BeatEvents e = f.update(2.061);
    REQUIRE(e.size() == 2);
    CHECK(e[0].type == BeatEventType::Judged);
    CHECK(e[0].grade == QteGrade::Miss);
    CHECK(e[0].kind == BeatKind::Counter);
    CHECK(e[0].time == doctest::Approx(2.06));
    CHECK(e[1].type == BeatEventType::LeadHit);
    CHECK(e[1].amount == 10);
    CHECK(e[1].enemy == 0);
    CHECK(e[1].prompt == e[0].prompt);
    CHECK(f.hp() == 990);
    CHECK(f.stats().damageTaken == 10);
    CHECK(f.stats().misses == 1);

    // A wrong arrow doesn't answer it; the right one does.
    BeatFight g = beginFight({"cU . . . . . . ."});
    CHECK(g.press(Arrow::Down, 2.0)[0].type == BeatEventType::Stray);
    CHECK(g.press(Arrow::Up, 2.0).size() == 2);
}

TEST_CASE("beat judging: a press just after a counter landed is Late, once, with no other effect") {
    BeatFight f = beginFight({"cU . . . . . . ."});
    f.update(2.07); // it landed at 2.06
    const int hp = f.hp();
    const BeatEvents late = f.press(Arrow::Up, 2.1); // 0.04 s after: within counter.late (0.2 s)
    REQUIRE(late.size() == 1);
    CHECK(late[0].type == BeatEventType::Late);
    CHECK(late[0].prompt == 1);
    CHECK(late[0].enemy == 0);
    CHECK(f.hp() == hp);
    CHECK(f.stats().strays == 0);
    CHECK_FALSE(f.counterLockedOut(2.11));
    // The second press is no longer late: a stray.
    const BeatEvents again = f.press(Arrow::Up, 2.12);
    REQUIRE(again.size() == 1);
    CHECK(again[0].type == BeatEventType::Stray);

    // A press that comes too long after, or with another arrow, is a stray.
    BeatFight g = beginFight({"cU . . . . . . ."});
    g.update(2.07);
    CHECK(g.press(Arrow::Up, 2.06 + 0.21)[0].type == BeatEventType::Stray);
    BeatFight h = beginFight({"cU . . . . . . ."});
    h.update(2.07);
    CHECK(h.press(Arrow::Down, 2.1)[0].type == BeatEventType::Stray);

    // The press that lands the counter late is both: the Miss and the LeadHit, then Late.
    BeatFight k = beginFight({"cU . . . . . . ."});
    const BeatEvents e = k.press(Arrow::Up, 2.1);
    CHECK(countOf(e, BeatEventType::LeadHit) == 1);
    CHECK(e[e.size() - 1].type == BeatEventType::Late);

    // Late goes to the counter whose arrow was pressed, even when another landed since (a long counter.late
    // puts two in it): up at 2.0 (landed 2.06) and down at 2.25 (landed 2.31), each late once.
    BeatTuning longLate = plain();
    longLate.counter.late = 0.5;
    BeatFight m = beginFight({"cU cD . . . . . ."}, longLate);
    m.update(2.32);
    CHECK(m.stats().damageTaken == 20);
    const BeatEvents up = m.press(Arrow::Up, 2.33);
    REQUIRE(up.size() == 1);
    CHECK(up[0].type == BeatEventType::Late);
    CHECK(up[0].prompt == 1);
    const BeatEvents down = m.press(Arrow::Down, 2.34);
    REQUIRE(down.size() == 1);
    CHECK(down[0].type == BeatEventType::Late);
    CHECK(down[0].prompt == 2);
    CHECK(m.press(Arrow::Up, 2.35)[0].type == BeatEventType::Stray);
    CHECK(m.press(Arrow::Down, 2.36)[0].type == BeatEventType::Stray);

    // Late wins over a whiff: with a second counter of the same arrow showing, the press is still Late
    // for the first, and no lockout starts.
    BeatFight n = beginFight({"cU . cU . . . . ."});
    n.update(2.07);
    const BeatEvents second = n.press(Arrow::Up, 2.1);
    REQUIRE(second.size() == 1);
    CHECK(second[0].type == BeatEventType::Late);
    CHECK_FALSE(n.counterLockedOut(2.2));
    CHECK(n.stats().whiffs == 0);
}

TEST_CASE("beat judging: mashing a counter's arrow before its window locks counters out, but never strikes") {
    // A strike at 2.0 s (left) then a counter at 2.25 s (up), whose marker shows from 1.25 s and whose
    // dodge window opens at 2.01 s.
    BeatFight f = beginFight({"sL cU . . . . . ."});
    // Showing, window closed: a whiff. The lockout is min(0.40 s, 0.75 beats = 0.375 s).
    const BeatEvents whiff = f.press(Arrow::Up, 1.7);
    REQUIRE(whiff.size() == 1);
    CHECK(whiff[0].type == BeatEventType::Whiff);
    CHECK(whiff[0].amount == 375);
    CHECK(whiff[0].prompt == 2);
    CHECK(f.stats().whiffs == 1);
    CHECK(f.stats().strays == 0);
    CHECK(f.counterLockedOut(1.7));
    CHECK(f.counterLockedOut(1.7 + 0.374));
    CHECK_FALSE(f.counterLockedOut(1.7 + 0.376));

    // Strikes are never locked out: the strike at 2.0 s is inside the lockout (until 2.075 s).
    const BeatEvents strike = f.press(Arrow::Left, 2.0);
    REQUIRE(strike.size() == 2);
    CHECK(strike[0].type == BeatEventType::Judged);
    CHECK(strike[0].grade == QteGrade::Perfect);

    // A press that would answer the counter, inside the lockout, is ignored.
    const BeatEvents ignored = f.press(Arrow::Up, 2.05);
    REQUIRE(ignored.size() == 1);
    CHECK(ignored[0].type == BeatEventType::LockedOut);
    CHECK(ignored[0].prompt == 2);
    CHECK(f.stats().ignored == 1);
    CHECK(f.promptCount() == 1); // the counter is still open

    // Once the lockout is over it answers: 0.15 s before its eighth is the dodge window (Good).
    const BeatEvents answered = f.press(Arrow::Up, 2.1);
    REQUIRE(answered.size() == 2);
    CHECK(answered[0].grade == QteGrade::Good);

    // A press during the lockout that is itself a whiff starts it over.
    BeatFight g = beginFight({"cU . . . . . . ."});
    CHECK(g.press(Arrow::Up, 1.5)[0].type == BeatEventType::Whiff); // locked until 1.875
    CHECK(g.press(Arrow::Up, 1.7)[0].type == BeatEventType::Whiff); // the window opens at 1.76: closed still
    CHECK(g.counterLockedOut(2.0));                                  // 1.7 + 0.375 = 2.075
    CHECK(g.stats().whiffs == 2);

    // Any showing counter of that arrow counts, not only the next prompt: the counter is third, at 2.5 s,
    // and its window opens at 2.26 s.
    BeatFight h = beginFight({"sL sD cU . . . . ."});
    CHECK(h.press(Arrow::Up, 2.0)[0].type == BeatEventType::Whiff);

    // Skipping to a counter is a press that would answer it: locked out, the skip doesn't happen either.
    BeatFight k = beginFight({"sL cU . . . . . ."});
    CHECK(k.press(Arrow::Up, 1.9)[0].type == BeatEventType::Whiff); // locked until 2.275; the window opens at 2.01
    const BeatEvents locked = k.press(Arrow::Up, 2.05);             // inside the strike's window as well
    REQUIRE(locked.size() == 1);
    CHECK(locked[0].type == BeatEventType::LockedOut);
    CHECK(locked[0].prompt == 2);
    CHECK(k.promptCount() == 2); // the strike wasn't skipped
}

TEST_CASE("beat judging: a press for a counter whose marker isn't showing yet is a stray") {
    // The counter is at 3.0 s; its marker shows from 2.0 s. The bar is open from 1.0 s.
    BeatFight f = beginFight({"sL . . . cU . . ."});
    REQUIRE(f.promptCount() == 2);
    const BeatEvents early = f.press(Arrow::Up, 1.5);
    REQUIRE(early.size() == 1);
    CHECK(early[0].type == BeatEventType::Stray);
    CHECK(f.stats().whiffs == 0);
    CHECK(f.stats().strays == 1);
    CHECK_FALSE(f.counterLockedOut(1.6));
    // Once it is showing but its window hasn't opened (from 2.76 s), the same press is a whiff.
    f.update(2.2);
    const BeatEvents whiff = f.press(Arrow::Up, 2.2);
    CHECK(countOf(whiff, BeatEventType::Whiff) == 1);
    CHECK(f.stats().whiffs == 1);
    CHECK(f.counterLockedOut(2.3));
}

TEST_CASE("beat judging: strict order, and the skipped-note exception") {
    // A press answers the earliest prompt: a press of the second prompt's arrow before its window is a stray.
    BeatFight f = beginFight({"sL sD sU . . . . ."});
    const BeatEvents wrong = f.press(Arrow::Down, 2.0);
    REQUIRE(wrong.size() == 1);
    CHECK(wrong[0].type == BeatEventType::Stray);
    CHECK(f.promptCount() == 3);
    CHECK(f.press(Arrow::Left, 2.0).size() == 2);
    // The same arrow again doesn't answer the next prompt (down), whose window isn't open.
    CHECK(f.press(Arrow::Left, 2.0)[0].type == BeatEventType::Stray);
    CHECK(f.press(Arrow::Down, 2.25).size() == 2);
    CHECK(f.press(Arrow::Up, 2.5).size() == 2);
    CHECK(f.promptCount() == 0);

    // The exception: the arrow of the next prompt, inside its window, while the earliest is still open.
    // A counter's dodge window opens 0.24 s before its eighth, inside the strike's window.
    BeatFight g = beginFight({"sL cD . . . . . ."});
    const BeatEvents skip = g.press(Arrow::Down, 2.05);
    REQUIRE(skip.size() == 3);
    CHECK(skip[0].type == BeatEventType::Judged); // the skipped strike is a Miss...
    CHECK(skip[0].grade == QteGrade::Miss);
    CHECK(skip[0].kind == BeatKind::Strike);
    CHECK(skip[0].prompt == 1);
    CHECK(skip[1].type == BeatEventType::Judged); // ...and the counter is answered
    CHECK(skip[1].grade == QteGrade::Good);
    CHECK(skip[1].kind == BeatKind::Counter);
    CHECK(skip[1].prompt == 2);
    CHECK(skip[2].type == BeatEventType::EnemyHit);
    CHECK(g.stats().misses == 1);
    CHECK(g.stats().goods == 1);
    CHECK(g.chain() == 1);
    CHECK(g.promptCount() == 0);

    // Skipping a counter costs the lead its hit.
    BeatFight h = beginFight({"cL cD . . . . . ."});
    const BeatEvents lose = h.press(Arrow::Down, 2.03);
    CHECK(countOf(lose, BeatEventType::LeadHit) == 1);
    REQUIRE(findEvent(lose, BeatEventType::Judged, 1) != nullptr);
    CHECK(findEvent(lose, BeatEventType::Judged, 0)->grade == QteGrade::Miss);
    CHECK(findEvent(lose, BeatEventType::Judged, 1)->grade == QteGrade::Good);
    CHECK(h.hp() == 990);

    // A press that matches neither prompt is a stray and skips nothing.
    BeatFight k = beginFight({"sL sD . . . . . ."});
    CHECK(k.press(Arrow::Up, 2.05)[0].type == BeatEventType::Stray);
    CHECK(k.promptCount() == 2);
}

// ---- judging: holds ----

TEST_CASE("beat judging: a hold completes when held to its end, with the grade of its press") {
    BeatFight f = beginFight({"hU2 - . . . . . ."}); // 2.0 s to 2.5 s
    CHECK(f.press(Arrow::Up, 2.0).empty());          // pressed: nothing resolves yet
    CHECK(f.holding());
    CHECK(f.holdProgress() == doctest::Approx(0.0));
    f.update(2.25);
    CHECK(f.holdProgress() == doctest::Approx(0.5));
    CHECK(f.promptCount() == 1);
    CHECK(f.prompt(0).grade == QteGrade::Perfect); // the press's grade, while it is held
    const BeatEvents done = f.update(2.5);
    REQUIRE(done.size() == 2);
    CHECK(done[0].type == BeatEventType::Judged);
    CHECK(done[0].grade == QteGrade::Perfect);
    CHECK(done[0].kind == BeatKind::Hold);
    CHECK(done[0].time == doctest::Approx(2.5));
    CHECK(done[1].type == BeatEventType::EnemyHit);
    CHECK(done[1].amount == 12); // holdDamage 6 * 2 eighths
    CHECK_FALSE(f.holding());
    CHECK(f.holdProgress() == 0.0);
    CHECK(f.release(Arrow::Up, 2.6).empty()); // letting go afterwards does nothing

    // A Good press keeps its grade: round(6 * 2 * 0.6) = 7.
    BeatFight g = beginFight({"hU2 - . . . . . ."});
    g.press(Arrow::Up, 2.1);
    const BeatEvents good = g.update(2.5);
    REQUIRE(good.size() == 2);
    CHECK(good[0].grade == QteGrade::Good);
    CHECK(good[1].amount == 7);

    // Nobody releases: it still completes at its end.
    BeatFight h = beginFight({"hU2 - . . . . . ."});
    h.press(Arrow::Up, 2.0);
    CHECK(countOf(h.update(3.0), BeatEventType::Judged) == 1);
    CHECK(h.stats().perfects == 1);

    // Nobody presses it: a Miss when its press window has passed.
    BeatFight k = beginFight({"hU2 - . . . . . ."});
    const BeatEvents miss = k.update(2.13);
    REQUIRE(miss.size() == 1);
    CHECK(miss[0].grade == QteGrade::Miss);
    CHECK(miss[0].kind == BeatKind::Hold);
    CHECK(miss[0].time == doctest::Approx(2.12));
}

TEST_CASE("beat judging: letting go of a hold early breaks it; within holdSlack of its end it still counts") {
    // Broken: let go 0.3 s before the end (holdSlack is 0.15 s).
    BeatFight f = beginFight({"hU2 - . . . . . ."});
    f.press(Arrow::Up, 2.0);
    const BeatEvents broken = f.release(Arrow::Up, 2.2);
    REQUIRE(broken.size() == 1);
    CHECK(broken[0].type == BeatEventType::Judged);
    CHECK(broken[0].grade == QteGrade::Miss);
    CHECK(broken[0].kind == BeatKind::Hold);
    CHECK(broken[0].time == doctest::Approx(2.2));
    CHECK_FALSE(f.holding());
    CHECK(f.stats().misses == 1);
    CHECK(f.hp() == 1000);
    CHECK(f.enemyHp(0) == 1000); // no damage
    CHECK(f.update(3.0).empty());

    // Within the slack (0.1 s early): complete, at the release.
    BeatFight g = beginFight({"hU2 - . . . . . ."});
    g.press(Arrow::Up, 2.0);
    const BeatEvents slack = g.release(Arrow::Up, 2.4);
    REQUIRE(slack.size() == 2);
    CHECK(slack[0].grade == QteGrade::Perfect);
    CHECK(slack[0].time == doctest::Approx(2.4));
    CHECK(slack[1].amount == 12);

    // The edge: 2.35 s is 0.15 s before the end.
    BeatFight h = beginFight({"hU2 - . . . . . ."});
    h.press(Arrow::Up, 2.0);
    CHECK(h.release(Arrow::Up, 2.34).size() == 1); // just outside: broken
    BeatFight k = beginFight({"hU2 - . . . . . ."});
    k.press(Arrow::Up, 2.0);
    CHECK(k.release(Arrow::Up, 2.36).size() == 2); // just inside: complete

    // Letting go of another arrow does nothing; a wider holdSlack widens the edge.
    BeatFight m = beginFight({"hU2 - . . . . . ."});
    m.press(Arrow::Up, 2.0);
    CHECK(m.release(Arrow::Down, 2.2).empty());
    CHECK(m.holding());
    BeatTuning wide = plain();
    wide.holdSlack = 0.4;
    BeatFight n = beginFight({"hU2 - . . . . . ."}, wide);
    n.press(Arrow::Up, 2.0);
    CHECK(n.release(Arrow::Up, 2.2).size() == 2);
}

TEST_CASE("beat judging: a press or release within holdSlack of a hold's end completes the hold first") {
    // The hold fills slots 0-1 (2.0 s to 2.5 s) and a strike follows at 2.5 s.
    BeatFight f = beginFight({"hU2 - sD . . . . ."});
    f.press(Arrow::Up, 2.0);
    // A press of the next prompt's arrow 0.1 s before the hold ends completes the hold first.
    const BeatEvents e = f.press(Arrow::Down, 2.4);
    REQUIRE(e.size() == 4);
    CHECK(e[0].type == BeatEventType::Judged);
    CHECK(e[0].kind == BeatKind::Hold);
    CHECK(e[0].grade == QteGrade::Perfect);
    CHECK(e[1].type == BeatEventType::EnemyHit);
    CHECK(e[2].type == BeatEventType::Judged);
    CHECK(e[2].kind == BeatKind::Strike);
    CHECK(e[2].grade == QteGrade::Good); // 0.1 s early
    CHECK(e[3].type == BeatEventType::EnemyHit);
    CHECK_FALSE(f.holding());

    // Not close enough to the end (0.2 s): the hold goes on and the press is outside the strike's window.
    BeatFight g = beginFight({"hU2 - sD . . . . ."});
    g.press(Arrow::Up, 2.0);
    const BeatEvents outside = g.press(Arrow::Down, 2.3);
    REQUIRE(outside.size() == 1);
    CHECK(outside[0].type == BeatEventType::Stray);
    CHECK(g.holding());

    // A release of another arrow that close to the end completes it too.
    BeatFight h = beginFight({"hU2 - sD . . . . ."});
    h.press(Arrow::Up, 2.0);
    const BeatEvents r = h.release(Arrow::Left, 2.4);
    REQUIRE(r.size() == 2);
    CHECK(r[0].kind == BeatKind::Hold);
    CHECK_FALSE(h.holding());
}

// ---- the assists and the autopilot ----

TEST_CASE("beat assists: hold toggle latches a hold on its press") {
    QteAssist toggle;
    toggle.holdToggle = true;
    BeatFight f = beginFight({"hU2 - . . . . . ."}, plain(), toggle);
    CHECK(f.press(Arrow::Up, 2.0).empty());
    CHECK(f.release(Arrow::Up, 2.1).empty()); // letting go does nothing
    CHECK(f.holding());
    const BeatEvents done = f.update(2.5);
    REQUIRE(done.size() == 2);
    CHECK(done[0].grade == QteGrade::Perfect);
    CHECK(done[0].time == doctest::Approx(2.5));

    // Without the assist the same release breaks it.
    BeatFight g = beginFight({"hU2 - . . . . . ."});
    g.press(Arrow::Up, 2.0);
    CHECK(g.release(Arrow::Up, 2.1)[0].grade == QteGrade::Miss);
}

TEST_CASE("beat assists: auto-complete plays the autopilot in update and ignores the player") {
    QteAssist assist;
    assist.autoComplete = true;
    BeatFight f;
    f.begin(defaultBeatFightDef(), BeatTuning{}, 0.0, 3, assist);
    Record rec;
    bool ignoredAll = true;
    for (double now = 1.0 / 60.0; !f.over() && now < 60.0; now += 1.0 / 60.0) {
        // Whatever the player does is ignored: it neither answers nor strays.
        const BeatEvents p = f.press(Arrow::Left, now);
        const BeatEvents r = f.release(Arrow::Left, now);
        ignoredAll = ignoredAll && p.empty() && r.empty();
        rec.add(f.update(now));
    }
    CHECK(ignoredAll);
    CHECK(f.phase() == BeatPhase::Victory);
    CHECK(f.stats().strays == 0);
    CHECK(f.stats().whiffs == 0);
    CHECK(f.stats().misses == 0);
    CHECK(f.stats().damageTaken == 0);
    CHECK(f.stats().perfects > 0);
    CHECK(rec.count(BeatEventType::Victory) == 1);
    CHECK(f.hp() == f.maxHp());

    // A huge frame is played in order too: the same fight in one call ends the same way.
    BeatFight g;
    g.begin(defaultBeatFightDef(), BeatTuning{}, 0.0, 3, assist);
    const BeatEvents all = g.update(60.0);
    CHECK(countOf(all, BeatEventType::Victory) == 1);
    CHECK(g.phase() == BeatPhase::Victory);
    CHECK(g.stats().misses == 0);
    CHECK(g.stats().strays == 0);
    CHECK(sameStats(f.stats(), g.stats()));
}

TEST_CASE("beat autopilot: presses on the beat, releases a hold at its end, and a release goes before a press") {
    // A hold ending at 2.5 s and a strike at 2.5 s.
    BeatFight f = beginFight({"hU2 - sD . . . . ."});
    QteAction a = f.autopilot(1.9);
    CHECK(a.kind == QteAction::Kind::None); // waiting
    a = f.autopilot(2.0);
    CHECK(a.kind == QteAction::Kind::Press);
    CHECK(a.input == Arrow::Up);
    CHECK(a.time == doctest::Approx(2.0));
    CHECK(f.press(a.input, a.time).empty());
    CHECK(f.autopilot(2.4).kind == QteAction::Kind::None); // nothing due before 2.5 s
    // At 2.5 s both are due: the release first, then the press.
    a = f.autopilot(2.5);
    CHECK(a.kind == QteAction::Kind::Release);
    CHECK(a.input == Arrow::Up);
    CHECK(a.time == doctest::Approx(2.5));
    CHECK(countOf(f.release(a.input, a.time), BeatEventType::Judged) == 1);
    a = f.autopilot(2.5);
    CHECK(a.kind == QteAction::Kind::Press);
    CHECK(a.input == Arrow::Down);
    CHECK(a.time == doctest::Approx(2.5));
    CHECK(f.press(a.input, a.time).size() == 2);
    CHECK(f.autopilot(2.5).kind == QteAction::Kind::None);

    // Both due at once because the clock was late: the release is still asked for first.
    BeatFight g = beginFight({"hU2 - sD . . . . ."});
    g.press(Arrow::Up, 2.0);
    g.update(2.4);
    a = g.autopilot(3.0);
    CHECK(a.kind == QteAction::Kind::Release);
    CHECK(a.time == doctest::Approx(2.5));
    CHECK(g.autopilot(2.45).kind == QteAction::Kind::None);

    // A late clock gets its move at now(), never in the past.
    BeatFight h = beginFight({"sL . . . . . . ."});
    h.update(2.1); // 0.1 s late: still inside the window
    a = h.autopilot(2.1);
    CHECK(a.kind == QteAction::Kind::Press);
    CHECK(a.time == doctest::Approx(2.1));
    // Once the prompt has run out there is nothing to press.
    h.update(2.2);
    CHECK(h.autopilot(2.2).kind == QteAction::Kind::None);
    CHECK(h.promptCount() == 0);

    // It makes no move that couldn't answer its prompt: with counters locked out right through the
    // window (min(2.0 s, 4 beats = 2.0 s)) it waits, and the counter lands.
    BeatTuning locked = plain();
    locked.counter.parryLockout = 2.0;
    locked.counterLockoutBeats = 4.0;
    BeatFight k = beginFight({"cU . . . . . . ."}, locked);
    CHECK(k.press(Arrow::Up, 1.5)[0].type == BeatEventType::Whiff); // locked out until 3.5
    k.update(2.0);
    CHECK(k.autopilot(2.0).kind == QteAction::Kind::None);
    CHECK(countOf(k.update(2.1), BeatEventType::LeadHit) == 1);

    // None before the fight starts.
    BeatFight idle;
    CHECK(idle.autopilot(100.0).kind == QteAction::Kind::None);
    CHECK(idle.phase() == BeatPhase::NotStarted);
    CHECK(idle.update(5.0).empty());
    CHECK(idle.press(Arrow::Left, 5.0).empty());
}

// ---- chain, multiplier, damage ----

TEST_CASE("beat chain: the multiplier steps every chainPerStep answers, caps, and resets on a Miss") {
    // Eight strikes a bar against a wall of hp: the damage each answer deals is 10 times the
    // multiplier it started with: x1 for the first 8, then x2, x3 and x4 (the cap).
    BeatFightDef def = oneEnemy({"sL sD sU sR sL sD sU sR"}, 100000, 10);
    def.hp = 100000;
    BeatFight f;
    f.begin(def, plain(), 0.0, 1);
    Record rec;
    playFrames(f, rec, 11.9, noSkip); // five bars (the sixth starts at 12.0 s): 40 answers
    CHECK(f.stats().perfects == 40);
    CHECK(f.stats().maxChain == 40);
    CHECK(f.chain() == 40);
    CHECK(f.multiplier() == 4);

    std::vector<int> amounts;
    std::vector<int> steps;
    std::vector<int> order; // 0 a hit, 1 a multiplier event, in sequence
    for (const BeatEvent& e : rec.events) {
        if (e.type == BeatEventType::EnemyHit) {
            amounts.push_back(e.amount);
            order.push_back(0);
        } else if (e.type == BeatEventType::Multiplier) {
            steps.push_back(e.amount);
            order.push_back(1);
        }
    }
    REQUIRE(amounts.size() == 40);
    for (std::size_t i = 0; i < 40; ++i) {
        CAPTURE(i);
        const int m = std::min(4, 1 + static_cast<int>(i) / 8); // the multiplier before answer i counts
        CHECK(amounts[i] == 10 * m);
    }
    // Multiplier events come as the chain reaches 8, 16 and 24 (right after that answer's hit), and
    // never again at the cap.
    REQUIRE(steps.size() == 3);
    CHECK(steps[0] == 2);
    CHECK(steps[1] == 3);
    CHECK(steps[2] == 4);
    std::size_t hitsSeen = 0;
    std::size_t stepsSeen = 0;
    for (const int what : order) {
        if (what == 0) {
            ++hitsSeen;
        } else {
            ++stepsSeen;
            CHECK(hitsSeen == 8u * stepsSeen);
        }
    }

    // A Miss resets the chain and the multiplier, and an event says so. Prompt 12 is missed (x2 before).
    BeatFight g;
    g.begin(def, plain(), 0.0, 1);
    Record run;
    playFrames(g, run, 4.9, [](const BeatPrompt& p) { return p.id == 12; });
    CHECK(g.stats().misses == 1);
    CHECK(g.stats().maxChain == 11);
    CHECK(g.chain() == 0);
    CHECK(g.multiplier() == 1);
    std::vector<int> seen;
    for (const BeatEvent& e : run.events)
        if (e.type == BeatEventType::Multiplier) seen.push_back(e.amount);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == 2); // the chain reached 8
    CHECK(seen[1] == 1); // the Miss

    // A Miss at x1 changes no multiplier: no event.
    BeatFight k = beginFight({"sL . . . . . . ."});
    CHECK(countOf(k.update(2.2), BeatEventType::Multiplier) == 0);
    CHECK(k.stats().misses == 1);

    // Strays, whiffs and ignored presses don't touch the chain.
    BeatFight m = beginFight({"sL sD . . . . . ."});
    m.press(Arrow::Left, 2.0);
    CHECK(m.chain() == 1);
    m.press(Arrow::Right, 2.1);
    m.press(Arrow::Up, 1.0);
    CHECK(m.chain() == 1);
}

TEST_CASE("beat damage: the formulas, goodFactor rounding and the multiplier") {
    // strikeDamage 10, counterDamage 14, holdDamage 6 per eighth: Perfect, and round(x * 0.6) for Good.
    struct Case {
        const char* pattern;
        Arrow arrow;
        double offset;
        int damage;
    };
    const Case cases[] = {
        {"sL . . . . . . .", Arrow::Left, 0.0, 10},  {"sL . . . . . . .", Arrow::Left, 0.1, 6},
        {"cU . . . . . . .", Arrow::Up, 0.0, 14},    {"cU . . . . . . .", Arrow::Up, -0.15, 8},  // 8.4 -> 8
        {"hU2 - . . . . . .", Arrow::Up, 0.0, 12},   {"hU2 - . . . . . .", Arrow::Up, 0.1, 7},   // 7.2 -> 7
        {"hU3 - - . . . . .", Arrow::Up, 0.0, 18},   {"hU3 - - . . . . .", Arrow::Up, 0.1, 11},  // 10.8 -> 11
        {"hU8 - - - - - - -", Arrow::Up, 0.0, 48},   {"hU8 - - - - - - -", Arrow::Up, 0.1, 29},  // 28.8 -> 29
    };
    for (const Case& c : cases) {
        CAPTURE(c.pattern);
        CAPTURE(c.offset);
        BeatFight f = beginFight({c.pattern});
        // A strike or a counter is answered by the press, a hold when it ends.
        const BeatEvents pressed = f.press(c.arrow, 2.0 + c.offset);
        const BeatEvents later = f.update(5.0);
        const BeatEvent* hit = findEvent(pressed, BeatEventType::EnemyHit);
        if (!hit) hit = findEvent(later, BeatEventType::EnemyHit);
        REQUIRE(hit != nullptr);
        CHECK(hit->amount == c.damage);
        CHECK(f.enemyHp(0) == 1000 - c.damage);
    }

    // Round half away from zero: strikeDamage 5, counterDamage 7, holdDamage 3 with goodFactor 0.5 give
    // 2.5 -> 3, 3.5 -> 4 and (3 * 3 eighths) 4.5 -> 5.
    BeatTuning half = plain();
    half.goodFactor = 0.5;
    BeatFightDef def = oneEnemy({"sL . cU . hD3 - - ."}, 1000, 10);
    def.hp = 1000;
    def.strikeDamage = 5;
    def.counterDamage = 7;
    def.holdDamage = 3;
    BeatFight f;
    f.begin(def, half, 0.0, 1);
    f.update(1.0);
    const BeatEvents strike = f.press(Arrow::Left, 2.1);
    CHECK(findEvent(strike, BeatEventType::EnemyHit)->amount == 3);
    const BeatEvents counter = f.press(Arrow::Up, 2.5 - 0.1);
    CHECK(findEvent(counter, BeatEventType::EnemyHit)->amount == 4);
    f.press(Arrow::Down, 3.0 + 0.1);
    const BeatEvents hold = f.update(5.0);
    CHECK(findEvent(hold, BeatEventType::EnemyHit)->amount == 5);

    // The multiplier multiplies the rounded Good damage: with chainPerStep 1 the second answer is x2.
    BeatTuning steep = half;
    steep.chainPerStep = 1;
    steep.maxMultiplier = 3;
    BeatFightDef two = oneEnemy({"sL sD sU sR . . . ."}, 1000, 10);
    two.hp = 1000;
    two.strikeDamage = 5;
    BeatFight g;
    g.begin(two, steep, 0.0, 1);
    g.update(1.0);
    CHECK(findEvent(g.press(Arrow::Left, 2.0), BeatEventType::EnemyHit)->amount == 5);        // Perfect x1
    CHECK(findEvent(g.press(Arrow::Down, 2.25 + 0.1), BeatEventType::EnemyHit)->amount == 6); // Good: 3 x2
    CHECK(findEvent(g.press(Arrow::Up, 2.5), BeatEventType::EnemyHit)->amount == 15);         // Perfect x3
    CHECK(findEvent(g.press(Arrow::Right, 2.75), BeatEventType::EnemyHit)->amount == 15);     // capped at x3
}

// ---- the finisher ----

TEST_CASE("beat finisher: a low enemy's next bar is a run of strikes that takes the rest of its hp") {
    // An enemy with a lot of hp, so the autopilot has time to wear it down (against the default enemy
    // it wins before it ever gets low). Four strikes a bar, then four empty slots.
    BeatFightDef def = oneEnemy({"sL sD sU sR . . . ."}, 400, 10);
    def.hp = 100000;
    BeatFight f;
    f.begin(def, BeatTuning{}, 0.0, 2);
    Record rec;
    bool started = false;
    int hpAtStart = 0;
    for (double now = 1.0 / 60.0; !f.over() && now < 200.0; now += 1.0 / 60.0) {
        for (int guard = 0; guard < 64; ++guard) {
            const QteAction a = f.autopilot(now);
            if (a.kind == QteAction::Kind::None) break;
            rec.add(a.kind == QteAction::Kind::Press ? f.press(a.input, a.time) : f.release(a.input, a.time));
        }
        const BeatEvents e = f.update(now);
        rec.add(e);
        if (!started && countOf(e, BeatEventType::FinisherStart) == 1) {
            started = true;
            hpAtStart = f.enemyHp(0);
            // The bar is a finisher: finisherLength strikes on consecutive eighths from the bar's start,
            // with random arrows, no arrow twice in a row.
            CHECK(findEvent(e, BeatEventType::FinisherStart)->enemy == 0);
            REQUIRE(f.promptCount() == 6);
            const double start = f.prompt(0).time;
            for (std::size_t i = 0; i < f.promptCount(); ++i) {
                const BeatPrompt& p = f.prompt(i);
                CHECK(p.finisher);
                CHECK(p.kind == BeatKind::Strike);
                CHECK(p.enemy == 0);
                CHECK(p.time == doctest::Approx(start + 0.25 * static_cast<double>(i)));
                if (i > 0) CHECK(p.input != f.prompt(i - 1).input);
            }
            // It is sent when the bar is generated: approachBeats (1.0 s) before the bar starts.
            CHECK(findEvent(e, BeatEventType::FinisherStart)->time == doctest::Approx(start - 1.0));
        }
    }
    REQUIRE(started);
    CHECK(hpAtStart > 0);
    CHECK(hpAtStart <= 100); // a quarter of 400

    // The finisher's six strikes deal ceil(hp / strikes left), so the last one takes the rest; and no
    // multiplier (the chain is long by now).
    std::vector<int> amounts;
    bool after = false;
    for (const BeatEvent& e : rec.events) {
        if (e.type == BeatEventType::FinisherStart) after = true;
        if (after && e.type == BeatEventType::EnemyHit) amounts.push_back(e.amount);
    }
    REQUIRE(amounts.size() == 6);
    int hp = hpAtStart;
    for (int left = 6; left >= 1; --left) {
        const int expect = (hp + left - 1) / left;
        CHECK(amounts[static_cast<std::size_t>(6 - left)] == expect);
        hp -= expect;
    }
    CHECK(hp == 0);
    CHECK(f.multiplier() > 1); // it was not the multiplier that made them
    CHECK(f.stats().finishers == 1);
    CHECK(f.phase() == BeatPhase::Victory);
    CHECK(rec.count(BeatEventType::EnemyDown) == 1);
    CHECK(rec.count(BeatEventType::FinisherBroken) == 0);
    CHECK_FALSE(f.enemyAlive(0));
}

TEST_CASE("beat finisher: a Miss breaks it, drops its other strikes and leaves the enemy its hp for a later turn") {
    BeatTuning t = plain();
    t.finisherAt = 1.0; // every bar of this enemy is a finisher
    t.finisherLength = 4;
    BeatFightDef def = oneEnemy({"cL . . . . . . ."}, 100, 10);
    def.hp = 1000;
    BeatFight f;
    f.begin(def, t, 0.0, 3);
    const BeatEvents generated = f.update(1.0);
    CHECK(countOf(generated, BeatEventType::FinisherStart) == 1);
    REQUIRE(f.promptCount() == 4);
    Arrow arrows[4];
    for (std::size_t i = 0; i < 4; ++i) {
        arrows[i] = f.prompt(i).input;
        CHECK(f.prompt(i).finisher);
        CHECK(f.prompt(i).time == doctest::Approx(2.0 + 0.25 * static_cast<double>(i)));
    }
    // Two strikes answered: ceil(100 / 4) = 25, then ceil(75 / 3) = 25.
    CHECK(findEvent(f.press(arrows[0], 2.0), BeatEventType::EnemyHit)->amount == 25);
    CHECK(findEvent(f.press(arrows[1], 2.25), BeatEventType::EnemyHit)->amount == 25);
    CHECK(f.enemyHp(0) == 50);
    // The third is not pressed: a Miss, the finisher is broken, the fourth strike is dropped silently.
    const BeatEvents broken = f.update(2.5 + 0.121);
    CHECK(countOf(broken, BeatEventType::Judged) == 1);
    CHECK(findEvent(broken, BeatEventType::Judged)->grade == QteGrade::Miss);
    REQUIRE(countOf(broken, BeatEventType::FinisherBroken) == 1);
    CHECK(findEvent(broken, BeatEventType::FinisherBroken)->enemy == 0);
    CHECK(f.promptCount() == 0);
    CHECK(f.update(2.9).empty());
    CHECK(f.enemyHp(0) == 50);
    CHECK(f.stats().finishers == 0);
    CHECK(f.stats().misses == 1);
    CHECK(f.chain() == 0);
    CHECK(f.hp() == 1000);
    // On its next turn the enemy is still low: another finisher. This one is answered: ceil(50 / 4) = 13,
    // ceil(37 / 3) = 13, ceil(24 / 2) = 12, 12.
    const BeatEvents again = f.update(3.0);
    CHECK(countOf(again, BeatEventType::FinisherStart) == 1);
    REQUIRE(f.promptCount() == 4);
    const int expect[] = {13, 13, 12, 12};
    for (std::size_t i = 0; i < 4; ++i) {
        const BeatPrompt p = f.prompt(0);
        const BeatEvents e = f.press(p.input, p.time);
        CHECK(findEvent(e, BeatEventType::EnemyHit)->amount == expect[i]);
        if (i == 3) {
            CHECK(countOf(e, BeatEventType::EnemyDown) == 1);
            CHECK(countOf(e, BeatEventType::Victory) == 1);
        }
    }
    CHECK(f.stats().finishers == 1);
    CHECK(f.phase() == BeatPhase::Victory);
}

TEST_CASE("beat finisher: skipping a strike breaks it, and the press that skipped has nothing left to answer") {
    // Wide strike windows (0.2 s, up to a whole eighth) make neighbouring strikes overlap, so a press of
    // the second strike's arrow can arrive inside both windows: the skipped note is a Miss, which breaks
    // the finisher and drops the second strike with it.
    BeatTuning t = plain();
    t.finisherAt = 1.0;
    t.finisherLength = 3;
    t.strikeCapEighths = 1.0;
    t.strike.good = 0.2;
    BeatFightDef def = oneEnemy({"cL . . . . . . ."}, 100, 10);
    def.hp = 1000;
    BeatFight f;
    f.begin(def, t, 0.0, 3);
    f.update(1.0);
    REQUIRE(f.promptCount() == 3);
    const Arrow other = f.prompt(1).input; // the strike at 2.25 s; finisher arrows never repeat
    REQUIRE(other != f.prompt(0).input);
    const BeatEvents e = f.press(other, 2.1); // inside the windows of the strikes at 2.0 s and 2.25 s
    REQUIRE(e.size() == 3);
    CHECK(e[0].type == BeatEventType::Judged);
    CHECK(e[0].grade == QteGrade::Miss);
    CHECK(e[1].type == BeatEventType::FinisherBroken);
    CHECK(e[2].type == BeatEventType::Stray);
    CHECK(f.promptCount() == 0);
    CHECK(f.stats().strays == 1);
    CHECK(f.stats().finishers == 0);
    CHECK(f.enemyHp(0) == 100);
}

TEST_CASE("beat finisher: an enemy is low at finisherAt of its max hp or less, and not above it") {
    // 100 hp, finisherAt 0.25: after one hit of 74 it has 26 (not low), after one of 75 it has 25 (low).
    for (const int damage : {74, 75}) {
        CAPTURE(damage);
        BeatTuning t = plain();
        t.finisherAt = 0.25;
        BeatFightDef def = oneEnemy({"sL . . . . . . ."}, 100, 10);
        def.hp = 1000;
        def.strikeDamage = damage;
        BeatFight f;
        f.begin(def, t, 0.0, 1);
        CHECK(countOf(f.update(1.0), BeatEventType::FinisherStart) == 0); // full hp: a pattern
        REQUIRE(f.promptCount() == 1);
        CHECK_FALSE(f.prompt(0).finisher);
        f.press(Arrow::Left, 2.0);
        CHECK(f.enemyHp(0) == 100 - damage);
        const BeatEvents next = f.update(3.0); // the next bar is generated
        CHECK(countOf(next, BeatEventType::FinisherStart) == (damage == 75 ? 1 : 0));
        REQUIRE(f.promptCount() >= 1);
        CHECK(f.prompt(f.promptCount() - 1).finisher == (damage == 75));
    }
    // finisherAt 0 switches finishers off.
    BeatTuning off = plain();
    BeatFightDef def = oneEnemy({"sL . . . . . . ."}, 100, 10);
    def.hp = 1000;
    def.strikeDamage = 99;
    BeatFight f;
    f.begin(def, off, 0.0, 1);
    f.update(1.0);
    f.press(Arrow::Left, 2.0);
    CHECK(countOf(f.update(3.0), BeatEventType::FinisherStart) == 0);
}

// ---- down enemies and the end ----

TEST_CASE("beat end: an enemy at 0 hp is down and its open prompts are dropped silently") {
    // Enemy 0 has 10 hp and three strikes; enemy 1 keeps the fight going.
    BeatFightDef def = defOf({{10, 10, {"sL sD sU . . . . ."}}, {1000, 10, {"sL . . . . . . ."}}});
    def.hp = 1000;
    BeatFight f;
    f.begin(def, plain(), 0.0, 1);
    f.update(1.0);
    REQUIRE(f.promptCount() == 3);
    const BeatEvents e = f.press(Arrow::Left, 2.0); // 10 damage: down
    REQUIRE(e.size() == 3);
    CHECK(e[0].type == BeatEventType::Judged);
    CHECK(e[1].type == BeatEventType::EnemyHit);
    CHECK(e[2].type == BeatEventType::EnemyDown);
    CHECK(e[2].enemy == 0);
    CHECK_FALSE(f.enemyAlive(0));
    CHECK(f.enemyHp(0) == 0);
    CHECK(f.promptCount() == 0); // the other two strikes are gone
    const BeatEvents later = f.update(2.6);
    CHECK(later.empty()); // and nothing is judged for them
    CHECK(f.stats().misses == 0);
    CHECK(f.phase() == BeatPhase::Playing);
    CHECK(f.enemyAlive(1));
}

TEST_CASE("beat end: every enemy down is Victory, the lead at 0 hp is Defeat, and then nothing moves") {
    // Victory: the autopilot against the built-in fight.
    BeatFight v;
    v.begin(defaultBeatFightDef(), BeatTuning{}, 0.0, 5);
    Record rec;
    playFrames(v, rec, 60.0, noSkip);
    REQUIRE(v.phase() == BeatPhase::Victory);
    CHECK(v.over());
    CHECK(rec.count(BeatEventType::Victory) == 1);
    CHECK(rec.count(BeatEventType::Defeat) == 0);
    CHECK(rec.events.back().type == BeatEventType::Victory);
    CHECK(v.hp() == v.maxHp());
    CHECK(v.promptCount() == 0);
    {
        const BeatStats before = v.stats();
        const double now = v.now();
        CHECK(v.update(now + 100.0).empty());
        CHECK(v.press(Arrow::Left, now + 101.0).empty());
        CHECK(v.release(Arrow::Left, now + 102.0).empty());
        CHECK(sameStats(before, v.stats()));
        CHECK(v.now() == now);
        CHECK(v.phase() == BeatPhase::Victory);
        CHECK(v.autopilot(now + 100.0).kind == QteAction::Kind::None);
        CHECK(v.turnEnemy() == -1);
    }

    // Defeat: no input at all against the same fight.
    BeatFight d;
    d.begin(defaultBeatFightDef(), BeatTuning{}, 0.0, 5);
    Record lost;
    for (double now = 1.0 / 60.0; !d.over() && now < 200.0; now += 1.0 / 60.0) lost.add(d.update(now));
    REQUIRE(d.phase() == BeatPhase::Defeat);
    CHECK(lost.count(BeatEventType::Defeat) == 1);
    CHECK(lost.count(BeatEventType::Victory) == 0);
    CHECK(lost.events.back().type == BeatEventType::Defeat);
    CHECK(d.hp() == 0);
    CHECK(d.stats().damageTaken == 100); // ten counters of 10
    CHECK(lost.count(BeatEventType::LeadHit) == 10);
    {
        const BeatStats before = d.stats();
        const double now = d.now();
        CHECK(d.update(now + 100.0).empty());
        CHECK(d.press(Arrow::Left, now + 101.0).empty());
        CHECK(d.release(Arrow::Left, now + 102.0).empty());
        CHECK(sameStats(before, d.stats()));
        CHECK(d.now() == now);
        CHECK(d.phase() == BeatPhase::Defeat);
    }

    // Defeat ends a call at once: one long frame stops at the killing hit (the lead has 20 hp: two counters).
    BeatFightDef frail = defaultBeatFightDef();
    frail.hp = 20;
    BeatFight k;
    k.begin(frail, BeatTuning{}, 0.0, 5);
    const BeatEvents all = k.update(500.0);
    CHECK(countOf(all, BeatEventType::Defeat) == 1);
    CHECK(all[all.size() - 1].type == BeatEventType::Defeat);
    CHECK(countOf(all, BeatEventType::LeadHit) == 2);
    CHECK(k.stats().damageTaken == 20);
    CHECK(k.stats().bars <= 3);
    CHECK(k.phase() == BeatPhase::Defeat);
}

TEST_CASE("beat end: restart replays the same bars, keeps the stats and counts itself") {
    // Plenty of hp on both sides, so the fight just runs; three patterns, so the picks matter.
    BeatFightDef def = oneEnemy({"sL . . . cU . . .", "sD . sU . . . cL .", "cR . . . sL . . ."}, 100000, 1);
    def.hp = 100000;
    BeatFight f;
    f.begin(def, plain(), 0.0, 9);
    std::vector<std::string> first;
    for (int bar = 0; bar < 12; ++bar) {
        f.update(barStart(f, bar) - 0.5);
        first.push_back(barSignature(f, bar));
    }
    const BeatStats before = f.stats();
    CHECK(before.damageTaken > 0);
    CHECK(before.restarts == 0);

    f.restart(100.0); // 100 s is beat 200, a bar line: the first bar is a bar later, beat 204
    CHECK(f.phase() == BeatPhase::Playing);
    CHECK(f.now() == 100.0);
    CHECK(f.firstBarBeat() == doctest::Approx(204.0));
    CHECK(f.hp() == f.maxHp());
    CHECK(f.enemyHp(0) == f.enemyMaxHp(0));
    CHECK(f.chain() == 0);
    CHECK(f.promptCount() == 0);
    CHECK(f.stats().restarts == 1);
    CHECK(f.stats().damageTaken == before.damageTaken); // the stats keep counting
    CHECK(f.stats().bars == before.bars);
    std::vector<std::string> second;
    for (int bar = 0; bar < 12; ++bar) {
        f.update(barStart(f, bar) - 0.5);
        second.push_back(barSignature(f, bar));
    }
    CHECK(first == second);
    CHECK(f.stats().damageTaken > before.damageTaken);
    // A fresh fight with the same seed plays them too, and another seed doesn't.
    BeatFight g;
    g.begin(def, plain(), 0.0, 9);
    std::vector<std::string> fresh;
    for (int bar = 0; bar < 12; ++bar) {
        g.update(barStart(g, bar) - 0.5);
        fresh.push_back(barSignature(g, bar));
    }
    CHECK(first == fresh);
    BeatFight h;
    h.begin(def, plain(), 0.0, 10);
    std::vector<std::string> other;
    for (int bar = 0; bar < 12; ++bar) {
        h.update(barStart(h, bar) - 0.5);
        other.push_back(barSignature(h, bar));
    }
    CHECK(first != other);

    // After a Defeat, a retry plays the same bars again too.
    BeatFightDef weak = oneEnemy({"sL . . . cU . . .", "sD . sU . . . cL .", "cR . . . sL . . ."}, 100000, 50);
    weak.hp = 100;
    BeatFight r;
    r.begin(weak, plain(), 0.0, 4);
    std::vector<std::string> run1;
    for (int bar = 0; !r.over() && bar < 40; ++bar) {
        r.update(barStart(r, bar) - 0.5);
        run1.push_back(barSignature(r, bar));
    }
    REQUIRE(r.phase() == BeatPhase::Defeat);
    r.restart(r.now() + 1.0);
    CHECK(r.phase() == BeatPhase::Playing);
    std::vector<std::string> run2;
    for (int bar = 0; !r.over() && bar < 40; ++bar) {
        r.update(barStart(r, bar) - 0.5);
        run2.push_back(barSignature(r, bar));
    }
    CHECK(r.phase() == BeatPhase::Defeat);
    REQUIRE(run1.size() >= 2);
    CHECK(run1 == run2);
    CHECK(r.stats().restarts == 1);
}

TEST_CASE("beat clock: the clock never runs back, and events past the capacity are counted") {
    BeatFight f = beginFight({"sL . . . . . . ."});
    f.update(5.0);
    CHECK(f.now() == 5.0);
    CHECK(f.update(4.0).empty());
    CHECK(f.now() == 5.0);
    // A press with an earlier time is read as now().
    const BeatEvents e = f.press(Arrow::Left, 1.0);
    REQUIRE(e.size() == 1);
    CHECK(e[0].type == BeatEventType::Stray);
    CHECK(e[0].time == 5.0);

    // Eight misses a bar over five bars is 40 events in one call: 32 are kept, the rest counted.
    BeatFightDef def = oneEnemy({"sL sD sU sR sL sD sU sR"}, 1000, 10);
    def.hp = 100000;
    BeatFight g;
    g.begin(def, plain(), 0.0, 1);
    const BeatEvents many = g.update(12.0);
    CHECK(many.size() == kMaxBeatEvents);
    CHECK(g.droppedEvents() == 8);
    CHECK(g.stats().misses == 40); // the state didn't depend on them: it all happened
    CHECK(g.stats().bars == 6);
}

// ---- determinism and balance ----

TEST_CASE("beat determinism: the same seed, inputs and times give the same events") {
    BeatFightDef def = defOf({{60, 10, {"sL . sR . cU . . .", "cD . . . sL . hU2 -"}},
                              {90, 14, {"cL . sD sD . cR . .", "sU . cD . sU . cL .", "cR . . . hD4 - - -"}}});
    def.hp = 150;
    const auto run = [&](std::uint32_t seed, std::vector<BeatEvent>& out) {
        BeatFight f;
        f.begin(def, BeatTuning{}, 0.0, seed);
        std::uint32_t rng = 0x9e3779b9u ^ seed; // a scripted player: presses and releases at random
        const auto next = [&rng]() {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return rng;
        };
        for (double now = 1.0 / 60.0; !f.over() && now < 120.0; now += 1.0 / 60.0) {
            for (int k = 0; k < 2; ++k) {
                const std::uint32_t r = next();
                if ((r & 7u) == 0u) {
                    const Arrow arrow = kArrows[(r >> 3) & 3u];
                    const BeatEvents e = (r & 0x100u) ? f.press(arrow, now - 0.01) : f.release(arrow, now - 0.02);
                    out.insert(out.end(), e.begin(), e.end());
                }
            }
            // And half the time it plays the autopilot's next move.
            if ((next() & 1u) != 0u) {
                const QteAction a = f.autopilot(now);
                if (a.kind != QteAction::Kind::None) {
                    const BeatEvents e = a.kind == QteAction::Kind::Press ? f.press(a.input, a.time) : f.release(a.input, a.time);
                    out.insert(out.end(), e.begin(), e.end());
                }
            }
            const BeatEvents e = f.update(now);
            out.insert(out.end(), e.begin(), e.end());
        }
        return f.stats();
    };
    std::vector<BeatEvent> a, b, c;
    const BeatStats sa = run(7, a);
    const BeatStats sb = run(7, b);
    run(8, c);
    REQUIRE(a.size() > 30);
    REQUIRE(a.size() == b.size());
    bool same = true;
    for (std::size_t i = 0; i < a.size(); ++i) same = same && sameEvent(a[i], b[i]);
    CHECK(same);
    CHECK(sameStats(sa, sb));
    // A different seed plays a different fight.
    bool differs = a.size() != c.size();
    for (std::size_t i = 0; !differs && i < a.size(); ++i) differs = !sameEvent(a[i], c[i]);
    CHECK(differs);

    // A copied fight carries its random state: it plays on exactly as the original does.
    BeatFight f;
    f.begin(def, BeatTuning{}, 0.0, 11);
    f.update(30.0);
    BeatFight copy = f;
    for (double now = 31.0; now < 70.0 && !f.over(); now += 1.0) {
        const BeatEvents x = f.update(now);
        const BeatEvents y = copy.update(now);
        REQUIRE(x.size() == y.size());
        for (std::size_t i = 0; i < x.size(); ++i) CHECK(sameEvent(x[i], y[i]));
    }
    CHECK(sameStats(f.stats(), copy.stats()));
}

TEST_CASE("beat balance: the autopilot wins and no input loses, within 64 bars, in every fight") {
    std::vector<std::pair<std::string, BeatSetup>> fights;
    {
        BeatSetup s;
        s.def = defaultBeatFightDef();
        fights.emplace_back("built-in default", s);
    }
    std::string text;
    if (!ftcore_test::readProjectData("Battle/beat.json", text)) {
        FAIL("Data/Battle/beat.json not found");
    }
    BeatTable table;
    std::string error;
    REQUIRE_MESSAGE(importBeatTable(text, table, &error), error);
    fights.emplace_back("beat.json default", table.fallback);
    for (const BeatEntry& e : table.entries) fights.emplace_back("beat.json " + e.scene, e.setup);
    // Every enemy of the file on its own, and all of them together, at several tempos.
    for (const double bpm : {60.0, 90.0, 120.0, 150.0, 180.0, 240.0}) {
        for (std::size_t i = 0; i <= table.enemies.size(); ++i) {
            BeatSetup s = table.fallback;
            s.def.bpm = bpm;
            if (i < table.enemies.size()) {
                s.def.enemyCount = 1;
                s.def.enemies[0] = table.enemies[i];
            } else {
                s.def.enemyCount = static_cast<std::uint8_t>(std::min(table.enemies.size(), kMaxBeatEnemies));
                for (std::size_t k = 0; k < s.def.enemyCount; ++k) s.def.enemies[k] = table.enemies[k];
            }
            const std::string name = (i < table.enemies.size() ? table.enemyIds[i] : std::string("all")) + " at " +
                                     std::to_string(static_cast<int>(bpm)) + " bpm";
            fights.emplace_back(name, s);
        }
    }
    for (const auto& entry : fights) {
        CAPTURE(entry.first);
        for (std::uint32_t seed = 1; seed <= 6; ++seed) {
            CAPTURE(seed);
            const Outcome win = playAutopilot(entry.second, seed);
            CHECK(win.phase == BeatPhase::Victory);
            CHECK(win.bars <= 64);
            // The autopilot is perfect: no stray, no Miss, no damage taken.
            CHECK(win.stats.misses == 0);
            CHECK(win.stats.strays == 0);
            CHECK(win.stats.whiffs == 0);
            CHECK(win.stats.goods == 0);
            CHECK(win.hpLeft == entry.second.def.hp);

            const Outcome lose = playNothing(entry.second, seed);
            CHECK(lose.phase == BeatPhase::Defeat);
            CHECK(lose.bars <= 64);
        }
    }
    // And at other frame rates.
    for (const double frame : {1.0 / 30.0, 0.05, 0.13}) {
        CAPTURE(frame);
        BeatSetup s;
        s.def = defaultBeatFightDef();
        const Outcome win = playAutopilot(s, 3, frame);
        CHECK(win.phase == BeatPhase::Victory);
        CHECK(win.stats.strays == 0);
    }
}

// ---- allocation ----

TEST_CASE("beat: update, press, release and autopilot allocate nothing over a whole fight") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    // Everything that allocates (the table) is made first.
    std::string text;
    if (!ftcore_test::readProjectData("Battle/beat.json", text)) {
        FAIL("Data/Battle/beat.json not found");
    }
    BeatTable table;
    std::string error;
    REQUIRE_MESSAGE(importBeatTable(text, table, &error), error);

    const BeatFightDef won = defaultBeatFightDef();
    const BeatFightDef lost = defaultBeatFightDef();
    const BeatTuning tuning{};
    QteAssist toggle;
    toggle.holdToggle = true;
    QteAssist autoPlay;
    autoPlay.autoComplete = true;
    BeatPattern parsed;

    int sink = 0;
    const std::uint64_t before = ftcore_test::allocStats().count;
    // A won fight: the autopilot, with a stray press and a stray release thrown in each frame.
    {
        BeatFight f;
        f.begin(won, tuning, 0.0, 1);
        for (double now = 1.0 / 60.0; !f.over() && now < 90.0; now += 1.0 / 60.0) {
            sink += static_cast<int>(f.press(Arrow::Down, now).size());
            sink += static_cast<int>(f.release(Arrow::Up, now).size());
            for (int guard = 0; guard < 16; ++guard) {
                const QteAction a = f.autopilot(now);
                if (a.kind == QteAction::Kind::None) break;
                sink += static_cast<int>(a.kind == QteAction::Kind::Press ? f.press(a.input, a.time).size()
                                                                          : f.release(a.input, a.time).size());
            }
            sink += static_cast<int>(f.update(now).size());
        }
        sink += static_cast<int>(f.phase());
    }
    // A lost fight, and a retry of it; holds toggled; the autopilot inside update().
    {
        BeatFight f;
        f.begin(lost, tuning, 0.0, 2);
        sink += static_cast<int>(f.update(200.0).size());
        f.restart(210.0);
        sink += static_cast<int>(f.update(300.0).size());
        BeatFight g;
        g.begin(table.fallback.def, table.fallback.tuning, 0.0, 3, toggle);
        for (double now = 1.0 / 60.0; !g.over() && now < 90.0; now += 1.0 / 60.0) {
            sink += static_cast<int>(g.press(Arrow::Left, now).size());
            sink += static_cast<int>(g.release(Arrow::Left, now).size());
            sink += static_cast<int>(g.update(now).size());
        }
        BeatFight h;
        h.begin(table.fallback.def, table.fallback.tuning, 0.0, 4, autoPlay);
        sink += static_cast<int>(h.update(60.0).size());
        sink += static_cast<int>(h.phase());
    }
    // Looking up a fight and parsing a pattern, good or bad, with no error text asked for.
    sink += static_cast<int>(table.find("SC-X", 1, 0).def.enemyCount);
    sink += table.findEntry("SC-X", 1, 0) == nullptr ? 1 : 0;
    sink += parseBeatPattern("sL . sR . cU . . .", parsed) ? 1 : 0;
    sink += parseBeatPattern("sL . sR", parsed) ? 1 : 0;
    sink += parseBeatPattern("hU9 - - - - - - -", parsed) ? 1 : 0;
    sink += static_cast<int>(defaultBeatFightDef().enemyCount);
    const std::uint64_t allocs = ftcore_test::allocStats().count - before;
    CHECK(allocs == 0);
    CHECK(sink > 0);
}
