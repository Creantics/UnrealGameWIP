#pragma once

// The line-up of the battle demo (two party members against two enemies) and a simple pilot that
// plays a battle on its own: it picks a skill and target for each party turn, steps on every
// note exactly on time and answers every enemy hit at its ideal time (read from the attack
// timeline, never from audio). The pilot drives the headless test of the demo fight and the demo's
// autoplay toggle. It also aims (WeakPoint.h): when a shot at the target's best weak point beats
// its best skill, it enters aim, puts the aim point on that weak point and fires. Opt-in only, given
// PilotItems, it also heals a member who is low with a carried item. Header-only, so
// there is no extra translation unit to wrap for Unreal.

#include "ftcore/Battle.h"
#include "ftcore/CombatTypes.h"
#include "ftcore/Progression.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
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

inline WeakPoint makeWeakPoint(std::string_view id, float x, float y, float radius, float multiplier) {
    WeakPoint w;
    w.setId(id);
    w.x = x;
    w.y = y;
    w.radius = radius;
    w.multiplier = multiplier;
    return w;
}

inline WeakPoints makeWeakPoints(float halfWidth, std::initializer_list<WeakPoint> points) {
    WeakPoints out;
    out.halfWidth = halfWidth;
    for (const WeakPoint& p : points) out.add(p);
    return out;
}

inline Combatant withWeakPoints(Combatant c, WeakPoints points) {
    c.weakPoints = points;
    return c;
}

inline Combatant makeCombatant(std::string name, Stats stats, std::vector<Skill> skills) {
    Combatant c;
    c.name = std::move(name);
    c.stats = stats;
    c.hp = stats.maxHp;
    c.skills = std::move(skills);
    return c;
}

// The party, in order: the first leads on the field and the rest follow it (PartyTrail.h).
inline constexpr std::array<const char*, 2> kPartyNames{"Devin", "Duy"};

// Stats are {maxHp, attack, defense, speed, technique}, at level 1.
inline std::vector<Combatant> partyAtLevel1() {
    const Skill strike = makeSkill("Strike", 0, 1, Targeting::SingleEnemy, 1.0f, 4, 1.0f);
    return {
        makeCombatant(kPartyNames[0], {140, 24, 8, 12, 12},
                      {strike, makeSkill("Twin Cut", 2, 0, Targeting::SingleEnemy, 2.0f, 5, 3.0f),
                       makeSkill("Sweep", 3, 0, Targeting::AllEnemies, 1.6f, 6, 4.0f),
                       makeSkill("Heavy Blow", 5, 0, Targeting::SingleEnemy, 4.0f, 8, 6.0f)}),
        makeCombatant(kPartyNames[1], {110, 20, 4, 15, 15},
                      {strike, makeSkill("Spark", 1, 0, Targeting::SingleEnemy, 1.5f, 4, 2.0f),
                       makeSkill("Pierce", 2, 0, Targeting::SingleEnemy, 2.2f, 6, 4.5f),
                       makeSkill("Flare", 4, 0, Targeting::AllEnemies, 2.4f, 7, 5.0f)}),
    };
}

// The party at `level` (see Progression.h), at full HP.
inline std::vector<Combatant> party(int level = 1) {
    std::vector<Combatant> members = partyAtLevel1();
    for (Combatant& c : members) {
        c.stats = statsAtLevel(c.stats, level);
        c.hp = c.stats.maxHp;
    }
    return members;
}

// The weak points of the brute and shade in Data/Battle/weakpoints.json (a test keeps them in step).
inline WeakPoints bruteWeakPoints() {
    return makeWeakPoints(0.5f, {makeWeakPoint("core", 0.0f, 0.55f, 0.12f, 2.5f),
                                 makeWeakPoint("knee", -0.28f, 0.15f, 0.10f, 1.5f)});
}

inline WeakPoints shadeWeakPoints() {
    return makeWeakPoints(0.45f, {makeWeakPoint("eye", 0.12f, 0.86f, 0.09f, 2.2f),
                                  makeWeakPoint("heart", -0.08f, 0.52f, 0.14f, 1.6f)});
}

