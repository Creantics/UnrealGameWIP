#pragma once

// Which way an imported character faces. The glTF imports don't agree on a forward axis, so the
// facing is read off the reference pose: the direction from the head bone to between the eyes, or
// on a body without eyes, from the ankles to the balls of the feet.

#include "CoreMinimal.h"

class USkeletalMesh;

namespace FTMeshFacing
{
// The yaw that turns the mesh to face +X. Zero when the mesh has neither head and eye bones nor
// foot and ball bones.
FORGOTTENTHIRD_API float FacingYaw(const USkeletalMesh& Mesh);
}
