#pragma once

// A map's night lights as data (FTBuildMassingWorld -NightLamps): no light actor per lamp or shopfront. The build
// spawns one AFTNightLights holding a UFTNightLightData (every lamp's light point, every shopfront's spill point and
// yaw, and the light values both kinds take) and one instanced mesh of every lamp's lens (LampHeads, wearing
// FTScene::NightLampMaterial, tagged FTScene::NightLampHeadTag). At run time a small pool of real spot lights moves to
// the lamps and shops nearest the camera at night (FTLampPoolSubsystem); every other lamp shows only its glowing lens.

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"

#include "FTNightLightData.generated.h"

class UInstancedStaticMeshComponent;

// The build's light values, copied into each map's UFTNightLightData so the runtime reads them from the map.
namespace FTNightLight
{
// A street lamp: a sodium spot aimed straight down from under its head.
inline constexpr float LampCandela = 400.f;  // under a head about 3.3 m up: some 35 lux at its foot
inline constexpr float LampKelvin = 2100.f;  // high-pressure sodium
inline constexpr float LampReach = 1800.f;   // cm: the attenuation radius
inline constexpr float LampOuter = 70.f;     // degrees: the cone's half-angles, a wide pool off a low head
inline constexpr float LampInner = 35.f;
inline constexpr float LampSource = 8.f;     // cm: the lens' radius, for soft highlights
// A shopfront's spill: a wide spot out of the face and down, warmer-white than the lamps. The cone's top edge
// (pitch + outer) stays at the horizontal, so it lights the street and the walls across it only up to its own height.
inline constexpr float ShopCandela = 150.f;
inline constexpr float ShopKelvin = 3500.f;
inline constexpr float ShopReach = 900.f;    // cm
inline constexpr float ShopOuter = 55.f;     // degrees
inline constexpr float ShopInner = 35.f;
inline constexpr float ShopSource = 40.f;    // cm: a shop window's glow, soft
inline constexpr float ShopPitch = -55.f;    // degrees under the horizontal
}

// Every lamp and shop light point of a map, world cm, and the values their lights take.
UCLASS(ClassGroup = (ForgottenThird))
class FORGOTTENTHIRD_API UFTNightLightData : public UActorComponent
{
	GENERATED_BODY()

public:
	UFTNightLightData();

	// The light point under each lamp's head; a spot there aims straight down.
	UPROPERTY()
	TArray<FVector3f> LampAt;
	// Each shopfront's spill point; a spot there faces ShopYaw (degrees, same index) pitched ShopPitch.
	UPROPERTY()
	TArray<FVector3f> ShopAt;
	UPROPERTY()
	TArray<float> ShopYaw;

	UPROPERTY()
	float LampCandela;
	UPROPERTY()
	float LampKelvin;
	UPROPERTY()
	float LampReach; // cm
	UPROPERTY()
	float LampOuter;
	UPROPERTY()
	float LampInner;
	UPROPERTY()
	float LampSource;

	UPROPERTY()
	float ShopCandela;
	UPROPERTY()
	float ShopKelvin;
	UPROPERTY()
	float ShopReach; // cm
	UPROPERTY()
	float ShopOuter;
	UPROPERTY()
	float ShopInner;
	UPROPERTY()
	float ShopSource;
	UPROPERTY()
	float ShopPitch;
};

// One per map: the night light data and the lamps' lens instances.
UCLASS()
class FORGOTTENTHIRD_API AFTNightLights : public AActor
{
	GENERATED_BODY()

public:
	AFTNightLights();

	UPROPERTY(VisibleAnywhere, Category = "Night")
	TObjectPtr<UFTNightLightData> Data;

	// One instance per lamp lens, wearing FTScene::NightLampMaterial; no collision, no shadow, no navigation; the
	// component carries FTScene::NightLampHeadTag.
	UPROPERTY(VisibleAnywhere, Category = "Night")
	TObjectPtr<UInstancedStaticMeshComponent> LampHeads;
};
