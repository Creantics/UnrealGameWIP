# Shared combat inputs (Core)

Five inputs every arena fight can use, the weak-point lock they serve, and the fight session that
owns them all. All of it is plain C++20 in
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
| `FightSession.h` | `FightSession`, `FightSessionDef`, `FightTick`, `SharedAction` | one fight's pieces on one clock |

## Common rules

* **Time.** Every time is the fight clock in seconds (as `ArenaBoss.h`). Each object keeps `now`; a
  time that is earlier than `now` or not finite is read as `now` (a bad clock stalls, never hangs),
  and a time more than `kMaxTimeJump` (600 s, above the longest cooldown of 300 s) past `now` is junk:
  that call reads it as `now` and stores nothing, so the next real time works as if the junk call never
  happened. Because of that the host must call `update(t)` on every object each frame (`StrikeCount`,
  `TableSet`, `GiftSlot`, a `WeakPointLock` it drives directly, `BossScript`); an object left alone
  for more than `kMaxTimeJump` would ignore every later time. Every
  `reset` (and `GiftSlot::resetFight`) takes the fight's start time with no jump bound (`startTime`),
  so a fight may begin late on the clock; a host that starts a fight away from 0 must pass it (the
  constructors start at 0). `GiftSlot::load` keeps the clock. A lock inside a boss machine
  (`LockedPoint`) follows its owner's clock instead, with no bound (`BossScript` bounds its own clock;
  `ArenaBossRun` only refuses earlier and non-finite times). Ask the owner whether its point is open
  (`weakPointOpen`, `unlocked`), not `lock().open`, which lags between offers. Queries
  (`open(t)`, `guarded(lead, t)`, `guardScaleFor(lead, t)`, `ready(gift, t)`) read their time the
  same way without moving `now`. `inputTime(now, t, maxJump)` is the one rule.
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
  * Holding has no cap: before a window is joined the held blows are all one fighter's, so their ids
    run in order and Core keeps them as one run (first id and count). Every one of them is boosted
    when a partner joins. Their `BlowSettled` events do share the 64-slot buffer below: past 64 in
    one update the rest are counted in `droppedSettles`, and the host applies those blows at x1.
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
* `cancel(t)`: drops a live count (the host stops it). Steps already due play; an opened
  window closes at `t` (`WindowClosed`); held blows settle at `t`; the cooldown runs from `t`.
  A cancel at a time past the window's end does nothing beyond playing those due steps: the window
  closed on its own, and the count and its cooldown stand as they were.
  `cancelFalse(t)` drops the false count after its due steps.
* `update(now) -> const CountEvents&` (`kMaxCountEvents` = 64 slots): `Cue`, `WindowOpened`,
  `WindowClosed`, `BlowSettled`, each `{index, isNow, isFalse, voice, caller, at}` plus, for a
  settle, `{blow, striker, outcome, multiplier}`. A late frame plays every step it passed in time
  order (true run, then settles, then the false run on a tie). **Call order per frame:** judge the
  frame's blows, then call `update(frameEnd)` once.
* **Full buffers.** `dropped` counts every event lost to a full buffer, including ones lost between
  updates; the returned events carry it to the host. A `BlowSettled` pushes out the earliest `Cue`, else
  the earliest `WindowOpened`, else the earliest `WindowClosed`, to make room; only when every slot is a settle is it
  lost, and `droppedSettles` counts it. A lost settle never strands a blow: see the wrapper section.
* Tuning (`CountTuning`): `bpm` 90, `leadBeats` 2, `windowBefore` 0.12 s, `windowAfter` 0.18 s,
  `onCountMultiplier` 2.5, `silencedMultiplier` 1.6, `silencedWindowScale` 0.6,
  `callerAloneMultiplier` 0.5, `falseMultiplier` 0, `cooldown` 1.5 s. `validCountTuning` checks
  ranges (windows shorter than a beat); `reset` falls back to the defaults on bad tuning.
* `sharedOutcome(outcome)` is true for `OnCount`: what the lock's `SharedBlow` key takes.

## 2. The third slot's gifts (`GiftSlot`)

One button, three gifts: `Heal`, `Guard`, `Count`. Never an attack (there is no enum for one).

* **States** (`SlotState`): `Companion` (gifts work), `Vacant` (button does nothing), `Hostile`
  (button does nothing; the boss uses the tallies). No press is ever discarded. The host sets it
  through `setState`.
