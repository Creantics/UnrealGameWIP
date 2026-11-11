# Boss script (`BossScript`, `importBossScript`)

One data-driven state machine for the shared-input bosses (`Core/include/ftcore/BossScript.h`, header-only, no
allocation after `reset`) and its loader (`importBossScript` in `ArenaBoss.h`, with the arena bosses' checks).
Each boss is one file in `Data/Battle/boss_script/<id>.json`. The same tables as C++
(`Core/tests/BossScriptFixtures.h`) are test fixtures only; a test keeps them equal to the files. Tests are in
`Core/tests/test_boss_script.cpp`.

The weak point goes through `LockedPoint` (`WeakPointLock.h`), the one adapter both `BossScript` and
`ArenaBossRun` use over the lock.

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
  * A `needsWatch` move does either only while the host says the point is watched (`setWatched`).
  * Blows on the point crit only while it is unlocked.
* **The end is an answered offer.** Hp never falls below 1.
  * The last phase plays the def's `offer` first and between its moves. Countering the offer ends the fight (`Ended`).
  * With `needsAnswer`, the counter waits for the host's `answer()`.
* **Ward (optional):** someone the party keeps alive. Each landed `harmsWard` move harms it. At `wardLimit` the fight is lost (`Lost`).
* **Time:** times follow `CombatInputs.h`. A late frame plays every step it passed, in order. One call plays at most 4096 steps, and the rest play on the next call.

## The file

```json
{ "version": 1, "id": "glider", "hp": 3000, "gauge": "hp", "critMultiplier": 2.5, "restSeconds": 1.2,
  "lock": { "keys": ["shared_blow"], "openSeconds": 3 },
  "moves": [ { "id": "swoop", "counter": "guard", "tell": 1.4, "window": 0.4, "recover": 1, "exposeSeconds": 2.5 } ],
  "offer": { "id": "offer", "counter": "answer", "tell": 2, "window": 6, "recover": 1, "needsAnswer": true },
  "phases": [ { "moves": ["swoop"] }, { "enterAt": 0.5, "moves": ["swoop"] } ] }
```

* `gauge`: `"hp"` (default) or `"resource"` with `resourceMax` and `resourceStart` (1..32).
* `wardLimit` 0..10, `critMultiplier` 1..4, `restSeconds` 0..10. `lock` is required: the arena boss lock format.
* A move: `id`, `counter` (`guard`, `dodge`, `strike`, `strike_on_count`, `take_half`, `listen`, `get_clear`,
  `recover`, `carry_together`, `answer`), `tell` 0.2..5, `window` 0.1..10 (the offer: ..30), `recover` 0..5;
  optional `exposeSeconds`, `unlocksOnCounter`, `harmsWard`, `needsAnswer`, `needsWatch`, `resourceOnLand`,
  `resourceOnCounter` (-8..8).
* `phases`: 1..3; the first has no `enterAt`; `moves` lists move ids.
* Across fields: each phase starts below the one before (hp: a share above 0; resource: a whole count), a move
  is listed once per phase, only the last phase may be empty, exposing or unlocking needs the `shared_blow` key,
  `wardLimit` needs a `harmsWard` move, and the offer never harms the ward. Each error names its path.

## The test tables

`Data/Battle/boss_script/` holds three tables (`glider`, `bearer`, `snuffer`) that together cover both
gauges, every counter kind, an exposing move, unlocking on a counter, `needsWatch`, a ward and an offer
with and without `needsAnswer`. `Core/tests/BossScriptFixtures.h` builds the same tables in C++ and the
tests check the files match them.

The tuning numbers are first guesses for playtests.
