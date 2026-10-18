#include "FTPawn.h"

#include "Camera/CameraComponent.h"
#include "FTConductorComponent.h"

AFTPawn::AFTPawn()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Root);

	Conductor = CreateDefaultSubobject<UFTConductorComponent>(TEXT("Conductor"));
}