* `select(gift)` picks which gift the button gives (the host cycles it); `press(target, t, StrikeCount* count = nullptr)`:
  - `Companion`: gives the selected gift to `target` (a lead) unless that gift is cooling down. A
    pressed gift cannot be taken back: there is no cancel. Returns `GiftResult { given, gift, target,
    error, healShare, healLoad, guardScale, guardUntil }`, everything the host needs to apply it:
    `Heal` (heal `healShare` of max HP and move `healLoad` Load off the target), `Guard` (the target
    takes x`guardScale` damage until `guardUntil`; Core also keeps the timer: `guardScaleFor(lead, t)`),
    `Count` (the slot calls a `StrikeCount` with caller `kThirdSlot`; `GiftSlot::press` takes the
    count so the call and the gift are one action; if the count refuses or is null, the error is
    `CountRefused` and the gift is not given). A refused press leaves the heal and guard fields at
    their neutral values (0, 0, 1, 0).
  - Errors (`GiftError`): `Vacant`, `Hostile`, `Cooling`, `BadTarget` (not a lead),
    `CountRefused`, `NoGift`.
  - `Vacant`: nothing happens; `vacantPresses` goes up.
  - `Hostile`: nothing happens; `hostilePresses` goes up (the host reads the tallies).
* **Threads and cooldowns** follow given gifts only: every given gift adds one thread to its target
  lead (`threads(lead)`), forever, and starts that gift's cooldown. A refused press does neither.
* **The tally** (`GiftTally`): every `Companion` press, save-wide, saturating at `UINT32_MAX`:
  `given[3]` per gift and `refused[4]` per gift, the last slot for a press with no gift selected.
  Read any time: `givenOf(gift)`, `refusedOf(gift)`, `count(gift)` (given + refused), `total()` (every
  press in the tally), `mostUsed()` (the gift with the highest `count`; on a tie the earliest in
  `Heal, Guard, Count` order; `Gift::None` when no gift has a press).
* **Three tallies.** Companion presses (per gift, above), `vacantPresses()` and `hostilePresses()`;
  `allPresses()` is the total across all three. Every press lands in exactly one of them.
* **Save** (`GiftSave`): plain data, `version` (2), `given[3]`, `refused[4]`, `vacantPresses`,
  `hostilePresses`, `threads[2]`, `selected`, `state`. `save()` / `load(const GiftSave&)`; load
  rejects (returns false, changes nothing) version 0, a newer version or an out-of-range enum. A
  version 1 save (no refused counts) loads with `refused` all 0, and its old per-gift tally loads as
  `given` (version 1 counted given presses only). `SlotState` keeps its stored
  integers (`Vacant` is 1). Cooldowns and live guards are not saved (a load is never mid-fight);
  `load` and `resetFight()` clear them. `reset()` wipes the save data too; `setTuning()` changes
  the tuning and keeps it.
* Tuning (`GiftTuning`): per-gift `cooldown` (heal 8 s, guard 6 s, count 4 s), `healShare` 0.15,
  `healLoad` 20, `guardScale` 0 (the guarded lead takes x0), `guardSeconds` 1.2.

## 3. Laying a place (`PlaceTable`)

One per fighter that can lay places. A place is a chair at the fighter's table.

* Place states: `Named` (a person, by an opaque `uint16_t` id from data), `LeftOpen` (deliberately
  left open), `Emptied` (the host emptied it; the chair stays taken, with nobody in it, and keeps the
  person id), `Free` (no chair).
* `seats` (rating, progression, saved): how many chairs the fighter can hold, 1..`kMaxPlaces` (8).
* **One chair per id.** `layPlace(person)` needs a free chair within the rating and the person
  in no `Named` or `Emptied` chair (`holding(person)`); a person whose chair was emptied can be laid
  again only after that chair is cleared. `leaveOpen()` needs a free chair. Errors: `NoSeat`,
  `AlreadySeated`, `BadPerson`.
* `carryCap()`: `baseCarry + perPlace x named` capped at `kMaxLoad` (40 + 20 each: three places carry
  a full 100). Only `Named` raises it; an open chair holds nobody.
* `emptyPlace(person, Load* load = nullptr)`: the chair becomes `Emptied`, `carryCap()` falls, and
  the weight falls on the fighter: `fallLoad` (20) is stored on `load` past the cap, up to `kMaxLoad`.
  Returns `PlaceEmptied { emptied, error, chair, capBefore, capAfter, fell, excess }`; `excess` is how
  far the Load now stands above the new cap. Over the cap the fighter is overloaded
  (`overCap(load)`), as `Load.h` already treats a full Load.
