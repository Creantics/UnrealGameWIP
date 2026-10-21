# Battle aim and weak points

Aim is a party command next to the skills. Instead of playing a combo, the acting party member aims at
an enemy and the screen shows that enemy's weak points. A shot that lands on a weak point deals bonus
damage.

The rules are in Core (`Core/include/ftcore/WeakPoint.h`, `ftcore::Battle::aim` / `fire` / `cancelAim`).
`UFTBattleComponent` wraps them (`AimAt`, `MoveReticle`, `SetReticle`, `Fire`, `CancelAim`).
`AFTBattleDemoActor` reads the input and runs the autopilot, and `AFTBattleHUD` draws the reticle and
markers on the canvas.

## Rules

- **Cost.** Entering aim spends 2 AP (`AimTuning::apCost`; `FFTBattleTuning::AimApCost`). Cancelling
  before the shot refunds it, and the same member chooses again.
- **One shot per turn.** Aim uses the member's turn: one shot, then the usual recovery.
- **Damage.** An aimed shot is a skill of power 1.4 (`AimTuning::power`), run through the normal damage
  formula. The shot's multiplier then scales it, and it always deals at least 1:

  | Where the shot lands | Multiplier | Hit event label |
  |---|---|---|
  | Inside a weak point (if several overlap, the highest) | that point's multiplier (1 to 4) | `WEAK POINT` |
  | On the body (\|x\| <= halfWidth, 0 <= y <= 1) | 1.0 (`bodyMultiplier`) | `HIT` |
  | Off the body | 0.5 (`grazeMultiplier`) | `GRAZE` |

  A shot reports one `Hit` event carrying that label, and its `beat` is the weak point's index. The
  grade is Perfect, Good or Miss for weak point, body or graze.
- **Time limit.** After 5 s (`timeLimit`) the shot fires on its own wherever the reticle is.
- **Sway.** The reticle drifts on a small fixed Lissajous path: 0.035 x 0.025 figure heights, with
  periods of 1.3 s and 1.9 s. Each shot is judged at its own time (reticle plus sway), so timing
  matters as well as position. A sway of 0 turns it off.
- **Reticle limits.** The reticle is clamped to the body plus a 0.4 margin across, and half that
  margin above and below.

## Figure space

Core never deals in pixels. Its unit is the figure's height:

- x runs across the figure as the camera sees it: 0 is the centre line and screen-right is positive.
- y runs up from the feet (0) to the top (1).

The demo maps this onto each figure with `AFTBattleDemoActor::GetFigurePoint(Id, X, Y)`, which is the
figure's pivot plus (0, X·H, Y·H). The camera looks along +X, so screen-right is world +Y.

## Data

The data lives in `Data/Battle/weakpoints.json`, keyed by the enemy ids of `enemies.json`. It holds
ids and numbers only:

```json
{ "version": 1, "weakpoints": {
    "brute": { "halfWidth": 0.5, "points": [
        { "id": "core", "x": 0.0, "y": 0.55, "radius": 0.12, "multiplier": 2.5 },
        { "id": "knee", "x": -0.28, "y": 0.15, "radius": 0.1, "multiplier": 1.5 } ] } } }
```

- `halfWidth` is in (0, 1] and defaults to 0.5.
- An enemy has 0 to 3 points.
- Point ids are lowercase `[a-z0-9_]`, 1 to 15 characters, and unique per enemy.
- Ranges: x in [-1, 1], y in [0, 1], radius in [0.05, 0.5], multiplier in [1, 4].
- An enemy id that isn't in the roster is only a warning.
- An enemy with no entry has no weak points. Aim still works on it, at the body multiplier.

The battle loads the file after the line-up (`ftcore::importWeakPointTable`, then
`ftcore::applyWeakPoints`). If the file is missing or refused, the battle logs a warning and the
enemies have no weak points. The built-in demo line-up carries the brute and shade points itself
(`ftcore::demo::bruteWeakPoints` / `shadeWeakPoints`), and a test keeps them equal to the file.

Every roster enemy has an entry: brute, shade, wisp, hound, bulwark and lurker.

## Controls

| | Keyboard / mouse | Gamepad |
|---|---|---|
| Enter aim | the **Aim** row of the menu, or F, or the right mouse button | the Aim row with A, or LT |
| Move the reticle | the mouse, or hold the arrows / WASD | either stick |
| Fire | left mouse button, Enter or Space | A |
| Cancel (refunds the AP) | Esc, Backspace or the right mouse button | B or LT |

The mouse moves the reticle by its own distance on screen. The demo divides the mouse delta by the
target's projected figure height. Sticks move the reticle at up to 0.8 figure heights per second and
held keys at 0.6. While aiming, the arrows don't step or dodge.

## HUD

While the member aims, the HUD shows:

- each weak point as a pulsing orange ring with its multiplier ("x2.5"), which turns gold when the
  aim point is inside it;
- the reticle: a ring and cross where a shot fired now would land (gold on a weak point, white on
  the body, grey for a graze);
- a timer bar, and the controls.

The damage number of a weak-point shot is gold and larger, for example "WEAK POINT  -70". A graze
reads "GRAZE  -14".

## Autopilot

`ftcore::demo::pilotChoice` aims when the member can afford it and a shot at the target's best weak
point (aim power times its multiplier) beats the best affordable skill played as a full combo (power
times 1 + fullComboBonus).

- **Headless** (`playHeadless`, `pilotShoot`): the pilot puts the aim point exactly on the weak point
  with `AimShot::reticleFor` and fires the same frame.
- **Unreal** (`AFTBattleDemoActor::TickAutoPlay`): the pilot glides the reticle onto the point for
  0.6 s of battle time so the shot can be seen, then sets it exactly and fires at that time
  (`UFTBattleComponent::FireAtTime`).

A pilot that never presses also never aims, so the balance gate's "no presses must lose" run is
unchanged. With presses, the default fight is now won in 9 turns, 5 of them aimed shots (all on a
weak point). Every encounter in the balance gate is won with aimed shots in the mix.

## Tests

`Core/tests/test_weakpoint.cpp` covers:

- the judgement (weak point, overlap, body and graze, including edges);
- the sway and clamp on a fake clock, and that aiming is allocation-free;
- the battle damage of each kind;
- refusals, cancel and refund, and the time limit;
- parser errors and warnings, and `applyWeakPoints` ordering;
- the real data file, which must match the demo;
- a headless win with aimed shots.
