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
#include "UObject/SoftObjectPath.h"

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
// The fastest a riposte's counter or finisher plays to reach its impact (a strike's: FightTuning::maxRate).
constexpr float kQuickClipRate = 3.f;
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

// The gauntlet hands' fist curl in guard (UFTLocomotionAnimInstance::SetHandCurl); a punch closes its hand fully.
constexpr float kGuardCurl = 0.5f;
// The arm aim comes in over the startup's last share and goes out over this share of the recovery.
constexpr float kAimInShare = 0.7f;
constexpr float kAimOutShare = 0.6f;
// The aim is checked this long after the attack goes active (the arm posed with it at full weight).
constexpr double kAimCheckLag = 0.05;
// The arm aims at full weight up to this angle off the body's facing and not at all past the second (cosines).
constexpr float kAimFrontCos = 0.7071f; // 45 degrees
constexpr float kAimSideCos = 0.f;      // 90 degrees

// The aim check runs on capture runs only (it reads bones and logs).
bool AimChecks()
{
	static const bool bOn = FCString::Strifind(FCommandLine::Get(), TEXT("ArenaShot=")) != nullptr
		|| FCString::Strifind(FCommandLine::Get(), TEXT("ExploreWeaponShot=")) != nullptr;
	return bOn;
}

// The arm bones by side (0 left, 1 right), named once.
struct FArmBones
{
	FName Shoulder;
	FName Hand;
	FName Knuckle;
	FName KnuckleMid; // the middle finger's second joint: with Knuckle, the closed fist's striking pad between them
};
const FArmBones& ArmBones(int32 Side)
{
	static const FArmBones Bones[2] = {
		{FName(TEXT("upperarm_l")), FName(TEXT("hand_l")), FName(TEXT("middle_01_l")), FName(TEXT("middle_02_l"))},
		{FName(TEXT("upperarm_r")), FName(TEXT("hand_r")), FName(TEXT("middle_01_r")), FName(TEXT("middle_02_r"))}};
	return Bones[Side == 0 ? 0 : 1];
}
// The closed fist's striking pad: halfway along the middle finger's first bone, out from the wrist by kPadOut (the
// knuckle bone alone when the second joint is missing). False with no knuckle bone.
constexpr double kPadOut = 2.0;
bool KnucklePad(const USkeletalMeshComponent& Body, int32 Side, FVector& Out)
{
	const FArmBones& Bones = ArmBones(Side);
	if (!Body.DoesSocketExist(Bones.Knuckle))
	{
		return false;
	}
	const FVector Knuckle = Body.GetSocketLocation(Bones.Knuckle);
	Out = Body.DoesSocketExist(Bones.KnuckleMid) ? (Knuckle + Body.GetSocketLocation(Bones.KnuckleMid)) * 0.5 : Knuckle;
	if (Body.DoesSocketExist(Bones.Hand))
	{
		Out += (Knuckle - Body.GetSocketLocation(Bones.Hand)).GetSafeNormal() * kPadOut;
	}
	return true;
}
// A view of a strike's impact, this long after it.
constexpr double kViewLag = 0.03;
// A bone's place in the clip's component space at Time (up its parent chain); zero for no such bone.
FVector ClipBoneAt(const UAnimSequence& Clip, int32 Bone, double Time)
{
	const USkeleton* Skeleton = Clip.GetSkeleton();
	if (!Skeleton || Bone == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}
	const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
	FTransform CS = FTransform::Identity;
	const FAnimExtractContext Context(Time, false);
	for (int32 B = Bone; B != INDEX_NONE; B = Ref.GetParentIndex(B))
	{
		FTransform Local;
		Clip.GetBoneTransform(Local, FSkeletonPoseBoneIndex(B), Context, false);
		CS = CS * Local;
	}
	return CS.GetTranslation();
}
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
	Begin(static_cast<AActor*>(InFigure), InFigure->GetLeadSkin(), Look, InKit);
	if (IsOn())
	{
		ArenaFigure = InFigure;
		if (bClips)
		{
			// The body strikes: the shapes' blade goes.
			InFigure->SetBladeShown(false);
		}
	}
}

