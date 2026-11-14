#pragma once

// Round 10 (-KitGlassFrontage): night glass on the building-kit modules the frontage stacks on street faces. An
// imported kit module wears the textured master, which has no night glow, and its windows are baked dark into its
// maps, so a facade of them reads black at night. With the switch, each frontage mesh of the named kits (default the
// BK-B2 balconies and BK-D1 doors; -KitGlassKits=<a>+<b> names others) gets, per slot on the textured master, a
// material instance of FTShopInteriors::KitGlassMaterial carrying that slot's maps and colour, made once per source
// material in the map package. The master lights a module's baked glass at night by its instance's random (HISM
// PerInstanceRandom), so no custom data is set. Without the switch, or when the master isn't built
// (FTBuildSurfaces -Windows -KitGlass), every mesh keeps its own materials.

#include "CoreMinimal.h"

class UMaterialInterface;
class UPackage;
class UStaticMesh;

namespace FTKitGlass
{
class FORGOTTENTHIRD_API FOverrides
{
public:
	FOverrides(UPackage* Package, const FString& Params);
	bool IsOn() const { return Master != nullptr; }
	// Mesh's per-slot overrides (null keeps the slot's own); empty when off or when the mesh isn't a named kit's or has
	// no slot on the textured master. The view stays valid for this object's life.
	TArrayView<UMaterialInterface* const> For(UStaticMesh* Mesh);
	// The census: meshes, slots overridden, instances made.
	void Log() const;

private:
	UMaterialInterface* InstanceOf(UMaterialInterface* Source);

	UPackage* Package = nullptr;
	UMaterialInterface* Master = nullptr;
	TArray<FString> Kits;
	TMap<UStaticMesh*, TArray<UMaterialInterface*>> PerMesh;
	TMap<UMaterialInterface*, UMaterialInterface*> PerSource;
	int32 Meshes = 0, Slots = 0;
};
} // namespace FTKitGlass
