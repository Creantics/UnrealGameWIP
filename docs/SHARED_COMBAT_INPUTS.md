# Shared combat inputs (Core)

Five inputs every arena fight can use, plus the weak-point lock they serve. All of it is plain C++20 in
`Core/include/ftcore/`, header-only (no new `Core/src` file), no allocation after
construction, deterministic for a given input order. One header per piece:

| Header | Types | What it is |
|---|---|---|
| `CombatInputs.h` | `FighterId`, `kLeadA/kLeadB/kThirdSlot`, `inputTime` | shared ids and the clock rule |
| `StrikeCount.h` | `StrikeCount`, `CountTuning`, `CountEvent`, `CountHit` | the count |
| `GiftSlot.h` | `GiftSlot`, `GiftTally`, `GiftSave`, `GiftTuning` | the third slot's gifts |
| `Places.h` | `PlaceTable`, `PlaceTuning`, `PlaceSave` | laying a place |
| `TableSet.h` | `TableSet`, `TableTuning`, `TableSplit` | setting the table |
| `HeldRefusal.h` | `HeldRefusal`, `RefusalState`, `RefusalInput` | held refusals |
| `WeakPointLock.h` | `WeakPointLock`, `UnlockKey` | the locked weak point |

## Common rules

* **Time.** Every time is the fight clock in seconds (as `ArenaBoss.h`). Each object keeps `now`; a
  time that is earlier than `now` or not finite is read as `now` (a bad clock stalls, never hangs).
* **Fighters.** `FighterId` is a `uint8_t` index, 0..`kMaxFighters`-1 (4): `kLeadA` 0, `kLeadB` 1,
  `kThirdSlot` 2, `kEnemyFighter` 3 (the enemy side, for the false count and the table's seated enemy).
  The host maps its pawns to these ids; Core never knows names.
* **Events.** Pieces with timed output (`StrikeCount`, `TableSet`) emit into a fixed array cleared by
  each call that returns it, like `ArenaBossEvents`. Everything else answers with a return value.
* **Enums are append-only** (saves and the wrapper store them as integers).

## 1. The count (`StrikeCount`)

One fighter calls a short shared strike window: "two, three, now". The count rewards blows landed
**together**: a blow cannot know whether a partner will join it, so a lone blow in a live window is
held and settles when the window decides.

```
Idle --call(caller, t, voice)--> Live (cues, then the window) --(window closes)--> Idle
      the next call waits for readyAt() = window end + cooldown (or cancel time + cooldown)
```

* `call(caller, t, voice)`: `caller` is a lead, the third slot, or `kNoFighter` (a silenced count).
  Cues at `t`, `t + 1 beat`, and the "now" at `t + leadBeats` beats (default 2). The window is
  `[now - before, now + after]`, edges inclusive. Returns `CountError`: `Live` (a true count is live),
  `Cooling`, `BadCaller` (the enemy or an unknown id: enemies only play false counts).
* `voice`: `Voice` (spoken) or `Click` (tempo click only). A silenced count is `kNoFighter`, always
  `Click`. The voice is cosmetic; the caller decides the rule below.
* **The rule.** If two or more different fighters land inside one window, every blow in it is boosted
  by `onCountMultiplier` (2.5), the caller's included: the window is *joined*. A blow landed alone
  inside a called window gets no boost, whoever threw it. A caller who calls and gets no partner blow
  at all lands at `callerAloneMultiplier` (0.5, a knob, 0..1); anyone else's lone blow is plain (x1).
  * `judgeBlow(striker, t) -> CountHit { outcome, multiplier, punishable, caller, blow }`. In the true
    window: before it is joined, the blow is `Held` (x1 for now, settled later); the first blow by a
    second fighter joins the window, comes back `OnCount`, and the held blows settle `OnCount` at that
    time; once joined, every blow is `OnCount` at once. Outside every window: `Alone`.
  * When the window closes (or the count is cancelled or replaced) still unjoined, each held blow
    settles `Unanswered` (the caller's: x`callerAloneMultiplier`) or `Alone` (anyone else's: x1).
  * The host applies a `Held` blow's damage when its `BlowSettled` event arrives (matched by `blow`,
    a per-blow id that is never 0 for a lead or slot blow).
  * At most `kMaxHeldBlows` (8) are held; past that a lone blow settles at once with its lone outcome.
  * Enemy and unknown strikers are never held or boosted (`Alone`, `blow` 0).
