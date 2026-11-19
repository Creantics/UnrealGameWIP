// The special prompt's row gating: the row is reserved only under the world's switch AND in a fight that has specials
// (ArenaFight::specialsOn). The host (FTNewUi) gates both the row's step and the prompt stack on uiSpecialRowReserved.
#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaSpecial.h"
#include "ftcore/UiSpecial.h"

#include "TestAssets.h"

#include <doctest/doctest.h>

#include <initializer_list>
#include <string>

using namespace ftcore;

namespace {

const ArenaTable& arenaTable() {
    static const ArenaTable t = [] {
        std::string text;
        ArenaTable a;
        if (!ftcore_test::readProjectData("Battle/arena.json", text)) return a;
        std::string err;
        importArenaTable(text, a, &err);
        return a;
    }();
    return t;
}

ArenaSpecialTable shippedSpecialTable() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena_special.json", text)) FAIL("Data/Battle/arena_special.json not found");
    ArenaSpecialTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaSpecials(text, t, &err), err);
    return t;
}

void beginFight(ArenaFight& fight, const char* enemy) {
    const ArenaTable& table = arenaTable();
    REQUIRE_FALSE(table.enemyIds.empty());
    ArenaDef def;
    def.fighter = table.fighter("devin");
    def.enemyCount = 0;
    for (std::size_t e = 0; e < table.enemyIds.size(); ++e)
        if (table.enemyIds[e] == enemy) {
            def.enemyKind[0] = static_cast<std::uint8_t>(e);
            def.enemies[0] = table.enemies[e];
            def.enemyCount = 1;
        }
    REQUIRE(def.enemyCount == 1);
    def.attackers = 1;
    def.ringRadius = 30.f;
    def.spawnDistance = 7.f;
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 60.f});
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
}

} // namespace

TEST_CASE("special row gating: reserved only under the world's switch and in a fight with specials") {
    static_assert(uiSpecialRowReserved(true, true));
    static_assert(!uiSpecialRowReserved(true, false));
    static_assert(!uiSpecialRowReserved(false, true));
    static_assert(!uiSpecialRowReserved(false, false));
    // Not reserved: the stack is exactly as it was, whichever half is missing.
    for (const bool worldSwitch : {false, true})
        for (const bool specials : {false, true}) {
            const bool reserved = uiSpecialRowReserved(worldSwitch, specials);
            for (int p = 0; p < static_cast<int>(kUiPromptCount); ++p) {
                const auto pr = static_cast<UiPrompt>(p);
                if (!reserved) CHECK(uiPromptRowAt(pr, reserved) == uiPromptRow(pr));
                if (reserved && uiPromptRow(pr) >= kUiSpecialRow) CHECK(uiPromptRowAt(pr, reserved) == uiPromptRow(pr) + 1);
            }
            // The row's own step follows the same gate: no row, nothing drawn, without both.
            UiSpecialState s;
            const UiSpecialView v = uiSpecialStep(s, reserved, UiSpecialInput{true, UiSpecialWhy::None, false}, 0.016f, false);
            CHECK(v.row == reserved);
            CHECK(v.drawn == reserved);
        }
}

TEST_CASE("special row gating: specialsOn is off until a table is set, steady through the fight, cleared by begin") {
    const ArenaSpecialTable table = shippedSpecialTable();
    ArenaFight fight;
    CHECK_FALSE(fight.specialsOn());
    CHECK_FALSE(uiSpecialRowReserved(true, fight.specialsOn()));

    beginFight(fight, "hulk");
    CHECK_FALSE(fight.specialsOn()); // a fight with no table: the stack stays as it was
    REQUIRE(fight.setSpecials(&table, "devin", "", LeadSpecialGauges{}));
    CHECK(fight.specialsOn());
    CHECK(uiSpecialRowReserved(true, fight.specialsOn()));
    CHECK_FALSE(uiSpecialRowReserved(false, fight.specialsOn())); // the world's switch still rules

    // A table with no move for the lead in a slot still reserves the row: it is about the fight, not the lead, so a
    // lead switch never moves the stack.
    REQUIRE(fight.setSpecials(&table, "duy", "devin", LeadSpecialGauges{}));
    CHECK(fight.specialsOn());

    // The next fight begins with none until the host sets them again.
    beginFight(fight, "hulk");
    CHECK_FALSE(fight.specialsOn());
    REQUIRE(fight.setSpecialTable(&table));
    CHECK(fight.specialsOn());
    REQUIRE(fight.setSpecialTable(nullptr));
    CHECK_FALSE(fight.specialsOn());
    CHECK_FALSE(uiSpecialRowReserved(true, fight.specialsOn()));
}

