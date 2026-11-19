#pragma once

// Imports a parked vehicle (SM_<Name>.fbx and its five 2048 px maps) with its material, from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportVehicle -Source=<game_export dir> [-Name=KeiCar]
//       [-Dest=/Game/Vehicles/<Name>] [-LodScreen=1,0.35,0.12] -AllowCommandletRendering -unattended -nopause -nosplash
//
// <dir> holds SM_<Name>.fbx (LODs under one LOD group, box collision UBX_SM_<Name>_00, front +X, origin at the ground
// centre, centimetres) and T_<Name>_BaseColor/Wear (sRGB), T_<Name>_Normal (glTF green, +Y up, as Blender bakes: the
// texture flips it, the one place it turns), T_<Name>_ORM (AO, roughness, metal) and T_<Name>_Mask (wear, paint, lamp,
// glass), all linear but the first two. A Wheel (or Tyre) slot takes its own five T_<Name>Wheel_* maps. It writes, under
// <Dest>:
//   Textures/T_<Name>_*   the five maps (vehicle texture groups; normal map and mask compression for the linear ones)
//   Materials/M_<Name>    the master (default lit), M_<Name>_ClearCoat (the paint) and M_<Name>_Glass (translucent,
//                         lit per pixel): one graph, the car material contract's maths (PaintColor tints the mask's paint,
//                         DirtAmount blends to the wear map and roughens, LampOn x the window collection's Night x LampGain
//                         lights the lens and sign slots). PaintColor, DirtAmount and LampOn are per-instance custom data
//                         0-2, 3 and 4 (ftcore/ParkedCarLook.h), each defaulting to its parameter when a mesh isn't instanced.
//   Materials/MI_<Name>_<Paint|Trim|Rubber|Metal|Glass|Lens|Interior|Sign|Wheel>   one instance per slot (Slot 0-8; the
//                         kei car has only the first six; Paint is Body in the step-B exports)
//   SM_<Name>             the mesh: its LODs (the kei car's three at screen sizes 1, 0.35, 0.12, or -LodScreen), no
//                         Nanite, one box collision (the file's, else the kei contract's or the bounds from 15 cm up),
//                         each slot on its instance by slot name.
// The kei car (the default name) keeps its paths: /Game/Vehicles/KeiCar, SM_KeiCar. Every file is replaced in place, so
// a refreshed export with the same names is re-run as is. Returns 1 when a file is missing, a map isn't 8 bits, a master
// doesn't compile (-AllowCommandletRendering makes the compile real), or the mesh comes out without its LODs or slots.

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
