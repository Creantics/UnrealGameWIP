#include "FTLeadWeapons.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "FTArenaFighter.h"
#include "FTLocomotionAnimInstance.h"
#include "FTLook.h"
#include "FTText.h"
#include "FTVfxComponent.h"
#include "ForgottenThird.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstanceController.h"
#include "Templates/UniquePtr.h"

#include "ftcore/BeatChoreo.h"

#include <string>
#include <vector>

static_assert(static_cast<int32>(ftcore::FightClip::Count) <= UFTLocomotionAnimInstance::MaxActionClips,
	"an action slot per fight clip");

namespace
{
using ftcore::FightClip;

// The strike clips a lead may have; each one's id in Data/Anim/beat_clips.json and asset name is ftcore::arenaLeadClipId's.
constexpr FightClip kStrikeClips[] = {FightClip::StrikeLeft, FightClip::StrikeRight, FightClip::KickRight,
	FightClip::Counter, FightClip::Finisher, FightClip::WeaponRight, FightClip::Cast};
constexpr const TCHAR* kBeatClipsFile = TEXT("Data/Anim/beat_clips.json");
// A clip with no impact in the table meets its beat this far through.
constexpr float kDefaultKeyShare = 0.4f;
bool IsAttack(ftcore::ArenaAction A)
{
	return A == ftcore::ArenaAction::Light || A == ftcore::ArenaAction::Heavy || A == ftcore::ArenaAction::Riposte
		|| A == ftcore::ArenaAction::Release;
}

bool IsLeft(FightClip C)
{
	return C == FightClip::StrikeLeft || C == FightClip::KickLeft || C == FightClip::WeaponLeft;
}

// A newengine cone points along its +Y, which FTBuildVfx makes the component's +Z: this turns it to Dir.
FRotator ConeTo(const FVector& Dir)
{
	return FRotationMatrix::MakeFromZ(Dir.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector)).Rotator();
}

constexpr const TCHAR* kEffectNames[] = {
	TEXT("muzzle_flash"), TEXT("gauntlet_spark"), TEXT("gauntlet_spark"), TEXT("spell_bolt"), TEXT("spell_burst"), TEXT("spell_ward")};
} // namespace

FString FTWeaponProps::FindMeshPath(const FPiece& Piece)
{
	const FString Base(Piece.Mesh);
	const FString File(Piece.File);
	const FString Names[4] = {Base, Base.StartsWith(TEXT("SM_")) ? Base.RightChop(3) : TEXT("SM_") + Base, File, TEXT("SM_") + File};
	// Interchange writes <Dest>/<file>/StaticMeshes/<mesh>; a flat <Dest>/<mesh> is tried too.
	const FString Dirs[2] = {FString(Folder) / File / TEXT("StaticMeshes"), FString(Folder)};
	for (const FString& Dir : Dirs)
	{
		for (const FString& Name : Names)
		{
			const FString Package = Dir / Name;
			if (FPackageName::DoesPackageExist(Package))
			{
				return Package + TEXT(".") + Name;
			}
		}
	}
	return FString();
}

