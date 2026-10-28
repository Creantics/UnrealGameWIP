#pragma once

// Builds the world surface master from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildWorldMaterials -NullRHI -unattended -nopause -nosplash
//
// Writes /Game/Rendering/M_WorldSurface (FTScene::SurfaceMaterial), the base every map builder tints for its
// blocks and props: an opaque lit surface (Substrate converts its classic inputs) with a "Color" vector,
// "Roughness" and "Metallic", and a low-frequency mottle on the world position that moves the albedo by up to
// "Variation" and the roughness by half that, over patches about "VariationScale" centimetres across. The
// albedo is held to a plausible 0.04-0.8 whatever colour a tint asks for (a brighter one is scaled down,
// keeping its hue). The file is replaced, and nothing is saved if the material doesn't compile. See
// docs/SCENE_LOOK.md.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildWorldMaterialsCommandlet.generated.h"

UCLASS()
class UFTBuildWorldMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildWorldMaterialsCommandlet();

	virtual int32 Main(const FString& Params) override;
};
