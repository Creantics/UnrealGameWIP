// A character-only rim light for gameplay. A spot light on lighting channel 2 alone sits behind the character as
// the active camera sees it, raised and to one side, so the hair, shoulders and arms get a bright edge against the scene.
// Only the owner's own primitives are put on channel 2, so nothing else in the level picks the light up.
// Switched by ft.RimLight: 0 = off (the old look), above 0 = on at that intensity scale.
#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "FTRimLight.generated.h"

class USpotLightComponent;

UCLASS(ClassGroup = (ForgottenThird), meta = (BlueprintSpawnableComponent))
class FORGOTTENTHIRD_API UFTRimLightComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UFTRimLightComponent();

	// The light aims at this component's own location: attach it at chest-to-head height on the character.
	UPROPERTY(EditAnywhere, Category = "Rim")
	float Intensity = 80.f; // candela at ft.RimLight 1

	UPROPERTY(EditAnywhere, Category = "Rim")
	float Back = 220.f; // cm past the character, away from the camera

	UPROPERTY(EditAnywhere, Category = "Rim")
	float Up = 120.f; // cm above the aim point

	UPROPERTY(EditAnywhere, Category = "Rim")
	float Side = 90.f; // cm to the camera's right, so one edge reads brighter than the other

	UPROPERTY(EditAnywhere, Category = "Rim")
	FLinearColor Color = FLinearColor(0.82f, 0.88f, 1.f); // a cool edge, against warm street light

	// False on figures that should never carry it (the arena sets it per figure).
	UPROPERTY(EditAnywhere, Category = "Rim")
	bool bAllowed = true;

	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<USpotLightComponent> Light;

	float ChannelTimer = 0.f;
	float AppliedScale = -1.f;

	void ApplyChannels();
	void MakeLight();
};
