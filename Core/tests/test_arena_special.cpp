#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaSpecial.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaTable shippedArena() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

const ArenaTable& arena() {
    static const ArenaTable t = shippedArena();
    return t;
}

ArenaSpecialTable shippedSpecials() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena_special.json", text)) FAIL("Data/Battle/arena_special.json not found");
    ArenaSpecialTable t;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importArenaSpecials(text, t, &err, &warnings), err);
    CHECK(warnings.empty());
    return t;
}

ArenaSpecialTable specialsFrom(const std::string& json) {
    ArenaSpecialTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaSpecials(json, t, &err), err);
    return t;
}

// A fight of shipped enemies, by id, with the lead's kit.
ArenaDef fightOf(std::initializer_list<const char*> ids, std::uint8_t attackers = 1, const char* lead = "devin") {
    const ArenaTable& table = arena();
    ArenaDef def;
    def.fighter = table.fighter(lead);
    def.enemyCount = 0;
    for (const char* id : ids)
        for (std::size_t e = 0; e < table.enemyIds.size(); ++e)
            if (table.enemyIds[e] == id) {
                def.enemyKind[def.enemyCount] = static_cast<std::uint8_t>(e);
                def.enemies[def.enemyCount++] = table.enemies[e];
            }
    REQUIRE(def.enemyCount == ids.size());
    def.attackers = attackers;
    def.ringRadius = 30.f;
    def.spawnDistance = 7.f;
    return def;
}

struct Rig {
    ArenaFight fight;
    ArenaBodies bodies;

    void begin(const ArenaDef& def, std::uint32_t seed = 1) {
        const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 60.f});
        fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, seed);
        bodies = ArenaBodies{};
        bodies.player = fight.playerStart();
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    }
    const ArenaOutput& tick(const ArenaInput& in = ArenaInput{}, float dt = kDt) {
        const ArenaOutput& o = fight.tick(dt, in, bodies);
        arenaStep(fight, bodies, dt);
        return o;
    }
    const ArenaOutput& press(std::uint8_t lead = kArenaPlayer, float dt = kDt) {
        ArenaInput in;
        in.special = lead;
        return tick(in, dt);
    }
    // Ticks until the special ends; returns the ticks it took (the cut back's included).
    int runOut(float dt = kDt, bool skip = false) {
        int ticks = 0;
        while (fight.frozen() && ticks < 100000) {
            ArenaInput in;
            in.specialSkip = skip;
            tick(in, dt);
            ++ticks;
        }
        return ticks;
    }
};

LeadSpecialGauges storeWith(const char* lead, float value, bool seen = false) {
    LeadSpecialGauges g;
    LeadSpecialGauges::Lead* e = g.entry(lead);
    REQUIRE(e != nullptr);
    e->value = value;
    e->seen = seen;
    return g;
}

int countEvents(const ArenaOutput& o, ArenaEventType type) {
    int c = 0;
    for (std::size_t i = 0; i < o.events.count; ++i)
        if (o.events.items[i].type == type) ++c;
    return c;
}

const ArenaEvent* findEvent(const ArenaOutput& o, ArenaEventType type, std::uint8_t target = kArenaNobody) {
    for (std::size_t i = 0; i < o.events.count; ++i)
        if (o.events.items[i].type == type && (target == kArenaNobody || o.events.items[i].target == target))
            return &o.events.items[i];
    return nullptr;
}

bool sameFrozenBytes(const ArenaFight& f, const std::vector<unsigned char>& snap) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&f);
    return std::equal(snap.begin(), snap.end(), p);
}

std::vector<unsigned char> frozenSnap(const ArenaFight& f) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&f);
    return std::vector<unsigned char>(p, p + f.frozenBytesForTest());
}

const char* kNineSeconds = R"({ "version": 1, "moves": [ { "id": "nine", "lead": "devin", "duration": 9, "impactAt": 6.5,
    "cues": [ { "at": 0, "cue": "a" }, { "at": 6.5, "cue": "impact" }, { "at": 9, "cue": "end" } ] } ] })";

} // namespace

// ---- the data ----

