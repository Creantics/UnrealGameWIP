#pragma once
// Off-screen pose stills of any clip on any character: the character (a lead look from config, or a bare skeletal mesh)
// stands in the stub room (/Game/Maps/Stub, a lit floor and wall), holds its clip at each time asked, and is shot from
// each view, framed on its bones (and its prop's). Nothing else is in the room: no story, HUD or pawn.
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
// is saved, 1 when an asset doesn't load or an argument doesn't parse.
#include "CoreMinimal.h"
#include "FTGameMode.h"
#include "FTLook.h"
#include "FTPoseShotsGameMode.generated.h"
class ACameraActor;
class UAnimSequence;
class USkeletalMeshComponent;
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

	FString OutDir;
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
	bool bActive = false;
};
