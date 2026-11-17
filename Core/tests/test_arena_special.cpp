#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaSpecial.h"
#include "ftcore/PartnerBrain.h"

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

int countEvents(const ArenaOutput& o, ArenaEventType type, std::uint8_t source = kArenaNobody) {
    int c = 0;
    for (std::size_t i = 0; i < o.events.count; ++i)
        if (o.events.items[i].type == type && (source == kArenaNobody || o.events.items[i].source == source)) ++c;
    return c;
}

const ArenaEvent* findEvent(const ArenaOutput& o, ArenaEventType type, std::uint8_t target = kArenaNobody) {
    for (std::size_t i = 0; i < o.events.count; ++i)
        if (o.events.items[i].type == type && (target == kArenaNobody || o.events.items[i].target == target))
            return &o.events.items[i];
    return nullptr;
}

// The fight's bytes ahead of the special's own block, less the last tick's event list.
struct FrozenSnap {
    std::vector<unsigned char> bytes;
    std::size_t skipAt = 0, skipLen = 0;
    explicit FrozenSnap(const ArenaFight& f) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&f);
        const unsigned char* ev = reinterpret_cast<const unsigned char*>(&f.output().events);
        REQUIRE(ev >= p);
        skipAt = static_cast<std::size_t>(ev - p);
        skipLen = sizeof(f.output().events);
        REQUIRE(skipAt + skipLen <= f.frozenBytesForTest());
        bytes.assign(p, p + f.frozenBytesForTest());
    }
    bool same(const ArenaFight& f) const {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&f);
        return std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(skipAt), p) &&
               std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(skipAt + skipLen), bytes.end(), p + skipAt + skipLen);
    }
};

PartnerTuning shippedTuning() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/partner_brain.json", text)) FAIL("Data/Battle/partner_brain.json not found");
    PartnerTuning t;
    std::string err;
    REQUIRE_MESSAGE(importPartnerBrain(text, t, &err), err);
    return t;
}

// A fight with the partner brain on: the host walks the ally toward its goal at its kit's pace (not while frozen)
// and swaps the bodies on a takeover.
struct PartnerRig : Rig {
    PartnerBrain brain;
    PartnerTuning tuning = shippedTuning();
    bool walkAlly = true;
    void beginWith(const ArenaDef& def, std::uint32_t seed) {
        fight.setAlly(def.partnerKit.hp, 0.4f);
        fight.setPartner(&brain, &tuning);
        begin(def, seed);
        const Vec2 f = fight.playerStartFacing();
        bodies.ally = Vec2{bodies.player.x - 1.5f * f.y, bodies.player.y + 1.5f * f.x};
    }
    const ArenaOutput& step(const ArenaInput& in = ArenaInput{}) {
        const ArenaOutput& o = tick(in);
        if (countEvents(o, ArenaEventType::TookOver) > 0) std::swap(bodies.player, bodies.ally);
        if (walkAlly && !fight.frozen()) {
            const Vec2 to{o.ally.goal.x - bodies.ally.x, o.ally.goal.y - bodies.ally.y};
            const float d = std::sqrt(to.x * to.x + to.y * to.y);
            const float pace = fight.def().partnerKit.moveSpeed * kDt;
            if (d > 1e-4f) {
                const float k = std::min(1.f, pace / d);
                bodies.ally = Vec2{bodies.ally.x + to.x * k, bodies.ally.y + to.y * k};
            }
        }
        return o;
    }
};

const char* kTwoLeads = R"({ "version": 1, "moves": [
    { "id": "devin_special_01", "lead": "devin" },
    { "id": "other_special_01", "lead": "duy", "duration": 3, "impactAt": 1, "damage": 50, "otherShare": 0,
      "knockBack": 0, "cues": [ { "at": 1, "cue": "impact" } ] } ] })";

// A party member who is never played: its move is keyed by its current identity id (neutral test ids).
const char* kMemberMove = R"({ "version": 1, "moves": [
    { "id": "devin_special_01", "lead": "devin" },
    { "id": "member_special_01", "lead": "identity_a", "duration": 4, "impactAt": 2, "damage": 60, "otherShare": 0,
      "knockBack": 0 } ] })";

const char* kNineSeconds = R"({ "version": 1, "moves": [ { "id": "nine", "lead": "devin", "duration": 9, "impactAt": 6.5,
    "cues": [ { "at": 0, "cue": "a" }, { "at": 6.5, "cue": "impact" }, { "at": 9, "cue": "end" } ] } ] })";

// kTwoLeads in the other order (a reloaded table: another address, other indices).
const char* kTwoLeadsReordered = R"({ "version": 1, "moves": [
    { "id": "other_special_01", "lead": "duy", "duration": 3, "impactAt": 1, "damage": 50, "otherShare": 0,
      "knockBack": 0, "cues": [ { "at": 1, "cue": "impact" } ] },
    { "id": "devin_special_01", "lead": "devin" } ] })";

// Every host call that would change the fight, made while a special runs: each does nothing (false, 0, no write).
void hostCallsDoNothing(ArenaFight& f, PartnerBrain& brain, const PartnerTuning& otherTuning) {
    const ArenaSwitchRefusal switchWas = f.switchRefusal();
    CHECK_FALSE(f.switchLeads());
    CHECK(f.switchRefusal() == switchWas);
    CHECK_FALSE(f.hostBlow(0, 50, 0));
    CHECK_FALSE(f.ruledHostBlow(0, 50, 0, [](void*, const ArenaBlow&) { return ArenaBlowRuling{}; }, nullptr));
    CHECK(f.giveLoad(5) == 0);
    CHECK(f.spendLoad(1) == 0);
    CHECK(f.hostHeal(10) == 0);
    CHECK(f.hostHurt(10) == 0);
    CHECK_FALSE(f.holdEnemy(0, ArenaBrain::Open, 2.f));
    f.setLoadCap(1);
    f.setLone(true);
    f.setLoad(3);
    f.setHealCharges(0);
    f.setChairs(ArenaChairs{});
    f.setAlly(999, 0.9f);
    f.setPartner(&brain, &otherTuning);
    f.setTableSeats(ArenaTableSeats{});
    f.releaseEnemy(0);
    f.setEnemyHpFloor(0, 150);
    f.setEnemyMoveMask(0, 0u);
    f.setEnemyFacingHeld(0, true);
    f.setEnemyTurnHeld(0, true);
    f.setEnemyFlinch(0, false);
    f.setEnemyFacing(0, Vec2{0.f, 1.f});
}

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

