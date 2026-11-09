#pragma once

// The lead's weapon in the arena: the strike clips the player's attacks play, the props on the lead's body and the
// weapon's effects. UFTArenaRunner begins it on the player's figure once that wears a look (FTLook.h), feeds it each
// step's events and the player's state, and paints it after the figure is posed. It does nothing for the test human or
// the shapes.
//
// Clips: <ClipFolder>/lead_jab, lead_cross, lead_kick_r, lead_counter and lead_finisher, the test human's beat clips
// retargeted onto the lead's body (FTBuildRig -RetargetFrom=... -Names=... -Out=<ClipFolder>; the look's ClipFolder, as
// FTLook reads it), for StrikeLeft, StrikeRight, KickRight, Counter and Finisher; their impacts come from
// Data/Anim/beat_clips.json. On each of the player's AttackStarted events the move's clip (ftcore::arenaPlayerStrike, the
// kit's weapon picking the pattern) plays keyed so its impact lands as the attack goes active; a kick, shot or cast the
// body has no clip for is thrown as the punch on its side (FightAnimator::playStrike). The clips play over the locomotion
// (UFTLocomotionAnimInstance's actions), and fade out over kFadeOut once the attack is over or cut short. While they
// are on, the figure's shapes blade is hidden.
//
// Props (FTImportProp into /Game/Props/Weapons; the sockets made by -run=FTWeaponSockets, see FTWeaponSocketsCommandlet.h):
//   - gauntlets: four pieces, each on its mesh-only socket on the body (ft_gauntlet_vambrace_r/l on the forearms,
//     ft_gauntlet_plate_r/l on the hands);
//   - guns: the pistol in ft_pistol_grip_r (on hand_r), moved to ft_pistol_holster_r (on thigh_r) once the fight is over.
// A missing mesh or socket leaves that piece off, with one warning.
//
// Effects (FTBuildVfx from Data/Vfx; docs/VFX_UE.md):
//   - muzzle_flash on the pistol's `muzzle` socket as a gun strike goes active;
//   - gauntlet_spark at the striking hand's knuckles (middle_01_l/r, else hand_l/r) on each of the lead's hits;
//   - spell_bolt from the casting hand along the lead's facing as a magic light attack goes active;
//   - spell_burst at the chest as a magic heavy or Release goes active;
//   - spell_ward at the feet while a magic lead guards.
// Each start logs one line, "Arena: weapon fx <effect> at <fight time>". -ArenaNoWeaponFx leaves the effects off and
// -ArenaNoWeaponProps the props (OFF halves of OFF/ON captures).
//
// Nothing here allocates after Begin.

#include "CoreMinimal.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/FightAnimator.h"

class AFTArenaFighter;
class UFTLocomotionAnimInstance;
class UFTVfxComponent;
class UStaticMesh;
class UStaticMeshComponent;
class USkeletalMeshComponent;
struct FFTLeadLook;

// The props' meshes and sockets, shared with FTWeaponSocketsCommandlet.
namespace FTWeaponProps
{
// Where FTImportProp puts them.
inline constexpr const TCHAR* Folder = TEXT("/Game/Props/Weapons");

struct FPiece
{
	const TCHAR* Mesh;   // the mesh's name in the source file
	const TCHAR* Socket; // the body's mesh-only socket it rides
	const TCHAR* Bone;   // the bone that socket is on
	const TCHAR* File;   // the source file's name (Data's .glb stem), which the importer names the mesh's folder after
};
inline constexpr int32 NumGauntletPieces = 4;
inline constexpr FPiece GauntletPieces[NumGauntletPieces] = {
	{TEXT("SM_DevinGauntlet_R_Vambrace"), TEXT("ft_gauntlet_vambrace_r"), TEXT("lowerarm_r"), TEXT("Devin_Gauntlet_R")},
	{TEXT("SM_DevinGauntlet_R_HandPlate"), TEXT("ft_gauntlet_plate_r"), TEXT("hand_r"), TEXT("Devin_Gauntlet_R")},
	{TEXT("SM_DevinGauntlet_L_Vambrace"), TEXT("ft_gauntlet_vambrace_l"), TEXT("lowerarm_l"), TEXT("Devin_Gauntlet_L")},
	{TEXT("SM_DevinGauntlet_L_HandPlate"), TEXT("ft_gauntlet_plate_l"), TEXT("hand_l"), TEXT("Devin_Gauntlet_L")},
};
inline constexpr FPiece PistolGrip = {TEXT("SM_DuyPistol"), TEXT("ft_pistol_grip_r"), TEXT("hand_r"), TEXT("Duy_Pistol")};
inline constexpr FPiece PistolHolster = {TEXT("SM_DuyPistol"), TEXT("ft_pistol_holster_r"), TEXT("thigh_r"), TEXT("Duy_Pistol")};
// The pistol mesh's own socket at the muzzle, its +X along the barrel.
inline constexpr const TCHAR* MuzzleSocket = TEXT("muzzle");

// A prop's static mesh under Folder. The importer writes <Folder>/<File>/StaticMeshes/<mesh>, naming a file's only mesh
// either after the mesh or after the file, so this tries Mesh, Mesh without (or with) "SM_", File and "SM_" + File there
// and then flat in Folder. FindMeshPath gives the object path found (empty when none); FindMesh loads it (null when none).
FORGOTTENTHIRD_API FString FindMeshPath(const FPiece& Piece);
FORGOTTENTHIRD_API UStaticMesh* FindMesh(const FPiece& Piece);
}

