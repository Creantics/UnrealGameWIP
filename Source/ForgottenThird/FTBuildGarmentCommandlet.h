#pragma once

// Builds the shared garment masters and one material instance per substance:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildGarment [-DumpDir=<dir>]
//       -AllowCommandletRendering -unattended -nopause -nosplash
//
// -AllowCommandletRendering is required: without it the masters never compile here and their errors go unseen.
//
// Writes, under /Game/Characters/Shared/Materials/Garment (replacing what is there):
// - T_FTGarment{Leather,Twill,Knit}N / ...M: tiling detail tiles from ftcore::makeGarmentHeight (tangent-space
//   normals; masks R cavity, G micro roughness); -DumpDir also saves them as PNGs;
// - T_FTGarmentFlatNormal, T_FTGarmentORM (occlusion 1, roughness 0.5, metal 0) and T_FTGarmentWear (curvature 0.5,
//   flat), the texture defaults;
// - M_FTGarment (default lit) and M_FTGarmentCloth (cloth shading: fuzz colour and cloth amount), built by one
//   function with the same parameters. The garment's own maps come in over its UVs (BaseColorTexture,
//   MetallicRoughnessTexture packed R occlusion / G roughness / B metal, NormalTexture, WearTexture R curvature).
//   The detail tile is projected three ways in the mesh's pre-skinned space, so its scale is in centimetres on every
//   UV island and it moves with the cloth: scale A (fine) and B (coarse) each fade out with distance. The detail adds
//   to the normal, darkens and roughens the cavities and varies the roughness. Curvature lightens and smooths
//   convex edges and roughens creases. Metal pixels (or ForceMetal) take MetalColor and MetalRoughness. Specular is
//   a parameter (it is 0.5 when left unconnected, which turns dark cloth into grey plastic);
// - MI_FTGarment_{Leather,Woven,Knit,Rubber,Metal}: the substances, each with its detail tile and values. A garment
//   piece's instance takes one of these as its parent and sets only its maps and Gain (albedo scale).
//
// Exits 1 if a master has compile errors or a save fails.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildGarmentCommandlet.generated.h"

UCLASS()
class UFTBuildGarmentCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildGarmentCommandlet();

	virtual int32 Main(const FString& Params) override;
};