TEST_CASE("arena special: a restart puts the gauges back as setSpecials gave them; the store keeps them between fights") {
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

TEST_CASE("arena special: the partner's gauge fills from its own play: hidden for a lead, Ready for a party member") {
    const ArenaSpecialTable withMove = specialsFrom(kTwoLeads);
    const ArenaSpecialTable shipped = shippedSpecials();
    // The partner's kit is "duy" (a lead, a move in kTwoLeads) or "third" (a party member, no move in the shipped file).
    for (const char* partner : {"duy", "third"}) {
        CAPTURE(std::string_view(partner));
        const ArenaSpecialTable& specials = std::string_view(partner) == "duy" ? withMove : shipped;
        PartnerRig r;
        // A boss aims some strings at the partner (blows on it); the dummy keeps the fight going after it falls.
        ArenaDef def = fightOf({"dummy", "kite_boss"}, 2);
        def.partnerKit = arena().fighter(partner);
        r.beginWith(def, 5);
        REQUIRE(r.fight.setSpecials(&specials, "devin", partner, LeadSpecialGauges{}));
        float expect = 0.f;
        int readies = 0, allyHits = 0, allyTaken = 0;
        const ArenaSpecialGaugeRules& g = specials.gauge;
        const bool lead = std::string_view(partner) == "duy";
        for (int i = 0; i < 60 * 240 && readies == 0 && r.fight.specialGaugeForTest(1) < g.full && !r.fight.over(); ++i) {
            const ArenaOutput& o = r.step(r.fight.autopilot());
            for (std::size_t e = 0; e < o.events.count; ++e) {
                const ArenaEvent& ev = o.events.items[e];
                const float amount = static_cast<float>(std::max(ev.amount, 0));
                if ((ev.type == ArenaEventType::Hit || ev.type == ArenaEventType::CounterStagger) && ev.source == kArenaAlly &&
                    ev.target < r.fight.enemyCount()) {
                    expect += amount * g.perDamageDealt;
                    ++allyHits;
                } else if (ev.type == ArenaEventType::AllyHit) {
                    expect += amount * g.perDamageTaken;
                    ++allyTaken;
                } else if (ev.type == ArenaEventType::AllyBlocked) {
                    expect += g.perBlock;
                    ++allyTaken;
                } else if (ev.type == ArenaEventType::SpecialReady) {
                    const bool partnerReady = ev.source == kArenaAlly;
                    CHECK(ev.amount == (partnerReady ? 1 : 0));
                    if (partnerReady) ++readies;
                }
            }
            if (readies == 0 && r.fight.specialGaugeForTest(1) < g.full)
                REQUIRE(r.fight.specialGaugeForTest(1) == doctest::Approx(expect).epsilon(1e-4));
        }
        MESSAGE("partner " << std::string_view(partner) << ": its blows " << allyHits << ", blows on it " << allyTaken
                           << ", Ready at " << r.fight.time() << " s");
        CHECK(allyHits > 0);
        CHECK(allyTaken > 0);
        CHECK(readies == (lead ? 0 : 1)); // a partner lead's full gauge is hidden: no Ready, no prompt
        CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(g.full));
        CHECK(r.fight.special().readyPartner == !lead);
        CHECK(r.fight.specialRefusal(kArenaAlly) == (lead ? ArenaSpecialRefusal::NotControlled : ArenaSpecialRefusal::NoMove));
        // Full stays full: no second Ready.
        for (int i = 0; i < 600 && !r.fight.over(); ++i)
            readies += countEvents(r.step(r.fight.autopilot()), ArenaEventType::SpecialReady, kArenaAlly);
        CHECK(readies == (lead ? 0 : 1));
        // A press on the partner is dropped either way (no input path for a lead; no move for the member); the gauge
        // stays full.
        {
            ArenaInput in = r.fight.autopilot();
            in.special = kArenaAlly;
            r.step(in);
            CHECK_FALSE(r.fight.frozen());
            CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(g.full));
        }
    }
}

TEST_CASE("arena special: the gauges go with the kits through a lead switch; a restart puts both back") {
    const ArenaSpecialTable specials = specialsFrom(kTwoLeads);
    PartnerRig r;
    ArenaDef def = fightOf({"hulk"});
    def.partnerKit = arena().fighter("duy");
    r.beginWith(def, 2);
    r.walkAlly = false; // the ally stays out of it: only the player's play fills a gauge
    REQUIRE(r.fight.setSpecials(&specials, "devin", "duy", storeWith("duy", 700.f)));
    r.step();
    CHECK_FALSE(r.fight.special().readyPlayer);
    CHECK_FALSE(r.fight.special().readyPartner);
    REQUIRE(r.fight.switchLeads());
    std::swap(r.bodies.player, r.bodies.ally);
    CHECK(r.fight.swapped());
    CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(525.f)); // the switch took a quarter off
    // The player drives the second lead's body now: its play fills slot 1, its Ready names the player's body.
    r.bodies.player = Vec2{0.f, 0.f};
    r.bodies.ally = Vec2{-20.f, 0.f};
    r.bodies.enemies[0] = Vec2{2.f, 0.f};
    int readies = 0;
    for (int i = 0; i < 60 * 120 && readies == 0 && !r.fight.over(); ++i) {
        const ArenaOutput& o = r.step(r.fight.autopilot());
        if (const ArenaEvent* e = findEvent(o, ArenaEventType::SpecialReady)) {
            ++readies;
            CHECK(e->source == kArenaPlayer);
            CHECK(e->amount == 1);
        }
    }
    REQUIRE(readies == 1);
    CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(1000.f));
    CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(0.f)); // the first lead, the partner now, did nothing
    CHECK(r.fight.special().readyPlayer);
    CHECK_FALSE(r.fight.special().readyPartner);
    // The player's press runs the second lead's move.
    ArenaInput in = r.fight.autopilot();
    in.special = kArenaPlayer;
    const ArenaOutput& o = r.step(in);
    REQUIRE(r.fight.frozen());
    const ArenaEvent* s = findEvent(o, ArenaEventType::SpecialStarted);
    REQUIRE(s != nullptr);
    CHECK(s->source == kArenaPlayer);
    CHECK(s->amount == 1);
    CHECK(r.fight.specialMove() == specials.find("other_special_01"));
    r.runOut();
    CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(0.f));
    // A restart: the bodies unswapped, the gauges as begin found them.
    r.fight.restart();
    CHECK_FALSE(r.fight.swapped());
    CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(0.f));
    CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(700.f));
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
    {   // ... but a locked enemy is the target at any range (the cinematic covers the distance).
        const Vec2 to{r.bodies.enemies[0].x - r.bodies.player.x, r.bodies.enemies[0].y - r.bodies.player.y};
        REQUIRE(std::sqrt(to.x * to.x + to.y * to.y) > 1.5f);
        ArenaInput lock;
        lock.view = Vec2{to.x / std::sqrt(to.x * to.x + to.y * to.y), to.y / std::sqrt(to.x * to.x + to.y * to.y)};
        lock.lock = true;
        r.tick(lock);
        CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Ok);
        r.tick(lock); // let go: out of range again
        CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NoTarget);
    }
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

