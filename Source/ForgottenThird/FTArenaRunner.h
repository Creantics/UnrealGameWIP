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
#include "CollisionQueryParams.h"

#include "ftcore/AimStance.h"
#include "ftcore/AttackMove.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/PartnerBrain.h"
#include "ftcore/HallucinationArena.h"
#include "ftcore/ArenaImpact.h"
#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaCloseFrame.h"
#include "ftcore/ArenaGun.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/CameraPreset.h"
#include "FTArenaGlide.h"
#include "ftcore/ArenaKindCrit.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/Places.h"
#include "ftcore/ReadyStance.h"
#include "ftcore/TableRing.h"
#include "ftcore/TableSet.h"
#include "ftcore/BeatChoreo.h"
#include "ftcore/InputBindings.h"
#include "ftcore/UiPartyScreen.h"

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
struct FHitResult;
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
	uint8 LeadSeat = 0; // the lead's place in the party (0 to 2): the HUD's mark and name
	uint8 AllySeat = 0; // the ally's place in the party
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
	static constexpr float StandOffRunSeconds = 18.f; // -ArenaStandOff: the run ends after this much fight time

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
	// Whether the jump's key is read in a fight: -ArenaJump, until the user has tried it; never in a shipping build.
	// Off, a fight plays as before the jump (Core's jump is only ever pressed by the jump probes).
	static bool JumpBound();
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
	// The fight was refused (no playable lead: Begin's guard, or the session's check): it never runs.
	bool IsRefused() const { return bLeadRefused; }
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
	// The partner's figure in a carrying run; null without one.
	AFTArenaFighter* GetAllyFigure() const { return AllyFigure; }
	// Logs the lead's place in the rendered frame as last sampled for "lead in frame" (its shares and whether it kept
	// the rule), tagged with a view's name: -ArenaShotAt's views say which frames broke the rule. Not in Shipping.
	void LogLeadFrameShot(const TCHAR* Shot) const;
	// The zone the fight is held in, for the HUD's edge cue: its walls, the world's look (0 the plain zone, 1 Solhaven,
	// 2 the Lattice, 3 the Unwritten), where the player stands (metres) and the height of the ground there (cm).
	const ftcore::ArenaZone& GetZone() const { return Fit.zone; }
	int32 GetZoneStyle() const { return ZoneStyle; }
	ftcore::Vec2 GetPlayerPosition() const { return Bodies.player; }
	// Everyone's position after the last step (metres).
	const ftcore::ArenaBodies& GetBodies() const { return Bodies; }
	// The table (-ArenaTable) for the HUD and effects: on at all, and its state, ring, seats and the Release hold.
	// A shared blow arrives as an ArenaEventType::TableShare event (in the popup stream, the target tinted as hit).
	bool IsTableOn() const { return bTable; }
	// Enemy Slot's rank for the HUD: its kind's carry grade (arena.json "carry"), 0 none, at most 5; 0 for an empty slot.
	int32 GetSlotRank(int32 Slot) const
	{
		return (Slot >= 0 && Slot < static_cast<int32>(Fight.def().enemyCount)) ? FMath::Clamp(static_cast<int32>(Fight.def().enemies[Slot].carry), 0, 5) : 0;
	}
	ftcore::ArenaTableView GetTableView() const;
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
	// Where the last blow on the lead came from, from the lead toward the striker in the camera's ground frame as it was
	// drawn then: X to the camera's right, Y ahead, a unit vector (the hit flash's side). False before any blow this
	// fight, and when the last blow had no enemy behind it (a hazard): no side then.
	bool GetLastHitFrom(FVector2D& RightAhead, double& At) const
	{
		RightAhead = LastHitFrom;
		At = LastHitAt;
		return LastHitAt > -1e8 && !LastHitFrom.IsZero();
	}
	// The lock's aim point (the telegraph, else the weak point, else the chest of a tall target) above the frame this
	// frame: true, with EdgeShare its place on the screen's top edge (X 0 left to 1 right, Y 0 the top). The UI draws
	// the marker there.
	bool GetLockAimAbove(FVector2D& EdgeShare) const
	{
		EdgeShare = FVector2D(LockAimX, 0.0);
		return bLockAimAbove;
	}
	double GetAllyHitTime() const { return AllyHitTime; }
	double GetParryTime() const { return ParryTime; }
	// The last Release (when it began and the Load it spent) and the last time the Load overloaded.
	double GetReleaseTime() const { return ReleaseTime; }
	int32 GetReleaseSpent() const { return ReleaseSpent; }
	// The kind id of enemy slot I, as the table names it ("" for a def built in code): the HUD's name key.
	const char* GetSlotKindId(int32 I) const { return SlotKindId(I); }
	// The count's Load and the Lift (-ArenaCountLift): the last Lift (its join time, -1e9 when none, and multiplier),
	// the Load the count stored, the Lifts, and a Release's mark per enemy slot (0 / -1e9 when unmarked).
	double GetLiftTime() const { return LiftTime; }
	float GetLiftMultiplier() const { return LiftMultiplier; }
	int32 GetCountLoadStored() const { return CountLoadStored; }
	int32 GetLifts() const { return Lifts; }
	float GetEnemyMarkBonus(int32 Slot) const;
	double GetEnemyMarkUntil(int32 Slot) const;
	// The partner lead's Load (-ArenaCountLift; the session keeps it): now, its cap (0 / 0 when none is kept), the
	// Load it stored this fight and its last change (-1e9 when none). The kept gauge follows control
	// (FightSession::keptLead): it is always on the lead without control, so it is the partner's.
	int32 GetPartnerLoad() const;
	int32 GetPartnerLoadCap() const;
	int32 GetPartnerLoadStored() const { return PartnerLoadStored; }
	double GetPartnerLoadTime() const { return PartnerLoadTime; }
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
	// The world point of the weak point on the boss in Slot (the stone bearer's set stone, the Kite-Mother's seam); false
	// when that slot holds no boss with one.
	bool GetBossWeakPoint(int32 Slot, FVector& Out) const;
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
	// For the HUD, each 0 (false) with no session or, for the refusal's, without -ArenaHallucination: the gift slot's
	// Heals left this fight and the seconds until the Heal and the Guard are ready (0 = ready; the Guard's time runs
	// from press to press, its guard included); a refusal that closed an offer too early to open anything (true for
	// kRefusedTooEarlyShow s from that step); the Escrow Brief's wait before a hold builds, and its seconds left; the
	// lock's seconds left, through which a refusal opens nothing.
	int32 GetGiftHealsLeft() const;
	double GetGiftHealReadyIn() const;
	double GetGiftGuardReadyIn() const;
	// For the GIFT row's prompts: what a press of gift `G` at the lead would answer now (the session's giftReason, the
	// press's own slot check, with the lead's health as the press sets it); Absent with no session.
	ftcore::PressReason GetGiftReason(ftcore::Gift G) const;
	bool IsRefusedTooEarly() const;
	bool IsEscrowWaiting() const;
	float GetEscrowWaitLeft() const;
	float GetWeakPointLockFor() const;
	static constexpr float kRefusedTooEarlyShow = 1.5f;
	// The Lone Carry's bargain card: true for kLoneBargainShow s from the step it began; its terms (the session's
	// tuning) and the roll as it stands (the save's lone Releases plus this fight's, until a win folds them in).
	bool IsLoneBargainDue() const;
	ftcore::LoneBargain GetLoneBargain() const;
	ftcore::LoneRoll GetLoneRoll() const;
	static constexpr float kLoneBargainShow = 4.f;
	// The fight is one the story handed off (set by each Begin): Restart rolls its story step back.
	bool IsStoryFight() const { return bStoryFight; }
	uint8 GetSessionLead() const { return SessionLead; }
	// The lead the player does not control, and whether the session has it in this fight (def.inFight): the one
	// source of truth for "a partner is here" (the hallucination's partnerPresent reads it; without a partner body
	// the session fights one lead).
	uint8 OtherLead() const { return SessionLead == ftcore::kLeadA ? ftcore::kLeadB : ftcore::kLeadA; }
	bool PartnerInSession() const { return Session != nullptr && Session->Get().inFight(OtherLead()) && !Fight.lone(); }
	// The party places (0 to 2) of the lead and the carrying run's ally, for the HUD's marks.
	uint8 GetLeadSeat() const { return LeadSeat; }
	uint8 GetAllySeat() const { return AllySeat; }
	// The lead's and the ally's look names (empty for the test human), the HUD's party rows' names.
	// They follow the figures: a switch swaps them at once, a takeover when its camera move starts (0.3 s after the fall).
	const FString& GetLeadName() const { return bNamesSwapped ? AllyName : LeadName; }
	const FString& GetAllyName() const { return bNamesSwapped ? LeadName : AllyName; }
	// The third slot as the session holds it (a checkpoint keeps it), and why any gift press is refused in it now
	// (None for a Companion; Vacant or Hostile otherwise). With no session: the fight's def value, and None.
	ftcore::SlotState GetThirdSlot() const;
	ftcore::GiftError GetThirdSlotRefusal() const;
	// The party screen (the new UI, FT::NewUiEnabled): Party down opens it on the fight's party block; a flick moves
	// the cursor, the release confirms. Open: the look and the target flick are held, the left stick still moves the
	// lead.
	const ftcore::UiPartyScreen& GetPartyScreen() const { return PartyScreen; }
	// Why a switch to session lead Lead would be refused now, the party row's word (ftcore::uiPartyLeadWhy over
	// FightSession::leadSwitchReason). THE one host-side condition ahead of the session: a lead with no body in this
	// fight cannot be controlled, so the other lead with no body (PartnerBody false: ordinary play, a Lone Carry, a
	// join not landed yet) is NOT HERE whatever the session says. Everything else is the session's word. Unknown with
	// no session.
	ftcore::UiPartyWhy GetLeadSwitchReason(uint8 Lead) const;
	// The last switch the session refused at a step's close (FightTick::switchRefused), as a count that rises with each
	// and its word: the HUD's SWITCH: <WORD> call.
	uint32 GetSwitchRefusedSerial() const { return SwitchRefusedSerial; }
	ftcore::UiPartyWhy GetSwitchRefusedWhy() const { return SwitchRefusedWhy; }
	// For the HUD's refused prompts (ftcore::uiRefusedStep): a count per prompt, in ftcore::UiPrompt's order (HEAL, GIFT
	// HEAL, GIFT GUARD, PARTNER, ATTACK, GUARD, DODGE, JUMP), that rises with each press of its key by the player, a press
	// the rules refuse included. The autopilot presses none.
	static constexpr int32 PromptPressCount = 8;
	uint32 GetPromptPresses(int32 Prompt) const { return Prompt >= 0 && Prompt < PromptPressCount ? PromptPresses[Prompt] : 0u; }

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
	// Not in a boss's break move's windup or active part (Core's enemyBreakGuard): blows there never flinch it.
	bool Flinches(int32 I) const { return !bKneeling[I] && Now >= UpOwedUntil[I] && !Fight.enemyBreakGuard(static_cast<size_t>(I)); }
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
	// The gift slot's press result, applied: a Heal's share of the lead's max hp and its Load moved off the lead (a
	// Guard's scale is read on every step instead, ArenaInput::takenScale; a Count is already called).
	void ApplyGift(const ftcore::GiftResult& Result);
	void TrackParts(const ftcore::ArenaOutput& Out);
	void PlaceFigures(float Alpha);
	void SteerCamera(float DeltaSeconds);
	// The lock's frame on a tall target (ftcore/ArenaLockFrame.h): the rig at pitch 0 (the zone's and the aim's), the
	// share of its offset the boom's wall probe leaves, the frame's aspect, the lead's body, and the height (m over the
	// lead's floor) the lock aims at on the target.
	ftcore::ArenaCameraPose LockRig() const;
	float BoomWallShare() const;
	float LockAspect() const;
	ftcore::ArenaLeadBody LeadBody() const;
	float LockAimHeight(int32 Target, float Base, float Top) const;
	// The arena camera's preset (ftcore/CameraPreset.h), view only: the presets read once from
	// Data/Battle/camera_presets.json, and at each Begin the one this fight is looked at through (-ArenaCamera=, else the
	// settings' Camera row), applied to Fit.camera there. Its lead rule is what the hold, the low fit, the partner's
	// turn and the lead-frame check all keep (ftcore::cameraPresetLeadRules); its cut is LeadBody's.
	void PickCameraPreset();
	// The preset's rule; in tall contact the feet margin goes to the lock's (the waist above the bottom margin).
	ftcore::ArenaLeadFrameRules LeadFrameRules() const
	{
		return ftcore::arenaCloseContactLowLeadRules(ftcore::cameraPresetLeadRules(CameraLens), bCloseShape ? CloseContactLow : 0.f);
	}
	ftcore::CameraPresets CameraPresetTable;
	bool bCameraPresetsTried = false;
	ftcore::CameraPresetId CameraPresetNow = ftcore::CameraPresetId::Classic;
	ftcore::CameraPreset CameraLens;
	float CameraLag = 0.f; // s, given to the boom (AFTArenaFighter::SetCameraLag): the preset's, or -ArenaCameraLag's
	float LockLow = 0.f;          // 0..1: the low lens's share (eased at ftcore::kArenaLow.rate toward LowFit)
	bool bShortArmLogged = false; // the arm under ftcore::kArenaHeadKeptArm was logged this fight
	bool bLowRig = false;         // the low rig was last given to the boom
	float LowSideSign = 1.f;      // the low shoulder's side (ftcore::arenaLowSide: +1 right, -1 left)
	float LowSideNow = 1.2f;      // m: the low shoulder as eased toward that side
	float LowFit = 1.f;           // the low share the arm allows (ftcore::arenaLowFit), for the arm, aspect and head below
	float LowFitArm = -1.f, LowFitAspect = 0.f, LowFitHead = 0.f;
	float PartnerYawNow = 0.f;    // degrees: the partner's soft turn (ftcore::arenaPartnerYaw), eased
	float PartnerYawGoal = 0.f;
	// The close lens's shape (ftcore/ArenaCloseFrame.h), close only (classic reads none of it): each frame ShapeCloseLens
	// writes the shaped pivot, side and fov on Fit.camera, and the target's share of the width and the cut's least
	// share on CameraLens, from CloseBase (the preset as picked); LeadBody takes the shaped cut.
	void ShapeCloseLens(int32 Target, float DeltaSeconds);
	void PickCloseShoulder(int32 Target);
	bool bCloseShape = false;     // the close preset is on this fight
