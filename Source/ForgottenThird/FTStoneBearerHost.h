#pragma once

// The host of the first Arena boss (arena.json's `stone_bearer`): one ftcore::StoneBearer (Core/include/ftcore/
// StoneBearer.h) driven beside the boss's ArenaFight body, as that header's host section asks. UFTArenaRunner owns one
// and steps it around Fight.tick; it holds no Unreal type, so a plain C++ harness can run it with the Core sources.
//
// What it does, call by call:
//   - load time: reads Data/Battle/arena_boss/stone_bearer.json (importStoneBearer) and rebuilds the kind's moves from
//     it (BuildKind) per ArenaBoss.h's wiring notes: each boss move is a string of chained ArenaEnemyMoves, one per
//     impact (the head picked by weight, the rest weight 0), timed in the boss's beats, its damage a share of the lead's
//     max hp. The rest of a move (its pick ranges, tell, tracking, poise and guard damage) comes from arena.json's move
//     of the same clip, so the arena data still tunes how it is picked and how it reads;
//   - Begin: StoneBearer::begin, the hp floor (above 0 until the pedestal is lifted) and the blow hook;
//   - BeforeTick: the boss's move mask (ArenaFight::setEnemyMoveMask), so ArenaFight only picks what the boss's rules
//     take: the head impact of a boss move in table(), while it is neither invulnerable nor down, and (SetPlayableMoves)
//     one the figure has a clip for. The lead's blows refresh it too (a break can change table() within a tick);
//   - AfterTick: AttackStarted -> startMove (the move's boss move, the lead's side from arenaSideOf). A pick the rules
//     still refuse (the mask makes it rare) is cancelled silently: no hold, its blows vetoed and nothing of it drawn,
//     and a note; Blocked -> onBlocked, the boss's Staggered -> onStagger, the lead's Released -> onRelease with the
//     Load before the tick; then update(now, hp share);
//   - Buttons: the lead's Intercept and Guard presses, each with the zone from arenaZoneOf (the lane to the marked
//     throw's point for Path);
//   - the blow hook: a lead's blow on the boss goes through onHit and is scaled by StoneBearerHit; the boss's blows are
//     scaled by damageScale(), kept to the keep-out (ArenaKeepOut's clamps on the arc, lane or area), capped by the
//     move's damageMax per use, and vetoed when the run judged the impact Intercepted;
//   - the run's events: CritOpened holds the boss Open until `until` (CritClosed lets go), Invulnerable holds it Down
//     on its plinth (the runner moves the body there; the lead's Load is topped up to one Release), Lifted ends the floor and holds it Down for the down time.
//     Everything the HUD shows (the calls, the answers, the stacks) is kept as a note.
//
// OnLoadGained gives a full intercept's Load to the lead (ArenaFight::giveLoad), and onto the plinth a lead too low to
// Release is topped up to just enough for one (the boss offers no parries there).
//
// Stubs, each a named hook point that only leaves a note: OnBroke (the runner reads its note and puts the carried
// piece's halves in the figure's hands: UFTArenaRunner::SplitBossCarried), OnShockRing (the ring a floor blow sends at 3+ stacks), OnGuestCall (the guest's voice), OnTileRain (a tile move's fragments: ArenaFight lands one all-round blow instead) and
// SoloLift (no stone pickups exist in the arena yet; nothing calls it).
//
// Nothing here allocates after Load.

#include "ftcore/ArenaBoss.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/StoneBearer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// One of the boss's notes: a run event, or one of the host's own (Kind).
struct FFTStoneBearerNote
{
	enum class EKind : std::uint8_t { Run, Refused, ShockRingSent, TileRain, LoadGiven, ReleaseMissed };

	EKind Kind = EKind::Run;
	ftcore::ArenaBossEvent Event; // the run's event (Kind Run); else type unused, move and at set
	ftcore::PedestalRelease Release = ftcore::PedestalRelease::NotHolding; // ReleaseMissed
};

// Where one of the boss's blows lands, for its effects (no rule reads it): From is the boss's spot, To the point.
struct FFTStoneBearerStrike
{
	// Area: all round, To the area's centre (the boss's spot as the keep-out moves it). Lane: To the lane's end. Side: a
	// called impact to one side of the boss. Arc: a share of the arc's reach ahead.
	enum class EShape : std::uint8_t { Arc, Lane, Area, Side };

	EShape Shape = EShape::Arc;
	ftcore::Vec2 From;
	ftcore::Vec2 To;
};

class FFTStoneBearerHost
{
public:
	static constexpr std::string_view KindId = "stone_bearer";
	static constexpr const char* DataPath = "Data/Battle/arena_boss/stone_bearer.json";
	static constexpr std::size_t MaxNotes = 32;
	// The plinth's height (m): the boss stands on it while the pedestal holds.
	static constexpr float PlinthHeight = 0.5f;
	// The mesh socket of its weak point (the json's `nape`, on the lower back): any marker, socket lookup or crit VFX
	// hook uses this name.
	static constexpr const char* WeakPointSocket = "setstone";

