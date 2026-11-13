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

#include "ftcore/AimStance.h"
#include "ftcore/AttackMove.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaImpact.h"
#include "ftcore/ArenaLook.h"
#include "FTArenaGlide.h"
#include "ftcore/ArenaKindCrit.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/Places.h"
#include "ftcore/TableRing.h"
#include "ftcore/TableSet.h"
#include "ftcore/BeatChoreo.h"
#include "ftcore/InputBindings.h"

#include "FTArenaBossVfx.h"
#include "FTFightSession.h"
#include "FTKiteHost.h"
#include "FTKiteVfx.h"
#include "FTLeadWeapons.h"
#include "FTStoneBearerHost.h"

#include <array>
#include <string>
#include <string_view>

class UFTVfxComponent;
class UFTArenaJoltModifier;
class UFTBeatBossMetronome;
class UAudioComponent;
namespace ftcore
{
struct Settings;
}

#include "FTArenaRunner.generated.h"

class AFTArenaFighter;
class AStaticMeshActor;
class UStaticMeshComponent;
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
	const FFTLeadLook* Ally = nullptr; // the partner lead's look, worn by the ally in a carrying run (null: the test human)
	uint32 Seed = 1;
	bool bAutoPlay = false;
	bool bIdle = false;
	bool bHoldLoad = false;
	float ParryHold = 0.f; // -ArenaParryHold=<s>
	bool bNoLock = false;  // -ArenaNoLock: the autopilot never presses lock, so its attacks take soft targets
	bool bAutoShoot = false; // -ArenaAutoShoot: with a guns kit the autopilot aims and shoots on the way in
	// -ArenaSharedCount: the autopilot's lead calls the count whenever it can, and a scripted partner (the other lead,
	// with no body or blow of its own in the fight) joins a window the step a lead's blow is held.
	bool bSharedCount = false;
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
	bool IsAutoPlay() const { return bAutoPlay; }
	// How long after the result the autopilot fights again: covers the longest defeat clip (s).
	double GetAutoAgainWait() const { return AutoAgainWait; }
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
	// Off-frame tells (ftcore::arenaOffFrameTells, from the camera's place, facing and field of view): whether enemy Index
	// is winding up out of the view now, and when this windup first went off frame (GetNow()'s clock). The HUD's edge marker and the
	// stinger both read this one answer.
	bool IsOffFrame(int32 Index) const { return Index >= 0 && Index < MaxEnemies && bOffFrame[Index]; }
	double GetOffFrameSince(int32 Index) const { return (Index >= 0 && Index < MaxEnemies) ? OffFrameSince[Index] : -1e9; }
	// The player's settings (null outside a game instance).
	const ftcore::Settings* GetSettings() const;
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
	// The lead's weapon: its strike clips, props and effects (FTLeadWeapons.h); the mode's strike views freeze and thaw it.
	const FFTLeadWeapons& GetLeadWeapons() const { return LeadWeapons; }
	// The aim stance (right mouse held): its blend 0..1, for the HUD's reticle.
	float GetAimAlpha() const { return ArenaAim.alpha(); }
	// A guns kit's crosshair is on an enemy its round would not reach (past the shot's range): the HUD dims it.
	bool IsReticleOutOfReach() const { return bReticleOutOfReach; }
	// A guns kit's round would meet another body before the one under the crosshair (one on the muzzle's line only).
	bool IsReticleBlocked() const { return bReticleBlocked; }
	FFTLeadWeapons& GetLeadWeapons() { return LeadWeapons; }
	// The time the figures were last drawn at, on the held fight clock (Fight.heldTime(), the lead's strike clips' clock):
	// Alpha of the way from its value before the last step to its value after. It stands still through the player's hit
	// pauses and otherwise moves by the delta TickFight is given.
	double GetDrawnTime() const { return DrawnTime; }
	// The clocks for the fight session (the shared combat inputs): the held fight clock (the fight clock less the player's
	// hit pauses), how far it moved over the last TickFight (what the session's Tick takes: 0 through a hit pause), the
	// player's pause left, and each figure's own pause left (0 once the fight is over, and on a dead enemy). All 0 before
	// the fight begins; Begin and Restart start them again.
	float GetHeldTime() const { return bBegun ? Fight.heldTime() : 0.f; }
	float GetHeldDelta() const { return HeldDelta; }
	float GetHitStopLeft() const { return bBegun ? Fight.hitStopLeft() : 0.f; }
	float GetPlayerStopLeft() const { return bBegun ? Fight.output().player.stopLeft : 0.f; }
	float GetEnemyStopLeft(int32 Index) const
	{
		return (bBegun && Index >= 0 && Index < static_cast<int32>(Fight.enemyCount())) ? Fight.output().enemies[static_cast<size_t>(Index)].stopLeft : 0.f;
	}
	// The fight session (the shared combat inputs) the runner owns and ticks on the held clock, and its lead slot (the
	// one the player's figure fills: SessionLeadOf the lead's look, set at Begin).
	UFTFightSession* GetSession() const { return Session; }
	// The fight is one the story handed off (set by each Begin): Restart rolls its story step back.
	bool IsStoryFight() const { return bStoryFight; }
	uint8 GetSessionLead() const { return SessionLead; }

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
	// The stone_bearer's stepping turn (its skinned figure, with both turn clips; see kStoneTurnDeg in the .cpp). Before
	// each step: Core's facing held while it stands, and the turn's facing written while one is under way. After each step:
	// a turn begun, carried into another cycle, ended or cancelled, a log line each.
	bool StoneTurns() const;
	void StoneBearerTurnBeforeTick();
	void StoneBearerTurn(const ftcore::ArenaOutput& Out);
	// The turn's facing T seconds (Fight time) into it.
	ftcore::Vec2 StoneTurnFacing(double T) const;
	// A hit reaction for slot I, light or heavy, when it flinches (the stone_bearer's slot holds a playing one a while).
	void StartFlinch(int32 I, bool bHeavy);
	// The boss's effects: its attack part going From -> To (its tell through the startup, its impact as it turns active),
	// a blow landing on it (E: Hit, CounterStagger or ReadCounter), and where its weak point is now.
	void BossPartChanged(const ftcore::ArenaEnemyOut& Out, uint8 From, uint8 To);
	void BossHitVfx(const ftcore::ArenaEvent& E);
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
	// Plain kinds' weak points (ftcore/ArenaKindCrit.h): Data/Battle/arena_crit.json read once, then on each begin and
	// restart one ArenaKindCrit per slot whose kind has an entry, and the blow hook that rules on the lead's blows on them.
	// After each step the slots' AttackStarted events open the windows; an opening and every crit are one log line each.
	void BeginKindCrits();
	void EndKindCrits();
	void FeedKindCrits(const ftcore::ArenaEvents& Events);
	static ftcore::ArenaBlowRuling RuleKindCritBlow(void* User, const ftcore::ArenaBlow& Blow);
	// The session's ruling, chained after the hook the sub-hosts set (kind crits, kite, stone bearer): installed after
	// they begin, at the fight's begin and each restart.
	static ftcore::ArenaBlowRuling RuleSessionBlow(void* User, const ftcore::ArenaBlow& Blow);
	void InstallSessionHook();
	// The entry's effects (ArenaKindCritEffects) on each crit slot's figure, made once after the figures: the tell plays
	// from the opening move's start until the window opens, the open effect while it is open, the crit and hit effects
	// on the lead's blows on the slot. Hung from the entry's socket on a skinned figure (else its root).
	void SpawnKindCritEffects();
	void StopKindCritEffects();
	UFTVfxComponent* MakeKindCritEffect(AFTArenaFighter* Figure, std::string_view Effect, std::string_view Socket);
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
	void HoldPausedEnemies();
	void StepFight();
	void HandleEvents(const ftcore::ArenaEvents& Events);
	// The shared combat inputs beside the fight (docs/SHARED_COMBAT_INPUTS.md section 7): presses before the step,
	// then the session ticked by the held clock's advance and the player's landed blows judged on the count.
	void PressSession(const ftcore::ArenaInput& In);
	void FeedSession();
	void BeginSession();
	void TrackParts(const ftcore::ArenaOutput& Out);
	void PlaceFigures(float Alpha);
	void SteerCamera(float DeltaSeconds);
	// Impact feedback (ftcore/ArenaImpact.h): the jolt kicked by a blow's event, stepped on the frame's clock (so it plays
	// through the hit pause) and handed to the camera modifier; the off-frame tells and their stinger.
	void KickJolt(const ftcore::ArenaEvent& E);
	void TickImpact(float DeltaSeconds);
	void EndImpact();
	float ShakeScale() const;
	// The aim stance this frame (right mouse held) and its reticle point; the camera pulled in by it.
	void TickAim(float DeltaSeconds);
	void ApplyAimCamera();
	void Finish();
	// The one result line per fight (VICTORY, DEFEAT, or TIMEOUT as a warning while it still runs).
	void LogResult() const;
	// One "Arena check:" line per combat feature (Core's arenaFeatureChecks over the fight's stats), for the headless
	// demo: PASS, FAIL, or n/a where nothing in the line-up can show it.
	void LogFeatureChecks() const;
	// Carrying (Load by chairs), off unless -ArenaChairs, -ArenaCap or -ArenaAlly asks for it (never in a shipping
	// build until promoted): the cap, block Load, the chairs and the ally go on the fight before it begins and stay
	// across restarts (ArenaFight keeps them).
	void BeginCarry(const FFTLeadLook* AllyLookIn);
	void SpawnAlly(UWorld* World);
	// Where the ally stands behind the player's left (m), facing `Facing`.
	ftcore::Vec2 AllyHome(const ftcore::Vec2& Player, const ftcore::Vec2& Facing) const;
	void MoveAlly();
	void PlaceAlly(float Alpha);
	void LogCarry() const;
	// Setting the table (-ArenaTable, a carrying run's): its switches, its reset (each attempt), its step after the
	// fight's, its ring drawn, its log line and its ring's removal.
	void BeginTable(const ftcore::PlaceTable& Places, bool bCapArg);
	void ResetTable();
	void TickTable(const ftcore::ArenaOutput& Out);
	void BeginTableNow(double T);
	void SpawnTable();
	void PaintTable(float Alpha);
	void LogTable() const;
	void EndTable();
	// Briefs (Lattice slice 1), off unless -ArenaBrief=<id> asks (never in a shipping build until promoted): the
	// lead's kit takes that Brief's kit from Data/Battle/briefs.json (the cylinder and the kit changes) before the
	// fight begins. Only the guns lead's own kit has Briefs; any other is left alone with a warning.
	// Slice 2: the kit is built from the Brief's stats (BriefKit); every stat 10 unless -ArenaRating=<1..30> asks
	// for that Rating's stats. A bad Rating, or one without -ArenaBrief, stops the run.
	void BeginBrief();
	void LogBrief() const;
	static uint8 SessionLeadOf(const FFTLeadLook* Look);
	APlayerController* GetPC() const;
	FVector ToWorld(const ftcore::Vec2& P) const { return FVector(P.x * 100.0, P.y * 100.0, ZoneZ); }

	// The shared combat inputs, ticked on Fight.heldTime() (it stands still in the player's hit pauses). Declared
	// first among the members: UHT keeps a property's offset in 16 bits, and the fight state below is large.
	UPROPERTY()
	TObjectPtr<UFTFightSession> Session;
	bool bStoryFight = false; // BeginSession: the story handed this fight off
	bool bRestarting = false; // Restart's BeginSession: not the fight's first attempt
	UPROPERTY()
	TObjectPtr<AFTArenaFighter> PlayerFigure;
	UPROPERTY()
	TObjectPtr<AFTArenaFighter> EnemyFigures[6]; // ftcore::kMaxArenaEnemies (a literal for UHT; checked in the .cpp)
	UPROPERTY()
	TObjectPtr<AFTArenaFighter> AllyFigure;      // the ally in a carrying run (BeginCarry)
	UPROPERTY()
	FFTArenaSkin Skins[6];                       // ... likewise
	// The stone_bearer's plinth and the guest's keep-out disc (markers). UPROPERTYs sit up here, ahead of the fight's
	// large members: UHT stores a property's offset in 16 bits.
	UPROPERTY()
	TObjectPtr<AStaticMeshActor> PlinthActor;
	UPROPERTY()
	TObjectPtr<AStaticMeshActor> KeepOutActor;
	// A set table's ring on the ground, its countdown arc and the lines to the setter (-ArenaTable; its parts are its
	// components).
	UPROPERTY()
	TObjectPtr<AStaticMeshActor> TableActor;
	// The impact jolt's camera modifier (added to the player's camera manager on the first frame it is there) and the
	// off-frame stinger's procedural voice and its sound (null with no audio device: the marker carries the tell alone).
	UPROPERTY()
	TObjectPtr<UFTArenaJoltModifier> JoltModifier;
	UPROPERTY()
	TObjectPtr<UFTBeatBossMetronome> Stinger;
	UPROPERTY()
	TObjectPtr<UAudioComponent> StingerAudio;
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
	// The walk's counts this fight: steps between moves Core wanted over the start speed (by brain), the runs of them,
	// the walks begun, and the wanted steps that couldn't start one (by what held it: the cone, a get-up, a hit).
	int32 StoneWantApproach = 0;
	int32 StoneWantStrafe = 0;
	int32 StoneWantRuns = 0;
	int32 StoneWalksBegun = 0;
	int32 StoneHeldCone = 0;
	int32 StoneHeldUp = 0;
	int32 StoneHeldHit = 0;
	int32 StoneHeldTurn = 0;
	bool bStoneWanted = false;
	// The stone_bearer's stepping turn: which way (+1 right, the yaw rising; -1 left; 0 none), since when (Fight time), the
	// facing it began from, the cycles it is committed to, the crossfade into its clip (s), where its pivot foot stood as
	// the cycle began (when found) and whether the first cycle's foot check is logged, and the fight's turn counts.
	int32 StoneTurnSign = 0;
	double StoneTurnStart = -1e9;
	ftcore::Vec2 StoneTurnFrom;
	int32 StoneTurnCycles = 0;
	double StoneTurnBlend = 0.25;
	FVector StoneTurnFoot = FVector::ZeroVector;
	bool bStoneTurnFoot = false;
	bool bStoneTurnChecked = false;
	int32 StoneTurnsBegun = 0;
	int32 StoneTurnSteps = 0;
	int32 StoneTurnsCancelled = 0;
	// The move under way plays its left-hand clip (decided once, as it starts: the run called its blow to the left).
	bool bStoneMoveLeft = false;

	ftcore::ArenaTable Table;
	ftcore::ArenaZoneFit Fit;
	std::string ZoneId;
	double ZoneZ = 0.0;
	int32 ZoneStyle = 0;
	ftcore::ArenaDef Def;
	ftcore::ArenaFight Fight;
	float SessionHeld = 0.f;         // Fight.heldTime() at the last session tick
	bool bSessionGuard = false;      // the guard as last sent to the session (refusal hold edges)
	uint8 SessionLead = ftcore::kLeadA; // the lead slot the player's figure fills
	ftcore::ArenaBlowHook SessionInnerHook = nullptr; // the sub-hosts' hook the session's ruling chains after
	void* SessionInnerUser = nullptr;
	bool bSessionPartnerDue = false; // -ArenaSharedCount: a lead's blow is held, the scripted partner joins after the step
	int32 SessionHeldBlows = 0;      // this fight's lead blows judged Held
	int32 SessionJoins = 0;          // the scripted partner's blows that joined a window
	int32 SessionBonuses = 0;        // held-blow bonuses the fight took (hostBlow)
	int32 SessionBonusDamage = 0;    // their extra damage
	ftcore::ArenaBodies Bodies;
	ftcore::ArenaBodies PrevBodies;
	// The lead's speed as its body moved (m/s; the sim's velocity less what the bodies' push took off), the median of
	// the last three steps' speeds: the legs' cycle runs at it, so they step at the pace the feet are drawn moving, and
	// it sets the strikes' upper share. A median, not one step: a body pushed in and out against another swings step to
	// step, and the gait and the foot lock would flip with it each frame.
	float PlayerStepSpeed = 0.f;
	float LeadWantSpeed = -1.f; // the walk's wanted pace handed to the lead's legs (m/s; -1 unknown), held through a hit pause
	float LeadStopDistance = -1.f; // and how far the body would slide at the brake (m; -1 unknown), held with it
	float PlayerSteps[3] = {0.f, 0.f, 0.f};
	int32 PlayerStepAt = -1; // the next slot in PlayerSteps; -1 after a start, a teleport or a facing snap
	float PlayerStepFacing = 0.f; // rad, at the last step
	bool bDrawSnap = false; // -ArenaDrawSnap: the mesh yaws by the rules' facing (the old snap), for before/after captures
	bool bWasDodging = false; // the player's last drawn action was a roll (logs each roll's start)
	// The facing the player's mesh is drawn with: the eased drawn facing, or the rules' one under -ArenaDrawSnap.
	ftcore::Vec2 PlayerDrawFacing(const ftcore::ArenaPlayerOut& P) const { return bDrawSnap ? P.facing : P.drawFacing; }
	static constexpr float kMaxStepSpeed = 12.f; // m/s: a faster step is a teleport (the dodge is under 6)
	static constexpr float kStepSnapTurn = 1.f;  // rad in one step: a snap, not a running turn
	// The strike clips' upper-body share this step, eased toward attackUpperShare of the walk (the lunge left out).
	float LeadUpperShare = 0.f;
	// The share moves by at most this much a second, so a strike's legs don't flip between the clip and the walk.
	static constexpr float kUpperShareRate = 6.f;
	// The lead's legs turned from its facing toward the way it moved over the last step (degrees, + right; 0 standing
	// or while a strike's clip has the legs), eased at kLegYawRate: its gaits only go forward, so a strafe walks them
	// sideways under a chest that keeps facing.
	float LeadLegYaw = 0.f;
	static constexpr float kLegYawRate = 420.f;
	static constexpr float kLegYawMax = 95.f;
	uint32 Seed = 1;

	// Presses waiting for a step, the held guard, the stick on the ground and the switch.
	bool bPressLight = false;
	bool bPressHeavy = false;
	bool bPressDodge = false;
	bool bPressHeal = false;
	bool bPressLock = false;
	bool bPressRelease = false;
	bool bPressPair = false;
	bool bPressTakeHalf = false;
	// Autoplay's partner: a parry came, so press the pair on the next step.
	bool bPartnerDue = false;
	int8 PressSwitch = 0;
	// The partner's Read pick: a card index, -1 for none.
	int8 PressRead = -1;
	bool bGuardHeld = false;
	bool bInterceptHeld = false;
	// The aim (right mouse held) and the attack button's tap and hold (left mouse; ftcore::AimStance, Data/Battle/aim.json,
	// loaded once), the reticle point (world, cm) and whether the camera was last left pulled in.
	ftcore::AimStance ArenaAim;
	// Attacking on the move (Data/Battle/attack_move.json, loaded with aim.json).
	ftcore::AttackMoveRules AttackMoveTable;
	ftcore::PressHold AttackHold;
	// Guard's hold or toggle latch (FTInputBindings).
	ftcore::HeldInput GuardInput;
	ftcore::HeldInput AimInput;
	ftcore::HeldInput InterceptInput;
	bool bAimRulesTried = false;
	bool bAimHeld = false;
	bool bAimCamera = false;
	FVector ReticlePoint = FVector::ZeroVector;
	// The point the aim's heading on the ground is taken toward: the reticle's point, or with the trace meeting nothing
	// the camera's line carried out to reticleRange along the ground (a steep look's far point lies nearly over the
	// lead, off the line by the shoulder offset).
	FVector ReticleHeadingPoint = FVector::ZeroVector;
	bool bReticleHit = false;
	int32 ReticleEnemy = -1; // the standing enemy whose body the reticle's line meets first (-1: none)
	bool bReticleOutOfReach = false; // see IsReticleOutOfReach
	bool bReticleBlocked = false;    // see IsReticleBlocked
	// The blocked crosshair's hysteresis: another body blocks only when the crosshair's own is met more than
	// kBlockMargin (m) further along the round, and the shown state changes once the new one has held kBlockHoldFrames.
	static constexpr float kBlockMargin = 0.25f;
	static constexpr int32 kBlockHoldFrames = 4;
	int32 BlockedRun = 0;  // frames the unshown state has held
	int32 BlockedFor = -1; // the crosshair's body that state is about (a new one starts clear)
	// The first standing enemy body (Core's radius, the figure's height and hover, as an upright cylinder) along the
	// line From -> To (cm) nearer than T on entry (0..1 of the line): its slot, and T then the share along the line. The
	// figures carry no collision, so the reticle's trace meets them by this.
	int32 ReticleBody(const FVector& From, const FVector& To, double& T) const;
	// -ArenaAimCheck (autopilot, a guns kit): once in the fight, the camera aims at the tallest standing enemy's top
	// (its height and hover) by ftcore::arenaLookUpPitch, the reticle is traced and one aimed shot is pressed; the
	// check line says whether the view reached it and the shot connected. 0 off, 1 waiting, 2 aiming, 3 fired, 4 done.
	int32 AimCheckPhase = 0;
	int32 AimCheckSlot = -1;
	int32 AimCheckPresses = 0;
	double AimCheckSince = 0.0;
	int32 AimCheckShotsBefore = 0;
	int32 AimCheckHitsBefore = 0;
	float AimCheckPitch = 0.f;
	FString AimCheckLine;
	bool bAimCheckPass = false;
	bool bAimCheckNa = false;      // the check could not be judged (the target fell first, no camera)
	bool bAimCheckOnBoss = false;  // the crosshair's line met the target as the shot was pressed
	bool bAimCheckPitchOk = false; // the view had reached the solved pitch (within 1.5 degrees)
	float AimCheckHpBefore = 0.f;
	float AimCheckRendered = 0.f;  // the view pitch the camera rendered as the shot was pressed
	bool bAimCheckFire = false;
	// The tallest standing enemy's slot (-1: none) and its top over the floor (m).
	int32 TallestStanding(float& OutTop) const;
	// The aim's heading on the ground from the lead toward the reticle (Fallback when it lies on the lead).
	ftcore::Vec2 ReticleAimDir(const ftcore::Vec2& Fallback) const;
	// The round's climb (rad) from the lead's muzzle (ftcore::kArenaShotHeight over its floor) to the reticle's heading
	// point; false (and 0) with no reticle traced.
	bool ReticlePitch(float& OutPitch) const;
	// The same toward the middle of enemy Slot's body (its hover and height); false for no such standing enemy.
	bool BodyPitch(int32 Slot, float& OutPitch) const;
	// The rendered view's rotation without the impact jolt (the camera's own, as the reticle and the tells read it).
	FRotator ViewRotation(const APlayerCameraManager& Camera) const;
	// -ArenaAimCheck's input for one step under the autopilot (replaces In while it aims and fires).
	void StepAimCheck(ftcore::ArenaInput& In);
	ftcore::Vec2 Stick;
	bool bStickFlicked = false;
	bool bMouseFlicked = false;

	double Now = 0.0;
	double Accumulator = 0.0;
	double DrawnTime = 0.0;
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

	// Impact feedback. The jolt, the camera shake the run forces (-ArenaCameraShake=<0..1>; below 0: the setting), the
	// blows that asked for a kick and the largest offset handed to the camera (degrees). The view the off-frame test
	// last used; per enemy, whether it is off frame, since when, and whether this windup has been told; the last
	// stinger (Now), and the counts for the check line.
	ftcore::ArenaJolt Jolt;
	float ShakeOverride = -1.f;
	// -ArenaGlide=<toward|across|away|pulse|free>: the glide probe (FTArenaGlide.h) on; with the autopilot, its stick held that way
	// (pulse: 0.8 s toward, 0.6 s let go, 0.8 s away, 0.6 s let go, over and over)
	// to its target (toward, across to the right, away) whenever the autopilot itself leaves the stick at rest, so its
	// strikes play with the stick held as a player's do (free: the autopilot's own stick). -1: off; 3: free.
	int32 GlideStick = -1;
	bool GlideRolling = false; // the player was rolling at the last step (-ArenaGlide=rollstrike)
	int32 GlideSideRolls = 0;  // rolls pressed so far under -ArenaGlide=sidestep (odd: to the target's left)
	FFTArenaGlideProbe Glide;
	int32 JoltAsks = 0;
	float JoltApplied = 0.f;
	ftcore::ArenaView CameraView;
	bool bOffFrame[ftcore::kMaxArenaEnemies] = {};
	double OffFrameSince[ftcore::kMaxArenaEnemies] = {};
	bool bWindupTold[ftcore::kMaxArenaEnemies] = {};
	double LastStinger = -1e9;
	int32 OffFrameWindups = 0;
	int32 Stingers = 0;

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
	bool bNoLock = false;
	bool bAutoShoot = false;
	bool bSharedCount = false;
	// Carrying (BeginCarry): on at all, the ally's body, the autopilot's take half, the chair keys named and their
	// effects, the cap given, the partner's look, and when the ally was last hit (its tint).
	bool bCarry = false;
	bool bAllyOn = false;
	bool bTakeHalfAuto = false;
	bool bAutoBlock = false; // -ArenaAutoBlock (carrying runs): the autopilot guards through every wind-up
	FString CarryChairKeys;
	ftcore::ArenaChairs CarryChairs;
	int32 CarryCap = ftcore::kMaxLoad;
	const FFTLeadLook* AllyLook = nullptr;
	double AllyHitTime = -1e9;
	int32 CarrySharedOffers = 0;
	// Setting the table (BeginTable): on, the autopilot sets it at full Load, the Load filled for a capture; the
	// places it counts, its clock and ring, and the counts for the TABLE line.
	bool bTable = false;
	bool bTableAuto = false;
	bool bTableFill = false;
	ftcore::PlaceTable TablePlaces;
	ftcore::TableSet Table;
	ftcore::TableRing TableSeats;
	int32 TableBegins = 0;
	int32 TableRefusals = 0;
	int32 TableSets = 0;
	int32 TableMostBodies = 0;
	int32 TableStands = 0;
	int32 TableLeaves = 0;
	int32 TableGutters = 0;
	int32 TableFails = 0;
	int32 TableClears = 0;
	int32 TableShares = 0;
	int32 TableShareHp = 0;
	ftcore::TableError TableLastRefusal = ftcore::TableError::None;
	TArray<TObjectPtr<UStaticMeshComponent>> TableRingParts; // the ring's segments
	TArray<TObjectPtr<UStaticMeshComponent>> TableArcParts;  // the countdown arc's
	TArray<TObjectPtr<UStaticMeshComponent>> TableLineParts; // a line per seat but the setter's: the ally, each enemy
	FString BriefId; // -ArenaBrief: the Brief laid over the kit ("" none)
	FString BriefStats;    // the five stats the kit was built from, for the log
	int32 BriefRating = 0; // -ArenaRating: the Brief's stats at this Rating (0: every stat 10, the plain kit)
	// The held fight clock (Fight.heldTime()) before the last step, and how far it moved over the last TickFight.
	double PrevHeldTime = 0.0;
	float HeldDelta = 0.f;
	// Seconds of each enemy's hit pause the steps of this frame covered (its clips leave them out).
	float PausedThisFrame[ftcore::kMaxArenaEnemies] = {};
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
	// The lead's weapon on the player's figure (begun as it is spawned, when it wears a look).
	FFTLeadWeapons LeadWeapons;
	// The direction from the lead to its aim target, set each step with the aim; the figure turns to it while
	// it attacks.
	ftcore::Vec2 LeadAimDir{1.f, 0.f};
	bool bLeadAimDir = false;
};