UStaticMesh* FTWeaponProps::FindMesh(const FPiece& Piece)
{
	const FString Path = FindMeshPath(Piece);
	return Path.IsEmpty() ? nullptr : LoadObject<UStaticMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

void FFTLeadWeapons::Begin(AFTArenaFighter* InFigure, const FFTLeadLook& Look, const ftcore::ArenaFighter& InKit)
{
	if (!InFigure || Figure.Get() == InFigure)
	{
		return;
	}
	Reset();
	USkeletalMeshComponent* Body = InFigure->GetLeadSkin();
	if (!Body || !Body->GetSkeletalMeshAsset())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead weapon: the player's figure wears no look; no strike clips, props or weapon effects."));
		return;
	}
	Figure = InFigure;
	Skin = Body;
	Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());
	Kit = InKit;
	Animator = ftcore::FightAnimator();

	// The kit's weapon picks the strikes: its light chain's pattern, and the heavy's.
	FightClip Pattern[ftcore::kMaxLightChain];
	const std::size_t Count = ftcore::arenaStrikePattern(Kit, Pattern, ftcore::kMaxLightChain);
	FString PatternText;
	if (Count > 0 && Animator.setPattern(Pattern, Count))
	{
		for (std::size_t I = 0; I < Count; ++I)
		{
			PatternText += (I > 0 ? TEXT(", ") : TEXT("")) + FT::ToFString(ftcore::fightClipName(Pattern[I]));
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead weapon: %s, %s; light chain %s; heavy %s."), *Look.Name,
		*FT::ToFString(ftcore::arenaWeaponName(Kit.weapon)), PatternText.IsEmpty() ? TEXT("none") : *PatternText,
		*FT::ToFString(ftcore::fightClipName(ftcore::arenaHeavyStrike(Kit))));

	LoadClips(Look);
	AttachProps();
	SpawnEffects();
	Restart();
}

void FFTLeadWeapons::LoadClips(const FFTLeadLook& Look)
{
	bClips = false;
	UFTLocomotionAnimInstance* Instance = Anim.Get();
	const USkeletalMesh* Body = Skin.IsValid() ? Skin->GetSkeletalMeshAsset() : nullptr;
	if (!Instance || !Body)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: lead weapon: %s has no locomotion anim instance; the attacks play no clips."), *Look.Name);
		return;
	}
	// The impacts.
	TUniquePtr<ftcore::BeatClips> Table = MakeUnique<ftcore::BeatClips>();
	{
		TArray<uint8> Bytes;
		const FString File = FPaths::ProjectDir() / kBeatClipsFile;
		std::string Error;
		std::vector<std::string> Warnings;
		if (!IFileManager::Get().FileExists(*File) || !FFileHelper::LoadFileToArray(Bytes, *File)
			|| !ftcore::importBeatClips(FT::BytesAsText(Bytes), *Table, &Error, &Warnings))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: lead weapon: couldn't read %s (%s); each strike meets its beat %.0f%% in."),
				kBeatClipsFile, *FT::ToFString(Error), kDefaultKeyShare * 100.f);
			Table->count = 0;
		}
	}
	// The folder FTLook reads the look's clips from.
	const FString Folder = !Look.ClipFolder.IsEmpty() || Look.Name.IsEmpty() ? Look.ClipFolder
		: FString::Printf(TEXT("/Game/Characters/%s/Rig/Retargeted"), *Look.Name);
	int32 Loaded = 0;
	FString Missing;
	for (const FightClip StrikeClip : kStrikeClips)
	{
		const char* const Id = ftcore::arenaLeadClipId(StrikeClip);
		const FString Name = FT::ToFString(Id);
		const FString Package = Folder / Name;
		UAnimSequence* Clip = !Folder.IsEmpty() && FPackageName::DoesPackageExist(Package)
			? LoadObject<UAnimSequence>(nullptr, *(Package + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet)
			: nullptr;
		if (!Clip || !UFTLocomotionAnimInstance::FitsBody(*Clip, *Body, TEXT("Arena: lead weapon")) || Clip->GetPlayLength() <= 0.f)
		{
			Missing += (Missing.IsEmpty() ? TEXT("") : TEXT(", ")) + Name;
			continue;
		}
		const float Length = Clip->GetPlayLength();
		const int Index = Table->find(Id);
		const float Impact = Index >= 0 ? static_cast<float>((*Table)[static_cast<std::size_t>(Index)].impact) : 0.f;
		const float Key = Impact > 0.f && Impact < Length ? Impact : Length * kDefaultKeyShare;
		Animator.setClip(StrikeClip, {Length, Key, false});
		Instance->SetActionClip(static_cast<int32>(StrikeClip), Clip);
		++Loaded;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead weapon: %s plays %s (%.2f s, impact %.2f s)."),
			*FT::ToFString(ftcore::fightClipName(StrikeClip)), *Name, Length, Key);
	}
	bClips = Loaded > 0;
	if (!Missing.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: lead weapon: no %s for %s in %s (retarget them with FTBuildRig -Names=..., docs/ARENA_DEMO.md)%s."),
			*Missing, *Body->GetName(), *Folder, bClips ? TEXT("; the strikes without one are thrown as the punch on their side") : TEXT("; the attacks play no clips"));
	}
	// The body strikes: the shapes' blade goes.
	if (bClips)
	{
		Figure->SetBladeShown(false);
	}
}

