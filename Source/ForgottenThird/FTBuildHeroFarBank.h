#pragma once

// FTBuildMassingWorld -HeroCorner (2026-11-16): the hero corner's view west across the canal, laid out by
// ftcore::heroFarBankLayout (Core/include/ftcore/HeroFarBank.h) and placed as instances of the build's cells:
//   - a row of low lean-to fronts on the far bank from the modular kit (/Game/Props/BKIT/<ID>) on stucco bodies of the
//     engine's cube, a lit front's glass on WindowGlow, its lantern pair and lamp slot
//   - the near quay's rope-and-post edge: timber posts (engine cylinders), the rope a cylinder rod per polyline segment
//   - three moored boats of engine shapes (hull, pointed bow, gunwales, barrel canopy, stern pole) with a small lantern and
//     a lamp slot each
//   - modelled trees (/Game/Props/TREE-<name>) on the far road's west pavement before the grey road-front blocks
// A mesh that isn't imported is logged and its pieces left out. Editor builds only.

#include "CoreMinimal.h"

class FFTCellInstancer;
class UMaterialInterface;
class UStaticMesh;
class UFTNightLightData;

namespace FTHeroFarBank
{
// Places everything; returns the instances added. Stucco, Timber, Dark: the bodies', posts' and hulls', and trims'
// surfaces (null: the kit's own jpk_wall_stucco, jpk_timber, jpk_dark taken from a loaded kit piece). WindowGlow goes on a
// lit piece's jpk_glass slots (null: the glass keeps its own). LanternLarge, LanternSmall: the hero corner's lantern
// meshes, hung by their pivot, their "paper" slots on LargePaper / SmallPaper. LampData (null: no lamp slots): the night
// lights' data the lamp slots append to, each at its gain times LampScale.
int32 Build(FFTCellInstancer& Cells, double Side, UMaterialInterface* Stucco, UMaterialInterface* Timber, UMaterialInterface* Dark,
	UMaterialInterface* WindowGlow, UStaticMesh* LanternLarge, UStaticMesh* LanternSmall, UMaterialInterface* LargePaper,
	UMaterialInterface* SmallPaper, UFTNightLightData* LampData, float LampScale);
}