void FFTLeadWeapons::Begin(AActor* Owner, USkeletalMeshComponent* Body, const FFTLeadLook& Look, const ftcore::ArenaFighter& InKit)
{
	if (!Owner || (Figure.Get() == Owner && Skin.Get() == Body))
	{
		return;
	}
	Reset();
	if (!Body || !Body->GetSkeletalMeshAsset())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead weapon: the player's figure wears no look; no strike clips, props or weapon effects."));
		return;
	}
	Figure = Owner;
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
		// The clip's own travel (its root, the balls of its feet in its space) to its impact and to its end (cm).
		if (const USkeleton* Skeleton = Clip->GetSkeleton())
		{
			const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
			const int32 BallL = Ref.FindBoneIndex(TEXT("ball_l")), BallR = Ref.FindBoneIndex(TEXT("ball_r"));
			const FVector R0 = ClipBoneAt(*Clip, 0, 0.0), RK = ClipBoneAt(*Clip, 0, Key), RE = ClipBoneAt(*Clip, 0, Length);
			const FVector L0 = ClipBoneAt(*Clip, BallL, 0.0), LK = ClipBoneAt(*Clip, BallL, Key), LE = ClipBoneAt(*Clip, BallL, Length);
			const FVector F0 = ClipBoneAt(*Clip, BallR, 0.0), FK = ClipBoneAt(*Clip, BallR, Key), FE = ClipBoneAt(*Clip, BallR, Length);
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead weapon: %s travel: root to impact %s end %s; ball_l %s / %s; ball_r %s / %s; root motion %s."),
				*Name, *(RK - R0).ToCompactString(), *(RE - R0).ToCompactString(), *(LK - L0).ToCompactString(), *(LE - L0).ToCompactString(),
				*(FK - F0).ToCompactString(), *(FE - F0).ToCompactString(), Clip->HasRootMotion() ? TEXT("on") : TEXT("off"));
		}
	}
	bClips = Loaded > 0;
	if (!Missing.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: lead weapon: no %s for %s in %s (retarget them with FTBuildRig -Names=..., docs/ARENA_DEMO.md)%s."),
			*Missing, *Body->GetName(), *Folder, bClips ? TEXT("; the strikes without one are thrown as the punch on their side") : TEXT("; the attacks play no clips"));
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
		// Skinned to the body's skeleton, posed by it.
		TArray<FSoftObjectPath> Paths;
		for (const TCHAR* Name : FTWeaponProps::GauntletPieces)
		{
			Paths.Emplace(FString::Printf(TEXT("%s/%s.%s"), FTWeaponProps::GauntletFolder, Name, Name));
		}
		TArray<TObjectPtr<USkeletalMeshComponent>> Added;
		FTLook::AddPieces(*Figure.Get(), *Skin.Get(), Paths, Added, Prefix());
		for (int32 I = 0; I < Added.Num() && I < FTWeaponProps::NumGauntletPieces; ++I)
		{
			Gauntlet[I] = Added[I];
		}
		UE_LOG(LogForgottenThird, Display, TEXT("%s: lead weapon: %d of %d gauntlet pieces on %s."), Prefix(), Added.Num(),
			FTWeaponProps::NumGauntletPieces, *Skin->GetSkeletalMeshAsset()->GetName());
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
	FxName[Muzzle] = TEXT("muzzle_flash");
	FxName[Travel] = Kit.weapon == ftcore::ArenaWeapon::Magic ? TEXT("spell_bolt") : TEXT("gun_tracer");
	FxName[Impact] = Kit.weapon == ftcore::ArenaWeapon::Magic ? TEXT("spell_impact")
		: Kit.weapon == ftcore::ArenaWeapon::Guns ? TEXT("gun_impact")
		: TEXT("gauntlet_spark");
	FxName[Ward] = TEXT("spell_ward");
	switch (Kit.weapon)
	{
	case ftcore::ArenaWeapon::Guns:
		// Along the barrel from the pistol's muzzle; without the pistol, from the hand along the aim.
		if (UStaticMeshComponent* Gun = Pistol.Get(); Gun && Gun->DoesSocketExist(FTWeaponProps::MuzzleSocket))
		{
			Fx[Muzzle] = MakeEffect(FxName[Muzzle], Gun, FTWeaponProps::MuzzleSocket);
			if (Fx[Muzzle].IsValid())
			{
				Fx[Muzzle]->SetRelativeRotation(ConeTo(FVector::ForwardVector));
			}
		}
		else
		{
			if (Gun)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("%s: lead weapon: the pistol has no socket %s (run -run=FTWeaponSockets); the flash is at the hand."),
					Prefix(), FTWeaponProps::MuzzleSocket);
			}
			Fx[Muzzle] = MakeEffect(FxName[Muzzle], Body, TEXT("hand_r"));
			if (Fx[Muzzle].IsValid())
			{
				Fx[Muzzle]->SetUsingAbsoluteRotation(true);
			}
		}
		Fx[Travel] = MakeEffect(FxName[Travel], Root, NAME_None);
		Fx[Impact] = MakeEffect(FxName[Impact], Root, NAME_None);
		break;
	case ftcore::ArenaWeapon::Gauntlets:
		Fx[Impact] = MakeEffect(FxName[Impact], Root, NAME_None);
		break;
	case ftcore::ArenaWeapon::Magic:
		// The bolt and its impact are put where they start; the ward rides the feet.
		Fx[Travel] = MakeEffect(FxName[Travel], Root, NAME_None);
		Fx[Impact] = MakeEffect(FxName[Impact], Root, NAME_None);
		Fx[Ward] = MakeEffect(FxName[Ward], Root, NAME_None);
		break;
	case ftcore::ArenaWeapon::None:
		break;
	}
	for (const EEffect E : {Travel, Impact})
	{
		if (Fx[E].IsValid())
		{
			Fx[E]->SetUsingAbsoluteLocation(true);
			Fx[E]->SetUsingAbsoluteRotation(true);
		}
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
		UE_LOG(LogForgottenThird, Display, TEXT("%s: weapon fx %s at %.2f"), Prefix(), FxName[Effect], T);
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
	static const FName Feet[2] = {FName(TEXT("foot_l")), FName(TEXT("foot_r"))};
	OutBone = bKick ? Feet[bOutLeft ? 0 : 1] : ArmBones(bOutLeft ? 0 : 1).Hand;
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
	AActor* Body = Figure.Get();
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
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: view frame %.3f: weapon fx %s at age %.3f (%s), %s."), T, FxName[E], AgeNow,
			bRerun ? TEXT("run again from its start") : TEXT("as it was"), Niagara->IsPaused() ? TEXT("paused") : TEXT("NOT paused"));
	}
	if (Running == 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: view frame %.3f: no weapon fx running."), T);
	}
}

