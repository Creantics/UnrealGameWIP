#pragma once

// Shared by the import commandlets (FTImportCharacter, FTImportProp), editor builds only.
//
// CheckGlbFile reads a .glb and checks it against the asset rules (ftcore::checkGlb, see
// docs/ASSET_INTAKE.md section 2), logging every issue; false on any error.
//
// UseProjectMasters moves Interchange's glTF material instances onto the project masters:
// M_FTCharacter for factor-only materials, M_FTCharacterTex (made on first use) for materials with
// textures, M_FTHair (masked, anisotropic; for hair cards) for a textured alphaMode MASK material,
// and sets each bound texture up for its slot. The masters have the skeletal mesh and
// morph target usages, and static meshes need none, so a held prop uses them too (a Nanite prop's need AllowNanite). False when a texture's
// sampler type can't be made to match its slot or a master reports compile errors. Instances already on
// M_FTCharacter, M_FTCharacterTex, M_FTHair or the skin master (SkinMasterPath, built by FTBuildSkin) are left alone.
// FTImportCharacter uses it; FTImportProp uses it only with -Held.
//
// UsePropMasters is FTImportProp's: the same move onto the prop masters, /Game/Props/Shared/Materials/M_FTPropFlat
// (factor-only) and M_FTPropTex (textured; its factors start at glTF's 1, not 0.6 and 0, when Interchange gives none).
// They take M_FTCharacter's and M_FTCharacterTex's parameters under the same names and put ftcore::propSurfaceHlsl's
// layers over them: RoughBreakup (set 1 on a material with no roughness map of its own), Grime (set to Grime, the
// world's ftcore::propGrimeFor), DustColor, WetRough (0.35) and Weather (1; 0 under cover), with the look's Wetness. Each
// is neutral at 0. An instance already on M_FTCharacter or M_FTCharacterTex moves onto the matching prop master with its
// overrides; one already on a prop master keeps its values and takes Grime and RoughBreakup again. A textured MASK
// material still goes onto M_FTHair, with no prop layers. The prop masters take the world wave's surface state (not the
// character state). bHeld (a prop held in a hand): instances on the prop masters go back onto the character masters with
// their values, then UseProjectMasters runs. False as UseProjectMasters, or when an existing prop master has no surface
// node.
//
// CheckPropGloss is the import's gloss guard (ftcore::propGlossCheck): per material it logs roughness p10/p50/p90 and
// metallic, effective (the MetallicRoughnessTexture's top source mip, G and B, times the Roughness and Metallic factors;
// the factors alone when it has no map of its own), and an error with the reason for each failed rule. False when any
// fails, or a /Game material has no Roughness or Metallic parameter to read (one outside /Game is skipped with a warning).
//
// AllowNanite gives the project material under each of Materials the Nanite usage (saving it), so a Nanite mesh
// draws with it instead of the default material.
//
// CompileErrors logs a master's compile errors for the current shader platform and returns how many.

#include "CoreMinimal.h"

#if WITH_EDITOR
#include "ftcore/Glb.h"

class UMaterial;
class UMaterialInterface;

namespace FT
{
bool CheckGlbFile(const TCHAR* Tag, const FString& Source, const ftcore::AssetRules& Rules);
bool UseProjectMasters(const TCHAR* Tag, TConstArrayView<UMaterialInterface*> Materials);
bool UsePropMasters(const TCHAR* Tag, TConstArrayView<UMaterialInterface*> Materials, float Grime, bool bHeld);
bool CheckPropGloss(const TCHAR* Tag, TConstArrayView<UMaterialInterface*> Materials);
void AllowNanite(const TCHAR* Tag, TConstArrayView<UMaterialInterface*> Materials);
int32 CompileErrors(const TCHAR* Tag, UMaterial* Master);
const TCHAR* SkinMasterPath();
} // namespace FT
#endif