TEST_CASE("arena special: the shipped file loads, one move for the first lead") {
    const ArenaSpecialTable t = shippedSpecials();
    REQUIRE(t.count == 1);
    const ArenaSpecialRules* m = t.find("devin_special_01");
    REQUIRE(m != nullptr);
    CHECK(t.forLead("devin") == m);
    CHECK(t.forLead("duy") == nullptr);
    CHECK(t.forLead("") == nullptr);
    CHECK(validArenaSpecialRules(*m));
    CHECK(validArenaSpecialGauge(t.gauge));
    CHECK(m->duration >= 7.f);
    CHECK(m->duration <= 9.f);
    CHECK(m->impactAt > 0.f);
    CHECK(m->impactAt < m->duration);
    CHECK(m->laneLength == doctest::Approx(14.f));
    CHECK(m->laneWidth == doctest::Approx(4.f));
    CHECK(m->bossPhaseShare >= 0.18f);
    CHECK(m->bossPhaseShare <= 0.22f);
    REQUIRE(m->cueCount >= 2);
    bool impact = false;
    for (std::size_t i = 0; i < m->cueCount; ++i)
        if (m->cues[i].view() == "impact") impact = std::fabs(m->cues[i].at - m->impactAt) < 1e-4f;
    CHECK(impact); // the impact cue is on the impact time
}

TEST_CASE("arena special: loader errors name their path and leave the table empty") {
    const auto bad = [](const std::string& json, const char* says) {
        ArenaSpecialTable t;
        std::string err;
        CHECK_FALSE(importArenaSpecials(json, t, &err));
        CHECK(t.count == 0);
        CHECK_MESSAGE(err.find(says) != std::string::npos, err);
    };
    bad("{", "arena special:");
    bad(R"({ "moves": {} })", "moves: expected an array");
    bad(R"({ "moves": [ { "lead": "devin" } ] })", "moves[0].id");
    bad(R"({ "moves": [ { "id": "a", "lead": "devin", "duration": 4, "impactAt": 5 } ] })", "moves[0].impactAt");
    bad(R"({ "moves": [ { "id": "a", "lead": "devin" }, { "id": "a", "lead": "duy" } ] })", "moves[1].id: duplicate");
    bad(R"({ "moves": [ { "id": "a", "lead": "devin" }, { "id": "b", "lead": "devin" } ] })", "moves[1].lead");
    bad(R"({ "moves": [ { "id": "a", "lead": "devin", "cues": [ { "at": 2, "cue": "x" }, { "at": 1, "cue": "y" } ] } ] })",
        "moves[0].cues[1].at");
    bad(R"({ "moves": [ { "id": "a", "lead": "devin", "damage": 1.5 } ] })", "moves[0].damage");
    bad(R"({ "gauge": { "full": 0 }, "moves": [] })", "gauge.full");
    bad(R"({ "version": 2, "moves": [] })", "version");

    ArenaSpecialTable t;
    std::vector<std::string> warnings;
    CHECK(importArenaSpecials(R"({ "moves": [ { "id": "a", "lead": "devin", "colour": 1 } ] })", t, nullptr, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "moves[0]: unknown key: colour");
}

// ---- the gauge ----

TEST_CASE("arena special: the gauge fills from the lead's play, says Ready once, and is never shown") {
    const ArenaSpecialTable specials = shippedSpecials();
    Rig r;
    r.begin(fightOf({"dummy"}));
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 990.f)));
    CHECK_FALSE(r.fight.special().readyPlayer);
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NotReady);
    int readies = 0;
    for (int i = 0; i < 1200 && r.fight.specialGaugeForTest(0) < 1000.f; ++i)
        readies += countEvents(r.tick(r.fight.autopilot()), ArenaEventType::SpecialReady);
    CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(1000.f));
    CHECK(readies == 1);
    CHECK(r.fight.special().readyPlayer);
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Ok);
    for (int i = 0; i < 300; ++i) readies += countEvents(r.tick(r.fight.autopilot()), ArenaEventType::SpecialReady);
    CHECK(readies == 1); // full stays full, and says so once

    // Damage taken fills it too (a player who does nothing against a brute).
    Rig hurt;
    hurt.begin(fightOf({"brute"}));
    REQUIRE(hurt.fight.setSpecials(&specials, "devin", "", LeadSpecialGauges{}));
    const int hp0 = hurt.fight.hp();
    for (int i = 0; i < 600 && hurt.fight.hp() == hp0; ++i) hurt.tick();
    REQUIRE(hurt.fight.hp() < hp0);
    CHECK(hurt.fight.specialGaugeForTest(0) == doctest::Approx(1.5f * static_cast<float>(hp0 - hurt.fight.hp())));
}

