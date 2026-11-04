#include "ftcore/ArenaKindCrit.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/BeatChoreo.h" // the skinned kind's clip table
#include "ftcore/WorldLayout.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

// A plain arena kind's weak points (ArenaKindCrit.h): the sidecar loader, the window its opening
// move opens, and the ruling on the blows.
//
// The test kind below has white_glove's three clips, with a Deal of startup 2.25 s and active 0.25 s
// (both exact in binary, so the window's edges read off directly): begun at 10 it opens at 12.5 and,
// open 1.6 s, closes at 14.1.

namespace {

const char* const kTestCrits = R"({
  "version": 1,
  "kinds": {
    "white_glove": { "opensAfter": "deal", "opens": 1.6,
      "weakPoints": { "halfWidth": 0.5, "points": [
        { "id": "fingertip_seam", "x": -0.22, "y": 0.37, "radius": 0.08, "multiplier": 2.0, "facing": 0, "arc": 90 } ] } },
    "kite-boss": { "opensAfter": "dive_2", "opens": 0.5,
      "weakPoints": { "points": [ { "id": "spar", "x": 0, "y": 0.5, "radius": 0.1, "multiplier": 1.5 } ] } }
  }
})";

std::string patched(const std::string& from, const std::string& to) {
    std::string s = kTestCrits;
    const std::size_t at = s.find(from);
    REQUIRE(at != std::string::npos);
    s.replace(at, from.size(), to);
    return s;
}

std::string importError(const std::string& json) {
    ArenaKindCritTable table;
    std::string error;
    CHECK_FALSE(importArenaKindCrits(json, table, &error));
    CHECK(table.size() == 0);
    return error;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const ArenaKindCritTable& testTable() {
    static ArenaKindCritTable table;
    static bool loaded = false;
    if (!loaded) {
        std::string error;
        REQUIRE_MESSAGE(importArenaKindCrits(kTestCrits, table, &error), error);
        loaded = true;
    }
    return table;
}

void setClip(ArenaEnemyMove& m, const char* clip) {
    m.clipText.fill('\0');
    for (std::size_t i = 0; clip[i] && i < kMaxArenaClipId; ++i) m.clipText[i] = clip[i];
}

ArenaEnemyKind dealer() {
    ArenaEnemyKind k;
    k.moveCount = 3;
    setClip(k.moves[0], "deal");
    k.moves[0].attack.startup = 2.25f;
    k.moves[0].attack.active = 0.25f;
    k.moves[0].attack.recovery = 1.6f;
    setClip(k.moves[1], "fan_of_blades");
    setClip(k.moves[2], "extra_elbow");
    return k;
}

constexpr std::uint8_t kSlot = 2;
const Vec2 kBoss{0.0f, 0.0f};
const Vec2 kFacing{1.0f, 0.0f};

// The lead's blow on the slot from `from`, at t.
ArenaBlow leadBlow(Vec2 from, float t) {
    ArenaBlow b;
    b.source = kArenaPlayer;
    b.target = kSlot;
    b.damage = 15;
    b.poise = 12.0f;
    b.from = from;
    b.facing = Vec2{kBoss.x - from.x, kBoss.y - from.y};
    b.at = kBoss;
    b.time = t;
    return b;
}

ArenaKindCrit armed() {
    ArenaKindCrit crit;
    crit.reset(*testTable().find("white_glove"));
    return crit;
}

} // namespace

// ---- The loader ------------------------------------------------------------------------------------

