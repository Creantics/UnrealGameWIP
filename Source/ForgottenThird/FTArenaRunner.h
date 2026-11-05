#pragma once

// The arena fight as it runs (docs/COMBAT_SYSTEMS.md "Arena melee"): ftcore::ArenaFight stepped at a fixed 1/60 s, the
// figures (AFTArenaFighter: the player's pawn and one per enemy) placed and posed from it, the keys read, the camera
// steered and the one result line logged. Whoever holds the fight owns one and ticks it: the arena demo's game mode
// (AFTArenaGameMode, which loads the table, fits the zone and builds the ground) and, in a world, the arena step's host on
// the explore map (AFTArenaHost). AFTArenaHUD draws from it.
//
// Core judges, the host moves. The runner steps the fight with an accumulator (at most 8 steps a frame), the step the Core
// tests and the balance check use, and moves the bodies with ftcore::arenaStep, which keeps every body inside the zone and
// out of its obstacles. The figures are placed between the last two steps, turned to Core's facings and posed from Core's
// actions; a skinned enemy's clip is picked and timed from Core's state (DriveSkin). The first Arena boss's effects
// (FFTArenaBossVfx) are played from the same state: its parts, the blows on it and its host's notes.
//
// The keys, the camera and the flags are as docs/ARENA_DEMO.md and AFTArenaGameMode list them.

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaKindCrit.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/BeatChoreo.h"
#include "ftcore/ClipShowcase.h"

#include "FTArenaBossVfx.h"
#include "FTKiteHost.h"
#include "FTKiteVfx.h"
#include "FTStoneBearerHost.h"

#include <array>
#include <string>
#include <string_view>

class UFTVfxComponent;

#include "FTArenaRunner.generated.h"

class AFTArenaFighter;
class ACameraActor;
class AStaticMeshActor;
class APlayerController;
class UWorld;
class UAnimSequence;
struct FFTLeadLook;

// A skinned enemy's clips: the table, the sequences loaded for it (by table index) and the table index of each clip the
// runner plays by role, -1 where it has none.
USTRUCT()
struct FFTArenaSkin
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<TObjectPtr<UAnimSequence>> Sequences;
	TSharedPtr<ftcore::BeatClips> Clips;
	int32 Idle = -1;
	int32 IdleTwo = -1;
	int32 Stagger = -1;
	int32 FaceDown = -1;
	int32 Open = -1;
	int32 Defeat = -1;
	int32 DefeatKneel = -1; // optional: a death on its knees, else defeat
	int32 HitLight = -1;
	int32 HitHeavy = -1;
	int32 Walk = -1; // optional: played at the body's ground speed over its authored speed_mps, else the idle
	// Optional, with face_down: the go-down into it and the get-up out of it, else face_down throughout.
	int32 FaceDownIn = -1;
	int32 FaceDownOut = -1;
	// Optional, both or neither: the stepping turns in place (looping cycles; the runner yaws the figure along them).
	int32 TurnL = -1;
	int32 TurnR = -1;
	std::array<int32, ftcore::kMaxArenaMoves> MoveClip{};
	// Optional, per move: its recoil (`<clip>_recoil`), played from a parry of it through the Open and the stagger after,
	// else the stagger.
	std::array<int32, ftcore::kMaxArenaMoves> RecoilClip{};
	// Optional per move: its mirrored clip (the move's clip id with _l), played when the run calls its blow to the left.
	std::array<int32, ftcore::kMaxArenaMoves> MoveClipL{};

	bool IsLoaded() const { return Clips.IsValid() && Idle >= 0; }
};

// One enemy slot's stepping turn (UFTArenaRunner::StepTurn): which way (+1 right, the yaw rising; -1 left; 0 none), since
// when (Fight time), the facing it began from, the cycles it is committed to, the crossfade into its clip (s), where its
// pivot foot stood as the cycle began, from the actor's origin in world axes (when found), whether the first cycle's foot
// check is logged, whether Core wanted its body moving on the last step (any kind but the stone_bearer), and the fight's
// counts: turns begun, steps, turns cancelled, and starts held by a move due within a step.
struct FFTArenaTurn
{
	int32 Sign = 0;
	double Start = -1e9;
	ftcore::Vec2 From;
	int32 Cycles = 0;
	double Blend = 0.25;
	FVector Foot = FVector::ZeroVector;
	bool bFoot = false;
	bool bChecked = false;
	bool bMoves = false;
	int32 Begun = 0;
	int32 Steps = 0;
	int32 Cancelled = 0;
	int32 HeldPick = 0;
};

