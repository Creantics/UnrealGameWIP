#pragma once

#include "ftcore/DefenceTimeline.h"
#include "ftcore/WeakPoint.h"

#include <string>
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

struct Combatant {
    std::string name;
    Team team = Team::Party;
    Stats stats;
    int hp = 100;
    int ap = 0;
    int maxAp = 9;
    std::vector<Skill> skills;
    WeakPoints weakPoints; // enemies: what aim mode reveals (see WeakPoint.h); none by default

    bool alive() const { return hp > 0; }
};

} // namespace ftcore