	// Reads the boss file. False (with the loader's error) when it is refused.
	bool Load(std::string_view Json, std::string* Error, std::vector<std::string>* Warnings);
	bool IsLoaded() const { return bLoaded; }
	const ftcore::StoneBearerDef& GetDef() const { return Def; }

	// Rewrites Kind (arena.json's stone_bearer) for a lead with Lead's kit: hp, poise, staggerTime and openTime from
	// the boss file, and the moves as above. False, Kind untouched, when the moves don't fit kMaxArenaMoves.
	bool BuildKind(ftcore::ArenaEnemyKind& Kind, const ftcore::ArenaFighter& Lead, std::string* Error) const;

	// Starts the boss in a fight just begun (or restarted) with it in slot Slot, in a zone centred on Centre with yaw
	// YawDegrees (the keep-out's frame). Fight must outlive the host's use of it (the blow hook points here).
	void Begin(ftcore::ArenaFight& Fight, std::size_t Slot, ftcore::Vec2 Centre, float YawDegrees);
	// Takes the blow hook back (and the move mask: every bit again); the host does nothing until the next Begin.
	void End();
	// The arena moves the figure can draw (bit m: move m has a clip; every bit, the default, until a skin says so).
	// Kept across Begin and End: the figure's clips don't change between fights.
	void SetPlayableMoves(std::uint32_t Mask) { Playable = Mask; }
	std::uint32_t GetPlayableMoves() const { return Playable; }
	// The move mask the rules allow now (what BeforeTick hands ArenaFight), and handing it over now.
	std::uint32_t PickMask() const;
	void RefreshMoveMask();
	bool IsActive() const { return FightPtr != nullptr; }
	std::size_t GetSlot() const { return Slot; }

	// ---- each step, around Fight.tick ----
	// The autopilot's part of the boss's buttons (it knows nothing of them): it intercepts each called impact in the
	// middle of its window, hoards Load as the pedestal nears and releases on the guest's call while it holds; while the
	// weak points are open it walks round to the gate of the first one and strikes only from inside it. In is the
	// autopilot's input, changed in place; bIntercept is the Intercept button it holds.
	void Assist(ftcore::ArenaInput& In, bool& bIntercept, const ftcore::ArenaBodies& Bodies) const;
	// The lead's Intercept and Guard buttons as they are now (held); presses and releases go to the boss with the
	// zone the lead stands in. Before the tick, with the bodies the tick will get.
	void Buttons(bool bInterceptDown, bool bGuardDown, const ftcore::ArenaBodies& Bodies);
	// Before the tick: the Load a Release would spend, and the move mask.
	void BeforeTick();
	// After the tick: the fight's events, the boss's update and what its rules ask for. Bodies as arenaStep left them;
	// when the boss steps onto its plinth its body is moved there (true: the caller should not lerp it).
	bool AfterTick(const ftcore::ArenaEvents& Events, ftcore::ArenaBodies& Bodies);

	// ---- for the figures and the HUD ----
	const ftcore::StoneBearer& GetBoss() const { return Boss; }
	// The boss is held but drawn on its feet: on its plinth.
	bool DrawnStanding() const;
	bool OnPlinth() const { return IsActive() && Boss.invulnerable(); }
	ftcore::Vec2 GetPlinthSpot() const { return Plinth; }
	const ftcore::ArenaKeepOut& GetKeepOut() const { return KeepOut; }
	// The zone the lead would press in now, and whether a called impact is pending.
	ftcore::CalledZone ZoneNow(const ftcore::ArenaBodies& Bodies) const;
	int32_t GetNoteTotal() const { return NoteTotal; }
	int32_t GetNoteCount() const { return NoteTotal < static_cast<int32_t>(MaxNotes) ? NoteTotal : static_cast<int32_t>(MaxNotes); }
	// Note I of the GetNoteCount() newest, oldest first; Serial its number (0..GetNoteTotal()-1).
	const FFTStoneBearerNote& GetNoteBySerial(int32_t Serial) const { return Notes[static_cast<std::size_t>(Serial) % MaxNotes]; }
	int32_t GetRefusals() const { return Refusals; }
	int32_t GetIntercepts() const { return Intercepts; }

