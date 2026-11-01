# Beat boss demo: fight animation clips

Real animation clips for the beat boss demo's two fighters, one `UAnimSequence` per id the choreography layer
schedules. Both fighters share the **test human's skeleton** (`/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_human`,
the mesh the demo's lead already uses). The boss is meant to be the same body scaled up (about 1.35x) with its own
material; scaling and the material are the host's job, the clips are retargeted for the unscaled body.

| Folder | Ids |
|---|---|
| `/Game/Combat/BeatAnims/Lead` | `lead_stance` `lead_jab` `lead_cross` `lead_kick_r` `lead_counter` `lead_finisher` `lead_block` `lead_parry` `lead_dodge_l` `lead_jump` `lead_hitreact` `lead_hitreact2` `lead_downed` `lead_victory` |
| `/Game/Combat/BeatAnims/Boss` | `boss_stance` `boss_windup` `boss_hook` `boss_smash` `boss_sweep` `boss_thrust` `boss_grab` `boss_roar` `boss_hitreact` `boss_stagger` `boss_defeat` |

Each asset is named after its id. `Data/Anim/beat_clips.json` holds ids, asset paths and numbers only:
`id`, `asset`, `length_s` (the sequence's play length), `impact_s` (the time of the blow or contact; 0 for idles, loops and
clips with no blow) and `loop`.

## How they were made

1. The clips were generated with a local text-to-motion model and are kept outside the repository. Each clip starts
   (and, for most, ends) on the fighter's own stance pose.
2. One glTF holds the skinned source body once and every clip as its own animation, so one import gives one skeleton
   and one IK rig for all of them (the Unreal importer names each clip `<file name><animation name>`).
3. `FTBuildRig` builds the source IK rig, then retargets onto the test human. `-Names` and `-Out` (new) move each result
   to `<Out>/<name>` instead of leaving it as `<clip>_to_<mesh>`:

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportCharacter -Source=<beat.glb> -Dest=/Game/Trial/Kimodo/beat -Face=0
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildRig -Mesh=/Game/Trial/Kimodo/beat/beat/SkeletalMeshes/beat -Dest=/Game/Trial/Kimodo/beat/Rig
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildRig -Mesh=/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_human \
    -Dest=/Game/Trial/Kimodo/beat/ToTestHuman -RetargetFrom=/Game/Trial/Kimodo/beat/Rig/IK_beat \
    -Clips=<source clip>,<source clip>,... -Names=Lead/lead_jab,Boss/boss_hook,... -Out=/Game/Combat/BeatAnims -AlignPose
```

The source import, the source rig and the retargeter live under `/Game/Trial` (local, not committed); the committed
clips reference only the test human's skeleton.

## Timing

A retargeted clip plays 1/30 s later than its source clip's own time line (the first key is held for one frame), so
`impact_s` is the contact time found in the source clip plus 1/30 s. The contact is the pose of full extension of the
striking hand or foot just after its speed peak; for a block it is the forearm's rise, for a hit reaction the head's
snap, for a dodge the furthest the body travels sideways, for a jump the apex. In the engine the peak hand and foot
speeds of the jab and kick land on the source's frames. Three clips were set from their in-engine bone tracks instead,
where the fastest-end rule picks the wrong stroke: the cross pumps twice (the first, which is also the speed peak, is
used), the hook's blow is where the fist crosses the body's centre line, and the smash's is where the hands reach the
bottom of the downward stroke.

## Checking a retarget

`FTBuildRig -ComparePose=<source>,<clip>` compares joint angles when the source has the target's bone names. The
source body here has its own names, so the check was made with a Python commandlet that measured elbow and knee bend
(0 = straight) and the shoulder line's turn on each source clip and its retarget at 13 times: the bends stay within
about 3 degrees of the source (nothing is straightened) and the shoulder turn within about 10 degrees.
