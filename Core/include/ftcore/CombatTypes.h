#pragma once

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

// A move. Performing it means playing an arrow combo: party skills are performed by the
// attacker (accuracy scales damage dealt), enemy skills by the defender (accuracy scales
// damage blocked).
struct Skill {
    std::string name;
    int apCost = 0;
    int apGain = 0;
    Targeting targeting = Targeting::SingleEnemy;
    float power = 1.0f;       // damage multiplier for the whole move at 100% accuracy
    int notes = 4;            // base combo length before difficulty adds more
    float difficulty = 2.0f;  // move complexity on the 0-10 combo rating scale
};

struct Combatant {
    std::string name;
    Team team = Team::Party;
    Stats stats;
    int hp = 100;
    int ap = 0;
    int maxAp = 9;
    std::vector<Skill> skills;

    bool alive() const { return hp > 0; }
};

} // namespace ftcore
