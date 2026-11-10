#pragma once

// The party's own vehicle in Solhaven (the micro-van): a thin host around ftcore::VehicleDrive and
// ftcore::VehicleRules. The body box is swept along the move Core reads back; a wall it hits goes to
// collide() and to the cosmetic wear; four wheel traces carry the wheels and tilt the body, and a
// rise under a front wheel is a kerb (mountKerb). Wheels spin and steer from Core's state.
//
// The lead never stops being the possessed pawn: AFTExploreCharacter hands its move, look, jump and
// interact input to the van while seated (TryEnter / Leave), and the view goes to the van's chase
// camera. Seat poses for the three leads are a later pass; the party is hidden while aboard.
//
// Meshes come from soft paths (BodyMesh, WheelMesh) and fall back to the engine's basic shapes, so
// the van drives before the import. Tunables: Data/Vehicles/microvan.json and vehicle_rules.json.
//
// -VanCourse (Explore map) lays a flat 200 m course in the sky (a straight with a 12 cm kerb strip on
// the right, a left turn, a marker every 25 m) with the van at its start; -VanDrive drives it on a
// fixed script (in, the kerb touch, the turn, stop, out), logs VANCHECK lines (the speed limit held,
// no overlap after a move, each wheel on the ground within 2 cm), takes -VanShots=<prefix> views
// side-on, behind and top-down, and quits with 0 on a pass. The limit is 60 km/h, 80 past 100 m.
//
// -PartyVanAt=X,Y,Yaw (cm, degrees) spawns the van in any map at that point, set on the ground (SpawnAt, called
// from the lead's BeginPlay). With -VanDrive it drives a straight route of -VanRoute=<m> (default 80) from there
// at up to 40 km/h on the same script and checks, without the kerb.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ftcore/VehicleDrive.h"
#include "ftcore/VehicleRules.h"

#include "FTPartyVan.generated.h"

class ACharacter;
class AFTExploreCharacter;
class UBoxComponent;
class UCameraComponent;
class UMaterialInstanceDynamic;
class UPointLightComponent;
class USpotLightComponent;
class USpringArmComponent;
class UStaticMeshComponent;

UCLASS(Config = Game, NotBlueprintable)
class FORGOTTENTHIRD_API AFTPartyVan : public AActor
{
	GENERATED_BODY()

public:
	AFTPartyVan();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// The lead asks to get in (interact near the van). True when boarding started.
	bool TryEnter(AFTExploreCharacter* Lead);
	bool IsAboard(const AFTExploreCharacter* Lead) const { return Driver.Get() == Lead && Lead != nullptr; }

	// Input from the seated lead: move axis (x steer, y throttle/brake), look, handbrake, exit held.
	void SetDriveAxis(const FVector2D& Axis) { DriveAxis = Axis; bAxisFresh = true; }
	void AddLook(const FVector2D& Axis) { LookYaw = FMath::Clamp(LookYaw + Axis.X, -150.f, 150.f); LookRest = 0.f; }
	void SetHandbrake(bool bOn) { bHandbrake = bOn; }
	void SetExitHeld(bool bHeld) { bExitHeld = bHeld; }

	// The road's limit (km/h): 60 in town, 80 on the coast road. Set by whatever knows the road.
	UPROPERTY(EditAnywhere, Category = "Vehicle")
	float LimitKmh = 60.f;

	// Right-hand drive: the driver sits and gets out on the right (+Y).
	UPROPERTY(Config, EditAnywhere, Category = "Vehicle")
	bool bRightHandDrive = true;

	UPROPERTY(Config, EditAnywhere, Category = "Vehicle")
	FSoftObjectPath BodyMesh = FSoftObjectPath(TEXT("/Game/Vehicles/PartyVan/SM_PartyVan.SM_PartyVan"));
	UPROPERTY(Config, EditAnywhere, Category = "Vehicle")
	FSoftObjectPath WheelMesh = FSoftObjectPath(TEXT("/Game/Vehicles/PartyVan/SM_PartyVanWheel.SM_PartyVanWheel"));

	// Geometry from the prep (cm): wheel radius, hub positions, the body box above the ground clearance.
	UPROPERTY(Config, EditAnywhere, Category = "Vehicle")
	float WheelRadius = 29.1f;
	UPROPERTY(Config, EditAnywhere, Category = "Vehicle")
	float HalfWheelbase = 103.1f;
	UPROPERTY(Config, EditAnywhere, Category = "Vehicle")
	float HalfTrack = 59.9f;
	UPROPERTY(Config, EditAnywhere, Category = "Vehicle")
	FVector BodyBox = FVector(340.f, 148.f, 190.f);
	UPROPERTY(Config, EditAnywhere, Category = "Vehicle")
	float Clearance = 16.f;