#if !UE_BUILD_SHIPPING
	bool bPartnerStation = true;  // -ArenaPartnerStation=0 (capture only): no close station handed to the brain
#else
	static constexpr bool bPartnerStation = true; // shipping: the close preset always hands the brain its station
#endif
	// A ranged partner under the close lens: the partner's kit has its own range and the close station hands it to the brain.
	bool RangedPartner() const;
	ftcore::CameraPreset CloseBase;
	float CloseMelee = 0.f;       // 0..1: the melee share, eased (ftcore::arenaCloseMeleeStep)
	float CloseTall = 0.f;        // 0..1: the followed target's tallness (ftcore::arenaCloseTallness)
	float CloseShoulder = 1.f;    // -1..1: the shoulder as eased (+1 right)
	float CloseShoulderWant = 1.f;
	int32 CloseShoulderFor = -1;  // the target the shoulder was last picked for
	float CloseNextPickAt = -1e9f; // fight time a wall may ask for the next pick
	int32 CloseTargetNow = -1;    // the target followed this frame, its distance and height (the shot log)
	float CloseDistanceNow = -1.f;
	float CloseHeightNow = 0.f;
	float CloseRadiusNow = 0.f;   // m: its body's radius, scaled with its figure
	// Contact (ftcore::arenaCloseContactWant): a tall or wide target within about 3 m of its body swings the view round
	// the lead along its flank. The share as blended and as the range asks this frame; whether the shoulder was picked
	// at the swing since contact was last off; the swing added to the drawn view; the tallness the cut is raised by.
	float CloseContact = 0.f;
	float CloseContactWant = 0.f;
	bool bContactPicked = false;
	float ContactYawNow = 0.f;    // degrees (ftcore::arenaCloseContactYaw): 0 exactly off contact
	float CloseCutTall = 0.f;     // 0..1 (ftcore::arenaCloseCutTallness)
	float CloseContactLow = 0.f;  // 0..1 (ftcore::arenaCloseContactLow): the tall contact's low lens; exactly 0 under a 4 m target
	float StandOffPitchNow = 0.f; // degrees (ftcore::arenaCloseStandOffPitch): the least look-up that keeps the target's top in
	bool bStandOffWeak = false;   // and whether it kept the weak point instead of the top
	// Stand-off clearance (ftcore::arenaCloseStandOffClear): the extra side (signed) and pivot rise as eased; asked only
	// off contact (scaled down as contact blends in), 0 with nothing followed. The target's head is its top less this.
	float ClearSideNow = 0.f;
	float ClearPivotNow = 0.f;
	static constexpr float CloseHeadDrop = 0.15f; // m
	float CloseHeadHeight() const { return FMath::Max(0.3f, CloseHeightNow - CloseHeadDrop); }
	static constexpr float CloseMateHead = 1.6f;  // m: the partner's head and waist heights the close still log tests
	static constexpr float CloseMateWaist = 1.0f;
	// The partner's close station (ftcore::PartnerCloseView, rule 19): the walls near the lead from the boom's own probe,
	// CloseWallRays sweeps round it every CloseWallSeconds, each hit a segment 1 m across its surface. No allocation.
	static constexpr int32 CloseWallRays = 16;
	static constexpr float CloseWallSeconds = 0.25f;
	static constexpr float CloseWallReach = 10.f; // m
	ftcore::PartnerWall CloseWalls[CloseWallRays];
	int32 CloseWallCount = 0;
	float CloseWallAge = 1e9f; // s since the last sweep (large: sweep on the next step)
	// The sweeps' query, kept: its ignored fighters (lead, ally, up to six enemies) pass its inline four, so a fresh
	// one would allocate on every sweep; refilled in place, it allocates on the first only.
	FCollisionQueryParams CloseWallParams;
	void RefreshCloseWalls(float Dt);
	ftcore::PartnerCloseView CloseStationView(const ftcore::PartnerCameraView& Camera) const;
	ftcore::ArenaStickBasis StickBasis; // the stick's basis yaw (ftcore::arenaCloseStickBasis: held through a close swap)
