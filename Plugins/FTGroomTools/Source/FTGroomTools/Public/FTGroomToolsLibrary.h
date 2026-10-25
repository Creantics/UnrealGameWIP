#pragma once

// Writes the strands of a groom asset to a flat binary file, so a stock groom can be mirrored, trimmed
// or recut outside Unreal and reimported as Alembic (tools/blender/groom_strands.py reads the file).
// Editor only; call it from Python in the editor:
//
//   info = unreal.FTGroomToolsLibrary.export_groom_strands(unreal.load_asset(groom_path), "D:/out.ftgs")
//
// File layout, little endian: "FTGS", int32 version (1), int32 strand count, int32 vertex count; then
// per strand int32 vertex count, int32 group id, int32 guide flag, float root u, float root v, float
// strand width; then per vertex float x, y, z (groom space, cm) and float width (cm). A missing
// attribute is written as 0 (the group id as -1). The strands are those of the asset's current hair
// description, guides included (flag 1).
// Returns one JSON object with the counts, the attributes found and each group id's name, or an empty
// string when the groom has no hair description or the file can't be written.
//
// ExportGroomColors / SetGroomColors read and write the per-vertex colour, roughness and AO and the
// per-strand root UV of the same hair description, in the same order as ExportGroomStrands. The MetaHuman
// hair material takes its ombre coordinate from the colour's red channel and samples its highlight mask at
// the root UV. File layout, little endian: "FTGC", int32 version (2), int32 vertex count, int32 has
// colour, int32 has roughness, int32 has AO, int32 strand count, int32 has root UV; then per vertex float
// r, g, b, roughness, AO, then per strand float u, v (0 where missing). Version 1 files stop after the
// has-AO field and the vertex block. SetGroomColors writes only the attributes the file has, refuses
// counts that don't match the groom, then rebuilds the groom; save the asset afterwards.

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "FTGroomToolsLibrary.generated.h"

class UGroomAsset;

UCLASS()
class UFTGroomToolsLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "FT|Groom")
	static FString ExportGroomStrands(UGroomAsset* Groom, const FString& Path);

	UFUNCTION(BlueprintCallable, Category = "FT|Groom")
	static FString ExportGroomColors(UGroomAsset* Groom, const FString& Path);

	UFUNCTION(BlueprintCallable, Category = "FT|Groom")
	static FString SetGroomColors(UGroomAsset* Groom, const FString& Path);
};
