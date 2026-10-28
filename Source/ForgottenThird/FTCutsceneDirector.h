#pragma once

// Plays a cutscene shot list (ftcore::CutscenePlan) on a cine camera. Each shot names a camera spot
// and one or two subjects by id; the director finds the map's actors tagged with those ids once,
// when it starts, and every frame puts the camera where ftcore::cutsceneCamera says (metres in Core,
// centimetres here). A cut jumps; a blend shot lerps from the previous shot's last camera over its
// blend time. AFTBuildCutsceneWorld places one in the Cutscene map with its camera;
// AFTCutsceneGameMode spawns one when the map has none.
//
// Markers: a camera spot is any actor tagged with the spot id (ATargetPoint in the blockout); its
// location is where the camera stands for a wide shot. A subject is any actor tagged with the subject
// id; the bottom centre of its bounds is where it stands. A missing spot sits 4 m in front of the
// subject; a missing subject stands at the world origin (each logged once, by id).
//
// The shot clock runs on game time (sped up with -AlphaFast like the stub screen); the autoplay cap
// and the skip grace run on real time without the time spent in the pause menu
// (UFTPauseSubsystem::WallSeconds). No allocation per frame.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ftcore/Cutscene.h"

#include "FTCutsceneDirector.generated.h"

class ACineCameraActor;

UCLASS()
class FORGOTTENTHIRD_API AFTCutsceneDirector : public AActor
{
	GENERATED_BODY()

public:
	AFTCutsceneDirector();

	// The camera the shots drive; spawned at Play when empty.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cutscene")
	TObjectPtr<ACineCameraActor> Camera;

	// Starts `Plan` (copied) from its first shot. Looks up the markers and places the camera.
	void Play(const ftcore::CutscenePlan& Plan, bool bAutoPlay);
	// A skip press (after the input grace). True when it skipped.
	bool Press();

	const ftcore::CutscenePlayer& GetPlayer() const { return Player; }
	ACineCameraActor* GetCamera() const { return Camera; }
	bool IsStarted() const { return bStarted; }

	virtual void Tick(float DeltaSeconds) override;

private:
	void EnsureCamera();
	void ResolveMarkers();
	// The location of the actor tagged `Id`: its own location (a spot) or its feet (a subject).
	bool FindMarker(std::string_view Id, bool bSubject, FVector& Out) const;
	void ApplyCamera();

	ftcore::CutscenePlayer Player;
	// Per shot of the plan: where its spot and subjects are, in Core metres.
	ftcore::CutsceneVec3 ShotSpot[ftcore::CutscenePlan::kMaxShots];
	ftcore::CutsceneVec3 ShotSubject[ftcore::CutscenePlan::kMaxShots];
	ftcore::CutsceneVec3 ShotSubject2[ftcore::CutscenePlan::kMaxShots];
	bool bShotHasSubject2[ftcore::CutscenePlan::kMaxShots] = {};

	bool bStarted = false;
	bool bFast = false;
	double LastRealTime = 0.0;
	int32 AppliedShot = -1; // the shot whose lens is set on the camera
};