TEST_CASE("arena special: a 9 s special leaves every frozen state bit-identical from the press to the cut back") {
    const ArenaSpecialTable specials = specialsFrom(kNineSeconds);
    PartnerRig r;
    ArenaDef def = fightOf({"brute", "skirmisher", "breaker", "spitter"}, 2);
    def.partnerKit = arena().fighter("duy");
    r.beginWith(def, 7);
    REQUIRE(r.fight.setSpecials(&specials, "devin", "duy", storeWith("devin", 1000.f)));
    // Into the thick of it first: the spitter's shot under way, attacks, cooldowns, tokens, hit pauses, the partner's
    // string and the brain's clock all running.
    bool spitting = false;
    for (int i = 0; i < 60 * 20 && !(i > 120 && spitting); ++i) {
        r.step(r.fight.autopilot());
        spitting = r.fight.output().enemies[3].brain == ArenaBrain::Attack;
    }
    REQUIRE(spitting);
    REQUIRE(r.fight.phase() == ArenaPhase::Fighting);
    const float time0 = r.fight.time();
    const float held0 = r.fight.heldTime();
    std::array<int, 4> hp0{};
    for (std::size_t i = 0; i < 4; ++i) hp0[i] = r.fight.enemyHp(i);
    const int allyHp0 = r.fight.allyHp();
    const std::uint64_t brain0 = r.brain.hash();

    const ArenaOutput& started = r.press();
    REQUIRE(r.fight.frozen());
    const ArenaEvent* s = findEvent(started, ArenaEventType::SpecialStarted);
    REQUIRE(s != nullptr);
    CHECK(s->source == kArenaPlayer);
    CHECK(s->move == kArenaSpecialMove);
    CHECK(s->amount == 0);
    CHECK(r.fight.special().duration == doctest::Approx(9.f));
    CHECK(r.brain.hash() == brain0); // the press tick freezes before the brain's step
    // The hash at the press: every byte of the fight ahead of the special's own block but the event list (the last
    // tick's events, emptied by the first frozen tick).
    const FrozenSnap snap(r.fight);
    REQUIRE(snap.bytes.size() > 1000);
    const PartnerTuning otherTuning = r.tuning; // another address: setPartner would take it
    const ArenaSwitchRefusal switchWas = r.fight.switchRefusal();
    int frozenTicks = 0;
    bool impactSeen = false;
    while (r.fight.special().time + kDt < r.fight.special().duration - 1e-3f) {
        // The host presses everything it can: none of it reaches the frozen fight.
        ArenaInput in = r.fight.autopilot();
        in.light = in.heavy = in.dodge = in.guard = in.jump = true;
        in.special = kArenaAlly;
        const ArenaOutput& o = r.step(in);
        ++frozenTicks;
        CHECK(o.events.count == 0);
        if (r.fight.special().impactDone) impactSeen = true;
        if (frozenTicks == 60 || frozenTicks == 450) hostCallsDoNothing(r.fight, r.brain, otherTuning); // before and after impact
        if (!snap.same(r.fight) || r.brain.hash() != brain0) {
            FAIL("the frozen state changed at frozen tick " << frozenTicks);
            break;
        }
    }
    CHECK(impactSeen);
    CHECK(frozenTicks >= 530);
    CHECK(r.fight.time() == time0);
    CHECK(r.fight.heldTime() == held0);
    for (std::size_t i = 0; i < 4; ++i) CHECK(r.fight.enemyHp(i) == hp0[i]); // ruled at impact, landed at the end
    CHECK(r.fight.allyHp() == allyHp0);
    CHECK(r.fight.switchRefusal() == switchWas);
    CHECK_FALSE(r.fight.swapped());

    const ArenaOutput& end = r.step();
    CHECK_FALSE(r.fight.frozen());
    CHECK(r.fight.special().endedThisTick);
    CHECK(findEvent(end, ArenaEventType::SpecialEnded) != nullptr);
    CHECK(r.fight.time() == time0); // the cut back doesn't run the clock either
    const std::uint8_t target = r.fight.special().target;
    REQUIRE(target < 4);
    const ArenaEvent* hit = findEvent(end, ArenaEventType::Hit, target);
    REQUIRE(hit != nullptr);
    CHECK(hit->move == kArenaSpecialMove);
    CHECK(r.fight.enemyHp(target) == std::max(0, hp0[target] - specials.moves[0].damage));
    // From the next tick the fight runs again, the brain with it.
    r.step(r.fight.autopilot());
    CHECK(r.fight.time() > time0);
    CHECK(r.brain.hash() != brain0);
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

TEST_CASE("arena special: a partner lead's move can't be fired; the controlled lead's can, after a switch") {
    const ArenaSpecialTable specials = specialsFrom(kTwoLeads);
    PartnerRig r;
    ArenaDef def = fightOf({"hulk", "brute"});
    def.partnerKit = arena().fighter("duy");
    r.beginWith(def, 3);
    r.walkAlly = false;
    r.bodies.ally = Vec2{0.f, 2.f};
    r.bodies.enemies[0] = Vec2{6.f, 2.f};
    r.bodies.enemies[1] = Vec2{6.f, -8.f};
    REQUIRE(r.fight.setSpecials(&specials, "devin", "duy", storeWith("duy", 1000.f)));
    CHECK_FALSE(r.fight.special().readyPartner); // full, hidden
    CHECK_FALSE(r.fight.special().readyPlayer);
    CHECK(r.fight.specialRefusal(kArenaAlly) == ArenaSpecialRefusal::NotControlled);
    CHECK(std::string_view(arenaSpecialRefusalName(ArenaSpecialRefusal::NotControlled)) == "not_controlled");
    r.press(kArenaAlly);
    CHECK_FALSE(r.fight.frozen());
    CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(1000.f));
    // Controlled, its move fires (a second move needs no code): a switch takes 25% off, so refill to full first.
    REQUIRE(r.fight.switchLeads());
    std::swap(r.bodies.player, r.bodies.ally);
    CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(750.f));
    r.bodies.player = Vec2{0.f, 0.f};
    r.bodies.ally = Vec2{-20.f, 0.f};
    r.bodies.enemies[0] = Vec2{2.f, 0.f};
    for (int i = 0; i < 60 * 120 && r.fight.specialGaugeForTest(1) < 1000.f && !r.fight.over(); ++i)
        r.tick(r.fight.autopilot());
    REQUIRE(r.fight.special().readyPlayer);
    const ArenaOutput& o = r.press(kArenaPlayer);
    REQUIRE(r.fight.frozen());
    const ArenaEvent* st = findEvent(o, ArenaEventType::SpecialStarted);
    REQUIRE(st != nullptr);
    CHECK(st->source == kArenaPlayer);
    CHECK(st->amount == 1);
    CHECK(r.fight.specialMove() == specials.find("other_special_01"));
    r.runOut();
    CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(0.f));
}

