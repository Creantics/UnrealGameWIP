#pragma once

// Game mode of the beat fight screen: what a fight step in a beat world opens (FTStep::Prepare picks the
// Beat map and sets StoryCarry.Combat to Beat; the rules are ftcore::BeatFight and ftcore::BeatScreen,
// BeatScreen.h, and docs/BEAT_SCREEN.md has the whole picture). It reads UFTGameInstance::StoryCarry, picks
// the step's fight from Data/Battle/beat.json (the table's default when the step isn't listed, the built-in
// default when the file is missing or refused), plays it with AFTBeatHUD and returns to the story the way
// the quick-time event screen does. No story on play. Opened without a story step (the map on its own) it
// plays the default fight.
//
// The clock. The fight runs on one clock, UFTConductorComponent's song time. When the fight names a music
// cue and the audio subsystem is playing that cue with a tempo, the conductor is bound to it and the clock
// is the song's (presses are stamped with songTimeForEvent); otherwise (no cue, -nosound, a cue that isn't
// the one playing) the conductor runs on the pause-aware wall clock. With a story step on autoplay and
// -AlphaFast the clock follows game time and the world runs at time dilation 8, as the battle's does; music
// is never bound then. Which clock it chose is logged once. The fight begins on the first tick, once the
// clock is running, so a load hitch can't cost the lead-in.
//
// Keys, read by polling (so a hold can see its key go up): the quick-time event screen's, arrow keys / WASD
// / pad d-pad or the pad's X A Y B for left, down, up, right. After a defeat, Enter / Space / pad A retries
// the same fight and Q / Backspace / pad B gives up (the step ends lost). Escape or Start opens the pause
// menu: the world stops, and so does the wall clock and the music, and a key still held in the frame after
// it closes is not a press.
//
// The enemy stand-ins are actors the map is tagged with (StandInTag(0..)): the mode seats as many as the
// fight has enemies in an arc facing the camera and hides the rest, and the HUD projects them for its
// markers. A map without them still plays: the HUD uses places on the screen.
//
// Finishing: a victory sets the carry's bDone and bWon; a Quit after a defeat sets bDone and leaves bWon
// false; either then lets FTStep::ContinueHere start the beat's next step in place or opens ReturnMap. The
// step is never counted here (the dispatcher counts every step once). One log line per fight, ids and
// counts only:
//   Beat: VICTORY at <scene> beat <b> step <s> after <t> s, <p> perfect, <g> good, <m> miss, best chain <c>, <r> restarts.
//
// Flags, so the map can be checked on its own: -BeatAutoPlay plays the fight with the autopilot (a story
// step's too, as -BattleAutoPlay does for the battle; a step also autoplays with StoryCarry.bAutoPlay) and
// -BeatQuitOnFinish exits when a fight on its own is over (exit code 0 for a victory, 1 otherwise).

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/BeatScreen.h"

#include "FTBeatGameMode.generated.h"

class UFTConductorComponent;

UCLASS()
class FORGOTTENTHIRD_API AFTBeatGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	// What the HUD pops up: what happened to which enemy's prompt, and when (seconds on the fight's clock).
	struct FPopup
	{
		ftcore::BeatEventType Type = ftcore::BeatEventType::Judged;
		ftcore::QteGrade Grade = ftcore::QteGrade::Pending;
		ftcore::BeatKind Kind = ftcore::BeatKind::Strike;
		int32 Enemy = 0;
		double Time = 0.0;
	};
	static constexpr int32 MaxPopups = 12;
	static constexpr int32 MaxStandIns = static_cast<int32>(ftcore::kMaxBeatEnemies);

	AFTBeatGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ---- the map's stand-ins (shared with FTBuildBeatWorld) ----
	// The tag of stand-in Index (0..MaxStandIns - 1).
	static FName StandInTag(int32 Index);
	// Where stand-in Index of Count stands (its body's centre) and which way it faces: an arc on the viewer's
	// side of the room, centred on the camera.
	static void StandInPlace(int32 Index, int32 Count, FVector& OutLocation, FRotator& OutRotation);
	// Where the camera stands and looks.
	static FVector ViewerLocation();
	static FRotator ViewerRotation();

	// ---- for AFTBeatHUD (no allocation) ----
	// True once the fight has begun (on the first tick, with the clock running). Before that there is
	// nothing to show.
	bool IsBegun() const { return bBegun; }
	// The map was opened without a story step and plays the default fight.
	bool IsStandalone() const { return bStandalone; }
	const ftcore::BeatScreen& GetScreen() const { return Screen; }
	// Above stand-in Index's head in the world; false when the map has no such stand-in.
	bool GetEnemyAnchor(int32 Index, FVector& OutLocation) const;
	// The popups, newest last; entries past GetPopupCount() are stale.
	int32 GetPopupCount() const { return FMath::Min(PopupTotal, MaxPopups); }
	const FPopup& GetPopup(int32 Index) const { return Popups[(PopupTotal - GetPopupCount() + Index) % MaxPopups]; }
	// Effect times, on the fight's clock (far in the past until the first one happens): counters are locked
	// out from GetLockoutSince() until GetLockoutUntil() (a whiff), the lead was last hit, a finisher last
	// began (GetFinisherEnemy() is whose), and the multiplier last changed.
	double GetLockoutSince() const { return LockoutSince; }
	double GetLockoutUntil() const { return LockoutUntil; }
	double GetLeadHitTime() const { return LeadHitTime; }
	double GetFinisherTime() const { return FinisherTime; }
	int32 GetFinisherEnemy() const { return FinisherEnemy; }
	double GetMultiplierTime() const { return MultiplierTime; }

private:
	// Reads Data/Battle/beat.json into Table; false (logged, ids only) when missing or refused.
	bool LoadTable();
	// Picks the clock (binds the music when the fight names a playing cue) and logs the choice once.
	void ChooseClock();
	// The conductor's clock is running (after a wait, a music clock that never started gives way to the wall
	// clock).
	bool ClockReady();
	void FindStandIns();
	void SeatStandIns(int32 Count);
	void RefreshStandIns();
	// Polls the four direction key sets and feeds the screen presses and releases, and the Retry and Quit keys.
	void PollKeys();
	void AddPopups(const ftcore::BeatEvents& Events);
	void RestoreDilation();
	void Finish();
	void OpenReturnMap();

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFTConductorComponent> Conductor;

	ftcore::BeatTable Table;
	ftcore::BeatSetup Setup;
	ftcore::BeatScreen Screen;
	ftcore::QteAssist Assist;
	TWeakObjectPtr<AActor> StandIns[MaxStandIns];
	bool bStandInShown[MaxStandIns] = {};
	FPopup Popups[MaxPopups];
	int32 PopupTotal = 0;
	bool bDown[4] = {};

	uint32 Seed = 1;
	double StartWall = 0.0;
	bool bBegun = false;
	bool bStandalone = true;
	bool bAutoPlay = false;
	bool bQuitOnFinish = false;
	bool bStoryAutoPlay = false;
	bool bClockIsMusic = false;
	bool bDilated = false;
	bool bReturning = false;

	double LockoutUntil = -1e9;
	double LockoutSince = -1e9;
	double LeadHitTime = -1e9;
	double FinisherTime = -1e9;
	int32 FinisherEnemy = -1;
	double MultiplierTime = -1e9;
};