void FFTLeadWeapons::AttachProps()
{
	if (Kit.weapon != ftcore::ArenaWeapon::Gauntlets && Kit.weapon != ftcore::ArenaWeapon::Guns)
	{
		return;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("ArenaNoWeaponProps")))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead weapon props off (-ArenaNoWeaponProps)."));
		return;
	}
	if (Kit.weapon == ftcore::ArenaWeapon::Gauntlets)
	{
		for (int32 I = 0; I < FTWeaponProps::NumGauntletPieces; ++I)
		{
			Gauntlet[I] = AttachProp(FTWeaponProps::GauntletPieces[I]);
		}
	}
	else
	{
		Pistol = AttachProp(FTWeaponProps::PistolGrip);
	}
}

UStaticMeshComponent* FFTLeadWeapons::AttachProp(const FTWeaponProps::FPiece& Piece)
{
	USkeletalMeshComponent* Body = Skin.Get();
	UStaticMesh* Mesh = FTWeaponProps::FindMesh(Piece);
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: lead weapon: no %s in %s; it is off (import it with FTImportProp)."), Piece.Mesh,
			FTWeaponProps::Folder);
		return nullptr;
	}
	if (!Body->DoesSocketExist(Piece.Socket))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: lead weapon: %s has no socket %s; %s is off (run -run=FTWeaponSockets)."),
			*Body->GetSkeletalMeshAsset()->GetName(), Piece.Socket, Piece.Mesh);
		return nullptr;
	}
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Figure.Get());
	C->SetMobility(EComponentMobility::Movable);
	C->SetStaticMesh(Mesh);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetGenerateOverlapEvents(false);
	C->SetCastShadow(true);
	C->SetupAttachment(Body, Piece.Socket);
	C->RegisterComponent();
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead weapon: %s on %s (%s)."), *Mesh->GetName(), Piece.Socket, Piece.Bone);
	return C;
}

void FFTLeadWeapons::SpawnEffects()
{
	if (FParse::Param(FCommandLine::Get(), TEXT("ArenaNoWeaponFx")))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead weapon fx off (-ArenaNoWeaponFx)."));
		return;
	}
	USkeletalMeshComponent* Body = Skin.Get();
	USceneComponent* Root = Figure->GetRootComponent();
	switch (Kit.weapon)
	{
	case ftcore::ArenaWeapon::Guns:
		// Along the barrel from the pistol's muzzle; without the pistol, from the hand along the lead's facing.
		if (UStaticMeshComponent* Gun = Pistol.Get(); Gun && Gun->DoesSocketExist(FTWeaponProps::MuzzleSocket))
		{
			Fx[Muzzle] = MakeEffect(kEffectNames[Muzzle], Gun, FTWeaponProps::MuzzleSocket);
			if (Fx[Muzzle].IsValid())
			{
				Fx[Muzzle]->SetRelativeRotation(ConeTo(FVector::ForwardVector));
			}
		}
		else
		{
			if (Gun)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: lead weapon: the pistol has no socket %s (run -run=FTWeaponSockets); the flash is at the hand."),
					FTWeaponProps::MuzzleSocket);
			}
			Fx[Muzzle] = MakeEffect(kEffectNames[Muzzle], Body, TEXT("hand_r"));
			if (Fx[Muzzle].IsValid())
			{
				Fx[Muzzle]->SetUsingAbsoluteRotation(true);
			}
		}
		break;
	case ftcore::ArenaWeapon::Gauntlets:
		// At the knuckles.
		Fx[SparkLeft] = MakeEffect(kEffectNames[SparkLeft], Body, Body->DoesSocketExist(TEXT("middle_01_l")) ? TEXT("middle_01_l") : TEXT("hand_l"));
		Fx[SparkRight] = MakeEffect(kEffectNames[SparkRight], Body, Body->DoesSocketExist(TEXT("middle_01_r")) ? TEXT("middle_01_r") : TEXT("hand_r"));
		break;
	case ftcore::ArenaWeapon::Magic:
		// The bolt and the burst are put where they start; the ward rides the feet.
		Fx[Bolt] = MakeEffect(kEffectNames[Bolt], Root, NAME_None);
		Fx[Burst] = MakeEffect(kEffectNames[Burst], Root, NAME_None);
		Fx[Ward] = MakeEffect(kEffectNames[Ward], Root, NAME_None);
		for (const EEffect E : {Bolt, Burst})
		{
			if (Fx[E].IsValid())
			{
				Fx[E]->SetUsingAbsoluteLocation(true);
				Fx[E]->SetUsingAbsoluteRotation(true);
			}
		}
		break;
	case ftcore::ArenaWeapon::None:
		break;
	}
}

