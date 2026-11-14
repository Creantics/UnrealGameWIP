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
// Props:
//   - gauntlets: six skinned pieces per FTWeaponProps::GauntletPieces (glove, wrist strap and bracer for each hand), fitted
//     to the gauntlet lead's body at rest and skinned to its skeleton, so they ride it by leader pose (FTLook::AddPieces)
//     and the fingers bend with the hand; they are on whenever the kit's weapon is gauntlets, both hands half closed in
//     guard and the striking one closing into a full fist through its punch (SetHandCurl);
//   - guns: the pistol (FTImportProp into /Game/Props/Weapons; sockets by -run=FTWeaponSockets, see
//     FTWeaponSocketsCommandlet.h) in ft_pistol_grip_r (on hand_r), moved to ft_pistol_holster_r (on thigh_r) once the
//     fight is over.
// A missing mesh or socket leaves that piece off, with one warning.
//
// Aim (SetAim, each step before Track): the hit point the host gives (the target's chest on the near side of its body,
// ftcore::fieldHitPoint; straight ahead at chest height with no target). While a strike, shot or cast runs, the arm
// that throws it is turned at the hit point (UFTLocomotionAnimInstance::SetArmAim: in over the startup, held through
// the active part, out over the recovery): the knuckles of the punching fist, the pistol's barrel, the casting hand's
// fingers. Kicks are not aimed.
//
// Effects (FTBuildVfx from Data/Vfx; docs/VFX_UE.md):
//   - muzzle_flash on the pistol's `muzzle` socket as a shot goes active;
//   - gun_tracer from the muzzle to the hit point as a shot goes active (its speed scaled so its head ends on the hit
//     point at its lifetime, kTracerTime), then gun_impact at the hit point;
//   - spell_bolt from the casting hand to the hit point as any magic attack goes active (the same way, kBoltTime), then
//     spell_impact at the hit point;
//   - gauntlet_spark at the striking fist's knuckles (the foot for a kick) on the lead's hit;
//   - spell_ward at the feet while a magic lead guards.
// The magic kit (ftcore::ArenaWeapon::Magic, "weapon": "magic" in arena.json: its cast, spell effects and ward) is kept
// for a future fighter kind: no shipped fighter uses it (the third party slot is a support slot with no attack kit), and
// Core's arena tests pin what it plays.
// Each start logs one line, "<Arena|Field>: weapon fx <effect> at <fight time>". Run checks, one line each:
// "<Arena|Field>: aim <clip> <deg> deg (<what>)" once the attack has been active a moment, only on capture runs
// (-ArenaShot= or -ExploreWeaponShot=) (the angle between the
// fist's, barrel's or casting hand's direction and its direction to the hit point), and "<Arena|Field>: impact <effect>
// <cm> cm from the hit point" as the impact plays. -ArenaNoWeaponFx leaves the effects off and -ArenaNoWeaponProps the
// props (OFF halves of OFF/ON captures).
//
// An arm is aimed only at a point in front of the body: its weight fades from full at 45 degrees off the body's facing
// to none at 90, so a target behind waits until the body has turned to it.
//
// Nothing here allocates after Begin (bone names are made once; the aim check runs on capture runs only).

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

// The gauntlets: skeletal meshes on the gauntlet lead's skeleton, each <GauntletFolder>/<name>.<name>.
inline constexpr const TCHAR* GauntletFolder = TEXT("/Game/Props/Weapons/Devin_Gauntlet");
inline constexpr int32 NumGauntletPieces = 6;
inline constexpr const TCHAR* GauntletPieces[NumGauntletPieces] = {
	TEXT("SKM_DevinGauntlet_R_Glove"), TEXT("SKM_DevinGauntlet_R_Strap"), TEXT("SKM_DevinGauntlet_R_Bracer"),
	TEXT("SKM_DevinGauntlet_L_Glove"), TEXT("SKM_DevinGauntlet_L_Strap"), TEXT("SKM_DevinGauntlet_L_Bracer"),
};