struct FFTLeadWeapons
{
	// The clips' fade-out once the attack is over or cut short (s).
	static constexpr double kFadeOut = 0.15;

	// Sets the lead up on Figure (the player's, after its Setup): Kit is the fighter kit it plays. Nothing when the
	// figure wears no look. Again on the same figure: nothing.
	void Begin(AFTArenaFighter* Figure, const FFTLeadLook& Look, const ftcore::ArenaFighter& Kit);
	// One step's events (after the step; T the fight's time).
	void Feed(const ftcore::ArenaEvents& Events, double T);
	// The player's state after the step: the effects that start as an attack goes active, the guard's ward, the clips'
	// fade when the attack ends.
	void Track(const ftcore::ArenaPlayerOut& P, double T);
	// After the figure is posed: the clips' layers onto its anim instance at T (the fight's time drawn).
	void Paint(double T);
	// The fight begun again (its clock back to 0): the clips stop, the effects stop, the pistol back in the hand.
	void Restart();
	// Forgets the figure and its components (the figure is going).
	void Reset();

	bool IsOn() const { return Figure.IsValid(); }
	// The player's attacks that have gone active this fight, the fight time of the last, and its clip's name
	// ("lead_jab", ...; "" before any), for -ArenaShotStrikes.
	int32 GetActiveCount() const { return ActiveCount; }
	double GetLastActiveTime() const { return LastActiveTime; }
	const TCHAR* GetLastStrikeName() const;

	// For -ArenaShotClose's views. The strike under way's limb (after the clip played: hand_l or hand_r, foot_l or
	// foot_r for a kick), where it is now and whether it is the lead's left; the middle of both forearms (elbows and
	// wrists); the pistol's muzzle. False when the body (or the pistol, or that bone or socket) isn't there.
	bool GetStrikeLimb(FName& OutBone, FVector& OutAt, bool& bOutLeft) const;
	bool GetForearms(FVector& OutMid) const;
	bool GetMuzzle(FVector& OutAt) const;
	bool HasGauntlets() const { return IsOn() && Kit.weapon == ftcore::ArenaWeapon::Gauntlets; }

	// -ArenaShotStrikes: holds the lead at fight time T (the time the figures are drawn at) for a view, whatever the
	// world's clock does while the fight is held. Each effect still running is started again and run to its age at T
	// (T less its start, in kFreezeTick steps) and paused; one started more than kFreezeRerun before T is paused as it
	// is. The figure's own clock stops (its CustomTimeDilation 0: the locomotion under the clips, cloth, hair), while the
	// clips stay posed at T by Paint. One log line per effect: its age and whether it paused. Thaw lets everything go on;
	// Restart and Reset thaw too. Nothing allocates.
	static constexpr double kFreezeRerun = 1.0;
	static constexpr float kFreezeTick = 1.f / 240.f;
	void Freeze(double T);
	void Thaw();
	bool IsFrozen() const { return bFrozen; }

private:
	enum EEffect : int32
	{
		Muzzle,
		SparkLeft,
		SparkRight,
		Bolt,
		Burst,
		Ward,
		EffectCount,
	};

	void LoadClips(const FFTLeadLook& Look);
	void AttachProps();
	UStaticMeshComponent* AttachProp(const FTWeaponProps::FPiece& Piece);
	void SpawnEffects();
	UFTVfxComponent* MakeEffect(const TCHAR* Effect, USceneComponent* Parent, FName Socket);
	void Play(EEffect Effect, double T);
	void Holster(bool bHolstered);
	// The strike under way is thrown from the left side.
	bool StrikesLeft() const;

	TWeakObjectPtr<AFTArenaFighter> Figure;
	TWeakObjectPtr<USkeletalMeshComponent> Skin;
	TWeakObjectPtr<UFTLocomotionAnimInstance> Anim;
	ftcore::ArenaFighter Kit;
	ftcore::FightAnimator Animator;
	bool bClips = false;

	TWeakObjectPtr<UStaticMeshComponent> Gauntlet[FTWeaponProps::NumGauntletPieces];
	TWeakObjectPtr<UStaticMeshComponent> Pistol;
	bool bHolstered = false;
	TWeakObjectPtr<UFTVfxComponent> Fx[EffectCount];
	// When each effect last started (fight time; below 0: not since the fight began) and which Freeze paused.
	static_assert(EffectCount == 6, "StartAt's initializer has one entry per effect");
	double StartAt[EffectCount] = {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
	bool bPaused[EffectCount] = {};
	bool bFrozen = false;
	float FrozenDilation = 1.f;

	// The strike playing (Stance: none), when the clips began fading (-1: not fading), the player's last state.
	ftcore::FightClip Strike = ftcore::FightClip::Stance;
	uint8 StrikeMove = 0;
	double FadeSince = -1.0;
	ftcore::ArenaAction LastAction = ftcore::ArenaAction::Idle;
	ftcore::ArenaPart LastPart = ftcore::ArenaPart::None;
	uint8 LastMove = 0;
	bool bWarding = false;
	int32 ActiveCount = 0;
	double LastActiveTime = -1e9;
};
