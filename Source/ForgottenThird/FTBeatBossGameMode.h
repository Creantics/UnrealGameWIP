#pragma once

// Game mode of the beat boss demo (docs/BEAT_BOSS_DEMO.md): a world's final as one authored track,
// ftcore::BeatBoss (Core/include/ftcore/BeatBoss.h), played on /Game/Maps/Demo_BeatBoss, which
// FTBuildBeatBossWorld builds. It sits beside the beat fight's host (AFTBeatGameMode), which it leaves alone.
//
// It loads Data/Battle/beat_boss/<id>.json (-BeatBossTrack=<id>, test_track by default; ids and numbers
// only), begins the fight on the first tick with the clock running, feeds it the action buttons
// (FTActionButtons.h: J I L K, the pad's face buttons; polled, so a held Guard or hold sees its key go up)
// and AFTBeatBossHUD draws it. No story: the map is a demo on its own.
//
// The clock. ftcore::BeatBoss wants a pause-aware wall clock that keeps running through its own silences:
// UFTConductorComponent on the wall clock (bWallClockWithoutAudio, nothing bound), which stands still while
// the pause menu is open. Presses are stamped with the conductor's latency-compensated input time. The
// track's music is procedural, UFTBeatBossMusic, driven from the same clock: ftcore::BeatMusicDriver plans each
// bar from the fight (a drum and bass pattern that builds with the phases, into the finisher, with an accent
// on each told move's impact and the count's blips) and the component places every sound on its exact frame,
// so what is heard and what the beat line draws agree. -BeatBossMetronome brings back the old stand-in,
// UFTBeatBossMetronome: a click on every beat the bar has, the count's blips and each move's stem.
//
// After a defeat the result shows, then Enter / Space / pad A retries from the phase's checkpoint
// (BeatBoss::retry) and Q / Backspace / pad B gives up. After a victory Enter / Space / pad A plays the
// track again from the top.
//
// The scene (docs/BEAT_BOSS_DEMO.md, "The stage"). The map has two skeletal mesh actors, the lead (tagged
// LeadTag()) and the boss (tagged BossTag(), the same body scaled up with a dark red material made here), both
// the test human, and a camera. ftcore::BeatChoreo (BeatChoreo.h) reads the fight and says, each frame, which
// clip each fighter plays at what time and rate, which shot the camera is on, and the prompts to draw and
// where: the host plays the clips through UFTBeatFighterAnimInstance (a crossfade between clips), puts the
// camera on the shot (a hard cut on the beat the choreography says, finisher_orbit orbiting, a shake on heavy
// blows), and AFTBeatBossHUD draws each prompt on the bone it belongs to. Hit-stop and the finisher's slow
// motion are in the clip times; the fight's clock and the music are never touched.
//
// Flags (FParse::Param, docs/ALPHA.md style):
//   -BeatBossAutoPlay      Core's autopilot plays (QteAssist::autoComplete); after a defeat the retry prompt
//                          shows and is confirmed on its own a second later, three times at most.
//   -BeatBossMissPhase=<n> the autopilot (implied), driven by the host, presses nothing from phase n's start
//                          (1 is the first) until the fight is lost, then plays on from the retry prompt
//                          as above, which restarts at phase n's checkpoint: the checkpoint retry, headless.
//   -BeatBossIdle          presses nothing at all and never retries: the fight must be lost.
//   -BeatBossMetronome     the old click-and-stems stand-in instead of the procedural music.
//   -BeatBossQuitOnFinish  exits when the fight is over: code 0 on a victory, 1 on a defeat, 2 if the
//                          watchdog fired after 300 s, 3 if the track didn't load (FTRequestExit, which forces
//                          a non-zero code past the engine's graceful exit, as the engine does for commandlets).
//   -BeatBossTrack=<id>    another track (cine_track by default; test_track is the dense old one).
//   -BeatBossShot=<prefix> renders the views docs/BEAT_BOSS_DEMO.md lists with HighResShot (needs a
//                          renderer: -RenderOffScreen, not -NullRHI), holding the fight still a moment for
//                          each, then quits; implies -BeatBossAutoPlay. -BeatBossShotDelay=<s> (8) waits
//                          before the fight begins, so shaders settle before the count-in.
//   -BeatBossSeq=<prefix>  renders a short frame sequence of a counter exchange (every tenth of a second of the
//                          fight's clock for three seconds from a parry's tell) as <prefix>_0000.png ...;
//                          implies -BeatBossAutoPlay. -BeatBossSeqCount=<n> frames (30).
// One result line per outcome, counts only:
//   BEATBOSS RESULT: Victory in <bars> bars, <s> s, <p> perfect, <g> good, <m> miss, best streak <k>, ...

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/BeatBoss.h"
#include "ftcore/BeatChoreo.h"
#include "ftcore/BeatMusic.h"

