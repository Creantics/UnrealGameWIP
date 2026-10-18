#pragma once

// A headless physics check of the demo map:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTPhysicsSim -NullRHI -unattended -nopause -nosplash
//
// Loads /Game/Maps/Demo as a game world with a physics scene, begins play, and ticks it at 60 Hz. Logs where
// each physics-corner prop (the Phys_* actors) starts and comes to rest, its speed and whether it sleeps, and
// the rubber ball's bounce. It also spawns the proof head with a UFTPhysicsComponent away from everything,
// knocks it out with the ragdoll asset FTBuildPhysics wrote, and logs where its bodies settle.
// Fails (exit 1) when a prop or ragdoll body ends below the floor or is still moving fast, or the KO fails.
//
//   -Seconds=<n>  simulated time (default 4)
//   -NoRagdoll    skip the knockout

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTPhysicsSimCommandlet.generated.h"

UCLASS()
class UFTPhysicsSimCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTPhysicsSimCommandlet();

	virtual int32 Main(const FString& Params) override;
};