#if !UE_BUILD_SHIPPING
	void LogCloseFrameShot(const TCHAR* Shot) const;
	int32 CloseSwaps = 0;         // shoulder picks that changed the side
	int32 CloseYawFrames = 0;     // frames held under close, with the partner's turn on, its sum and its most
	int32 CloseYawOn = 0;
	float CloseYawSum = 0.f;
	float CloseYawMost = 0.f;
	// Held close frames with the boom wall-shortened (under 98% of its arm), and of them the ones with the lead's spine
	// under 28% of the width (mirrored on the left shoulder).
	int32 CloseWallFrames = 0, CloseWallLow = 0;
	int32 CloseWallHeld = 0; // frames the close lens's pitch was held off the lead's rule's dive (ftcore::arenaCloseWallPitch)
	// Held close frames in contact, the most swing, and shoulder swaps made with the contact share over half.
	int32 CloseContactFrames = 0, CloseContactSwaps = 0;
	// Held close frames off contact with a target followed, those with its head clear of the lead's box in the drawn
	// lens (no margin), and the most clearance eased in (side, pivot).
	int32 CloseClearFrames = 0, CloseClearSeen = 0;
	float CloseClearSideMost = 0.f, CloseClearPivotMost = 0.f;
	float CloseContactYawMost = 0.f;
	// Held close frames in tall contact (low share over half): the lead's highest head and lowest waist share, the most
	// pitch; the partner's frames there and those in the frame (it is exempt in contact: logged only).
	int32 CloseTallFrames = 0, CloseTallMateFrames = 0, CloseTallMateIn = 0;
	float CloseTallHeadMost = 0.f, CloseTallWaistLeast = 1.f, CloseTallPitchMost = 0.f;
	float CloseArmSwitch = 0.f;   // -ArenaCameraArm=<m>: the close lens's arm for this run (0: the preset's)
	float PartnerYawCapSwitch = 0.f; // -ArenaPartnerYawCap=<deg>: a ranged partner's close yaw most for this run (0: the rules' 12)
	// Held close frames with a ranged partner (its kit's own range on): those with the turn over the melee cap, the most.
	int32 CloseRangedFrames = 0, CloseRangedWide = 0;
	float CloseRangedYawMost = 0.f;
	float StandOffAt = 0.f;       // -ArenaStandOff=<m>: the autopilot holds this far from the target (0: off)
	// -ArenaStandOffStrike=<s>: under -ArenaStandOff, a light string every <s> s (0: off); the next press's fight time
	// and the presses so far.
	float StandOffStrikeEvery = 0.f, StandOffStrikeNext = 1.f;
	int32 StandOffStrikePresses = 0;
	// Where the partner stood from the drawn lens on held frames (both presets): in the frame, behind the lens, off
	// the left or the right, over or under; and its mean place in the lens's ground frame while out of the frame.
	int32 MateFrames = 0, MateIn = 0, MateBehind = 0, MateLeft = 0, MateRight = 0, MateOffY = 0;
	float MateOutAhead = 0.f, MateOutRight = 0.f;
	// The partner's facing while a string runs. Steps: Core's facing off the line to its target by over 30 degrees and the
	// worst (degrees). Drawn frames, read after PlaceAlly: the drawn yaw off Core's by over 30 once the blow is out (its
	// active or recovery part: a fault) and in its startup (still turning in, logged only), and the worst out.
	int32 MateCastSteps = 0, MateCastCoreOff = 0;
	float MateCastCoreWorst = 0.f;
	int32 MateDrawFrames = 0, MateDrawOff = 0, MateDrawTurning = 0;
	float MateDrawWorst = 0.f;
	// -ArenaRollAt=<s,s,...>: capture-only dodge presses under the autopilot at these fight times (the dodge strip).
	static constexpr int32 MaxRollAt = 8;
	float RollAt[MaxRollAt] = {};
	int32 RollAtCount = 0, RollAtNext = 0;
