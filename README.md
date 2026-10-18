# The Forgotten Third

A turn-based RPG with arrow-combo battles, built in Unreal Engine 5.

## Setup

1. Install Git LFS, then run `git lfs install`. Do this **before** cloning, or assets come down as pointer files.
2. Install the latest Unreal Engine 5.x (Epic Games Launcher) and Visual Studio 2022 with the "Game development with C++" workload.
3. Clone onto a fast NVMe drive; Unreal's shader and data caches write a lot.

## Layout

- `ForgottenThird.uproject`, `Config/`, `Content/`, `Source/ForgottenThird/`: the Unreal project and its thin game module.
- `Core/`: the game rules as a plain C++20 library (`ftcore`) with its own CMake build and tests (battle, arrow combos, story state and saves, dialogue loops, face and lip-sync math). It has no Unreal dependency.
- `Source/ForgottenCore/`: the Unreal module that compiles `Core/`. It has one wrapper `.cpp` per `Core/src/*.cpp`; add one when you add a Core source file.
- `Data/` (planned): JSON game data shared by Core and Unreal.

## Core tests

Needs CMake 3.24+. The first configure fetches doctest.

```
cmake -S Core -B Core/build
cmake --build Core/build --config Debug
ctest --test-dir Core/build -C Debug --output-on-failure
```

The story, the art direction and the tools carried over from the earlier engine live in [creantics/newengine](https://github.com/Creantics/newengine); start with `docs/HANDOFF_UE.md` there.