// What a fight is begun with: the def (the fighter kit and the enemies, already fitted to the zone), the fitted zone, where
// its ground is, who plays and what happens at the end. Copied in at Begin.
struct FFTArenaRunSetup
{
	const ftcore::ArenaTable* Table = nullptr; // for the result line's kind ids; copied
	ftcore::ArenaDef Def;
	ftcore::ArenaZoneFit Fit;
	std::string ZoneId;
	double ZoneZ = 0.0;  // cm: the height of the ground in the zone
	float ZoneYaw = 0.f; // degrees: the zone's yaw (arenas.json), the frame of a boss's keep-out bearing
	int32 ZoneStyle = 0; // the world's look: 0 the plain zone, 1 Solhaven, 2 the Lattice, 3 the Unwritten
	FString KitId;       // the fighter kit the player plays, as the table resolved it
	const FFTLeadLook* Lead = nullptr; // the look the player's figure wears (null: the test human)
	uint32 Seed = 1;
	bool bAutoPlay = false;
	bool bIdle = false;
	bool bHoldLoad = false;
	float ParryHold = 0.f; // -ArenaParryHold=<s>
	FString BossLoop;      // -ArenaBossLoop=<clip> (review only; empty: off)
	bool bBossSplitAtStart = false; // -ArenaBossSplit
	// Exit the process on the outcome (0 a victory, 1 a defeat), and end a fight that outlasts the watchdog.
	bool bQuitOnFinish = false;
	// A -ArenaShot run: the shots decide when to quit, and nothing fights again.
	bool bShots = false;
	// After the outcome, fight again on a press (the autopilot after 4 s, or once a boss's defeat clip has played out, if
	// that is longer); else the fight stays over for the owner.
	bool bFightAgain = true;
	// The player's figure replaces the controller's pawn, which is destroyed; else the old pawn is only unpossessed.
	bool bReplacePawn = true;
	// -ArenaShowcase (review only): no fight; the boss's clips one at a time and its effects alone, from a fixed camera,
	// then the process quits (see BeginShowcase).
	bool bShowcase = false;
};

