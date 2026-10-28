#pragma once

// Plays a cutscene shot list (ftcore::CutscenePlan) on a cine camera. Each shot names a camera spot
// and one or two subjects by id, and every frame the camera goes where ftcore::cutsceneCamera says
// (metres in Core, centimetres here). A cut jumps; a blend shot lerps from the previous shot's last
// camera over its blend time. It plays a story step two ways:
//
// On the stage (the Cutscene map): AFTBuildCutsceneWorld places one with its camera, and
// AFTCutsceneGameMode spawns one when the map has none. The director finds the map's actors tagged
// with the shot ids once, when it starts. A camera spot is any actor tagged with the spot id
// (ATargetPoint in the blockout); its location is where the camera stands for a wide shot. A subject
// is any actor tagged with the subject id; the bottom centre of its bounds is where it stands. A
// missing spot sits 4 m in front of the subject; a missing subject stands at the world origin (each
// logged once, by id).
//
// In place (HostSteps(true, true)): AFTExploreDirector spawns one on a field map, where it registers
// as the cutscene host (FTStepHost.h), so a CG step after a field step plays where the lead stands,
// with no map load. The stage (ftcore::cutsceneStageMark) is laid over the map with subj_a's mark on
// the lead's feet and its front along the lead's facing; the followers in view stand in for the other
// subject marks, nearest first, and a mark no follower stands on is framed where it lies. The lead's
// input is off and the party held for the whole cutscene, and a traversal (vault, climb, roll) is
// waited out first. A follower whose mark a shot frames walks onto it (ftcore::MarkWalk: round the
// lead, braking onto the mark, then turning to face the lead) while the camera blends in, when the
// mark has ground within a metre of the lead's feet and nothing stands in the way at waist height;
// otherwise it stays where it stands and the shots frame it there. The view switches to the cine
// camera while it sits exactly on the gameplay camera, then the camera orbits round the lead from the
// gameplay view into the first shot over the plan's blend-in time and back out at the end
// (ftcore::CutscenePlayer::presence), and the view switches back once it is on the gameplay camera
// again. A sphere sweep from the look point keeps the camera in front of walls. The letterbox grows in
// and out with the blend (FT::DrawCutsceneHUD, drawn by AFTExploreHUD in place of the field's lines
// while IsPlayingInPlace). Under autoplay there are no blends, so both switches are cuts; their size
// is logged. At the end the beat's next step plays in place when it can (FTStep::ContinueHere), else
// the story map opens as before; the party is let go and takes up the trail from where it stands.
//
// The shot clock runs on game time (sped up with -AlphaFast like the stub screen); the autoplay cap
// and the skip grace run on real time without the time spent in the pause menu
// (UFTPauseSubsystem::WallSeconds). Space / Enter / pad A skip (after a short grace). No allocation
// per frame (the HUD's text is built once per step). Logs ids and counts only, never text:
//   Cutscene: done <scene> beat <b> step <s> shots <n> skipped <0|1>[ in place, pulled <k>, walked <w>]
//   Cutscene: in place, view to the cine camera (<cm> cm, <deg> deg, fov <d>)
//   Cutscene: in place, view back to the lead (<cm> cm, <deg> deg, fov <d>)
//   Cutscene: in place, follower <k> walks <m> m onto <mark>
//   Cutscene: in place, follower <k> on its mark after <s> s
// where pulled counts the shots the sweep pulled in and walked the followers set walking (arrived or
// not). -CutsceneBlendTest plays an autoplay step in place at full length with its blends, to watch
// them (and the walks) in an alpha run.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CollisionQueryParams.h"

#include "ftcore/Cutscene.h"

#include "FTCutsceneDirector.generated.h"

class ACineCameraActor;
class AFTExploreCharacter;
class AFTExploreDirector;
class UCameraComponent;
class USpringArmComponent;

UCLASS()
class FORGOTTENTHIRD_API AFTCutsceneDirector : public AActor
{
	GENERATED_BODY()

public:
	AFTCutsceneDirector();

	// The camera the shots drive; spawned when empty.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cutscene")
	TObjectPtr<ACineCameraActor> Camera;

	// The step's shot list from Data/Cutscene/cutscenes.json, or a one-shot list when the file can't
	// be read (logged).
	static void LoadPlan(const FString& Scene, int32 Beat, int32 Step, ftcore::CutscenePlan& Out);

	// How StartStepHere plays: in place around the lead, or on the stage. With bRegister it is this
	// map's cutscene host until it ends play.
	void HostSteps(bool bInPlaceHost, bool bRegister);
	// Plays the story carry's step (a preview of the default list without one, on the stage only).
	void StartStepHere();

	// Starts `Plan` (copied) on the stage from its first shot. Looks up the markers and places the camera.
	void Play(const ftcore::CutscenePlan& Plan, bool bAutoPlay);
	// A skip press (after the input grace). True when it skipped.
	bool Press();

