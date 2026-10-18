# Blender: making models by script

Blender **5.0.1** is installed at `C:\Program Files\Blender Foundation\Blender 5.0\` (winget `BlenderFoundation.Blender`, pinned to 5.0.1 because newengine's rig tools were verified on it). Claude drives it headless. Every model is a Python script, so it can be re-run and reviewed like code.

```
blender -b --factory-startup --python-exit-code 1 --python tools/blender/<script>.py -- <args>
```

- `-b` means no window. Never launch `blender.exe` without it from a script. A window must open on the portrait monitor, unfocused.
- Arguments after `--` reach the script in `sys.argv`.
- Scripts run on Blender's bundled Python 3.11, which includes numpy. They don't use the system Python.

## Tools in this repo (`tools/blender/`)

| Script | What it does |
|---|---|
| `ft_sculpt.py` | Scripted sculpting brushes on a mesh's vertices: `draw`, `inflate`, `grab`, `crease`, `flatten`, `trim` (crisp planar cut), `smooth` (Taubin when `keep_volume`) and `noise`. Each applies at a point or along a stroke, with a smooth falloff. `Sculpt.sphere()` gives an even quad sphere to start from. |
| `ft_preview.py` | Renders front, three-quarter and side PNGs with Workbench. `--shape name=weight` sets shape keys first, for checking a face shape. This is how a model gets looked at between passes. |
| `sculpt_boulder.py` | The test sculpt: a boulder. It is sculpted in three passes (big forms, trimmed facets, cracks and breakup) and exported to `Data/Models/test_boulder.glb`. |

![The test boulder](img/test_boulder.png)

## Why scripted brushes

Blender's own sculpt brushes (`bpy.ops.sculpt.*`) need a 3D viewport, and there is none under `-b`. `ft_sculpt` does the same kind of moves directly on the vertex array.

The loop is: sculpt pass, then render, then look, then adjust. It suits:
- props, rocks, terrain pieces and stylised forms
- shape keys
- big forms on characters

Fine hand-sculpted organic detail (pores, cloth folds, a likeness) is still a job for a person in the Blender window, or for a later tool.

## Rigged characters

The rig pipeline was copied from newengine's `tools/blender/` on 2026-10-18. These files are still kept in step with newengine, which remains their source of truth.

| Script | What it does |
|---|---|
| `make_rig_template.py` | Builds `proof_head` on the 87-bone canonical skeleton, with all 56 face shapes, plus its `.expected.json` and `.rigexpect.json`. |
| `make_all.py` | Builds the example assets: the `hero` character, `crate_prop` and the `yard` level. |
| `make_character.py`, `make_prop.py`, `make_level.py` | One example asset each. |
| `rig_spec.py`, `face_spec.py`, `rig_common.py` | The canonical skeleton and face definitions the others share. |
| `rig_validate.py` | Checks a rigged character against the rig rules. |
| `rig_fit.py`, `fit_canonical.py`, `bake_to_canonical.py`, `retarget_maps/` | Fit the canonical skeleton to a character and bake Rigify rigs onto it. |
| `ne_blender.py` | Shared helpers: scene reset and glTF export profiles. |

```
blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_rig_template.py -- --out <dir>
blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_all.py -- --out <dir>
```

Both ran from this repo on Blender 5.0.1. `proof_head.glb` came out at 745,624 bytes, the same size as the copy in `Data/Models/`, with 56 shapes and 87 joints. The export settings for hand-made characters are in [RIGGING_TEST.md](RIGGING_TEST.md#later-characters-blender-export).

## Into Unreal

- **Skinned characters:** use `-run=FTImportCharacter` (see RIGGING_TEST.md). It imports the file and checks the face bones and shapes.
- **Static meshes** like the boulder import through the same Interchange path. A static-mesh import commandlet is still to come.
