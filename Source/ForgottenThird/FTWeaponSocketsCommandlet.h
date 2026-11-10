#pragma once

// Puts the sockets the leads' weapon props ride on their assets, from the props' socket files, so no socket is placed by
// hand:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTWeaponSockets -Pistol=<pistol socket .json>
//       [-Duy=<look> | -DuyBody=<mesh>]
//       [-PistolMesh=<static mesh>] [-DryRun] -NullRHI -unattended -nopause -nosplash
//
// Run it after FTImportProp has brought the pistol into /Game/Props/Weapons (FTLeadWeapons.h names the mesh, the
// sockets and their bones). The gauntlets are skinned pieces on the body's skeleton and need no socket. It writes:
//   - on the gun lead's body (-Duy, Duy by default, or -DuyBody), ft_pistol_grip_r (hand_r) and ft_pistol_holster_r
//     (thigh_r);
//   - on the pistol mesh (found as FTLeadWeapons finds it, or -PistolMesh), the socket `muzzle` at the barrel's end, its
//     +X along the barrel.
// A socket of the same name is replaced; the skeleton's own sockets are left alone. -DryRun works everything out and logs
// it without saving. Each socket logs one line, "FTWeaponSockets: <asset> <socket> on <bone>: ...", with its checks.
// Returns 1 when a file, mesh, bone or file entry is missing, or a save fails.
//
// The socket files are measured in Blender (right-handed, Z up). Their axes, and the ones used here:
//   - F, the import frame. Both importers put a Blender point (x, y, z) at Unreal (x, -y, z) * 100: the props come in as
//     glTF (exported +Y up, which stores (x, z, -y); Interchange reads a glTF (x, y, z) as Unreal (x, z, y)) and the
//     bodies as FBX, which lands the same way. Bones and prop vertices go through the same F, so a transform between
//     them measured in Blender carries over unchanged: the files' bone-local columns (loc_cm, quat_xyzw) are F already,
//     and they are what is used. A body faces +Y in its component space.
//   - The files' comp_* columns (and the "+X is the barrel" of the pistol's notes) are in a frame turned 90 degrees from
//     F about Z: a point (a, b, c) there is (-b, a, c) in F. They are used only to check, turned into F first.
// Pistol: the barrel's direction in the imported mesh is read from its bounds (the longer horizontal side, toward its far
// end, the grip sitting near the origin) as a yaw of 0, 90, 180 or -90 degrees: 90 (+Y) under F, a warning otherwise.
// The muzzle socket is the file's muzzle point turned by that yaw about Z, rotated by it. The grip and holster sockets
// are the file's bone-local transforms with a turn of (90 - yaw) about Z applied first, in the mesh's own frame, which
// leaves them as they are at 90. The log checks each against the file's comp_* columns (position and rotation).

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTWeaponSocketsCommandlet.generated.h"

UCLASS()
class UFTWeaponSocketsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTWeaponSocketsCommandlet();

	virtual int32 Main(const FString& Params) override;
};