void FFTLeadWeapons::HitPause(bool bOn)
{
	if (bOn == bHitPaused)
	{
		return;
	}
	bHitPaused = bOn;
	AActor* Body = Figure.Get();
	if (!Body)
	{
		return;
	}
	// While a view frame holds the figure, its clock is Thaw's to give back: the pause changes what Thaw restores.
	float& Dilation = bFrozen ? FrozenDilation : Body->CustomTimeDilation;
	if (bOn)
	{
		HitPauseDilation = Dilation;
		Dilation = 0.f;
	}
	else
	{
		Dilation = HitPauseDilation;
	}
}

void FFTLeadWeapons::Thaw()
{
	if (!bFrozen)
	{
		return;
	}
	bFrozen = false;
	if (AActor* Body = Figure.Get())
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

FVector FFTLeadWeapons::Chest() const
{
	if (const AFTArenaFighter* Arena = ArenaFigure.Get())
	{
		return Arena->GetChest();
	}
	const USkeletalMeshComponent* Body = Skin.Get();
	for (const TCHAR* Bone : {TEXT("spine_05"), TEXT("spine_04"), TEXT("spine_03")})
	{
		if (Body && Body->DoesSocketExist(Bone))
		{
			return Body->GetSocketLocation(Bone);
		}
	}
	const AActor* Owner = Figure.Get();
	return Owner ? Owner->GetActorLocation() + FVector(0.0, 0.0, 40.0) : FVector::ZeroVector;
}

void FFTLeadWeapons::End()
{
	Thaw();
	if (UFTLocomotionAnimInstance* Instance = Anim.Get())
	{
		Instance->ClearActions();
		Instance->SetHandCurl(-1.f, -1.f);
		Instance->SetHandGrip(false, false);
		Instance->SetSupportHand(0.f);
		if (AimSide >= 0)
		{
			Instance->SetArmAim(AimSide, AimPoint, 0.f, FVector::ForwardVector, 0.f);
		}
		for (const FightClip StrikeClip : kStrikeClips)
		{
			Instance->SetActionClip(static_cast<int32>(StrikeClip), nullptr);
		}
	}
	for (TWeakObjectPtr<USkeletalMeshComponent>& Piece : Gauntlet)
	{
		if (Piece.IsValid())
		{
			Piece->DestroyComponent();
		}
	}
	if (Pistol.IsValid())
	{
		Pistol->DestroyComponent();
	}
	for (TWeakObjectPtr<UFTVfxComponent>& Vfx : Fx)
	{
		if (Vfx.IsValid())
		{
			Vfx->DestroyComponent();
		}
	}
	Reset();
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
				// Long clips (a second of wind-up) on a riposte's short startup: faster, so less of the wind-up is skipped.
				Played = Animator.play(Want, T, KeyAt, kQuickClipRate) ? Want : Animator.playStrike(FightClip::StrikeRight, T, KeyAt);
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
			// The fist's spark on the hit point (a shot's or cast's impact plays as it arrives, in Track).
			if (E.source == ftcore::kArenaPlayer && !bSparked && Kit.weapon == ftcore::ArenaWeapon::Gauntlets)
			{
				ImpactAt = AimPoint;
				ImpactDue = T;
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
		AimCheckDue = AimChecks() ? T + kAimCheckLag : -1.0;
		AimCheckPoint = AimPoint;
		USkeletalMeshComponent* Body = Skin.Get();
		// A shot or a cast flies from the muzzle or the hand to the hit point; its impact plays as it arrives.
		bool bTravel = false;
		FVector From = Chest();
		double Reach = kTracerReach;
		double Flight = kTracerTime;
		switch (Kit.weapon)
		{
		case ftcore::ArenaWeapon::Guns:
			if (ftcore::arenaPlayerStrike(Kit, P.move) == FightClip::WeaponRight)
			{
				if (!GetMuzzle(From) && Body && Body->DoesSocketExist(TEXT("hand_r")))
				{
					From = Body->GetSocketLocation(TEXT("hand_r"));
				}
				if (UFTVfxComponent* Vfx = Fx[Muzzle].Get(); Vfx && Vfx->IsUsingAbsoluteRotation())
				{
					Vfx->SetWorldRotation(ConeTo(AimPoint - From));
				}
				Play(Muzzle, T);
				bTravel = true;
			}
			break;
		case ftcore::ArenaWeapon::Magic:
		{
			const FName Hand = StrikesLeft() ? TEXT("hand_l") : TEXT("hand_r");
			if (Body && Body->DoesSocketExist(Hand))
			{
				From = Body->GetSocketLocation(Hand);
			}
			Reach = kBoltReach;
			Flight = kBoltTime;
			bTravel = true;
			break;
		}
		case ftcore::ArenaWeapon::Gauntlets:
		case ftcore::ArenaWeapon::None:
			break;
		}
		if (bTravel)
		{
			// The head is stretched or shrunk to die on the hit point at its lifetime.
			const FVector Path = AimPoint - From;
			if (UFTVfxComponent* Vfx = Fx[Travel].Get())
			{
				Vfx->SetWorldLocationAndRotation(From, ConeTo(Path));
				Vfx->SetParameters(static_cast<float>(FMath::Max(Path.Size(), 1.0) / Reach), 1.f);
			}
			Play(Travel, T);
			ImpactAt = AimPoint;
			ImpactDue = T + Flight;
		}
	}
	if (ImpactDue >= 0.0 && T + 1e-6 >= ImpactDue)
	{
		ImpactDue = -1.0;
		if (UFTVfxComponent* Vfx = Fx[Impact].Get())
		{
			// The fist's spark at its knuckles (a kick's at the foot), where it lands.
			if (Kit.weapon == ftcore::ArenaWeapon::Gauntlets)
			{
				FName Limb;
				bool bLeft = false;
				const USkeletalMeshComponent* Body = Skin.Get();
				if (!(Body && Strike != FightClip::KickLeft && Strike != FightClip::KickRight && KnucklePad(*Body, StrikesLeft() ? 0 : 1, ImpactAt)))
				{
					GetStrikeLimb(Limb, ImpactAt, bLeft);
				}
			}
			Vfx->SetWorldLocationAndRotation(ImpactAt, FRotator::ZeroRotator);
			Play(Impact, T);
			LastImpactTime = T;
			UE_LOG(LogForgottenThird, Display, TEXT("%s: impact %s %.1f cm from the hit point"), Prefix(), FxName[Impact],
				FVector::Dist(Vfx->GetComponentLocation(), AimPoint));
		}
	}
	PushArmAim(P);
	if (AimCheckDue >= 0.0 && T + 1e-6 >= AimCheckDue)
	{
		AimCheckDue = -1.0;
		LogAim();
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

const TCHAR* FFTLeadWeapons::Prefix() const
{
	return ArenaFigure.IsValid() ? TEXT("Arena") : TEXT("Field");
}

double FFTLeadWeapons::GetViewTime(bool bImpact) const
{
	switch (Kit.weapon)
	{
	case ftcore::ArenaWeapon::Guns:
		return bImpact ? LastActiveTime + kTracerTime + kViewLag : LastActiveTime + kTracerTime * 0.5;
	case ftcore::ArenaWeapon::Magic:
		return bImpact ? LastActiveTime + kBoltTime + kViewLag : LastActiveTime + kBoltTime * 0.5;
	default:
		return LastActiveTime + kViewLag;
	}
}

void FFTLeadWeapons::PushArmAim(const ftcore::ArenaPlayerOut& P)
{
	UFTLocomotionAnimInstance* Instance = Anim.Get();
	const USkeletalMeshComponent* Body = Skin.Get();
	if (!Instance || !Body)
	{
		return;
	}
	const bool bKick = Strike == FightClip::KickLeft || Strike == FightClip::KickRight;
	const ftcore::ArenaAttack* Attack = IsAttack(P.action) ? ftcore::arenaPlayerAttack(Kit, P.move) : nullptr;
	int32 Side = -1;
	switch (Kit.weapon)
	{
	case ftcore::ArenaWeapon::Gauntlets:
	case ftcore::ArenaWeapon::Magic:
		Side = StrikesLeft() ? 0 : 1;
		break;
	case ftcore::ArenaWeapon::Guns:
		Side = ftcore::arenaPlayerStrike(Kit, P.move) == FightClip::WeaponRight ? 1 : -1;
		break;
	case ftcore::ArenaWeapon::None:
		break;
	}
	float W = 0.f;
	if (Attack && !bKick && Side >= 0)
	{
		const float S = Attack->startup;
		const float A = Attack->active;
		const float R = Attack->recovery;
		const float Time = P.actionTime;
		if (Time < S)
		{
			const float In = FMath::Max(S * kAimInShare, 1e-3f);
			W = FMath::SmoothStep(0.f, 1.f, (Time - (S - In)) / In);
		}
		else if (Time < S + A)
		{
			W = 1.f;
		}
		else
		{
			const float Out = FMath::Max(R * kAimOutShare, 1e-3f);
			W = 1.f - FMath::SmoothStep(0.f, 1.f, (Time - S - A) / Out);
		}
	}
	// Held aim: the pistol arm (the right) or the casting hand stays raised at the aim point between attacks.
	if (AimHold > 0.f && !bKick && Kit.weapon != ftcore::ArenaWeapon::Gauntlets && Kit.weapon != ftcore::ArenaWeapon::None)
	{
		if (Side < 0)
		{
			Side = 1;
		}
		W = FMath::Max(W, AimHold);
	}
	// Gauntlets: both hands half closed in guard, the striking one closing into a full fist as the arm comes in.
	if (Kit.weapon == ftcore::ArenaWeapon::Gauntlets)
	{
		float Curl[2] = {kGuardCurl, kGuardCurl};
		Curl[Side == 0 ? 0 : 1] += (1.f - kGuardCurl) * W;
		Instance->SetHandCurl(Curl[0], Curl[1]);
	}
	// The pistol: the right hand shaped round its grip while it is in the hand.
	Instance->SetHandGrip(false, Kit.weapon == ftcore::ArenaWeapon::Guns && Pistol.IsValid() && !bHolstered);
	// Only at a point in front of the body: a target behind waits for the body to turn.
	if (W > 0.f)
	{
		const AActor* Owner = Body->GetOwner();
		const FVector Ahead = (Owner ? Owner->GetActorForwardVector() : Body->GetForwardVector()).GetSafeNormal2D();
		const FVector To = (AimPoint - Body->GetComponentLocation()).GetSafeNormal2D();
		if (!Ahead.IsNearlyZero() && !To.IsNearlyZero())
		{
			const float Cos = static_cast<float>(FVector::DotProduct(Ahead, To));
			W *= FMath::SmoothStep(kAimSideCos, kAimFrontCos, Cos);
		}
	}
	// Aim held with the pistol: the left hand joins the right on the grip (a two-handed hold).
	Instance->SetSupportHand(Kit.weapon == ftcore::ArenaWeapon::Guns && Side == 1 ? FMath::Min(AimHold, W) : 0.f);
	if (W <= 0.f || Side < 0)
	{
		if (AimSide >= 0)
		{
			Instance->SetArmAim(AimSide, AimPoint, 0.f, FVector::ForwardVector, 0.f);
			AimSide = -1;
		}
		return;
	}
	const FName HandBone = ArmBones(Side).Hand;
	const FName Knuckle = ArmBones(Side).Knuckle;
	if (!Body->DoesSocketExist(HandBone))
	{
		return;
	}
	const FTransform HandWorld = Body->GetSocketTransform(HandBone);
	FVector Axis = FVector::ForwardVector;
	float Reach = 0.f;
	const UStaticMeshComponent* Gun = Pistol.Get();
	if (Kit.weapon == ftcore::ArenaWeapon::Guns && Gun && Gun->DoesSocketExist(FTWeaponProps::MuzzleSocket))
	{
		Axis = HandWorld.InverseTransformVectorNoScale(Gun->GetSocketTransform(FTWeaponProps::MuzzleSocket).GetUnitAxis(EAxis::X));
	}
	else if (Body->DoesSocketExist(Knuckle))
	{
		const FVector Knuckles = Body->GetSocketLocation(Knuckle);
		Axis = HandWorld.InverseTransformVectorNoScale((Knuckles - HandWorld.GetLocation()).GetSafeNormal());
		if (Kit.weapon == ftcore::ArenaWeapon::Gauntlets)
		{
			Reach = static_cast<float>(FVector::Dist(Knuckles, HandWorld.GetLocation()));
		}
	}
	if (AimSide >= 0 && AimSide != Side)
	{
		Instance->SetArmAim(AimSide, AimPoint, 0.f, FVector::ForwardVector, 0.f);
	}
	AimSide = Side;
	Instance->SetArmAim(Side, AimPoint, W, Axis, Reach);
}

void FFTLeadWeapons::LogAim()
{
	const USkeletalMeshComponent* Body = Skin.Get();
	if (!Body || AimSide < 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: aim %s none (no arm aimed)"), Prefix(), GetLastStrikeName());
		return;
	}
	const FName Shoulder = ArmBones(AimSide).Shoulder;
	const FName Hand = ArmBones(AimSide).Hand;
	const FName Knuckle = ArmBones(AimSide).Knuckle;
	FVector From;
	FVector Dir;
	const TCHAR* What = TEXT("");
	FVector MuzzleAt;
	if (Kit.weapon == ftcore::ArenaWeapon::Guns && GetMuzzle(MuzzleAt))
	{
		From = MuzzleAt;
		Dir = Pistol->GetSocketTransform(FTWeaponProps::MuzzleSocket).GetUnitAxis(EAxis::X);
		What = TEXT("barrel");
	}
	else if (Kit.weapon == ftcore::ArenaWeapon::Gauntlets && Body->DoesSocketExist(Shoulder) && Body->DoesSocketExist(Knuckle))
	{
		From = Body->GetSocketLocation(Shoulder);
		Dir = Body->GetSocketLocation(Knuckle) - From;
		What = TEXT("shoulder to knuckles");
	}
	else if (Body->DoesSocketExist(Hand) && Body->DoesSocketExist(Knuckle))
	{
		From = Body->GetSocketLocation(Hand);
		Dir = Body->GetSocketLocation(Knuckle) - From;
		What = TEXT("hand to fingers");
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: aim %s none (no bones)"), Prefix(), GetLastStrikeName());
		return;
	}
	const FVector To = AimPoint - From;
	const double Cos = FVector::DotProduct(Dir.GetSafeNormal(), To.GetSafeNormal());
	const double Deg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Cos, -1.0, 1.0)));
	UE_LOG(LogForgottenThird, Display, TEXT("%s: aim %s %.1f deg (%s%s)"), Prefix(), GetLastStrikeName(), Deg, What,
		bAimed ? TEXT(", at the target") : TEXT(", straight ahead"));
	if (Kit.weapon == ftcore::ArenaWeapon::Gauntlets && Body->DoesSocketExist(Knuckle))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: fist %.1f cm from the hit point"), Prefix(),
			FVector::Dist(Body->GetSocketLocation(Knuckle), AimPoint));
	}
}

