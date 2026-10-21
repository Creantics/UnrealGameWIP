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
| `prep_scan.py`, `rig_scan.py` | Take a generated or scanned character mesh to a rigged lead glb (see below). |

```
blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_rig_template.py -- --out <dir>
blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_all.py -- --out <dir>
```

Both ran from this repo on Blender 5.0.1. `proof_head.glb` came out at 745,624 bytes, the same size as the copy in `Data/Models/`, with 56 shapes and 87 joints. The export settings for hand-made characters are in [RIGGING_TEST.md](RIGGING_TEST.md#later-characters-blender-export).

Blender 5.2's glTF exporter writes each bone's children sorted by name, not in creation order. That breaks the canonical joint order that `check_glb` and `rig_validate` (R12) require. `ne_blender.export_rig` keeps creation order for the duration of the export, by swapping the exporter's `_sort_by_name` out and restoring it afterwards. The swap is skipped on exporters that don't have that function. newengine's copy of `ne_blender.py` doesn't have this fix yet.

The lead scripts can also build inside a running Blender (through the Blender MCP add-on, for example). `reset_scene` calls `read_factory_settings`, which a live session can't run, so replace `make_leads.nb.reset_scene` with a function that deletes the scene's objects and data before calling `build_character`.

## Generated or scanned characters

A character mesh from an image-to-3D service or a scan arrives as one dense textured mesh with no rig. Two scripts take it to a lead glb:

1. **`prep_scan.py`** orients the mesh so it faces -Y, scales it to `--height`, puts its soles on z = 0 and brings it down to game size. It has two ways to cut the mesh down:
   - The default decimates the mesh to `--faces` triangles and shrinks its textures to `--tex`.
   - `--retopo QUADS` builds a clean mesh instead. A voxel remesh closes the scan into one shell, then QuadriFlow lays about QUADS quads over it, mirrored across x = 0. Smart UV project and pack give it new UVs, and Cycles bakes the full scan onto it: base colour, ORM and a tangent-space normal map.

   Both ways first weld coincident vertices. glTF splits a vertex at every UV seam, so a mesh with a per-triangle UV atlas (a projected texture) arrives as loose triangles, which the decimate collapses to nothing and the voxel remesh can't close. For a mesh grafted from overlapping parts (body, head, hands), the decimate keeps the parts' own textures and is the better choice. With `--retopo`, the remesh keeps only its largest shell, because overlaps leave closed voids behind. If QuadriFlow still refuses the mesh as non-manifold, the script decimates the voxel mesh to `2 * QUADS` triangles instead and says so.

   `--grid DIR` renders orthographic front and side views on a 5 cm grid, for reading off the landmarks. `grid.json` gives the pixel-to-metre mapping. The glTF importer leaves objects in quaternion rotation mode, so the script switches to XYZ before it turns the mesh.
2. **Landmarks:** the 14 joints `fit_canonical.py` needs, read off the grid views, go into a JSON file of `{name: [x, y, z]}` in metres. `_l` is +x, the character's left.
3. **`rig_scan.py`** runs `fit_canonical` (joint fit and skinning), then swaps its `tpose` clip for `make_leads`' idle and walk. That gives the same `idle`, `rom` and `walk` clips as the other leads. It checks the result with `make_leads.check_glb(face=False)`.
   - `--skirt HEM,TOP,FOLLOW,HALF_WIDTH` reweights a tunic or skirt so it doesn't tear between the legs. Between the two heights, the weight blends from the pelvis to the thighs, with a soft left/right split. Vertices beyond the half width (the hands) keep their weights.
   - When heat weighting leaves more than 5% of the vertices empty, the weights come from a voxel proxy instead (`rig_common.proxy_weights`). It voxel-remeshes a copy at 4 mm, which keeps the fingers apart, heat-weights that, and carries the weights back from the nearest proxy face. Heat weighting fails on a mesh grafted from overlapping parts; the log then says `weights carried from a N-vertex voxel proxy`.
   - `--idle-arm` and `--walk-arm` count from the scan's own arm angle. A T-pose scan needs about -62 and -64 to end where an A-pose scan does with the defaults.
   - `--preview DIR` renders the rest pose in x-ray with the landmark joints marked, plus two walk frames.
   - Add `neck_02`, `head`, `jaw`, `eye_l` and `eye_r` to the landmarks if the face will get shapes. Without them the head joints are extrapolated from the neck and land too high.
4. **`face_transfer.py`** (optional) gives the rigged mesh the proof head's 56 face shapes and a jaw mask. It takes rig_scan's `.blend`:

   ```
   blender -b RIG.blend --factory-startup --python-exit-code 1 --python tools/blender/face_transfer.py --        --face face.json --out name.glb [--blend name_face.blend] [--preview DIR]
   ```

   - `face.json` holds the scan's side of a dozen correspondences: eye centres, mouth corners, lips, nose tip, chin, brows, crown, back, sides and neck. They're read off the grid crops. A thin-plate spline warps the proof head onto the scan, and each vertex takes the shape deltas of the nearest point on the warped proof skin.
   - A `"jaw"` entry (a cut through the mouth slit that rises to a pivot under the ear) replaces the transferred jaw mask. Use it when the scan's mouth is much wider than the proof's.
   - A scan has no eyeballs or tongue. The eye-look shapes move only the lids, and tongueOut pushes the lower lip forward, because Unreal drops an all-zero morph target.
   - The mouth of a scan is closed, so the jaw stretches the teeth between the lips when it opens wide. Speech levels (jawOpen about 0.4) look fine.
5. **`attach_rigid.py`** (optional) joins parts that can't go through the voxel remesh or heat weighting, such as hair cards and glasses, into the rigged mesh. It takes rig_scan's or face_transfer's `.blend`:

   ```
   blender -b RIG.blend --factory-startup --python-exit-code 1 --python tools/blender/attach_rigid.py --        --part hair.glb:head [--part glasses.glb:head] --out name.glb [--face 1] [--blend name.blend]
   ```

   - Each part must already sit in prep_scan's frame. Every vertex of it is weighted fully to the named joint, and it keeps its own materials.
   - Pass `--face 1` after face_transfer, so the check expects the 56 shapes.
6. Import with `FTImportCharacter -Role=lead`, adding `-Face=0` if step 4 was skipped. A lead's textures must be 2048 px or smaller, so run prep_scan with `--tex 2048`. `-Source` takes an absolute path, so a private model can stay outside the repo. Then run `FTBuildRig` and `FTBuildLeadsWorld`. From Git Bash, set `MSYS_NO_PATHCONV=1` first, or it turns `-Dest=/Game/...` into a Windows path.

## Into Unreal

- **Skinned characters:** use `-run=FTImportCharacter` (see RIGGING_TEST.md). It imports the file and checks the face bones and shapes.
- **Static meshes** like the boulder import through the same Interchange path. A static-mesh import commandlet is still to come.
