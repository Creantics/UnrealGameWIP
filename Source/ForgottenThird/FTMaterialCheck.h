#pragma once

// FTBuildMassingWorld's material check (-MaterialCheck): once everything is laid (after the instancer's Flush, before
// the map is saved), every static and instanced static mesh component of the built world must draw each of its
// material slots with a material of the build's: no null slot, and none that is the engine's default surface material
// (the grey world grid) or resolves to it (an instance with no parent). Components that aren't drawn are left out:
// hidden ones (a kit mesh's collider, the hidden walls), ones without a mesh and instanced ones without an instance.
//
// Each failing component is logged once as an Error naming its owner, itself, its mesh and its bad slots:
//   MaterialCheck: Cell_3_1.HISM_12 mesh /Game/Props/X/SM_X.SM_X: slot 1 null, slot 2 default (/Engine/.../WorldGridMaterial)
// (the first kMaxLines of them; the summary line counts them all), and the build exits non-zero without saving the map.
// Defined in editor builds only (WITH_EDITOR): the game targets, Shipping included, have no material check.

#include "CoreMinimal.h"

class UWorld;

namespace FTMaterialCheck
{
inline constexpr int32 kMaxLines = 60;

// The number of components of World with a null or default material slot (0: the check holds). Logs as above.
FORGOTTENTHIRD_API int32 Count(const UWorld* World);
// The build's call: true without -MaterialCheck on the command line (nothing is checked), or when Count is 0.
FORGOTTENTHIRD_API bool Run(const UWorld* World);
} // namespace FTMaterialCheck