TEST_CASE("arena kind crit loader: the test file loads every field") {
    const ArenaKindCritTable& t = testTable();
    REQUIRE(t.size() == 2);
    const ArenaKindCritDef* g = t.find("white_glove");
    REQUIRE(g);
    CHECK(g->kind() == "white_glove");
    CHECK(g->opensAfter() == "deal");
    CHECK(g->opens == doctest::Approx(1.6));
    REQUIRE(g->weakPoints.points.size() == 1);
    const WeakPoint& p = g->weakPoints.points[0];
    CHECK(p.id() == "fingertip_seam");
    CHECK(p.x == doctest::Approx(-0.22));
    CHECK(p.y == doctest::Approx(0.37));
    CHECK(p.radius == doctest::Approx(0.08));
    CHECK(p.multiplier == doctest::Approx(2.0));
    CHECK(g->weakPoints.points.halfWidth == doctest::Approx(0.5));
    CHECK(g->weakPoints.gates[0].facing == doctest::Approx(0.0));
    CHECK(g->weakPoints.gates[0].arc == doctest::Approx(90.0));

    // Ids with a dash, and the gate's all-round default.
    const ArenaKindCritDef* k = t.find("kite-boss");
    REQUIRE(k);
    CHECK(k->opensAfter() == "dive_2");
    CHECK(k->weakPoints.points.halfWidth == doctest::Approx(kDefaultHalfWidth));
    CHECK(k->weakPoints.gates[0].arc == doctest::Approx(180.0));
    CHECK_FALSE(t.find("spitter"));

    ArenaKindCritTable empty;
    CHECK(importArenaKindCrits(R"({ "version": 1, "kinds": {} })", empty));
    CHECK(empty.size() == 0);
}

