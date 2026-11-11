#pragma once

// Test fixtures: the first three shared-input bosses as BossScriptDef tables, kept equal to their data
// files in Data/Battle/boss_script (docs/BOSS_SCRIPT.md); the game loads the files. Tuning numbers
// are first guesses for playtests; the structure (phases, the five moves and their counters, the
// lock and its key, the offer) is the design.

#include "ftcore/BossScript.h"

#include <initializer_list>

namespace ftcore {

namespace detail {
inline BossMoveDef bossMove(std::string_view id, BossCounter counter, float tell, float window, float recover) {
    BossMoveDef m;
    m.setId(id);
    m.counter = counter;
    m.tell = tell;
    m.window = window;
    m.recover = recover;
    return m;
}
inline BossPhaseDef bossPhase(float enterAt, std::initializer_list<std::uint8_t> moves) {
    BossPhaseDef p;
    p.enterAt = enterAt;
    for (const std::uint8_t m : moves)
        if (p.moveCount < kMaxScriptMoves) p.moves[p.moveCount++] = m;
    return p;
}
} // namespace detail

// The glider: fights aloft, then grounded. The weak point shows only while it is pinned by a guarded
// swoop, and opens to a blow on the count. The end: at a tenth of its health a bystander answers its
// offer, and the party takes it.
inline BossScriptDef gliderBossDef() {
    using detail::bossMove;
    BossScriptDef d;
    d.setId("glider");
    d.hp = 3000;
    d.critMultiplier = 2.5f;
    d.restSeconds = 1.2f;
    d.lock.keys = kUnlockSharedBlow;
    d.lock.openSeconds = 3.0;
    enum : std::uint8_t { Swoop, Lash, Lull, Grab, Updraft };
    d.moves[Swoop] = bossMove("swoop", BossCounter::Guard, 1.4f, 0.4f, 1.0f); // shadow narrows
    d.moves[Swoop].exposeSeconds = 2.5f; // guarded, it is pinned: the weak point shows
    d.moves[Lash] = bossMove("tail_lash", BossCounter::Dodge, 0.9f, 0.35f, 1.2f); // rattle; snags a post
    d.moves[Lull] = bossMove("lull", BossCounter::StrikeOnCount, 2.0f, 1.5f, 0.8f); // colours dim
    d.moves[Grab] = bossMove("grab", BossCounter::TakeHalf, 1.2f, 1.0f, 1.0f); // takes a bystander
    d.moves[Updraft] = bossMove("updraft", BossCounter::Recover, 1.0f, 0.8f, 0.6f); // litter spirals
    d.moveCount = 5;
    d.offer = bossMove("offer", BossCounter::Answer, 2.0f, 6.0f, 1.0f);
    d.offer.needsAnswer = true;
    d.phases[0] = detail::bossPhase(1.0f, {Swoop, Lash, Grab, Updraft}); // aloft
    d.phases[1] = detail::bossPhase(0.5f, {Swoop, Lull, Lash, Grab});    // grounded
    d.phases[2] = detail::bossPhase(0.1f, {Swoop});                      // the offer
    d.phaseCount = 3;
    return d;
}

// The bearer: walks a circuit carrying a heavy load. It does not attack at first: its swing is the
// danger. Its bearing shoulder opens when two fighters take the load's free end together on the count.
// The end: it holds out the free end, and two fighters carry it home with it.
inline BossScriptDef bearerBossDef() {
    using detail::bossMove;
    BossScriptDef d;
    d.setId("bearer");
    d.hp = 3600;
    d.critMultiplier = 2.0f;
    d.restSeconds = 1.5f;
    d.lock.keys = kUnlockSharedBlow;
    d.lock.openSeconds = 4.0;
    enum : std::uint8_t { Turn, Shift, SetDown, Knocks, FreeEnd };
    d.moves[Turn] = bossMove("turn", BossCounter::Strike, 1.5f, 0.6f, 1.0f); // pivots: step in, strike low
    d.moves[Shift] = bossMove("shoulder_shift", BossCounter::Strike, 1.0f, 0.5f, 0.8f);
    d.moves[Shift].exposeSeconds = 1.0f; // the old shoulder, open for a breath
    d.moves[SetDown] = bossMove("set_down", BossCounter::GetClear, 1.6f, 0.5f, 1.4f); // kneels, then slams
    d.moves[Knocks] = bossMove("two_knocks", BossCounter::StrikeOnCount, 1.8f, 1.6f, 0.8f); // all holds still
    d.moves[FreeEnd] = bossMove("free_end", BossCounter::CarryTogether, 1.5f, 2.0f, 1.0f);
    d.moves[FreeEnd].unlocksOnCounter = true;
    d.moveCount = 5;
    d.offer = bossMove("carry_home", BossCounter::CarryTogether, 2.0f, 8.0f, 1.0f);
    d.phases[0] = detail::bossPhase(1.0f, {Turn, Shift});                // carrying
    d.phases[1] = detail::bossPhase(0.6f, {SetDown, Turn, Knocks, Shift}); // failed set-downs
    d.phases[2] = detail::bossPhase(0.25f, {FreeEnd, Knocks});           // the offer
    d.phaseCount = 3;
    return d;
}

// The snuffer: an arena of lamps it eats. Phases follow the lamps left (eight, three, one). Its
// belly opens only as it swallows, and only while a bystander keeps a lamp lit behind it (the ward:
// the fight is lost if they fall). The end: the bystander answers its offer, and a blow on the count
// finishes it.
inline BossScriptDef snufferBossDef() {
    using detail::bossMove;
    BossScriptDef d;
    d.setId("snuffer");
    d.hp = 3200;
    d.gauge = BossGauge::Resource;
    d.resourceMax = 8;
    d.resourceStart = 8;
    d.wardLimit = 3;
    d.critMultiplier = 2.5f;
    d.restSeconds = 1.0f;
    d.lock.keys = kUnlockSharedBlow;
    d.lock.openSeconds = 2.5;
    enum : std::uint8_t { Snuff, Swallow, DarkStep, Breath, Reach };
    d.moves[Snuff] = bossMove("snuff", BossCounter::Strike, 1.2f, 0.5f, 1.0f); // reaches for a lamp
    d.moves[Snuff].resourceOnLand = -1;
    d.moves[Swallow] = bossMove("swallow", BossCounter::StrikeOnCount, 1.4f, 1.0f, 1.0f); // head tips back
    d.moves[Swallow].unlocksOnCounter = true;
    d.moves[Swallow].needsWatch = true; // the host: watched while a lamp burns behind it
    d.moves[DarkStep] = bossMove("dark_step", BossCounter::Listen, 1.0f, 0.8f, 0.8f); // a lamp dies
    d.moves[DarkStep].resourceOnLand = -1;
    d.moves[DarkStep].resourceOnCounter = -1; // the lamp dies either way; listening finds it
    d.moves[Breath] = bossMove("glass_breath", BossCounter::Guard, 1.0f, 0.5f, 1.0f); // shards
    d.moves[Reach] = bossMove("reach", BossCounter::TakeHalf, 1.2f, 0.8f, 1.0f); // an arm at the ward
    d.moves[Reach].harmsWard = true;
    d.moveCount = 5;
    d.offer = bossMove("offer", BossCounter::StrikeOnCount, 2.0f, 6.0f, 1.0f);
    d.offer.needsAnswer = true;
    d.phases[0] = detail::bossPhase(8.0f, {Snuff, Reach, Breath, DarkStep}); // eight lamps
    d.phases[1] = detail::bossPhase(3.0f, {DarkStep, Swallow, Reach, Breath});  // three lamps
    d.phases[2] = detail::bossPhase(1.0f, {Swallow, Reach});                   // one lamp
    d.phaseCount = 3;
    return d;
}

} // namespace ftcore
