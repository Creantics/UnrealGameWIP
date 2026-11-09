#pragma once

// Puts the sockets the leads' weapon props ride on their assets, from the props' socket files, so no socket is placed by
// hand:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTWeaponSockets -Gauntlets=<gauntlet socket .json>
//       -Pistol=<pistol socket .json> [-Devin=<look> -Duy=<look> | -DevinBody=<mesh> -DuyBody=<mesh>]
//       [-PistolMesh=<static mesh>] [-DryRun] -NullRHI -unattended -nopause -nosplash
//
// Run it after FTImportProp has brought the props into /Game/Props/Weapons (FTLeadWeapons.h names the meshes, the
// sockets and their bones). It writes:
//   - on the gauntlet lead's body (the PartyLooks entry -Devin names, Devin by default, or the mesh -DevinBody names), the
//     mesh-only sockets ft_gauntlet_vambrace_r/l (lowerarm_r/l) and ft_gauntlet_plate_r/l (hand_r/l);
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
// Gauntlets: each piece is modelled in place on the body at rest, its origin at its bone's head and its axes the body's
// component axes. So its socket sits at the file's bone-local offset (zero) with the rotation that undoes the bone's rest
// rotation, the inverse of the bone's component-space reference rotation, worked out from this body's own reference
// skeleton rather than copied. The log gives the file's quat_xyzw against it (degrees apart), the bone's rest position
// against the file's comp_loc_cm (cm apart), and the vambrace's long direction (its bounds' far corner) against the
// forearm (lowerarm to hand); if the vambrace lies better along the forearm turned a quarter or half turn about Z, the
// pieces of that side get that turn first, with a warning.
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
