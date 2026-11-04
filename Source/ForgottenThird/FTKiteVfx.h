#pragma once

// The Kite-Mother's effects in the arena: Data/Battle/arena_boss/kite_boss_vfx.json names them, FTBuildVfx builds their
// systems from Data/Vfx. UFTArenaRunner spawns them on the kite's figure once the figures are up, and feeds them each step
// after FFTKiteHost::AfterTick; they hang off the figure, so they go when it does.
//
// What plays when:
//   - tell: the downwash ring on the floor under the kite, from the swoop's AttackStarted to the end of its active part
//     (the json's `tellMove`); a stagger, a death or the seam opening cuts it short;
//   - open: the seam's glow, on the mesh's `seam` bone, for as long as the weak point is open (FFTKiteHost::IsOpen);
//   - crit: a burst at the seam on each lead blow the host counted as a crit;
//   - hit: a puff where a plain lead blow lands, on the kite's body edge toward the lead at the lead's chest height.
// Each start logs one line, "Arena: kite fx <effect> at <fight time>". `-ArenaNoKiteFx` leaves them all off (the OFF half of
// an OFF/ON capture).
//
// Nothing here allocates after Spawn.

#include "CoreMinimal.h"

#include "ftcore/ArenaFight.h"

#include <string>

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
	// Stops every effect (a restart, after FFTKiteHost::Begin; the end of the fight).
	void Stop();
	// Stops them and forgets the components (the figure is going).
	void Reset();
	bool IsSpawned() const { return bSpawned; }

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
	void StopOne(EEffect Effect);

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
