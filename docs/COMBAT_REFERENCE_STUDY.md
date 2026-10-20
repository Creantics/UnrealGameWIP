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
