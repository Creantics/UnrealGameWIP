# Plan: from Core library to first playable

State on 2026-10-18 (origin/main at 4ae8eb4). The critical path to a first playable is **the face decision (A1), then the first map (C1), then the battle and framework wrappers (B1, B2), then a real character (C2)**. Nothing has run inside Unreal yet. Everything is verified only by the UBT compile and 105 Core tests.

## Where things stand

**Core (plain C++20, tested outside Unreal):**
- Battle, TurnQueue
- ComboGenerator / ComboSequence
- Conductor and Calibration
- Face (composeFace, FacePlayer, .nface) and FaceImport
- Emotion (draft)
- Json
- Story: Database, Import, Runner, Save

The "dialogue loop" that the handoff lists is already covered by StoryRunner's dialogue exchanges (miss options). newengine has no separate loop to port.

**Unreal module:** three wrappers, each compiled but never run:
- `UFTStorySubsystem`
- `UFTFaceComponent`
- `UFTConductorComponent`

**Data:**
- `Data/Rig/face_presets.json` is a copy of newengine's `assets/rig/face_presets.json`. Until the F10 face review, the newengine file is the source of truth. If it changes there, copy it over again.
- `Data/Rig/emotion_map.json` is a draft with unreviewed weights.

## Decisions

From 2026-10-18, the advisor makes these calls and the user steps in only to change one.

| # | Decision | Ruling (advisor) |
|---|---|---|
| A1 | Faces | Stylised characters with ARKit-52 shapes. Lip-sync tracks map directly. |
| A2 | CI | The user fixes the GitHub spending limit when convenient. Until then, the local gate (ctest plus UBT) is the authority. |
| A3 | Save format | A Core JSON text codec for `StorySave`. The Unreal `USaveGame` stores that string. |
| A4 | Dialogue | The critique loop on 04b, 04c and 04e is story work, so it's deferred out of the porting and rigging-test push. VO waits until all five parts are final. |
| A5 | Remote URL | Leave it, since redirects work. |
| D | Python | Install Python 3 now (winget). |
| — | Merging | Session branches merge into main after their diff is read and the gate passes. |
| — | Editor launches | Only `UnrealEditor-Cmd -run=<Commandlet>`, never the interactive editor unattended. Always run it in the background with a hard timeout, and check for live editor processes before each UBT build. |

`Data/Story/` is a copy of newengine `assets/story/`. As with the face presets, newengine is the source of truth until the story is final.

## A. Open decisions (background)

Everything else branches on these.

1. **Faces: stylised ARKit-52 characters or MetaHuman.**
   - The handoff recommends ARKit-52. It fits the art direction, and the lip-sync tracks already use ARKit names.
   - This choice gates all character, face, AnimBP and IK work.
2. **GitHub billing.** CI jobs never start because of the account's spending limit. `.github/workflows/core.yml` is ready (gcc, clang, msvc).
3. **Save format.** Choose one:
   - a Core text codec for `StorySave` (portable, testable here); or
   - an Unreal-only `USaveGame` built from the `FFTStorySave` USTRUCT that already exists.
4. **When the dialogue is final.** Voice generation (ElevenLabs, key in place) waits on this.
   - Acts I and IV converged in draft62, at 4.56 and 4.57.
   - Acts II, III and the finale/epilogue (04b, 04c, 04e) have handoffs but no draft62 critique or converged score. They're the long pole for VO.
   - Should the critique loop run on them next?
5. **Remote URL** (cosmetic). `origin` still points at the old repo name. GitHub redirects, so pushes work. Fix it with:
   `git remote set-url origin https://github.com/Creantics/UnrealGameWIP.git`

## B. Work a session can do unattended

Each of these is gated by UBT plus ctest.

1. **Battle and combo wrappers.**
   - A battle component over `Battle`/`TurnQueue`, and an arrow-combo component over `ComboSequence`, timed by `UFTConductorComponent`.
   - This is the biggest gameplay area not yet wrapped for Unreal.
   - The idle "port-arrow-combo-timing" session may already hold some of this context.
2. **Game-framework skeleton in C++.**
   - GameInstance, GameMode, PlayerController, Pawn, plus Enhanced Input actions and mappings created from C++.
   - It will compile, but it needs a map before it can run.
3. **The rest of the face direction spec** (newengine `rigging_plan` §7.2), on top of Emotion:
   - containment profiles
   - micro-expression rate
   - listening state
   - emphasis brow flicks
   - pause blink slots
4. **Packaging.**
   - Stage `Data/` via `DirectoriesToAlwaysStageAsNonUFS` in `Config/DefaultGame.ini`.
   - It's a text edit, but only a packaging run proves it works.
5. **Save codec,** once A3 is answered.

## C. Needs the editor and you at the keyboard

1. **First map.** Set the default GameMode and see the three components tick once: a story plays, a face moves, the conductor clock runs. Until this exists, nothing has actually run.
2. **A character mesh with morph targets** wired to `UFTFaceComponent`, plus its AnimBP (jaw and tongue rotation from the component's getters).
3. **IK Rig and IK Retargeter** from the Rigify body rig: DEF-only export, A-pose.
4. **Post-process materials** rebuilt from `docs/reference/style_packs.md`.
5. **Niagara and Chaos** built from `reference/vfx/*.json` and `reference/physics/`.

## D. Tooling gap

Python isn't installed on this PC, so these have never run here:
- `tools/story_import`
- `tools/lipsync` (face-track baking)
- `tools/vo_direction`

This blocks the lip-sync pipeline whatever A1 decides. Either install Python 3 here, or run the tools in a cloud Linux session against the newengine checkout.

## Session report (this run)

**Commits:** 4fb7be5, dbc0fc1, 4ae8eb4, all on origin/main. Core tests went from 93 to 105. The JSON session's 8b38f38 also landed.

- **Emotion map** (4fb7be5): `resolveEmotion` composes the primary emotion (0.2 per intensity step, capped) with the secondary at 0.35 on the upper face and eyes only. Draft data covers all 53 vocabulary words, pending the F10 review.
- **Save round-trip** (dbc0fc1): 162 saves across the whole real script. Each one loads into a fresh runner, which then plays on identically to the original.
- **Edge cases** (4ae8eb4):
  - .nface files cut short at every byte
  - a 64-shape face with its correctives last
  - the track channel limit
  - a 300-option choice
- **Behaviour change:** the channel-limit test found that a refused `FacePlayer::play()` left the previous track running. The header says a refused track plays nothing, so `play()` now stops on refusal. `UFTFaceComponent::PlayTrack` already called `stop()` first, so nothing in the game changes.