#endif
	float LastLookInputAt = -1e9f; // fight time of the last look input (the stick or the mouse): the soft turn waits
	bool bLockAimAbove = false;   // the lock's aim point is above the frame this frame (GetLockAimAbove)
	float LockAimX = 0.5f;
	// The lens this frame solved (the arm as the boom's probe leaves it) and that camera as the brain reads it: the next step's feed.
	ftcore::ArenaCameraPose CameraLensNow;
	ftcore::PartnerCameraView FedCamera;
	bool bFedCamera = false;
	int32 LockTarget = -1;        // the tall target held to the lead's rule this frame; -1 none
	bool bLeadHeld = false;       // the last frame held the lead's rule (the next frame's sample measures it)
	// The lead's cylinder (Core's ReloadStarted, Reloaded and DryPress events): where the reload clip, the dry-press flinch
	// and their sounds attach once they exist. For now they only log.
	void OnLeadReload(bool bStarted, int32 Amount);
	void OnLeadDryPress(int32 Presses);
	// The cylinder event's sound (ftcore::arenaGunCue) through UFTAudioSubsystem.
	void PlayGunCue(const ftcore::ArenaEvent& E);
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
	// The partner's start under the close camera: 3 m ahead of the lead, 1.5 m to the side away from the shoulder.
	ftcore::Vec2 PartnerCloseStart(const ftcore::Vec2& Player, const ftcore::Vec2& Facing) const;
	void MoveAlly();
	void PlaceAlly(float Alpha);
	// -ArenaPartner: the brain set on the fight (BeginPartner), the figures' swap on a takeover, the walk to the ally's
	// goal, both leads' health for the session, and the 20 s report windows with the end's checks.
	void BeginPartner();
	void TakeOverBodies(const ftcore::ArenaOutput& Out);
	// How the camera goes to the new lead: a cut (a retry), an ease (a switch: kSwitchEaseSeconds), or a takeover's
	// hold on the fallen lead (kTakeoverHoldSeconds) then a move (kTakeoverMoveSeconds), its names and input with it.
	enum class EHandoff : uint8 { Cut, Switch, Takeover };
	static constexpr float kSwitchEaseSeconds = 0.35f;
	static constexpr float kTakeoverHoldSeconds = 0.3f;
	static constexpr float kTakeoverMoveSeconds = 0.5f;
	void SwapLeadFigures(EHandoff Handoff = EHandoff::Cut);
	void TickHandoff();
	// After a switch: the partner's first intents on the left body, its first move, and when it leaves the view.
	void TrackSwitchPartner();
	void LogHandoffChecks() const;
	// A takeover's hand-off still in its hold, dropped (the fight ended, another body change, a restart): counted and
	// logged, the names put with the figures.
	void DropPendingHandoff(const TCHAR* Why);
	// A partner blow's Hit: the lead's impact effect at kPartnerImpactScale at the partner's blow point, and the
	// impact sound hook (both sources call it).
	static constexpr float kPartnerImpactScale = 0.7f;
	void PartnerImpact(const ftcore::ArenaEvent& E);
	void ImpactSoundHook(uint8 Source, const FVector& At, float Scale);
	void WalkPartner(const ftcore::ArenaOutput& Out);
	void FeedLeadHealths();
	void TrackPartner(const ftcore::ArenaOutput& Out);
	void LogPartnerWindow();
	void LogPartnerChecks(bool bWon, bool bLost);
	void CheckPartnerRetry();
	void BringPartnerIn();
	void SwitchLeadBodies();
	void PutPartnerOut();
	// The partner's body acts: -ArenaPartner, not held out (a fight begun Lone keeps it out, the session with one lead
	// too, until the partner call brings it in), and the fight not Lone.
	bool PartnerBody() const { return bPartner && !bPartnerHeldOut && !Fight.lone(); }
	void ArmSwapCheck(bool bSwitch = false);
	void TrackSwapCheck(const ftcore::ArenaInput& In);
	void CheckSwapInput();
	void LogCarry() const;
	// Setting the table (-ArenaTable, a carrying run's): its switches, its reset (each attempt), its step after the
	// fight's, its ring drawn, its log line and its ring's removal.
	void BeginTable(const ftcore::PlaceTable& Places, bool bCapArg);
	void ResetTable();
	void TickTable(const ftcore::ArenaOutput& Out);
	bool BeginTableNow(double T);
	// The player's table key: Release held at full Load with enough bodies in reach (TableHold), on the step's input.
	bool TableCanBegin() const;   // TableCouldBegin and the setter free
	bool TableCouldBegin() const; // the table's own conditions: Ready, alive, full Load, places, bodies in reach
	bool TableSetterFree() const; // ftcore::tableSetterFree on the player's last action
	void StepTableKey(ftcore::ArenaInput& In);
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
	// A step back's ground speed (m/s), from its SteppedBack event: the walk clip plays backward at it.
	float StepBackSpeed[ftcore::kMaxArenaEnemies] = {};
	// Its way, from the event's move: 0 back, 1 a sidestep to the enemy's left, 2 to its right (no room behind it).
	uint8 StepBackWay[ftcore::kMaxArenaEnemies] = {};
	// The camera's horizontal half-angle last handed to the fight (ArenaFight::setCameraHalfAngle); -1 none yet.
	float FightCameraHalf = -1.f;
	// The half-angle the log last named and when (real time): a zoom's every frame is not a line.
	float LoggedCameraHalf = -1.f;
	double CameraHalfLoggedAt = -1e9;
	double CameraHalfChangedAt = -1e9; // when it last changed: a change the throttle kept back is logged once it holds 1 s
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
	uint8 LeadSeat = 0;
	uint8 AllySeat = 0;
	FString LeadName;
	FString AllyName;
	ftcore::ArenaBlowHook SessionInnerHook = nullptr; // the sub-hosts' hook the session's ruling chains after
	void* SessionInnerUser = nullptr;
	bool bSessionPartnerDue = false; // -ArenaSharedCount: a lead's blow is held, the scripted partner joins after the step
	int32 SessionHeldBlows = 0;      // this fight's lead blows judged Held
	int32 SessionJoins = 0;          // the scripted partner's blows that joined a window
	int32 SessionBonuses = 0;        // held-blow bonuses the fight took (hostBlow)
	int32 SessionBonusDamage = 0;    // their extra damage
	// -ArenaCountLift: the count stores Load and lifts, a releaseMark chair marks the Release's target, and a locked kite
	// seam opens on a shared blow.
	bool bCountLift = false;
	int32 CountLoadStored = 0;
	int32 CountLoadStores = 0;
	int32 Lifts = 0;
	int32 LiftSpent = 0;
	double LiftTime = -1e9;
	float LiftMultiplier = 1.f;
	double LastJoinAt = -1e9;   // the session's last join time seen
	int32 Marks = 0;
	int32 MaxJoinLoad = 0;       // the most Load two joiners held at a join
	int32 CappedBlows = 0; // blows the blow cap kept down this fight (-ArenaCountLift)
	// -ArenaLone, or a third slot that is not Companion (which forces it): the lead takes the Lone Carry (ftcore/LoneCarry.h).
	bool bLone = false;
	// The fight's third slot (ftcore::SlotState), the session's def.thirdSlot at begin: the story carry's (Companion with
	// no carry); -ArenaThirdSlot=Companion|Vacant|Hostile overrides it outside Shipping.
	uint8 ThirdSlot = 0;
	ftcore::LoneTuning LoneTuning{};
	bool bLoneAtStart = false;
	double LoneEndedAt = -1.0;          // the fight time a given gift ended it (-1: it did not)
	int32 LoneReleases = 0;             // Releases made while Lone (each pooled)
	int32 LoneGiftPresses = 0;          // gift presses while Lone, given or refused
	int32 LoneGiftsRefused = 0;         // of those, refused (still Lone)
	int32 LoneBreaches = 0;             // while Lone: a join, a table begun, or a Release not pooled (the check: 0)
	// The session's own fall rules, seen from a fight with no partner body: takeovers and rises it reported (the check:
	// none) and whether it called the defeat the fight's loss is.
	int32 SessionTakeovers = 0;
	int32 SessionRises = 0;
	bool bSessionDefeated = false;
	int32 LonePartnerCalls = 0;         // Count presses the session accepted while Lone: the lead calling the partner in
	int32 LonePartnerEnds = 0;          // endLone(PartnerCalled) calls that ended it (the check: one per call, at most one)
	bool bNoLoneCall = false;          // probe (-ArenaNoLoneCall): the autopilot never presses the Count while Lone
	float LoneGiftAt = -1.f;            // probe (-ArenaLoneGiftAt=S): one gift press at fight time S (-1: none)
	bool bLoneGiftDone = false;
	// The probe's self-check: the lead's health handed to the slot just before the probe's press (setLeadHealth), and
	// whether the press waits for the step's end. At full health the probe's Heal must come back refused NotHurt.
	float ProbeLeadHealth = -1.f;
	bool bProbeAwaiting = false;
	void LogProbeCheck(const ftcore::GiftResult& Result);
	// The stance this fight begins in: chosen, or forced by the third slot.
	bool LoneWanted() const { return bLone || ftcore::loneForced(static_cast<ftcore::SlotState>(ThirdSlot)); }
	// The stance the session will accept for this lead (asked before the fight begins, so a refused one changes
	// nothing: the chairs' heal charges stay).
	bool LoneAccepted(uint8 Lead) const { return ftcore::loneAtBegin(LoneWanted(), Session != nullptr, Lead, LoneTuning); }
	// The fight follows the session's truth (a given gift ends it).
	void MirrorLone();
	// While Lone a gift press waits until after the step (a same-tick Release pools first).
	ftcore::LoneGiftHold LoneGift;
	bool bLoneGiftApply = false; // the waiting press came from a gift key (applied), not the heal's (as before)
	void PressPendingLoneGift();
	// The save's pool of lone Releases (the runner's life: nothing persists it yet), the pool's tuning
	// (Data/Battle/lone_pool.json) and the fight time the Lone Carry began (the bargain's card; -1 none).
	ftcore::LoneSave LoneSaved{};
	ftcore::LonePoolTuning LonePool{};
	double LoneBargainAt = -1.0;
	// -ArenaHallucination=accept|refuse|expire (a probe; off by default): offers of the lead's tragedies by the Core
	// rules (ftcore/Hallucination.h). The hold slows the lead's stick and a full one stills the lead; the enemies go on.
	// 0 off, 1 accept (the partner pulls back), 2 refuse after a second, 3 let it expire, 4 accept, then refuse with
	// the guard a second into the hold and keep it down a second after
	int32 HallMode = 0;
	float HallAt = 5.f;            // no offer before this fight time (-ArenaHallucinationAt=S)
	ftcore::HallucinationTuning HallTuning{};
	ftcore::TragedyTable HallTable;
	ftcore::TragedyPool HallPool;
	ftcore::Hallucination Hall;
	uint32 HallRng = 7u;
	double HallShownAt = 0.0;
	int32 HallSeen[8] = {};        // events by ftcore::HallucinationEventType
	int32 HallHeldSteps = 0;       // steps the lead stood held
	int32 HallEnemyActs = 0;       // enemy attacks begun while the lead stood held
	int32 HallGuardHeld = 0;       // held steps the guard button was down (the refusal's, never the fight's)
	int32 HallGuardKept = 0;       // steps after a hold the guard from it stayed down (still the refusal's)
	int32 HallBlocksHeld = 0;      // blocks and parries the fight gave in those steps: 0, the guard never reaches it
	double HallHeldAt = -1.0;      // the Hallucination's clock at the last Held, and at the last Refused
	double HallRefusedAt = -1.0;
	bool bHallKeptStep = false;
	float HallHeldDrift = 0.f;     // metres the lead's body moved while held (knocks included)
	float HallMaxHold = 0.f;
	bool bHallHeldStep = false;
	ftcore::Vec2 HallHeldFrom{};
	// The refusal's weak point: the window's seconds left after the last step (a rise is a new opening), the blows the
	// lead took last step (the hold's push), the fight time a refusal last closed an offer too early, and the lead's
	// Brief (BeginBrief's; Count for none).
	float HallOpenLeft = 0.f;
	int32 HallBlowsTaken = 0;
	double HallTooEarlyAt = -1.0;
	bool bHallNoWeakPointLogged = false;
	ftcore::BriefId LeadBrief = ftcore::BriefId::Count;
	void HallWeakPoint();
	void BeginHallucination();
	void StepHallucination(ftcore::ArenaInput& In);
	void AfterHallucinationStep(const ftcore::ArenaOutput& Out);
	int32 PartnerLoadStored = 0; // the partner's kept gauge: stored this fight, and the last change
	double PartnerLoadTime = -1e9;
	int32 PartnerLoad = -1;      // -ArenaPartnerLoad=N: the scripted partner's fixed Load (a probe; -1: no gauge)
	double KiteLockUntil = -1e9; // the kite's lock end last opened
	int32 KiteUnlocks = 0;
	// Feeds the lead's Load to the session (the scripted partner has no gauge). In a partner run (bPerLead) it feeds
	// FeedLeadValues instead.
	void FeedSessionLoads();
	// Per-lead Load and heal charges (a partner run, -ArenaPartner): each lead owns both, and the fight's one gauge and
	// one charge count are FedLead's, the lead the last of the session's leadLoadNow / leadHealsNow gave them (the
	// controlled lead, but for the step a takeover or the begin's own switch moves control: FedLead moves only when the
	// session reports the new lead's values). FeedLeadValues feeds FedLead's every step (before blows are judged and
	// before the session's tick), never the kept lead's; a probe's -ArenaPartnerLoad is set once, at the begin.
	// ApplyLeadValues puts the reported values in the fight right after the tick, before its Load changes and the
	// fight's next step. One-lead fights never take this path (the session never reports them): as before.
	void FeedLeadValues();
	void ApplyLeadValues(const ftcore::FightTick& Tick, int32 FedLoad, int32 FedHeals);
	// FightSession's host calls per step: OpenLeadStep feeds FedLead's gauge and charges as the step opens (1), every
	// spend between that and the session's tick goes through spendLeadLoad / spendLeadHeal (3: SpendLeadLoadNow, the
	// gift's and the table's; ReportFightSpends, the fight's own Release and heal), and no set of that lead follows
	// them on the step (the blow hook feeds nothing while the step is open). A spend outside the window (after the
	// tick, before the next open) is in the next open's feed. The session's lateSets() must stay 0.
	void OpenLeadStep();
	void SpendLeadLoadNow(int32 Amount);
	void ReportFightSpends(const ftcore::ArenaOutput& Out);
	bool bPerLead = false;
	uint8 FedLead = 0;
	bool bLeadStepOpen = false;
	int32 StepOpenLoad = 0, StepOpenHeals = 0, StepLoadSpent = 0, StepHealsSpent = 0;
	int32 LeadLoadSpends = 0, LeadHealSpends = 0, LateSetsSeen = 0;
	int32 LateSetsRun = 0; // the most late sets any fight of the run saw: no begin resets it (a retry keeps it)
	// The "per-lead Load" check: the runner's own ledger of the lead without control (its values as it lost control,
	// or the begin's for the partner, plus the kept Load changes the session reported for it since; -1: none), checked
	// against the values the session reports when control comes back to it. bOwnDeparted: the ledger is from that
	// lead's own departure (a switch back), not the begin's.
	int32 OwnLoad[2] = {-1, -1};
	int32 OwnHeals[2] = {-1, -1};
	bool bOwnDeparted[2] = {false, false};
	int32 LeadMoves = 0, LeadMovesChecked = 0, LeadReturnsChecked = 0, LeadValueMisses = 0;
	// After a session tick: applies its Load changes, notes a Lift and opens the kite's seam on an unlock.
	void ApplyCountLift(const ftcore::FightTick& Tick);
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
	double JumpStartedAt = -1.0; // fight time the drawn jump took off (logs its end and the first free frame); -1: none
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
	bool bPressJump = false; // only with the jump bound (JumpBound)
	bool bPressHeal = false;
	bool bPressLock = false;
	bool bPressRelease = false;
	// The Release key down now, either device (read only under -ArenaTable, for the table's hold).
	bool bReleaseDown = false;
	bool bPressPair = false;
	bool bPressTakeHalf = false;
	// The gift the last gift key pressed (Gift Heal, Gift Guard; None: none), until the step's PressSession reads it.
	ftcore::Gift GiftPick = ftcore::Gift::None;
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
	bool bReticleTakes = false;      // a round fired now would take the crosshair's body (this frame's, not held as blocked is)
	// The blocked crosshair's display hold: another body blocks by the round's own rule (Core's kArenaShotGraze), and
	// the shown state changes once the new one has held kBlockHoldFrames.
	static constexpr int32 kBlockHoldFrames = 4;
	int32 BlockedRun = 0;  // frames the unshown state has held
	int32 BlockedFor = -1; // the crosshair's body that state is about (a new one starts clear)
	// The first standing enemy body (Core's radius, the figure's height and hover, as an upright cylinder) along the
	// line From -> To (cm) nearer than T on entry (0..1 of the line): its slot, and T then the share along the line. The
	// figures carry no collision, so the reticle's trace meets them by this.
	int32 ReticleBody(const FVector& From, const FVector& To, double& T) const;
	// The crosshair's pick along From -> To (cm): the world's first hit on the visibility channel (the lead's figure not
	// counted) clips the line, and the body ReticleBody meets before it is picked (-1: none, as for a body behind cover).
	// T: the share along the line of the pick, else of the world hit, else 1.
	int32 PickReticle(const FVector& From, const FVector& To, FHitResult& Hit, bool& bWorldHit, double& T) const;
	// Aim assist's condition: aimed (not blending out), not locked on, a guns kit, and the crosshair on a standing body a
	// round fired now would take (bReticleTakes).
	bool AssistOverBody() const;