struct FPiece
{
	const TCHAR* Mesh;   // the mesh's name in the source file
	const TCHAR* Socket; // the body's mesh-only socket it rides
	const TCHAR* Bone;   // the bone that socket is on
	const TCHAR* File;   // the source file's name (Data's .glb stem), which the importer names the mesh's folder after
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
	// The same on any actor wearing a lead's body (the explore character, fed by ftcore::FieldStrike): Body is its
	// skinned body, which the props and effects attach to and the clips play on. Again on the same body: nothing.
	void Begin(AActor* Owner, USkeletalMeshComponent* Body, const FFTLeadLook& Look, const ftcore::ArenaFighter& Kit);
	// Removes the props and effects it made from the body and forgets it (a look swapped on a body that stays).
	void End();
	// Where the attacks are aimed this step (world, cm), and whether at a target (else straight ahead).
	void SetAim(const FVector& HitPoint, bool bAtTarget)
	{
		AimPoint = HitPoint;
		bAimed = bAtTarget;
	}
	// The aim stance's weight (0..1, ftcore::AimStance::alpha): the pistol arm and the casting hand are raised at the aim
	// point by it between attacks (the gauntlets keep their guard; kicks are not aimed).
	void SetAimHold(float Weight) { AimHold = FMath::Clamp(Weight, 0.f, 1.f); }
	// The share of the strike clips played on the upper body only (ftcore::attackUpperShare), for Paint.
	void SetActionUpper(float Share) { ActionUpper = Share; }
	// The leg (0 left, 1 right; -1 none) that keeps the strike clip under that share, for Paint.
	void SetActionLeg(int32 Side) { ActionLeg = Side; }
	// The gun between shots (ftcore::arenaReloadLayer, arenaFlinchLayer): the reload overlay posed by the fight's reload
	// (ArenaFight::reloading, reloadProgress), and the last dry press's fight time. Each a no-op without its clip
	// (lead_reload, lead_dry_flinch in the look's clip folder; a guns kit only).
	void SetReload(bool bOn, float Progress)
	{
		bReloading = bOn;
		ReloadProgress = Progress;
	}
	void DryPressed(double T) { DryAt = T; }
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
	// The fight time to view the last strike at: its impact (bImpact; the hit or the shot's or cast's arrival) or, for a
	// shot or cast, half way along its flight. Each a moment after, for the effect to show.
	double GetViewTime(bool bImpact) const;
	// A shot's and a cast's flight from the hand to the hit point (s): gun_tracer's and spell_bolt's head lifetime, and
	// the distance their heads cover at speed scale 1 (cm).
	static constexpr double kTracerTime = 0.1;
	static constexpr double kTracerReach = 300.0;
	static constexpr double kBoltTime = 0.2;
	static constexpr double kBoltReach = 320.0;

	// For -ArenaShotClose's views. The strike under way's limb (after the clip played: hand_l or hand_r, foot_l or
	// foot_r for a kick), where it is now and whether it is the lead's left; the middle of both forearms (elbows and
	// wrists); the pistol's muzzle. False when the body (or the pistol, or that bone or socket) isn't there.
	bool GetStrikeLimb(FName& OutBone, FVector& OutAt, bool& bOutLeft) const;
	bool GetForearms(FVector& OutMid) const;
	bool GetMuzzle(FVector& OutAt) const;
	bool HasGauntlets() const { return IsOn() && Kit.weapon == ftcore::ArenaWeapon::Gauntlets; }
	// The pistol into its holster (true) or back in the hand, as a fight's end puts it; for the close views.
	void SetHolstered(bool bPut) { Holster(bPut); }

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
	// The lead's hit pause: its body's clock stands still while on (Core's hitStopLeft), and is given back after. Works
	// with Freeze/Thaw in either order.
	void HitPause(bool bOn);
	bool IsFrozen() const { return bFrozen; }

private:
	enum EEffect : int32
	{
		Muzzle,
		Travel, // the tracer or the bolt
		Impact,
		Ward,
		EffectCount,
	};

