#include "FTArenaJoltModifier.h"

#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"

bool UFTArenaJoltModifier::ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
	Super::ModifyCamera(DeltaTime, InOutPOV);
	// A paused world (a menu) holds the runner, and with it the kick's settling: the view drops the offset rather
	// than freezing mid-kick, and takes the kick's remainder back on resume.
	const UWorld* World = CameraOwner ? CameraOwner->GetWorld() : nullptr;
	const bool bPaused = World && World->IsPaused();
	AppliedPitch = bPaused ? 0.f : Pitch;
	AppliedYaw = bPaused ? 0.f : Yaw;
	if (AppliedPitch != 0.f || AppliedYaw != 0.f)
	{
		InOutPOV.Rotation.Pitch += static_cast<double>(AppliedPitch);
		InOutPOV.Rotation.Yaw += static_cast<double>(AppliedYaw);
	}
	return false; // the modifiers after this one still run
}
