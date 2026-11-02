#pragma once

// Game mode of the arena demo map, Demo_Arena (docs/ARENA_DEMO.md): the real-time melee of
// ftcore::ArenaFight (Core/include/ftcore/ArenaFight.h, docs/COMBAT_SYSTEMS.md "Arena melee") with this mode
// as its host and AFTArenaHUD as its UI. No story on play; the map is built by FTBuildArenaWorld.
//
// The fight is Data/Battle/arena.json's default fighter against the enemies of the zone it is held in
// (Data/Battle/arenas.json; by default the sand ring, a plain 12 m circle of flat ground, which hosts the
// three-kind wave: brute, skirmisher, breaker, one attacking at a time). -ArenaStage=<id> picks another zone;
// most are a portion of one of the worlds' own scenes (a plaza, a street between buildings): the mode opens that
// world's map (World_Solhaven, World_Lattice, World_Unwritten) with itself as the game mode and holds the player and
// the enemies inside the zone's invisible walls, with the real buildings, trees and landmark there as obstacles.
// -ArenaLineup=<id>,<id>,... puts enemies of those kinds in it instead (-ArenaLineup=brute: one brute),
// -ArenaAttackers=<n> sets how many attack at once (1 by default), and -ArenaSeed=<n> picks the seed (1). The
// zone's size, spawn distance and camera scale with the enemies (ftcore::fitArenaZone). The built-in
// ftcore::defaultArenaDef() stands in when arena.json is missing or refused.
//
// Nothing is built for a zone in a world: the world's own art and light stay. Only the plain zone gets a flat
// ground and the shared look (FTScene::SpawnLook), so the demo map (Demo_Arena, a player start and nothing else)
// still plays with no flag. The walls are Core's: ftcore::arenaStep(fight, bodies, dt, zone) holds every body inside
// the zone and out of its obstacles; Unreal blocks nothing. The camera may leave the walls but not pass through
// the scene: its spring arm probes the world and is pulled in.
//
// Core judges, the host moves. The mode steps the fight at a fixed 1/60 s (an accumulator, at most 8 steps
// a frame), the step the Core tests and the balance check use, and moves the bodies with ftcore::arenaStep:
// the ring has nothing inside it to collide with, and Core keeps every body inside it. The figures
// (AFTArenaFighter: the player's pawn, which the player controller possesses, and one per enemy) are placed
// between the last two steps, turned to Core's facings and posed from Core's actions.
//
// Keys, read by polling. The four action buttons are FTActionButtons' (the J I L K diamond and the pad's
// face buttons), each with a second home:
//   move           W A S D / left stick (turned by the camera)
//   look           mouse / right stick (when nothing is locked)
//   light attack   J / left mouse / pad X / pad RB   (a riposte at an open enemy in reach)
//   heavy attack   K / pad A / pad RT
//   guard (hold)   I / right mouse / pad Y / pad LB  (a fresh press opens the parry window)
//   dodge          L / Space / pad B
//   heal           H / pad d-pad up (not while Read cards are up)
//   lock on / off  Tab / middle mouse / pad right stick press
//   switch lock    Q / E / pad d-pad left / right (not while Read cards are up) / a flick of the right stick
//                  while locked
//   read           1 / 2 / 3 / pad d-pad left / up / right while Read cards are up (the second lead's pick
//                  of the coming move, before it goes active)
//   release        R / pad LT                         (open above a third of the Load; spends it all)
//   partner        F / pad d-pad down                 (the second lead's press: within 0.12 s of a parry, either
//                                                      side, it pairs with it into a counter that staggers)
// After the fight Enter, Space or pad A fights again (the same fight). Esc and pad Start pause, as on
// every screen (UFTPauseSubsystem); the world stops, and a key still held in the frame after the menu
// closes is not a press.
//
// The camera rides a spring arm over the player's right shoulder. Locked on, it turns to keep the player
// and the target in view; otherwise the mouse and the right stick turn it.
//
// Flags (FParse::Param, docs/ALPHA.md style): -ArenaAutoPlay lets Core's autopilot (ArenaFight::autopilot)
// play; -ArenaIdle presses nothing at all (the fight must be lost); -ArenaQuitOnFinish exits when the fight
// is over (it asks for 0 on a victory and 1 on a defeat, but UnrealEditor-Cmd exits 0 either way: read the
// result line; a fight that outlasts WatchdogSeconds, a hang guard for headless runs, logs a TIMEOUT and exits 2). Without -ArenaQuitOnFinish the autopilot fights again 4 s after the outcome.
// Under -ArenaAutoPlay the host plays the partner too: it presses the pair on the step after each parry (Core's
// autopilot never presses it). -ArenaHoldLoad keeps the autopilot from releasing until it is overloaded, so the
// overload shows (a check of the HUD, not a way to play).
// One log line per fight, ids and counts only:
//   Arena: VICTORY at <scene> beat <b> step <s> after <t> s, zone <z>, kit <k>, enemies <id>+<id>, autoplay seed <n>, hp ...
// (ftcore::arenaResultLine; DEFEAT for a loss, TIMEOUT for the watchdog; "standalone" with no story step).
// -ArenaShot=<path prefix> renders the HUD-on views docs/ARENA_DEMO.md lists (<prefix>_1_ring.png, ...)
// with HighResShot after an 8 s settle (-ArenaShotDelay=<s>) and then quits; it implies -ArenaAutoPlay.
// -ArenaOverview adds a first view, <prefix>_0_overview.png: the whole zone from a camera high over its south-west
// side, HUD off, with its walls (green), obstacles (red) and road mouths (yellow) drawn as debug lines for the
// picture only, and a second, <prefix>_0b_edge.png: the player 1.4 m from the wall, to show the edge cue.
//
// The player's figure wears a lead's look from config (the explore character's PartyLooks, Config/UserGame.ini): the first
// by default, -ArenaLead=<name> picks another; with none, or none that loads, the test human is worn (one warning).
// An enemy kind whose arena.json entry has a `look` is drawn as a skeletal mesh (AFTArenaFighter::SetupSkinned) whose clips
// come from the clip table the look names (Data/Anim/*.json, the beat clips' format): the mode picks the clip from Core's
// state each frame and times it from Core's clocks (DriveSkin), so the pose is a function of the fight. If the mesh or
// its idle clip doesn't load, the kind keeps its shapes (one warning).

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/BeatChoreo.h"
#include "ftcore/WorldLayout.h"