#include "FTBeatBossGameMode.generated.h"

class AActor;
class ACameraActor;
class UAnimSequence;
class UAudioComponent;
class UFTBeatBossMetronome;
class UFTBeatBossMusic;
class UFTConductorComponent;
class UMaterialInstanceDynamic;
class USkeletalMeshComponent;

UCLASS()
class FORGOTTENTHIRD_API AFTBeatBossGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	enum class EStage : uint8
	{
		Waiting,     // the clock (or a shot run's settle) before the fight begins
		Playing,
		Result,      // victory or defeat, shown a moment
		RetryPrompt, // after a defeat: Retry (from the checkpoint) or Give up
		Done,        // given up, or a run that is about to quit
	};

	// What the HUD pops up at the hit line, and when (seconds on the fight's clock).
	struct FPopup
	{
		ftcore::BeatBossEventType Type = ftcore::BeatBossEventType::Judged;
		ftcore::QteGrade Grade = ftcore::QteGrade::Pending;
		ftcore::BeatBossAnswer Answer = ftcore::BeatBossAnswer::Attack;
		int32 Amount = 0;
		double Time = 0.0;
		// Where the move's prompt stood in the world when it was judged: the judgement pops there.
		FVector Anchor = FVector::ZeroVector;
		bool bAnchored = false;
	};
	static constexpr int32 MaxPopups = 16;

	// One prompt to draw on the action: where it is in the world (the bone of the body part it answers), how far its
	// ring has closed, and what it asks.
	struct FPromptView
	{
		uint32 Id = 0;
		FVector World = FVector::ZeroVector;
		float Progress = 0.f; // 0 at the tell, 1 at the impact
		ftcore::BeatBossAnswer Answer = ftcore::BeatBossAnswer::Attack;
		ftcore::ActionButton Button = ftcore::ActionButton::Attack;
		ftcore::ActionButton Second = ftcore::ActionButton::Attack;
		bool bFinisher = false;
		bool bOnScreen = false; // sits at the middle of the screen, not on a body
		ftcore::ChoreoAnchor Anchor = ftcore::ChoreoAnchor::BossChest;
	};
	static constexpr int32 MaxPromptViews = 4;

	// Effects the HUD and the stage pulse with (times on the fight's clock, far in the past until they happen).
	enum class EPulse : uint8
	{
		LeadHit,
		BossHit,
		Whiff,
		Multiplier,
		Finisher,
		FinisherBroken,
		Stagger,
		Silence,
		Resumed,
		Phase,
		Released,
		LoadStored,
		RestOpened,
		Reward,
		Judged,
		Count, // a count beat
		Num
	};

	AFTBeatBossGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ---- the stage (shared with FTBuildBeatBossWorld) ----
	static FName BossTag();
	static FName LeadTag();
	// Where the fighters' feet stand (centimetres, x from the lead towards the boss, the floor's top at z = 0).
	static FVector BossLocation();
	static FVector LeadLocation();
	static FVector ViewerLocation();
	static FRotator ViewerRotation();
	// The boss is the lead's body at this scale.
	static constexpr double BossScaleFactor = 1.35;
	// The distance between the two fighters' feet, centimetres: far enough that neither stands in the other, near
	// enough that a fist or a foot reaches.
	static constexpr double FightGap = 110.0;

	// ---- for AFTBeatBossHUD (no allocation) ----
	EStage GetStage() const { return Stage; }
	bool IsBegun() const { return Stage != EStage::Waiting; }
	bool IsAutoPlay() const { return bAutoPlay; }
	bool IsIdle() const { return bIdle; }
	const ftcore::BeatBoss& GetFight() const { return Fight; }
	const ftcore::BeatBossTrack& GetTrack() const { return Track; }
	// The beat line, filled once a frame after the fight's update.
	const ftcore::BeatLine& GetLine() const { return Line; }
	// The fight's clock this frame.
	double GetNow() const { return Now; }
	double GetPulse(EPulse Which) const { return Pulses[static_cast<int32>(Which)]; }
	int32 GetPulseAmount(EPulse Which) const { return PulseAmounts[static_cast<int32>(Which)]; }
	// Counters are locked out until this time (a whiff), on the fight's clock.
	double GetLockoutUntil() const { return LockoutUntil; }
	// The silence playing: its length (seconds); 0 when none has.
	double GetSilenceSeconds() const { return SilenceSeconds; }
	int32 GetPopupCount() const { return FMath::Min(PopupTotal, MaxPopups); }
	const FPopup& GetPopup(int32 Index) const { return Popups[(PopupTotal - GetPopupCount() + Index) % MaxPopups]; }
	double GetResultAge() const { return Now - ResultTime; }
	int32 GetRetries() const { return Retries; }
	bool GaveUp() const { return bGaveUp; }
	// The button a prompt is answered with (the track names it only for a hold, a release or a pair).
	static ftcore::ActionButton ButtonFor(ftcore::BeatBossAnswer Answer, ftcore::ActionButton Own);
	// The boss's head in the world (its hp plate hangs over it, its damage rises from it).
	FVector GetBossHead() const { return BossHeadWorld; }
	// ---- the cinematic scene ----
	// Whether a choreography is playing (a track without one leaves the fighters in their stance).
	bool HasScene() const { return Choreo.begun(); }
	const ftcore::ChoreoSample& GetSample() const { return Sample; }
	int32 GetPromptViewCount() const { return PromptViewCount; }
	const FPromptView& GetPromptView(int32 Index) const { return PromptViews[Index]; }
	// The camera's shot now, and the seconds since the last cut.
	ftcore::ChoreoShot GetShot() const { return Sample.shot; }