TEST_CASE("arena special: the player fires a party member's move at the player's target, the player frozen too") {
    const ArenaSpecialTable specials = specialsFrom(kMemberMove);
    for (const bool lock : {false, true}) {
        CAPTURE(lock);
        Rig r;
        ArenaDef def = fightOf({"hulk", "hulk"});
        def.partnerKit = arena().fighter("third"); // not playable: a party member
        r.fight.setAlly(300, 0.4f);
        r.begin(def);
        // Enemy 0 in front of the player; enemy 1 behind it, beside the partner (the partner's nearest).
        const Vec2 f = r.fight.output().player.facing;
        r.bodies.player = Vec2{0.f, 0.f};
        r.bodies.ally = Vec2{-4.f * f.x - 2.f * f.y, -4.f * f.y + 2.f * f.x};
        r.bodies.enemies[0] = Vec2{8.f * f.x, 8.f * f.y};
        r.bodies.enemies[1] = Vec2{-6.f * f.x - 2.f * f.y, -6.f * f.y + 2.f * f.x};
        r.tick();
        if (lock) { // the player locks on the one behind it
            ArenaInput in;
            in.view = Vec2{-f.x, -f.y};
            in.lock = true;
            r.tick(in);
        }
        REQUIRE(r.fight.setSpecials(&specials, "devin", "identity_a", storeWith("identity_a", 1000.f)));
        CHECK(r.fight.special().readyPartner);
        REQUIRE(r.fight.specialRefusal(kArenaAlly) == ArenaSpecialRefusal::Ok);
        const ArenaOutput& o = r.press(kArenaAlly);
        REQUIRE(r.fight.frozen());
        const ArenaEvent* st = findEvent(o, ArenaEventType::SpecialStarted);
        REQUIRE(st != nullptr);
        CHECK(st->source == kArenaAlly);
        CHECK(r.fight.special().performer == kArenaAlly);
        CHECK(r.fight.specialMove() == specials.find("member_special_01"));
        CHECK(r.fight.special().target == (lock ? 1 : 0));
        // The controlled lead is frozen with the rest: the player's every press and the host's moves reach nothing.
        const FrozenSnap snap(r.fight);
        int frozenTicks = 0;
        while (r.fight.special().time + kDt < r.fight.special().duration - 1e-3f) {
            ArenaInput in;
            in.light = in.heavy = in.dodge = in.guard = in.jump = true;
            in.move = Vec2{0.f, 1.f};
            in.special = kArenaPlayer;
            r.bodies.player = Vec2{0.5f, 0.5f};
            r.tick(in);
            ++frozenTicks;
            if (!snap.same(r.fight)) {
                FAIL("the frozen state changed at frozen tick " << frozenTicks);
                break;
            }
        }
        CHECK(frozenTicks > 200);
        r.bodies.player = Vec2{0.f, 0.f};
        r.runOut();
        const std::size_t t = lock ? 1u : 0u;
        CHECK(r.fight.enemyHp(t) == fightOf({"hulk"}).enemies[0].hp - 60);
        CHECK(r.fight.enemyHp(1u - t) == fightOf({"hulk"}).enemies[0].hp);
    }
}

TEST_CASE("arena special: an identity change carries the one body's gauge to the new identity id") {
    LeadSpecialGauges store = storeWith("identity_a", 640.f, true);
    REQUIRE(store.carry("identity_a", "identity_b"));
    REQUIRE(store.find("identity_b") != nullptr);
    CHECK(store.find("identity_b")->value == doctest::Approx(640.f));
    CHECK_FALSE(store.find("identity_b")->seen); // its own move: not seen yet
    CHECK(store.find("identity_a")->value == doctest::Approx(0.f));
    CHECK(store.find("identity_a")->seen);
    CHECK_FALSE(store.carry("identity_b", "identity_b"));
    CHECK_FALSE(store.carry("", "identity_b"));
    // A fight under the new id begins from the carried value; with no move for it, its refusal is NoMove.
    const ArenaSpecialTable specials = specialsFrom(kMemberMove);
    Rig r;
    ArenaDef def = fightOf({"hulk"});
    def.partnerKit = arena().fighter("third");
    r.fight.setAlly(300, 0.4f);
    r.begin(def);
    REQUIRE(r.fight.setSpecials(&specials, "devin", "identity_b", store));
    CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(640.f));
    r.tick();
    CHECK(r.fight.specialRefusal(kArenaAlly) == ArenaSpecialRefusal::NoMove);
}

namespace {
// Two leads, both gauges set; `extra` goes into the gauge rules.
ArenaSpecialTable switchTable(const char* extra) {
    const std::string text = std::string(R"({ "version": 1, "gauge": { )") + extra +
                             R"( }, "moves": [ { "id": "devin_special_01", "lead": "devin" },
        { "id": "other_special_01", "lead": "duy", "duration": 3, "impactAt": 1 } ] })";
    ArenaSpecialTable t;
    std::string error;
    REQUIRE_MESSAGE(importArenaSpecials(text, t, &error), error);
    return t;
}
struct SwitchRig : PartnerRig {
    explicit SwitchRig(const ArenaSpecialTable& table, float devin, float duy) {
        ArenaDef def = fightOf({"dummy"});
        def.partnerKit = arena().fighter("duy");
        beginWith(def, 1);
        walkAlly = false;
        LeadSpecialGauges store = storeWith("devin", devin);
        store.entry("duy")->value = duy;
        REQUIRE(fight.setSpecials(&table, "devin", "duy", store));
    }
};
} // namespace

TEST_CASE("arena special: a lead switch lowers the gauges switchLossAppliesTo names by switchLoss") {
    struct Case { const char* extra; float devin, duy; };
    const Case cases[] = {
        {R"("switchLoss": 0.25, "switchLossAppliesTo": "both")", 750.f, 600.f},
        {R"("switchLoss": 0.25, "switchLossAppliesTo": "outgoing")", 750.f, 800.f},
        {R"("switchLoss": 0.25, "switchLossAppliesTo": "incoming")", 1000.f, 600.f},
        {R"("switchLoss": 0)", 1000.f, 800.f},
    };
    for (const Case& c : cases) {
        CAPTURE(std::string_view(c.extra));
        const ArenaSpecialTable table = switchTable(c.extra);
        SwitchRig r(table, 1000.f, 800.f);
        CHECK(r.fight.special().readyPlayer);
        CHECK(countEvents(r.tick(), ArenaEventType::SpecialReady) == 1); // full at setSpecials: Ready on the first tick
        REQUIRE(r.fight.switchLeads());
        std::swap(r.bodies.player, r.bodies.ally);
        CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(c.devin));
        CHECK(r.fight.specialGaugeForTest(1) == doctest::Approx(c.duy));
        // The first lead is the partner now: its prompt goes, whether its gauge dropped or not.
        CHECK_FALSE(r.fight.special().readyPartner);
        const ArenaOutput& o = r.tick();
        const ArenaEvent* un = findEvent(o, ArenaEventType::SpecialUnready);
        REQUIRE(un != nullptr);
        CHECK(un->source == kArenaAlly);
        CHECK(un->amount == 0);
        CHECK(countEvents(o, ArenaEventType::SpecialReady) == 0);
        CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NotReady);
    }
    // The shipped rules: 0.25 off both.
    const ArenaSpecialTable shipped = shippedSpecials();
    CHECK(shipped.gauge.switchLoss == doctest::Approx(0.25f));
    CHECK(shipped.gauge.switchLossAppliesTo == ArenaSpecialSwitchLoss::Both);
    CHECK(shipped.gauge.readyLineGap == doctest::Approx(60.f));
}