TEST_CASE("arena special: a restart puts the gauges back as begin found them; the store keeps them between fights") {
    const ArenaSpecialTable specials = shippedSpecials();
    Rig r;
    r.begin(fightOf({"dummy"}));
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 400.f)));
    for (int i = 0; i < 600; ++i) r.tick(r.fight.autopilot());
    const float played = r.fight.specialGaugeForTest(0);
    CHECK(played > 400.f);
    r.fight.restart();
    CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(400.f));
    for (int i = 0; i < 600; ++i) r.tick(r.fight.autopilot());

    LeadSpecialGauges store = storeWith("devin", 0.f);
    r.fight.storeSpecialGauges(store);
    REQUIRE(store.find("devin") != nullptr);
    CHECK(store.find("devin")->value == doctest::Approx(r.fight.specialGaugeForTest(0)));

    // The next fight starts where the last one left it (no decay).
    Rig next;
    next.begin(fightOf({"brute"}));
    REQUIRE(next.fight.setSpecials(&specials, "devin", "", store));
    CHECK(next.fight.specialGaugeForTest(0) == doctest::Approx(store.find("devin")->value));
    // A value over full is clipped; a lead the store doesn't know starts empty.
    REQUIRE(next.fight.setSpecials(&specials, "devin", "duy", storeWith("devin", 5000.f)));
    CHECK(next.fight.specialGaugeForTest(0) == doctest::Approx(1000.f));
    CHECK(next.fight.specialGaugeForTest(1) == doctest::Approx(0.f));
}

TEST_CASE("arena special: a lead with no move (the Third) still fills and says Ready; only its move is missing") {
    const ArenaSpecialTable specials = shippedSpecials();
    Rig r;
    ArenaDef def = fightOf({"dummy"});
    def.partnerKit = arena().fighter("third");
    r.fight.setAlly(300, 0.4f);
    r.begin(def);
    r.bodies.ally = Vec2{-1.f, 1.f};
    REQUIRE(r.fight.setSpecials(&specials, "devin", "third", storeWith("third", 995.f)));
    CHECK(r.fight.specialRefusal(kArenaAlly) == ArenaSpecialRefusal::NoMove);
    // The partner takes a blow from the host's view: an AllyHit fills its gauge. Here: the ally is hit by nothing, so
    // give it a fill through the test door and check the event's shape on the next real fill instead.
    r.fight.setSpecialGaugeForTest(1, 1000.f);
    CHECK(r.fight.special().readyPartner);
    CHECK(r.fight.specialRefusal(kArenaAlly) == ArenaSpecialRefusal::NoMove); // ready, but no move to run
    ArenaInput in;
    in.special = kArenaAlly;
    r.tick(in);
    CHECK_FALSE(r.fight.frozen());
}

// ---- starting ----

TEST_CASE("arena special: the refusals") {
    ArenaSpecialTable specials = shippedSpecials();
    Rig r;
    r.begin(fightOf({"hulk"})); // lives through the special: the fight goes on
    // No table: nothing.
    r.tick();
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NoMove);
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Ok);
    CHECK(r.fight.specialRefusal(kArenaAlly) == ArenaSpecialRefusal::NoMove); // no ally
    CHECK(r.fight.specialRefusal(0x42) == ArenaSpecialRefusal::NoMove);

    // Out of the host's sight: dropped, the gauge kept.
    ArenaInput in;
    in.special = kArenaPlayer;
    in.specialSight = false;
    r.tick(in);
    CHECK_FALSE(r.fight.frozen());
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NoSight);
    CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(1000.f));

    // Out of range (or behind with no lock): NoTarget.
    specials.moves[0].range = 1.f;
    r.tick();
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NoTarget);
    specials.moves[0].range = 30.f;

    // In the air: Down.
    ArenaInput jump;
    jump.jump = true;
    r.tick(jump);
    if (r.fight.output().player.action == ArenaAction::Jump) CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Down);
    for (int i = 0; i < 120; ++i) r.tick();

    // Running: a second press is dropped.
    r.press();
    REQUIRE(r.fight.frozen());
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Running);
    r.runOut();
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NotReady); // spent
    CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(0.f));
}

