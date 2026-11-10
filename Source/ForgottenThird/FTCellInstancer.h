#pragma once

// The massing build's instancer: every box, pavement, road solid, prop and stand-in car it lays goes in here instead of
// becoming an actor of its own. Pieces are grouped by 250 m cell of the plan, mesh, material set, collision kind, cull
// class, shadow and tag; each group is one hierarchical instanced mesh component on its cell's holder actor (labelled
// "Cell_<i>_<j>"). Instances are gathered by Add and made at once by Flush, before the navmesh is built. A piece whose
// materials can't be drawn instanced (the usage flag unset on an imported material, which the build doesn't save) is
// spawned as a plain static mesh actor instead, so it still looks as it did; Flush logs those materials.

#include "CoreMinimal.h"

class AActor;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
class UPackage;
class UStaticMesh;
class UStaticMeshComponent;
class UWorld;
struct FActorSpawnParameters;

// How a piece collides.
enum class EFTCellKind : uint8
{
	Solid,     // blocks everything and shapes the navmesh (buildings, frames, cars, road furniture that stops the lead)
	Walk,      // blocks everything and shapes the navmesh, walked on (pavements, kerbs, carriageways, quays' decks)
	NoCollide, // dressing: no collision, no navigation (kit meshes, props, frontage, markings, roof clutter)
	Hidden,    // a solid hidden in game, casting no shadow: the collider a kit mesh stands in for
	Count
};

// How far (cm) a piece is drawn: per class, constants. 0 draws it at any distance.
enum class EFTCellCull : uint8
{
	None,     // buildings, ground pieces (kerbs, pads, carriageways, markings)
	Prop,     // street furniture, frontage pieces, road furniture
	Dressing, // small dressing: roof clutter, quay furniture
	Car,      // stand-in cars and two-wheelers
	Count
};

namespace FTCells
{
inline constexpr double CellSize = 25000.0;   // cm: a 250 m cell
inline constexpr int32 PropCull = 15000;      // cm
inline constexpr int32 DressingCull = 8000;   // cm
inline constexpr int32 CarCull = 20000;       // cm
inline constexpr float CullFadeStart = 0.85f; // instances start fading at this share of the end distance
int32 CullEnd(EFTCellCull Cull);
}

// What else a piece takes beyond its mesh, materials, transform and kind.
struct FFTCellOptions
{
	EFTCellCull Cull = EFTCellCull::None;
	bool bCastShadow = true;
	FName Tag;     // on the component (ComponentTags); NAME_None for none
	FString Label; // the actor's label when the piece falls back to an actor
};

// A piece's place, for setting its custom data after Add: an instance of a group, or a fallback actor's component.
struct FFTCellHandle
{
	int32 Group = INDEX_NONE;
	int32 Index = INDEX_NONE;
	UStaticMeshComponent* Component = nullptr;
	bool IsValid() const { return Component != nullptr || (Group != INDEX_NONE && Index != INDEX_NONE); }
};

class FFTCellInstancer
{
public:
	FFTCellInstancer(UWorld* InWorld, const FActorSpawnParameters& InSpawn);

	// Adds a piece. Materials: per slot, null keeps the mesh's own (an empty view keeps them all). CustomData: the
	// instance's custom data floats from slot 0 (more may be set later through the handle).
	FFTCellHandle Add(UStaticMesh* Mesh, TArrayView<UMaterialInterface* const> Materials, const FTransform& Transform,
		TArrayView<const float> CustomData, EFTCellKind Kind, const FFTCellOptions& Options = FFTCellOptions());
	// Sets one custom data float of an added piece (custom primitive data on a fallback actor).
	void SetCustomData(const FFTCellHandle& Handle, int32 Slot, float Value);
	// The holder actor of the cell At (world cm) lies in, spawned on first use.
	AActor* CellAt(const FVector& At);
	// Makes every gathered group's component and instances (once; later Adds are made by a later Flush).
	void Flush();

	struct FStats
	{
		int32 Cells = 0, Components = 0, Instances = 0;
		int32 PerKind[static_cast<int32>(EFTCellKind::Count)] = {};
		int32 FallbackActors = 0;
	};
	const FStats& Stats() const { return Totals; }

private:
	struct FKey
	{
		FIntPoint Cell = FIntPoint::ZeroValue;
		UStaticMesh* Mesh = nullptr;
		TArray<UMaterialInterface*, TInlineAllocator<2>> Materials;
		EFTCellKind Kind = EFTCellKind::Solid;
		EFTCellCull Cull = EFTCellCull::None;
		bool bCastShadow = true;
		FName Tag;
		bool operator==(const FKey& Other) const
		{
			return Cell == Other.Cell && Mesh == Other.Mesh && Materials == Other.Materials && Kind == Other.Kind && Cull == Other.Cull &&
				bCastShadow == Other.bCastShadow && Tag == Other.Tag;
		}
		friend uint32 GetTypeHash(const FKey& Key)
		{
			uint32 Hash = HashCombine(GetTypeHash(Key.Cell), PointerHash(Key.Mesh));
			for (const UMaterialInterface* Material : Key.Materials)
			{
				Hash = HashCombine(Hash, PointerHash(Material));
			}
			Hash = HashCombine(Hash, (static_cast<uint32>(Key.Kind) << 8) | (static_cast<uint32>(Key.Cull) << 1) | (Key.bCastShadow ? 1u : 0u));
			return HashCombine(Hash, GetTypeHash(Key.Tag));
		}
	};
	struct FGroup
	{
		FKey Key;
		TArray<FTransform> Transforms;
		TArray<TArray<float>> Data; // per instance, empty when it has none
		UHierarchicalInstancedStaticMeshComponent* Component = nullptr;
		int32 Made = 0; // instances already added to Component
	};

	bool AllowInstanced(UMaterialInterface* Material);
	bool CanInstance(UStaticMesh* Mesh, TArrayView<UMaterialInterface* const> Materials);
	UStaticMeshComponent* SpawnActor(UStaticMesh* Mesh, TArrayView<UMaterialInterface* const> Materials, const FTransform& Transform,
		EFTCellKind Kind, const FFTCellOptions& Options);
	static FIntPoint CellOf(const FVector& At);

	UWorld* World = nullptr;
	const FActorSpawnParameters* Spawn = nullptr;
	TMap<FIntPoint, AActor*> Holders;
	TMap<FKey, int32> GroupOf;
	TArray<FGroup> Groups;
	TMap<UMaterialInterface*, bool> Instanceable;
	TSet<FString> NotInstanceable; // the materials logged by Flush
	TSet<UPackage*> UsageSaves;    // project materials given the instanced usage, saved by Flush
	FStats Totals;
};