* **Silenced.** A silenced count has no caller. Two fighters together still join it, at
  `silencedMultiplier` (1.6), on the narrower `silencedWindowScale` (0.6) window: the click helps,
  less. A lone blow on it is plain (nobody called).
* **False counts.** `falseCall(t, shiftSeconds)`: the enemy plays the first two cues on the true tempo
  and the "now" shifted early (negative) or late (positive), flagged `isFalse` so the host picks the
  false voice. The shift is clamped to one beat either way (the cues stay in order; a NaN or infinite
  shift reads as 0). It opens no window events. A blow inside `[falseNow - before, falseNow + after]`
  and in no true window is `FalseCount`: it misses (x`falseMultiplier`, default 0) and
  `punishable = true`: it leaves the fighter open. A blow in the true window is judged by the true
  one. At most one false count; a new one replaces it.
* `cancel(t)`: drops a live count (caller staggered, cutscene). Steps already due play; an opened
  window closes at `t` (`WindowClosed`); held blows settle at `t`; the cooldown runs from `t`.
  `cancelFalse(t)` drops the false count after its due steps.
* `update(now) -> const CountEvents&` (32 slots, `dropped` counts overflow): `Cue`, `WindowOpened`,
  `WindowClosed`, `BlowSettled`, each `{index, isNow, isFalse, voice, caller, at}` plus, for a
  settle, `{blow, striker, outcome, multiplier}`. A late frame plays every step it passed in time
  order (true run, then settles, then the false run on a tie). **Call order per frame:** judge the
  frame's blows, then call `update(frameEnd)` once.
* Tuning (`CountTuning`): `bpm` 90, `leadBeats` 2, `windowBefore` 0.12 s, `windowAfter` 0.18 s,
  `onCountMultiplier` 2.5, `silencedMultiplier` 1.6, `silencedWindowScale` 0.6,
  `callerAloneMultiplier` 0.5, `falseMultiplier` 0, `cooldown` 1.5 s. `validCountTuning` checks
  ranges (windows shorter than a beat); `reset` falls back to the defaults on bad tuning.
* `sharedOutcome(outcome)` is true for `OnCount`: what the lock's `SharedBlow` key takes.

## 2. The third slot's gifts (`GiftSlot`)

One button, three gifts: `Heal`, `Guard`, `Count`. Never an attack (there is no enum for one).

* **States** (`SlotState`): `Companion` (gifts work), `EmptyChair` (button does nothing), `Hostile`
  (button does nothing; the boss uses the tallies). No press is ever discarded. Set by the story through `setState`.
* `select(gift)` picks which gift the button gives (the host cycles it); `press(target, t, StrikeCount* count = nullptr)`:
  - `Companion`: gives the selected gift to `target` (a lead) unless that gift is cooling down. A
    pressed gift cannot be taken back: there is no cancel. Returns `GiftResult { given, gift, target,
    error }`; the host applies it: `Heal` (heal `healShare` of max HP and move `healLoad` Load off the
    target), `Guard` (the target takes x`guardScale` damage for `guardSeconds`, Core keeps the timer:
    `guardScaleFor(lead, t)`), `Count` (the slot calls a `StrikeCount` with caller `kThirdSlot`;
    `GiftSlot::press` takes the count so the call and the gift are one action; if the count refuses
    or is null, the error is `CountRefused`, the gift is not given and nothing is counted).
  - Errors (`GiftError`): `EmptyChair`, `Hostile`, `Cooling`, `BadTarget` (not a lead),
    `CountRefused`, `NoGift`.
  - `EmptyChair`: nothing happens; `emptyPresses` goes up (the game remembers you reached for him).
  - `Hostile`: nothing happens; `hostilePresses` goes up.
* **Threads.** Every given gift adds one thread to its target lead (`threads(lead)`), forever.
* **The tally** (`GiftTally`): presses per gift, save-wide, saturating at `UINT32_MAX`. Read any time:
  `count(gift)`, `total()`, `mostUsed()` (the most pressed; on a tie the earliest in `Heal, Guard,
  Count` order; `Gift::None` when the total is 0). Only given gifts count here.
* **Three tallies.** Companion gifts (per gift, above), `emptyPresses()` and `hostilePresses()`;
  `allPresses()` is the total across all three. A companion press refused for its cooldown or target
  is not counted.
