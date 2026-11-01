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
// The map's actors: the lead (a skeletal mesh actor tagged LeadTag(), the test human playing its idle) and
// the boss (a body of basic shapes tagged BossTag(), the parts attached to it). The boss winds up as a
// counter's marker closes in (it swells, leans back and reddens), lunges on the impact, flashes when hit
// and sags while staggered; the lead lunges, sidesteps or hops with each answer.
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
//   -BeatBossShot=<prefix> renders the views docs/BEAT_BOSS_DEMO.md lists with HighResShot (needs a
//                          renderer: -RenderOffScreen, not -NullRHI), holding the fight still a moment for
//                          each, then quits; implies -BeatBossAutoPlay. -BeatBossShotDelay=<s> (8) waits
//                          before the fight begins, so shaders settle before the count-in.
// One result line per outcome, counts only:
//   BEATBOSS RESULT: Victory in <bars> bars, <s> s, <p> perfect, <g> good, <m> miss, best streak <k>, ...

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/BeatBoss.h"
#include "ftcore/BeatMusic.h"

#include "FTBeatBossGameMode.generated.h"

class AActor;
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
	};
	static constexpr int32 MaxPopups = 16;

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
	// Where the boss's body and the lead's feet stand (centimetres, x towards the boss).
	static FVector BossLocation();
	static FVector LeadLocation();
	static FVector ViewerLocation();
	static FRotator ViewerRotation();
	// The boss's body: a cylinder this wide and tall (cm), standing on the floor.
	static constexpr double BossWidth = 230.0;
	static constexpr double BossHeight = 360.0;

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
	// Above the boss's head, in the world.
	FVector GetBossAnchor() const;
	// How wound up the boss is: 0 at rest to 1 as a counter's marker reaches the hit line.
	float GetWindUp() const { return WindUp; }

private:
	bool LoadTrack(const FString& Id);
	void FindStage();
	void PollKeys(double EventTime);
	void PollChoice();
	void Handle(const ftcore::BeatBossEvents& Events);
	void Pulse(EPulse Which, double Time, int32 Amount = 0);
	void AddPopup(const ftcore::BeatBossEvent& E);
	// The stand-in music for this frame: beats crossed, impacts crossed (the count's blips come as events).
	void Sound();
	void Voice(uint8 Voice);
	// The procedural music for this frame (UFTBeatBossMusic): the driver follows the fight, the component places it.
	void MusicFrame();
	void MusicReset();
	// The boss's wind-up, lunge, flash and stagger; the lead's moves.
	void Animate();
	void Begin();
	void Retry();
	// -BeatBossMissPhase: the autopilot's presses for this frame, none while the phase is being lost.
	void Pilot();
	void Finish();
	void LogResult() const;
	void Quit(int32 Code);
	// -BeatBossShot's views; true while the fight should hold still.
	bool Shots();
	void TakeShot(int32 Index, const TCHAR* Name);

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
	TObjectPtr<UMaterialInstanceDynamic> BossMaterial;

	TWeakObjectPtr<AActor> Boss;
	TWeakObjectPtr<AActor> Lead;
	TWeakObjectPtr<USkeletalMeshComponent> LeadMesh;
	FVector BossHome = FVector::ZeroVector;
	FVector BossScale = FVector::OneVector;
	FVector LeadHome = FVector::ZeroVector;

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
	// The boss's last blow (a counter's impact), and when.
	double BossBlowTime = -1e9;
	float WindUp = 0.f;

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
};