TEST_CASE("special row gating: a lead with no move and no partner (Duy alone) holds the row but never draws it") {
    const ArenaSpecialTable table = shippedSpecialTable();
    REQUIRE_MESSAGE(table.forLead("duy") == nullptr, "the shipped table gave Duy a move: pick another lead for this case");
    ArenaFight fight;
    beginFight(fight, "hulk");
    // The host's store with Duy's gauge full: a full gauge with no move must still never light the row.
    LeadSpecialGauges gauges;
    LeadSpecialGauges::Lead* duy = gauges.entry("duy");
    REQUIRE(duy != nullptr);
    duy->value = 1000.f;
    REQUIRE(fight.setSpecials(&table, "duy", "", gauges));
    REQUIRE(fight.specialsOn());
    const bool reserved = uiSpecialRowReserved(true, fight.specialsOn());
    CHECK(reserved);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    UiSpecialState s;
    bool everFed = false, everRowMissing = false, everDrawn = false, everRunning = false, alwaysNoMove = true;
    const float dt = 1.f / 60.f;
    int ticks = 0;
    for (; ticks < 180 && !fight.over(); ++ticks) {
        fight.tick(dt, ArenaInput{}, bodies);
        arenaStep(fight, bodies, dt);
        const ArenaSpecialView& rules = fight.special();
        // As the host feeds it (FTNewUi TickSpecial). The rules' own ready flag is theirs (the gauge is full); the row
        // reads the feed only.
        const UiSpecialInput in = uiSpecialInputFrom(rules);
        const UiSpecialView v = uiSpecialStep(s, reserved, in, dt, false);
        everFed = everFed || in.ready || in.thirdReady;
        everRunning = everRunning || rules.running;
        alwaysNoMove = alwaysNoMove && (fight.over() || rules.refusalPlayer == ArenaSpecialRefusal::NoMove);
        everRowMissing = everRowMissing || !v.row;
        everDrawn = everDrawn || v.drawn;
    }
    CHECK(ticks > 60);
    CHECK(alwaysNoMove);
    CHECK_FALSE(everRunning);
    CHECK_FALSE(everFed);
    CHECK_FALSE(everRowMissing); // the row is held: a lead switch would never move the stack
    CHECK_FALSE(everDrawn);      // and nothing is drawn in it
}

TEST_CASE("special row feed: a full gauge with no move, or while a special runs, never shows; each refusal's reason") {
    ArenaSpecialView rules;
    rules.readyPlayer = true;
    rules.refusalPlayer = ArenaSpecialRefusal::Ok;
    UiSpecialInput in = uiSpecialInputFrom(rules);
    CHECK(in.ready);
    CHECK(in.why == UiSpecialWhy::None);
    CHECK_FALSE(in.thirdReady);
    // A full gauge whose lead has no move: nothing to press, nothing shown.
    rules.refusalPlayer = ArenaSpecialRefusal::NoMove;
    in = uiSpecialInputFrom(rules);
    CHECK_FALSE(in.ready);
    // Not full: nothing shown, whatever the refusal.
    rules.readyPlayer = false;
    rules.refusalPlayer = ArenaSpecialRefusal::NotReady;
    CHECK_FALSE(uiSpecialInputFrom(rules).ready);
    // Each refusal of a full gauge with a move: the reason (NotReady can't come with a full gauge; it reads as none).
    rules.readyPlayer = true;
    const struct {
        ArenaSpecialRefusal r;
        UiSpecialWhy why;
    } reasons[] = {{ArenaSpecialRefusal::Ok, UiSpecialWhy::None},          {ArenaSpecialRefusal::NotReady, UiSpecialWhy::None},
                   {ArenaSpecialRefusal::NoTarget, UiSpecialWhy::NoTarget}, {ArenaSpecialRefusal::NoSight, UiSpecialWhy::OutOfSight},
                   {ArenaSpecialRefusal::Down, UiSpecialWhy::NotNow},       {ArenaSpecialRefusal::NotControlled, UiSpecialWhy::NotNow},
                   {ArenaSpecialRefusal::Unhurt, UiSpecialWhy::NotNow}};
    for (const auto& e : reasons) {
        CAPTURE(arenaSpecialRefusalName(e.r));
        rules.refusalPlayer = e.r;
        in = uiSpecialInputFrom(rules);
        CHECK(in.ready);
        CHECK(in.why == e.why);
    }
    // While a special runs nothing is ready, the lead's or the Third's.
    rules.refusalPlayer = ArenaSpecialRefusal::Running;
    rules.readyPartner = true;
    rules.refusalPartner = ArenaSpecialRefusal::Running;
    rules.running = true;
    in = uiSpecialInputFrom(rules);
    CHECK_FALSE(in.ready);
    CHECK_FALSE(in.thirdReady);
    // The Third's: a full gauge with a move calls; with no move, never.
    rules.running = false;
    rules.readyPlayer = false;
    rules.refusalPlayer = ArenaSpecialRefusal::NotReady;
    rules.refusalPartner = ArenaSpecialRefusal::NoTarget;
    in = uiSpecialInputFrom(rules);
    CHECK_FALSE(in.ready);
    CHECK(in.thirdReady);
    rules.refusalPartner = ArenaSpecialRefusal::NoMove;
    CHECK_FALSE(uiSpecialInputFrom(rules).thirdReady);
    // Through the row: the CALL row stands for the Third's alone, and a moveless lead's full gauge draws nothing.
    UiSpecialState s;
    rules.refusalPartner = ArenaSpecialRefusal::Ok;
    UiSpecialView v = uiSpecialStep(s, true, uiSpecialInputFrom(rules), 0.016f, true);
    CHECK(v.drawn);
    CHECK(v.call);
    UiSpecialState t;
    rules.readyPartner = false;
    rules.readyPlayer = true;
    rules.refusalPlayer = ArenaSpecialRefusal::NoMove;
    v = uiSpecialStep(t, true, uiSpecialInputFrom(rules), 0.016f, true);
    CHECK(v.row);
    CHECK_FALSE(v.drawn);
}
