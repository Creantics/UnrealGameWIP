## Addendum to the eyes-agent brief: the owner chose Rigify (2026-10-18)

Besides F7–F9, update docs/rigging_plan.md §4 and §9 (no engine code changes needed for this part):
- Rigify is the ONE control rig for every humanoid (our own, generated and external meshes). AccuRIG and Auto-Rig Pro are dropped:
  neither runs headless. Rigify is verified present in headless bpy 5.0.1
  (`uv run --offline --python 3.11 --with bpy==5.0.1 python -I`, enable with
  `addon_utils.enable("rigify", default_set=True, persistent=True)`).
- Measured: the human metarig with the face subtree deleted generates 73 DEF bones, and 52 of them have bbone_segments > 1.
  The bake script MUST set bbone_segments = 1 on every DEF bone (or bbones=1 on the metarig limbs) BEFORE the R-checks run.
  Otherwise Blender deforms more smoothly than the engine's single-joint LBS, so the checks would pass rigs that fail in the engine.
- Rigify face rig excluded (over the 128-joint budget). Faces stay morphs + jaw bone (§7–§8).
- Export DEF-only: flip `export_def_bones` to True in the `rig` profile of tools/blender/ne_blender.py (E14).
  Add a rename table, e.g. DEF-upper_arm.L -> upper_arm_l, to map onto the canonical names.
- Use an A-pose bind: adjust the metarig arms, because Rigify's default metarig is T-ish.
- Metarig placement (decided): assume external, unrigged humanoid meshes from an artist or a store. No landmark detector.
  The metarig is placed once per character by someone with a Blender GUI and saved as assets/rigs/<character>.metarig.json
  (bone head/tail/roll). The headless script reads that file. Every step after it runs in CI:
  generate -> flatten bbones -> auto-weight -> cleanup -> bake to canonical -> export -> validate.
  Prove the whole chain now on hero.glb, writing its metarig.json from make_character.py's joint positions.
- Add one line to docs/art_pipeline.md and the concept-art brief: all humanoid characters share ONE base-body topology,
  so skin weights are authored once and transferred. This must be said before any meshes are commissioned.
- Weights remain the quality lever: automatic weights, then scripted cleanup, then the R6/R7/stretch/volume gate.
- Creatures and the Third's extra hands keep their own path (UniRig / declared chains).
- Optional, if time allows: a headless `tools/blender/rigify_bake.py` (metarig -> generate -> flatten bbones -> bake to canonical -> export),
  with a test on the 21-bone hero fixture.
