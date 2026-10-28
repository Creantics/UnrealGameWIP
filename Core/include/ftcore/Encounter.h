#pragma once

// Which enemies a story fight puts up: Data/Battle/enemies.json (the enemy roster: ids, stats,
// skills) and Data/Battle/encounters.json (scene id + gameplay beat index + plan step index -> a
// line-up of enemy ids, with a default line-up for every fight not listed).
//
//   ftcore::EnemyRoster roster;
//   ftcore::EncounterTable table;
//   std::string error;
//   if (!ftcore::importEnemyRoster(enemiesText, roster, &error)) ...;
//   if (!ftcore::importEncounterTable(encountersText, roster, table, &error)) ...;
//   const ftcore::EncounterLineup& lineup = table.find(sceneId, beat, step); // the default when not listed
//   std::vector<ftcore::Combatant> enemies;
//   ftcore::makeEnemies(roster, lineup, enemies);                             // allocates: at battle set-up
//
// Both files carry ids and numbers only: no story text. Loading allocates; find() does not.
//
// enemies.json:
//   { "version": 1, "enemies": [
//       { "id": "brute", "name": "Brute", "stats": { "maxHp": 260, "attack": 18, "defense": 6, "speed": 8, "technique": 8 },
//         "skills": [ { "id": "slam", "name": "Slam", "power": 1.3, "notes": 3, "difficulty": 1.0 } ] } ] }
//   Skill keys: id, name, apCost, apGain (default 0), targeting ("single" default, or "all"), power,
//   notes, difficulty, hits. Stats keys default to the Stats defaults. `name` defaults to the id.
//   `reward` (optional, each part 0..100000, default 0) is what beating the enemy pays:
//     "reward": { "xp": 20, "gold": 12, "items": [ { "id": "bandage_roll", "count": 1 } ] }
//   `items` (optional) are the item drops: at most kMaxEnemyDrops entries, each an `id` (see
//   validItemId; a test checks that Data/Battle/items.json has it) and a `count` (1..kMaxItemStack,
//   default 1), and an id may be listed once.
//   `hits` is the attack timeline the defender answers on an enemy turn (see DefenceTimeline.h):
//     "hits": [ { "at": 0.9, "tell": 0.4 }, { "at": 1.5, "tell": 0.4, "weight": 2, "jumpable": true } ]
//   1..8 hits, `at` seconds after the banner (0 < at <= 30, strictly increasing, required); `tell`
//   (0..2, default 0.4) and `telegraph` (0..10, default 0) seconds before impact, neither longer
//   than `at`; `weight` (> 0, default 1) is the hit's share of the skill's damage; `jumpable`
//   (default false). A skill without `hits` gets one default hit 1.2 s after its banner.
//   `phases` (optional, 1..kMaxEnemyPhases) are the stages of the enemy's fight and the skill
//   pattern of each (see EnemyPhase in CombatTypes.h); without it the enemy cycles its skills:
//     "phases": [ { "pattern": ["hammer", "decree"] },
//                 { "below": 0.6, "label": "Overdrive", "pattern": ["rally", "hammer"], "attack": 10 },
//                 { "below": 0.25, "label": "Last Stand", "pattern": ["verdict", "rally", "hammer"], "loop": 1,
//                   "heal": 10 } ]
//   `pattern` is 1..kMaxPatternSteps skill ids (required); `loop` (0..length-1, default 0) is where
//   the pattern starts again after its last step. `below` (a fraction of max HP, 0 < below < 1) is
//   required on every phase but the first, which has none, and falls from phase to phase. `label`
//   (up to kPhaseLabelCapacity - 1 bytes, default empty) is the banner when the phase begins.
//   `attack` (0..kMaxPhaseAttackPercent) is the percent added to the enemy's battle-start attack
//   while the phase holds; `heal` (0..100) the percent of max HP it heals on entering. Both default 0.
// encounters.json:
//   { "version": 1, "default": { "enemies": ["brute", "shade"] },
//     "encounters": [ { "scene": "SC-X", "beat": 4, "step": 0, "enemies": ["shade"] } ] }
//   `step` defaults to 0. `tier` (0..9, default 0; "default" takes one too) toughens the line-up:
//   each tier adds kTierHpPercent of the base max HP and kTierAttackPercent of the base attack to
//   every enemy in it (see
//   makeEnemies). An unknown enemy id, a duplicate (scene, beat, step) or an empty line-up is an
//   error; unknown keys are warnings.

#include "ftcore/Api.h"
#include "ftcore/CombatTypes.h"
#include "ftcore/Progression.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct EnemyDef {
    std::string id;
    std::string name;
    Stats stats;
    std::vector<std::string> skillIds; // parallel to skills
    std::vector<Skill> skills;
    Reward reward; // what beating it pays (see Progression.h)
    EnemyPhases phases;
};

struct EnemyRoster {
    std::vector<EnemyDef> enemies;
    // Index of the enemy with that id, or -1. No allocation.
    FTCORE_API int find(std::string_view id) const;
};

// Percent of an enemy's base max HP and of its base attack that each tier adds. Tiers keep pace
// with the party's levels (see Progression.h): a typical player ends a late fight with about half
// their HP, and a novice wins the fights up to tier 5.
inline constexpr int kTierHpPercent = 10;
inline constexpr int kTierAttackPercent = 11;
inline constexpr int kMaxTier = 9;

struct EncounterLineup {
    std::vector<std::uint32_t> enemies; // indices into the roster, in line-up order
    int tier = 0;                       // 0..kMaxTier; later fights are a bit tougher
};

struct EncounterEntry {
    std::string scene;
    std::uint32_t beat = 0, step = 0;
    EncounterLineup lineup;
};

struct EncounterTable {
    EncounterLineup fallback; // the "default" line-up
    std::vector<EncounterEntry> entries;

    // The entry for (scene, beat, step), or nullptr. No allocation.
    FTCORE_API const EncounterEntry* findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
    // The line-up for (scene, beat, step): the entry's, else the default. No allocation.
    FTCORE_API const EncounterLineup& find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importEnemyRoster(std::string_view json, EnemyRoster& out, std::string* error = nullptr,
                                  std::vector<std::string>* warnings = nullptr);
// Enemy ids are resolved against `roster`.
FTCORE_API bool importEncounterTable(std::string_view json, const EnemyRoster& roster, EncounterTable& out,
                                     std::string* error = nullptr, std::vector<std::string>* warnings = nullptr);

// The line-up as battle combatants (Team::Enemies, full HP, the line-up's tier applied to max HP and
// attack). Replaces `out`. Allocates.
FTCORE_API void makeEnemies(const EnemyRoster& roster, const EncounterLineup& lineup, std::vector<Combatant>& out);

} // namespace ftcore