// Same as the brute and shade in Data/Battle/enemies.json and weakpoints.json (a test keeps them
// in step).
inline std::vector<Combatant> enemies() {
    return {
        withWeakPoints(makeCombatant("Brute", {260, 18, 6, 8, 8},
                      {withTimeline(makeSkill("Slam", 0, 0, Targeting::SingleEnemy, 1.3f, 3, 1.0f),
                                    makeTimeline({{1.6, 0.4, 1.2, 1.0f, false}})),
                       withTimeline(makeSkill("Double Swipe", 0, 0, Targeting::SingleEnemy, 1.6f, 5, 2.5f),
                                    makeTimeline({{0.9, 0.4, 0.0, 1.0f, false}, {1.4, 0.4, 0.0, 1.0f, false}}))}),
                       bruteWeakPoints()),
        withWeakPoints(makeCombatant("Shade", {180, 15, 3, 14, 14},
                      {withTimeline(makeSkill("Flicker", 0, 0, Targeting::SingleEnemy, 1.6f, 5, 4.0f),
                                    makeTimeline({{0.8, 0.4, 0.0, 1.0f, false},
                                                  {1.1, 0.4, 0.0, 1.0f, false},
                                                  {1.4, 0.4, 0.0, 1.0f, false}})),
                       withTimeline(makeSkill("Feint", 0, 0, Targeting::SingleEnemy, 1.8f, 5, 5.0f),
                                    makeTimeline({{1.0, 0.4, 0.0, 1.0f, false}, {1.8, 0.4, 0.0, 2.0f, false}}))}),
                       shadeWeakPoints()),
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
    bool aim = false;                     // aim at the target instead of using `skill`
    std::size_t weakPoint = kMaxWeakPoints; // when aiming: the weak point to shoot
};

// For the acting party member: the strongest skill it can afford, on the living enemy with the
// least HP (ties: lowest id). Aim instead when it can afford it and a shot at the target's best
// weak point (aim power times its multiplier) beats that skill played as a full combo (power times
// 1 + fullComboBonus; an all-enemies skill counts half again). Only meaningful in
// Phase::AwaitingCommand.
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
    const AimTuning& aim = b.tuning().aim;
    const WeakPoints& points = all[out.target].weakPoints;
    const std::size_t weak = points.best();
    if (found && weak < points.size() && me.ap >= aim.apCost &&
        aim.power * points[weak].multiplier > best * (1.0f + b.tuning().fullComboBonus)) {
        out.aim = true;
        out.weakPoint = weak;
    }
    return out;
}

