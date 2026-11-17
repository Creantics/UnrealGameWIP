#pragma once

// The water surface's build side (editor builds only; the game reads what it saves):
//   FTWaterSurfaceBuild::WireMaterial  gives a water master (M_Look_Water, M_Look_CanalWater) the moving surface:
//       the waves' vertex offset (Plugins/FTLook/Shaders/Private/Water/Surface.ush, the copy of ftcore::waterWaveOffset),
//       the tide, and the waves' normal added to the material's own ripples. All of it sits behind SurfaceOn (default
//       0), so a plane wearing the master draws as before; UFTWaterSubsystem's tiles turn it on in their instances.
//       Called by FTBuildLookMaterials after each master's inputs are connected.
//   FTWaterSurfaceBuild::BuildTileMesh  the tile (FTScene::WaterTileMesh): a 16 m square with three LODs (cells of
//       25 cm, 1 m, 4 m; the cell baked in UV1.x so a vertex drops the waves its grid can't carry) and a skirt that
//       hides the crack where two LODs meet. Called by FTBuildWaterSurface.

#include "CoreMinimal.h"

class UMaterial;
class UStaticMesh;

namespace FTWaterSurfaceBuild
{
// The tile's side, cm, and its cells per LOD.
inline constexpr float kTileCm = 1600.f;
inline constexpr int32 kTileCells[3] = {64, 16, 4};
inline constexpr float kSkirtCm = 120.f;

#if WITH_EDITOR
void WireMaterial(UMaterial* Material);
UStaticMesh* BuildTileMesh();
#endif
} // namespace FTWaterSurfaceBuild