UFTVfxComponent* FFTLeadWeapons::MakeEffect(const TCHAR* Effect, USceneComponent* Parent, FName Socket)
{
	const FName Name(Effect);
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UFTVfxComponent::SystemPath(Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!System || !Parent)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: lead weapon: no %s; that effect is off (run FTBuildVfx -Only=%s)."),
			*UFTVfxComponent::SystemPath(Name), Effect);
		return nullptr;
	}
	UFTVfxComponent* Vfx = NewObject<UFTVfxComponent>(Figure.Get());
	Vfx->Effect = Name;
	Vfx->System = System;
	Vfx->bPlayOnBeginPlay = false;
	Vfx->SetupAttachment(Parent, Socket);
	Vfx->RegisterComponent();
	return Vfx;
}

void FFTLeadWeapons::Play(EEffect Effect, double T)
{
	if (UFTVfxComponent* Vfx = Fx[Effect].Get())
	{
		Vfx->Play();
		StartAt[Effect] = T;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: weapon fx %s at %.2f"), kEffectNames[Effect], T);
	}
}

bool FFTLeadWeapons::GetStrikeLimb(FName& OutBone, FVector& OutAt, bool& bOutLeft) const
{
	const USkeletalMeshComponent* Body = Skin.Get();
	if (!Body)
	{
		return false;
	}
	bOutLeft = StrikesLeft();
	const bool bKick = Strike == FightClip::KickLeft || Strike == FightClip::KickRight;
	OutBone = bKick ? (bOutLeft ? FName(TEXT("foot_l")) : FName(TEXT("foot_r"))) : (bOutLeft ? FName(TEXT("hand_l")) : FName(TEXT("hand_r")));
	if (!Body->DoesSocketExist(OutBone))
	{
		return false;
	}
	OutAt = Body->GetSocketLocation(OutBone);
	return true;
}

bool FFTLeadWeapons::GetForearms(FVector& OutMid) const
{
	const USkeletalMeshComponent* Body = Skin.Get();
	if (!Body)
	{
		return false;
	}
	const TCHAR* const Bones[] = {TEXT("lowerarm_l"), TEXT("hand_l"), TEXT("lowerarm_r"), TEXT("hand_r")};
	FVector Sum = FVector::ZeroVector;
	for (const TCHAR* Bone : Bones)
	{
		const FName Name(Bone);
		if (!Body->DoesSocketExist(Name))
		{
			return false;
		}
		Sum += Body->GetSocketLocation(Name);
	}
	OutMid = Sum / 4.0;
	return true;
}

