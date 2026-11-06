#pragma once

// The Kite-Mother's effects in the arena: Data/Battle/arena_boss/kite_boss_vfx.json names them, FTBuildVfx builds their
// systems from Data/Vfx. UFTArenaRunner spawns them on the kite's figure once the figures are up, and feeds them each step
// after FFTKiteHost::AfterTick; they hang off the figure, so they go when it does.
//
// The json's `moves` gives each of the kite's moves (by clip id) its tell and its impact:
//   - tell: from the move's AttackStarted to its contact frame (the ring under the kite runs on to the end of the active
//     part): `ring` (the downwash ring on the floor under the kite), `tell` (one of the bone tells: pegs, tail, sheet,
//     rope, each riding the bone the effect names) and `mittens` (the raw-pink flush in those mittens, §7's cue);
//     a break of the move (the parry, a stagger, the kill) stops them: no new particles, the live ones fade out over their
//     own lives (UFTVfxComponent::Stop; RUBRIC S-65 note);
//   - impact: at the contact frame (startup after AttackStarted), dust and a shock ring on the floor under the lowest of the
//     move's `contact` bones (where the mitten, the face, the tail or the sheet comes down).
// And for every move:
//   - open: the seam's glow, on the mesh's `seam` bone, for as long as the weak point is open (FFTKiteHost::IsOpen); when the
//     fight ends (the kite's death, or the lead's defeat) it is stopped and cleared within GlowFadeSeconds (the fight is not
//     stepped again, so the window's own end never reaches it);
//   - crit: a burst at the seam on each lead blow the host counted as a crit;
//   - hit: a plain lead blow's puff on the body, attached to the `hitBones` bone nearest where the blow lands (the kite's
//     body edge toward the lead, at the lead's chest height), plus the scraps and pegs it knocks loose falling to the floor
//     under that bone and lying there a moment (`hitFloor`).
// Each start logs one line, "Arena: kite fx <effect> at <fight time>"; a stop that ends a tell early logs "Arena: kite fx
// <effect> stops at <t>" and the glow's end "Arena: kite fx <effect> fades at <t>". `-ArenaNoKiteFx` leaves them all off
// (the OFF half of an OFF/ON capture). Under -ArenaShowcase nothing is fed: the reel's cues play them instead (ShowCue).
//
// Nothing here allocates after Spawn.

#include "CoreMinimal.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/ClipShowcase.h"

#include <string_view>

class AFTArenaFighter;
class FFTKiteHost;
class UFTVfxComponent;

struct FFTKiteVfx
{
	static constexpr const TCHAR* DataPath = TEXT("Data/Battle/arena_boss/kite_boss_vfx.json");
	static constexpr int32 MaxMoves = 12;
	static constexpr int32 MaxContacts = 8;
	static constexpr int32 MaxMittens = 8;
	static constexpr int32 MaxHitBones = 16;

	// Reads the json and hangs the effects on Figure, the kite's (a missing system leaves that effect off, with one warning).
	// Again before Reset: nothing. With -ArenaNoKiteFx: nothing, with one note.
	void Spawn(AFTArenaFighter* Figure);
	// One step. Events are this tick's, Host the kite's host (already stepped) and Fight the fight; KiteGround is the kite's
	// position on the floor and LeadChest the lead's chest (world, cm).
	void Feed(const ftcore::ArenaEvents& Events, const FFTKiteHost& Host, const ftcore::ArenaFight& Fight, const FVector& KiteGround,
		const FVector& LeadChest);
	// The fight is over (the kite is down, or the lead): the tells stop and the glow is cleared within GlowFadeSeconds.
	// Feed does it on the step that ends the fight; UFTArenaRunner::Finish does it again in case that step was missed. The
	// one-shots (crit, hit, impact) play out. T is the fight time for the log line (< 0: no line).
	void EndOfFight(double T = -1.0);
	// -ArenaShowcase (UFTArenaRunner::ShowcaseCue): one cue of the reel, no fight; the log lines carry the reel time. Move is
	// the clip id of the attack a Strike cue belongs to (empty for the lone takes, which play the swoop's); Ground is the
	// floor under the kite and HitAt where a lead's plain blow would land.
	//   - tell, tell_stop: the move's tells, as in a fight; impact: its impact on the floor under its contact;
	//   - chips (light or heavy): the hit at the hit bone nearest HitAt, and its scraps falling under it;
	//   - glow_on: the seam's glow; glow_off: stopped and cleared within GlowFadeSeconds, as at the end of a fight;
	//   - crit: the burst at the seam.
	void ShowCue(const ftcore::ShowCueAt& Cue, std::string_view Move, const FVector& Ground, const FVector& HitAt);
	// The weak point's window in a later phase's go-down (s, the json's `showcase.downGlowTwo`; 0: none). The first phase's
	// is the kind's openTime.
	double GetShowDownGlowTwo() const { return ShowDownGlowTwo; }
	// Cuts every effect at once (a restart, after FFTKiteHost::Begin).
	void Stop();
	// Stops them and forgets the components (the figure is going).
	void Reset();
	bool IsSpawned() const { return bSpawned; }

