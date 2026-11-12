#pragma once

// The arena's impact jolt on the camera (ftcore/ArenaImpact.h): the runner sets the view's pitch and yaw offsets each
// frame from its ftcore::ArenaJolt, and this modifier adds them to the camera's view after the camera has placed itself.
// Rotation only: the control rotation, the arm and the field of view are never touched, so the jolt can't pull the
// camera out, and it plays through the hit pause (which holds the control rotation).

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"

#include "FTArenaJoltModifier.generated.h"

UCLASS()
class FORGOTTENTHIRD_API UFTArenaJoltModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	// Degrees: pitch + up, yaw + right. Both 0 once the kick has settled.
	void SetOffsets(float InPitch, float InYaw)
	{
		Pitch = InPitch;
		Yaw = InYaw;
	}

	virtual bool ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV) override;

private:
	float Pitch = 0.f;
	float Yaw = 0.f;
};