// ---- the freeze ----

TEST_CASE("arena special: a 9 s special leaves every frozen state bit-identical until the cut back") {
    const ArenaSpecialTable specials = specialsFrom(kNineSeconds);
    Rig r;
    r.fight.setAlly(300, 0.4f);
    r.begin(fightOf({"brute", "skirmisher", "breaker"}, 2), 7);
    r.bodies.ally = Vec2{-1.f, 1.5f};
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    // Into the thick of it first: attacks, cooldowns, tokens, hit pauses all under way.
    for (int i = 0; i < 150; ++i) r.tick(r.fight.autopilot());
    REQUIRE(r.fight.phase() == ArenaPhase::Fighting);
    const float time0 = r.fight.time();
    const float held0 = r.fight.heldTime();
    std::array<int, 3> hp0{};
    for (std::size_t i = 0; i < 3; ++i) hp0[i] = r.fight.enemyHp(i);
    const ArenaBodies bodies0 = r.bodies;

    const ArenaOutput& started = r.press();
    REQUIRE(r.fight.frozen());
    const ArenaEvent* s = findEvent(started, ArenaEventType::SpecialStarted);
    REQUIRE(s != nullptr);
    CHECK(s->source == kArenaPlayer);
    CHECK(s->move == kArenaSpecialMove);
    CHECK(s->amount == 0);
    CHECK(r.fight.special().duration == doctest::Approx(9.f));

    r.tick(r.fight.autopilot()); // the first frozen tick (it empties the last tick's events)
    const std::vector<unsigned char> snap = frozenSnap(r.fight);
    REQUIRE(snap.size() > 1000);
    int frozenTicks = 1;
    bool impactSeen = false;
    while (r.fight.special().time + kDt < r.fight.special().duration - 1e-3f) {
        // The host presses everything it can: none of it reaches the frozen fight.
        ArenaInput in = r.fight.autopilot();
        in.light = in.heavy = in.dodge = in.guard = in.jump = true;
        in.special = kArenaPlayer;
        const ArenaOutput& o = r.tick(in);
        ++frozenTicks;
        CHECK(o.events.count == 0);
        if (r.fight.special().impactDone) impactSeen = true;
        if (!sameFrozenBytes(r.fight, snap)) {
            FAIL("the frozen state changed at frozen tick " << frozenTicks);
            break;
        }
    }
    CHECK(impactSeen);
    CHECK(frozenTicks >= 530);
    CHECK(r.fight.time() == time0);
    CHECK(r.fight.heldTime() == held0);
    for (std::size_t i = 0; i < 3; ++i) CHECK(r.fight.enemyHp(i) == hp0[i]); // ruled at impact, landed at the end
    CHECK(r.bodies.player.x == bodies0.player.x);
    CHECK(r.bodies.enemies[1].y == bodies0.enemies[1].y);

    const ArenaOutput& end = r.tick();
    CHECK_FALSE(r.fight.frozen());
    CHECK(r.fight.special().endedThisTick);
    CHECK(findEvent(end, ArenaEventType::SpecialEnded) != nullptr);
    CHECK(r.fight.time() == time0); // the cut back doesn't run the clock either
    const std::uint8_t target = r.fight.special().target;
    REQUIRE(target < 3);
    const ArenaEvent* hit = findEvent(end, ArenaEventType::Hit, target);
    REQUIRE(hit != nullptr);
    CHECK(hit->move == kArenaSpecialMove);
    CHECK(r.fight.enemyHp(target) == std::max(0, hp0[target] - specials.moves[0].damage));
    // From the next tick the fight runs again.
    r.tick(r.fight.autopilot());
    CHECK(r.fight.time() > time0);
}

