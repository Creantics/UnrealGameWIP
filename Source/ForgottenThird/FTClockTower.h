#pragma once

// A clock tower whose four dials show the game clock (UFTWorldClockSubsystem, ftcore::clockHands).
//
// The massing build spawns it (FTBuildMassingWorld -ClockTower) and gives it its meshes with Setup: the tower, an hour
// hand and a minute hand, each hand mesh pointing at twelve along +Z with its pivot at the origin and its face toward
// +Y. The tower's four dial pivots lie DialOut cm out from its centre at DialHeight cm, on +Y, +X, -Y and -X; each face
// gets one hour and one minute hand, a few mm proud of the dial.
//
// At run time the hands are turned about each dial's outward axis, clockwise as seen from outside, once a second (actor
// tick interval) and only when the minute has moved; nothing allocates per tick. With no running clock (a still with no
// -TimeOfDay) the clock's own hour stands, so the dial reads the subsystem's start hour. The hands never collide.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "FTClockTower.generated.h"

class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;

UCLASS()
class FORGOTTENTHIRD_API AFTClockTower : public AActor
{
	GENERATED_BODY()

public:
	AFTClockTower();

	static constexpr int32 FaceCount = 4;

	// The meshes and the dial geometry (cm). Glow takes the place of any tower slot whose name is in GlowSlots.
	void Setup(UStaticMesh* Tower, UStaticMesh* HourHand, UStaticMesh* MinuteHand, double DialHeight, double DialOut,
		UMaterialInterface* Glow, const TArray<FName>& GlowSlots);

	// Turns the hands to an hour of the clock (0..24).
	void ShowHours(double Hours);

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "FT|Clock")
	TObjectPtr<UStaticMeshComponent> TowerMesh;
	UPROPERTY(VisibleAnywhere, Category = "FT|Clock")
	TArray<TObjectPtr<UStaticMeshComponent>> HourHands;
	UPROPERTY(VisibleAnywhere, Category = "FT|Clock")
	TArray<TObjectPtr<UStaticMeshComponent>> MinuteHands;
	// Each face's yaw in the tower's frame (0: the dial faces +Y).
	UPROPERTY()
	TArray<float> FaceYaw;

	double ShownMinute = -1.0;
};
