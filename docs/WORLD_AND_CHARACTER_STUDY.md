# World and character reference study

Hands-on study of the same reference RPG as `COMBAT_REFERENCE_STUDY.md`,
covering world structure, traversal, character presentation, weapons and
menus. Only techniques are recorded here. No premise, names, places, story or
set pieces carry over.

## World structure

**Two layers.** The world has two scales:
- An overworld where the party is small against huge terrain.
- Full-scale zones entered from the overworld.

The miniature layer makes a large world quick to cross and keeps zone
entrances readable from far away.

**Miniature look.** The camera on the overworld sits high and behind, with a
wide field of view, and keeps the party in the lower third of the frame. On
top of that it uses strong haze and a tilt-shift-like depth of field on
distant terrain. Landmarks fill most of the frame.

**Skyline anchors.** Giant floating rock slabs and frozen debris form
silhouettes that are visible from anywhere. They serve as navigation
landmarks, and each zone has its own distant silhouette.

**Water gates traversal.** The landmass is a central plateau ringed by coast
and satellite islands. Areas across water open up later with a mount or
vehicle.

**Map as an artefact.** The full map looks hand-inked on parchment:
- Contour hatching and painted terrain icons.
- Fog of war drawn as soft painted cloud over unexplored areas.
- Each biome has its own colour (snow, meadow, autumn, sea, coral).
- Locations are small illustrated vignettes rather than glyphs.

It zooms from local to the whole landmass. The current objective is one line
pinned top-left, with a single reticle marker on the map.

**Encounters are visible.** Single large monsters stand or patrol in open
ground near props such as ruins or a wreck. Size on the miniature layer
signals threat. Striking a monster on the overworld starts the fight with a
first-strike bonus. The fight then cuts to a separate staged arena rather
than taking place where you engaged.

## Traversal feel

- **Followers.** Party members trail the leader in a loose line. They path
  after the leader with a slight lag and never block movement.
- **Gaits.** Walking is the default. Run is a held key with a clearly
  different stride. Starting has a short acceleration, and stopping ends with
  a settle step.
- **Camera yaw smoothing.** Yaw eases out over about 0.2 s after input stops,
  with no overshoot.
- **Walls.** Scenery blocks movement outright, with no auto step-around. The
  camera pushes in close when the player is against a wall.

## Camp and rest points

- **Camp.** One key teleports the party to a night camp at full scale. The
  camera becomes a close over-the-shoulder view, offset left of centre.
  Lighting contrasts warm firelight with a cool moonlight rim. Characters read
  by rim light against dark backgrounds rather than by flat fill.
- **Leaving camp.** You leave by sleeping at the fire, chosen from a short
  dialogue-style list. Other entries check on party members, show the
  objective or open the journal.
- **Rest points.** A planted, glowing banner offers three things:
  - Rest (refill).
  - Spend attribute points.
  - Learn skills.

  Spending progression only at rest points ties levelling to safe places.
- **Load screens.** Each shows one mechanic tip on black inside an ink-stroke
  frame.

## Character presentation

**Proportions.** Adults are realistically proportioned, about 7.5 heads tall,
with realistic faces. Detail is densest at the torso and collar, where the
camera spends its time. Line-art tattoos on skin mark magic users.

**Group versus individual.** All party members share a faction uniform in
one palette. Outsiders wear distinct dark palettes. Individuality comes from
three things:
- Silhouette: long coat, slit skirt, or shorts with boots.
- Hair.
- One accent colour per character.

**Signature idle poses.** The party screen shows everyone full-body on black
under one top spotlight with a rim light. Each character holds a pose that
works as personality shorthand: hand on chin, hands behind the back, weight
on one hip, arms loose and guarded. It's a cheap, strong piece of
characterisation.

**Portraits.** The character page centres on a large face close-up lit like
a film still.

## Weapons and build

- **Roster per character.** Each character has their own weapons, from 3 to
  about 8 at this point. Names share a suffix per character, so ownership is
  clear at a glance.
- **Live preview.** In the menu, the character model holds the highlighted
  weapon in a pose specific to its class: a sword held across the body, or a
  sabre raised en garde.
- **Weapon card.** Each card shows:
  - Power and an element.
  - Two attribute scaling grades, as letters.
  - Three perks that unlock at weapon levels 4, 10 and 20. Locked perks show
    a padlock, so the upgrade path is visible up front.

  Hovering shows a side-by-side comparison.
- **Perks build on status effects.** Perks are built on status effects (crits
  apply burn, attacks stack burn, crit bonuses that depend on stance). This
  pushes each weapon toward a playstyle loop rather than raw numbers.
- **Scaling letters on the attribute radar.** The character page shows five
  attributes on a pentagon. Each attribute is annotated with the equipped
  weapon's scaling letter for it, which ties weapon choice to point
  allocation.
- **Passive budget.** Each character has 3 equipped items, a grid of 6
  skills, and a point budget for extra passives learned from items.

## Techniques to borrow

1. **Two-layer world.** A miniature overworld for travel, with full-scale
   zones for play. It's cheaper to build and reads clearly.
2. **Skyline landmarks.** Give every zone a silhouette that's visible from
   the overworld.
3. **Map as a crafted object.** Ink style, painted fog of war, biome colours
   and illustrated location icons.
4. **Signature idle poses** for each character on menu screens.
5. **Shared palette, individual silhouette.** Show group identity through
   colour and individuality through shape plus one accent colour.
6. **Light characters with a rim light.** Use warm key and cool rim light at
   night rather than flat fill.
7. **Make weapon perks visible but locked.** Showing what upgrades will
   unlock gives a reason to level.
8. **Scaling letters next to stats.** Gear choice and stat allocation then
   explain each other.
9. **Progression only at rest points.** Safe places become decision points.
