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
//   notes, difficulty. Stats keys default to the Stats defaults. `name` defaults to the id.
// encounters.json:
//   { "version": 1, "default": { "enemies": ["brute", "shade"] },
//     "encounters": [ { "scene": "SC-X", "beat": 4, "step": 0, "enemies": ["shade"] } ] }
//   `step` defaults to 0. An unknown enemy id, a duplicate (scene, beat, step) or an empty line-up
//   is an error; unknown keys are warnings.

#include "ftcore/Api.h"
#include "ftcore/CombatTypes.h"

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
};

struct EnemyRoster {
    std::vector<EnemyDef> enemies;
    // Index of the enemy with that id, or -1. No allocation.
    FTCORE_API int find(std::string_view id) const;
};

struct EncounterLineup {
    std::vector<std::uint32_t> enemies; // indices into the roster, in line-up order
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

// The line-up as battle combatants (Team::Enemies, full HP). Replaces `out`. Allocates.
FTCORE_API void makeEnemies(const EnemyRoster& roster, const EncounterLineup& lineup, std::vector<Combatant>& out);

} // namespace ftcore
