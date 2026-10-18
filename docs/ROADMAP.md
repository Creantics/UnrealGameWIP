# Roadmap: from here to 1.0 (Oct 2026 → Nov 2027)

Approved 2026-10-18. 1.0 means the full 97-scene script, shipped at indie fidelity. The story stays as written, and production breadth is what gets cut.

**Assumptions** (the user can override any of them):
- one developer plus Claude sessions, part-time
- Win64 on Steam
- English only
- weekly agent usage is the throughput limit
- the decisions A1–A5 in `docs/PLAN.md` stand

## Phase S: the story gate (Oct–Dec 2026)

No game building starts until the script reaches all three bars:
- every one of the five parts scores 4.9/5 or more under the rubric, each graded by a fresh blind critic;
- no part has a STRUCTURAL note;
- two independent whole-script readers each give 9/10 or more.

The work happens in newengine (`build/draft64/`, local only). The private handoff tracks it.

- **Plateau rule:** if three rounds in a row improve nothing, report to the user with the blocker.
- **On exit:** rerun the importer and the VO list on the new draft, refresh `Data/Story/`, and get ctest green. Then VO generation can start.

The only game work allowed during Phase S is chores that aren't building: merging the two branches that are already built, plus the leak purge and the billing fix, which the user does.

## Scope for 1.0

| Area | In 1.0 | Cut or later |
|---|---|---|
| Story | All 97 scenes, choices and the Home Echo consequence flags | — |
| Combat | The existing Core loop (`Battle`, `TurnQueue`, `Conductor`, `ComboGenerator`) plus boss patterns | The design's advanced combat verbs (the biggest cut) |
| Bosses | 16, built from about 5 archetypes; one skirmish template | Unique tech for each boss |
| Environments | Modular kits, about 4 per world (20–25 spaces), styled by `PP_Style` and `MI_Style_*` | About 82 unique locations |
| Staging | Text, VO, camera, and Core `FaceDirection` / `Emotion` / `FacePlayer` | Mocap |
| Characters | 5 leads with full faces, about 10 supporting characters with faces, generic bodies for the rest | Bespoke bodies for minor characters |
| VO | Generated, about 2,100 lines | Human cast |
| Music | About 30 cues; `Conductor` handles the timing | Adaptive stems |
| Optional | — | Camps, side quests and mini-games (post-launch) |
| Accessibility | Subtitles, input remapping, a photosensitivity toggle | The full plan |

## Milestones

Each milestone ends with a cooked Win64 build that the user plays.

| Milestone | Date | Content |
|---|---|---|
| M0 | Feb 2027 | Clear the blockers: merge the branches, fix the grey characters, purge, fix billing. Build the dialogue UI, movement, menus and VO playback. Prologue Ch1–2 playable end to end (the vertical slice). |
| M1 | Apr 2027 | Act I. Combat on real maps, 2 bosses, the Solhaven and Lattice kits, save and load, the full VO pass, choice branching. |
| M2 | Jun 2027 | Acts II–III. The Unwritten and Ink-Solhaven kits, 8 bosses, music, Home Echo flags, supporting-cast faces. |
| M3 | Aug 2027 | Content complete (alpha). Acts IV–V and the Epilogue, the Restored world and the Interstice, all 16 bosses. |
| M4 | Oct 2027 | Beta. Full-playthrough tests, performance, accessibility, the Steam page and trailer. |
| M5 | Nov 2027 | 1.0. |

## Working method

- **Systems before content:** front-load everything reusable into M0 and M1.
- **Code placement:** logic lives in Core and is covered by ctest; the UE module wraps it.
- **Sessions:** one session per system or act, each in its own worktree. A branch merges after its diff is read and `tools/gate.ps1 -Ubt` passes.
- **Assets:** maps and assets are built by commandlets, never by hand-edited binaries.
- **Story text:** it stays in newengine and is never committed here.

## Handoffs

Both handoff files open with a **Resume here** block: the phase, the next action, and what's in flight.
- `docs/HANDOFF_NEXT_SESSION.md` is public and must contain no story text.
- The private handoff stays outside the repo.

Update them:
- after every finished step;
- after every agent report;
- before any batch of agents or long build;
- at least every ~30 minutes.

When usage runs low, stop launching agents, write both handoffs, push, and stop.

## What the user does

- **M0:** run the purge, fix billing, and play the vertical slice.
- **M1:** give art direction for each world kit, and provide the VO key, which stays outside the repo.
- **M2:** set the music budget.
- **M4:** set up Steam and pick the release date.
