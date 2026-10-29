#pragma once

// Builds the shared skin master and moves one material slot of a character onto it:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildSkin -Mesh=<skeletal mesh object path> [-Slot=0]
//       [-Albedo=<png>] [-Mask=<png>] [-ScatterScale=1] [-ScatterColor=R,G,B] [-Profile=Field=Value;Field=Value]
//       [-Scalars=Name:Value,Name:Value] [-DumpDir=<dir>]
//       -AllowCommandletRendering -unattended -nopause -nosplash
//
// -AllowCommandletRendering is required: without it the master never compiles in a commandlet.
//
// Writes, under /Game/Characters/Shared/Materials (replacing what is there):
// - SP_FTSkin, a Burley subsurface profile (Unreal's human skin defaults; -ScatterScale scales the mean
//   free path, for checking that the scatter shows at all; -ScatterColor replaces its per-channel mean free path;
//   -Profile sets any other FSubsurfaceProfileStruct field, values in Unreal's text form);
// - T_FTSkinPores (tangent-space normals) and T_FTSkinPoresMask (R cavity, G micro roughness), one
//   tiling tile from ftcore::makeSkinHeight; -DumpDir also saves both as PNGs;
// - T_FTSkinMaskNeutral, the default region mask (R ambient occlusion 1, G roughness scale 0.5, B scatter 1);
// - M_FTSkin, the master: subsurface profile shading, the character's albedo, the pore tile over the
//   UVs (DetailTiling repeats per 0-1 UV), roughness from Roughness x region mask x pore cavity x micro
//   variation, specular (skin is about 0.35) darkened in the pores, scatter amount from the region mask.
//
// The slot's material instance is reparented onto M_FTSkin, keeping its BaseColorTexture. -Albedo and
// -Mask import PNGs next to the mesh's other textures and bind them (BaseColorTexture, SkinMaskTexture);
// -Scalars sets instance parameters. Exits 1 if the master has compile errors or a save fails.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildSkinCommandlet.generated.h"

UCLASS()
class UFTBuildSkinCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildSkinCommandlet();

	virtual int32 Main(const FString& Params) override;
};
