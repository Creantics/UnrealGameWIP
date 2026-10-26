#pragma once

#include "ftcore/DefenceTimeline.h"
#include "ftcore/WeakPoint.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class Team { Party, Enemies };

struct Stats {
    int maxHp = 100;
    int attack = 10;
    int defense = 5;
    int speed = 10;
    // Combo skill. A higher technique than your opponent makes your own combos (attacking
    // or defending) easier; an opponent with higher technique makes them harder.
    int technique = 10;
};

enum class Targeting { SingleEnemy, AllEnemies };

// A move. Party skills are performed as an arrow combo by the attacker (accuracy scales damage
// dealt). Enemy skills play their attack timeline instead, and the defender answers each hit with
// a dodge, parry or jump (see DefenceTimeline.h); notes and difficulty only matter for party skills.
struct Skill {
    std::string name;
    int apCost = 0;
    int apGain = 0;
    Targeting targeting = Targeting::SingleEnemy;
    float power = 1.0f;       // damage multiplier for the whole move at 100% accuracy
    int notes = 4;            // base combo length before difficulty adds more
    float difficulty = 2.0f;  // move complexity on the 0-10 combo rating scale
    AttackTimeline timeline;  // enemy skills: the hits to defend (default: one hit, kDefaultImpact s in)
};

inline constexpr std::size_t kMaxEnemyPhases = 4;
inline constexpr std::size_t kMaxPatternSteps = 8;
inline constexpr std::size_t kPhaseLabelCapacity = 24; // bytes, terminator included
inline constexpr int kMaxPhaseAttackPercent = 300;

// One stage of an enemy's fight. The first phase holds from the start; a later one begins when the
// enemy's HP falls to `hpBelow` of its max HP (thresholds fall from phase to phase). While a phase
// holds, the enemy's turns follow its pattern: pattern[0..length) in order, then again from
// loopFrom, so the steps before loopFrom are an opener played once. Entering the phase starts its
// pattern from the top, sets the enemy's attack to its battle-start attack plus attackPercent, and
// heals healPercent of its max HP. A pattern of length 0 cycles through every skill in order.
struct EnemyPhase {
    std::array<char, kPhaseLabelCapacity> labelChars{}; // fixed buffer: copying a combatant never allocates
    float hpBelow = 1.0f;
    std::array<std::uint8_t, kMaxPatternSteps> pattern{}; // skill indices
    std::uint8_t length = 0;
    std::uint8_t loopFrom = 0;
    int attackPercent = 0;
    int healPercent = 0;

    std::string_view label() const { return std::string_view(labelChars.data()); }
    // Copies at most kPhaseLabelCapacity - 1 bytes.
    void setLabel(std::string_view label) {
        labelChars.fill('\0');
        const std::size_t n = label.size() < kPhaseLabelCapacity ? label.size() : kPhaseLabelCapacity - 1;
        for (std::size_t i = 0; i < n; ++i) labelChars[i] = label[i];
    }
    // Appends a step; false (and nothing added) when full.
    bool addStep(std::size_t skill) {
        if (length >= kMaxPatternSteps) return false;
        pattern[length++] = static_cast<std::uint8_t>(skill);
        return true;
    }
    // The skill of the n-th turn taken in this phase (n from 0) for an enemy with `skillCount` skills.
    std::size_t step(std::size_t n, std::size_t skillCount) const {
        if (length == 0) return skillCount == 0 ? 0 : n % skillCount;
        if (n < length) return pattern[n];
        const std::size_t loop = loopFrom < length ? length - loopFrom : length;
        return pattern[length - loop + (n - length) % loop];
    }
};

// An enemy's phases (fixed capacity). Empty: one phase that cycles through every skill.
struct EnemyPhases {
    std::array<EnemyPhase, kMaxEnemyPhases> items{};
    std::uint8_t count = 0;

    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const EnemyPhase& operator[](std::size_t i) const { return items[i]; }
    void clear() { count = 0; }
    // Appends a phase; false (and nothing added) when full.
    bool add(const EnemyPhase& p) {
        if (count >= kMaxEnemyPhases) return false;
        items[count++] = p;
        return true;
    }
};

struct Combatant {
    std::string name;
    Team team = Team::Party;
    Stats stats;
    int hp = 100;
    int ap = 0;
    int maxAp = 9;
    std::vector<Skill> skills;
    WeakPoints weakPoints; // enemies: what aim mode reveals (see WeakPoint.h); none by default
    EnemyPhases phases;    // enemies: the stages of its fight and the skill pattern of each; none by default

    bool alive() const { return hp > 0; }
};

} // namespace ftcore
