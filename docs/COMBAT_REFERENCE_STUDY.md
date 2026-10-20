# Combat reference study: reactive defence, cues, aim

Hands-on study of a reference turn-based RPG whose enemy turns use real-time
defence. Mechanics only: no premise, names, story or set pieces carry over.
The point is to learn which techniques make reactive defence readable, and
how to build our own fight sequences and audio around them.

## Mechanics observed

**Turn structure.** Party turns use a command menu: Attack, Skills, Items,
Aim, Flee and Skip Turn. Skip Turn needs a hold of about 2 s, so a tap does
nothing. Enemy turns hand control to the player for defence.

**Defence inputs.** Three reactive inputs during enemy attacks: dodge,
parry and jump. Each must be pressed *before* the hit lands. Pressing on
the impact itself is too late.

**Attack rhythm is fixed per attack.** The same enemy action replays its hits
at the same offsets every time, within about ±0.07 s once anchored to the
start of the attack. Some actions are multi-hit combos, with each hit needing
its own input. Learning a fight means memorising each attack's beat.

**Cues, from latest to earliest.**
- *Impact sound* (broadband high-frequency burst). It fires about 0.05 s
  before the damage number, which is too late to react to. It confirms a hit
  rather than warning of one.
- *Windup ramp.* All frequency bands rise steadily for about 0.3 s before
  impact. An earlier-firing detector gains only about 0.1 s and triggers 3–4
  times as often on harmless sounds.
- *Tell click.* Some attacks (about a third measured) have a sharp transient
  about 0.35–0.45 s before impact. It's attack-specific, not universal.
- *Visual windup.* The attacker closes distance for about 1 s, then a short
  white flash marks the impact. This is readable to a player who knows the
  attack.
- Non-attack actions such as buffs ("applies Rush") make loud sounds too.
  Sound alone doesn't separate threat from noise; the camera and the
  attacker's pose do.

**Measured effect.** Pressing dodge on the impact sound, compared with no
defence, raised survival from about 102 s to about 129 s (two fights each).
The gain probably comes from catching later hits in combos.

**Defeat flow.** Defeat offers "send in reserve team?" and then "retry
battle?". A retry restores the HP the party had when the battle *started*.
Declining returns the party to the map, restored.

**Aim.** Right mouse enters free aim: over the shoulder, with a reticle that
turns red and shows the target's HP bar when it locks onto a target. Left
mouse fires, which costs a resource and does light damage. Some enemies have
floating weak-point orbs to shoot. Firing can trigger passives such as
"first strike" bonuses. Right mouse backs out.

## Techniques to borrow

1. **Every attack has a fixed, learnable rhythm.** Author hits as timeline
   events at fixed offsets from the attack start. Mastery comes from
   memorising those offsets, not from fast reflexes.
2. **Layer the warnings.** Each attack gets a windup pose, a tell sound
   about 0.4 s before impact, and an impact sound that is feedback only.
   Never make the impact sound the only cue.
3. **Separate threat sounds from flavour sounds.** Buffs and taunts must not
   share the tell's sonic signature.
4. **Use a different window per input.** Dodge gets a wide window, parry a
   narrow one, and jump is reserved for specific attacks. This gives easy
   safety and a hard reward.
5. **Retry means practice.** Restore the party's HP from the start of the
   fight and send the player straight back in, so learning a fight is cheap.
6. **Aim as a free-target action.** Weak points on the enemy model reward
   precision outside the menu.

## Implications for our build

- Core combat needs per-attack hit timelines (offsets plus a cue for each
  hit) and a defence resolver that judges each input against a window ending
  at impact. All of it is plain C++ in `Core/`, unit-testable with a fake clock.
- Audio direction: every enemy attack needs a distinct tell about 0.4 s
  before impact. Impact sounds are feedback only.
- A headless autoplay can drive defence from the timeline directly, so it
  never has to listen to audio.

## Second session: aim, weak points, defence probes

**Aim has soft lock-on.** Once the reticle nears a target it snaps on and
shows an X marker plus the target's HP bar. Near a target, raw mouse input no
longer maps linearly to reticle movement. Precision isn't the test: a small
nudge is enough. Each shot costs one action point. Aim stays open after a
shot, so shots can be chained, and the damage number pops at the impact point.

**Weak points are one-time bombs, not multipliers.** One enemy carried a
visible destructible part that could be read from the normal command camera
before aiming. A body shot did 78 damage. Shooting the part blew it up:
- About 4x that damage to its carrier, and about 15x to the enemy beside it.
- A burn status on both.
- An extra on-hit passive fired.

The part is then gone. Its value comes from sitting next to other enemies,
which rewards spending one point on the first turn to scan the arena.

**Passive procs are announced.** Each time an equipped passive fires, a small
chip with its name and a count stacks above the party HUD. The player learns
which build pieces are working without opening a menu.

**Every enemy action has a banner.** A one-line caption at top centre names
each enemy action: attacks, buffs on allies, status ticks and named specials.
The banner is the universal "something is coming" cue. The camera then
frames the targeted party member with the attacker beside or behind them.

**Telegraph length scales with the attack.** A melee swing landed about
1.8–2.2 s after its banner. A summoned strike landed about 5.4 s after its
banner. For the last 2.5 s, a glowing vertical column marked its target,
visible from any camera angle.

**Dodge is forgiving, parry is not.** Mashing dodge every 0.2 s through an
enemy phase produced some perfect dodges: a "PERFECT" caption over the
defender plus an action-point pip. Other hits still got through, including
the slow summoned strike. Mashing parry every 0.1 s gave no perfects in four
hits, so parry has a narrower window, a mash lockout, or both. Perfect
defence refunds action points, which ties defence skill back into the turn
economy.

**Skills can spend marks instead of points.** One skill card listed its cost
as two elemental marks and zero action points. The marks are left on targets
by other skills, so they act as a combo currency across characters.

**Failure flow, timed.**
1. Party wipe.
2. "Send in reserve team?" A spare character then fights alone.
3. A failure card of about 7 s.
4. "Retry battle?" Retry restarts the fight from its first turn with full HP
   and no load screen.

Downed allies show a red slash over their portrait.

### More techniques to borrow

7. **Announce every enemy action in one caption line.** Name threats and
   flavour actions alike, so the player never wonders what just happened.
8. **Scale the telegraph to the attack.** Slow, big attacks get a long marker
   on the target in the world. Fast attacks rely on the windup pose.
9. **Weak points as positional bombs.** A shootable part that explodes for
   area damage and a status effect rewards reading the formation, not aim.
10. **Make perfect defence pay into the turn economy.** Refund a point on a
    perfect dodge or parry, so defence skill turns into offence.
11. **Punish mashing on the hard input only.** Let the safe input tolerate
    some mashing, and give the rewarding input a lockout.
12. **Surface passive procs.** Name each proc briefly as it fires.
