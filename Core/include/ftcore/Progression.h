#pragma once

// Party progression: experience, gold and the party level that experience buys. Winning a story
// fight awards the line-up's reward (each enemy's xp and gold in Data/Battle/enemies.json, raised by
// the line-up's tier); the level follows from total xp on a fixed curve, and each level above 1
// raises the party's max HP, attack, defense and technique over their level 1 values.
//
//   ftcore::PartyProgress progress;                                  // kept in the save
//   const ftcore::AwardResult r = ftcore::award(progress, ftcore::encounterReward(roster, lineup));
//   if (r.levelAfter > r.levelBefore) ...;                           // a level up
//   Stats s = ftcore::statsAtLevel(baseStats, ftcore::levelForXp(progress.xp));
//
// The whole party shares one level: its members always fight together. The curve is set so a
// player who wins every story fight in script order gains a level about every four fights and
// reaches kMaxLevel near the finale (a test walks the encounter table to keep it so). Nothing
// here allocates.

#include "ftcore/Api.h"
#include "ftcore/CombatTypes.h"

namespace ftcore {

struct EnemyRoster;
struct EncounterLineup;

inline constexpr int kMaxLevel = 10;

// Percent of a member's level 1 stat that each level above 1 adds. Speed doesn't grow (turn order
// stays as designed); technique grows by one every kTechniqueLevels levels.
inline constexpr int kLevelHpPercent = 8;
inline constexpr int kLevelAttackPercent = 6;
inline constexpr int kLevelDefensePercent = 6;
inline constexpr int kTechniqueLevels = 2;

// Percent of a line-up's base reward that each tier adds (later fights pay a little more).
inline constexpr int kTierRewardPercent = 10;

// Caps so sums never overflow.
inline constexpr int kMaxXp = 1000000;
inline constexpr int kMaxGold = 9999999;

struct PartyProgress {
    int xp = 0;   // total experience, 0..kMaxXp
    int gold = 0; // 0..kMaxGold; nothing spends it yet
};

struct Reward {
    int xp = 0;
    int gold = 0;
};

struct AwardResult {
    int levelBefore = 1;
    int levelAfter = 1;
};

// Total xp needed to reach `level` (1 needs 0). Levels outside 1..kMaxLevel are clamped.
FTCORE_API int xpForLevel(int level);
// The level `xp` total experience reaches, 1..kMaxLevel.
FTCORE_API int levelForXp(int xp);
// Experience still needed for the next level, or 0 at kMaxLevel.
FTCORE_API int xpToNextLevel(int xp);

// The sum of the line-up's enemy rewards, raised by kTierRewardPercent per tier.
FTCORE_API Reward encounterReward(const EnemyRoster& roster, const EncounterLineup& lineup);
// Adds `reward` (negative parts count as 0) to `progress`, clamped to the caps.
FTCORE_API AwardResult award(PartyProgress& progress, const Reward& reward);

// A member's stats at `level` (clamped to 1..kMaxLevel) from their level 1 stats.
FTCORE_API Stats statsAtLevel(const Stats& base, int level);

} // namespace ftcore
