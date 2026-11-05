#pragma once

// The Kite-Mother's effects in the arena: Data/Battle/arena_boss/kite_boss_vfx.json names them, FTBuildVfx builds their
// systems from Data/Vfx. UFTArenaRunner spawns them on the kite's figure once the figures are up, and feeds them each step
// after FFTKiteHost::AfterTick; they hang off the figure, so they go when it does.
//
// What plays when:
//   - tell: the downwash ring on the floor under the kite, from the swoop's AttackStarted to the end of its active part
//     (the json's `tellMove`); the break of the swoop (the parry, a stagger, the kill) cuts it at once, in the step it happens
//     in (UFTVfxComponent::StopNow: every live particle goes, none finishes its lifetime);
//   - open: the seam's glow, on the mesh's `seam` bone, for as long as the weak point is open (FFTKiteHost::IsOpen); when the
//     fight ends (the kite's death, or the lead's defeat) it is stopped and cleared within GlowFadeSeconds (the fight is not
//     stepped again, so the window's own end never reaches it);
//   - crit: a burst at the seam on each lead blow the host counted as a crit;
//   - hit: a puff where a plain lead blow lands, on the kite's body edge toward the lead at the lead's chest height.
// Each start logs one line, "Arena: kite fx <effect> at <fight time>"; a cut logs "Arena: kite fx <effect> cut at <t>" and the
// glow's end "Arena: kite fx <effect> fades at <t>". `-ArenaNoKiteFx` leaves them all off (the OFF half of an OFF/ON capture).
// Under -ArenaShowcase nothing is fed: the reel's cues play them instead (ShowCue).
//
// Nothing here allocates after Spawn.

#include "CoreMinimal.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/ClipShowcase.h"

#include <string>
#include <string_view>

class AFTArenaFighter;
class FFTKiteHost;
class UFTVfxComponent;

struct FFTKiteVfx
{
	static constexpr const TCHAR* DataPath = TEXT("Data/Battle/arena_boss/kite_boss_vfx.json");

	// Reads the json and hangs the effects on Figure, the kite's (a missing system leaves that effect off, with one warning).
	// Again before Reset: nothing. With -ArenaNoKiteFx: nothing, with one note.
	void Spawn(AFTArenaFighter* Figure);
	// One step. Events are this tick's, Host the kite's host (already stepped) and Fight the fight; KiteGround is the kite's
	// position on the floor and LeadChest the lead's chest (world, cm).
	void Feed(const ftcore::ArenaEvents& Events, const FFTKiteHost& Host, const ftcore::ArenaFight& Fight, const FVector& KiteGround,
		const FVector& LeadChest);
	// The fight is over (the kite is down, or the lead): the ring goes at once and the glow is cleared within GlowFadeSeconds.
	// Feed does it on the step that ends the fight; UFTArenaRunner::Finish does it again in case that step was missed. The
	// one-shots (crit, hit) play out. T is the fight time for the log line (< 0: no line).
	void EndOfFight(double T = -1.0);
	// -ArenaShowcase (UFTArenaRunner::ShowcaseCue): one cue of the reel, no fight; the log lines carry the reel time.
	//   - tell: the ring on the floor at Ground (the runner sends a tell inside an attack's take only for the move the fight
	//     rings: IsTellMove); tell_stop ends it: in an attack's take as the swoop's active part ends it (the live particles
	//     finish), the lone ring (ShowPlace::Ahead) cut as a break cuts it (every particle goes now);
	//   - chips: the puff at HitAt; impact: nothing (the kite has no dust);
	//   - glow_on: the seam's glow; glow_off: stopped and cleared within GlowFadeSeconds, as at the end of a fight;
	//   - crit: the burst at the seam.
	void ShowCue(const ftcore::ShowCueAt& Cue, const FVector& Ground, const FVector& HitAt);
	// The move with this clip rings the floor (the json's `tellMove`).
	bool IsTellMove(std::string_view Clip) const { return Clip == std::string_view(TellMove); }
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
		Tell,
		Open,
		Crit,
		Hit,
		EffectCount,
	};
	struct FLook
	{
		FName Effect;
		float SpeedScale = 1.f;
		float ShapeScale = 1.f;
	};

	void ReadTuning();
	UFTVfxComponent* Make(AFTArenaFighter* Figure, const FLook& Look, FName Bone);
	void Play(EEffect Effect, double T);
	// Stops spawning; the live particles finish.
	void StopOne(EEffect Effect);
	// Every live particle goes now; T < 0: not logged.
	void CutOne(EEffect Effect, double T);
	// Stops spawning and clears the rest within GlowFadeSeconds.
	void FadeOne(EEffect Effect, double T);

	FLook Looks[EffectCount];
	FName Socket = TEXT("seam");
	std::string TellMove = "swoop";
	float Lift = 0.05f; // m: the tell's height over the floor

	TWeakObjectPtr<UFTVfxComponent> Fx[EffectCount];
	bool bSpawned = false;
	bool bTelling = false;
	double TellUntil = 0.0;
	bool bWasOpen = false;
	int32 CritsSeen = 0;
};
