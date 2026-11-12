#include "FTArenaJoltModifier.h"

#include "Camera/CameraTypes.h"

bool UFTArenaJoltModifier::ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
	Super::ModifyCamera(DeltaTime, InOutPOV);
	if (Pitch != 0.f || Yaw != 0.f)
	{
		InOutPOV.Rotation.Pitch += static_cast<double>(Pitch);
		InOutPOV.Rotation.Yaw += static_cast<double>(Yaw);
	}
	return false; // the modifiers after this one still run
}