UCLASS()
class FORGOTTENTHIRD_API UFTArenaRunner : public UActorComponent
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxEnemies = static_cast<int32>(ftcore::kMaxArenaEnemies);
	static constexpr int32 MaxPopups = 16;
	static constexpr float StepSeconds = 1.f / 60.f;
	static constexpr int32 MaxStepsPerFrame = 8;
	static constexpr float WatchdogSeconds = 180.f;

	// What the HUD pops up over a body, and when (seconds on the screen's clock, GetNow()).
	struct FPopup
	{
		ftcore::ArenaEventType Type = ftcore::ArenaEventType::Hit;
		uint8 Source = ftcore::kArenaNobody;
		uint8 Target = ftcore::kArenaNobody;
		uint8 Move = 0;
		int32 Amount = 0;
		FVector At = FVector::ZeroVector;
		double Time = -1e9;
	};

	UFTArenaRunner();

	// The result line's name for who plays: "autoplay", "idle" or "player".
	static const TCHAR* PilotName(bool bAutoPlayRun, bool bIdleRun);
	// The world's ground height at p (metres, in cm), from a trace onto its Landscape or a plaza's paving (0 when there is none).
	static double SampleGround(const UWorld* World, const ftcore::Vec2& P);

	// Begins the fight: spawns the figures (the player's possessed), puts the bodies at their starts and places them.
	void Begin(const FFTArenaRunSetup& Setup);
	// One frame: the keys, the steps, the figures and the camera; after the outcome the result line, then fighting again
	// or quitting as the setup says. bHold stops the fight's clock (an -ArenaShot view being taken).
	void TickFight(float DeltaSeconds, bool bHold);
	// The same fight from the start (its counts run on).
	void Restart();
	// Destroys the figures (the owner gives the controller its pawn back first).
	void End();
	// Puts the player's body at P (metres), for -ArenaShot's edge view.
	void PutPlayerAt(const ftcore::Vec2& P);

	// ---- for AFTArenaHUD and the owner (no allocation) ----
	bool IsBegun() const { return bBegun; }
	// The outcome has come and its line is logged.
	bool IsFinished() const { return bResultLogged; }
	const ftcore::ArenaFight& GetFight() const { return Fight; }
	// The screen's clock: seconds of play since the fight began, stopped while held.
	double GetNow() const { return Now; }
	// Seconds since the fight ended (0 while it runs).
	double GetResultAge() const { return bResultLogged ? Now - ResultTime : 0.0; }
	// Seconds since the result may show (negative before): a victory's banner waits for the death clips on screen as the
	// fight ended to play out, so it never hides a boss's death.
	double GetResultShowAge() const { return bResultLogged ? Now - ResultTime - ResultWait : 0.0; }
	bool IsAutoPlay() const { return bAutoPlay; }
	bool IsIdle() const { return bIdle; }
	// The result offers to fight again.
	bool CanFightAgain() const { return bFightAgain; }
	AFTArenaFighter* GetPlayerFigure() const { return PlayerFigure; }
	// The zone the fight is held in, for the HUD's edge cue: its walls, the world's look (0 the plain zone, 1 Solhaven,
	// 2 the Lattice, 3 the Unwritten), where the player stands (metres) and the height of the ground there (cm).
	const ftcore::ArenaZone& GetZone() const { return Fit.zone; }
	int32 GetZoneStyle() const { return ZoneStyle; }
	ftcore::Vec2 GetPlayerPosition() const { return Bodies.player; }
	// Everyone's position after the last step (metres).
	const ftcore::ArenaBodies& GetBodies() const { return Bodies; }
	double GetZoneZ() const { return ZoneZ; }
	AFTArenaFighter* GetEnemyFigure(int32 Index) const { return (Index >= 0 && Index < MaxEnemies) ? EnemyFigures[Index].Get() : nullptr; }
	// The move enemy Index is in (its attack's numbers), or null when it isn't attacking.
	const ftcore::ArenaEnemyMove* GetEnemyMove(int32 Index) const;
	// When enemy Index's current attack went active (far in the past when it hasn't this attack).
	double GetEnemyActiveSince(int32 Index) const { return (Index >= 0 && Index < MaxEnemies) ? ActiveSince[Index] : -1e9; }
	// When enemy Index was last hit (far in the past when never).
	double GetEnemyHitTime(int32 Index) const { return (Index >= 0 && Index < MaxEnemies) ? EnemyHitTime[Index] : -1e9; }
	double GetPlayerHitTime() const { return PlayerHitTime; }
	double GetParryTime() const { return ParryTime; }
	// The last Release (when it began and the Load it spent) and the last time the Load overloaded.
	double GetReleaseTime() const { return ReleaseTime; }
	int32 GetReleaseSpent() const { return ReleaseSpent; }
	double GetOverloadTime() const { return OverloadTime; }
	int32 GetPopupCount() const { return FMath::Min(PopupTotal, MaxPopups); }
	const FPopup& GetPopup(int32 Index) const { return Popups[(PopupTotal - GetPopupCount() + Index) % MaxPopups]; }
	// Bumped on every new popup, so the HUD rebuilds a popup's number only when its slot changes.
	int32 GetPopupTotal() const { return PopupTotal; }
	// The first Arena boss's host while it fights here (null otherwise), and the Intercept button as the step took it.
	const FFTStoneBearerHost* GetStoneBearer() const { return StoneBearer.IsActive() ? &StoneBearer : nullptr; }
	bool IsInterceptHeld() const { return bInterceptHeld; }
	// Enemy Index's weak points when its kind has an entry in arena_crit.json and they are on this fight (null otherwise):
	// whether the window is open (at GetFight().time()), when it closes, and the crits landed, for the HUD and effects.
	const ftcore::ArenaKindCrit* GetKindCrit(int32 Index) const
	{
		return (Index >= 0 && Index < MaxEnemies && bKindCritSlot[Index]) ? &KindCrits[static_cast<size_t>(Index)] : nullptr;
	}
	// The Kite-Mother's weak-point host while it fights here (null otherwise).
	const FFTKiteHost* GetKite() const { return Kite.IsActive() ? &Kite : nullptr; }