	const ftcore::CutscenePlayer& GetPlayer() const { return Player; }
	ACineCameraActor* GetCamera() const { return Camera; }
	bool IsStarted() const { return bStarted; }
	bool IsInPlace() const { return bInPlace; }
	// In place, from the step's start (a traversal still being waited out included) until it ends: the
	// screen is the cutscene's, so the field HUD draws none of its own lines.
	bool IsPlayingInPlace() const { return bInPlace && !bFinished && (bBeginPending || bStarted); }
	// Text for the HUD, built once per step: the step's title, "Step i of n", and "Shot i of n" for the
	// current shot (empty before the shots start).
	const FString& GetTitleText() const { return TitleText; }
	const FString& GetStepText() const { return StepText; }
	const FString& GetShotText() const;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	void EnsureCamera();
	void ResolveMarkers();
	// The location of the actor tagged `Id`: its own location (a spot) or its feet (a subject).
	bool FindMarker(std::string_view Id, bool bSubject, FVector& Out) const;
	void ApplyCamera();

	// In place: the lead is free of any traversal, so the stage goes down around it.
	void BeginInPlace();
	void ResolveStage(const ftcore::StageFrame& Frame);
	FVector LeadFeet() const;
	// Where cast member `Index` stands now (Core metres); false (Out untouched) when there is none.
	bool CastFeet(int32 Index, ftcore::CutsceneVec3& Out) const;
	ftcore::CameraPose PoseOfShot(uint32 Index, const ftcore::CutsceneShot& Shot, float Progress) const;
	// The shots' camera this frame, the blend from the previous shot included.
	void ShotPose(ftcore::CameraPose& Pose, float& Focal) const;
	float FovForFocal(float Focal) const;
	void ApplyInPlace();
	// In place: sets walking each follower whose subject mark a shot frames, where the way is clear.
	void BeginWalks(const ftcore::StageFrame& Frame);
	void TickWalks(float DeltaSeconds);
	void LogSwitch(const TCHAR* What) const;
	void RestoreControl();
	// Hands the view, the input and the party back.
	void EndInPlace();
	void Finish();

	ftcore::CutscenePlayer Player;
	// Per shot of the plan: where its spot and subjects are, in Core metres.
	ftcore::CutsceneVec3 ShotSpot[ftcore::CutscenePlan::kMaxShots];
	ftcore::CutsceneVec3 ShotSubject[ftcore::CutscenePlan::kMaxShots];
	ftcore::CutsceneVec3 ShotSubject2[ftcore::CutscenePlan::kMaxShots];
	bool bShotHasSubject2[ftcore::CutscenePlan::kMaxShots] = {};
	// In place: the cast member each subject follows (-1: the stored point).
	int8 ShotSubjectCast[ftcore::CutscenePlan::kMaxShots] = {};
	int8 ShotSubject2Cast[ftcore::CutscenePlan::kMaxShots] = {};

	bool bStarted = false;
	bool bFast = false;
	double LastRealTime = 0.0;
	int32 AppliedShot = -1; // the shot whose lens is set on the camera

	// The step StartStepHere plays, and its HUD text.
	ftcore::CutscenePlan StepPlan;
	FString TitleText;
	FString StepText;
	TArray<FString> ShotTexts;
	bool bInPlace = false;
	bool bRegistered = false;
	bool bHasStep = false;
	bool bAutoPlayStep = false;
	bool bFinished = false;
	bool bBlendTest = false;

	// In place: the lead first, then the followers in view, nearest first.
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> CastMembers;
	TWeakObjectPtr<AFTExploreCharacter> Lead;
	TWeakObjectPtr<AFTExploreDirector> ExploreDirector;
	TWeakObjectPtr<UCameraComponent> LeadCamera;
	TWeakObjectPtr<USpringArmComponent> LeadBoom;
	FCollisionQueryParams SweepParams;
	FRotator SavedControl = FRotator::ZeroRotator;
	// The shots' camera, held through the blend out.
	FVector HeldPos = FVector::ZeroVector;
	FVector HeldLook = FVector::ZeroVector;
	float HeldFov = 90.f;
	// The orbit's turn from the gameplay camera's bearing to the shot's (degrees), kept from frame to
	// frame so it never flips side mid-blend.
	float OrbitDelta = 0.f;
	float LeadYaw = 0.f;
	double PendingSince = 0.0;
	uint32 PulledShots = 0; // bit per shot the sweep pulled in
	// In place, by cast member (the lead's slot 0 unused): its walk onto the subject mark it stands in
	// for, whether it is walking (or turning to face the lead once on the mark), and when it set off.
	static constexpr int32 kWalkSlots = 3;
	ftcore::MarkWalk Walks[kWalkSlots];
	bool bWalking[kWalkSlots] = {};
	bool bWalkArrived[kWalkSlots] = {};
	double WalkSince[kWalkSlots] = {};
	int32 WalkedCount = 0;
	bool bBeginPending = false;
	bool bControlRestored = false;
	bool bWasBlendingOut = false;
	bool bOrbitValid = false;
};
