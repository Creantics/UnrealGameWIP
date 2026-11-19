#pragma once
// Off-screen pose stills of any clip on any character: the character (a lead look from config, or a bare skeletal mesh)
// stands in the stub room (/Game/Maps/Stub, a lit floor and wall), holds its clip at each time asked, and is shot from
// each view, framed on its bones (and its prop's; unskinned helpers far off the body don't count) to the narrower of the
// window's two angles, so a square still from a wide window still holds the whole body. Nothing else is in the room: no
// story, HUD or pawn.
//
//   UnrealEditor-Cmd ForgottenThird.uproject "/Game/Maps/Stub?game=/Script/ForgottenThird.FTPoseShotsGameMode"
//       -game -RenderOffScreen -unattended -nosplash -nosound
//       -PoseShots=<out dir> (-PoseLook=<PartyLooks or CastLooks name> | -PoseMesh=<skeletal mesh>) -PoseClip=<anim>
//       [-PoseProp=<skeletal mesh>,<anim>[,<body socket>]] [-PosePieces=<skeletal mesh>+<skeletal mesh>...]
//       -PoseTimes=0.3,2.3 [-PoseViews=front,tq] [-PoseSize=1200x1200] [-PoseYaw=90] [-PoseNear=hand_r:socket_grip_r+...]
//
// -PoseProp: a skinned prop playing its own clip at the same time, on the body's actor (its root where the body's is;
// the clip carries the prop's motion) or, with a socket, riding that body socket. -PosePieces: more pieces worn by leader
// pose (gauntlets). Views (ftcore/AnimImport.h): front, tq, tqr, side, sider, back or name:yaw:pitch, yaw from the
// character's facing (+Y of its mesh, turned by -PoseYaw about Z; 90 faces the room's sun). -PoseNear logs, per time,
// the distance between a body bone and a prop bone (a hand and its grip). Each still is <out>/<clip>_t<s>_<view>.png
// and logs one line "FTPoseShots: still <file> ..." with the assets, the time and the frame. Exits 0 when every still
// is saved, 1 when an asset doesn't load or an argument doesn't parse. Development builds only: in Shipping the class is
// declared but its code is compiled out, so ?game= gives the plain AFTGameMode it derives from, which reads no -Pose flags.
//
// A static mesh instead of a character (no clip, one still per view, <out>/<mesh>_t0.000_<view>.png):
//       -PoseStatic=<static mesh> [-PoseAt=x,y,z] [-PoseDist=<cm>] [-PoseFov=<deg>] [-PoseLod=<n>] [-PoseYaw=90] [-PoseViews=...]
// framed on the mesh bounds, or aimed at -PoseAt (cm in the mesh's own frame) from -PoseDist away. -PoseLod forces LOD n
// (0 = LOD0); without it the engine picks by screen size. Facing is the mesh's +Y turned by -PoseYaw, as for a character.
#include "CoreMinimal.h"
#include "FTGameMode.h"
#include "FTLook.h"
#include "FTPoseShotsGameMode.generated.h"
class ACameraActor;
class UAnimSequence;
class USkeletalMeshComponent;
class UStaticMeshComponent;
UCLASS()
class FORGOTTENTHIRD_API AFTPoseShotsGameMode : public AFTGameMode
{
	GENERATED_BODY()
public:
	AFTPoseShotsGameMode();
	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	bool Setup();
	bool SetupStatic(const FString& MeshPath);
	void SpawnCamera();
	void Pose(double Seconds);
	void Aim(int32 View);
	void Fail(const FString& Why);

	UPROPERTY()
	TObjectPtr<AActor> Subject;
	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> Body;
	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> Prop;
	UPROPERTY()
	TArray<TObjectPtr<USkeletalMeshComponent>> Pieces;
	UPROPERTY()
	TArray<TObjectPtr<USceneComponent>> Grooms;
	UPROPERTY()
	FFTWornFronts Fronts;
	UPROPERTY()
	TObjectPtr<UAnimSequence> Clip;
	UPROPERTY()
	TObjectPtr<UAnimSequence> PropClip;
	UPROPERTY()
	TObjectPtr<ACameraActor> Camera;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Still;

	FString OutDir;
	// The still's base name: the clip's, or the static mesh's.
	FString StillBase;
	float Fov = 30.f;
	// -PoseDist (cm, 0 = frame the bounds) and -PoseAt (cm in the mesh's frame).
	double FixedDist = 0.0;
	FVector AimAt = FVector::ZeroVector;
	bool bAimAt = false;
	FString Size = TEXT("1200x1200");
	FString Describe;
	TArray<double> Times;
	struct FView
	{
		FString Name;
		double Yaw = 0.0;
		double Pitch = 0.0;
	};
	TArray<FView> Views;
	TArray<TPair<FName, FName>> Near;
	int32 TimeIndex = 0;
	int32 ViewIndex = 0;
	int32 Wait = 0;
	FString Pending;
	int32 PendingTicks = 0;
	double AimedHalf = 0.0;  // the half angle (radians) the camera was last aimed to; the window may size after Setup's aim
	bool bActive = false;
};
