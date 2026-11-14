#pragma once

// FTBuildMassingWorld's parked kei cars: the imported kei car mesh (FTImportVehicle) in place of the box stand-in cars.
//
//   -KeiCars            the kei bays' cars (ftcore::RoadVehicleKind::Kei) become the mesh
//   -KeiCars=All        every parked four-wheeler but the bus becomes the mesh (the kei is shorter than any bay)
//   -KeiCarLamps=<0..1> the share of those cars whose lamps glow at night (default 0: none)
//
// Each car is one instance of its cell (FFTCellInstancer), kind Solid (the mesh's one box collides and shapes the
// navmesh, as the stand-in's body did), cull Car, tag FTVehicle_<kind>, with ftcore::parkedCarLook as its custom data
// (paint rgb, dirt, lamp: ftcore/ParkedCarLook.h). The mesh's origin is the ground centre of the body and its nose is
// +X, which is the stand-in's own pose (the foot's centre on the ground, the nose along the plan heading), so the car
// stands at the vehicle's point with yaw massingEngineYaw(rot) and no offset. Without the switch, or when the mesh isn't
// in the project, the box stand-ins are laid as before.

#include "CoreMinimal.h"

#include "ftcore/Roads.h"

class FFTCellInstancer;
class UStaticMesh;

namespace FTBuildKeiCars
{
// Where FTImportVehicle writes the car (its default -Dest=).
inline constexpr const TCHAR* Folder = TEXT("/Game/Vehicles/KeiCar");
inline constexpr const TCHAR* MeshName = TEXT("SM_KeiCar");

enum class EMode : uint8
{
	Off,
	Kei,
	All
};

struct FSet
{
	EMode Mode = EMode::Off;
	UStaticMesh* Mesh = nullptr;
	float LampShare = 0.f;
	uint32 Seed = 1110;
	int32 Placed = 0;   // cars laid as the mesh
	int32 Fallback = 0; // cars the switch wanted as the mesh but laid as stand-ins (no mesh)
};

// The mesh's object path under Folder (Interchange's <Dest>/<file>/StaticMeshes/<mesh>, <Dest>/StaticMeshes/<mesh>
// or a flat <Dest>/<mesh>); empty when there is none.
FString FindMeshPath();

// Reads the switches from Params and loads the mesh when they ask for it (a warning when it's missing).
FSet Load(const TCHAR* Params, uint32 Seed);

// Lays V as the mesh when the set takes its kind: true when it did (bOk false when the instancer failed), false when
// the caller lays the stand-in.
bool Place(FSet& Set, FFTCellInstancer& Cells, double Side, const ftcore::RoadVehicle& V, bool& bOk);

// One line: how many cars became the mesh and how many fell back.
void LogSummary(const FSet& Set);
} // namespace FTBuildKeiCars