#include <array>
#include <memory>

#include "FTArenaGameMode.generated.h"

class AFTArenaFighter;
class APlayerController;
class UAnimSequence;
struct FFTLeadLook;

// A skinned enemy's clips: the table, the sequences loaded for it (by table index) and the table index of each clip the
// mode plays by role, -1 where it has none.
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
	int32 Defeat = -1;
	int32 HitLight = -1;
	int32 HitHeavy = -1;
	std::array<int32, ftcore::kMaxArenaMoves> MoveClip{};

	bool IsLoaded() const { return Clips.IsValid() && Idle >= 0; }
};

UCLASS()
class FORGOTTENTHIRD_API AFTArenaGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxEnemies = static_cast<int32>(ftcore::kMaxArenaEnemies);
	static constexpr int32 MaxPopups = 16;
	// The ring of the plain zone and of the map's player start (metres), centred on the world's origin.
	static constexpr float MapRingRadius = 12.f;
	static constexpr float StepSeconds = 1.f / 60.f;
	static constexpr int32 MaxStepsPerFrame = 8;
	static constexpr float WatchdogSeconds = 180.f;

	// Where the player starts (cm) for a ring of Radius metres: Radius * 0.35 behind the centre, facing +X,
	// so ftcore::arenaRingAt puts the ring's centre on the origin. Shared with FTBuildArenaWorld.
	static FVector PlayerStartFor(float Radius);

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

	AFTArenaGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// ---- for AFTArenaHUD (no allocation) ----
	bool IsBegun() const { return bBegun; }
	const ftcore::ArenaFight& GetFight() const { return Fight; }
	// The screen's clock: seconds of play since the map started, stopped while paused.
	double GetNow() const { return Now; }
	// Seconds since the fight ended (0 while it runs).
	double GetResultAge() const { return bResultLogged ? Now - ResultTime : 0.0; }
	bool IsAutoPlay() const { return bAutoPlay; }
	bool IsIdle() const { return bIdle; }
	AFTArenaFighter* GetPlayerFigure() const { return PlayerFigure; }
	// The zone the fight is held in, for the HUD's edge cue: its walls, the world's look (0 the plain zone, 1 Solhaven,
	// 2 the Lattice, 3 the Unwritten), where the player stands (metres) and the height of the ground there (cm).
	const ftcore::ArenaZone& GetZone() const { return Fit.zone; }
	int32 GetZoneStyle() const { return ZoneStyle; }
	ftcore::Vec2 GetPlayerPosition() const { return Bodies.player; }
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