// In Phase::PartyAim: puts the aim point on weak point `weakPoint` at `now` and fires. Returns
// whether it fired.
inline bool pilotShoot(Battle& b, std::size_t weakPoint, double now) {
    if (b.phase() != Phase::PartyAim) return false;
    const AimPoint at = b.aimShot().reticleFor(weakPoint, now);
    b.setReticle(at.x, at.y);
    return b.fire(now);
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

// How well a simulated player plays: the share of combo notes it presses on time and of enemy hits
// it answers. The misses are spread evenly and deterministically (rate r: action n succeeds when
// round((n+1)r) > round(nr)), counted over the whole fight, so a 0.75 pilot hits exactly three in
// four. The defaults are the perfect pilot. Aiming stays perfect.
struct PilotSkill {
    double press = 1.0;
    double defend = 1.0;
};

inline bool pilotSucceeds(unsigned n, double rate) {
    if (rate >= 1.0) return true;
    if (rate <= 0.0) return false;
    return std::floor((n + 1) * rate + 0.5) > std::floor(n * rate + 0.5);
}

// Carried by an imperfect pilot across frames and turns.
struct PilotState {
    unsigned notes = 0; // combo notes acted on this fight
    unsigned hits = 0;  // enemy hits decided this fight
    std::size_t skippedHit = static_cast<std::size_t>(-1); // the incoming hit it lets land
};

// Like pilotPress, but a note the skill misses gets a wrong arrow at its time (one that matches
// neither it nor the next two notes), which grades it a miss without touching its neighbours.
inline int pilotPress(Battle& b, double now, const PilotSkill& skill, PilotState& state) {
    int presses = 0;
    if (b.phase() != Phase::PartySkill) return presses;
    const ComboSequence& c = b.combo();
    for (std::size_t i = c.nextIndex(); i < c.size() && c.note(i).time <= now; ++i) {
        if (c.grade(i) != Grade::Pending) continue;
        Arrow arrow = c.note(i).arrow;
        if (!pilotSucceeds(state.notes++, skill.press)) {
            const Arrow next = i + 1 < c.size() ? c.note(i + 1).arrow : arrow;
            const Arrow after = i + 2 < c.size() ? c.note(i + 2).arrow : arrow;
            for (Arrow a : {Arrow::Left, Arrow::Down, Arrow::Up, Arrow::Right})
                if (a != c.note(i).arrow && a != next && a != after) {
                    arrow = a;
                    break;
                }
        }
        b.press(arrow, c.note(i).time);
        ++presses;
        if (b.phase() != Phase::PartySkill) break;
    }
    return presses;
}

// Like pilotDefend, but a hit the skill misses gets no press: the pilot waits for it to land.
inline int pilotDefend(Battle& b, double horizon, DefenceInput mode, const PilotSkill& skill, PilotState& state) {
    int presses = 0;
    if (b.phase() != Phase::EnemySkill) state.skippedHit = static_cast<std::size_t>(-1);
    while (b.phase() == Phase::EnemySkill) {
        const DefenceTimeline& d = b.defence();
        const std::size_t i = d.nextIndex();
        if (i >= d.size() || i == state.skippedHit) break;
        const DefenceInput input = mode == DefenceInput::Parry ? DefenceInput::Parry
                                   : d.accepts(i, DefenceInput::Dodge) ? DefenceInput::Dodge
                                                                       : DefenceInput::Jump;
        const double t = d.idealPress(i, input);
        if (t > horizon) break;
        if (!pilotSucceeds(state.hits++, skill.defend)) {
            state.skippedHit = i;
            break;
        }
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
    int aims = 0;        // aimed shots fired
    int weakHits = 0;    // of those, shots on a weak point
    int partyHpLeft = 0; // sum over the party at the end
    double endTime = 0.0;
    int itemsUsed = 0;   // items the pilot used (only with PilotItems)
};

// An opt-in for the pilot: it uses carried items. The pilot never touches an item unless it is given
// these, so the balance gate (which plays without them) is unchanged.
struct PilotItems {
    ItemTable table;         // what the items do
    Inventory carried;       // what the party brings into the fight
    float healBelow = 0.4f;  // a member at or below this share of its max HP gets a heal, if one is carried
};

struct PilotItemChoice {
    bool use = false;
    std::size_t slot = 0;    // into Battle::carriedItems()
    CombatantId target = 0;
};

// For the acting party member: when the living member with the lowest share of max HP is at or below
// `healBelow`, use a carried heal item on it: the smallest heal that covers the HP it is missing, else
// the biggest one carried. Only meaningful in Phase::AwaitingCommand, with the items loaded.
inline PilotItemChoice pilotItemChoice(const Battle& b, float healBelow) {
    PilotItemChoice out;
    const std::vector<Combatant>& all = b.combatants();
    bool found = false;
    float lowest = 2.0f;
    CombatantId hurt = 0;
    for (CombatantId id = 0; id < all.size(); ++id) {
        const Combatant& c = all[id];
        if (c.team != Team::Party || !c.alive()) continue;
        const float share = static_cast<float>(c.hp) / static_cast<float>(c.stats.maxHp > 0 ? c.stats.maxHp : 1);
        if (share < lowest) {
            lowest = share;
            hurt = id;
            found = true;
        }
    }
    if (!found || lowest > healBelow) return out;
    const Combatant& member = all[hurt];
    const int missing = member.stats.maxHp - member.hp;
    int bestHeal = 0;
    bool covers = false;
    const Inventory& carried = b.carriedItems();
    for (std::size_t slot = 0; slot < carried.size(); ++slot) {
        const ItemDef* def = b.itemTable().find(carried[slot].id.view());
        if (!def || def->effect != ItemEffect::Heal) continue;
        const int heal = def->hpFor(member.stats.maxHp);
        const bool fits = heal >= missing;
        const bool better = !out.use || (fits && !covers) || (fits && covers && heal < bestHeal) ||
                            (!fits && !covers && heal > bestHeal);
        if (!better) continue;
        out.use = true;
        out.slot = slot;
        out.target = hurt;
        bestHeal = heal;
        covers = fits;
    }
    return out;
}

// The general form of playHeadless below: with `items` the pilot loads them into the battle and
// uses them (pilotItemChoice); without, nothing differs from a plain fight.
inline HeadlessResult playHeadlessWith(std::vector<Combatant> party, std::vector<Combatant> foes, bool press, double step,
                                       double limit, DefenceInput defence, PilotSkill skill, const PilotItems* items) {
    Battle b(std::move(party), std::move(foes), tuning());
    if (items) b.setItems(items->table, items->carried);
    HeadlessResult r;
    PilotState state;
    double now = 0.0;
    b.start(now);
    while (b.phase() != Phase::Victory && b.phase() != Phase::Defeat && now < limit) {
        if (items && b.phase() == Phase::AwaitingCommand) {
            const PilotItemChoice item = pilotItemChoice(b, items->healBelow);
            if (item.use && b.useItem(item.slot, item.target, now) == CommandError::None) {
                ++r.partyTurns;
                ++r.itemsUsed;
            }
        }
        if (b.phase() == Phase::AwaitingCommand) {
            const PilotChoice choice = pilotChoice(b);
            const bool aim = press && choice.aim;
            const CommandError e =
                aim ? b.aim(choice.target, now) : b.command(choice.skill, choice.target, now);
            if (e != CommandError::None) {
                r.commandFailed = true;
                break;
            }
            ++r.partyTurns;
            if (aim && pilotShoot(b, choice.weakPoint, now)) {
                ++r.aims;
                if (b.lastShot().kind == ShotKind::Weak) ++r.weakHits;
            }
        }
        const Phase before = b.phase();
        if (press)
            r.presses += pilotPress(b, now, skill, state) + pilotDefend(b, now + step, defence, skill, state);
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

// Plays a whole fight on a fixed clock `step` seconds apart (default 120 Hz), up to `limit`
// battle seconds. The pilot picks skills and targets for every party turn; with `press` it steps on
// every note exactly on time, answers every enemy hit perfectly with `defence` (parries, or
// dodges and jumps) and aims when pilotChoice says so (the shot fires the same frame, on the weak
// point); without it it never presses and never aims. `skill` makes the presses and answers
// imperfect (see PilotSkill). The same clock the Unreal autopilot sees:
// presses go in before the update that would expire them. It uses no items.
inline HeadlessResult playHeadless(std::vector<Combatant> party, std::vector<Combatant> foes, bool press,
                                   double step = 1.0 / 120.0, double limit = 1200.0,
                                   DefenceInput defence = DefenceInput::Parry, PilotSkill skill = {}) {
    return playHeadlessWith(std::move(party), std::move(foes), press, step, limit, defence, skill, nullptr);
}

// The same with the pilot carrying `items`: it heals a member whose HP is low (PilotItems).
inline HeadlessResult playHeadless(std::vector<Combatant> party, std::vector<Combatant> foes, bool press,
                                   const PilotItems& items, double step = 1.0 / 120.0, double limit = 1200.0,
                                   DefenceInput defence = DefenceInput::Parry, PilotSkill skill = {}) {
    return playHeadlessWith(std::move(party), std::move(foes), press, step, limit, defence, skill, &items);
}

} // namespace ftcore::demo