TEST_CASE("arena kind crit loader: refusals carry their path; unknown keys warn") {
    CHECK(has(importError("{"), "arena crit:"));
    CHECK(has(importError("[]"), "arena crit: expected an object"));
    CHECK(has(importError(patched("\"version\": 1", "\"version\": 2")), "version: expected 1"));
    CHECK(has(importError(R"({ "version": 1 })"), "kinds: expected an object of 0..8 kinds"));
    CHECK(has(importError(R"({ "kinds": [] })"), "kinds: expected an object"));
    CHECK(has(importError(patched("\"white_glove\": {", "\"white glove\": {")),
              "kinds.white glove: expected a kind id of 1..31"));
    CHECK(has(importError(patched("\"kite-boss\": {", "\"white_glove\": {")),
              "kinds.white_glove: duplicate kind id"));
    CHECK(has(importError(patched("\"opensAfter\": \"deal\"", "\"opensAfter\": \"\"")),
              "kinds.white_glove.opensAfter: expected a clip id"));
    CHECK(has(importError(patched("\"opensAfter\": \"deal\", ", "")), "kinds.white_glove.opensAfter:"));
    CHECK(has(importError(patched("\"opens\": 1.6", "\"opens\": 0")),
              "kinds.white_glove.opens: expected a number in 0.1..10"));
    CHECK(has(importError(patched("\"opens\": 1.6", "\"opens\": \"long\"")), "kinds.white_glove.opens:"));
    // The weak points through ArenaBoss.h's own parser, under the entry's path.
    CHECK(has(importError(patched("\"x\": -0.22", "\"x\": -1.5")),
              "kinds.white_glove.weakPoints.points[0].x: expected a number in -1..1"));
    CHECK(has(importError(patched("\"radius\": 0.08", "\"radius\": 0.9")),
              "kinds.white_glove.weakPoints.points[0].radius:"));
    CHECK(has(importError(patched("\"arc\": 90", "\"arc\": 200")), "kinds.white_glove.weakPoints.points[0].arc:"));
    CHECK(has(importError(patched("\"halfWidth\": 0.5", "\"halfWidth\": 0")),
              "kinds.white_glove.weakPoints.halfWidth: expected a number above 0"));
    CHECK(has(importError(patched("\"id\": \"fingertip_seam\"", "\"id\": \"Seam\"")),
              "kinds.white_glove.weakPoints.points[0].id:"));
    CHECK(has(importError(patched("\"points\": [ { \"id\": \"spar\", \"x\": 0, \"y\": 0.5, \"radius\": 0.1, "
                                  "\"multiplier\": 1.5 } ]",
                                  "\"points\": []")),
              "kinds.kite-boss.weakPoints.points: expected 1..3 points"));
    CHECK(has(importError(patched("\"opens\": 0.5,\n      \"weakPoints\": { \"points\": [ { \"id\": \"spar\", \"x\": 0, "
                                  "\"y\": 0.5, \"radius\": 0.1, \"multiplier\": 1.5 } ] } }",
                                  "\"opens\": 0.5 }")),
              "kinds.kite-boss.weakPoints: expected an object"));
    {
        std::string kinds;
        for (std::size_t i = 0; i < kMaxArenaKindCrits; ++i)
            kinds += "\"k" + std::to_string(i) +
                     "\": { \"opensAfter\": \"a\", \"opens\": 1, \"weakPoints\": { \"points\": [ { \"id\": \"p\", "
                     "\"x\": 0, \"y\": 0.5, \"radius\": 0.1, \"multiplier\": 2 } ] } },\n";
        CHECK(has(importError(patched("\"kinds\": {\n", "\"kinds\": {\n" + kinds)),
                  "kinds: expected an object of 0..8 kinds"));
    }
    {
        // Several problems: one per line.
        std::string both = patched("\"opens\": 1.6", "\"opens\": 11");
        const std::string arc = "\"arc\": 90";
        both.replace(both.find(arc), arc.size(), "\"arc\": 200");
        const std::string many = importError(both);
        CHECK(has(many, "kinds.white_glove.opens:"));
        CHECK(has(many, "\nkinds.white_glove.weakPoints.points[0].arc:"));
    }

    ArenaKindCritTable table;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(importArenaKindCrits(patched("\"version\": 1", "\"version\": 1, \"tempo\": 3"), table, &error, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "unknown key: tempo");
    warnings.clear();
    CHECK(importArenaKindCrits(patched("\"opens\": 1.6", "\"opens\": 1.6, \"closes\": 2"), table, &error, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "kinds.white_glove: unknown key: closes");
    warnings.clear();
    CHECK(importArenaKindCrits(patched("\"arc\": 90", "\"arc\": 90, \"glow\": 1"), table, &error, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "kinds.white_glove.weakPoints.points[0]: unknown key: glow");
}

TEST_CASE("arena kind crit data: the shipped file loads clean, white_glove's seam on its Deal") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena_crit.json", text));
    ArenaKindCritTable table;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importArenaKindCrits(text, table, &error, &warnings), error);
    CHECK(warnings.empty());
    const ArenaKindCritDef* g = table.find("white_glove");
    REQUIRE(g);
    CHECK(g->opensAfter() == "deal");
    CHECK(g->opens == doctest::Approx(1.6));
    REQUIRE(g->weakPoints.points.size() == 1);
    const WeakPoint& p = g->weakPoints.points[0];
    CHECK(p.id() == "fingertip_seam");
    CHECK(p.multiplier == doctest::Approx(2.0));
    // About 1.0 m up a 2.7 m figure, on its right hand: the viewer's left through the front gate.
    CHECK(p.y * 2.7f == doctest::Approx(1.0).epsilon(0.05));
    CHECK(p.x < 0.0f);
    CHECK(g->weakPoints.gates[0].facing == doctest::Approx(0.0));
    CHECK(g->weakPoints.gates[0].arc == doctest::Approx(90.0));
    // Its effects hang from the seam's finger, and each is one of Data/Vfx's.
    CHECK(g->effects.socket() == "middle_03_r");
    for (const std::string_view fx : {g->effects.tell(), g->effects.open(), g->effects.crit(), g->effects.hit()}) {
        CAPTURE(fx);
        REQUIRE_FALSE(fx.empty());
        std::string vfx;
        CHECK(ftcore_test::readProjectData("Vfx/" + std::string(fx) + ".json", vfx));
    }
}

TEST_CASE("arena kind crit loader: effects are optional names, each refused with its path") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena_crit.json", text));
    const auto load = [](const std::string& json, std::string& error, std::vector<std::string>& warnings) {
        ArenaKindCritTable table;
        error.clear();
        warnings.clear();
        const bool ok = importArenaKindCrits(json, table, &error, &warnings);
        return ok ? *table.find("white_glove") : ArenaKindCritDef{};
    };
    const auto swap = [&](std::string_view from, std::string_view to) {
        std::string t = text;
        const std::size_t at = t.find(from);
        REQUIRE(at != std::string::npos);
        t.replace(at, from.size(), to);
        return t;
    };
    std::string error;
    std::vector<std::string> warnings;
    // Without the block, every name is empty.
    // (From the comma before it: a checkout may have either line ending.)
    const std::size_t key = text.find("\"effects\"");
    REQUIRE(key != std::string::npos);
    const std::size_t at = text.rfind(',', key);
    REQUIRE(at != std::string::npos);
    const std::size_t end = text.find('}', at);
    std::string bare = text;
    bare.erase(at, end + 1 - at);
    const ArenaKindCritDef none = load(bare, error, warnings);
    CHECK(error.empty());
    CHECK(none.kind() == "white_glove");
    CHECK(none.effects.socket().empty());
    CHECK(none.effects.hit().empty());
    // A missing key stays empty; the others load.
    const ArenaKindCritDef noHit = load(swap(", \"hit\": \"ink_hit\"", ""), error, warnings);
    CHECK(error.empty());
    CHECK(noHit.effects.hit().empty());
    CHECK(noHit.effects.crit() == "dealer_seam_crit");
    load(swap("\"crit\": \"dealer_seam_crit\"", "\"crit\": \"seam crit\""), error, warnings);
    CHECK(error.find("kinds.white_glove.effects.crit: expected a name of 1..31") != std::string::npos);
    load(swap("\"socket\": \"middle_03_r\"", "\"socket\": 3"), error, warnings);
    CHECK(error.find("kinds.white_glove.effects.socket:") != std::string::npos);
    load(swap("\"effects\": {", "\"effects\": [ ], \"x\": {"), error, warnings);
    CHECK(error.find("kinds.white_glove.effects: expected an object") != std::string::npos);
    load(swap("\"hit\": \"ink_hit\"", "\"hit\": \"ink_hit\", \"miss\": \"x\""), error, warnings);
    CHECK(error.empty());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "kinds.white_glove.effects: unknown key: miss");
}

