#pragma once

// One off-screen still of a game map from a fixed eye: a look-lock frame, or any review frame named by its eye.
//
//   UnrealEditor-Cmd ForgottenThird.uproject /Game/Maps/<map> -game -RenderOffScreen -unattended -nosound
//       -WorldEyeShot=<file.png> -WorldEye=<x,y,z> -WorldEyeAt=<x,y,z> [-WorldEyeLens=35] [-WorldEyeSide=1000]
//       [-WorldEyeSize=2560x1440] [-WorldEyeDelay=8] [-WorldEyeNight [-WorldEyeNightSun=<share>] [-WorldEyeNightSky=<share>]
//       [-WorldEyeNightEV=<ev>] [-WorldEyeNightEVRange=<ev>] [-WorldEyeNightLamp=<cd>] [-WorldEyeNightLampRadius=<m>] [-WorldEyeNightShop=<cd>]
//       [-WorldEyeNightShopRadius=<m>] [-WorldEyeNightGlow=1]] [-WorldEyeGpuMs=<frames>] [-WorldEyePlainWalls]
//       [-WorldEyeSunHour=<hours>] [-WindowParallax=<0..1>]
//       [-WorldEyeWalkTo=<x,y,z> [-WorldEyeWalkSpeed=1.4] [-WorldEyeWalkFps=60]]
//       [-WorldEyeFocal=<mm>] [-WorldEyeFocus=<x,y,z>|<m>] [-WorldEyeAperture=<f>] [-WorldEyeBlur=0.5]
// Any of -WorldEyeFocal, -WorldEyeFocus or -WorldEyeAperture switches to the vista camera standard: a cine camera on a
// full-frame 36 x 24 mm filmback, focal length -WorldEyeFocal (default 50, 35 to 135), focus on a plan point or at a
// distance in metres (default the -WorldEyeAt point), aperture f/-WorldEyeAperture (default 4.8) and motion blur
// -WorldEyeBlur (default 0.5). Without them the plain camera at -WorldEyeLens as before.
//
// A day still of a Solhaven map turns the directional lights to the clock's sun at its start hour
// (ftcore::kWorldClockStartHours), or at -WorldEyeSunHour on any map, unless a running clock (-TimeOfDay) lights it; a
// night still's moon stands where the clock puts it at midnight (or at -WorldEyeSunHour).
// -WorldEyeNight takes its values from the night spec the running clock reaches at full night (FTNightLook.h and its
// -Night* switches), so a still matches a played night; the -WorldEyeNight* values override them for the still alone,
// and it also gives the height fog the spec's kept colour and city glow. -WorldEyeNight sets the windows' Night to 1 (FTScene::WindowCollection) and their NightGlow to -WorldEyeNightGlow when
// given, turns the sun into a cool moon at -WorldEyeNightSun of the map's, leaves a blue sky fill of -WorldEyeNightSky of
// the map's (net: a real-time capture already dims with the sun, so it isn't dimmed twice), switches on the street lamps
// (FTScene::NightLampTag, -WorldEyeNightLamp candelas, warm, reaching -WorldEyeNightLampRadius metres) and the shop
// spill lights (FTScene::NightShopTag, -WorldEyeNightShop candelas), puts FTScene::NightLampMaterial on the lamp heads
// (FTScene::NightLampHeadTag; while a map has none, a small glowing lens stands in over each lamp's light), holds the
// auto exposure to -WorldEyeNightEV +- -WorldEyeNightEVRange (EV100) and sets the night's white balance. On a map whose lights are data (AFTNightLights) the lamp pool lights the lamps
// and shops nearest the eye instead (UFTLampPoolSubsystem::ForceNight, at the same candelas and reach). -WorldEyeGpuMs logs the GPU's frame time, averaged over that many frames once
// the view has settled, before the still. -WorldEyePlainWalls puts each window instance's surface instance (MIW_<id> to
// MI_Surface_<id>) back on the meshes, for a like-for-like timing.
//
// The eye and the point it looks at are in the plan frame of a square -WorldEyeSide metres across, the frame the
// massing tables use (ftcore::massingEnginePoint), z in metres. -WorldEyeLens is the focal length in millimetres on a
// 36 mm-wide frame, so 35 mm sees about 54.4 degrees across. The view is held on the eye with the HUD hidden while
// every shader compiles and -WorldEyeDelay seconds pass, then the still is saved at -WorldEyeSize and the game
// quits (1 if the still never arrives). It needs a renderer: a -NullRHI run draws nothing.
// -WorldEyeWalkTo: once settled, the eye moves in a straight line to that point at -WorldEyeWalkSpeed metres a second,
// facing as it started, one step and one frame (<file>_f0001.png ...) a tick at -WorldEyeWalkFps steps a second, so the
// frames play back as a walk past whatever the machine's own frame rate was; the still is then saved from the far end.
// -WorldEyeLeadCam [-WorldEyeLeadYaw=<deg>] [-WorldEyeLeadPitch=<deg>] [-WorldEyeFrames=<n>] [-WorldEyeLeadGo=<x,y>]
// [-WorldEyeLeadDip=<pitch>,<from>,<to>]: the still, or n frames, through the lead's own camera instead of the eye's, the
// lead walking at a plan point and the camera's pitch dipped and brought back over the frames (the .cpp's note).
// -WorldEyeLeadGo=<x,y> -WorldEyeLeadGoEarly=<s> [-WorldEyeLeadGoJump] without -WorldEyeLeadCam: the lead walks at the
// point from <s> seconds before the wait ends, so a still from the eye catches it moving.
// -WorldEyeLeadRoute=<x,y>;<x,y>...: further points the lead walks at in turn after -WorldEyeLeadGo's.
// -WorldEyeList=<file>: more eyes in the same run, each shot like the first (the .cpp's note has the line's form).

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
	TArray<float> GpuSamples; // the sampled frames' GPU ms, for the median, p95 and spikes (reserved once)
	bool bLookApplied = false;
	bool bActive = false;
	bool bShot = false;
	bool bWalk = false;
	FVector WalkFrom = FVector::ZeroVector;
	FVector WalkTo = FVector::ZeroVector;
	int32 WalkFrames = 0;
	int32 WalkFrame = 0;
	int32 WalkEvery = 1; // -WorldEyeEvery: a walk saves every n-th frame
	// -WorldEyeLeadCam and its script (the .cpp's note).
	bool bLeadCam = false;
	bool bDip = false;
	bool bLeadGo = false;
	float LeadYaw = 0.f;
	float LeadPitch = -10.f;
	float DipPitch = 0.f;
	float DipFrom = 0.4f;
	float DipTo = 0.9f;
	FVector LeadGoTo = FVector::ZeroVector;
	TArray<FVector> LeadRoute; // -WorldEyeLeadRoute: the walk's further points
	// -WorldEyeLeadGo from the eye (-WorldEyeLeadGoEarly, -WorldEyeLeadGoJump) and -WorldEyeList's further eyes.
	struct FNextEye
	{
		FString Name;
		FVector From = FVector::ZeroVector;
		FVector To = FVector::ZeroVector;
		double Lens = 35.0;
		bool bLead = false;
		FVector Lead = FVector::ZeroVector;
		bool bGo = false;
		FVector GoTo = FVector::ZeroVector;
		float GoEarly = 0.f;
	};
	// The lead set at a place (engine cm), any scripted walk of it ended.
	void PlaceLead(const FVector& At);
	// The next eye of the list, if any: the still's name, the eye, the lead, the wait and the frame time begun again.
	bool StartNextEye();
	TArray<FNextEye> NextEyes;
	int32 NextEye = 0;
	double SideM = 1000.0;
	float GoEarly = 0.f;
	bool bGoStarted = false;
	bool bGoJump = false;
	bool bLeadHome = false;
	FVector LeadHome = FVector::ZeroVector;
};