#if !UE_BUILD_SHIPPING
	// -ArenaAimCheck (autopilot, a guns kit): once in the fight, the camera aims at the tallest standing enemy's top
	// (its height and hover) by ftcore::arenaLookUpPitch, the reticle is traced and one aimed shot is pressed; the
	// check line says whether the view reached it and the shot connected. 0 off, 1 waiting, 2 aiming, 3 fired, 4 done,
	// 5 the -ArenaAssistCheck sweep.
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
	// -ArenaAssistCheck (with -ArenaAimCheck): after the aimed shot (phase 5), four steps, each capped at 6 s: 0 a
	// half-way right stick sweeps the view off the body and back through TurnLook with aim assist on (a lock from before
	// is let go: the view does not turn while one is on); 1 a lock is pressed on and let go; 2 aim is let go and held
	// again; 3 the target is killed by a host blow. The view's pitch, and from step 1 its yaw, are kept on the body's
	// middle. Each change of the stick's share is logged with its cause; the check line compares the turn rate over a
	// body and off it and says whether the share went back to 1 on each of the four.
	bool bAssistCheck = false;
	int32 AssistStep = 0;
	int32 AssistSub = 0;           // the step's own progress
	double AssistStepAt = 0.0;     // when the step began
	double AssistMarkAt = -1e9;    // when the step last acted (a lock press, the aim let go)
	double AssistSince = 0.0;      // when the share last changed
	float AssistSweep = 1.f;
	float AssistKeep = 1.f;        // the share as last logged
	bool bAssistAimOff = false;    // step 2: the aim let go
	bool bAssistLockPress = false; // StepAimCheck presses lock on its next input
	uint8 AssistReturns = 0;       // the share back at 1, bits: 1 the crosshair left, 2 lock-on, 4 aim let go, 8 the target died
	FString AssistFail;            // the step that ran out of time
	int32 AssistEnters = 0;
	int32 AssistExits = 0;
	float AssistTime[2] = {0.f, 0.f};   // seconds swept off a body [0] and over one [1]
	float AssistTurned[2] = {0.f, 0.f}; // degrees swept off a body [0] and over one [1]
	void StepAssistCheck(float DeltaSeconds);
	// -ArenaCoverCheck: once, a second into the fight, a synthetic crosshair line (not the camera's) from 0.5 m over the
	// floor through the cover drum nearest a standing enemy to its body's middle, picked by PickReticle as the reticle is, and the same
	// line with the world left out as the control. Logs "Arena check: reticle behind cover" and, with
	// -ArenaQuitOnFinish, ends the run.
	bool bCoverCheck = false;
	void StepCoverCheck();