TEST_CASE("arena special: a switched-in full gauge says Ready again; its line at most once per readyLineGap") {
    const ArenaSpecialTable table = switchTable(R"("switchLoss": 0, "readyLineGap": 60)");
    SwitchRig r(table, 1000.f, 1000.f);
    {   // 0: the first lead arrives full: Ready on the first tick, with its line (none played yet).
        const ArenaEvent* first = findEvent(r.tick(), ArenaEventType::SpecialReady);
        REQUIRE(first != nullptr);
        CHECK(first->source == kArenaPlayer);
        CHECK(first->move == 1);
    }
    const auto switchAndTick = [&r]() -> const ArenaOutput& {
        REQUIRE(r.fight.switchLeads());
        std::swap(r.bodies.player, r.bodies.ally);
        return r.tick();
    };
    // 1: the second lead is controlled: Unready for the first, Ready (with its line) for the second.
    const ArenaOutput& a = switchAndTick();
    const ArenaEvent* un = findEvent(a, ArenaEventType::SpecialUnready);
    const ArenaEvent* re = findEvent(a, ArenaEventType::SpecialReady);
    REQUIRE(un != nullptr);
    REQUIRE(re != nullptr);
    CHECK(un->amount == 0);
    CHECK(re->amount == 1);
    CHECK(re->source == kArenaPlayer);
    CHECK(re->move == 1);
    // 2: back: the first lead's prompt again, not its line (it played on the first tick).
    for (int i = 0; i < 30; ++i) r.tick();
    const ArenaEvent* re2 = findEvent(switchAndTick(), ArenaEventType::SpecialReady);
    REQUIRE(re2 != nullptr);
    CHECK(re2->amount == 0);
    CHECK(re2->move == 0);
    // 3: again within 60 s: the prompt comes back, the line doesn't.
    for (int i = 0; i < 30; ++i) r.tick();
    const ArenaEvent* re3 = findEvent(switchAndTick(), ArenaEventType::SpecialReady);
    REQUIRE(re3 != nullptr);
    CHECK(re3->amount == 1);
    CHECK(re3->move == 0);
    CHECK(r.fight.special().readyPlayer);
    // 4: past 60 s since that lead's line: the line again.
    for (int i = 0; i < 60 * 60; ++i) r.tick();
    REQUIRE_FALSE(r.fight.over());
    switchAndTick();
    const ArenaEvent* re4 = findEvent(switchAndTick(), ArenaEventType::SpecialReady);
    REQUIRE(re4 != nullptr);
    CHECK(re4->amount == 1);
    CHECK(re4->move == 1);
}

TEST_CASE("arena special: the gauge rules' switch keys load and are checked") {
    ArenaSpecialTable t;
    std::string error;
    CHECK_FALSE(importArenaSpecials(R"({ "version": 1, "gauge": { "switchLoss": 1.5 }, "moves": [] })", t, &error));
    CHECK(error.find("switchLoss") != std::string::npos);
    CHECK_FALSE(importArenaSpecials(R"({ "version": 1, "gauge": { "switchLossAppliesTo": "all" }, "moves": [] })", t,
                                    &error));
    CHECK(error.find("switchLossAppliesTo") != std::string::npos);
    CHECK_FALSE(importArenaSpecials(R"({ "version": 1, "gauge": { "readyLineGap": -1 }, "moves": [] })", t, &error));
    CHECK(error.find("readyLineGap") != std::string::npos);
    ArenaSpecialGaugeRules g;
    CHECK(validArenaSpecialGauge(g));
    g.switchLoss = 2.f;
    CHECK_FALSE(validArenaSpecialGauge(g));
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


// ---- the review's cases ----

TEST_CASE("arena special: the fight keeps table indices, not pointers: a reloaded table rebinds; a lost move ends empty") {
    const ArenaSpecialTable a = specialsFrom(kTwoLeads);
    const ArenaSpecialTable b = specialsFrom(kTwoLeadsReordered);
    const auto pressed = [&](Rig& r, const ArenaSpecialTable& table) {
        r.begin(fightOf({"brute", "hulk"}), 3);
        REQUIRE(r.fight.setSpecials(&table, "devin", "", storeWith("devin", 1000.f)));
        r.bodies.enemies[0] = Vec2{5.f, 0.f};
        r.bodies.enemies[1] = Vec2{9.f, 1.f};
        r.press();
        REQUIRE(r.fight.frozen());
        for (int i = 0; i < 100; ++i) r.tick();
        REQUIRE(r.fight.frozen());
        REQUIRE_FALSE(r.fight.special().impactDone);
    };
    Rig r;
    pressed(r, a);
    CHECK(r.fight.special().move == 0);
    CHECK(r.fight.specialMove() == a.find("devin_special_01"));
    // The same press on another copy of the table: the same state, byte for byte where it holds the move.
    {
        const ArenaSpecialTable a2 = a;
        Rig other;
        pressed(other, a2);
        CHECK(other.fight.special().move == r.fight.special().move);
        CHECK(other.fight.specialMove() == a2.find("devin_special_01"));
    }
    // A checkpoint restored after the host reloaded its table (another address and order) ends as the original.
    Rig copy = r;
    REQUIRE(copy.fight.setSpecialTable(&b));
    CHECK(copy.fight.special().move == 1);
    CHECK(copy.fight.specialMove() == b.find("devin_special_01"));
    r.runOut();
    copy.runOut();
    for (std::size_t i = 0; i < 2; ++i) CHECK(copy.fight.enemyHp(i) == r.fight.enemyHp(i));
    REQUIRE(copy.fight.special().resultCount == r.fight.special().resultCount);
    CHECK(r.fight.special().resultCount == 2);
    for (std::size_t i = 0; i < r.fight.special().resultCount; ++i) {
        CHECK(copy.fight.special().results[i].foe == r.fight.special().results[i].foe);
        CHECK(copy.fight.special().results[i].damage == r.fight.special().results[i].damage);
    }

    // The table gone (nullptr, or one without the move) mid-special: it ends at the next tick, nothing lands, the gauge
    // stays spent.
    for (const bool emptyTable : {false, true}) {
        CAPTURE(emptyTable);
        const ArenaSpecialTable none{};
        Rig lost;
        pressed(lost, a);
        const int hp0 = lost.fight.enemyHp(0), hp1 = lost.fight.enemyHp(1);
        REQUIRE(lost.fight.setSpecialTable(emptyTable ? &none : nullptr));
        CHECK(lost.fight.specialMove() == nullptr);
        const ArenaOutput& o = lost.tick();
        CHECK_FALSE(lost.fight.frozen());
        CHECK(findEvent(o, ArenaEventType::SpecialEnded) != nullptr);
        CHECK(findEvent(o, ArenaEventType::Hit) == nullptr);
        CHECK(lost.fight.special().resultCount == 0);
        CHECK(lost.fight.enemyHp(0) == hp0);
        CHECK(lost.fight.enemyHp(1) == hp1);
        CHECK(lost.fight.specialGaugeForTest(0) == 0.f);
        CHECK(lost.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NoMove);
    }
}

TEST_CASE("arena special: begin clears the specials until setSpecials; begin or restart mid-special ends it unlanded") {
    const ArenaSpecialTable specials = shippedSpecials();
    const int hulkHp = fightOf({"hulk"}).enemies[0].hp;
    Rig r;
    r.begin(fightOf({"hulk"}));
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f, true)));
    r.tick();
    REQUIRE(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Ok);
    // A new fight with no setSpecials: no move, empty gauges, nothing carried from the last fight.
    r.begin(fightOf({"hulk"}));
    CHECK(r.fight.specialGaugeForTest(0) == 0.f);
    CHECK(r.fight.specialGaugeForTest(1) == 0.f);
    CHECK(r.fight.specialMove() == nullptr);
    CHECK_FALSE(r.fight.special().readyPlayer);
    CHECK(countEvents(r.tick(), ArenaEventType::SpecialReady) == 0);
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NoMove);
    r.press();
    CHECK_FALSE(r.fight.frozen());
    LeadSpecialGauges store = storeWith("devin", 123.f);
    r.fight.storeSpecialGauges(store);
    CHECK(store.find("devin")->value == doctest::Approx(123.f)); // no lead in the fight: the store keeps its own
    CHECK_FALSE(store.find("devin")->seen);

    // begin mid-special: it ends, nothing lands.
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    r.press();
    REQUIRE(r.fight.frozen());
    for (int i = 0; i < 50; ++i) r.tick();
    r.begin(fightOf({"hulk"}));
    CHECK_FALSE(r.fight.frozen());
    CHECK_FALSE(r.fight.special().running);
    CHECK(r.fight.enemyHp(0) == hulkHp);
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NoMove);

    // restart mid-special (past the impact): it ends, nothing lands, the gauge as setSpecials gave it.
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    r.press();
    REQUIRE(r.fight.frozen());
    while (!r.fight.special().impactDone) r.tick();
    REQUIRE(r.fight.frozen());
    r.fight.restart();
    CHECK_FALSE(r.fight.frozen());
    CHECK(r.fight.enemyHp(0) == hulkHp);
    CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(1000.f));
    const ArenaOutput& o = r.tick();
    CHECK(findEvent(o, ArenaEventType::Hit) == nullptr);
    CHECK(countEvents(o, ArenaEventType::SpecialReady) == 1); // armed again: full says Ready
    CHECK(r.fight.enemyHp(0) == hulkHp);
}