* `clearPlace(chair)` frees an `Emptied` or `LeftOpen` chair; a `Named` chair empties first.
* `tableSeats()`: `Named + LeftOpen` (an emptied chair does not count).
* `PlaceTable::storeCapped(Load&, amount)` stores up to the place cap and returns what it stored.
* `raiseSeats(n)` only raises the rating.
* Save (`PlaceSave`): `version`, `seats`, `states[8]`, `persons[8]`. `load` rejects a bad version,
  rating or state, a chair past the rating that is not `Free`, a `Named` or `Emptied` chair with no
  person, and a person in two chairs (`Named` or `Emptied`).

## 4. Setting the table (`TableSet`)

```
Ready --begin--> Setting --(setTime)--> Set --(setDuration)--> Cooling --(cooldown)--> Ready
                    |  interrupt(t)               |  cancel(t)
                    +--> Cooling (failed, failCooldown)  +--> Cooling
```

* `begin(setter, load, places, seated, t)`: needs `load >= places.carryCap()` (full Load) and
  `places.tableSeats() >= minSeats` (3). `seated` is the bit mask of fighters at the table in this
  fight (bit i = FighterId i, the enemy bit ignored; the setter always sits). Errors: `BadSetter`, `Busy`,
  `Cooling`, `NotFull`, `TooFewSeats`, checked in that order. On success `begin` spends the setter's
  Load itself (`load.release()`); on an error the Load is untouched.
* `interrupt(t)` during `Setting` (the fighter staggered or downed): a `Failed` event, `failed()`, the Load stays spent,
  `failCooldown` applies.
* **The seat rule.** The table holds only with at least `minSeats` seats, each filled (`Named`) or
  deliberately left open (`LeftOpen`); an open seat is a state of its own, not an empty one.
  `TableSet` keeps no reference to the `PlaceTable`, so **the host must call `checkSeats(places, t)`
  after every change to the setter's places** while the table is `Setting` or `Set`: with too few
  seats the table gutters out (`Guttered` event, `failed()`, the enemy unseated, `failCooldown`).
* `seatEnemy(t)` while `Set` and the boss allows it: the enemy is seated until the table clears.
* `split(target, amount, t) -> TableSplit`: while `Set`, a blow on a seated fighter is shared: every
  seated fighter (and a seated enemy, at `enemyShare` x its share) takes `amount / n` each, where n
  counts the seated enemy. The seated enemy's share is real damage to it: this is how it is hurt.
  A blow on the seated enemy itself is not shared back: it takes the blow in full. A setter seated
  alone shares with nobody. Outside `Set`, or on an unseated target, the target takes it all.
* Tuning: `minSeats` 3, `setTime` 0.8 s, `setDuration` 6 s, `cooldown` 20 s, `failCooldown` 8 s,
  `enemyShare` 1.0.
* Events (`update(now) -> const TableEvents&`): `Setting`, `Set`, `Failed`, `EnemySeated`, `Cleared`,
  `Ready`, `Guttered`, each at its own time; a late frame plays every timed change it passed. Past
  `kMaxTableEvents` (16) waiting, later events are lost and the returned `dropped` counts them; the
  host reads the table's state (`state()`, `set()`, `failed()`) when `dropped > 0`.

## 5. Held refusals (`HeldRefusal`)

A refusal input made by holding a button. No meter, no timer: the hold lasts as long as the host's
prompt does, and the host ends the prompt with `offerEnded()`.

```
Offered --holdDown--> Holding --offerEnded--> Refused
   ^                     |
   +-----holdUp----------+  (released early: the prompt repeats, repeats++)
Offered --accept()--> Accepted          Offered --offerEnded / defer()--> Deferred (= accepted)
```

* `offer()` starts (or restarts) the prompt. `repeats()` counts early releases so the host can vary
  the prompt. There is no limit.
* **An unheld end accepts.** An offer that ends without a hold, or an explicit `defer()`, resolves as
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
  (a `HeldRefusal` resolved `Refused`). Each boss's lock lists the keys that open it (one or more).
* `offer(key, t, clause)` -> `UnlockResult` (`Unlocked`, `Refreshed` when already open: the open time
  restarts, `WrongKey`, `WrongClause`, `BadKey`). `onDeferred(t)` reports a `Deferred` refusal: it
  counts `deferrals()` and relocks an open lock when `relockOnDefer` (default true).
* Open: `openSeconds` (3.0) after the unlock, or until `relock()` when 0. While open the host opens the
  `CritWindow` (`ArenaBoss.h`) with `until()`; locked, it keeps it closed. `opens()` counts unlocks.