// ---- The window and the ruling ---------------------------------------------------------------------

TEST_CASE("arena kind crit: a Deal opens the seam at the end of its active part, for 1.6 s") {
    ArenaKindCrit crit = armed();
    const ArenaEnemyKind kind = dealer();
    CHECK(crit.opensOn(kind));
    CHECK_FALSE(crit.isOpen(12.5));

    CHECK(crit.onAttackStarted(0, 10.0, kind));
    CHECK(crit.opensAt() == doctest::Approx(12.5));
    CHECK(crit.closesAt() == doctest::Approx(14.1));
    CHECK(crit.openings() == 1);
    CHECK_FALSE(crit.isOpen(12.49));
    CHECK(crit.isOpen(12.5));
    CHECK(crit.isOpen(14.09));
    CHECK_FALSE(crit.isOpen(14.1));

    const Vec2 front{2.0f, 0.0f};
    // Just before the window: plain.
    ArenaBlowRuling r = crit.rule(leadBlow(front, 12.49f), kSlot, kBoss, kFacing);
    CHECK(r.damage == 1.0f);
    CHECK(r.poise == 1.0f);
    CHECK_FALSE(r.veto);
    CHECK(crit.crits() == 0);
    CHECK(crit.lastPoint() == kMaxWeakPoints);

    // On its first moment: x2 on damage and poise.
    r = crit.rule(leadBlow(front, 12.5f), kSlot, kBoss, kFacing);
    CHECK(r.damage == doctest::Approx(2.0));
    CHECK(r.poise == doctest::Approx(2.0));
    CHECK_FALSE(r.veto);
    CHECK(crit.crits() == 1);
    CHECK(crit.lastPoint() == 0);
    CHECK(crit.lastMultiplier() == doctest::Approx(2.0));
    CHECK(crit.lastDamage() == 30);
    CHECK(crit.lastCritAt() == doctest::Approx(12.5));
    CHECK(crit.points().points[crit.lastPoint()].id() == "fingertip_seam");

    // Every blow inside it crits, not only the first.
    r = crit.rule(leadBlow(front, 13.0f), kSlot, kBoss, kFacing);
    CHECK(r.damage == doctest::Approx(2.0));
    CHECK(crit.crits() == 2);

    // At startAt + 1.6 it has shut.
    r = crit.rule(leadBlow(front, 14.1f), kSlot, kBoss, kFacing);
    CHECK(r.damage == 1.0f);
    CHECK(r.poise == 1.0f);
    CHECK(crit.crits() == 2);

    // A second Deal replaces the window; reset clears everything.
    CHECK(crit.onAttackStarted(0, 20.0, kind));
    CHECK(crit.openings() == 2);
    CHECK(crit.isOpen(23.0));
    crit.reset(*testTable().find("white_glove"));
    CHECK_FALSE(crit.isOpen(23.0));
    CHECK(crit.openings() == 0);
    CHECK(crit.crits() == 0);
    CHECK(crit.lastPoint() == kMaxWeakPoints);
}

