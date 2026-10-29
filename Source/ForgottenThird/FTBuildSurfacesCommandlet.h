#pragma once

// Builds the worlds' photographed surfaces from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildSurfaces -Source=<dir> -AllowCommandletRendering -unattended -nopause -nosplash
//
// Reads Data/Field/surfaces.json and, for each surface, the three JPEG maps tools/fetch_surfaces.py puts in
// <dir>/<id>/: <id>_diff_<res>.jpg (colour), <id>_nor_dx_<res>.jpg (DirectX normal) and <id>_arm_<res>.jpg (ambient
// occlusion, roughness and metal in red, green and blue). Writes, under /Game/Rendering/Surfaces/<id>/, the textures
// T_<id>_D, T_<id>_N and T_<id>_ARM (each keeps its JPEG as its source, so a texture costs about its file's size) and
// MI_Surface_<id> (FTScene::SurfaceInstancePackage), an instance of the master with the surface's textures and real
// tile size. The master, /Game/Rendering/M_WorldSurfaceTex (FTScene::SurfaceTexMaterial), projects the textures along
// each shape's own axes (each instance's, for instanced shapes): one copy every "TileCm" centimetres whatever the
// shape's scale, image-down pointing down the world on a side face and down the slope on a tilted top, so a pitched
// roof's tiles run down it. It takes a "Color" tint (white keeps the texture's colour), M_WorldSurface's mottle
// ("Variation", "VariationScale") and a broad one ("MacroVariation", "MacroScale") that keeps the repeats from lining
// up over open ground, and holds the albedo to 0.03-0.8. The textures are loaded whole: the streamer judges a
// texture's need from the mesh's own UVs, which a projection doesn't use. Every file is replaced; nothing is saved
// if a file is missing or the master doesn't compile (-AllowCommandletRendering makes the compile real).
//
// -Only=<id>,<id>,... rebuilds just those surfaces' textures and instances and keeps the others. The master keeps
// its graph, but its default textures follow the table's first surface: it is saved only when they move. See
// docs/SCENE_LOOK.md.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildSurfacesCommandlet.generated.h"

UCLASS()
class UFTBuildSurfacesCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildSurfacesCommandlet();

	virtual int32 Main(const FString& Params) override;
};
