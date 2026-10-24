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
};
