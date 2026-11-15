#pragma once

// Imports a plant made by tools/blender/make_tree.py (a tree, and by the same contract a shrub, hedge or grass clump)
// with its LOD chain, billboard, collision and foliage materials, from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportPlant -Source=<dir>/<NAME>.glb [-Dest=/Game/Props] [-Masters]
//       -AllowCommandletRendering -unattended -nopause -nosplash
//
// The contract, beside <NAME>.glb (nodes <NAME>_LOD0.._LODn, each LOD's faces on the materials Bark, Leaf and
// Billboard by name; an optional UCX_<NAME>_LOD0_00 convex):
//   <NAME>_leaf.png (sRGB, alpha = the card's cut-out), <NAME>_leaf_n.png (normal, green down), <NAME>_leaf_m.png
//   (R leaf coverage, G twig coverage, also under the leaves, B a per-leaf random id), and optionally <NAME>_bark.png
//   and _bark_n.png (no bark: a grass or hedge clump of cards only), <NAME>_bb.png (the billboard's picture; one
//   picture, or a 2 x 2 atlas of leafy, bare and blossom states when the json says "billboard_atlas": 1) and
//   <NAME>_bloom.png (sRGB, the blossom cards, same cells as the leaf atlas). <NAME>.json may give trunk_radius_m and
//   crown_base_m (a capsule when the file has no UCX), autumn_a / autumn_b / twig / spring (sRGB 0..255 triples),
//   leaf_mean_srgb and billboard_atlas.
//
// It writes, under <Dest>/<NAME>:
//   T_<NAME>_*            the maps (leaf alpha kept through the mips by its coverage, no virtual texturing)
//   MI_<NAME>_Leaf, _Bark, _Billboard   instances of the shared masters, with this plant's maps and palette
//   <NAME>                one static mesh: the file's LODs (screen sizes 1, 0.45, 0.20, 0.06 or as many as there are),
//                         no Nanite, the LODs dithered into each other, the file's normals, simple collision only (the
//                         file's trunk convex, else a trunk capsule; the leaves never collide).
// And, when missing or with -Masters, the shared masters under /Game/Rendering/Foliage:
//   M_FTFoliage   masked, two-sided, two-sided foliage shading, cast ray-traced shadows through the cut-out; the leaf
//                 colour varied per instance (hue and value), the season (FTSeason.h, MPC_Look Season: blossom, spring
//                 green, summer, autumn by each leaf's id, bare winter twigs), subsurface from the colour, the card's
//                 normal kept on both faces (the crown's normals light the cards as one canopy)
//   M_FTBark      default lit, opaque
//   Both move by the same wind (MPC_Env WindDeg and WindMps, never stiller than a light breeze): the lower trunk still,
//   the crown swaying more with height and reach, a phase per instance; the leaves flutter on top.
// Returns 1 when a file is missing, a master doesn't compile, or the mesh comes out without its LODs.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTImportPlantCommandlet.generated.h"

UCLASS()
class UFTImportPlantCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTImportPlantCommandlet();

	virtual int32 Main(const FString& Params) override;
};
