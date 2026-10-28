#include "ftcore/Progression.h"

#include "ftcore/Encounter.h"

namespace ftcore {

namespace {

// Total xp to reach each level (index = level). Set against the story's encounter order: about
// four fights per level, the cap a few fights before the end.
constexpr int kXpToReach[kMaxLevel + 1] = {0, 0, 70, 160, 280, 420, 580, 750, 940, 1150, 1450};

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

int xpForLevel(int level) { return kXpToReach[clampInt(level, 1, kMaxLevel)]; }

int levelForXp(int xp) {
    int level = 1;
    while (level < kMaxLevel && xp >= kXpToReach[level + 1]) ++level;
    return level;
}

int xpToNextLevel(int xp) {
    const int level = levelForXp(xp);
    return level >= kMaxLevel ? 0 : kXpToReach[level + 1] - xp;
}

Reward encounterReward(const EnemyRoster& roster, const EncounterLineup& lineup) {
    Reward r;
    for (const std::uint32_t index : lineup.enemies) {
        if (index >= roster.enemies.size()) continue;
        r.xp += roster.enemies[index].reward.xp;
        r.gold += roster.enemies[index].reward.gold;
        for (const ItemStack& drop : roster.enemies[index].reward.items) r.items.add(drop.id.view(), drop.count);
    }
    const int tier = clampInt(lineup.tier, 0, kMaxTier);
    r.xp += r.xp * tier * kTierRewardPercent / 100;
    r.gold += r.gold * tier * kTierRewardPercent / 100;
    return r;
}

AwardResult award(PartyProgress& progress, const Reward& reward) {
    AwardResult r;
    r.levelBefore = levelForXp(progress.xp);
    const int xp = reward.xp > 0 ? reward.xp : 0, gold = reward.gold > 0 ? reward.gold : 0;
    progress.xp = progress.xp > kMaxXp - xp ? kMaxXp : clampInt(progress.xp + xp, 0, kMaxXp);
    progress.gold = progress.gold > kMaxGold - gold ? kMaxGold : clampInt(progress.gold + gold, 0, kMaxGold);
    for (const ItemStack& drop : reward.items) r.itemsLost += drop.count - progress.items.add(drop.id.view(), drop.count);
    r.levelAfter = levelForXp(progress.xp);
    return r;
}

Stats statsAtLevel(const Stats& base, int level) {
    const int up = clampInt(level, 1, kMaxLevel) - 1;
    Stats s = base;
    s.maxHp += base.maxHp * up * kLevelHpPercent / 100;
    s.attack += base.attack * up * kLevelAttackPercent / 100;
    s.defense += base.defense * up * kLevelDefensePercent / 100;
    s.technique += up / kTechniqueLevels;
    return s;
}

} // namespace ftcore