	// Lays the -VanCourse course and spawns a van at its start. Returns the van.
	static AFTPartyVan* SpawnCourse(UWorld* World);
	// -PartyVanAt: spawns the van there unless one is already in the world. Returns it, or null.
	static AFTPartyVan* SpawnAt(UWorld* World);

private:
	void LoadData();
	void PlaceOnGround(bool bFirst, float Moved = 0.f);
	// The nearest pedestrian ahead (a character on foot or a crowd agent), in the van's frame from its front bumper
	// (cm). Ahead is ftcore::kNobodyAhead when there is none.
	void SensePedestrian(float& Ahead, float& Side, AActor*& Who, int32& CrowdSlot);
	void ReactPedestrian(float Ahead, float Side, AActor* Who, int32 CrowdSlot);
	void ClearHeldInput();
	void MoveBody(float DeltaSeconds);
	void TickSeats(float DeltaSeconds);
	void TickScript(float DeltaSeconds);
	void TickChecks();
	void Shot(const TCHAR* Name, float CamYaw, float CamPitch, float Arm);
	void SetupLook(bool bBodyAsset);
	void TickLamps(float DeltaSeconds);
	void ShowWear();
	bool SideClear(float Sign) const;
	void FinishExit();

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UBoxComponent> Box;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Tilt;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> Body;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> Wheels[4]; // FL, FR, RL, RR
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USpringArmComponent> Boom;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> Lens[4]; // head lamps L, R, tail lamps L, R
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USpotLightComponent> HeadBeam[2];
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UPointLightComponent> TailGlow;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BodyLook; // M_VanBody on the body's painted slot: glass and wear
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> LensLook[2]; // head, tail (each pair shares one)

	ftcore::VehicleDrive Drive;
	ftcore::VehicleState State;
	ftcore::VehicleRules Rules;
	ftcore::WearState Wear;
	ftcore::PartySeats Seats;

	TWeakObjectPtr<AFTExploreCharacter> Driver;
	FVector2D DriveAxis = FVector2D::ZeroVector;
	bool bAxisFresh = false;
	bool bHandbrake = false;
	bool bExitHeld = false;
	float LookYaw = 0.f;
	float LookRest = 0.f;
	float Spin = 0.f;          // wheel roll (degrees)
	float GroundZ = 0.f;       // under the pivot (cm)
	float WheelGround[4] = {0.f, 0.f, 0.f, 0.f};
	bool WheelHit[4] = {false, false, false, false};
	float WheelGrade[4] = {0.f, 0.f, 0.f, 0.f}; // the front wheels' last ground slope (rise per cm), for kerb edges
	float Lift = 0.f;          // the body box raised to the highest wheel's ground (cm)
	TWeakObjectPtr<AActor> LastPedChar; // the last one reacted to (once per approach)
	// Lamps: night from the clock every LookEvery s, the lamp state each frame, applied only when it changes.
	float LookAge = 0.f;
	bool bNight = false;
	bool bLampsSet = false;
	ftcore::LampState Lamps;
	float LampBrake = 0.f; // this frame's brake input and handbrake, after the seats' hold
	bool bLampHandbrake = false;
	int32 WearShown[5] = {-1, -1, -1, -1, -1}; // the stages on the body's instance: front, rear, left, right, dirt
	TArray<TWeakObjectPtr<AActor>> PedCache; // characters and followers on foot, refreshed every PedRefresh s (reserved once)
	float PedCacheAge = 1e9f;
	int32 LastPedSlot = -1;
	ftcore::SeatPhase LastPhase = ftcore::SeatPhase::OnFoot;
	float BodyHalfZ = 87.f;

	// -VanDrive
	bool bScript = false;
	bool bCourse = false;
	bool bMapRoute = false;    // -PartyVanAt with -VanDrive: a straight route from the spawn
	float RouteLen = 8000.f;   // (cm)
	float TraceAge = 0.f;      // scripted runs: seconds since the last VANTRACE line
	// -VanLoop=X:Y,X:Y,...: a closed route through these engine points (cm) from the spawn and back to it, in the
	// course frame (relative to the start, its heading along +X). LoopCum: the distance along to each point.
	static constexpr int32 MaxLoop = 16;
	FVector2D Loop[MaxLoop + 2];
	float LoopCum[MaxLoop + 2] = {};
	int32 LoopN = 0;
	FVector2D RoutePoint(float S) const;
	float RouteDistance(const FVector2D& P, float Near) const;
	float CornerSpeed(float Along) const;
	FString ShotPrefix;
	float ScriptTime = 0.f;
	int ScriptStage = 0;
	float Travelled = 0.f;     // path length driven (cm)
	FVector2D Start = FVector2D::ZeroVector;
	float StartHeading = 0.f;
	float Peak = 0.f;          // highest speed seen (cm/s)
	int Fails = 0;
	int Shots = 0;
	float NextLog = 0.f;
	bool bKerbTouched = false;
	float LastShotAt = -10.f;
	FString ShotPending;
	bool bShotTaken = false;
	float EndAt = 0.f;
	float ExitSign = 1.f;
};
