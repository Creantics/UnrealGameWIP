#pragma once

// Builds an arena stage in the world at run time (docs/ARENA_DEMO.md, "Stages"): the primitives, lamps and
// scene look of one ftcore::ArenaStageLayout, which Core generates from Data/Battle/arenas.json and the fight's
// enemies (Core/include/ftcore/ArenaStage.h). Everything is an engine basic shape (a cube or a cylinder) in a
// dynamic instance of the world surface material, or of the glow material (/Game/Rendering/M_ArenaGlow, built by
// FTBuildArenaWorld) for the lit bands, cracks and caps; point lights carry the lamp colours, and the sun, sky,
// fog and camera finish come from the layout's SceneLook through FTScene::SpawnLook. Nothing here runs per
// frame, and the actors are never moved after they are placed.

#include "CoreMinimal.h"

#include "ftcore/ArenaStage.h"

class UWorld;

namespace FTArenaStage
{
// The glow material's package: a "Color" vector times a "Strength" scalar into the emissive colour, built by
// FTBuildArenaWorld. The stage falls back to a bright surface when it hasn't been built.
inline constexpr const TCHAR* GlowMaterial = TEXT("/Game/Rendering/M_ArenaGlow");

// Spawns Layout's pieces, lamps and look into World. Returns the number of actors made.
FORGOTTENTHIRD_API int32 Build(UWorld* World, const ftcore::ArenaStageLayout& Layout);
}