* **Save** (`GiftSave`): plain data, `version` (1), `presses[3]`, `emptyPresses`, `hostilePresses`, `threads[2]`,
  `selected`, `state`. `save()` / `load(const GiftSave&)`; load rejects (returns false, changes
  nothing) version 0, a newer version or an out-of-range enum. Cooldowns and live guards are not
  saved (a load is never mid-fight); `load` and `resetFight()` clear them. `reset()` wipes the save
  data too; `setTuning()` changes the tuning and keeps it.
* Tuning (`GiftTuning`): per-gift `cooldown` (heal 8 s, guard 6 s, count 4 s), `healShare` 0.15,
  `healLoad` 20, `guardScale` 0 (the third stands in the lead's place: the guarded lead takes x0, nothing), `guardSeconds` 1.2.

## 3. Laying a place (`PlaceTable`)

One per fighter that can lay places. A place is a chair at the fighter's table.

* Place states: `Named` (a person, by an opaque `uint16_t` id from data), `LeftOpen` (deliberately
  left open), `Emptied` (the person died or forgot; the chair stays, empty), `Free` (no chair).
* `seats` (rating, progression, saved): how many chairs the fighter can hold, 1..`kMaxPlaces` (8).
* `layPlace(person)`: needs a free chair within the rating and the person not already seated;
  `leaveOpen()`: the same for an open chair. Errors: `NoSeat`, `AlreadySeated`, `BadPerson`.
* `carryCap()`: `baseCarry + perPlace x named` capped at `kMaxLoad` (40 + 20 each: three places carry
  a full 100). Only `Named` raises it; an open chair holds nobody.
* `emptyPlace(person, Load* load = nullptr)`: the chair becomes `Emptied`, `carryCap()` falls, and
  the weight falls on the fighter: `fallLoad` (20) is stored on `load` past the cap, up to `kMaxLoad`.
  Returns `PlaceEmptied { emptied, error, chair, capBefore, capAfter, fell, excess }`; `excess` is how
  far the Load now stands above the new cap. The same person may be laid again later (a new chair). Over the cap the fighter
  is overloaded (`overCap(load)`), as `Load.h` already treats a full Load.
* `clearPlace(chair)` frees an `Emptied` or `LeftOpen` chair (story only).
* `tableSeats()`: `Named + LeftOpen` (an emptied chair does not count).
* `PlaceTable::storeCapped(Load&, amount)` stores up to the place cap and returns what it stored.
* `raiseSeats(n)` only raises the rating.
* Save (`PlaceSave`): `version`, `seats`, `states[8]`, `persons[8]`. `load` rejects a bad version,
  rating or state, a chair past the rating that is not `Free`, a `Named` or `Emptied` chair with no
  person, and a person `Named` twice.

## 4. Setting the table (`TableSet`)

```
Ready --begin--> Setting --(setTime)--> Set --(setDuration)--> Cooling --(cooldown)--> Ready
                    |  interrupt(t)               |  cancel(t)
                    +--> Cooling (failed, failCooldown)  +--> Cooling
```

* `begin(setter, load, places, seated, t)`: needs `load >= places.carryCap()` (full Load) and
  `places.tableSeats() >= minSeats` (3). `seated` is the bit mask of fighters at the table in this
  fight (bit i = FighterId i, the enemy bit ignored; the setter always sits). Errors: `BadSetter`, `Busy`,
  `Cooling`, `NotFull`, `TooFewSeats`, checked in that order. On begin the fighter's Load is spent (the host calls `load.release()`).
* `interrupt(t)` during `Setting` (the fighter staggered or downed): a `Failed` event, `failed()`, the Load stays spent,
  `failCooldown` applies. This is the "can fail" rule.
* **The seat rule.** The table holds only with at least `minSeats` seats, each filled (`Named`) or
  deliberately left open (`LeftOpen`); an open seat is a state of its own, not an empty one. The host
  calls `checkSeats(places, t)` whenever a place changes: while `Setting` or `Set` with too few seats,
  the table gutters out (`Guttered` event, `failed()`, the enemy unseated, `failCooldown`).
* `seatEnemy(t)` while `Set` and the boss allows it: the enemy is seated until the table clears.
* `split(target, amount, t) -> TableSplit`: while `Set`, a blow on a seated fighter is shared: every
  seated fighter (and a seated enemy, at `enemyShare` x its share) takes `amount / n` each, where n
  counts the seated enemy. The seated enemy's share is real damage to it: this is how it is hurt.
  A blow on the seated enemy itself is not shared back: it takes the blow in full. A setter seated
  alone shares with nobody. Outside `Set`, or on an unseated target, the target takes it all.
* Tuning: `minSeats` 3, `setTime` 0.8 s, `setDuration` 6 s, `cooldown` 20 s, `failCooldown` 8 s,
  `enemyShare` 1.0.
* Events (`update(now) -> const TableEvents&`): `Setting`, `Set`, `Failed`, `EnemySeated`, `Cleared`,
  `Ready`, `Guttered`, each at its own time; a late frame plays every timed change it passed.

## 5. Held refusals (`HeldRefusal`)

An offer the player refuses by holding a button. No meter, no timer: the hold lasts as long as the
offer does, which the host plays (a line, a beat) and ends with `offerEnded()`.

```
Offered --holdDown--> Holding --offerEnded--> Refused
   ^                     |
   +-----holdUp----------+  (released early: the prompt repeats, repeats++)
Offered --accept()--> Accepted          Offered --offerEnded / defer()--> Deferred (= accepted)
```

* `offer()` starts (or restarts) the prompt. `repeats()` counts early releases so the host can vary
  the line. There is no limit.
* **"Not yet" is a yes.** An offer that ends without a hold, or an explicit `defer()`, resolves as
  `Deferred`, and `accepted()` is true for both `Accepted` and `Deferred`. Only a hold held to the end
  is `Refused`.
* A hold that starts before `offer()` does not count; it must start while the prompt is up.
* `defer()` works from `Offered` or `Holding`. `clearButton()` forgets the button without a release
  (focus lost): a stale press is cleared, a hold in progress stays a hold.

## 6. The locked weak point (`WeakPointLock`)

A boss weak point that is locked until one of its keys unlocks it.

* Keys (`UnlockKey`, a mask): `SharedBlow` (`sharedBlow(hit)` or `sharedBlow(event)`: an `OnCount`
  hit or a `BlowSettled` event that settled `OnCount`; for a hit a lead took half of, the host calls
  `offer(kUnlockSharedBlow, t)` directly),
  `ReadClause` (the right hidden clause read: `offer(kUnlockReadClause, t, clauseId)` with the lock's
  `clause`), `Refusal`
  (a `HeldRefusal` resolved `Refused`). Each boss's lock lists the keys that open it (one world's
  bosses take one key; nothing stops a boss taking two).