private:
	void SpawnFigures(const FFTLeadLook* Lead);
	// The stone_bearer boss, when the line-up has one: its file read and its kind rebuilt in Def (before the fight
	// begins), then the host begun on the fight (after it begins or restarts), its plinth and keep-out drawn.
	void PrepareStoneBearer();
	void BeginStoneBearer();
	void EndStoneBearer();
	// The host's new notes, one log line each (ids, counts and outcome words only); a crit's burst.
	void LogStoneBearerNotes();
	// The stone_bearer's move mask, a log line each time it changes.
	void LogStoneMoveMask();
	// The stone_bearer's walk after each step (its skinned figure only): starts and stops it from Core's wanted speed, and
	// between moves moves its body at the walk clip's speed, or holds it still (see kStoneWalkHold in the .cpp).
	void StoneBearerWalk(const ftcore::ArenaOutput& Out);
	// The stepping turn of any enemy whose skin has both turn clips (see kArenaTurnDeg in the .cpp). bHost: the
	// stone_bearer's host is in the fight (the stone_bearer turns only with it). Before each step: Core's facing held while
	// it stands, and the turn's facing written while one is under way. After each step: a turn begun, carried into another
	// cycle, ended or cancelled, a log line each.
	bool CanTurn(int32 I, bool bHost) const;
	void TurnsBeforeTick(bool bHost);
	void StepTurns(const ftcore::ArenaOutput& Out, bool bHost);
	void StepTurn(int32 I, const ftcore::ArenaOutput& Out);
	// Slot I's turn facing T seconds (Fight time) into it.
	ftcore::Vec2 TurnFacing(int32 I, double T) const;
	// The turn counts of every slot with turn clips but the stone_bearer's (LogStoneWalkCounts has its), a log line each.
	void LogTurnCounts() const;
	// A hit reaction for slot I, light or heavy, when it flinches (the stone_bearer's slot holds a playing one a while).
	void StartFlinch(int32 I, bool bHeavy);
	// The boss's effects: its attack part going From -> To (its tell through the startup, its impact as it turns active),
	// a blow landing on it (E: Hit, CounterStagger or ReadCounter), and where its weak point is now.
	void BossPartChanged(const ftcore::ArenaEnemyOut& Out, uint8 From, uint8 To);
	// bHeavy: a heavy blow, a riposte, a Release or a counter-stagger (the heavy chips).
	void BossHitVfx(const ftcore::ArenaEvent& E, bool bHeavy);
	FVector BossWeakPoint() const;
	// Where the crit flash and the weak-point glow go: the weak point, lifted and moved toward the camera (the tuning's
	// flashUp and flashToCamera).
	FVector BossFlashPoint() const;
	// The stone_bearer kneels in this clip (going down, down, getting up): its hits hang off the pelvis then.
	bool StoneKneelShows() const;
	// The boss broke its carried piece: its figure swaps in the parts on the split file's bones (once a fight). bStagger:
	// a break in the fight (not the run's split at the start), which also plays the impact and the stagger at once.
	void SplitBossCarried(bool bStagger);
	// The split's parts made hidden on the figure as it is spawned, so the split only swaps visibility.
	void PrepareBossCarried();
	// The stone_bearer's walk counts for the fight, one log line at its end.
	void LogStoneWalkCounts() const;
	// -ArenaShowcase: one boss's reel (ftcore/ClipShowcase.h) built from its skin and moves, everyone else hidden, the HUD
	// off and a fixed camera at play distance (the lead's rig: its arm plus the boss's reach) 30 degrees off the boss's
	// front to its left. The boss is the stone_bearer, else the kite_boss, else the line-up's first skinned enemy (any
	// boss with a clip table). Each frame plays the reel's pose and the effects of the cues it crossed, logs each part's
	// start (with the full frame counter, for the cutter) and quits at the end. False, with a warning, when there is no
	// skinned enemy to show (the flag then does nothing).
	// Shared by every boss: the clips, their entry points, the cross-fades, the camera, the log and the stepping turns (for
	// a boss with turn_l and turn_r). Boss-specific: the walk's start phase (the stone_bearer's host only), the left-hand
	// twins and the strike points
	// (StrikeAt), and the effects, which ShowcaseCue plays only for a boss whose effects it knows (the stone_bearer's
	// BossVfx); for any other it logs the cue and plays nothing, the seam where that boss's effects go.
	bool BeginShowcase();
	void TickShowcase(float DeltaSeconds);
	void ShowcaseCue(const ftcore::ShowCueAt& Cue, AFTArenaFighter& Figure);
	// The boss's contact socket for arena move Move (a label mapped to its bone; bLeft: its left-hand clip), null: none.
	const char* BossContactSocket(uint8 Move, bool bLeft) const;
	// Plain kinds' weak points (ftcore/ArenaKindCrit.h): Data/Battle/arena_crit.json read once, then on each begin and
	// restart one ArenaKindCrit per slot whose kind has an entry, and the blow hook that rules on the lead's blows on them.
	// After each step the slots' AttackStarted events open the windows; an opening and every crit are one log line each.
	void BeginKindCrits();
	void EndKindCrits();
	void FeedKindCrits(const ftcore::ArenaEvents& Events);
	static ftcore::ArenaBlowRuling RuleKindCritBlow(void* User, const ftcore::ArenaBlow& Blow);
	// The entry's effects (ArenaKindCritEffects) on each crit slot's figure, made once after the figures: the tell plays
	// from the opening move's start until the window opens, the open effect while it is open, the crit and hit effects
	// on the lead's blows on the slot. Hung from the entry's socket on a skinned figure (else its root).
	void SpawnKindCritEffects();
	void StopKindCritEffects();
	UFTVfxComponent* MakeKindCritEffect(AFTArenaFighter* Figure, std::string_view Effect, std::string_view Socket);
	// -ArenaShowcase, the Dealer's block of ShowcaseCue (any kind with an arena_crit entry gets it; white_glove is the one so
	// far): the reel's cues on that entry's four effects, as the fight plays them. The tell for the opening move's clip and
	// for the lone tell, stopped at TellStop; the open effect from GlowOn to GlowOff; the crit on Crit; the hit's ink on
	// Chips, half the body radius out toward the camera at the boss's chest height (the fight plays it at the body's centre).
	// Impact plays nothing (the Dealer has no dust of its own).
	// Null components (-ArenaNoKindFx, or a system that didn't load) play nothing: the OFF half of an effect pair.
	void ShowcaseKindCritCue(const ftcore::ShowCueAt& Cue, AFTArenaFighter& Figure);
	// The kind id of slot I, as the table names it ("" for a def built in code).
	const char* SlotKindId(int32 I) const;
	// The kite_boss's weak point, when the line-up has one and the stone_bearer's host isn't in the fight (ArenaFight has one
	// blow hook): its file read (before the fight begins), then the host begun on the fight (after it begins or restarts).
	void PrepareKite();
	void BeginKite();
	void EndKite();
	// The host's new notes, one log line each.
	void LogKiteNotes();
	// -ArenaBossLoop (review only): finds the boss's slot and the clip's table index once, then logs the loop's start (at each
	// fight's begin). DriveSkin plays that clip looped from the fight's start whatever the fight does, one log line a wrap.
	void BeginBossLoop();
	// Dresses enemy slot I as a skeletal mesh when its kind has a look that loads (see AFTArenaGameMode's header).
	void LoadSkin(int32 I);
	// Picks and times slot I's clip from its Core state: the move's own clip through an attack, at the rate that lands its
	// impact at the end of the startup; stagger; face down while open; the hit reaction between moves; defeat; idle.
	void DriveSkin(int32 I, const ftcore::ArenaEnemyOut& Out, const ftcore::ArenaEnemyKind& Kind, float Alpha);
	// Whether a blow makes slot I flinch: not on its knees (the go-down is its reaction), nor while it gets up from them.
	bool Flinches(int32 I) const { return !bKneeling[I] && Now >= UpOwedUntil[I]; }
	void PutBodiesAtStart();
	// This frame's presses (edges kept until a step takes them), the held guard, the stick and the camera.
	void PollInput(float DeltaSeconds);
	ftcore::ArenaInput TakeInput();
	void StepFight();
	void HandleEvents(const ftcore::ArenaEvents& Events);
	void TrackParts(const ftcore::ArenaOutput& Out);
	void PlaceFigures(float Alpha);
	void SteerCamera(float DeltaSeconds);
	void Finish();
	// The one result line per fight (VICTORY, DEFEAT, or TIMEOUT as a warning while it still runs).
	void LogResult() const;
	APlayerController* GetPC() const;
	FVector ToWorld(const ftcore::Vec2& P) const { return FVector(P.x * 100.0, P.y * 100.0, ZoneZ); }

	UPROPERTY()
	TObjectPtr<AFTArenaFighter> PlayerFigure;
	UPROPERTY()
	TObjectPtr<AFTArenaFighter> EnemyFigures[6]; // ftcore::kMaxArenaEnemies (a literal for UHT; checked in the .cpp)
	UPROPERTY()
	FFTArenaSkin Skins[6];                       // ... likewise
	// The stone_bearer's plinth and the guest's keep-out disc (markers). UPROPERTYs sit up here, ahead of the fight's
	// large members: UHT stores a property's offset in 16 bits.
	UPROPERTY()
	TObjectPtr<AStaticMeshActor> PlinthActor;
	UPROPERTY()
	TObjectPtr<AStaticMeshActor> KeepOutActor;
	// The -ArenaShowcase reel's fixed camera.
	UPROPERTY()
	TObjectPtr<ACameraActor> ShowCamera;
	// The stone_bearer's effects: telegraph, impact, hit chips, crit burst and the open weak point's glow.
	UPROPERTY()
	FFTArenaBossVfx BossVfx;
	// Each crit slot's effects (see SpawnKindCritEffects), owned by its figure.
	UPROPERTY()
	TObjectPtr<UFTVfxComponent> KindCritTellFx[6]; // ftcore::kMaxArenaEnemies (a literal for UHT)
	UPROPERTY()
	TObjectPtr<UFTVfxComponent> KindCritOpenFx[6];
	UPROPERTY()
	TObjectPtr<UFTVfxComponent> KindCritCritFx[6];
	UPROPERTY()
	TObjectPtr<UFTVfxComponent> KindCritHitFx[6];
	// What each skinned enemy plays: the clip (a table index; -2 none yet) and since when, the last hit reaction.
	int32 SkinClip[ftcore::kMaxArenaEnemies];
	double SkinSince[ftcore::kMaxArenaEnemies];
	// The move whose clip it last drew (-1 none) and Core's time into that move then: a new start of a move logs its clip.
	int32 SkinMove[ftcore::kMaxArenaEnemies];
	float SkinMoveTime[ftcore::kMaxArenaEnemies] = {};
	double HitSince[ftcore::kMaxArenaEnemies];
	bool bHitHeavy[ftcore::kMaxArenaEnemies] = {};
	// A walking skin: its smoothed ground speed (m/s), whether it walks, and where it is in the walk clip (s).
	float WalkSpeed[ftcore::kMaxArenaEnemies] = {};
	bool bWalking[ftcore::kMaxArenaEnemies] = {};
	double WalkPhase[ftcore::kMaxArenaEnemies] = {};
	// A skin on its knees (Down, or Open without a clip of its own; one spell however the states chain) and since when, how
	// far into its get-up it was last drawn (s), and until when a get-up cut short by the spell's end is still owed.
	bool bKneeling[ftcore::kMaxArenaEnemies] = {};
	double KneelSince[ftcore::kMaxArenaEnemies] = {};
	double UpAt[ftcore::kMaxArenaEnemies] = {};
	double UpOwedUntil[ftcore::kMaxArenaEnemies] = {};
	// A parried move's recoil still to play (a table index, -1 none) and since when (the parry), and whether the Open went
	// face down first (a stagger after that starts it over). It lasts while the Open, Staggered or Down does.
	int32 RecoilPending[ftcore::kMaxArenaEnemies];
	double RecoilSince[ftcore::kMaxArenaEnemies] = {};
	bool bRecoilDown[ftcore::kMaxArenaEnemies] = {};
	float FrameDelta = 0.f; // the frame's seconds, for the clips' crossfades
	// The stone_bearer's figure: when its walk began, when its get-up began (-1e9: none under way) and whether it is up
	// for the rest of its down, the death clip it chose (-1: not yet), and its move mask as last logged.
	double StoneWalkSince = -1e9;
	// The walk's stop, latched when Core stops wanting it: seconds into the walk it stops at (its next passing pose; < 0: none
	// latched) and why.
	double StoneWalkStopAt = -1.0;
	const TCHAR* StoneWalkStopWhy = TEXT("");
	double StoneUpStart = -1e9;
	bool bStoneStood = false;
	int32 StoneDeathClip = -1;
	uint32 StoneMoveMask = 0xFFFFFFFFu;
	// The split's stagger, drawn from the split's frame until Core's held Stagger takes over (-1e9: none).
	double StoneSplitStaggerSince = -1e9;
	double StoneSplitStaggerUntil = -1e9;
	// The pilot's last heavy on the stone_bearer (kStonePilotHeavyEvery; -1e9: none yet).
	double StonePilotHeavyAt = -1e9;
	// The tell's point, frozen as the startup begins, for the impact's distance check (move kArenaNoMove... none: 255).
	ftcore::Vec2 StoneTellAt;
	uint8 StoneTellMove = 255;
	// The tell's shape scale, Core's facing and the figure's place and yaw as the startup began (the blow's log line).
	float StoneTellShapeMul = 1.f;
	ftcore::Vec2 StoneTellFacing;
	FVector StoneTellFigurePos = FVector::ZeroVector;
	float StoneTellFigureYaw = 0.f;
	// The tell's blow strikes in the air (its contact's "air"): its dust bursts this high (m) over the tell.
	bool bStoneTellAir = false;
	float StoneTellUp = 0.f;
	// The walk's counts this fight: steps between moves Core wanted over the start speed (by brain), the runs of them,
	// the walks begun, and the wanted steps that couldn't start one (by what held it: the cone, a get-up, a hit, a turn, a
	// move due within a cycle). And the steps a turn would have started on but a move was due within its step.
	int32 StoneWantApproach = 0;
	int32 StoneWantStrafe = 0;
	int32 StoneWantRuns = 0;
	int32 StoneWalksBegun = 0;
	int32 StoneHeldCone = 0;
	int32 StoneHeldUp = 0;
	int32 StoneHeldHit = 0;
	int32 StoneHeldTurn = 0;
	int32 StoneHeldPick = 0;
	bool bStoneWanted = false;
	// Each enemy slot's stepping turn.
	FFTArenaTurn SlotTurn[ftcore::kMaxArenaEnemies];
	// The move under way plays its left-hand clip (decided once, as it starts: the run called its blow to the left).
	bool bStoneMoveLeft = false;

	ftcore::ArenaTable Table;
	ftcore::ArenaZoneFit Fit;
	std::string ZoneId;
	double ZoneZ = 0.0;
	int32 ZoneStyle = 0;
	ftcore::ArenaDef Def;
	ftcore::ArenaFight Fight;
	ftcore::ArenaBodies Bodies;
	ftcore::ArenaBodies PrevBodies;
	uint32 Seed = 1;

	// Presses waiting for a step, the held guard, the stick on the ground and the switch.
	bool bPressLight = false;
	bool bPressHeavy = false;
	bool bPressDodge = false;
	bool bPressHeal = false;
	bool bPressLock = false;
	bool bPressRelease = false;
	bool bPressPair = false;
	// Autoplay's partner: a parry came, so press the pair on the next step.
	bool bPartnerDue = false;
	int8 PressSwitch = 0;
	// The partner's Read pick: a card index, -1 for none.
	int8 PressRead = -1;
	bool bGuardHeld = false;
	bool bInterceptHeld = false;
	ftcore::Vec2 Stick;
	bool bStickFlicked = false;

	double Now = 0.0;
	double Accumulator = 0.0;
	double ActiveSince[ftcore::kMaxArenaEnemies];
	double EnemyHitTime[ftcore::kMaxArenaEnemies];
	uint8 LastPart[ftcore::kMaxArenaEnemies] = {};
	double PlayerHitTime = -1e9;
	double ParryTime = -1e9;
	double ReleaseTime = -1e9;
	int32 ReleaseSpent = 0;
	double OverloadTime = -1e9;
	FPopup Popups[MaxPopups];
	int32 PopupTotal = 0;

	float CameraYaw = 0.f;
	float CameraPitch = -16.f;

	bool bBegun = false;
	bool bFiguresSpawned = false;
	bool bAutoPlay = false;
	bool bIdle = false;
	bool bQuitOnFinish = false;
	bool bShots = false;
	bool bFightAgain = true;
	bool bReplacePawn = true;
	bool bHoldLoad = false;
	float ParryHold = 0.f;
	// -ArenaBossLoop: the clip's name, the boss's slot and the clip's table index (-1: off, or not found), whether it was
	// looked for, and the wraps logged this fight.
	FString BossLoopName;
	int32 BossLoopSlot = -1;
	int32 BossLoopClip = -1;
	bool bBossLoopResolved = false;
	int32 BossLoopWraps = 0;
	bool bBossSplitAtStart = false;
	bool bResultLogged = false;
	double ResultTime = 0.0;
	double ResultWait = 0.0; // how long after the result its banner waits (s)
	// How long after the result the autopilot fights again (s): kArenaAutoAgain, or longer for a long defeat clip.
	double AutoAgainWait = 4.0;
	double BeginNow = 0.0;
	FString KitId;

	// The first Arena boss (stone_bearer): its host, its slot in the line-up (-1 without one), the zone's yaw and the
	// notes logged so far (its plinth and keep-out disc are with the figures above).
	FFTStoneBearerHost StoneBearer;
	int32 StoneBearerSlot = -1;
	float ZoneYaw = 0.f;
	int32 StoneBearerLogged = 0;
	mutable bool bWeakPointWarned = false; // the mesh has no weak-point bone (warned once)

	// Plain kinds' weak points: the file (read once), each slot's crit and whether it is on, whether the window was open
	// after the last step, and whether this runner holds the fight's blow hook for them.
	ftcore::ArenaKindCritTable KindCritTable;
	bool bKindCritTableRead = false;
	std::array<ftcore::ArenaKindCrit, ftcore::kMaxArenaEnemies> KindCrits{};
	bool bKindCritSlot[ftcore::kMaxArenaEnemies] = {};
	bool bKindCritWasOpen[ftcore::kMaxArenaEnemies] = {};
	bool bKindCritHook = false;
	// The crit count each slot's effects have answered (ArenaKindCrit::crits), and whether its tell is playing.
	int32 KindCritCritsSeen[ftcore::kMaxArenaEnemies] = {};
	bool bKindCritTelling[ftcore::kMaxArenaEnemies] = {};
	// The Kite-Mother's weak-point host, its slot in the line-up (-1 without one), the notes logged so far and its effects.
	FFTKiteHost Kite;
	int32 KiteSlot = -1;
	int32 KiteLogged = 0;
	FFTKiteVfx KiteFx;

	// -ArenaShowcase: on, the reel (on the heap: it is large), its clock (s; it starts below 0 on the idle's first frame,
	// for the exposure to settle), the last part and cue time played, where the boss stood and faced at the start (the
	// reel's frame), each reel attack's arena move, the strike point its tell was drawn at (for its dust), and done.
	bool bShowcase = false;
	int32 ShowSlot = -1; // the boss shown
	TSharedPtr<ftcore::ClipShow> Show;
	ftcore::ShowSpec ShowSpecUsed;
	double ShowTime = 0.0;
	double ShowCueTime = -1.0;
	int32 ShowSegment = -1;
	bool bShowHidden = false;
	bool bShowUnhideDue = false; // shown again from the next frame (posed this one while still hidden)
	FVector ShowOrigin = FVector::ZeroVector;
	float ShowBaseYaw = 0.f;
	ftcore::Vec2 ShowTellAt;
	float ShowTellShapeMul = 1.f;
	int16 ShowTellAttack = -1;
	bool bShowTellAir = false; // its blow strikes in the air: its dust ShowTellUp (m) over the tell
	float ShowTellUp = 0.f;
	bool bShowDone = false;
};