private:
	// Reads Data/Battle/arena.json into Table; false (logged, ids only) when missing or refused.
	bool LoadTable();
	// Reads Data/Battle/arenas.json (and Data/Field/worlds.json for the worlds) and picks the zone (-ArenaStage, else
	// the file's default) into Spec; a built-in plain zone when the files or the table are missing or refused.
	void LoadZones(bool bTable);
	// The fight's def: the table's default, the zone's bosses or -ArenaLineup's kinds, or the built-in one; then the
	// zone fitted to those enemies in the world's real layout (ring, spawn, walls and obstacles).
	void BuildDef(bool bTable);
	// The plain zone's ground and look.
	void BuildPlainGround();
	// What a zone in a world adds to its scene: the zone's sheets of water, its own cover, and a barrier across each
	// road its wall crosses (so the invisible wall is a thing there).
	void BuildZoneDressing();
	// The world's ground height at p (metres), from a trace onto its Landscape (0 when there is none).
	double SampleGround(const ftcore::Vec2& P) const;
	void Begin();
	void SpawnFigures();
	// The lead look the player's figure wears: -ArenaLead=<name> among the explore character's PartyLooks, else the first;
	// null (logged) when the config has none.
	const FFTLeadLook* FindLead() const;
	// The fighter kit's id in arena.json: -ArenaLead=<name> lower-cased (even when no look of that name loads), else
	// the first PartyLooks name lower-cased, else "default". The table falls back to "default" for an id it lacks.
	FString FindKitId() const;
	// Dresses enemy slot I as a skeletal mesh when its kind has a look that loads (see the top of this file).
	void LoadSkin(int32 I);
	// Picks and times slot I's clip from its Core state: the move's own clip through an attack, at the rate that lands its
	// impact at the end of the startup; stagger; face down while open; the hit reaction between moves; defeat; idle.
	void DriveSkin(int32 I, const ftcore::ArenaEnemyOut& Out, const ftcore::ArenaEnemyKind& Kind, float Alpha);
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
	void Restart();
	// -ArenaShot's views; true while the fight should hold still (before the first view and just after each).
	bool Shots(float DeltaSeconds);
	void TakeShot(const TCHAR* Name, bool bCounted = true);
	// -ArenaOverview's view: true while it holds the fight still (before the first view and just after it).
	bool OverviewShot();
	// -ArenaOverview's second view, _0b_edge: the player moved to 1.4 m from the zone's wall on his left, to show the
	// edge cue; then the fight is put back as it began.
	bool EdgeShot();
	APlayerController* GetPC() const;
	FVector ToWorld(const ftcore::Vec2& P) const { return FVector(P.x * 100.0, P.y * 100.0, ZoneZ); }

	UPROPERTY()
	TObjectPtr<AFTArenaFighter> PlayerFigure;
	UPROPERTY()
	TObjectPtr<AFTArenaFighter> EnemyFigures[6]; // ftcore::kMaxArenaEnemies (a literal for UHT; checked in the .cpp)
	UPROPERTY()
	FFTArenaSkin Skins[6];                       // ... likewise
	// What each skinned enemy plays: the clip (a table index; -2 none yet) and since when, the last hit reaction.
	int32 SkinClip[ftcore::kMaxArenaEnemies];
	double SkinSince[ftcore::kMaxArenaEnemies];
	double HitSince[ftcore::kMaxArenaEnemies];
	bool bHitHeavy[ftcore::kMaxArenaEnemies] = {};
	float FrameDelta = 0.f; // the frame's seconds, for the clips' crossfades

	ftcore::ArenaTable Table;
	ftcore::WorldTable Worlds;
	ftcore::ArenaZoneTable Zones;
	ftcore::ArenaZoneSpec Spec;
	ftcore::ArenaZoneFit Fit;
	std::unique_ptr<ftcore::WorldLayout> WorldData; // the zone's world (generated at load), when it has one
	bool bZoneFromFile = false;
	bool bTravelPending = false;
	bool bTravelStarted = false;
	double ZoneZ = 0.0; // cm: the height of the ground in the zone
	int32 ZoneStyle = 0; // the world's look: 0 the plain zone, 1 Solhaven, 2 the Lattice, 3 the Unwritten
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
	bool bHoldLoad = false;
	bool bResultLogged = false;
	double ResultTime = 0.0;
	FString KitId; // the fighter kit the player plays (FindKitId), as the table resolved it

	// -ArenaShot: the prefix, the settle before the first, which have been taken, and a short hold after
	// each so the frame it renders is the moment it was taken for.
	FString ShotPrefix;
	bool bOverview = false;
	int32 OverviewPhase = 0; // 0 not yet, 1 camera placed, 2 taken, 3 back on the player
	int32 EdgePhase = 0;     // 0 not yet, 1 player at the wall, 2 taken, 3 put back
	UPROPERTY()
	TObjectPtr<AActor> OverviewCamera;
	float ShotDelay = 8.f;
	int32 ShotsTaken = 0;
	double ShotWall = 0.0;
	double ShotHoldUntil = -1.0;
	double BeginNow = 0.0;
};