* `offer(key, t, clause)` -> `UnlockResult` (`Unlocked`, `Refreshed` when already open: the open time
  restarts, `WrongKey`, `WrongClause`, `BadKey`). `onDeferred(t)` reports a `Deferred` refusal: it
  counts `deferrals()` and relocks an open lock when `relockOnDefer` (default true).
* Open: `openSeconds` (3.0) after the unlock, or until `relock()` when 0. While open the host opens the
  `CritWindow` (`ArenaBoss.h`) with `until()`; locked, it keeps it closed. `opens()` counts unlocks.

## What the Unreal wrapper needs

* Input actions: `Count` (call), the gift button and a gift select, `LayPlace` (with a person id from
  the story data), `SetTable`, `Refuse` (a hold). Blows report `(striker, time)` to
  `StrikeCount::judgeBlow` before damage, and their damage through `TableSet::split`.
* Audio and HUD: the count cues (three voices plus a false voice, and a click), the gift threads per
  lead, the place chairs (`Named`, `LeftOpen`, `Emptied`), the table state, the refusal prompt with no
  meter.
* Saves: `GiftSave` and one `PlaceSave` per fighter in the SaveGame object (plain integers).
* Story hooks: `GiftSlot::setState`, `PlaceTable::emptyPlace`, the boss's lock keys from its data.

## Edge cases the tests cover

Calls during a live count or cooldown; enemy calling a true count; two fighters joining (either
order), the caller alone (light), a partner alone (plain), held overflow, blow ids wrapping; a second
blow by one fighter; cancel and replace settling held blows; blows on the window's edges (inclusive); a silenced count; false counts early and
late, overlapping a true one, replaced; a late frame passing several cues; NaN and earlier times; a
gift pressed while cooling, in the empty chair, hostile; a count gift that the count refuses; tally
ties, zero, saturation, save/load round trip and bad saves; laying past the rating, the same person
twice, emptying an absent person, cap after emptying, the fall past the cap; the table with two
seats, two plus an open chair, not full, interrupted, cancelled, enemy share, unseated targets; the
table guttering when a place empties; the
refusal released early (repeats), held to the end, never held (deferred), held before the offer, a
lost button; each
lock key, wrong clause, key not in the mask, relock after time and on deferral; zero allocations in
every tick and judge call.
