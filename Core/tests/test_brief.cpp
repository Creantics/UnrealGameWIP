// Briefs (Data/Battle/briefs.json) laid over the guns lead's kit, and the pistol's cylinder (rounds, reload).
#include "ftcore/ArenaFight.h"
#include "ftcore/Brief.h"
#include "ftcore/UiViews.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaTable shippedArena() {
    std::string text;
    ArenaTable t;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) return t;
    std::string err;
    importArenaTable(text, t, &err);
    return t;
}

bool shippedBriefs(const ArenaFighter& lead, BriefTable& out, std::string& err) {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/briefs.json", text)) {
        err = "Data/Battle/briefs.json not found";
        return false;
    }
    return importBriefTable(text, lead, out, &err);
}

ArenaEnemyKind standingKind() {
    ArenaEnemyKind k;
    k.hp = 100000;
    k.poise = 100000.f;
    k.attackDelay = 1000.f;
    k.bodyRadius = 0.45f;
    k.passive = true;
    return k;
}

// A fight of `kit` against one standing enemy 10 m ahead; the bodies never move.
struct Range {
    ArenaDef def;
    ArenaFight fight;
    ArenaBodies bodies;
    ArenaInput in;
    int ticks = 0;
    explicit Range(const ArenaFighter& kit) {
        def.fighter = kit;
        def.enemies[0] = standingKind();
        def.enemyCount = 1;
        fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
        bodies.enemies[0] = Vec2{10.f, 0.f};
    }
    // One tick; `shoot` presses the aimed light, `kick` the unaimed one.
    void tick(bool shoot, bool kick = false) {
        in.aim = shoot;
        in.aimDir = Vec2{1.f, 0.f};
        in.light = shoot || kick;
        fight.tick(kDt, in, bodies);
        ++ticks;
    }
    float time() const { return static_cast<float>(ticks) * kDt; }
};

const char* const kOneBrief = R"({ "version": 1, "common": { "rounds": 6, "reload": 1.2, "shot": { "stamina": 0 } },
  "briefs": [ { "id": "notary", "base": [7, 15, 9, 10, 9], "growth": [25, 90, 45, 45, 45], "kit": { "shot": { "range": 36 } } } ] })";

} // namespace

TEST_CASE("briefs: names round-trip and unknown text is refused") {
    for (std::size_t i = 0; i < kBriefCount; ++i) {
        BriefId b = BriefId::Count;
        CHECK(briefFromName(briefName(static_cast<BriefId>(i)), b));
        CHECK(b == static_cast<BriefId>(i));
    }
    BriefId b = BriefId::Counsel;
    CHECK_FALSE(briefFromName("Bailiff", b));
    CHECK_FALSE(briefFromName("", b));
    CHECK(std::string(briefName(BriefId::Count)) == "none");
}

TEST_CASE("briefs: the shipped table lays bailiff and notary over the guns lead's row (slice 1)") {
    const ArenaTable arena = shippedArena();
    const ArenaFighter& duy = arena.fighter("duy");
    REQUIRE(duy.weapon == ArenaWeapon::Guns);
    CHECK(duy.rounds == 0); // the lead's own row has no cylinder: fights without a Brief are unchanged
    BriefTable t;
    std::string err;
    REQUIRE_MESSAGE(shippedBriefs(duy, t, err), err);
    CHECK(t.find(BriefId::Counsel) == nullptr);
    CHECK(t.find(BriefId::Proxy) == nullptr);
    CHECK(t.find(BriefId::Escrow) == nullptr);
    const BriefDef* bailiff = t.find(BriefId::Bailiff);
    const BriefDef* notary = t.find(BriefId::Notary);
    REQUIRE(bailiff);
    REQUIRE(notary);
    for (const BriefDef* d : {bailiff, notary}) {
        CHECK(d->kit.rounds == 6);
        CHECK(d->kit.reload == doctest::Approx(1.2f));
        CHECK(d->kit.shot.stamina == doctest::Approx(0.f));
        CHECK(d->kit.weapon == ArenaWeapon::Guns);
        CHECK(d->kit.hp == duy.hp);
        CHECK(d->kit.lightCount == duy.lightCount);
    }
    // Bailiff: kicks x1.10, kick poise x1.20, poise x1.25, guard leak x0.80, the slower shot (D.C).
    CHECK(bailiff->base == std::array<int, 5>{14, 7, 12, 9, 8});
    CHECK(bailiff->growth == std::array<int, 5>{85, 30, 70, 35, 30});
    CHECK(bailiff->kit.light[0].damage == 12);
    CHECK(bailiff->kit.light[0].poiseDamage == doctest::Approx(12.f));
    CHECK(bailiff->kit.light[0].startup == doctest::Approx(duy.light[0].startup)); // a partial row keeps the rest
    CHECK(bailiff->kit.light[2].damage == 19);
    CHECK(bailiff->kit.light[2].poiseDamage == doctest::Approx(24.f));
    CHECK(bailiff->kit.heavy.damage == 40);
    CHECK(bailiff->kit.heavy.poiseDamage == doctest::Approx(54.f));
    CHECK(bailiff->kit.riposte.damage == 48);
    CHECK(bailiff->kit.poise == doctest::Approx(duy.poise * 1.25f));
    CHECK(1.f - bailiff->kit.guardAbsorb == doctest::Approx((1.f - duy.guardAbsorb) * 0.8f));
    CHECK(bailiff->kit.shot.startup == doctest::Approx(duy.shot.startup + 0.06f));
    CHECK(bailiff->kit.shot.recovery == doctest::Approx(duy.shot.recovery + 0.08f));
    CHECK(bailiff->kit.shot.damage == duy.shot.damage);
    // Notary: shot x1.15 (11.5 rounds up), range x1.20, shot poise x1.25, kicks x0.90.
    CHECK(notary->base == std::array<int, 5>{7, 15, 9, 10, 9});
    CHECK(notary->kit.shot.damage == 12);
    CHECK(notary->kit.shot.range == doctest::Approx(36.f));
    CHECK(notary->kit.shot.poiseDamage == doctest::Approx(7.5f));
    CHECK(notary->kit.shot.startup == doctest::Approx(duy.shot.startup));
    CHECK(notary->kit.light[0].damage == 10);
    CHECK(notary->kit.light[2].damage == 15);
    CHECK(notary->kit.heavy.damage == 32);
    CHECK(notary->kit.riposte.damage == 40);
    CHECK(notary->kit.poise == doctest::Approx(duy.poise));
}