TEST_CASE("arena special: a gauge arriving full says Ready on the first tick; the line's gap runs on the store's clock") {
    const ArenaSpecialTable specials = shippedSpecials();
    REQUIRE(specials.gauge.readyLineGap == doctest::Approx(60.f));
    LeadSpecialGauges store = storeWith("devin", 1000.f);
    const auto firstReadyLine = [&](float hostClockOn) {
        store.clock += hostClockOn; // the host's play outside fights
        Rig r;
        r.begin(fightOf({"hulk"}));
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", store));
        CHECK(r.fight.special().readyPlayer);
        const ArenaOutput& o = r.tick();
        REQUIRE(countEvents(o, ArenaEventType::SpecialReady) == 1);
        const int line = findEvent(o, ArenaEventType::SpecialReady)->move;
        for (int i = 0; i < 10; ++i) CHECK(countEvents(r.tick(), ArenaEventType::SpecialReady) == 0);
        const float before = store.clock;
        r.fight.storeSpecialGauges(store);
        CHECK(store.clock == doctest::Approx(before + 11.f * kDt));
        return line;
    };
    CHECK(firstReadyLine(0.f) == 1);  // none played yet
    CHECK(firstReadyLine(0.f) == 0);  // played in the last fight, 11 ticks ago
    CHECK(firstReadyLine(30.f) == 0); // 30 s on
    CHECK(firstReadyLine(30.f) == 1); // past 60 s on the host's clock
    CHECK(firstReadyLine(0.f) == 0);

    // The clock runs on through a restart, never back; the store never runs back either.
    Rig r;
    r.begin(fightOf({"hulk"}));
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", store));
    const float c0 = store.clock;
    for (int i = 0; i < 10; ++i) r.tick();
    r.fight.restart();
    for (int i = 0; i < 10; ++i) r.tick();
    LeadSpecialGauges later = store;
    r.fight.storeSpecialGauges(later);
    CHECK(later.clock == doctest::Approx(c0 + 20.f * kDt));
    LeadSpecialGauges ahead = store;
    ahead.clock = c0 + 5000.f;
    r.fight.storeSpecialGauges(ahead);
    CHECK(ahead.clock == doctest::Approx(c0 + 5000.f));
}

TEST_CASE("arena special: a special that ends the fight settles it as a blow does: one Victory, no reload left") {
    const ArenaSpecialTable specials = shippedSpecials();
    ArenaDef def = fightOf({"brute"});
    ArenaFighter& f = def.fighter;
    f.weapon = ArenaWeapon::Guns;
    f.rounds = 1;
    f.reload = 3.f;
    f.shot = f.light[0];
    f.shot.range = 12.f;
    Rig r;
    r.begin(def);
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
    // One aimed shot empties the cylinder: a reload runs at the press.
    for (int i = 0; i < 120 && !r.fight.reloading(); ++i) {
        ArenaInput in;
        in.aim = true;
        in.light = i % 2 == 0;
        r.tick(in);
    }
    REQUIRE(r.fight.reloading());
    r.press();
    REQUIRE(r.fight.frozen());
    CHECK(r.fight.reloading()); // frozen with the rest
    int victories = 0;
    while (r.fight.frozen()) victories += countEvents(r.tick(), ArenaEventType::Victory);
    CHECK(r.fight.phase() == ArenaPhase::Victory);
    CHECK(r.fight.enemyHp(0) == 0);
    CHECK(victories == 1);
    CHECK_FALSE(r.fight.reloading());
    CHECK(r.fight.reloadProgress() == 0.f);
    for (int i = 0; i < 30; ++i) victories += countEvents(r.tick(), ArenaEventType::Victory);
    CHECK(victories == 1);
}

