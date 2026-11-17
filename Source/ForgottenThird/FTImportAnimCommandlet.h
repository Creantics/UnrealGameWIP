#pragma once

// Imports a clip from an FBX onto a skeleton that already exists (no mesh, no retarget), so a clip authored on a body's
// own skeleton (a hand-keyed take, a swim set) comes in from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportAnim -Source=<file.fbx> -Skeleton=<skeleton or skeletal mesh>
//       -Dest=/Game/Characters/<Lead>/Anims -Name=A_Clip [-Socket=<spec>]... [-Fps=60] [-RootLock] [-MinTrackShare=0.9]
//       -NullRHI -unattended -nopause -nosplash
//
// -Skeleton names a USkeleton or a USkeletalMesh (its skeleton is used). Before importing, the FBX's bones (its LimbNode
// models, ftcore/AnimImport.h) are checked against the skeleton: any bone the skeleton lacks is listed and nothing is
// imported. The clip keeps every key (no redundant-key removal) at the file's own rate (a 60 fps cinematic stays at 60,
// a 30 fps gameplay clip at 30; never the engine's default rate); -Fps resamples to that rate instead. After the
// import it logs the clip's frames, rate, length and bone tracks, and fails when the tracks are fewer than
// -MinTrackShare of the file's bones (0.9 by default). -RootLock locks the root (no root motion, the root held at its
// first key). A clip of the same name in Dest is deleted first, so a re-import replaces it under the same name (soft
// references by path keep working).
//
// -Socket (repeatable) puts a socket on the skeleton, replacing one of the same name:
//   name,bone,x,y,z[,roll,pitch,yaw]   bone-local, cm and degrees;
//   name,bone,cs,x,y,z[,ax,ay,az]      a point in the reference pose's component space, with the socket's +X along
//                                      (ax, ay, az) there; it is turned into the bone's frame. Both logged.
// Returns 1 when a file or the skeleton is missing, the file has a bone the skeleton lacks, the import gives no clip
// or too few tracks, a socket's bone is missing, or a save fails.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTImportAnimCommandlet.generated.h"

UCLASS()
class UFTImportAnimCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTImportAnimCommandlet();

	virtual int32 Main(const FString& Params) override;
};