TEST_CASE("arena special: a checkpoint copy mid-special ends the same, and so does every dt") {
    const ArenaSpecialTable specials = shippedSpecials();
    const auto play = [&](float dt, int splitAt, std::vector<int>& hp, std::vector<ArenaSpecialResult>& results) {
        Rig r;
        r.begin(fightOf({"brute", "skirmisher", "duelist"}, 2), 3);
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
        for (int i = 0; i < 90; ++i) r.tick(r.fight.autopilot());
        r.press(kArenaPlayer, dt);
        REQUIRE(r.fight.frozen());
        for (int i = 0; i < splitAt && r.fight.frozen(); ++i) r.tick(ArenaInput{}, dt);
        Rig copy = r; // the checkpoint: a plain copy
        r.runOut(dt);
        copy.runOut(dt);
        CHECK(copy.fight.enemyHp(0) == r.fight.enemyHp(0));
        CHECK(copy.fight.enemyHp(1) == r.fight.enemyHp(1));
        CHECK(copy.fight.enemyHp(2) == r.fight.enemyHp(2));
        CHECK(copy.fight.output().events.count == r.fight.output().events.count);
        CHECK(std::memcmp(copy.fight.output().events.items.data(), r.fight.output().events.items.data(),
                          sizeof(ArenaEvent) * r.fight.output().events.count) == 0);
        // ... and they go on alike.
        for (int i = 0; i < 120; ++i) {
            r.tick(r.fight.autopilot(), dt);
            copy.tick(copy.fight.autopilot(), dt);
        }
        CHECK(copy.fight.enemyHp(0) == r.fight.enemyHp(0));
        CHECK(copy.bodies.enemies[1].x == r.bodies.enemies[1].x);
        CHECK(copy.fight.time() == r.fight.time());
        hp.clear();
        results.clear();
        const ArenaSpecialView& v = r.fight.special();
        for (std::size_t i = 0; i < v.resultCount; ++i) results.push_back(v.results[i]);
    };
    std::vector<int> hpA, hpB, hpC;
    std::vector<ArenaSpecialResult> a, b, c;
    play(1.f / 60.f, 200, hpA, a);   // split before impact
    play(1.f / 30.f, 300, hpB, b);   // after impact
    play(1.f / 120.f, 5, hpC, c);    // just after the press
    REQUIRE_FALSE(a.empty());
    // The press and its results don't depend on the tick length (90 ticks of each differ in time, so compare the
    // shape: the target always, damage as the rules say).
    for (const auto* rs : {&a, &b, &c}) {
        int targets = 0;
        for (const ArenaSpecialResult& res : *rs) {
            if (res.target) {
                ++targets;
                CHECK(res.damage == specials.moves[0].damage);
            } else {
                CHECK(res.damage == static_cast<int>(std::lround(specials.moves[0].damage * specials.moves[0].otherShare)));
            }
        }
        CHECK(targets == 1);
    }
}

TEST_CASE("arena special: the same press at different dt gives the same results and lands on the same tick count") {
    const ArenaSpecialTable specials = shippedSpecials();
    const auto run = [&](float dt) {
        Rig r;
        r.begin(fightOf({"brute", "brute"}));
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
        r.bodies.enemies[0] = Vec2{5.f, 0.f};
        r.bodies.enemies[1] = Vec2{9.f, 1.f};
        r.press(kArenaPlayer, dt);
        REQUIRE(r.fight.frozen());
        const int ticks = r.runOut(dt);
        CHECK(std::fabs(static_cast<float>(ticks) * dt - specials.moves[0].duration) <= dt + 1e-3f);
        return std::array<int, 2>{r.fight.enemyHp(0), r.fight.enemyHp(1)};
    };
    const auto a = run(1.f / 60.f);
    const auto b = run(1.f / 30.f);
    const auto c = run(1.f / 144.f);
    CHECK(a == b);
    CHECK(a == c);
    CHECK(a[0] == 200 - 220 + 220 - 200); // the target (brute, 200 hp) dies: 0
    CHECK(a[1] == 200 - 110);
}

// ---- the result ----