	void LoadClips(const FFTLeadLook& Look);
	// The gun's own clips (a guns kit): its length, or 0 when Folder has none that fits the body.
	float LoadGunClip(const FString& Folder, const TCHAR* Name, int32 Slot);
	// The reload and flinch layers after the strike's (StrikeWeight, 0..1, keeps its share of the arms).
	void PaintGun(UFTLocomotionAnimInstance& Instance, double T, float StrikeWeight);
	void AttachProps();
	UStaticMeshComponent* AttachProp(const FTWeaponProps::FPiece& Piece);
	void SpawnEffects();
	UFTVfxComponent* MakeEffect(const TCHAR* Effect, USceneComponent* Parent, FName Socket);
	void Play(EEffect Effect, double T);
	void Holster(bool bHolstered);
	// The strike under way is thrown from the left side.
	bool StrikesLeft() const;
	// The body's chest (the arena figure's own, else the spine's top bone, else above the actor).
	FVector Chest() const;
	// The arm aim onto the anim instance for this state (weight 0 when nothing is aimed).
	void PushArmAim(const ftcore::ArenaPlayerOut& P);
	// The run check: the aimed direction's angle to the hit point, logged once per attack.
	void LogAim();
	const TCHAR* Prefix() const;

	TWeakObjectPtr<AActor> Figure;
	TWeakObjectPtr<AFTArenaFighter> ArenaFigure;
	TWeakObjectPtr<USkeletalMeshComponent> Skin;
	TWeakObjectPtr<UFTLocomotionAnimInstance> Anim;
	ftcore::ArenaFighter Kit;
	ftcore::FightAnimator Animator;
	bool bClips = false;

	TWeakObjectPtr<USkeletalMeshComponent> Gauntlet[FTWeaponProps::NumGauntletPieces];
	TWeakObjectPtr<UStaticMeshComponent> Pistol;
	bool bHolstered = false;
	TWeakObjectPtr<UFTVfxComponent> Fx[EffectCount];
	const TCHAR* FxName[EffectCount] = {};
	// When each effect last started (fight time; below 0: not since the fight began) and which Freeze paused.
	static_assert(EffectCount == 4, "StartAt's initializer has one entry per effect");
	double StartAt[EffectCount] = {-1.0, -1.0, -1.0, -1.0};
	bool bPaused[EffectCount] = {};
	bool bFrozen = false;
	float FrozenDilation = 1.f;
	bool bHitPaused = false;
	float HitPauseDilation = 1.f;

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

	// The aim (SetAim), the arm it turns (0 left, 1 right; -1 none), the impact due at ImpactAt (fight time; below 0:
	// none) and when the last strike's aim is checked (below 0: done).
	FVector AimPoint = FVector::ZeroVector;
	bool bAimed = false;
	float AimHold = 0.f;
	float ActionUpper = 0.f;
	int32 ActionLeg = -1;
	int32 AimSide = -1;
	double ImpactDue = -1.0;
	// The gun's slots (after the fight clips'), its clips' lengths (0: none), the reload's state and the last dry press.
	static constexpr int32 kReloadSlot = static_cast<int32>(ftcore::FightClip::Count);
	static constexpr int32 kFlinchSlot = kReloadSlot + 1;
	float ReloadClipLength = 0.f;
	float FlinchClipLength = 0.f;
	bool bReloading = false;
	float ReloadProgress = 0.f;
	double DryAt = -1.0;
	FVector ImpactAt = FVector::ZeroVector;
	double LastImpactTime = -1e9;
	double AimCheckDue = -1.0;
	FVector AimCheckPoint = FVector::ZeroVector;
};
