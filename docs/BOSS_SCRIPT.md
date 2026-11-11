# Boss script (`BossScript`, `BossScriptData`)

One data-driven state machine for the shared-input bosses, plus the first three boss tables. These
are header-only in `Core/include/ftcore/`, with no allocation after `reset`. Tests are in
`Core/tests/test_boss_script.cpp`.

## Rules

* **Moves:** five at most. Each move is played as *tell → window → recovery*.
  * The host plays the tell on `Tell` and opens the counter on `WindowOpened`.
  * The move's one counter (`BossCounter`) is taken only inside the window.
  * If the window runs out uncountered, the move lands (`Landed`) and the host applies its damage.
  * The shared counters need the shared input (`onCount`). That is a `StrikeCount` `OnCount`, or a hit a lead took half of.
    * `StrikeOnCount` and `TakeHalf` need it.
    * `CarryTogether` needs it from two different fighters in the same window.
* **Phases:** three at most. Each phase has its own move rotation.
  * A phase starts when the gauge falls to its `enterAt`. The gauge is either:
    * the hp share (`Hp`), or
    * a resource count (`Resource`): something in the arena the boss consumes, which the host can also add back with `addResource`.
  * Phases never go back.
* **Locked weak point (`WeakPointLock`):**
  * A move with `exposeSeconds` exposes the point when it is countered. A shared blow while exposed (`sharedBlow`) unlocks it.
  * A move with `unlocksOnCounter` unlocks the point on its counter.
  * Blows on the point crit only while it is unlocked.
* **The end is an answered offer.** Hp never falls below 1.
  * The last phase plays the def's `offer` first and between its moves. Countering the offer ends the fight (`Ended`).
  * With `needsAnswer`, the counter waits for the host's `answer()`.
* **Ward (optional):** someone the party keeps alive. Each landed `harmsWard` move harms it. At `wardLimit` the fight is lost (`Lost`).
* **Time:** times follow `CombatInputs.h`. A late frame plays every step it passed, in order. One call plays at most 4096 steps, and the rest play on the next call.

## The three tables

| id | gauge | moves (counter) | phases | weak point | end |
|---|---|---|---|---|---|
| `glider` | hp | swoop (guard, exposes), tail_lash (dodge), lull (on count), grab (take half), updraft (recover) | 1.0 / 0.5 / 0.1 | a guarded swoop pins it; a shared blow opens it | the offer, answered, then `Answer` |
| `bearer` | hp | turn (strike), shoulder_shift (strike, exposes), set_down (get clear), two_knocks (on count), free_end (carry together, unlocks) | 1.0 / 0.6 / 0.25 | two fighters take the free end together | `carry_home`, carried together |
| `lamp_eater` | lamps 8 | snuff (strike; eats a lamp), swallow (on count, unlocks), dark_step (listen; a lamp dies), glass_breath (guard), reach (take half; harms the ward) | 8 / 3 / 1 lamps | struck on the count as it swallows | the offer, answered, then a blow on the count |

The tuning numbers are first guesses for playtests.