TEST_CASE("arena kind crit: another move, a bad move index or a bad time opens nothing") {
    ArenaKindCrit crit = armed();
    const ArenaEnemyKind kind = dealer();
    CHECK_FALSE(crit.onAttackStarted(1, 10.0, kind)); // fan_of_blades
    CHECK_FALSE(crit.onAttackStarted(2, 10.0, kind)); // extra_elbow
    CHECK_FALSE(crit.onAttackStarted(3, 10.0, kind)); // past moveCount
    CHECK_FALSE(crit.onAttackStarted(0, std::numeric_limits<double>::quiet_NaN(), kind));
    CHECK(crit.openings() == 0);
    for (const float t : {10.0f, 11.5f, 12.5f, 13.0f, 14.0f}) {
        CAPTURE(t);
        CHECK_FALSE(crit.isOpen(t));
        const ArenaBlowRuling r = crit.rule(leadBlow({2.0f, 0.0f}, t), kSlot, kBoss, kFacing);
        CHECK(r.damage == 1.0f);
        CHECK(r.poise == 1.0f);
    }
    CHECK(crit.crits() == 0);

    // A kind none of whose moves plays the clip never opens.
    ArenaEnemyKind other = kind;
    setClip(other.moves[0], "deal_two");
    CHECK_FALSE(crit.opensOn(other));
    CHECK_FALSE(crit.onAttackStarted(0, 10.0, other));
}

TEST_CASE("arena kind crit: only the lead's blows on the slot are judged; the slot's own pass untouched") {
    ArenaKindCrit crit = armed();
    REQUIRE(crit.onAttackStarted(0, 10.0, dealer()));
    REQUIRE(crit.isOpen(13.0));

    // The Dealer's blow on the lead while its seam is open.
    ArenaBlow hit;
    hit.source = kSlot;
    hit.target = kArenaPlayer;
    hit.damage = 20;
    hit.poise = 24.0f;
    hit.from = kBoss;
    hit.facing = kFacing;
    hit.at = Vec2{2.0f, 0.0f};
    hit.time = 13.0f;
    ArenaBlowRuling r = crit.rule(hit, kSlot, kBoss, kFacing);
    CHECK(r.damage == 1.0f);
    CHECK(r.poise == 1.0f);
    CHECK_FALSE(r.veto);

    // The lead's blow on another enemy.
    ArenaBlow elsewhere = leadBlow({2.0f, 0.0f}, 13.0f);
    elsewhere.target = static_cast<std::uint8_t>(kSlot + 1);
    r = crit.rule(elsewhere, kSlot, kBoss, kFacing);
    CHECK(r.damage == 1.0f);
    CHECK(r.poise == 1.0f);
    CHECK(crit.crits() == 0);
}

TEST_CASE("arena kind crit: a blow from behind, outside the front gate, is plain") {
    ArenaKindCrit crit = armed();
    REQUIRE(crit.onAttackStarted(0, 10.0, dealer()));
    // Straight behind, and behind on its right: outside 90 degrees of its facing.
    for (const Vec2 from : {Vec2{-2.0f, 0.0f}, Vec2{-1.5f, 1.5f}}) {
        CAPTURE(from.x);
        CAPTURE(from.y);
        const ArenaBlowRuling r = crit.rule(leadBlow(from, 13.0f), kSlot, kBoss, kFacing);
        CHECK(r.damage == 1.0f);
        CHECK(r.poise == 1.0f);
    }
    CHECK(crit.crits() == 0);
    // Just ahead of square on its right: inside the gate.
    CHECK(crit.rule(leadBlow({0.2f, 2.0f}, 13.0f), kSlot, kBoss, kFacing).damage == doctest::Approx(2.0));
    // The boss turned round: the same body is now in front of it.
    CHECK(crit.rule(leadBlow({-2.0f, 0.0f}, 13.0f), kSlot, kBoss, Vec2{-1.0f, 0.0f}).damage == doctest::Approx(2.0));
    CHECK(crit.crits() == 2);
}