TEST_CASE("arena special: the target always, the lane's others at the reduced share, nobody else") {
    const ArenaSpecialTable specials = shippedSpecials();
    Rig r;
    r.begin(fightOf({"hulk", "duelist", "duelist", "breaker"}, 1));
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    // The target nearest in front; one farther down the lane, one beside it, one behind the lead.
    r.bodies.player = Vec2{0.f, 0.f};
    r.bodies.enemies[0] = Vec2{6.f, 0.f};
    r.bodies.enemies[1] = Vec2{11.f, 1.f};
    r.bodies.enemies[2] = Vec2{3.f, 6.f};
    r.bodies.enemies[3] = Vec2{-5.f, 0.f};
    r.press();
    REQUIRE(r.fight.frozen());
    const std::uint8_t target = r.fight.special().target;
    CHECK(target == 0);
    CHECK(r.fight.special().dir.x > 0.99f);
    r.runOut();
    const ArenaSpecialView& v = r.fight.special();
    REQUIRE(v.resultCount == 2);
    int seenTarget = 0, seenLane = 0;
    for (std::size_t i = 0; i < v.resultCount; ++i) {
        if (v.results[i].foe == target) {
            ++seenTarget;
            CHECK(v.results[i].target);
            CHECK(v.results[i].damage == 220);
        } else {
            CHECK(v.results[i].foe == 1);
            ++seenLane;
            CHECK(v.results[i].damage == 110);
        }
    }
    CHECK(seenTarget == 1);
    CHECK(seenLane == 1);
    CHECK(r.fight.enemyHp(2) == fightOf({"duelist"}).enemies[0].hp);
    CHECK(r.fight.enemyHp(3) == fightOf({"breaker"}).enemies[0].hp);
    CHECK(r.fight.stats().specials == 1);
    CHECK(r.fight.stats().specialDamage == 330);
}

TEST_CASE("arena special: the target is hit even with a host blow hook that vetoes everything") {
    const ArenaSpecialTable specials = shippedSpecials();
    Rig r;
    r.begin(fightOf({"brute"}));
    r.fight.setBlowHook([](void*, const ArenaBlow&) {
        ArenaBlowRuling v;
        v.veto = true;
        return v;
    }, nullptr);
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    r.press();
    REQUIRE(r.fight.frozen());
    r.runOut();
    CHECK(r.fight.enemyHp(0) == 0);
    CHECK(r.fight.phase() == ArenaPhase::Victory); // the last enemy fell at the cut back
}

TEST_CASE("arena special: a boss loses its share of the phase, is never killed from full, never staggered") {
    const ArenaSpecialTable specials = shippedSpecials();
    Rig r;
    ArenaDef def = fightOf({"kite_boss"});
    r.begin(def);
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    r.press();
    REQUIRE(r.fight.frozen());
    r.runOut();
    const ArenaEnemyKind& k = r.fight.def().enemies[0];
    const float span = (1.f - k.breakPhaseEnds[0]) * static_cast<float>(k.hp);
    const int expected = static_cast<int>(std::lround(span * specials.moves[0].bossPhaseShare));
    CHECK(r.fight.enemyHp(0) == k.hp - expected);
    CHECK(r.fight.output().enemies[0].brain != ArenaBrain::Staggered);

    // A share of 1 on a one-phase boss at full hp would empty it: it keeps 1 hp.
    ArenaSpecialTable all = specials;
    all.moves[0].bossPhaseShare = 1.f;
    ArenaDef one = def;
    one.enemies[0].breakPhases = 1;
    Rig r2;
    r2.begin(one);
    REQUIRE(r2.fight.setSpecials(&all, "devin", "", storeWith("devin", 1000.f)));
    r2.press();
    r2.runOut();
    CHECK(r2.fight.enemyHp(0) == 1);
}

TEST_CASE("arena special: the knock-back carries a staggered enemy away from the performer after the cut back") {
    const ArenaSpecialTable specials = shippedSpecials();
    Rig r;
    r.begin(fightOf({"hulk"}));
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    r.bodies.enemies[0] = Vec2{4.f, 0.f};
    r.press();
    r.runOut();
    CHECK(r.fight.output().enemies[0].brain == ArenaBrain::Staggered);
    const float before = r.bodies.enemies[0].x;
    for (int i = 0; i < 40; ++i) r.tick();
    CHECK(r.bodies.enemies[0].x - before >= 0.85f * specials.moves[0].knockBack);
}

