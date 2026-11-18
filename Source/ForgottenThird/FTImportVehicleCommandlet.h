#pragma once

// Imports the parked kei car (SM_KeiCar.fbx and its five 2048 px maps) with its material, from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportVehicle -Source=<game_export dir>
//       [-Dest=/Game/Vehicles/KeiCar] -AllowCommandletRendering -unattended -nopause -nosplash
//
// <dir> holds SM_KeiCar.fbx (LOD0-2 under one LOD group, box collision UBX_SM_KeiCar_00, front +X, origin at the ground
// centre, centimetres) and T_KeiCar_BaseColor/Wear (sRGB), T_KeiCar_Normal (DirectX, no flip), T_KeiCar_ORM (AO,
// roughness, metal) and T_KeiCar_Mask (wear, paint, lamp, glass), all linear but the first two. It writes, under <Dest>:
//   Textures/T_KeiCar_*   the five maps (vehicle texture groups; normal map and mask compression for the linear ones)
//   Materials/M_KeiCar    the master (default lit), M_KeiCar_ClearCoat (the paint) and M_KeiCar_Glass (translucent,
//                         lit per pixel): one graph, the car material contract's maths (PaintColor tints the mask's paint,
//                         DirtAmount blends to the wear map and roughens, LampOn x the window collection's Night x LampGain
//                         lights the lens slot). PaintColor, DirtAmount and LampOn are per-instance custom data 0-2, 3 and
//                         4 (ftcore/ParkedCarLook.h), each defaulting to its parameter when a mesh isn't instanced.
//   Materials/MI_KeiCar_<Paint|Trim|Rubber|Metal|Glass|Lens>   one instance per slot (Slot 0-5)
//   SM_KeiCar             the mesh: three LODs (screen sizes 1, 0.35, 0.12), no Nanite, one box collision (the file's,
//                         else the contract's box), each slot on its instance by slot name.
// Every file is replaced in place, so a refreshed export with the same names is re-run as is. Returns 1 when a file is
// missing, a map isn't 8 bits, a master doesn't compile (-AllowCommandletRendering makes the compile real), or the mesh
// comes out without its three LODs or six slots.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTImportVehicleCommandlet.generated.h"

UCLASS()
class UFTImportVehicleCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTImportVehicleCommandlet();

	virtual int32 Main(const FString& Params) override;
};
