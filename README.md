# The Forgotten Third

A turn-based RPG with arrow-combo battles, built in Unreal Engine 5.

## Setup

1. Install Git LFS, then run `git lfs install`. Do this **before** cloning, or assets come down as pointer files.
2. Install the latest Unreal Engine 5.x (Epic Games Launcher) and Visual Studio 2022 with the "Game development with C++" workload.
3. Clone onto a fast NVMe drive; Unreal's shader and data caches write a lot.

## Layout (planned)

- `ForgottenThird.uproject`, `Config/`, `Content/`, `Source/ForgottenThird/`: the Unreal project and its thin game module.
- `Core/`: the game rules as a plain C++20 library with its own CMake build and tests (battle, arrow combos, story state and saves, dialogue loops, face and lip-sync math). It has no Unreal dependency.
- `Data/`: JSON game data shared by Core and Unreal.

The story, the art direction and the tools carried over from the earlier engine live in [creantics/newengine](https://github.com/Creantics/newengine); start with `docs/HANDOFF_UE.md` there.
