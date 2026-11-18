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