TEST_CASE("arena special: setSpecials and setSpecialTable refuse a table the loader would refuse") {
    const ArenaSpecialTable good = shippedSpecials();
    Rig r;
    r.begin(fightOf({"hulk"}));
    REQUIRE(r.fight.setSpecials(&good, "devin", "", storeWith("devin", 1000.f)));
    using Spoil = void (*)(ArenaSpecialTable&);
    const Spoil spoils[] = {
        [](ArenaSpecialTable& t) { t.moves[0].knockTime = 0.f; }, // an endless shove
        [](ArenaSpecialTable& t) { t.moves[0].laneWidth = 0.f; },
        [](ArenaSpecialTable& t) { t.moves[0].damage = 0; },
        [](ArenaSpecialTable& t) { t.count = t.moves.size() + 1; },
        [](ArenaSpecialTable& t) { t.gauge.full = 0.f; },
    };
    for (const Spoil spoil : spoils) {
        ArenaSpecialTable t = good;
        spoil(t);
        const bool valid = validArenaSpecialRules(t.moves[0]) && validArenaSpecialGauge(t.gauge) && t.count <= t.moves.size();
        CHECK_FALSE(valid);
        CHECK_FALSE(r.fight.setSpecials(&t, "devin", "", storeWith("devin", 500.f)));
        CHECK_FALSE(r.fight.setSpecialTable(&t));
        CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(1000.f)); // nothing changed
        CHECK(r.fight.special().readyPlayer);
    }
    // The good table still stands: the press runs its move.
    r.press();
    REQUIRE(r.fight.frozen());
    CHECK(r.fight.specialMove() == &good.moves[0]);
}

TEST_CASE("arena special: no Ready or Unready on the tick the fight ends") {
    const ArenaSpecialTable specials = shippedSpecials();
    const auto said = [](const ArenaOutput& o) {
        return countEvents(o, ArenaEventType::SpecialReady) + countEvents(o, ArenaEventType::SpecialUnready);
    };
    {   // Victory: the killing blow fills the gauge on the last tick.
        Rig r;
        r.begin(fightOf({"dummy"}));
        r.tick();
        REQUIRE(r.fight.hostBlow(0, r.fight.enemyHp(0) - 1, 0));
        r.tick();
        REQUIRE(r.fight.enemyHp(0) == 1);
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 999.999f)));
        int count = 0;
        for (int i = 0; i < 60 * 20 && !r.fight.over(); ++i) count += said(r.tick(r.fight.autopilot()));
        REQUIRE(r.fight.phase() == ArenaPhase::Victory);
        CHECK(r.fight.specialGaugeForTest(0) == 1000.f); // full on the tick it ended
        CHECK(count == 0);
        for (int i = 0; i < 30; ++i) count += said(r.tick());
        CHECK(count == 0);
    }
    {   // Defeat: the blow that downs the player fills the gauge on the last tick.
        Rig r;
        r.begin(fightOf({"brute"}));
        r.tick();
        REQUIRE(r.fight.hostHurt(r.fight.hp() - 1) > 0);
        r.tick();
        REQUIRE(r.fight.hp() == 1);
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 999.999f)));
        int count = 0;
        for (int i = 0; i < 60 * 30 && !r.fight.over(); ++i) count += said(r.tick());
        REQUIRE(r.fight.phase() == ArenaPhase::Defeat);
        CHECK(r.fight.specialGaugeForTest(0) == 1000.f);
        CHECK(count == 0);
    }
}

TEST_CASE("arena special: an enemy held unhurt shows the hit and loses nothing; alone in the lane the press is refused") {
    const ArenaSpecialTable specials = shippedSpecials();
    const ArenaSpecialRules& m = specials.moves[0];
    CHECK(std::string_view(arenaSpecialRefusalName(ArenaSpecialRefusal::Unhurt)) == "unhurt");
    // Enemy 0 steps back (held unhurt) when two blows fill its count while enemy 1 holds the only token.
    const auto steppedBack = [](Rig& r, Vec2 holderAt) {
        ArenaDef def = fightOf({"brute", "brute"}, 1);
        for (std::size_t l = 0; l < def.fighter.lightCount; ++l) def.fighter.light[l].poiseDamage = kArenaBreakPoiseUnit;
        ArenaEnemyKind& k = def.enemies[0];
        k.breakAt = 2;
        k.breakMove = 0;
        k.breakDecay = 5.f;
        k.poise = 1000.f;
        k.flinchPoise = 1000.f;
        k.attackDelay = 1000.f; // never asks for the token
        r.begin(def);
        r.fight.setEnemyMoveMask(1, 0u); // the holder never strikes: it keeps the token
        for (int i = 0; i < 60 * 15 && !r.fight.output().enemies[1].token; ++i) {
            r.bodies.player = Vec2{0.f, 0.f};
            r.bodies.enemies[0] = Vec2{1.5f, 0.f};
            r.tick();
        }
        REQUIRE(r.fight.output().enemies[1].token);
        for (int i = 0; i < 60 * 10 && r.fight.output().enemies[0].brain != ArenaBrain::StepBack; ++i) {
            r.bodies.player = Vec2{0.f, 0.f};
            r.bodies.enemies[0] = Vec2{1.5f, 0.f};
            r.bodies.enemies[1] = holderAt;
            ArenaInput in;
            in.light = true;
            r.tick(in);
        }
        REQUIRE(r.fight.output().enemies[0].brain == ArenaBrain::StepBack);
        REQUIRE(r.fight.output().enemies[1].token);
        r.bodies.enemies[1] = holderAt;
    };
    {   // Nothing else in the lane: refused, the gauge kept.
        Rig r;
        steppedBack(r, Vec2{0.f, 8.f});
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
        CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Unhurt);
        r.press();
        CHECK_FALSE(r.fight.frozen());
        REQUIRE(r.fight.output().enemies[0].brain == ArenaBrain::StepBack);
        CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Unhurt);
        CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(1000.f));
    }
    {   // A hurtable enemy down the lane: it runs; the held target shows the hit and loses nothing, the other its share.
        Rig r;
        steppedBack(r, Vec2{6.f, 0.f});
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
        REQUIRE(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Ok);
        const int hp0 = r.fight.enemyHp(0), hp1 = r.fight.enemyHp(1);
        r.press();
        REQUIRE(r.fight.frozen());
        CHECK(r.fight.special().target == 0);
        r.runOut();
        const ArenaOutput& end = r.fight.output();
        CHECK(findEvent(end, ArenaEventType::Hit, 0) == nullptr);
        CHECK(findEvent(end, ArenaEventType::Hit, 1) != nullptr);
        const ArenaSpecialView& v = r.fight.special();
        REQUIRE(v.resultCount == 2);
        const int share = static_cast<int>(std::lround(static_cast<float>(m.damage) * m.otherShare));
        for (std::size_t i = 0; i < v.resultCount; ++i) {
            CAPTURE(i);
            CHECK(v.results[i].target == (v.results[i].foe == 0));
            CHECK(v.results[i].damage == (v.results[i].foe == 0 ? 0 : share));
        }
        CHECK(r.fight.enemyHp(0) == hp0);
        CHECK(r.fight.enemyHp(1) == hp1 - share);
        CHECK(r.fight.output().enemies[0].brain != ArenaBrain::Staggered);
        CHECK(r.fight.stats().specialDamage == share);
    }
}