* **Wired to the boss data.** A boss's `weakPoints` block may carry a `lock` (`keys` of `shared_blow`,
  `read_clause`, `refusal`; `clause`; `openSeconds`; `relockOnDefer`), in the boss file or the
  standalone weak-point file. `ArenaBossRun` then owns the lock: `offerUnlock(key, t, clause)` opens
  the lock and the crit window together (`Unlocked`, `CritOpened`), `onDeferred(t)` and `relock(t)`
  shut both (`Relocked`, `CritClosed`), and `onHit` lands no crit while it is locked, even through a
  full intercept's opening. A boss with no `lock` is unchanged.

## 7. The fight session (`FightSession`)

`FightSession.h` owns one of each piece for a fight (the count, the gift slot, a `PlaceTable` per
fighter, the table, the held refusal, its own `WeakPointLock`, and an optional `BossScript` with its
own copy of the boss def) and moves them all on one fight clock. It is what a host holds instead of
the pieces.

* **One clock.** `begin(def, start)` resets every piece at `start`, clamped to +-`kMaxFightStart`
  (1e9 s; further out a double's spacing outgrows a frame's step and the clock would stop moving;
  `FightSessionCheck::start` reports a clamp). `tick(fightDt)`
  is the only time input after that: it adds the step to the session's clock and updates every piece
  with it, every frame. No session call takes a time; presses and blows are stamped with `now()`.
  A step that is negative or not finite counts as 0, and one step is at most `kMaxFightStep`
  (0.25 s; a longer frame loses the rest), so the pieces can never be pushed past `kMaxTimeJump`.
* **Hit-stop.** The shared inputs see fight time, which does not advance during the host's hit-stop:
  the host passes 0 (or only the part of the frame outside the hit-stop) while it holds. So:
  * presses and blows during a hit-stop are accepted, stamped at the frozen time;
  * windows, cooldowns, gift guards, the table's timers, the lock's open time and the boss script's
    steps all stretch in real time by the hit-stop's length;
  * the count's cues fire in fight time, so the host plays each cue when its event arrives and never
    schedules cues ahead on the audio clock.
* **Events.** `tick` returns a `FightTick`: the count's, the table's and the boss script's events
  since the last tick (a call between ticks buffers its events into the next one), the step taken,
  and how many settled count blows were shared blows. The buffers are the pieces' own, not copies:
  read them before the next session call.
* **Glue.** A blow judged a shared outcome (at once in `judgeBlow`, or when a held blow settles) is a
  shared blow (a settle lost to a full count event buffer is not): `BossScript::sharedBlow` with a boss script, else the shared-blow key offered to the session's
  lock (`def.locked`). A refusal held to the end of its prompt (`endRefusal`) offers the refusal key to
  the session's lock; a boss script's weak point opens only through its counters and the shared
  blow, so with a boss script a refusal opens nothing. Every change to a fighter's places through the session re-checks the table when
  that fighter is its setter; after changing places directly (`places(who)` or a load), call
  `recheckTable`. Without a lock and without a boss script the weak point is always open. A boss def
  that `begin` refuses leaves no boss script (`hasBoss` false), so the session's lock rules instead.
* **Presses.** `press(SharedAction, who, target)` maps a host's input action onto the session
  (`Count`, `Gift`, `RefuseDown`, `RefuseUp`, `LeaveOpen`; `SetTable` needs the setter's `Load`, so it
  goes through `setTable`). The host's binding table decides which of its actions make which press:
  the count action makes `Count`, the gift action `Gift` with the chosen lead, and the guard button
  held while a refusal prompt is up makes `RefuseDown`/`RefuseUp`. The typed calls (`callCount`,
  `giveGift`, `layPlace`, `setTable`, `counter`, `bossHit`, ...) return each piece's own result.
* **Story.** With a story state bound (`bindStory(state, registry)`, both or neither), every press
  of the gift button aimed at a lead is one press in it (`storyCountPress`), given or refused (a
  cooldown, a refused count, a hostile or vacant slot): the side is the target lead (`kLeadA` 0,
  `kLeadB` 1), the kind is the selected gift (Heal 0, Guard 1, Count 2; the slot never selects none).
  A press at a non-lead has no side and writes nothing. Hostile and vacant presses sit outside the
  tally's refused slots but still count here. A closed side,
  a counter at its cap or an unbound counter is a normal answer (`lastStoryWrite`). The per-kind press
  counters (counter wides) roll back with a scene restart, so a retried fight counts afresh; a raw
  total does not.