TEST_CASE("briefs: refused files leave the table empty and name the problem") {
    const ArenaTable arena = shippedArena();
    const ArenaFighter& duy = arena.fighter("duy");
    BriefTable t;
    std::string err;
    REQUIRE_MESSAGE(importBriefTable(kOneBrief, duy, t, &err), err);
    CHECK(t.find(BriefId::Notary));

    const auto refused = [&](const std::string& json, const char* needle) {
        BriefTable u;
        std::string e;
        CHECK_FALSE(importBriefTable(json, duy, u, &e));
        CHECK_MESSAGE(e.find(needle) != std::string::npos, e);
        CHECK(u.find(BriefId::Notary) == nullptr);
    };
    const std::string row = R"("base": [7, 15, 9, 10, 9], "growth": [25, 90, 45, 45, 45])";
    refused(R"({ "version": 2, "briefs": [] })", "version");
    refused(R"({ "version": 1 })", "briefs: expected an array");
    refused(R"({ "version": 1, "briefs": [ { "id": "clerk", )" + row + " } ] }", "briefs[0].id");
    refused(R"({ "version": 1, "briefs": [ { "id": "notary", )" + row + R"( }, { "id": "notary", )" + row + " } ] }",
            "duplicate brief");
    refused(R"({ "version": 1, "briefs": [ { "id": "notary", "base": [7, 15, 9, 10, 10], "growth": [25, 90, 45, 45, 45] } ] })",
            "sum of 50");
    refused(R"({ "version": 1, "briefs": [ { "id": "notary", "base": [7, 15, 9, 10, 9], "growth": [25, 90, 45, 45, 46] } ] })",
            "sum of 250");
    refused(R"({ "version": 1, "briefs": [ { "id": "notary", "base": [7, 15, 9, 10], "growth": [25, 90, 45, 45, 45] } ] })",
            "array of 5");
    refused(R"({ "version": 1, "briefs": [ { "id": "notary", )" + row + R"(, "kit": { "weapon": "magic" } } ] })",
            "id or a weapon");
    refused(R"({ "version": 1, "briefs": [ { "id": "notary", )" + row + R"(, "kit": { "shoot": 1 } } ] })",
            "unknown key: shoot");
    refused(R"({ "version": 1, "briefs": [ { "id": "notary", )" + row + R"(, "passive": {} } ] })", "unknown key: passive");
    refused(R"({ "version": 1, "common": { "rounds": 31 }, "briefs": [] })", "common.rounds");
    refused(R"({ "version": 1, "briefs": [ { "id": "notary", )" + row + R"(, "kit": { "reload": -1 } } ] })", "kit.reload");

    // A cylinder needs a guns kit.
    const ArenaFighter& devin = arena.fighter("devin");
    BriefTable u;
    std::string e;
    CHECK_FALSE(importBriefTable(kOneBrief, devin, u, &e));
    CHECK_MESSAGE(e.find("only a guns kit") != std::string::npos, e);
}

