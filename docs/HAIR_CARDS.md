# Hair cards

The leads' hair, brows, beards and moustaches are grooms: strand assets. Each groom also gets hair cards,
textured strips that the engine's hair card generator (an Experimental plugin in UE 5.8) builds from the
strands, one set for each level of detail. A level with cards draws them in place of its strands, for a
fraction of the cost.

`Data/Grooms/hair_cards.json` lists the grooms and each level's card budget. The editor command
`FT.HairCards`, in the opt-in plugin `Plugins/FTHairCards`, builds the cards and saves them with each groom.

## Pieces

| Piece | File | Job |
|---|---|---|
| `ftcore::HairCardSpec` | `Core/include/ftcore/HairCardSpec.h`, `Core/src/HairCardSpec.cpp` | Loads and checks the spec. The header has the full format. Tests: `Core/tests/test_hair_card_spec.cpp`. |
| The spec | `Data/Grooms/hair_cards.json` | Seven grooms and their levels. Ids, asset paths and numbers only. |
| `FT.HairCards` | `Plugins/FTHairCards/` | The console command that drives the generator. The plugin is off by default, so the everyday editor loads neither the generator nor its Python packages. |
| `-HairCards` | `tools/gate.ps1` | The gate, with the plugin in the UBT build. |

## A run

1. Run `tools/gate.ps1 -HairCards` right before, every time. Every other build leaves the plugin out, so after
   one the plugin is still built against the older game code (and the editor may refuse it and offer to
   rebuild it).

   If its build stops with `RulesError FabToolset` (the launcher's 0.0.22 update of the engine's Fab plugin
   added a module that the engine's installed build rules don't have), run `tools/gate.ps1` for the tests, then
   build by hand with Fab left out (PowerShell, from the repo root):

   ```
   & 'E:\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat' ForgottenThirdEditor Win64 Development "-Project=$PWD\ForgottenThird.uproject" -WaitMutex -NoHotReloadFromIDE -DisablePlugin=Fab -EnablePlugin=FTHairCards
   ```
2. Start the full editor, not `UnrealEditor-Cmd`, with the plugin on and the command on its command line
   (PowerShell, from the repo root; the exe is `E:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe`):

   ```
   $env:UE_PIPINSTALL_PATH = 'D:/projects/ue_pip/ForgottenThird'
   UnrealEditor ForgottenThird.uproject -EnablePlugins=FTHairCards -ForcePipInstallOnInit "-ExecCmds=FT.HairCards quit"
   ```

   - `UE_PIPINSTALL_PATH` puts the generator's Python packages in one folder outside the checkout, so every
     worktree uses the same install. Without it they go in each checkout's `Intermediate/PipInstall`.
   - `-ForcePipInstallOnInit` installs those packages before the editor starts. The first install is about
     2.5 GB to download (an estimated 5 to 6 GB on disk) and took about 4 minutes here. Without the switch the
     install runs in the background after startup and can outlast the command's wait.
   - `-ExecCmds` gives the command on the editor's first tick. It can also be typed in the editor's console.
3. Watch the log (`Saved/Logs/ForgottenThird.log`, category `LogFTHairCards`). Each level shows a progress
   dialog. The run's summary line is
   `FT.HairCards: <n> grooms built (<levels> levels, <cards> cards), <failed> failed`.

The run uses the GPU (the generator's packages include PyTorch with CUDA and an OpenGL renderer), so leave the
machine to it. The first groom built, a moustache of 3,367 strands, took about 2 minutes: 63 s and 34 s for its
two levels, then 14 s to save. Grooms of tens of thousands of strands aren't measured yet.

There's no commandlet. The generator reports its progress and failures through the editor's UI
(notifications and dialogs), which a commandlet doesn't have, and it crashes on the first one.

### The command

`FT.HairCards [spec] [only=<id>] [quit]`

| Argument | Meaning |
|---|---|
| `spec` | The spec file, relative to the project folder or absolute. Default `Data/Grooms/hair_cards.json`. |
| `only=<id>` | Builds only the groom with this id. An empty id, or one the spec doesn't list, is refused. |
| `quit` | Closes the editor when the run ends, with exit code 0 when every groom built and 1 otherwise. |

The generator's Python side comes up on the editor's first ticks, after any package install. The command
waits for it for up to 300 s, with a log line every 30 s, then fails and says to pass `-ForcePipInstallOnInit`.
One run at a time: a second `FT.HairCards` during a run is refused.

## What a run does

For each groom in the spec:

1. Loads the groom. A groom with several hair groups gets a warning (see below).
2. Adds the levels the groom doesn't have yet as the groom editor's Add LOD does: each new level copies the one
   before with half the vertices, half the curves, twice the angular threshold and half the screen size. Then
   the levels take the spec's screen sizes.
3. Builds each listed level in the groom editor's order: settings, strands, clumps, card geometry, texture
   clusters, atlas layout and atlases, then the card mesh and textures. A level keeps its material slot and
   options but starts with no mesh, textures or generator settings, because an earlier mesh would send the
   output to that mesh's folder.
4. When every level has built, gives the groom the cards, switches each level whose `useCards` is true (the
   default) to draw them, and saves the groom, the new meshes and textures, and any other changed package
   under the groom's `out` folder. `useCards: false` builds and saves the cards but leaves the level drawing
   what it drew, so they can be checked first.

Each level is built on its own, with its own atlas: the generator's options to reduce the previous level's
cards or reserve its atlas space stay off. Each strand filter group of the hair group gets the level's targets
(`cards`, `triangles`, `textures`, `flyaways`). Every run rebuilds every listed level from scratch, whatever
the generator thinks changed; `only=` keeps a run to one groom.

The card target steers the generator rather than capping it: the moustache's first level came out at 652 cards
for 500 asked, because the generator gives some clumps more than one card, while its triangles stayed within the
budget (5,996 of 6,000). So a level with more cards than its target isn't a fault in the spec.

The generator itself saves nothing. If any level of a groom fails, the plugin saves nothing for that groom,
logs `<id>: left unsaved` and goes on to the next one. That groom stays changed in memory, so don't save it if
the editor asks.

## Hair groups

`group` in the spec (0 when absent) picks the hair group that gets the cards. The generator builds them from
every hair group's strands: that's the groom editor's default, and the plugin keeps it. Turned off, the
generator keeps only the cards whose strands' group id equals the group's index, which keeps none when a
groom's ids don't start at 0.

So on a groom with several hair groups, the listed group draws cards made from all of them and the other
groups keep drawing their strands. The plugin warns when it sees one.

## Output

- Each level goes in its own folder, `<out>/<groom>_LOD<n>`: the card mesh (named the same, with `_PG<group>`
  added on a groom with several hair groups) and its textures.
- The generator's working files go in `Intermediate/GroomHairCardGen/`: cached strands, and each level's
  settings under `Metadata/`.
- The leads' grooms and cards live under `Content/Characters/*/MH/`, which stays local and out of git, as
  `Intermediate/` does.
- Materials are left alone. A level that had no cards before uses the engine's default cards material until
  the look pass gives it the hair material.

## Hazards

- **The groom editor's own Generate.** The generator skips loading a groom it believes it loaded last, but
  `FT.HairCards` loads strands without updating that memory. After a run, the groom editor's Generate could
  build from the strands the run loaded last, so restart the editor before using it.
- **A changed `out`.** Within one editor session the generator reuses a level's settings, which keep the
  folder they were first given, and it reloads each level's settings from `Intermediate/GroomHairCardGen/Metadata`.
  When a run refuses a level because the generator "would write to … outside …", delete that groom's files in
  that folder, restart the editor and run again.