TEST_CASE("arena special: skippable only once seen; a skip lands the same results at once") {
    const ArenaSpecialTable specials = shippedSpecials();
    const auto firstAndSkip = [&](bool seen, int& ticks) {
        Rig r;
        r.begin(fightOf({"hulk"}));
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f, seen)));
        r.press();
        REQUIRE(r.fight.frozen());
        CHECK(r.fight.special().skippable == seen);
        ticks = r.runOut(kDt, true);
        LeadSpecialGauges store;
        r.fight.storeSpecialGauges(store);
        CHECK(store.find("devin")->seen); // seen to its end now
        return r.fight.enemyHp(0);
    };
    int unseenTicks = 0, seenTicks = 0;
    const int a = firstAndSkip(false, unseenTicks);
    const int b = firstAndSkip(true, seenTicks);
    CHECK(a == b);
    CHECK(seenTicks == 1);
    CHECK(unseenTicks > 400);
}

TEST_CASE("arena special: the partner's press runs its own lead's move (a second move needs no code)") {
    const ArenaSpecialTable specials = specialsFrom(R"({ "version": 1, "moves": [
        { "id": "devin_special_01", "lead": "devin" },
        { "id": "other_special_01", "lead": "duy", "duration": 3, "impactAt": 1, "damage": 50, "otherShare": 0,
          "knockBack": 0, "cues": [ { "at": 1, "cue": "impact" } ] } ] })");
    Rig r;
    ArenaDef def = fightOf({"hulk", "brute"});
    def.partnerKit = arena().fighter("duy");
    r.fight.setAlly(300, 0.4f);
    r.begin(def);
    r.bodies.ally = Vec2{0.f, 2.f};
    r.bodies.enemies[0] = Vec2{6.f, 2.f};
    r.bodies.enemies[1] = Vec2{6.f, -8.f};
    LeadSpecialGauges store = storeWith("duy", 1000.f);
    REQUIRE(r.fight.setSpecials(&specials, "devin", "duy", store));
    CHECK(r.fight.special().readyPartner);
    CHECK_FALSE(r.fight.special().readyPlayer);
    const ArenaOutput& o = r.press(kArenaAlly);
    REQUIRE(r.fight.frozen());
    const ArenaEvent* s = findEvent(o, ArenaEventType::SpecialStarted);
    REQUIRE(s != nullptr);
    CHECK(s->source == kArenaAlly);
    CHECK(s->amount == 1);
    CHECK(r.fight.special().move == specials.find("other_special_01"));
    CHECK(r.fight.special().target == 0);
    const int ticks = r.runOut();
    CHECK(ticks == 180);
    CHECK(r.fight.enemyHp(0) == fightOf({"hulk"}).enemies[0].hp - 50);
    CHECK(r.fight.enemyHp(1) == fightOf({"brute"}).enemies[0].hp);
    CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(0.f));
}

TEST_CASE("arena special: cues fire once each, in order, on their times") {
    const ArenaSpecialTable specials = shippedSpecials();
    const ArenaSpecialRules& m = specials.moves[0];
    Rig r;
    r.begin(fightOf({"hulk"}));
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    r.press();
    std::vector<std::uint8_t> fired;
    std::vector<float> at;
    const auto take = [&] {
        const ArenaSpecialView& v = r.fight.special();
        for (std::size_t i = 0; i < v.cueCount; ++i) {
            fired.push_back(v.cuesFired[i]);
            at.push_back(v.time);
        }
    };
    take();
    while (r.fight.frozen()) {
        r.tick();
        take();
    }
    REQUIRE(fired.size() == m.cueCount);
    for (std::size_t i = 0; i < fired.size(); ++i) {
        CHECK(fired[i] == i);
        CHECK(at[i] + 1e-4f >= m.cues[i].at);
        CHECK(at[i] < m.cues[i].at + kDt + 1e-4f);
    }
}

TEST_CASE("arena special: a fight with a special allocates nothing") {
    const ArenaSpecialTable specials = shippedSpecials();
    Rig r;
    const bool counting = ftcore_test::allocHookInstalled();
    r.begin(fightOf({"brute", "skirmisher"}, 2));
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 60; ++i) r.tick(r.fight.autopilot());
    r.press();
    r.runOut();
    for (int i = 0; i < 600 && !r.fight.over(); ++i) r.tick(r.fight.autopilot());
    LeadSpecialGauges store = storeWith("devin", 0.f);
    r.fight.storeSpecialGauges(store);
    if (counting) CHECK(ftcore_test::allocStats().count == before);
}