	// ---- for the effects ----
	// The use in progress was refused: its blows are vetoed, so nothing of it is drawn.
	bool UseRefused() const { return bUseRefused; }
	// Where arena move Move of the boss lands, from the bodies and the boss's facing now, cut by the keep-out as its blows
	// are: a called impact to a side lands Side metres to that side and Ahead metres ahead, an arc blow ReachShare of its
	// reach ahead. False when the host isn't active, the use was refused or Move isn't one of the boss's.
	bool StrikeOf(std::uint8_t Move, const ftcore::ArenaBodies& Bodies, float Ahead, float Side, float ReachShare,
		FFTStoneBearerStrike& Out) const;
	// The side the run called for arena move Move's impact while that use is live (StrikeOf's Side shape): Left or
	// Right; Path for anything else (not a called impact, not live, refused, or the host isn't active).
	ftcore::CalledZone CalledSideOf(std::uint8_t Move) const;
	// The weak points are open (from CritOpened until CritClosed).
	bool CritOpen() const { return IsActive() && Boss.run().crit().isOpen(static_cast<double>(FightPtr->time())); }

	// A move's clip contact (ArenaBossMove::contact*) is its strike point and reach when it lies outside the body
	// (planar distance from the root above BodyRadius) and the move is neither a lane nor a tile move.
	static bool UsableContact(const ftcore::ArenaBossMove& Move, float BodyRadius);
	static float ContactPlanar(const ftcore::ArenaBossMove& Move);
	// The boss move of an arena move (kArenaBossNoMove past the table).
	std::uint8_t BossMoveOf(std::size_t ArenaMove) const
	{
		return ArenaMove < MoveOf.size() ? MoveOf[ArenaMove] : ftcore::kArenaBossNoMove;
	}
	// What the last AfterTick did with an hp-share break of the carried piece: Held is a stagger hold from it (the
	// runner draws the stagger on that same tick); the rest say why there is none.
	enum class EBreakStagger : std::uint8_t { None, Held, Already, Kneeling, Frozen, Refused };
	EBreakStagger GetBreakStagger() const { return BreakStaggerNote; }
	bool BreakStaggered() const { return bBreakStagger; }

private:
	static ftcore::ArenaBlowRuling RuleBlow(void* User, const ftcore::ArenaBlow& Blow);
	ftcore::ArenaBlowRuling RuleLeadBlow(const ftcore::ArenaBlow& Blow);
	ftcore::ArenaBlowRuling RuleBossBlow(const ftcore::ArenaBlow& Blow);
	// The boss's AttackStarted for arena move Move, StateTime seconds into it.
	void OnAttackStarted(std::uint8_t Move, float StateTime, const ftcore::ArenaBodies& Bodies);
	// The run's events: what each asks of the fight, and a note.
	void Absorb(const ftcore::ArenaBossEvents& Events);
	void Note(FFTStoneBearerNote::EKind Kind, std::uint8_t BossMove, double At);
	float HpShare() const;
	ftcore::Vec2 BossFacing() const;
	// The reach of the live boss move's lane along the boss's facing, kept out of the keep-out.
	ftcore::Vec2 LaneEnd(ftcore::Vec2 Pos, ftcore::Vec2 Facing) const;

	// The named hook points (stubs): see the header comment.
	void OnBroke(std::uint8_t CutMove, double At);
	void OnShockRing(std::uint8_t BossMove, double At);
	void OnGuestCall(const ftcore::ArenaBossEvent& Call);
	void OnLoadGained(const ftcore::ArenaBossEvent& Gained);
	void OnTileRain(std::uint8_t BossMove, double At);

	ftcore::StoneBearerDef Def; // StoneBearer keeps it by reference
	ftcore::StoneBearer Boss;
	bool bLoaded = false;

	ftcore::ArenaFight* FightPtr = nullptr;
	std::size_t Slot = 0;
	ftcore::Vec2 Plinth;
	ftcore::ArenaKeepOut KeepOut;
	int LeadHp = 100;

	// Arena move index -> the boss move and which of its impacts it lands (rebuilt from Def as BuildKind lays it out).
	std::array<std::uint8_t, ftcore::kMaxArenaMoves> MoveOf{};
	std::array<std::uint8_t, ftcore::kMaxArenaMoves> ImpactOf{};

	// The use in progress: its boss move, refused or not, the damage its blows dealt (for damageMax).
	std::uint8_t UseMove = ftcore::kArenaBossNoMove;
	bool bUseRefused = false;
	int UseDealt = 0;
	std::uint32_t Playable = 0xFFFFFFFFu;
	// The boss's attack part last step (a floor blow's shock ring goes when it turns active).
	ftcore::ArenaPart LastPart = ftcore::ArenaPart::None;
	bool bCritHold = false;
	bool bPlinthPending = false;
	bool bBreakPending = false;
	bool bBreakStagger = false;
	EBreakStagger BreakStaggerNote = EBreakStagger::None;

	bool bInterceptWas = false;
	bool bGuardWas = false;
	int LoadBefore = 0;

	std::array<FFTStoneBearerNote, MaxNotes> Notes{};
	int32_t NoteTotal = 0;
	int32_t Refusals = 0;
	int32_t Intercepts = 0;
};
