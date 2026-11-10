#pragma once

// One off-screen still of a game map from a fixed eye: a look-lock frame, or any review frame named by its eye.
//
//   UnrealEditor-Cmd ForgottenThird.uproject /Game/Maps/<map> -game -RenderOffScreen -unattended -nosound
//       -WorldEyeShot=<file.png> -WorldEye=<x,y,z> -WorldEyeAt=<x,y,z> [-WorldEyeLens=35] [-WorldEyeSide=1000]
//       [-WorldEyeSize=2560x1440] [-WorldEyeDelay=8] [-WorldEyeNight [-WorldEyeNightSun=0.03] [-WorldEyeNightSky=0.1]
//       [-WorldEyeNightEV=-2] [-WorldEyeNightLamp=25] [-WorldEyeNightLampRadius=18] [-WorldEyeNightShop=15]
//       [-WorldEyeNightGlow=1]] [-WorldEyeGpuMs=<frames>] [-WorldEyePlainWalls]
//
// -WorldEyeNight sets the windows' Night to 1 (FTScene::WindowCollection) and their NightGlow to -WorldEyeNightGlow when
// given, turns the sun into a cool moon at -WorldEyeNightSun of the map's, leaves a blue sky fill of -WorldEyeNightSky of
// the map's (net: a real-time capture already dims with the sun, so it isn't dimmed twice), switches on the street lamps
// (FTScene::NightLampTag, -WorldEyeNightLamp candelas, warm, reaching -WorldEyeNightLampRadius metres) and the shop
// spill lights (FTScene::NightShopTag, -WorldEyeNightShop candelas), puts FTScene::NightLampMaterial on the lamp heads
// (FTScene::NightLampHeadTag; while a map has none, a small glowing lens stands in over each lamp's light) and locks the
// exposure at -WorldEyeNightEV (EV100). On a map whose lights are data (AFTNightLights) the lamp pool lights the lamps
// and shops nearest the eye instead (UFTLampPoolSubsystem::ForceNight, at the same candelas and reach). -WorldEyeGpuMs logs the GPU's frame time, averaged over that many frames once
// the view has settled, before the still. -WorldEyePlainWalls puts each window instance's surface instance (MIW_<id> to
// MI_Surface_<id>) back on the meshes, for a like-for-like timing.
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
	// The night and plain-walls switches, once every actor has begun play.
	void ApplyLook(UWorld& World);

	UPROPERTY()
	TObjectPtr<ACameraActor> Eye;

	FString File;
	FString Size;
	float Delay = 8.f;
	float Waited = 0.f;
	int32 PendingTicks = 0;
	int32 GpuFrames = 0;
	int32 GpuSampled = 0;
	double GpuSumMs = 0.0;
	double GpuMinMs = 0.0;
	double GpuMaxMs = 0.0;
	bool bLookApplied = false;
	bool bActive = false;
	bool bShot = false;
};