#endif
	// The unlocked look: the mouse and the right stick turn the view, by aim assist's share (ftcore::aimAssistTurn) while
	// AssistOverBody (last frame's readout), by ftcore::arenaLookStep. Returns the stick's share.
	float TurnLook(float MouseX, float MouseY, float LookX, float LookY, float DeltaSeconds, bool bAssist);
	// The tallest standing enemy's slot (-1: none) and its top over the floor (m).
	int32 TallestStanding(float& OutTop) const;
	// The aim's heading on the ground from the lead toward the reticle (Fallback when it lies on the lead).
	ftcore::Vec2 ReticleAimDir(const ftcore::Vec2& Fallback) const;
	// The body under the crosshair as the fight's ArenaInput::aimBody (kArenaNobody: none).
	uint8 ReticleAimBody() const;
	// The round's climb (rad) from the lead's muzzle (ftcore::kArenaShotHeight over its floor) to the reticle's heading
	// point; false (and 0) with no reticle traced.
	bool ReticlePitch(float& OutPitch) const;
	// The same toward the middle of enemy Slot's body (its hover and height); false for no such standing enemy.
	bool BodyPitch(int32 Slot, float& OutPitch) const;
	// The rendered view's rotation without the impact jolt (the camera's own, as the reticle and the tells read it).
	FRotator ViewRotation(const APlayerCameraManager& Camera) const;
#if !UE_BUILD_SHIPPING
	// -ArenaAimCheck's input for one step under the autopilot (replaces In while it aims and fires).
	void StepAimCheck(ftcore::ArenaInput& In);
#endif
	// The one send path of a switch to the partner (the party screen's pick, Tab, the -ArenaSwitchAt probe): sent to the
	// session whenever the partner has a body in this fight (PartnerBody); the session's tick.switched then swaps the
	// bodies, the figures, the seats and the camera (SwitchLeadBodies). With no body nothing is sent and the HUD's call
	// reads SWITCH: NOT HERE; the session refuses the rest at the step's close, and a switch never ends a Lone Carry.
	// From names the press for the log. One ask per step: a second press in the same step (Tab and the screen's
	// release in one frame) sends and counts nothing, since the session resolves one request at the close. True when
	// the session took the request (the step's first ask).
	bool AskLeadSwitch(const TCHAR* From);
	// The party screen's step: In with the partner's row's presence (GetLeadSwitchReason, the row's own word), the
	// -ArenaSwitchAt probe's drive where it runs (ProbePartyInput), and a pick of the partner's row sent by AskLeadSwitch.
	void StepPartyScreen(ftcore::UiPartyInput In, float DeltaSeconds);
	// -ArenaSwitchAt=<S> with the party screen (dev runs; SwitchAt stays -1 in Shipping): the probe asks its switch as
	// the player would, on the screen (ftcore::uiPartyProbeInput): keys up from S-1.2 s, Party held from S-1 s, the
	// cursor flicked up to the partner at S-0.6 s, let go at S (the pick); Party held again from S+0.4 s to S+1.6 s, so
	// the screen stands open on the new lead (its dot, the rows traded). False outside that window: the player's own
	// input (or none, under the autopilot).
	bool ProbePartyInput(ftcore::UiPartyInput& In) const;
	ftcore::Vec2 Stick;
	bool bStickFlicked = false;
	bool bMouseFlicked = false;
	// The party screen's state (bPartyScreenOn: FT::NewUiEnabled, read at Begin), and the last refused switch (see
	// GetSwitchRefusedSerial).
	bool bPartyScreenOn = false;
	ftcore::UiPartyScreen PartyScreen;
	uint32 SwitchRefusedSerial = 0;
	ftcore::UiPartyWhy SwitchRefusedWhy = ftcore::UiPartyWhy::Unknown;
	uint32 PromptPresses[PromptPressCount] = {};
	bool bGuardWasDown = false; // the Guard key down last poll (its press is the key going down, whatever the toggle)

	double Now = 0.0;
	double TellSince[ftcore::kMaxArenaEnemies] = {}; // when each enemy's break tell rose (DriveSkin's body pulse)
	bool bTellShown[ftcore::kMaxArenaEnemies] = {};
	double Accumulator = 0.0;
	double DrawnTime = 0.0;
	double ActiveSince[ftcore::kMaxArenaEnemies];
	double EnemyHitTime[ftcore::kMaxArenaEnemies];
	uint8 LastPart[ftcore::kMaxArenaEnemies] = {};
	double PlayerHitTime = -1e9;
	FVector2D LastHitFrom = FVector2D::ZeroVector;
	double LastHitAt = -1e9;
	double ParryTime = -1e9;
	double ReleaseTime = -1e9;
	int32 ReleaseSpent = 0;
	double OverloadTime = -1e9;
	FPopup Popups[MaxPopups];
	int32 PopupTotal = 0;

	float CameraYaw = 0.f;
	float CameraPitch = -16.f;
	// The lock's framing: an orbit turn (degrees, ftcore::arenaFrameTurnAt) on top of CameraYaw, which stays on the lock's
	// line. It goes into CameraYaw once nothing is followed (ftcore::arenaFrameYawStep), so the view never jumps.
	float FrameTurn = 0.f;
	// The yaw the view is drawn at: what the stick, the crosshair and the fight's view read (the partner's soft turn
	// included, as the drawn camera and the control rotation have it).
	float ViewYaw() const { return ftcore::arenaFrameViewYaw(ftcore::ArenaFrameYaw{CameraYaw, FrameTurn}) + PartnerYawNow + ContactYawNow; }
	// The lock framing's rules in this zone: the global ones, held to the zone's own turn cap.
	ftcore::ArenaFrameRules FrameRules() const
	{
		ftcore::ArenaFrameRules Rules = ftcore::kArenaFrame;
		Rules.maxTurn = Fit.camera.frameTurnCapDeg;
		return Rules;
	}
#if !UE_BUILD_SHIPPING
	// -ArenaFrameCheck: the framing as the rendered camera shows it, sampled on each followed frame and logged by
	// LogFeatureChecks ("Arena check: framing"). -ArenaNoFrame holds the framing at 0, for a side-by-side.
	bool bFrameCheck = false;
	bool bNoFrame = false;
	int32 FrameFrames = 0;
	int32 FrameSettled = 0;
	int32 FrameInThird = 0;
	float FrameLeadWorst = 0.f;
	float FrameFoeWorst = 0.f;
	float FrameTurnMost = 0.f;
	float FrameRateMost = 0.f;
	float FrameLensMost = 0.f;
	float FrameArmMost = 0.f;
	float FramePrev = 0.f;
	int32 FrameBandN[4] = {};
	float FrameBandFoe[4] = {};
	float FrameBandGap[4] = {};
	// The close camera's own bar on the same settled frames (ftcore::arenaCloseFrameCheck): all three terms, and each.
	int32 CloseFrameOk = 0, CloseFrameLead = 0, CloseFrameFoe = 0, CloseFrameMate = 0;
	// Its misses split by what the frame had, for the target ([0]) and the lead ([1]): the boom wall-shortened (under 98%),
	// the lens lagging (over 0.15 m further from the pivot than the rig puts it), the framing turn at its cap, the
	// partner's turn on (over 1 degree), the lead moving over 3 m/s, the target under 3 m. And the target's misses below
	// and above its band, their mean share, and those with its aim point under 1 m in front of the lens.
	int32 CloseMissWall[2] = {}, CloseMissLag[2] = {}, CloseMissCap[2] = {}, CloseMissMateTurn[2] = {}, CloseMissFast[2] = {}, CloseMissNear[2] = {};
	int32 CloseMissFoeLow = 0, CloseMissFoeHigh = 0, CloseMissFoeAtLens = 0;
	float CloseMissFoeSum = 0.f;
	int32 FrameShotSkipped = 0; // frames of a capture's view (or a lens past the arm and a metre) left out of the frame check
	void SampleFrame(int32 Target, float Want, float DeltaSeconds);
	// "Arena check: lead in frame": each frame after one held to the lead's rule on a tall target, the rendered
	// camera's view of the lead (its floor point and its head, its radius each side) and the target's telegraph.
	void SampleLeadFrame();
	void ResetLeadFrameChecks();
	int32 LeadFrameFrames = 0, LeadFrameKept = 0, LeadFrameVisible = 0;
	ftcore::ArenaLeadShares LeadShotShares; // the last frame sampled on a tall target (bLeadShotSampled), for LogLeadFrameShot
	bool bLeadShotSampled = false;
	float LeadFeetLeast = 1.f, LeadHeadMost = 0.f, LeadSideWorst = 0.f;
	// The three samples that missed the rule most (how far past it, a share of the frame; 1 + for one behind the
	// camera) and their fight times, worst first.
	float LeadMissWorst[3] = {0.f, 0.f, 0.f};
	float LeadMissAt[3] = {-1.f, -1.f, -1.f};
	int32 TellFrames = 0, TellInPlace = 0, TellPinned = 0, TellEdge = 0;