	// Seconds a stopped glow takes to clear.
	static constexpr float GlowFadeSeconds = 0.3f;

private:
	enum EEffect : int32
	{
		Ring,
		Open,
		Crit,
		Hit,
		HitFloor,
		ImpactHeavy,
		ImpactLight,
		PegTell,
		TailTell,
		SheetTell,
		RopeTell,
		EffectCount,
	};
	struct FLook
	{
		FName Effect;
		FName Bone; // attached there (none: placed where it plays)
		float SpeedScale = 1.f;
		float ShapeScale = 1.f;
	};
	struct FMoveFx
	{
		static constexpr int32 MaxClip = 31;
		char Clip[MaxClip + 1] = {}; // the clip id (ASCII), compared without converting
		int32 ClipLen = 0;
		std::string_view ClipId() const { return std::string_view(Clip, static_cast<size_t>(ClipLen)); }
		bool bRing = false;
		int32 Tell = -1;     // a bone tell (EEffect), or -1
		uint32 Mittens = 0;  // bit i: MittenBones[i]
		int32 Impact = -1;   // EEffect, or -1
		FName Contact[MaxContacts];
		int32 ContactCount = 0;
	};

	void ReadTuning();
	UFTVfxComponent* Make(AFTArenaFighter* Figure, FName Effect, float SpeedScale, float ShapeScale, FName Bone);
	const FMoveFx* FindMove(std::string_view Clip) const;
	void Play(EEffect Effect, double T);
	// Stops spawning; the live particles finish.
	void StopOne(EEffect Effect);
	// Every live particle goes now (a restart only).
	void CutOne(EEffect Effect);
	// Stops spawning and clears the rest within GlowFadeSeconds.
	void FadeOne(EEffect Effect, double T);
	// The move's tells start: the ring at Ground.
	void StartTells(const FMoveFx& Row, const FVector& Ground, double T);
	// Every running tell stops spawning; its live particles fade out. Early: logged as a stop at T.
	void StopTells(double T, bool bEarly);
	// The move's impact on the floor (FloorZ) under the lowest of its contact bones (none found: under the kite at Ground).
	void PlayImpact(const FMoveFx& Row, const FVector& Ground, double T);
	// The hit at the hit bone nearest At, and its scraps on the floor (FloorZ) under it.
	void PlayHit(const FVector& At, double FloorZ, double T);

	// Logs the start of one effect.
	static void Started(FName Effect, double T);
	// The bone tell and the mittens stop (no new particles); Early: logged as a stop at T.
	void StopBoneTells(double T, bool bEarly);
	void StopRing(double T, bool bEarly);

	FLook Looks[EffectCount];
	FLook MittenLook;
	FMoveFx Moves[MaxMoves];
	int32 MoveCount = 0;
	FName Socket = TEXT("seam");
	FName MittenBones[MaxMittens];
	int32 MittenCount = 0;
	FName HitBones[MaxHitBones];
	int32 HitBoneCount = 0;
	FName LoneMove = TEXT("swoop"); // the lone takes' move
	int32 LoneRow = -1;             // its row in Moves
	float Lift = 0.05f;            // m: the ring's height over the floor
	double ShowDownGlowTwo = 0.0;

	TWeakObjectPtr<AFTArenaFighter> FigureRef;
	TWeakObjectPtr<UFTVfxComponent> Fx[EffectCount];
	TWeakObjectPtr<UFTVfxComponent> MittenFx[MaxMittens];
	bool bSpawned = false;
	bool bTelling = false;
	bool bRingRunning = false;
	int32 TellRunning = -1;      // the bone tell running, or -1
	uint32 MittensRunning = 0;
	double TellUntil = 0.0;      // the bone tell and the mittens stop here (the contact frame)
	double RingUntil = 0.0;      // the ring stops here (the active part's end)
	const FMoveFx* Pending = nullptr; // its impact is due at ImpactAt
	double ImpactAt = 0.0;
	bool bWasOpen = false;
	int32 CritsSeen = 0;
};
