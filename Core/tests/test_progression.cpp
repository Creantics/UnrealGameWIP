#include "ftcore/Encounter.h"
#include "ftcore/Progression.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

TEST_CASE("the level curve rises, starts at 0 and clamps") {
    CHECK(xpForLevel(1) == 0);
    CHECK(xpForLevel(0) == 0);
    CHECK(xpForLevel(-5) == 0);
    CHECK(xpForLevel(kMaxLevel + 3) == xpForLevel(kMaxLevel));
    for (int level = 1; level < kMaxLevel; ++level) {
        CAPTURE(level);
        CHECK(xpForLevel(level + 1) > xpForLevel(level));
        // Each level costs at least as much as the one before.
        if (level > 1) CHECK(xpForLevel(level + 1) - xpForLevel(level) >= xpForLevel(level) - xpForLevel(level - 1));
        CHECK(levelForXp(xpForLevel(level)) == level);
        CHECK(levelForXp(xpForLevel(level + 1) - 1) == level);
        CHECK(xpToNextLevel(xpForLevel(level)) == xpForLevel(level + 1) - xpForLevel(level));
    }
    CHECK(levelForXp(-10) == 1);
    CHECK(levelForXp(kMaxXp) == kMaxLevel);
    CHECK(xpToNextLevel(xpForLevel(kMaxLevel)) == 0);
    CHECK(xpToNextLevel(kMaxXp) == 0);
}

TEST_CASE("award adds xp and gold, reports level ups and clamps") {
    PartyProgress p;
    AwardResult r = award(p, {xpForLevel(2) - 1, 7});
    CHECK(r.levelBefore == 1);
    CHECK(r.levelAfter == 1);
    CHECK(p.gold == 7);
    r = award(p, {1, 0});
    CHECK(r.levelAfter == 2);
    // One big reward can jump several levels.
    r = award(p, {xpForLevel(5) - p.xp, 0});
    CHECK(r.levelBefore == 2);
    CHECK(r.levelAfter == 5);
    // Negative parts count as nothing.
    const PartyProgress before = p;
    r = award(p, {-100, -100});
    CHECK(p.xp == before.xp);
    CHECK(p.gold == before.gold);
    CHECK(r.levelBefore == r.levelAfter);
    // Caps.
    award(p, {kMaxXp, kMaxGold});
    award(p, {2000000000, 2000000000});
    CHECK(p.xp == kMaxXp);
    CHECK(p.gold == kMaxGold);
    CHECK(levelForXp(p.xp) == kMaxLevel);
}

TEST_CASE("stats grow with level from the level 1 values; speed stays") {
    const Stats base{140, 24, 8, 12, 12};
    const Stats one = statsAtLevel(base, 1);
    CHECK(one.maxHp == base.maxHp);
    CHECK(one.attack == base.attack);
    CHECK(one.defense == base.defense);
    CHECK(one.technique == base.technique);
    CHECK(statsAtLevel(base, 0).maxHp == base.maxHp); // clamped up to 1
    Stats prev = one;
    for (int level = 2; level <= kMaxLevel; ++level) {
        CAPTURE(level);
        const Stats s = statsAtLevel(base, level);
        CHECK(s.maxHp > prev.maxHp);
        CHECK(s.attack >= prev.attack);
        CHECK(s.defense >= prev.defense);
        CHECK(s.technique >= prev.technique);
        CHECK(s.speed == base.speed);
        prev = s;
    }
    const Stats top = statsAtLevel(base, kMaxLevel);
    CHECK(top.maxHp == base.maxHp + base.maxHp * (kMaxLevel - 1) * kLevelHpPercent / 100);
    CHECK(top.technique == base.technique + (kMaxLevel - 1) / kTechniqueLevels);
    CHECK(statsAtLevel(base, kMaxLevel + 5).maxHp == top.maxHp); // clamped down
}

TEST_CASE("an encounter pays its enemies' rewards, raised by tier") {
    EnemyRoster roster;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importEnemyRoster(R"J({"enemies": [
        { "id": "a", "stats": {}, "reward": { "xp": 20, "gold": 10 }, "skills": [ { "id": "s" } ] },
        { "id": "b", "stats": {}, "reward": { "xp": 5 }, "skills": [ { "id": "s" } ] },
        { "id": "c", "stats": {}, "skills": [ { "id": "s" } ] } ]})J",
                                    roster, &err, &warnings),
                    err);
    CHECK(warnings.empty());
    CHECK(roster.enemies[1].reward.gold == 0); // absent parts are 0
    CHECK(roster.enemies[2].reward.xp == 0);

    EncounterLineup lineup;
    lineup.enemies = {0, 1, 2, 0};
    Reward r = encounterReward(roster, lineup);
    CHECK(r.xp == 45);
    CHECK(r.gold == 20);
    lineup.tier = 5;
    r = encounterReward(roster, lineup);
    CHECK(r.xp == 45 + 45 * 5 * kTierRewardPercent / 100);
    CHECK(r.gold == 20 + 20 * 5 * kTierRewardPercent / 100);
    lineup.enemies.push_back(99); // a bad index pays nothing
    CHECK(encounterReward(roster, lineup).xp == r.xp);

    if (ftcore_test::allocHookInstalled()) {
        const auto before = ftcore_test::allocStats().count;
        PartyProgress p;
        int sum = 0;
        for (int i = 0; i < 50; ++i) {
            const AwardResult a = award(p, encounterReward(roster, lineup));
            sum += a.levelAfter + statsAtLevel(Stats{}, a.levelAfter).maxHp + xpToNextLevel(p.xp);
        }
        CHECK(ftcore_test::allocStats().count == before);
        CHECK(sum > 0);
    }
}

TEST_CASE("bad rewards are refused with a reason") {
    const char* bad[] = {
        R"J({"enemies": [ { "id": "a", "stats": {}, "reward": 3, "skills": [ { "id": "s" } ] } ]})J",
        R"J({"enemies": [ { "id": "a", "stats": {}, "reward": { "xp": -1 }, "skills": [ { "id": "s" } ] } ]})J",
        R"J({"enemies": [ { "id": "a", "stats": {}, "reward": { "gold": 1.5 }, "skills": [ { "id": "s" } ] } ]})J",
        R"J({"enemies": [ { "id": "a", "stats": {}, "reward": { "xp": 100001 }, "skills": [ { "id": "s" } ] } ]})J",
    };
    for (const char* text : bad) {
        INFO(text);
        EnemyRoster roster;
        std::string err;
        CHECK_FALSE(importEnemyRoster(text, roster, &err));
        CHECK(err.find("reward") != std::string::npos);
    }
    EnemyRoster roster;
    std::vector<std::string> warnings;
    REQUIRE(importEnemyRoster(R"J({"enemies": [ { "id": "a", "stats": {}, "reward": { "xp": 1, "silver": 2 }, "skills": [ { "id": "s" } ] } ]})J",
                              roster, nullptr, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("silver") != std::string::npos);
}
