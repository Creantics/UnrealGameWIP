# Cinematic prompt words

A vocabulary of weighty words for revising scripted prompts: cinematics, shots, renders, art and agent prompts. Pick a
few precise words from the sections that matter for the shot rather than stacking many; one tone, one look, one light.
Names of real artists, studios or franchises are for prompts only and never go into shipped text.

## Cutscene pipeline

The process for making cutscenes. The word lists below are step 2.

1. Caffeine.
2. A compiled list of impactful words in a markdown document (e.g. cinematic, volumetric lighting): this file.
3. A compiled list of general guidelines in a markdown document. Examples: use existing 3D assets; prefer high poly
   over low poly; never hand-roll assets on your own; you are allowed to use 1 sub-agent for any task. Essentially,
   put steering lines in prompts to keep away from what you do NOT want (like low poly, which the user hates).
4. Have a general idea of what you want, in rough human form, spelling mistakes included ("I want a cyberpunk alley").
5. Spin up ChatGPT or Claude, give it the rough prompt, and tell it to adapt it against the impact-word list, expand
   it, add detail and apply the guidelines document.
6. Run it again and have it adapt the prompt to the game engine (here Unreal 5.8; Godot 4.7 for Godot work).
7. Give it to Claude or ChatGPT and let it spin up the agents, set up the folders and launch each agent in it.

Tools:

- Game engine: Unreal 5.8 (Godot 4.7 for Godot work).
- Blender 5: 3D modeling and tweaks.
- Poly Haven: 3D assets.
- Sketchfab: 3D assets.
- The internet: always search for an asset if it can't be found on the sites above.
- Muxer: the user's own multiplexing terminal. Agents are started in a terminal the user can watch, not as sub-agents
  of another agent, since agents behave differently when started by an agent than when they think a human started
  them.

The guidelines document from step 3 doesn't exist yet; write it before the first cutscene.

## Tone
cinematic, moody, ethereal, gritty, whimsical, ominous, serene, epic, melancholic, nostalgic, dreamlike, surreal,
uncanny, eerie, cozy, tranquil, foreboding, haunting, majestic, intimate, claustrophobic, desolate, triumphant,
playful, somber, tense, euphoric, otherworldly

## Aesthetic
liminal, brutalist, vaporwave, cyberpunk, solarpunk, art deco, art nouveau, ukiyo-e, lo-fi, dieselpunk, steampunk,
atompunk, biopunk, analog horror, backrooms, retro-futurism, cassette futurism, biomechanical, holographic, synthwave,
outrun, Y2K, frutiger aero, dreamcore, weirdcore, cottagecore, gothic, baroque, bauhaus, minimalist, maximalist,
Scandinavian, wabi-sabi, mid-century modern, industrial, post-apocalyptic, dystopian, utopian, mythic, eldritch,
Lovecraftian, noir, neo-noir, spaghetti western, space opera, used future, tropical, arctic

## Color / grading
monochrome, duotone, desaturated, high contrast, low contrast, pastel, neon, muted palette, earth tones,
complementary colors, analogous palette, limited palette, teal and orange, bleach bypass, sepia, cross-processed,
warm grade, cool grade, color graded, LUT, crushed blacks, lifted blacks, HDR, filmic

## Lighting / render
volumetric, chiaroscuro, rim lighting, subsurface scattering, bokeh, tilt-shift, caustics, ambient occlusion,
screen-space reflections, SSAO, SSIL, SDFGI, VoxelGI, lightmapped, reflection probes, anamorphic, dithering, parallax,
long exposure, PBR, global illumination, HDRI-lit, image-based lighting, ray-traced, path-traced, three-point lighting,
softbox, studio lighting, raking light, emissive, specular highlight, fresnel, light bleed, color bleed,
contact shadows, soft shadows, hard shadows, cascaded shadows, golden hour, blue hour, midnight, overcast, high noon,
backlit, silhouette, underlit, top-lit, practical lights, light linking, light cookies, gobo, bloom, glow, lens flare,
chromatic aberration, vignette, film grain, lens dirt, volumetric fog, god rays, crepuscular rays, light shafts,
depth of field, atmospheric perspective, ACES tonemapping, exposure, auto exposure

## Materials
weathered, worn edges, brushed metal, polished chrome, anodized, galvanized, hammered metal, translucent, frosted,
glass, crystal, iridescent, pearlescent, holographic foil, clearcoat, matte, satin, glossy, procedural, trim sheet,
tri-planar, patina, verdigris, oxidized, rusted, tarnished, wet, rain-slicked, damp, mossy, velvet, sheen, leather,
fabric, woven, ceramic, porcelain, terracotta, concrete, marble, granite, sandstone, wood grain, charred, burnt,
cracked, chipped paint, peeling, anisotropic, displacement, normal mapped, parallax occlusion, decals, vertex paint,
dust accumulation, grime pass, micro-surface detail, edge wear, cavity map, curvature map, roughness variation

