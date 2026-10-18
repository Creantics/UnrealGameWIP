#pragma once

// The mapping from ftcore::StyleSettings to the parameters of the post-process material
// /Game/Rendering/PP_Style (built by FTBuildStyleMaterialsCommandlet). The commandlet uses it to
// create the parameter nodes and the pack instances, and AFTStyleController to drive its dynamic
// instance, so both always agree. See docs/STYLE_PACKS_UE.md.
//
// Units: StyleSettings positions and distances are newengine world units (metres, Y up). They are
// converted here to Unreal centimetres, Z up: (x, y, z) m -> (x, z, y) * 100 cm.

#include "CoreMinimal.h"
#include "Templates/Function.h"

namespace ftcore
{
struct StyleSettings;
}

namespace FTStyle
{
// Asset paths (package names; the object name equals the last path element).
inline const TCHAR* const MaterialPackage = TEXT("/Game/Rendering/PP_Style");
inline const TCHAR* const MaterialObjectPath = TEXT("/Game/Rendering/PP_Style.PP_Style");

// Scene depth (cm) at or beyond which a pixel counts as sky. A material parameter, not a style field.
inline constexpr float DefaultSkyDepth = 60000.f;
FORGOTTENTHIRD_API FName SkyDepthName();

// Calls Scalar / Vector once per material parameter, with the value for Style. No heap allocation.
FORGOTTENTHIRD_API void VisitParameters(const ftcore::StyleSettings& Style, TFunctionRef<void(FName, float)> Scalar,
	TFunctionRef<void(FName, const FLinearColor&)> Vector);
}
