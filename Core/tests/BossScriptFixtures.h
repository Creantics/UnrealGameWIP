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

// Test table 1: hp gauge, three phases (1.0 / 0.5 / 0.1). A guarded move exposes the weak point and a
// shared blow unlocks it. The last phase's offer needs answer() before its counter.
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
    d.moves[Swoop] = bossMove("swoop", BossCounter::Guard, 1.4f, 0.4f, 1.0f);
    d.moves[Swoop].exposeSeconds = 2.5f;  // exposes on its counter
    d.moves[Lash] = bossMove("tail_lash", BossCounter::Dodge, 0.9f, 0.35f, 1.2f);
    d.moves[Lull] = bossMove("lull", BossCounter::StrikeOnCount, 2.0f, 1.5f, 0.8f);
    d.moves[Grab] = bossMove("grab", BossCounter::TakeHalf, 1.2f, 1.0f, 1.0f);
    d.moves[Updraft] = bossMove("updraft", BossCounter::Recover, 1.0f, 0.8f, 0.6f);
    d.moveCount = 5;
    d.offer = bossMove("offer", BossCounter::Answer, 2.0f, 6.0f, 1.0f);
    d.offer.needsAnswer = true;
    d.phases[0] = detail::bossPhase(1.0f, {Swoop, Lash, Grab, Updraft});
    d.phases[1] = detail::bossPhase(0.5f, {Swoop, Lull, Lash, Grab});
    d.phases[2] = detail::bossPhase(0.1f, {Swoop});
    d.phaseCount = 3;
    return d;
}

// Test table 2: hp gauge, three phases (1.0 / 0.6 / 0.25). One move exposes on its counter and a
// carry-together counter unlocks the weak point; the offer takes a carry-together counter.
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
    d.moves[Turn] = bossMove("turn", BossCounter::Strike, 1.5f, 0.6f, 1.0f);
    d.moves[Shift] = bossMove("shoulder_shift", BossCounter::Strike, 1.0f, 0.5f, 0.8f);
    d.moves[Shift].exposeSeconds = 1.0f;  // exposes on its counter
    d.moves[SetDown] = bossMove("set_down", BossCounter::GetClear, 1.6f, 0.5f, 1.4f);
    d.moves[Knocks] = bossMove("two_knocks", BossCounter::StrikeOnCount, 1.8f, 1.6f, 0.8f);
    d.moves[FreeEnd] = bossMove("free_end", BossCounter::CarryTogether, 1.5f, 2.0f, 1.0f);
    d.moves[FreeEnd].unlocksOnCounter = true;
    d.moveCount = 5;
    d.offer = bossMove("offer_carry", BossCounter::CarryTogether, 2.0f, 8.0f, 1.0f);
    d.phases[0] = detail::bossPhase(1.0f, {Turn, Shift});
    d.phases[1] = detail::bossPhase(0.6f, {SetDown, Turn, Knocks, Shift});
    d.phases[2] = detail::bossPhase(0.25f, {FreeEnd, Knocks});
    d.phaseCount = 3;
    return d;
}

// Test table 3: resource gauge (8), phases at 8 / 3 / 1, a ward (limit 3). An on-count counter unlocks
// the weak point only while watched (needsWatch); the offer needs answer() and an on-count counter.
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
    d.moves[Snuff] = bossMove("snuff", BossCounter::Strike, 1.2f, 0.5f, 1.0f);
    d.moves[Snuff].resourceOnLand = -1;
    d.moves[Swallow] = bossMove("swallow", BossCounter::StrikeOnCount, 1.4f, 1.0f, 1.0f);
    d.moves[Swallow].unlocksOnCounter = true;
    d.moves[Swallow].needsWatch = true;  // only while the host says watched
    d.moves[DarkStep] = bossMove("dark_step", BossCounter::Listen, 1.0f, 0.8f, 0.8f);
    d.moves[DarkStep].resourceOnLand = -1;
    d.moves[DarkStep].resourceOnCounter = -1;  // on land or counter
    d.moves[Breath] = bossMove("glass_breath", BossCounter::Guard, 1.0f, 0.5f, 1.0f);
    d.moves[Reach] = bossMove("reach", BossCounter::TakeHalf, 1.2f, 0.8f, 1.0f);
    d.moves[Reach].harmsWard = true;
    d.moveCount = 5;
    d.offer = bossMove("offer", BossCounter::StrikeOnCount, 2.0f, 6.0f, 1.0f);
    d.offer.needsAnswer = true;
    d.phases[0] = detail::bossPhase(8.0f, {Snuff, Reach, Breath, DarkStep});
    d.phases[1] = detail::bossPhase(3.0f, {DarkStep, Swallow, Reach, Breath});
    d.phases[2] = detail::bossPhase(1.0f, {Swallow, Reach});
    d.phaseCount = 3;
    return d;
}

} // namespace ftcore