void FFTLeadWeapons::Paint(double T)
{
	UFTLocomotionAnimInstance* Instance = Anim.Get();
	if (!bClips || !Instance)
	{
		return;
	}
	Instance->ClearActions();
	Instance->SetActionUpper(ActionUpper);
	Instance->SetActionLeg(ActionLeg);
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
	HitPause(false);
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
	ImpactDue = -1.0;
	AimCheckDue = -1.0;
	LastImpactTime = -1e9;
	if (UFTLocomotionAnimInstance* Instance = Anim.Get())
	{
		Instance->ClearActions();
		if (AimSide >= 0)
		{
			Instance->SetArmAim(AimSide, AimPoint, 0.f, FVector::ForwardVector, 0.f);
		}
	}
	AimSide = -1;
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
	HitPause(false);
	Thaw();
	for (double& At : StartAt)
	{
		At = -1.0;
	}
	Figure.Reset();
	ArenaFigure.Reset();
	Skin.Reset();
	Anim.Reset();
	bClips = false;
	for (TWeakObjectPtr<USkeletalMeshComponent>& Piece : Gauntlet)
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
	ImpactDue = -1.0;
	AimCheckDue = -1.0;
	LastImpactTime = -1e9;
	AimSide = -1;
	bAimed = false;
	AimHold = 0.f;
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