bool FFTLeadWeapons::GetMuzzle(FVector& OutAt) const
{
	const UStaticMeshComponent* Gun = Pistol.Get();
	if (!Gun || !Gun->DoesSocketExist(FTWeaponProps::MuzzleSocket))
	{
		return false;
	}
	OutAt = Gun->GetSocketLocation(FTWeaponProps::MuzzleSocket);
	return true;
}

void FFTLeadWeapons::Freeze(double T)
{
	AFTArenaFighter* Body = Figure.Get();
	if (!Body || bFrozen)
	{
		return;
	}
	bFrozen = true;
	FrozenDilation = Body->CustomTimeDilation;
	Body->CustomTimeDilation = 0.f;
	int32 Running = 0;
	for (int32 E = 0; E < EffectCount; ++E)
	{
		bPaused[E] = false;
		UFTVfxComponent* Vfx = Fx[E].Get();
		UNiagaraComponent* Niagara = Vfx ? Vfx->GetNiagara() : nullptr;
		// Only an effect still running: a finished one is not brought back.
		if (!Niagara || !Niagara->IsActive())
		{
			continue;
		}
		const double Age = T - StartAt[E];
		bool bRerun = false;
		if (StartAt[E] >= 0.0 && Age > 0.0 && Age <= kFreezeRerun)
		{
			// From its start again, then on to its age at T (the order matters: a paused system doesn't advance).
			Vfx->Play();
			const int32 Ticks = FMath::Max(1, FMath::CeilToInt32(static_cast<float>(Age) / kFreezeTick));
			Niagara->AdvanceSimulation(Ticks, static_cast<float>(Age) / static_cast<float>(Ticks));
			bRerun = true;
		}
		Niagara->SetPaused(true);
		bPaused[E] = true;
		++Running;
		const FNiagaraSystemInstanceControllerConstPtr Controller = Niagara->GetSystemInstanceController();
		const float AgeNow = Controller.IsValid() ? static_cast<float>(Controller->GetAge()) : -1.f;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: view frame %.3f: weapon fx %s at age %.3f (%s), %s."), T, kEffectNames[E], AgeNow,
			bRerun ? TEXT("run again from its start") : TEXT("as it was"), Niagara->IsPaused() ? TEXT("paused") : TEXT("NOT paused"));
	}
	if (Running == 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: view frame %.3f: no weapon fx running."), T);
	}
}

void FFTLeadWeapons::Thaw()
{
	if (!bFrozen)
	{
		return;
	}
	bFrozen = false;
	if (AFTArenaFighter* Body = Figure.Get())
	{
		Body->CustomTimeDilation = FrozenDilation;
	}
	for (int32 E = 0; E < EffectCount; ++E)
	{
		if (!bPaused[E])
		{
			continue;
		}
		bPaused[E] = false;
		const UFTVfxComponent* Vfx = Fx[E].Get();
		if (UNiagaraComponent* Niagara = Vfx ? Vfx->GetNiagara() : nullptr)
		{
			Niagara->SetPaused(false);
		}
	}
}

bool FFTLeadWeapons::StrikesLeft() const
{
	return IsLeft(Strike);
}

void FFTLeadWeapons::Holster(bool bHolster)
{
	UStaticMeshComponent* Gun = Pistol.Get();
	USkeletalMeshComponent* Body = Skin.Get();
	if (!Gun || !Body || bHolster == bHolstered)
	{
		return;
	}
	const TCHAR* Socket = bHolster ? FTWeaponProps::PistolHolster.Socket : FTWeaponProps::PistolGrip.Socket;
	if (!Body->DoesSocketExist(Socket))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: lead weapon: %s has no socket %s; the pistol stays where it is."),
			*Body->GetSkeletalMeshAsset()->GetName(), Socket);
		return;
	}
	Gun->AttachToComponent(Body, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket);
	bHolstered = bHolster;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead weapon: the pistol %s."), bHolster ? TEXT("holstered") : TEXT("in the hand"));
}

