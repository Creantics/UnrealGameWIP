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
// Core judges, the host moves. The fight itself runs in the mode's UFTArenaRunner (FTArenaRunner.h), the same runner the
// arena step's host on the explore map uses: it steps the fight at a fixed 1/60 s (an accumulator, at most 8 steps
// a frame), the step the Core tests and the balance check use, and moves the bodies with ftcore::arenaStep:
// the ring has nothing inside it to collide with, and Core keeps every body inside it. The figures
// (AFTArenaFighter: the player's pawn, which the player controller possesses, and one per enemy) are placed
// between the last two steps, turned to Core's facings and posed from Core's actions. The mode reads the flags, loads
// the table, fits the zone, builds the plain ground and takes -ArenaShot's views.
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
// result line; a fight that outlasts WatchdogSeconds, a hang guard for headless runs, logs a TIMEOUT and exits 2). Without -ArenaQuitOnFinish the autopilot fights again 4 s after the outcome, or 0.5 s after the longest defeat clip of the bosses it beat, when that is later.
// Under -ArenaAutoPlay the host plays the partner too: it presses the pair on the step after each parry (Core's
// autopilot never presses it). -ArenaHoldLoad keeps the autopilot from releasing until it is overloaded, so the
// overload shows (a check of the HUD, not a way to play). -ArenaParryHold=<s> keeps the autopilot and the partner
// from any follow-up for s seconds after each parry (no blow, Release or pair), so the open boss's clips show (a check
// of the clips, not a way to play). -ArenaBossLoop=<clip> has the boss's skin play that clip of its table looped from the
// fight's start, whatever the fight does (the fight itself runs unchanged; a check of the clips' loops, not a way to play):
// one log line at the start and one at each wrap, whose frame counter prefix finds the seam; an unknown clip logs a
// warning and the flag does nothing. -ArenaBossSplit breaks the boss's carried piece into its
// hands as the fight begins, as its first stagger would (a check of the parts in its hands, not a way to play).
// One log line per fight, ids and counts only:
//   Arena: VICTORY at <scene> beat <b> step <s> after <t> s, zone <z>, kit <k>, enemies <id>+<id>, autoplay seed <n>, hp ...
// (ftcore::arenaResultLine; DEFEAT for a loss, TIMEOUT for the watchdog; "standalone" with no story step).
// -ArenaShot=<path prefix> renders the HUD-on views docs/ARENA_DEMO.md lists (<prefix>_1_ring.png, ...)
// with HighResShot after an 8 s settle (-ArenaShotDelay=<s>) and then quits; it implies -ArenaAutoPlay.
// -ArenaOverview adds a first view, <prefix>_0_overview.png: the whole zone from a camera high over its south-west
// side, HUD off, with its walls (green), obstacles (red) and road mouths (yellow) drawn as debug lines for the
// picture only, and a second, <prefix>_0b_edge.png: the player 1.4 m from the wall, to show the edge cue.
// -ArenaShotAt=<s>,<s>,... adds a view <prefix>_t_<s>.png at each of those fight times (uncounted: the six views and the
// quit are unchanged), so two runs of the same fight (an OFF/ON pair) are shot at the same moments.
// -ArenaWorldView (without -ArenaOverview) adds instead one clean first view, <prefix>_0_world.png: a low wide look
// across the zone at the scene round it, HUD off, nothing drawn, for judging the world's look.
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
#include "ftcore/WorldLayout.h"

#include <memory>

#include "FTArenaGameMode.generated.h"

class APlayerController;
class UFTArenaRunner;
struct FFTLeadLook;

UCLASS()
class FORGOTTENTHIRD_API AFTArenaGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxEnemies = static_cast<int32>(ftcore::kMaxArenaEnemies);
	// The ring of the plain zone and of the map's player start (metres), centred on the world's origin.
	static constexpr float MapRingRadius = 12.f;

	// Where the player starts (cm) for a ring of Radius metres: Radius * 0.35 behind the centre, facing +X,
	// so ftcore::arenaRingAt puts the ring's centre on the origin. Shared with FTBuildArenaWorld.
	static FVector PlayerStartFor(float Radius);

	AFTArenaGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// The fight, for AFTArenaHUD.
	const UFTArenaRunner* GetRunner() const { return Runner; }

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
	// The ground, the zone's dressing, then the runner's fight.
	void Begin();
	// The lead look the player's figure wears: -ArenaLead=<name> among the explore character's PartyLooks, else the first;
	// null (logged) when the config has none.
	const FFTLeadLook* FindLead() const;
	// The fighter kit's id in arena.json: -ArenaLead=<name> lower-cased (even when no look of that name loads), else
	// the first PartyLooks name lower-cased, else "default". The table falls back to "default" for an id it lacks.
	FString FindKitId() const;
	// -ArenaShot's views; true while the fight should hold still (before the first view and just after each).
	bool Shots(float DeltaSeconds);
	void TakeShot(const TCHAR* Name, bool bCounted = true);
	// -ArenaOverview's view: true while it holds the fight still (before the first view and just after it).
	bool OverviewShot();
	// -ArenaOverview's second view, _0b_edge: the player moved to 1.4 m from the zone's wall on his left, to show the
	// edge cue; then the fight is put back as it began.
	bool EdgeShot();
	// -ArenaBossCam: places the camera once the boss stands, then logs its yaw each time it has turned 10 degrees.
	void BossCam();
	APlayerController* GetPC() const;

	UPROPERTY()
	TObjectPtr<UFTArenaRunner> Runner;

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
	uint32 Seed = 1;

	bool bAutoPlay = false;
	bool bIdle = false;
	bool bQuitOnFinish = false;
	bool bHoldLoad = false;
	float ParryHold = 0.f;
	FString BossLoop; // -ArenaBossLoop=<clip>
	bool bBossSplit = false;
	// -ArenaBossCam: a fixed camera on the first enemy's side, so a capture shows the boss's own turns apart from the
	// follow camera's; the yaw it last logged.
	bool bBossCam = false;
	UPROPERTY()
	TObjectPtr<AActor> BossCamera;
	float BossCamYaw = 0.f;
	FString KitId; // the fighter kit the player plays (FindKitId), as the table resolved it

	// -ArenaShot: the prefix, the settle before the first, which have been taken, and a short hold after
	// each so the frame it renders is the moment it was taken for.
	FString ShotPrefix;
	bool bOverview = false;
	bool bWorldView = false;
	int32 OverviewPhase = 0; // 0 not yet, 1 camera placed, 2 taken, 3 back on the player
	int32 EdgePhase = 0;     // 0 not yet, 1 player at the wall, 2 taken, 3 put back
	UPROPERTY()
	TObjectPtr<AActor> OverviewCamera;
	float ShotDelay = 8.f;
	int32 ShotsTaken = 0;
	double ShotWall = 0.0;
	double ShotHoldUntil = -1.0;
	// -ArenaShotAt: extra views at these fight times (seconds, ascending), and the next one due.
	TArray<double> ShotAt;
	int32 NextShotAt = 0;
};