private:
	bool LoadTrack(const FString& Id);
	void FindStage();
	void PollKeys(double EventTime);
	void PollChoice();
	void Handle(const ftcore::BeatBossEvents& Events, double Clock);
	void Pulse(EPulse Which, double Time, int32 Amount = 0);
	void AddPopup(const ftcore::BeatBossEvent& E);
	// The stand-in music for this frame: beats crossed, impacts crossed (the count's blips come as events).
	void Sound();
	void Voice(uint8 Voice);
	// The procedural music for this frame (UFTBeatBossMusic): the driver follows the fight, the component places it.
	void MusicFrame();
	void MusicReset();
	// The scene for this frame: the choreography's sample, the two fighters' poses, the camera and the prompts.
	void Scene(float DeltaSeconds);
	bool LoadChoreo(const FString& Id);
	void SpawnFighters();
	void PlaceFighter(AActor* Actor, const FVector& Location, double Yaw, double Scale);
	void UpdateCamera(float DeltaSeconds);
	void UpdatePrompts();
	FVector AnchorWorld(ftcore::ChoreoAnchor Anchor) const;
	void CalibrateFacing(USkeletalMeshComponent* Mesh, double TargetYaw);
	FVector BoneWorld(USkeletalMeshComponent* Mesh, const TCHAR* Bone, const FVector& Fallback) const;
	void Shake(float Amount);
	void Observe(const ftcore::BeatBossEvents& Events, double Clock);
	void Begin();
	void Retry();
	// -BeatBossMissPhase: the autopilot's presses for this frame, none while the phase is being lost.
	void Pilot();
	void Finish();
	void LogResult() const;
	void Quit(int32 Code);
	// -BeatBossShot's views; true while the fight should hold still.
	bool Shots();
	void TakeShot(int32 Index);
	// -BeatBossSeq's frames; true while the fight should hold still.
	bool Sequence();

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFTConductorComponent> Conductor;

	UPROPERTY(Transient)
	TObjectPtr<UFTBeatBossMetronome> Metronome;
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> MetronomeAudio;
	UPROPERTY(Transient)
	TObjectPtr<UFTBeatBossMusic> Music;
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> MusicAudio;
	ftcore::BeatMusicDriver MusicDriver;
	ftcore::MusicEvents MusicOut;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> BossMaterials; // one per body material, tinted for the boss
	// The fight clips by catalogue index (null where an asset is missing). Kept above the big Core structs below:
	// a UPROPERTY's offset must fit in 16 bits.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> ClipAssets;

	// The scene: the choreography, the clips it plays and the two fighters.
	ftcore::BeatClips Clips;
	ftcore::BeatChoreoDef ChoreoDef;
	ftcore::BeatChoreo Choreo;
	ftcore::ChoreoSample Sample;
	int32 StanceLeadClip = -1;
	int32 StanceBossClip = -1;

	TWeakObjectPtr<AActor> Boss;
	TWeakObjectPtr<AActor> Lead;
	TWeakObjectPtr<USkeletalMeshComponent> LeadMesh;
	TWeakObjectPtr<USkeletalMeshComponent> BossMesh;
	TWeakObjectPtr<ACameraActor> Camera;
	FVector BossHome = FVector::ZeroVector;
	FVector LeadHome = FVector::ZeroVector;
	FVector BossHeadWorld = FVector::ZeroVector;
	TArray<FLinearColor> BossBase;     // each boss material's resting BaseColor and Emissive
	TArray<FLinearColor> BossEmissive;
	float BossDazedLast = -1.f;
	float BossStruckLast = -1.f;
	bool bRootMotion = false;
	bool bFaced = false;
	int32 SceneFrames = 0;

	// The prompts to draw this frame, and where each one stood last (a judgement pops where its prompt was).
	FPromptView PromptViews[MaxPromptViews];
	int32 PromptViewCount = 0;
	struct FAnchorMemo
	{
		uint32 Id = 0;
		FVector World = FVector::ZeroVector;
	};
	static constexpr int32 MaxAnchorMemos = 16;
	FAnchorMemo AnchorMemos[MaxAnchorMemos];
	int32 AnchorMemoNext = 0;

	// The camera: its trauma (a shake that dies away), the shot it is on and the clock of the last frame.
	float Trauma = 0.f;
	ftcore::ChoreoShot CameraShot = ftcore::ChoreoShot::Wide;
	bool bCameraCut = true;
	double OrbitAngle = 0.0;
	double SceneWall = 0.0;

	ftcore::BeatBossTrack Track;
	ftcore::BeatBossTuning Tuning;
	ftcore::QteAssist Assist;
	ftcore::BeatBoss Fight;
	ftcore::BeatLine Line;
	EStage Stage = EStage::Waiting;

	FPopup Popups[MaxPopups];
	int32 PopupTotal = 0;
	double Pulses[static_cast<int32>(EPulse::Num)];
	int32 PulseAmounts[static_cast<int32>(EPulse::Num)] = {};
	double LockoutUntil = -1e9;
	double SilenceSeconds = 0.0;
	ftcore::BeatCountKind CountKind = ftcore::BeatCountKind::Click;
	bool bDown[4] = {};

	// The lead's last answer (its move), and when.
	ftcore::BeatBossAnswer LeadMove = ftcore::BeatBossAnswer::Attack;
	double LeadMoveTime = -1e9;

	// The stand-in music: which bar (BeatBossStats::bars) and where on it the last frame was.
	int32 SoundBars = -1;
	double SoundPlayhead = 0.0;
	// The told moves whose impact hasn't sounded yet (an answered one leaves the fight before its impact, so
	// the host keeps its own list): the prompt id, its impact on the track's clock and its stem.
	struct FSoundCue
	{
		uint32 Id = 0;
		double Impact = 0.0;
		uint8 Stem = 0;
	};
	static constexpr int32 MaxSoundCues = 48;
	FSoundCue SoundCues[MaxSoundCues];
	int32 SoundCueCount = 0;
	uint32 SoundCueId = 0; // the highest prompt id listed
	double SoundTrack = 0.0; // the track's clock last frame

	double StartWall = 0.0;
	double Now = 0.0;
	double LastSong = 0.0;
	double HeldSeconds = 0.0; // fight clock held still for the shots
	double ResultTime = 0.0;
	double BeginTime = 0.0;
	int32 Retries = 0;
	int32 AutoRetries = 0;
	int32 MissPhase = 0;     // -BeatBossMissPhase, 1-based; 0: none
	bool bMissing = false;   // the miss phase has started and is being lost
	double PromptTime = 0.0; // when the retry prompt showed
	bool bAutoPlay = false;
	bool bIdle = false;
	bool bQuitOnFinish = false;
	bool bGaveUp = false;
	bool bResultLogged = false;
	bool bQuitting = false;

	FString ShotPrefix;
	float ShotDelay = 8.f;
	uint32 ShotsTaken = 0; // a bit per view
	int32 ShotCount = 0;
	double ShotHoldUntil = 0.0; // wall seconds

	// -BeatBossSeq: frames of a counter exchange, one every SeqStep seconds of the fight's clock.
	FString SeqPrefix;
	int32 SeqCount = 30;
	int32 SeqTaken = 0;
	bool bSeqRunning = false;
	double SeqNextClock = 0.0;
	double SeqHoldUntil = 0.0;
};