void FFTLeadWeapons::Feed(const ftcore::ArenaEvents& Events, double T)
{
	if (!IsOn())
	{
		return;
	}
	bool bSparked = false;
	for (const ftcore::ArenaEvent& E : Events)
	{
		switch (E.type)
		{
		case ftcore::ArenaEventType::AttackStarted:
		{
			if (E.source != ftcore::kArenaPlayer)
			{
				break;
			}
			const FightClip Want = ftcore::arenaPlayerStrike(Kit, E.move);
			if (Want == FightClip::Stance)
			{
				break;
			}
			Strike = Want;
			if (!bClips)
			{
				break;
			}
			// A strike cut short and fading: the new one starts clean.
			if (FadeSince >= 0.0)
			{
				Animator.reset();
				FadeSince = -1.0;
			}
			// Its impact as the attack goes active.
			const ftcore::ArenaAttack* Attack = ftcore::arenaPlayerAttack(Kit, E.move);
			const double KeyAt = Attack ? T + static_cast<double>(Attack->startup) : -1.0;
			FightClip Played = FightClip::Stance;
			if (Want == FightClip::Counter || Want == FightClip::Finisher)
			{
				Played = Animator.play(Want, T, KeyAt) ? Want : Animator.playStrike(FightClip::StrikeRight, T, KeyAt);
			}
			else
			{
				Played = Animator.playStrike(Want, T, KeyAt);
			}
			if (Played != FightClip::Stance)
			{
				Strike = Played;
			}
			break;
		}
		case ftcore::ArenaEventType::Hit:
			if (E.source == ftcore::kArenaPlayer && !bSparked && Kit.weapon == ftcore::ArenaWeapon::Gauntlets)
			{
				Play(StrikesLeft() ? SparkLeft : SparkRight, T);
				bSparked = true;
			}
			break;
		case ftcore::ArenaEventType::Victory:
		case ftcore::ArenaEventType::Defeat:
			Holster(true);
			break;
		default:
			break;
		}
	}
}

void FFTLeadWeapons::Track(const ftcore::ArenaPlayerOut& P, double T)
{
	if (!IsOn())
	{
		return;
	}
	const bool bAttacking = IsAttack(P.action);
	const bool bWentActive = bAttacking && P.part == ftcore::ArenaPart::Active
		&& (LastPart != ftcore::ArenaPart::Active || LastMove != P.move || LastAction != P.action);
	if (bWentActive)
	{
		++ActiveCount;
		LastActiveTime = T;
		const FVector Facing(P.facing.x, P.facing.y, 0.0);
		USkeletalMeshComponent* Body = Skin.Get();
		switch (Kit.weapon)
		{
		case ftcore::ArenaWeapon::Guns:
			if (ftcore::arenaPlayerStrike(Kit, P.move) == FightClip::WeaponRight)
			{
				if (UFTVfxComponent* Vfx = Fx[Muzzle].Get(); Vfx && Vfx->IsUsingAbsoluteRotation())
				{
					Vfx->SetWorldRotation(ConeTo(Facing));
				}
				Play(Muzzle, T);
			}
			break;
		case ftcore::ArenaWeapon::Magic:
			if (P.move == ftcore::kArenaHeavyMove || P.move == ftcore::kArenaReleaseMove)
			{
				if (UFTVfxComponent* Vfx = Fx[Burst].Get())
				{
					Vfx->SetWorldLocation(Figure->GetChest());
				}
				Play(Burst, T);
			}
			else if (P.action == ftcore::ArenaAction::Light)
			{
				if (UFTVfxComponent* Vfx = Fx[Bolt].Get())
				{
					const FName Hand = StrikesLeft() ? TEXT("hand_l") : TEXT("hand_r");
					const FVector From = Body && Body->DoesSocketExist(Hand) ? Body->GetSocketLocation(Hand)
						: Figure->GetChest();
					Vfx->SetWorldLocationAndRotation(From, ConeTo(Facing));
				}
				Play(Bolt, T);
			}
			break;
		case ftcore::ArenaWeapon::Gauntlets:
		case ftcore::ArenaWeapon::None:
			break;
		}
	}
	const bool bWard = Kit.weapon == ftcore::ArenaWeapon::Magic && P.action == ftcore::ArenaAction::Guard;
	if (bWard != bWarding)
	{
		bWarding = bWard;
		if (bWard)
		{
			Play(Ward, T);
		}
		else if (UFTVfxComponent* Vfx = Fx[Ward].Get())
		{
			Vfx->Stop();
		}
	}
	// The attack is over or cut short: the clips fade out.
	if (!bAttacking && FadeSince < 0.0 && Animator.actionCount() > 0)
	{
		FadeSince = T;
	}
	LastAction = P.action;
	LastPart = P.part;
	LastMove = P.move;
}

