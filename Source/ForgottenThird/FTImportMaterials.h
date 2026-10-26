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
// morph target usages, and static meshes need none, so props use them too. False when a texture's
// sampler type can't be made to match its slot or a master reports compile errors.

#include "CoreMinimal.h"

#if WITH_EDITOR
#include "ftcore/Glb.h"

class UMaterialInterface;

namespace FT
{
bool CheckGlbFile(const TCHAR* Tag, const FString& Source, const ftcore::AssetRules& Rules);
bool UseProjectMasters(const TCHAR* Tag, TConstArrayView<UMaterialInterface*> Materials);
} // namespace FT
#endif
