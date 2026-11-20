# Animation tools: FTImportAnim and FTPoseShots

The full flag docs are at the top of `FTImportAnimCommandlet.h` and `FTPoseShotsGameMode.h`. This page covers the
short form and the things that catch people out.

## FTImportAnim: a clip onto an existing skeleton

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportAnim -Source=<file.fbx> -Skeleton=<skeleton or skeletal mesh>
    -Dest=/Game/Characters/<Lead>/Anims -Name=A_Clip [-Socket=<spec>]... [-Fps=30] [-RootLock] [-MinTrackShare=0.9]
    -NullRHI -unattended -nopause -nosplash
```

- **Import only, onto the skeleton you name.** No mesh is imported and nothing is retargeted. If the FBX has a bone
  the skeleton lacks, the commandlet lists the missing bones and imports nothing.
- **Frame rate.** By default the clip keeps the file's own rate: a 60 fps cinematic stays at 60 and a 30 fps
  gameplay clip stays at 30. `-Fps=N` resamples to N instead. The log line gives keys, rate, length and bone tracks.
- **Re-import.** Importing again under the same `-Name` in the same `-Dest` replaces the clip, and paths that
  refer to it keep working.
- **Root.** `-RootLock` holds the root at its first key, so the clip has no root motion.
- **Sockets.** `-Socket=name,bone,x,y,z[,roll,pitch,yaw]` is bone-local.
  `-Socket=name,bone,cs,x,y,z[,ax,ay,az]` is a point in the reference pose's component space; with
  `-Skeleton=<mesh>`, that is measured on the mesh's own reference pose.

### Caveats

- **Sockets go on the skeleton, and the skeleton may be shared.** `-Skeleton=<skeletal mesh>` resolves to that
  mesh's USkeleton. For MetaHuman bodies that is the shared MetaHuman base skeleton, so a socket added there exists
  for every body on it. Use names that cannot clash, such as `socket_<prop>_<side>`. Never commit a likeness
  skeleton or mesh that a run has saved.
- **Exit code in worktrees.** A worktree whose Content still holds Git LFS pointer files logs hundreds of
  "Package is unloadable" errors at startup. The engine then exits the process with 1, even when the commandlet
  succeeded. Judge a run by its own line, `finished execution (result 0)`, plus the `FTImportAnim:` lines, not by
  the process exit code.
- **Maps in worktrees.** In a fresh worktree a map such as `/Game/Maps/Stub` can be a pointer too. Pull it with
  LFS, or copy the real file from a full checkout after checking that its sha256 equals the pointer's oid.

## FTPoseShots: off-screen stills of any clip on any character

```
UnrealEditor-Cmd ForgottenThird.uproject "/Game/Maps/Stub?game=/Script/ForgottenThird.FTPoseShotsGameMode"
    -game -RenderOffScreen -unattended -nosplash -nosound
    -PoseShots=<out dir> (-PoseLook=<look name> | -PoseMesh=<skeletal mesh>) -PoseClip=<anim>
    [-PoseProp=<mesh>,<anim>[,<socket>]] [-PosePieces=<mesh>+<mesh>] -PoseTimes=0.3,2.3 [-PoseViews=front,tq]
    [-PoseNear=hand_r:socket_grip_r+...]
```

- **Output.** One PNG per time and view, named `<clip>_t<s>_<view>.png`. Each still logs one line,
  `FTPoseShots: still ...`, giving the assets, the time and the frame. The frame is 0-based, so a Blender frame is
  this number plus 1.
- **Exit code.** The run exits 0 even when the map fails to load, because the game mode never runs. Check that the
  PNGs exist.
- **`-PoseNear`.** Logs the distance between a body bone and a prop bone. Hands slide along a staff, so a fixed
  grip socket far from the hand does not by itself mean the hand misses the shaft. Look at the stills.
- **Framing.** It is currently tight: a raised hand or the feet can leave the frame.
- **GPU.** This renders, so it needs the shared GPU slot.
- **Shipping.** Development builds only. In Shipping the class is declared but its code is compiled out, so
  `?game=` gives the plain `AFTGameMode` it derives from, which reads no `-Pose` flags and shoots nothing.