TEST_CASE("cylinder: six rounds, then a reload that refuses only the shot; restart fills it (D.J 34)") {
    const ArenaTable arena = shippedArena();
    BriefTable t;
    std::string err;
    REQUIRE_MESSAGE(shippedBriefs(arena.fighter("duy"), t, err), err);
    const ArenaFighter kit = t.find(BriefId::Notary)->kit;
    Range r(kit);
    CHECK(r.fight.roundsMax() == 6);
    CHECK(r.fight.rounds() == 6);
    CHECK(r.fight.reloadProgress() == doctest::Approx(0.f));

    // Firing on cooldown: every tick presses the aimed light.
    std::vector<float> starts;
    int seen = 0;
    float emptyAt = -1.f, reloadSeenAt = -1.f;
    while (r.time() < 8.f && starts.size() < 7) {
        r.tick(true);
        const int now = r.fight.stats().shots;
        if (now > seen) {
            seen = now;
            starts.push_back(r.time());
        }
        if (emptyAt < 0.f && r.fight.rounds() == 0) emptyAt = r.time();
        if (reloadSeenAt < 0.f && r.fight.reloadProgress() > 0.f) reloadSeenAt = r.time();
    }
    REQUIRE(starts.size() == 7);
    CHECK(r.fight.stats().reloads == 1);
    CHECK(r.fight.stats().dryPresses > 0);
    CHECK(emptyAt == doctest::Approx(starts[5]));
    // The reload starts once the last round's shot has ended, and runs 1.2 s.
    const float shotEnd = starts[5] + kit.shot.total();
    CHECK(reloadSeenAt >= shotEnd - kDt);
    CHECK(reloadSeenAt <= shotEnd + 3.f * kDt);
    CHECK(starts[6] - shotEnd >= kit.reload - kDt);
    CHECK(starts[6] - shotEnd <= kit.reload + 3.f * kDt);
    CHECK(r.fight.rounds() == 5); // the 7th spent one of a full cylinder

    // Empty the cylinder again, then kick during the reload: the kick starts, the shot does not.
    while (r.fight.rounds() > 0 && r.time() < 20.f) r.tick(true);
    while (r.fight.reloadProgress() <= 0.f && r.time() < 20.f) r.tick(false);
    REQUIRE(r.fight.reloadProgress() > 0.f);
    ArenaHudLead hud;
    fillArenaHudLead(r.fight, hud);
    CHECK(hud.roundsMax == 6);
    CHECK(hud.rounds == 0);
    CHECK(hud.reload01 > 0.f);
    CHECK(hud.reload01 < 1.f);
    const int lights = r.fight.stats().lights, shots = r.fight.stats().shots;
    for (int i = 0; i < 6; ++i) r.tick(false, true);
    CHECK(r.fight.stats().lights == lights + 1);
    for (int i = 0; i < 30; ++i) r.tick(true);
    CHECK(r.fight.stats().shots == shots);
    CHECK(r.fight.rounds() == 0);

    r.fight.restart();
    CHECK(r.fight.rounds() == 6);
    CHECK(r.fight.reloadProgress() == doctest::Approx(0.f));
}

TEST_CASE("cylinder: a kit with no rounds fires on stamina alone and reports no cylinder") {
    const ArenaTable arena = shippedArena();
    ArenaFighter kit = arena.fighter("duy");
    kit.shot.stamina = 0.f; // so stamina never stops it either
    Range r(kit);
    CHECK(r.fight.roundsMax() == 0);
    ArenaHudLead hud;
    fillArenaHudLead(r.fight, hud);
    CHECK(hud.roundsMax == 0);
    CHECK(hud.rounds == 0);
    CHECK(hud.reload01 == doctest::Approx(0.f));
    for (int i = 0; i < 600; ++i) r.tick(true);
    CHECK(r.fight.stats().shots > 6);
    CHECK(r.fight.stats().reloads == 0);
    CHECK(r.fight.stats().dryPresses == 0);
    CHECK(r.fight.rounds() == 0);
    CHECK(r.fight.reloadProgress() == doctest::Approx(0.f));
}

TEST_CASE("cylinder: arena.json refuses rounds on a kit without guns, and rounds past 30") {
    ArenaTable t;
    std::string err;
    CHECK_FALSE(importArenaTable(R"({ "fighters": [ { "id": "default", "rounds": 6 } ] })", t, &err));
    CHECK_MESSAGE(err.find("only a guns kit has a cylinder") != std::string::npos, err);
    CHECK_FALSE(importArenaTable(R"({ "fighters": [ { "id": "default" }, { "id": "duy", "weapon": "guns",
        "shot": { "range": 30, "width": 0.3 }, "rounds": 40 } ] })", t, &err));
    CHECK_MESSAGE(err.find("rounds") != std::string::npos, err);
}