#endif

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
	double GlideJumpLast = -1e9; // -ArenaGlide=jump...: the last time the lead was on the jump's clock
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
	bool bLeadRefused = false; // IsRefused
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
	// The ally's drawn yaw (degrees), turned toward the wanted yaw at kArenaAllyTurnRate at most, and the clock it was
	// last drawn at (below 0: not drawn yet this session, so the first placement takes the wanted yaw at once).
	float AllyDrawYaw = 0.f;
	double AllyDrawYawAt = -1.0;
	int32 CarrySharedOffers = 0;
	// -ArenaPartner (outside Shipping; off by default, so ordinary play never has it): the other lead stands in the ring
	// as the partner, its body driven by Core's PartnerBrain through the fight (ArenaFight::setPartner) with the tuning
	// of Data/Battle/partner_brain.json. Unless the fight is Lone, both leads are then in the session's fight and both
	// leads' health is fed every step before any press; the player's fall with the partner up is a takeover (bodies,
	// figures, looks and the session lead swap), both down the defeat. B toggles the order: STAY CLOSE and SPREAD.
	bool bPartner = false;
	bool bPartnerSpread = true;          // the order: Spread (ArenaInput::partnerOrder 1) or StayClose (0)
	bool bFiguresSwapped = false;        // PlayerFigure and AllyFigure have swapped an odd number of times this fight
	uint8 StartLead = 0;                 // the session lead the fight began with (a restart goes back to it)
	const FFTLeadLook* PlayerLook = nullptr; // the look PlayerFigure wears (swaps with AllyLook on a takeover)
	// The ready stances (Data/Anim/ready_stance.json, read at the first ask) and each figure's (0 the player's, 1 the
	// ally's), by the look it wears: the look's name maps to a stance, looked up again only when the look changes.
	ftcore::ReadyStanceSet ReadyStances;
	bool bReadyStancesRead = false;
	const FFTLeadLook* ReadyLookOf[2] = {nullptr, nullptr};
	const ftcore::ReadyStance* ReadyOf[2] = {nullptr, nullptr};
	const ftcore::ReadyStance* ReadyStanceFor(int32 Figure, const FFTLeadLook* Look);
	ftcore::PartnerBrain AllyBrain;
	ftcore::PartnerTuning AllyTuning;
	// The report windows (TrackPartner): attack seconds this window, the totals at its start, its counts.
	double PartnerPrevTime = 0.0;
	double PartnerWinStart = 0.0;
	double PartnerWinLead = 0.0;
	double PartnerWinAlly = 0.0;
	int32 PartnerWinDamage0 = 0, PartnerWinLeadDamage0 = 0, PartnerWinBoss0 = 0, PartnerWinAimed0 = 0;
	int32 PartnerWinLeadFalls = 0, PartnerWinAllyFalls = 0, PartnerWinTakeovers = 0, PartnerWinRises = 0;
	int32 PartnerWindows = 0, PartnerWindowsHeld = 0;
	double PartnerWorstOver = -1e9;      // the most a window's partner attack time stood over (or under: < 0) its limit
	// The fight's counts: falls of the player's body and of the partner's (a takeover's AllyDowned is the same fall),
	// takeovers the host swapped, rises, the session's takeovers, steps where the session's controlled lead was not the
	// runner's, and the order's presses.
	int32 PartnerLeadFalls = 0, PartnerAllyFalls = 0, PartnerTakeovers = 0, PartnerRises = 0;
	int32 PartnerSessionTookOver = 0, PartnerLeadMismatch = 0, PartnerOrders = 0;
	// A fight begun Lone with -ArenaPartner (bPartnerLoneStart): the partner is out (bPartnerHeldOut: no setPartner, no
	// ally body, one lead in the session) until the lead's partner call ends the carry; then the session's joinFight
	// brings it in at the step's close and the fight takes it on tick.joined (BringPartnerIn). A Release (or any other)
	// end keeps it out to the fight's end; a retry is Lone again, so it is out again (PutPartnerOut). Steps where
	// Core's partner acted while out (a leak); joins asked, landed and refused; steps where the session's word on the
	// partner (inFight(OtherLead())) and the fight's (Out.ally.partnerOn) disagreed.
	bool bPartnerLoneStart = false;
	bool bPartnerHeldOut = false;
	int32 PartnerHeldLeaks = 0;
	int32 PartnerJoinAsks = 0, PartnerJoinsLanded = 0, PartnerJoinsRefused = 0, PartnerJoinDisagree = 0;
	// Stationing: steps the partner was on (and fed the camera), and of them the steps stationing ran.
	int32 PartnerOnSteps = 0, PartnerStationedSteps = 0, PartnerCameraSteps = 0;
	// "Arena check: partner in play": the steps the partner (up, in the fight) stood inside the play camera's frustum
	// (ftcore::partnerInFrame on ArenaInput::partnerCamera), and its distinct actions (a new intent other than Wait, or a new string).
	// PartnerClearSteps: of the steps on screen, those it was not hidden behind the locked target from the lens
	// (ftcore::arenaHiddenBehind on both its middle and its head).
	// PartnerLeadClearSteps: of the steps on screen, those it was not hidden behind the lead (the same test on the lead's body).
	int32 PartnerSeenSteps = 0, PartnerFrustumSteps = 0, PartnerActions = 0, PartnerClearSteps = 0, PartnerLeadClearSteps = 0;
	// The same two on the floor plane, as the close station's rule measures them (ftcore::partnerCloseHiddenBehind on the
	// locked target's radius; ftcore::partnerCloseOverlaps with the lead, 0.4 m each), printed beside the two above.
	int32 PartnerClearFlatSteps = 0, PartnerLeadClearFlatSteps = 0;
	// Under the close camera: the steps measured in its frame, and those the partner stood where the near field's rule 1
	// allows (ftcore::partnerCloseNearOk on the fed camera's ground place and forward).
	int32 PartnerNearSteps = 0, PartnerNearOkSteps = 0;
	float PartnerPlaySeconds = 0.f;
	// The partner standing still (not swinging or guarding, not moving): this run and the fight's longest (s).
	float PartnerStillFor = 0.f, PartnerStillLongest = 0.f;
	ftcore::Vec2 PartnerStillAt{};
	bool bPartnerStillAt = false;
	// The act in progress for that count: from when, to the last step seen, where the partner stood at its start, and
	// whether it was carried out (FlushPartnerAction counts it).
	float PartnerActFrom = -1.f, PartnerActTo = -1.f;
	// Engaged steps (the lead's last attack within kPartnerEngagedWindow s): their seconds, the counted actions that ran
	// on one, the lead's last attacking fight time, and whether the act in progress ran on one.
	float PartnerEngagedSeconds = 0.f, LeadAttackAt = -1e9f;
	int32 PartnerEngagedActions = 0;
	bool PartnerActEngaged = false;
	ftcore::Vec2 PartnerActAt{};
	bool PartnerActDone = false;
	void FlushPartnerAction();
	float PartnerHeight() const;

	uint8 PartnerLastAct = 0xFF, PartnerIdleWhy = 0xFF;
	double PartnerJoinedAt = -1.0;
	// The lead switch with both up (the party menu's setActiveLead; -ArenaSwitchAt=<s> asks one at that fight time in a
	// dev run): the session's tick.switched to the other lead swaps the bodies (Fight.switchLeads), the figures and
	// the camera. Asked by the probe, switched by the session, swapped by the fight, refused by the fight.
	// Every host request the session took counts in SwitchAsks (AskLeadSwitch); one still open at a step's close and
	// refused there (cooldown, a body down) counts in SwitchAsksRefused, a player's answer, not a mismatch; one the close
	// resolved as Same (control already on that lead: a takeover the same step) counts in SwitchAsksSame.
	float SwitchAt = -1.f;
	bool bSwitchAsked = false;
	// -ArenaSwitchBackAt=<s> (dev runs, after -ArenaSwitchAt): one more ask, back to the first lead, at that fight time
	// (the same send path, AskLeadSwitch), so the per-lead Load check sees a switch and a switch back.
	float SwitchBackAt = -1.f;
	bool bSwitchBackAsked = false;
	// The hand-off (SwapLeadFigures): the HUD's names follow the figures (a takeover's at its move's start); the
	// takeover's move is due at HandoffMoveAt (Now), its input ignored to TakeoverInputUntil (fight time).
	bool bNamesSwapped = false;
	// All three on the fight's clock (Fight.time()). HandoffBegun: takeovers whose hold began; HandoffTakeovers: of them
	// the moves that started; HandoffDropped: the rest (DropPendingHandoff).
	double HandoffMoveAt = -1.0, HandoffFellAt = -1.0, TakeoverInputUntil = -1.0;
	int32 HandoffTakeovers = 0, HandoffMovesOnTime = 0, HandoffSwitches = 0, TakeoverInputsIgnored = 0;
	int32 HandoffBegun = 0, HandoffDropped = 0;
	// The play camera's pose as last drawn (SteerCamera): the partner's stationing reads its lens from it, and a switch
	// hands its rig to the new lead. False until the first frame draws one.
	ftcore::ArenaCameraPose CameraPoseNow{};
	bool bCameraPoseNow = false;
	// After a switch's ease: the pose's lens against the camera manager's, on the ground (cm, measured once per switch).
	double SwitchCamCheckAt = -1.0, SwitchCamWorst = 0.0;
	int32 SwitchCamChecks = 0;
	// The benched figure's gauntlet morphs after each body change (all back to 0: no tucked cuff on the body left).
	// BenchMorphs: the gauntlet look's morphs, BenchMorphOwner the figure that wears that look.
	TArray<FName> BenchMorphs;
	TWeakObjectPtr<AFTArenaFighter> BenchMorphOwner;
	int32 BenchMorphChecks = 0, BenchMorphUnchecked = 0;
	float BenchMorphWorst = 0.f;
	// The fight's partnerSwaps when this session began (a restart begins the brain again, not the fight's stats).
	int32 PartnerSwapsBase = 0;
	double HandoffWorstLate = 0.0;
	float HandoffFrameDt = 0.f;
	// After a switch: the fight time it happened, the left body's place then, the first step its brain moved it (or
	// attacked), the first frame it stood out of the camera's view; -1: not yet. The intents logged each 0.25 s to 2 s.
	double SwitchPartnerAt = -1.0, SwitchPartnerMoved = -1.0, SwitchPartnerOut = -1.0;
	int32 SwitchPartnerLogs = 0;
	FVector SwitchPartnerFrom = FVector::ZeroVector;
	double SwitchWorstMoved = 0.0, SwitchWorstOut = 0.0;
	int32 SwitchPartnerChecks = 0;
	TWeakObjectPtr<class UFTVfxComponent> PartnerImpactFx;
	double PartnerImpactPlayedAt = -1.0; // the held time it last played (a capture view's freeze reads its age)
	bool bPartnerImpactFrozen = false;
	bool bImpactHookLogged[2] = {false, false};
	bool bSwitchAskOpen = false;
	bool bSwitchAskedThisStep = false; // AskLeadSwitch's one ask per step (cleared at the step's close, as bSwitchAskOpen)
	int32 SwitchAsks = 0, SwitchesSeen = 0, SwitchesDone = 0, SwitchesRefused = 0, SwitchAsksRefused = 0, SwitchAsksSame = 0;
	// The partner's blows: the session ruled them (RuleSessionBlow's partner path), their Hit events, their joins.
	int32 PartnerBlowsRuled = 0, PartnerHitEvents = 0, PartnerJoins = 0;
	// After a takeover or a lead switch: the figures' places once drawn, the steps since with a move input (and the
	// sum of those moves), the check's counts. The controller reads the input (FTInputBindings on the
	// PlayerController), the fight moves Bodies.player, and PlayerFigure (swapped to the partner's actor) is drawn
	// there: the move must show on the new body. After a takeover the old body lies still; after a switch the partner
	// brain walks it, so there the new body's way must follow the summed input instead (SwapWorstAlign, the cosine).
	bool bSwapCheckArmed = false, bSwapCheckPlaced = false, bSwapIsSwitch = false;
	FVector SwapNewAt = FVector::ZeroVector, SwapOldAt = FVector::ZeroVector;
	FVector2D SwapMoveSum = FVector2D::ZeroVector;
	int32 SwapMoveSteps = 0, SwapSteps = 0, SwapChecks = 0, SwapChecksPassed = 0, SwapSwitchChecks = 0;
	double SwapWorstNew = 1e9, SwapWorstOld = 0.0, SwapWorstAlign = 1.0;
	// Setting the table (BeginTable): on, the autopilot sets it at full Load, the Load filled for a capture; the
	// places it counts, its clock and ring, and the counts for the TABLE line.
	bool bTable = false;
	bool bTableAuto = false;
	bool bTableFill = false;
	// -ArenaTableKey: the autopilot works the player's key (StepTableKey): odd presses held to a begin, even ones tapped.
	bool bTableKey = false;
	// -ArenaTableOpen: the autopilot stands still and open while the table is set (a review switch for the share).
	bool bTableOpen = false;
	int32 TableKeyPresses = 0;
	ftcore::PlaceTable TablePlaces;
	ftcore::TableSet TableClock;
	ftcore::TableRing TableSeats;
	ftcore::TableKeyPath TableKey;
	int32 TableKeyBegins = 0;
	double TableSetSeconds = 0.0; // fight seconds a table stood Set (the uptime on the TABLE line)
	double TableLastTick = -1.0;
	int32 TableKeyTaps = 0;
	int32 TableKeyCancels = 0;
	int32 TableKeyDrops = 0; // holds dropped stale or while the setter was busy (logged)
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
	// The boss kinds whose break fallback error was logged (PrepareStoneBearer): once a kind a play session.
	TSet<FName> BreakErrorLogged;
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
	// The partner's body's weapon (-ArenaPartner): the same strike clips, props and effects its look plays when it is
	// the lead, driven from the ally's blows (TrackAllyWeapons); begun on the ally figure, ended on a swap.
	FFTLeadWeapons AllyWeapons;