TEST_CASE("arena kind crit: a fight's worth of deals and blows allocates nothing") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const ArenaKindCritDef& def = *testTable().find("white_glove");
    const ArenaEnemyKind kind = dealer();
    ArenaKindCrit crit;
    float sink = 0.0f;
    const std::uint64_t before = ftcore_test::allocStats().count;
    crit.reset(def);
    for (int frame = 0; frame < 60 * 120; ++frame) {
        const float t = static_cast<float>(frame) / 60.0f;
        if (frame % 300 == 0) crit.onAttackStarted(static_cast<std::uint8_t>((frame / 300) % 3), t, kind);
        sink += crit.rule(leadBlow({2.0f, 0.0f}, t), kSlot, kBoss, kFacing).damage;
        sink += crit.isOpen(t) ? 1.0f : 0.0f;
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink > 0.0f);
    CHECK(crit.crits() > 0);
}

// ---- The shipped Dealer under the autopilot --------------------------------------------------------

namespace {

// The host's side of the hook, as FTArenaRunner plays it: the slot's window opened by its moves' starts, each blow
// ruled on with the slot's facing from the last tick.
struct DealerHost {
    ArenaFight* fight = nullptr;
    ArenaKindCrit crit;
};

ArenaBlowRuling ruleDealerBlow(void* user, const ArenaBlow& blow) {
    DealerHost& host = *static_cast<DealerHost*>(user);
    if (blow.target != 0) return ArenaBlowRuling();
    return host.crit.rule(blow, 0, blow.at, host.fight->output().enemies[0].facing);
}

struct DealerRun {
    ArenaPhase phase = ArenaPhase::NotStarted;
    float time = 0.0f;
    int staggers = 0;
    int crits = 0;
    int hp = 0;
};

// white_glove alone in the stone ring, as `Demo_Arena -ArenaStage=stone_ring -ArenaLineup=white_glove -ArenaLead=<lead>
// -ArenaAutoPlay` plays it: the lead's kit, the zone fitted to the Dealer, the seam's hook.
DealerRun playDealer(const char* lead, std::uint32_t seed) {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    ArenaTable arena;
    std::string error;
    REQUIRE_MESSAGE(importArenaTable(text, arena, &error), error);
    REQUIRE(ftcore_test::readProjectData("Field/worlds.json", text));
    WorldTable worlds;
    REQUIRE_MESSAGE(importWorldTable(text, worlds, &error), error);
    REQUIRE(ftcore_test::readProjectData("Battle/arenas.json", text));
    ArenaZoneTable zones;
    REQUIRE_MESSAGE(importArenaZoneTable(text, arena, worlds, zones, &error), error);
    REQUIRE(ftcore_test::readProjectData("Battle/arena_crit.json", text));
    ArenaKindCritTable crits;
    REQUIRE_MESSAGE(importArenaKindCrits(text, crits, &error), error);

    const ArenaZoneSpec* spec = zones.find("stone_ring");
    REQUIRE(spec);
    ArenaDef def = arena.fallback;
    def.fighter = arena.fighter(lead);
    def.enemyCount = 0;
    for (std::size_t e = 0; e < arena.enemyIds.size(); ++e)
        if (arena.enemyIds[e] == "white_glove") {
            def.enemies[0] = arena.enemies[e];
            def.enemyKind[0] = static_cast<std::uint8_t>(e);
            def.enemyCount = 1;
        }
    REQUIRE(def.enemyCount == 1);
    def.attackers = 1;
    ArenaZoneFit fit;
    REQUIRE_MESSAGE(fitArenaZone(*spec, nullptr, def, fit, &error), error);
    fit.apply(def);

    ArenaFight fight;
    DealerHost host;
    host.fight = &fight;
    host.crit.reset(*crits.find("white_glove"));
    fight.setBlowHook(&ruleDealerBlow, &host);
    fight.begin(def, fit.ring, fit.playerStart, fit.playerFacing, seed);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    bodies.enemies[0] = fight.enemyStart(0);
    DealerRun run;
    while (!fight.over() && fight.time() < 180.0f) {
        const ArenaOutput& out = fight.tick(1.0f / 60.0f, fight.autopilot(), bodies);
        arenaStep(fight, bodies, 1.0f / 60.0f, fit.zone);
        for (const ArenaEvent& e : out.events) {
            if (e.type == ArenaEventType::AttackStarted && e.source == 0) host.crit.onAttackStarted(e.move, fight.time(), def.enemies[0]);
            if (e.type == ArenaEventType::Staggered && e.target == 0) ++run.staggers;
        }
    }
    run.phase = fight.phase();
    run.time = fight.time();
    run.crits = host.crit.crits();
    run.hp = fight.hp();
    return run;
}

} // namespace

