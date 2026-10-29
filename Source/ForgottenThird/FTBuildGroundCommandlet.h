#pragma once

// Builds the big worlds' ground master from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildGround -AllowCommandletRendering -unattended -nopause -nosplash
//
// Writes /Game/Rendering/M_WorldGround (FTScene::GroundMaterial), which each world map's Landscape wears through the
// instance FTBuildExploreWorld makes for it, and /Game/Rendering/T_GroundNoRoads, the empty road mask the master takes
// by default. The master blends three of FTBuildSurfaces' photographed surfaces (FTScene::GroundLayers):
//   - the ground, laid flat along the world's X and Y and drawn twice, the second copy turned and larger, with a broad
//     mottle choosing between them so neither copy's repeat lines up across open ground;
//   - rock, projected along the world's three axes, wherever the ground is steeper than about 0.75 to 0.95 (37 to 44
//     degrees), the line wandering by noise, and on gentler ground from "RockAltitudeCm" up, over "RockAltitudeRunCm"
//     (the master's default lies over any ground; a world map's instance sets it over its square's highest ground). The
//     two meet by height: each surface's occlusion map, about its own mean, stands for its height ("RockBlendHeight",
//     at most 0.95 less the depth), and wherever one stands "RockBlendDepth" clear of the other it alone shows, so the
//     rock breaks through at its high points and the ground keeps to its low ones;
//   - the road, laid flat inside each road's edge. "RoadMask" covers the playable square ("MaskSideCm" across) with
//     ftcore::roadMask's bytes, read back as the distance from the nearest road's centre line; the edge wanders by
//     noise and softens over "RoadEdgeCm".
// Each layer takes a tint, a tile size and a roughness that replaces its packed map's ("GroundTint", "GroundTileCm",
// "GroundRoughness", ...; negative keeps the map's). All three take M_WorldSurfaceTex's mottles ("Variation",
// "VariationScale", "MacroVariation", "MacroScale") and a drift between warm and cool over broad patches ("HueDrift"),
// and the albedo is held to 0.03-0.8. Far off, from "FarStartCm" to "FarEndCm" from the eye, the detail that would tile
// or glitter gives way: a third copy of the ground, "FarScale" times the size, takes "FarBlend" of its colour and packed
// maps, the bumps ease to "FarNormalStrength" of their strength and the fine mottle fades. The defaults are the town's
// surfaces from Data/Field/surfaces.json, so
// FTBuildSurfaces must have built them. Both files are replaced; nothing is saved if a texture is missing or the master
// doesn't compile (-AllowCommandletRendering makes the compile real). See docs/SCENE_LOOK.md.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildGroundCommandlet.generated.h"

UCLASS()
class UFTBuildGroundCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildGroundCommandlet();

	virtual int32 Main(const FString& Params) override;
};