public:
	const FFTLeadWeapons& GetAllyWeapons() const { return AllyWeapons; }
	FFTLeadWeapons& GetAllyWeapons() { return AllyWeapons; } // a capture view's freeze (-ArenaShotAllyStrikes)
	// A capture view's freeze of the partner's impact (-ArenaShotAllyStrikes; never in a fight's own path): one still
	// running is run again to its age at T and paused, as FFTLeadWeapons::Freeze; one line with its age. Thaw lets it go.
	void FreezePartnerImpact(double T);
	void ThawPartnerImpact();
	// Where the partner's impact plays, while it is frozen and running.
	bool GetPartnerImpactAt(FVector& OutAt) const;
private:
	void TrackAllyWeapons(const ftcore::ArenaOutput& Out);
	bool bAllyStruck = false;  // the ally was in a blow last step ...
	uint8 AllyStruckBlow = 0;  // ... this one ...
	float AllyStruckTime = 0.f; // ... this far in
	bool bAllyStrikeCut = false; // its blow's clip was cut (it was hit, or its target is down) until its next blow
	int32 AllyTargetDeadLast = -1; // the blow's target when it was already dead the step before (the killing blow is not cut)
	TWeakObjectPtr<AFTArenaFighter> AllyWeaponsFailed; // the ally figure its driver could not begin on: not tried again
	// A partner shot's or cast's impact waits for what is left of its flight after the hit (FFTLeadWeapons::FlightOf).
	ftcore::ArenaEvent PartnerImpactWaiting{};
	double PartnerImpactDue = -1.0;
	void PlayWaitingPartnerImpact(double T);
	// The partner's strike clips over the fight: started, and played into their recovery uncut (whole); summed across swaps.
	int32 PartnerClipsStarted = 0, PartnerClipsWhole = 0;
	// The direction from the lead to its aim target, set each step with the aim; the figure turns to it while
	// it attacks.
	ftcore::Vec2 LeadAimDir{1.f, 0.f};
	bool bLeadAimDir = false;
};
