#pragma once

// The line-up of the battle demo (two party members against two enemies) and a simple pilot that
// plays a battle on its own: it picks a skill and target for each party turn and steps on every
// note exactly on time. The pilot drives the headless test of the demo fight and the demo's
// autoplay toggle. Header-only, so there is no extra translation unit to wrap for Unreal.

#include "ftcore/Battle.h"
#include "ftcore/CombatTypes.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ftcore::demo {

inline Skill makeSkill(std::string name, int apCost, int apGain, Targeting targeting, float power, int notes,
                       float difficulty) {
    Skill s;
    s.name = std::move(name);
    s.apCost = apCost;
    s.apGain = apGain;
    s.targeting = targeting;
    s.power = power;
    s.notes = notes;
    s.difficulty = difficulty;
    return s;
}

inline Combatant makeCombatant(std::string name, Stats stats, std::vector<Skill> skills) {
    Combatant c;
    c.name = std::move(name);
    c.stats = stats;
    c.hp = stats.maxHp;
    c.skills = std::move(skills);
    return c;
}

// Stats are {maxHp, attack, defense, speed, technique}.
inline std::vector<Combatant> party() {
    const Skill strike = makeSkill("Strike", 0, 1, Targeting::SingleEnemy, 1.0f, 4, 1.0f);
    return {
        makeCombatant("Devin", {140, 24, 8, 12, 12},
                      {strike, makeSkill("Twin Cut", 2, 0, Targeting::SingleEnemy, 2.0f, 5, 3.0f),
                       makeSkill("Sweep", 3, 0, Targeting::AllEnemies, 1.6f, 6, 4.0f),
                       makeSkill("Heavy Blow", 5, 0, Targeting::SingleEnemy, 4.0f, 8, 6.0f)}),
        makeCombatant("Duy", {110, 20, 4, 15, 15},
                      {strike, makeSkill("Spark", 1, 0, Targeting::SingleEnemy, 1.5f, 4, 2.0f),
                       makeSkill("Pierce", 2, 0, Targeting::SingleEnemy, 2.2f, 6, 4.5f),
                       makeSkill("Flare", 4, 0, Targeting::AllEnemies, 2.4f, 7, 5.0f)}),
    };
}

inline std::vector<Combatant> enemies() {
    return {
        makeCombatant("Brute", {260, 18, 6, 8, 8},
                      {makeSkill("Slam", 0, 0, Targeting::SingleEnemy, 1.3f, 3, 1.0f),
                       makeSkill("Double Swipe", 0, 0, Targeting::SingleEnemy, 1.6f, 5, 2.5f)}),
        makeCombatant("Shade", {180, 15, 3, 14, 14},
                      {makeSkill("Flicker", 0, 0, Targeting::SingleEnemy, 1.6f, 5, 4.0f),
                       makeSkill("Feint", 0, 0, Targeting::SingleEnemy, 1.8f, 5, 5.0f)}),
    };
}

inline constexpr double kBpm = 120.0;

// The demo's rules. Music sync is on at kBpm (the Unreal side takes bpm and offset from its conductor).
inline BattleTuning tuning() {
    BattleTuning t;
    t.leadIn = 1.0;
    t.windup = 1.2;
    t.recovery = 0.8;
    t.music.bpm = kBpm;
    t.music.offset = 0.0;
    return t;
}

struct PilotChoice {
    std::size_t skill = 0;
    CombatantId target = 0;
};

// For the acting party member: the strongest skill it can afford, on the living enemy with the
// least HP (ties: lowest id). Only meaningful in Phase::AwaitingCommand.
inline PilotChoice pilotChoice(const Battle& b) {
    PilotChoice out;
    const Combatant& me = b.combatant(b.actor());
    float best = -1.0f;
    for (std::size_t i = 0; i < me.skills.size(); ++i) {
        const Skill& s = me.skills[i];
        if (s.apCost > me.ap) continue;
        const float value = s.power * (s.targeting == Targeting::AllEnemies ? 1.5f : 1.0f);
        if (value > best) {
            best = value;
            out.skill = i;
        }
    }
    bool found = false;
    const std::vector<Combatant>& all = b.combatants();
    for (CombatantId id = 0; id < all.size(); ++id) {
        if (all[id].team != Team::Enemies || !all[id].alive()) continue;
        if (!found || all[id].hp < all[out.target].hp) out.target = id;
        found = true;
    }
    return out;
}

// Presses every pending note of the active combo that is due by `now`, exactly at its time.
// Call before Battle::update(now). Returns the number of presses.
inline int pilotPress(Battle& b, double now) {
    int presses = 0;
    if (b.phase() != Phase::PartySkill && b.phase() != Phase::EnemySkill) return presses;
    const ComboSequence& c = b.combo();
    for (std::size_t i = c.nextIndex(); i < c.size() && c.note(i).time <= now; ++i) {
        if (c.grade(i) != Grade::Pending) continue;
        b.press(c.note(i).arrow, c.note(i).time);
        ++presses;
        if (b.phase() != Phase::PartySkill && b.phase() != Phase::EnemySkill) break;
    }
    return presses;
}

} // namespace ftcore::demo