TEST_CASE("arena special: a press on the tick a host blow fills the gauge starts the move, the fill taken first") {
    const ArenaSpecialTable specials = shippedSpecials();
    Rig r;
    r.begin(fightOf({"hulk"}));
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 999.f)));
    r.tick();
    REQUIRE(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::NotReady);
    REQUIRE(r.fight.hostBlow(0, 10, 0)); // reported (HostHit) at the next tick: 10 toward the gauge
    const ArenaOutput& o = r.press();
    REQUIRE(r.fight.frozen());
    CHECK(findEvent(o, ArenaEventType::HostHit, 0) != nullptr);
    CHECK(findEvent(o, ArenaEventType::SpecialStarted) != nullptr);
    CHECK(countEvents(o, ArenaEventType::SpecialReady) == 0); // spent at once: no prompt said
    CHECK(r.fight.specialGaugeForTest(0) == 0.f);
}

TEST_CASE("arena special: a boss on a phase boundary loses the next phase's share; inside a phase the share may cross it") {
    const ArenaSpecialTable specials = shippedSpecials();
    const ArenaSpecialRules& m = specials.moves[0];
    const ArenaEnemyKind k = fightOf({"kite_boss"}).enemies[0];
    REQUIRE(k.breakPhases >= 3);
    const auto after = [&](int startHp) {
        Rig r;
        r.begin(fightOf({"kite_boss"}));
        r.tick();
        REQUIRE(r.fight.hostBlow(0, k.hp - startHp, 0));
        r.tick();
        REQUIRE(r.fight.enemyHp(0) == startHp);
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
        r.press();
        REQUIRE(r.fight.frozen());
        r.runOut();
        return r.fight.enemyHp(0);
    };
    const float hp = static_cast<float>(k.hp);
    const int end0 = static_cast<int>(std::lround(k.breakPhaseEnds[0] * hp));
    const int second = static_cast<int>(std::lround((k.breakPhaseEnds[0] - k.breakPhaseEnds[1]) * hp * m.bossPhaseShare));
    const int first = static_cast<int>(std::lround((1.f - k.breakPhaseEnds[0]) * hp * m.bossPhaseShare));
    CHECK(after(end0) == end0 - second);            // on the boundary: in the second phase
    CHECK(after(end0 + 10) == end0 + 10 - first);   // just above it: the first phase's share, carried past the boundary
    CHECK(end0 + 10 - first < end0);
}

TEST_CASE("arena special: the performer at the press: hitstun refuses (Down); a roll does not, and the cut back ends it") {
    const ArenaSpecialTable specials = shippedSpecials();
    {
        Rig r;
        r.begin(fightOf({"brute"}));
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
        for (int i = 0; i < 60 * 20 && r.fight.output().player.action != ArenaAction::Hitstun; ++i) r.tick();
        REQUIRE(r.fight.output().player.action == ArenaAction::Hitstun);
        CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Down);
        r.press();
        CHECK_FALSE(r.fight.frozen());
        CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(1000.f));
    }
    {
        Rig r;
        r.begin(fightOf({"hulk"}));
        REQUIRE(r.fight.setSpecials(&specials, "devin", "", storeWith("devin", 1000.f)));
        r.tick();
        const Vec2 to{r.bodies.enemies[0].x - r.bodies.player.x, r.bodies.enemies[0].y - r.bodies.player.y};
        const float d = std::sqrt(to.x * to.x + to.y * to.y);
        REQUIRE(d > 4.f);
        ArenaInput roll; // toward the enemy: it stays the target in front
        roll.dodge = true;
        roll.move = Vec2{to.x / d, to.y / d};
        r.tick(roll);
        REQUIRE(r.fight.output().player.action == ArenaAction::Dodge);
        REQUIRE(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Ok);
        r.press();
        REQUIRE(r.fight.frozen());
        r.runOut();
        CHECK(r.fight.output().player.action == ArenaAction::Idle);
        CHECK_FALSE(r.fight.output().player.invulnerable);
    }
}

TEST_CASE("arena special: a party member performs while the player is in hitstun: Running meanwhile, Down after") {
    const ArenaSpecialTable specials = specialsFrom(kMemberMove);
    Rig r;
    ArenaDef def = fightOf({"brute"});
    def.partnerKit = arena().fighter("third");
    r.fight.setAlly(300, 0.4f);
    r.begin(def);
    r.bodies.ally = Vec2{-2.f, 2.f};
    LeadSpecialGauges store = storeWith("identity_a", 1000.f);
    store.entry("devin")->value = 1000.f;
    REQUIRE(r.fight.setSpecials(&specials, "devin", "identity_a", store));
    for (int i = 0; i < 60 * 20 && r.fight.output().player.action != ArenaAction::Hitstun; ++i) r.tick();
    REQUIRE(r.fight.output().player.action == ArenaAction::Hitstun);
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Down);
    REQUIRE(r.fight.specialRefusal(kArenaAlly) == ArenaSpecialRefusal::Ok);
    r.press(kArenaAlly);
    REQUIRE(r.fight.frozen());
    CHECK(r.fight.special().performer == kArenaAlly);
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Running);
    r.runOut();
    CHECK(r.fight.output().player.action == ArenaAction::Hitstun); // the player's state is its own, untouched
    CHECK(r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Down);
    CHECK(r.fight.specialGaugeForTest(0) == doctest::Approx(1000.f));
    CHECK(r.fight.specialGaugeForTest(1) == 0.f);
}

namespace {

struct SpecialTally {
    int fights = 0, wins = 0, fills = 0, uses = 0;
    long long dealt = 0, special = 0;
    float time = 0.f;
    float fillShare = 0.f; // summed over fights with a fill: the first fill's time over the fight's length
    int filled = 0;
};

// The autopilot plays `def` with the special pressed whenever it may start; the gauge carried in `store`.
void playWithSpecial(const ArenaDef& def, std::uint32_t seed, const ArenaSpecialTable& specials, LeadSpecialGauges& store,
                     SpecialTally& t) {
    Rig r;
    r.begin(def, seed);
    REQUIRE(r.fight.setSpecials(&specials, "devin", "", store));
    float firstFill = -1.f;
    while (!r.fight.over() && r.fight.time() < 120.f) {
        ArenaInput in = r.fight.autopilot();
        if (r.fight.specialRefusal(kArenaPlayer) == ArenaSpecialRefusal::Ok) in.special = kArenaPlayer;
        const ArenaOutput& o = r.tick(in);
        const int fills = countEvents(o, ArenaEventType::SpecialReady);
        if (fills > 0 && firstFill < 0.f) firstFill = r.fight.time();
        t.fills += fills;
        if (r.fight.frozen()) r.runOut();
    }
    if (firstFill >= 0.f && r.fight.time() > 0.f) {
        t.fillShare += firstFill / r.fight.time();
        ++t.filled;
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
                                    << " per fight), mean " << t.time / static_cast<float>(t.fights) << " s, first fill at "
                                    << (t.filled > 0 ? 100.f * t.fillShare / static_cast<float>(t.filled) : 0.f)
                                    << "% of the fight, special's share of damage "
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
