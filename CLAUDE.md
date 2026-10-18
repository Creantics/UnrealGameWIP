# Working on this repo

- Read `docs/HANDOFF_UE.md` in creantics/newengine (branch `feature/engine-foundation`) before starting. It has the plan, what carries over, and open decisions.
- Game logic goes in `Core/` as plain C++20: no Unreal headers, no exceptions, no RTTI, clean under `-Wall -Wextra -Wshadow`, with unit tests that run outside Unreal. The Unreal module only wraps it.
- Avoid per-frame heap allocations in gameplay code.
- Don't hand-write Blueprints or other binary assets. Keep logic in C++.
- Large binaries go through Git LFS (see `.gitattributes`). Never commit `Binaries/`, `Intermediate/`, `Saved/` or `DerivedDataCache/`.
- Don't open a pull request unless asked.
- Push any branch to origin without asking once its checks pass (ctest green, plus UBT Succeeded when `Source/` changed). Never force-push. The repo is public, so never push story text (see below).
- Story originality: borrow techniques only from reference games. Take no premise, world mechanics, names, plot beats or set pieces, and never copy their text. No franchise or artist names in shipped text. The full rules are in the newengine design docs.
- Don't upload story text to outside sites (a logline at most) unless the user says so.
