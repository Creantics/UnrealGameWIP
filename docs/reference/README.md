# Reference docs from the frozen engine

These pages are copied unchanged from newengine (`feature/engine-foundation`, `docs/`). They describe
data formats and pipelines that carry over to Unreal, so they're kept as written. Read their engine
paths through the table below. Anything about the old renderer, joint budget or canonical skeleton
doesn't apply here (see `docs/HANDOFF_UE.md` in newengine).

| Page | What carries over |
|---|---|
| [story_format.md](story_format.md) | The story JSON contract. Read by `ftcore::StoryImport` into `StoryDatabase`. |
| [lipsync_track_format.md](lipsync_track_format.md) | `.nface` tracks and the viseme set. Read by `ftcore::parseFaceTrack` and played by `ftcore::FacePlayer`. |
| [rigging.md](rigging.md), [rigging_plan.md](rigging_plan.md) | Face layering and the measured numbers. Body-rig parts are superseded by Rigify -> IK Rig / IK Retargeter. |
| [briefs/eyes_rigify_addendum.md](briefs/eyes_rigify_addendum.md) | Keep: DEF-only export, flatten bbones, A-pose, one metarig per character, shared base-body topology. Drop: the 128-joint budget, the canonical skeleton, ne_blender, ne_rig_check. |
| [style_packs.md](style_packs.md) | The four world styles. To rebuild as Unreal post-process materials. |
| [vfx.md](vfx.md), [particles_plan.md](particles_plan.md) | The particle module model behind `reference/vfx/*.json`. Rebuild as Niagara. |
| [physics_plan.md](physics_plan.md) | The physics plan behind `reference/physics/`. Rebuild as Chaos. |

## Path mapping

| newengine | here |
|---|---|
| `ne/story`, `engine/src/story/*` | `Core/include/ftcore/StoryDatabase.h`, `StoryRunner.h`, `StorySave.h`, `StoryImport.h` |
| `ne::anim::composeFace`, `FacePlayer`, `parseFaceTrack` (`engine/src/anim/FacePlayer.cpp`) | `Core/include/ftcore/Face.h`, `FaceImport.h` |
| FacePlayer joint output (`applyJoints`) | none: the Unreal owner rotates jaw / tongue_01..03 about local +X by `jawDeg()` / `tongueDeg()` |
| `assets/rig/face_presets.json` | stays in newengine for now; the Core tests read it from there |
| `tools/story_import`, `tools/lipsync`, `tools/vo_direction` | `tools/` in this repo |
