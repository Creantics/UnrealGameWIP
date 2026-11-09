#pragma once

// One off-screen still of a game map from a fixed eye: a look-lock frame, or any review frame named by its eye.
//
//   UnrealEditor-Cmd ForgottenThird.uproject /Game/Maps/<map> -game -RenderOffScreen -unattended -nosound
//       -WorldEyeShot=<file.png> -WorldEye=<x,y,z> -WorldEyeAt=<x,y,z> [-WorldEyeLens=35] [-WorldEyeSide=1000]
//       [-WorldEyeSize=2560x1440] [-WorldEyeDelay=8]
//
// The eye and the point it looks at are in the plan frame of a square -WorldEyeSide metres across, the frame the
// massing tables use (ftcore::massingEnginePoint), z in metres. -WorldEyeLens is the focal length in millimetres on a
// 36 mm-wide frame, so 35 mm sees about 54.4 degrees across. The view is held on the eye with the HUD hidden while
// every shader compiles and -WorldEyeDelay seconds pass, then the still is saved at -WorldEyeSize and the game
// quits (1 if the still never arrives). It needs a renderer: a -NullRHI run draws nothing.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "FTWorldEyeShotSubsystem.generated.h"

class ACameraActor;

UCLASS()
class FORGOTTENTHIRD_API UFTWorldEyeShotSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bActive; }

private:
	void Finish(int32 Code);

	UPROPERTY()
	TObjectPtr<ACameraActor> Eye;

	FString File;
	FString Size;
	float Delay = 8.f;
	float Waited = 0.f;
	int32 PendingTicks = 0;
	bool bActive = false;
	bool bShot = false;
};