void FFTLeadWeapons::Paint(double T)
{
	UFTLocomotionAnimInstance* Instance = Anim.Get();
	if (!bClips || !Instance)
	{
		return;
	}
	Instance->ClearActions();
	float Fade = 1.f;
	if (FadeSince >= 0.0)
	{
		Fade = static_cast<float>(1.0 - (T - FadeSince) / kFadeOut);
		if (Fade <= 0.f)
		{
			Animator.reset();
			FadeSince = -1.0;
			return;
		}
		Fade = FMath::Min(Fade, 1.f);
	}
	Animator.update(T);
	for (std::size_t I = 0; I < Animator.layerCount(); ++I)
	{
		const ftcore::FightLayer& Layer = Animator.layer(I);
		if (Layer.clip != FightClip::Stance)
		{
			Instance->AddAction(static_cast<int32>(Layer.clip), Layer.time, Layer.weight * Fade);
		}
	}
}

void FFTLeadWeapons::Restart()
{
	Thaw();
	for (double& At : StartAt)
	{
		At = -1.0;
	}
	Animator.reset();
	FadeSince = -1.0;
	Strike = FightClip::Stance;
	LastAction = ftcore::ArenaAction::Idle;
	LastPart = ftcore::ArenaPart::None;
	LastMove = 0;
	ActiveCount = 0;
	LastActiveTime = -1e9;
	bWarding = false;
	if (UFTLocomotionAnimInstance* Instance = Anim.Get())
	{
		Instance->ClearActions();
	}
	for (TWeakObjectPtr<UFTVfxComponent>& Vfx : Fx)
	{
		if (Vfx.IsValid())
		{
			Vfx->Stop();
		}
	}
	Holster(false);
}

void FFTLeadWeapons::Reset()
{
	Thaw();
	for (double& At : StartAt)
	{
		At = -1.0;
	}
	Figure.Reset();
	Skin.Reset();
	Anim.Reset();
	bClips = false;
	for (TWeakObjectPtr<UStaticMeshComponent>& Piece : Gauntlet)
	{
		Piece.Reset();
	}
	Pistol.Reset();
	bHolstered = false;
	for (TWeakObjectPtr<UFTVfxComponent>& Vfx : Fx)
	{
		Vfx.Reset();
	}
	Animator = ftcore::FightAnimator();
	FadeSince = -1.0;
	Strike = FightClip::Stance;
	ActiveCount = 0;
	LastActiveTime = -1e9;
	bWarding = false;
}

const TCHAR* FFTLeadWeapons::GetLastStrikeName() const
{
	// kStrikeClips' ids, as TCHAR.
	switch (Strike)
	{
	case FightClip::Stance: return TEXT("");
	case FightClip::StrikeLeft: return TEXT("lead_jab");
	case FightClip::StrikeRight: return TEXT("lead_cross");
	case FightClip::KickRight: return TEXT("lead_kick_r");
	case FightClip::Counter: return TEXT("lead_counter");
	case FightClip::Finisher: return TEXT("lead_finisher");
	case FightClip::WeaponRight: return TEXT("lead_fire");
	case FightClip::Cast: return TEXT("lead_cast");
	default: break;
	}
	return TEXT("strike");
}