// ---- balance ----

namespace {

struct SpecialTally {
    int fights = 0, wins = 0, fills = 0, uses = 0;
    long long dealt = 0, special = 0;
    float time = 0.f;
};

// The autopilot plays `def` with the special pressed whenever it may start; the gauge carried in `store`.
void playWithSpecial(const ArenaDef& def, std::uint32_t seed, const ArenaSpecialTable& specials, LeadSpecialGauges& store,
                     SpecialTally& t) {
    Rig r;
    r.begin(def, seed);
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", store));
    while (!r.fight.over() && r.fight.time() < 120.f) {
        ArenaInput in = r.fight.autopilot();
        if (r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Ok) in.special = kArenaPlayer;
        const ArenaOutput& o = r.tick(in);
        t.fills += countEvents(o, ArenaEventType::SpecialReady);
        if (r.fight.frozen()) r.runOut();
    }
    r.fight.storeSpecialGauges(store);
    ++t.fights;
    if (r.fight.phase() == ArenaPhase::Victory) ++t.wins;
    t.uses += r.fight.stats().specials;
    t.dealt += r.fight.stats().damageDealt;
    t.special += r.fight.stats().specialDamage;
    t.time += r.fight.time();
}

} // namespace

TEST_CASE("arena special: balance (how often the gauge fills, its share of a fight's damage)") {
    const ArenaSpecialTable specials = shippedSpecials();
    struct Case {
        const char* name;
        ArenaDef def;
    };
    ArenaDef fallback = arena().fallback;
    fallback.fighter = arena().fighter("devin");
    std::vector<Case> cases{
        {"shipped default fight", fallback},
        {"three kinds, attackers 1", fightOf({"brute", "skirmisher", "breaker"}, 1)},
        {"a brute and a spitter, attackers 2", fightOf({"brute", "spitter"}, 2)},
        {"two brutes, attackers 2", fightOf({"brute", "brute"}, 2)},
        {"hulk", fightOf({"hulk"})},
        {"titan", fightOf({"titan"})},
        {"kite_boss", fightOf({"kite_boss"})},
        {"stone_bearer", fightOf({"stone_bearer"})},
        {"white_glove", fightOf({"white_glove"})},
    };
    for (Case& c : cases) c.def.ringRadius = 12.f;
    SpecialTally all;
    for (const Case& c : cases) {
        SpecialTally t;
        for (std::uint32_t seed = 1; seed <= 8; ++seed) {
            LeadSpecialGauges fresh; // each fight from empty: fills per fight
            playWithSpecial(c.def, seed, specials, fresh, t);
        }
        MESSAGE("special balance: " << std::string_view(c.name) << ": " << t.wins << "/" << t.fights << " won, fills " << t.fills
                                    << " in " << t.fights << " fights from empty (" << static_cast<float>(t.fills) / static_cast<float>(t.fights)
                                    << " per fight), mean " << t.time / static_cast<float>(t.fights) << " s, special's share of damage "
                                    << (t.dealt > 0 ? 100.0 * static_cast<double>(t.special) / static_cast<double>(t.dealt) : 0.0)
                                    << "%");
        CHECK(t.wins == t.fights);
        all.fights += t.fights;
        all.fills += t.fills;
    }
    // A session: the regular fights one after another, the gauge carried.
    LeadSpecialGauges carried;
    SpecialTally session;
    for (std::uint32_t seed = 1; seed <= 8; ++seed)
        for (std::size_t i = 0; i < 4; ++i) playWithSpecial(cases[i].def, seed, specials, carried, session);
    const float perFill = session.fills > 0 ? static_cast<float>(session.fights) / static_cast<float>(session.fills) : 99.f;
    MESSAGE("special balance: a session of " << session.fights << " regular fights, gauge carried: " << session.fills
                                             << " fills, one every " << perFill << " fights; special's share of damage "
                                             << 100.0 * static_cast<double>(session.special) / static_cast<double>(std::max(1LL, session.dealt)) << "%");
    // The target: about every 2-3 regular fights.
    CHECK(perFill >= 1.5f);
    CHECK(perFill <= 4.f);
}
