#pragma once

// The line-up of the battle demo (two party members against two enemies) and a simple pilot that
// plays a battle on its own: it picks a skill and target for each party turn, steps on every
// note exactly on time and answers every enemy hit at its ideal time (read from the attack
// timeline, never from audio). The pilot drives the headless test of the demo fight and the demo's
// autoplay toggle. Header-only, so there is no extra translation unit to wrap for Unreal.

#include "ftcore/Battle.h"
#include "ftcore/CombatTypes.h"

#include <cstddef>
#include <initializer_list>
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

// An attack timeline from {at, tell, telegraph, weight, jumpable} hits.
inline AttackTimeline makeTimeline(std::initializer_list<TimelineHit> hits) {
    AttackTimeline t;
    t.clear();
    for (const TimelineHit& h : hits) t.add(h);
    return t;
}

inline Skill withTimeline(Skill s, AttackTimeline t) {
    s.timeline = t;
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

// Same as the brute and shade in Data/Battle/enemies.json (a test keeps them in step).
inline std::vector<Combatant> enemies() {
    return {
        makeCombatant("Brute", {260, 18, 6, 8, 8},
                      {withTimeline(makeSkill("Slam", 0, 0, Targeting::SingleEnemy, 1.3f, 3, 1.0f),
                                    makeTimeline({{1.6, 0.4, 1.2, 1.0f, false}})),
                       withTimeline(makeSkill("Double Swipe", 0, 0, Targeting::SingleEnemy, 1.6f, 5, 2.5f),
                                    makeTimeline({{0.9, 0.4, 0.0, 1.0f, false}, {1.4, 0.4, 0.0, 1.0f, false}}))}),
        makeCombatant("Shade", {180, 15, 3, 14, 14},
                      {withTimeline(makeSkill("Flicker", 0, 0, Targeting::SingleEnemy, 1.6f, 5, 4.0f),
                                    makeTimeline({{0.8, 0.3, 0.0, 1.0f, false},
                                                  {1.1, 0.3, 0.0, 1.0f, false},
                                                  {1.4, 0.3, 0.0, 1.0f, false}})),
                       withTimeline(makeSkill("Feint", 0, 0, Targeting::SingleEnemy, 1.8f, 5, 5.0f),
                                    makeTimeline({{1.0, 0.4, 0.0, 1.0f, false}, {1.8, 0.4, 0.0, 2.0f, false}}))}),
    };
}

inline constexpr double kBpm = 120.0;

// The demo's rules. Music sync is on at kBpm (the Unreal side takes bpm and offset from its conductor).
inline BattleTuning tuning() {
    BattleTuning t;
    t.leadIn = 1.0;
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

// Presses every pending note of the party combo that is due by `now`, exactly at its time.
// Call before Battle::update(now). Returns the number of presses.
inline int pilotPress(Battle& b, double now) {
    int presses = 0;
    if (b.phase() != Phase::PartySkill) return presses;
    const ComboSequence& c = b.combo();
    for (std::size_t i = c.nextIndex(); i < c.size() && c.note(i).time <= now; ++i) {
        if (c.grade(i) != Grade::Pending) continue;
        b.press(c.note(i).arrow, c.note(i).time);
        ++presses;
        if (b.phase() != Phase::PartySkill) break;
    }
    return presses;
}

// Answers every pending hit of the enemy attack whose ideal press time is at or before `horizon`,
// at that ideal time: perfect parries with DefenceInput::Parry, perfect dodges with Dodge (jumps on
// jumpable hits, which a dodge can't answer). Call before Battle::update(horizon) (or with a small
// look-ahead past the current time, so a coarse frame clock never steps over a window). Returns
// the number of presses.
inline int pilotDefend(Battle& b, double horizon, DefenceInput mode = DefenceInput::Parry) {
    int presses = 0;
    while (b.phase() == Phase::EnemySkill) {
        const DefenceTimeline& d = b.defence();
        const std::size_t i = d.nextIndex();
        if (i >= d.size()) break;
        const DefenceInput input = mode == DefenceInput::Parry ? DefenceInput::Parry
                                   : d.accepts(i, DefenceInput::Dodge) ? DefenceInput::Dodge
                                                                       : DefenceInput::Jump;
        const double t = d.idealPress(i, input);
        if (t > horizon) break;
        b.defend(input, t);
        ++presses;
    }
    return presses;
}

struct HeadlessResult {
    Phase outcome = Phase::NotStarted; // Victory or Defeat when the fight ended, else where it stopped
    bool finished = false;
    bool commandFailed = false; // the pilot's choice was refused (never expected)
    int partyTurns = 0;
    int enemyAttacks = 0;
    int presses = 0;
    int partyHpLeft = 0; // sum over the party at the end
    double endTime = 0.0;
};

// Plays a whole fight on a fixed clock `step` seconds apart (default 120 Hz), up to `limit`
// battle seconds. The pilot picks skills and targets for every party turn; with `press` it steps on
// every note exactly on time and answers every enemy hit perfectly with `defence` (parries, or
// dodges and jumps), without it never presses. The same clock the Unreal autopilot sees: presses
// go in before the update that would expire them.
inline HeadlessResult playHeadless(std::vector<Combatant> party, std::vector<Combatant> foes, bool press,
                                   double step = 1.0 / 120.0, double limit = 1200.0,
                                   DefenceInput defence = DefenceInput::Parry) {
    Battle b(std::move(party), std::move(foes), tuning());
    HeadlessResult r;
    double now = 0.0;
    b.start(now);
    while (b.phase() != Phase::Victory && b.phase() != Phase::Defeat && now < limit) {
        if (b.phase() == Phase::AwaitingCommand) {
            const PilotChoice choice = pilotChoice(b);
            if (b.command(choice.skill, choice.target, now) != CommandError::None) {
                r.commandFailed = true;
                break;
            }
            ++r.partyTurns;
        }
        const Phase before = b.phase();
        if (press) r.presses += pilotPress(b, now) + pilotDefend(b, now + step, defence);
        now += step;
        b.update(now);
        if (before != Phase::EnemySkill && b.phase() == Phase::EnemySkill) ++r.enemyAttacks;
        b.clearEvents();
    }
    r.outcome = b.phase();
    r.finished = r.outcome == Phase::Victory || r.outcome == Phase::Defeat;
    r.endTime = now;
    for (const Combatant& c : b.combatants())
        if (c.team == Team::Party) r.partyHpLeft += c.hp;
    return r;
}

} // namespace ftcore::demo