## Modeling
low-poly, high-poly, mid-poly, sculpted, hard-surface, organic, subdivision, bevelled, chamfered, filleted, kitbash,
greebles, nurnies, quad topology, clean topology, edge loops, support loops, watertight, manifold, panel lines, seams,
boolean, lattice, deform, retopology, photogrammetry, scanned, parametric, procedural geometry, CSG, instancing,
scatter, MultiMesh, array, radial array, mirrored, symmetrical, modular kit, blockout, greybox, silhouette-readable

## Style
photorealistic, hyper-detailed, octane render, unreal engine, 8K, hand-drawn, painterly, impressionistic, watercolor,
ink wash, cel-shaded, toon-shaded, outlined, inverted hull, flat shaded, smooth shaded, claymation, clay render,
stop-motion, diorama, voxel, pixel art, PS1, N64, low-fi 3D, affine texture warping, vertex snapping,
stylized realism, blueprint, wireframe, low-poly faceted, hand-painted textures, miniature, tabletop, plastic toy,
vinyl, paper craft, origami, cardboard, felt, knitted, Ghibli-like, storybook, comic book, halftone, risograph,
sketch, graphic novel, chibi, anime, rubber hose, Saturday morning cartoon

## Composition / camera
dutch angle, low angle, high angle, bird's eye, worm's eye, overhead, top-down, side-view, over-the-shoulder,
first-person, third-person, POV, rule of thirds, symmetrical, centered, leading lines, framing within a frame,
establishing shot, wide shot, medium shot, close-up, extreme close-up, macro, telephoto, wide angle, fisheye,
isometric, orthographic, turntable, exploded view, cutaway, cross-section, foreground framing, negative space,
hero prop, focal point, scale reference, dolly, tracking shot, crane shot, orbit, push in, pull out, handheld,
camera shake, rack focus, static camera

## Scene / environment
overgrown, reclaimed by nature, abandoned, derelict, ruined, decaying, pristine, lived-in, cluttered, sparse,
monolithic, cavernous, vast, cramped, labyrinthine, vertical, terraced, floating islands, underground, underwater,
subterranean, interior, exterior, set dressing, environmental storytelling, skybox, backdrop, matte painting,
horizon line, vista, landmark

## Weather / atmosphere
rain, drizzle, downpour, snow, blizzard, fog, mist, haze, smog, dust storm, sandstorm, embers, ash fall, fireflies,
floating particles, motes, pollen, sparks, steam, smoke, heat shimmer, aurora, starfield, nebula, storm clouds,
lightning, puddles, wind-swept

## Animation
secondary motion, follow-through, overlapping action, anticipation, squash and stretch, ease in, ease out, arcs,
timing, spacing, staging, exaggeration, appeal, smears, hold frames, stepped animation, on twos, seamless loop,
ping-pong, idle animation, walk cycle, run cycle, blend tree, state machine, inverse kinematics, ragdoll,
physics-driven, simulated, cloth sim, soft body, rigid body, procedural animation, tweened, keyframed, motion blur,
bouncy, springy, snappy, floaty, weighty

## Godot-specific
WorldEnvironment, Environment resource, CameraAttributes, DirectionalLight3D, OmniLight3D, SpotLight3D, FogVolume,
GPUParticles3D, CPUParticles3D, MultiMeshInstance3D, CSG nodes, GridMap, AnimationPlayer, AnimationTree, Tween,
ShaderMaterial, StandardMaterial3D, ORMMaterial3D, visual shader, spatial shader, canvas_item shader, next_pass,
SubViewport, Forward+, Mobile renderer, Compatibility renderer, autoload, signals, @export, @tool

## Pipeline
game-ready, baked normals, baked AO, baked lighting, LOD, occlusion culling, frustum culling, rigged, skinned,
blend shapes, modular, instanced, non-destructive, texel density, UV unwrapped, UDIM, atlased, draw call optimized,
batched, Nanite, virtualized geometry, real-time, offline, 60fps target, performant, scalable, glTF, import-ready

## Code / agent
idiomatic, minimal, production-ready, defensive, terse, exhaustive, self-contained, single-file, modular,
data-driven, typed, documented, deterministic, extensible, decoupled, no placeholders, runnable, verified,
iterate until correct