* **Active lead.** `setActiveLead(lead)` names the lead the player's figure fills (`kLeadA` until
  set; it stays across `begin`). The host sets it at fight start and on every lead switch, and aims
  its gift presses, count calls and blow reports for the player at `activeLead()`.
* **Boss counters.** `counter(kind, who, shared)`: `shared` is the host's word that the counter carries
  the shared input (a blow judged a shared outcome, or a hit the lead took half of). An open count
  window alone is not it; a shared counter kind without it is refused (`NeedsShared`).
* The session cannot be copied or moved (its boss script points into its def), and allocates nothing
  after construction.

## What the Unreal wrapper needs

* `UFTFightSession` (`Source/ForgottenThird/FTFightSession.h`) holds a `FightSession` for a host:
  `Begin`, `Tick(FightDeltaSeconds)` (0 during hit-stop), `Press(EFTSharedAction, ...)`, and the story
  binding, which `Begin` drops (bind again after each `Begin`). A host makes one per fight, keeps it in
  a `UPROPERTY`, ticks it every frame, and reads `Get()` and a tick's result within the frame.
* Input actions: `Count` (call), the gift button and a gift select, `LayPlace` (with a person id from
  data), `SetTable`, `Refuse` (a hold). Blows report `(striker, time)` to `StrikeCount::judgeBlow`
  before damage, and their damage through `TableSet::split`.
* **Held damage.** A blow judged `Held` deals no damage yet: the host buffers its damage by `blow` id
  and applies it, times the event's `multiplier`, when that id's `BlowSettled` event arrives. A held
  blow settles within one window (at the latest when the window closes, or on cancel or replace), so
  the buffer holds at most one window's blows. If an update reports `droppedSettles > 0`, the host
  applies every blow still buffered at x1 and clears the buffer: a lost settle never strands damage.
* **Gifts.** The host applies a given gift from the `GiftResult` alone (`healShare`, `healLoad`,
  `guardScale`, `guardUntil`), or asks `guardScaleFor(lead, t)` on each blow the lead takes.
* **The table.** The host calls `TableSet::checkSeats(places, t)` after every change to the setter's
  places (`layPlace`, `leaveOpen`, `emptyPlace`, `clearPlace`, a load) while the table is `Setting` or
  `Set`; `TableSet` does not watch the `PlaceTable` itself.
* Audio and HUD: the count cues (three voices plus a false voice, and a click), the gift threads per
  lead, the place chairs (`Named`, `LeftOpen`, `Emptied`), the table state, the refusal prompt with no
  meter.
* Saves: `GiftSave` and one `PlaceSave` per fighter in the SaveGame object (plain integers).
* Data and host hooks: `GiftSlot::setState`, `PlaceTable::emptyPlace`, the boss's lock keys from its
  data.

## Edge cases the tests cover

Calls during a live count or cooldown; enemy calling a true count; two fighters joining (either
order), the caller alone (light), a partner alone (plain), many held blows all boosted when a partner
joins, blow ids wrapping past 0; a second blow by one fighter; cancel and replace settling held
blows; a cancel after the window closed leaving the cooldown alone; blows on the window's edges
(inclusive, including a blow exactly at the upper edge); a silenced count; false counts early and
late, overlapping a true one, replaced; a late frame passing several cues; full event buffers (lost
events counted, a settle pushing out a cue, lost settles counted); NaN, earlier and junk far-future
times, for every piece; a gift pressed while cooling, with a bad target, vacant, hostile, all
tallied; a count gift with no count, one the count refuses, one at its cooldown boundary; the gift
result's heal and guard fields; tally ties, zero, saturation, save/load round trip, a version 1 save
and bad saves; laying past the rating, the same person twice (named or emptied), emptying an absent
person, cap after emptying, the fall past the cap, a save with one person in two chairs; the table
with two seats, two plus an open chair, not full, interrupted, cancelled, enemy share, unseated
targets, the Load spent by `begin` (and kept on an error), lost table events; the table guttering
when a place empties; the refusal released early (repeats), held to the end, never held (deferred),
held before the offer, a lost button; each lock key, wrong clause, key not in the mask, relock after
time and on deferral; the fight session's clock (a late start, bad and oversized steps, a hundred
thousand hit-stop ticks that expire nothing, 2000 s of ticks after which every piece still answers),
a shared blow opening its lock or a boss script's point, events made between ticks, the table
re-checked when a place empties, gifts written to the story state (unbound, closed side, cap); zero
allocations in every tick and judge call (the test fails if the
allocation hook is missing).
