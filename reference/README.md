# Reference data from the frozen engine

This is engine-specific data copied unchanged from newengine `assets/`. It's here as the spec to rebuild in Unreal, not as data Unreal reads.

- `vfx/*.json`: the 12 particle effects (emitters, modules, curves). Rebuild them as Niagara systems. The module model is described in `docs/reference/vfx.md` and the plan in `docs/reference/particles_plan.md`.
- `physics/ragdoll/`: ragdoll bodies and constraints. Rebuild them as Chaos physics assets. See `docs/reference/physics_plan.md`.

These files need the editor, so no unattended session works on them.