// Its poise breaks a few times a fight, whoever fights it: the demo's seed (1), each lead's kit.
TEST_CASE("arena kind crit: the shipped Dealer staggers under each lead's autopilot") {
    for (const std::string lead : {"devin", "duy", "third"}) {
        const DealerRun run = playDealer(lead.c_str(), 1);
        MESSAGE(lead << ": " << std::string(run.phase == ArenaPhase::Victory ? "won" : "lost") << " in " << run.time << " s, hp " << run.hp
                     << ", staggers " << run.staggers << ", crits " << run.crits);
        CHECK_MESSAGE(run.phase == ArenaPhase::Victory, "Error: " << lead << "'s autopilot did not beat white_glove in 180 s");
        CHECK_MESSAGE(run.staggers >= 2, "Error: white_glove staggered " << run.staggers << " times against " << lead);
        CHECK(run.crits > 0);
    }
}

// Each of its moves plays its own clip: the blow on the end of the startup at a play rate of 1, the clip as long as the move.
TEST_CASE("arena kind crit: the shipped Dealer's moves name clips of its table, impacts on the end of the startup") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    ArenaTable arena;
    std::string error;
    REQUIRE_MESSAGE(importArenaTable(text, arena, &error), error);
    const ArenaEnemyKind* boss = nullptr;
    for (std::size_t e = 0; e < arena.enemyIds.size(); ++e)
        if (arena.enemyIds[e] == "white_glove") boss = &arena.enemies[e];
    REQUIRE(boss);
    CHECK(boss->look.clips() == "Data/Anim/white_glove_clips.json");
    REQUIRE(ftcore_test::readProjectData("Anim/white_glove_clips.json", text));
    BeatClips clips;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importBeatClips(text, clips, &error, &warnings), error);
    CHECK(warnings.empty());
    // Its own crossfade, longer than the host's 0.12 s, so a blow taken mid-swing eases into the reaction.
    CHECK(clips.blend >= 0.2);
    CHECK(clips[static_cast<std::size_t>(clips.find("idle"))].loop);
    REQUIRE(boss->moveCount == 3);
    for (std::size_t m = 0; m < boss->moveCount; ++m) {
        const ArenaEnemyMove& mv = boss->moves[m];
        const int clip = clips.find(mv.clip());
        REQUIRE_MESSAGE(clip >= 0, "move " << m << " clip " << mv.clip());
        const BeatClip& c = clips[static_cast<std::size_t>(clip)];
        CHECK_FALSE(c.loop);
        const double rate = c.impact / static_cast<double>(mv.attack.startup);
        CHECK_MESSAGE(std::fabs(rate - 1.0) < 0.01, mv.clip() << " rate " << rate);
        CHECK_MESSAGE(std::fabs(c.length - static_cast<double>(mv.attack.total())) < 0.01, mv.clip());
    }
    // The state clips last as long as the state, so a riposte or a stagger never outlasts or cuts its clip.
    const std::pair<const char*, float> states[] = {
        {"stagger", boss->staggerTime}, {"open", boss->openTime}, {"face_down", boss->downTime}};
    for (const auto& [id, time] : states) {
        const int clip = clips.find(id);
        REQUIRE_MESSAGE(clip >= 0, "clip " << id);
        const BeatClip& c = clips[static_cast<std::size_t>(clip)];
        CHECK_FALSE(c.loop);
        CHECK_MESSAGE(std::fabs(c.length - static_cast<double>(time)) < 0.01, id << " length " << c.length);
    }
    for (const char* id : {"hit_light", "hit_heavy", "defeat"}) CHECK_MESSAGE(clips.find(id) >= 0, "clip " << id);
    // The two short hit reactions land at once on whatever pose the blow finds (additive over the crossfade); the
    // longer state clips and the idle ease in as every other clip does.
    for (const char* id : {"hit_light", "hit_heavy"}) CHECK_MESSAGE(clips[static_cast<std::size_t>(clips.find(id))].additiveIn, id);
    for (const char* id : {"idle", "stagger", "open", "face_down", "deal"})
        CHECK_FALSE_MESSAGE(clips[static_cast<std::size_t>(clips.find(id))].additiveIn, id);
}
