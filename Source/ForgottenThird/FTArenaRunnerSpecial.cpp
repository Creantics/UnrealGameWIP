// UFTArenaRunner's special move (FTArenaRunner.h, "A special move"): the shot list over a frozen fight.
//
// Core runs the move (ftcore/ArenaSpecial.h): the gauge, the press, the target, the freeze, the results at the cut back.
// The press carries the end point this host allows: Core's endTravel/endSide (the body's end: the clip's pelvis; the shot
// file's end_feet are its feet, which may end a little behind, the slip hidden by the cut back), forward toward
// the target, clamped to walkable ground first (the capsule swept along the path through the static world, the ring,
// the standing bodies). Blocked: the performer starts back by the missing distance when there is room behind
// (specialBack), else the travel is shortened (specialTravel). Core puts the performer at view.origin at the press and
// at view.end at the cut back; this host draws it there with no step that tick. While frozen the fight keeps stepping
// (only the special's clock moves); the world but the performer is held here, the camera plays the shots, and a tail
// of shots past Core's cut back holds the fight until the last shot has blended into the play camera.
//
// The lead plays the move's clip (the shot file's "clip", found under the worn body's folders) in its special action
// slot on the shots' clock; a lead with no clip for its move has the press dropped (8.5 s of a frozen fight with no
// performance), unless -ArenaSpecialNoClip. A press of Special or Interact while it plays is a skip: Core cuts to its
// end when the move is skippable (seen to its end once), else it is refused (logged once); in the shots' tail past
// Core's cut back it ends the shots. Every way out (played out, skipped, the fight ended or begun again, the
// component torn down) gives back the freeze, the rain and the camera.

#include "FTArenaRunner.h"

#include "Algo/Sort.h"
#include "Animation/AnimSequence.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "CollisionShape.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/HitResult.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "FTArenaFighter.h"
#include "FTInputBindings.h"
#include "FTLocomotionAnimInstance.h"
#include "FTLook.h"
#include "FTPauseSubsystem.h"
#include "FTRainSubsystem.h"
#include "FTSpecialCine.h"
#include "FTVfxComponent.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "RHI.h"
#include "RenderCore.h"

#include <limits>
#include <string>
#include <vector>

namespace
{
constexpr float kSpecialFreeze = 0.001f; // the held world's clock
constexpr float kSpecialRain = 0.02f;    // the rain's
constexpr double kSpecialCostWindow = 2.0; // s after the end the cost keeps counting
constexpr int32 kSpecialAfterFrames = 10;   // frames after the end the no-pop check reads
constexpr float kCoreSpecialEps = 1.0e-4f;  // s: Core's own (ArenaFight.cpp, specialTick): an end due within it is due now
const FName kSpecialBone(TEXT("hand_r"));

FVector Cm(const ftcore::CutsceneVec3& M) { return FVector(M.x, M.y, M.z) * 100.0; }

// The hit's effect: the move's impact row (FX_DS_Wave.hit in Data/Battle/arena_special.json) names an asset not built
// yet; this tracked burst stands in for it until it is.
const FName kSpecialHitStandIn(TEXT("crit_burst"));
constexpr float kSpecialHitScale = 1.f;
constexpr float kSpecialHitProbeLag = 0.1f; // s on the shots' clock: the probe after the hit's

// The hit's time: the move's impact row (the first FX_ row on the impact cue), else Core's impact itself.
float SpecialHitTime(const ftcore::ArenaSpecialRules& Rules, float Impact, const TCHAR** Row = nullptr)
{
	for (uint8 I = 0; I < Rules.fxCount; ++I)
	{
		const ftcore::ArenaSpecialFx& Fx = Rules.fx[I];
		if (Fx.start.cue == ftcore::kArenaSpecialAtImpact && Fx.view().rfind("FX_", 0) == 0)
		{
			if (Row)
			{
				*Row = TEXT("its impact row");
			}
			return ftcore::arenaSpecialFxAt(Rules, Fx.start, Impact);
		}
	}
	if (Row)
	{
		*Row = TEXT("the impact");
	}
	return Impact;
}

// The closed fist's striking pad (as FFTLeadWeapons' KnucklePad finds it): halfway along the middle finger's first
// bone, kSpecialPadOut cm out from the wrist. Per side: the wrist, the knuckle, the second joint.
const FName kSpecialPadBones[2][3] = {{FName(TEXT("hand_l")), FName(TEXT("middle_01_l")), FName(TEXT("middle_02_l"))},
	{FName(TEXT("hand_r")), FName(TEXT("middle_01_r")), FName(TEXT("middle_02_r"))}};
constexpr double kSpecialPadOut = 2.0;
FVector SpecialPadOf(const FVector* Wrist, const FVector& Knuckle, const FVector* Mid)
{
	FVector Pad = Mid ? (Knuckle + *Mid) * 0.5 : Knuckle;
	if (Wrist)
	{
		Pad += (Knuckle - *Wrist).GetSafeNormal() * kSpecialPadOut;
	}
	return Pad;
}
// A bone's place in the clip's component space at Time (up its parent chain); false for no such bone.
bool SpecialClipBone(const UAnimSequence& Clip, const FName Name, double Time, FVector& Out)
{
	const USkeleton* Skeleton = Clip.GetSkeleton();
	const FReferenceSkeleton* Ref = Skeleton ? &Skeleton->GetReferenceSkeleton() : nullptr;
	const int32 Bone = Ref ? Ref->FindBoneIndex(Name) : INDEX_NONE;
	if (Bone == INDEX_NONE)
	{
		return false;
	}
	FTransform CS = FTransform::Identity;
	const FAnimExtractContext Context(Time, false);
	for (int32 B = Bone; B != INDEX_NONE; B = Ref->GetParentIndex(B))
	{
		FTransform Local;
		Clip.GetBoneTransform(Local, FSkeletonPoseBoneIndex(B), Context, false);
		CS = CS * Local;
	}
	Out = CS.GetTranslation();
	return true;
}
// The pad in the clip's component space at Time; false with no knuckle bone.
bool SpecialClipPad(const UAnimSequence& Clip, int32 Side, double Time, FVector& Out)
{
	const FName* Bones = kSpecialPadBones[Side == 0 ? 0 : 1];
	FVector Wrist, Knuckle, Mid;
	if (!SpecialClipBone(Clip, Bones[1], Time, Knuckle))
	{
		return false;
	}
	const bool bWrist = SpecialClipBone(Clip, Bones[0], Time, Wrist);
	const bool bMid = SpecialClipBone(Clip, Bones[2], Time, Mid);
	Out = SpecialPadOf(bWrist ? &Wrist : nullptr, Knuckle, bMid ? &Mid : nullptr);
	return true;
}
// The pad on the drawn body (world, cm); false with no knuckle bone.
bool SpecialDrawnPad(const USkeletalMeshComponent& Body, int32 Side, FVector& Out)
{
	const FName* Bones = kSpecialPadBones[Side == 0 ? 0 : 1];
	if (!Body.DoesSocketExist(Bones[1]))
	{
		return false;
	}
	const FVector Wrist = Body.GetSocketLocation(Bones[0]);
	const FVector Mid = Body.GetSocketLocation(Bones[2]);
	Out = SpecialPadOf(Body.DoesSocketExist(Bones[0]) ? &Wrist : nullptr, Body.GetSocketLocation(Bones[1]), Body.DoesSocketExist(Bones[2]) ? &Mid : nullptr);
	return true;
}

bool ReadData(const FString& Rel, std::string& Out)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *(FPaths::ProjectDir() / TEXT("Data") / Rel)))
	{
		return false;
	}
	Out = TCHAR_TO_UTF8(*Text);
	return true;
}

// The fix search's world test (ftcore::SpecialCineWorldTest): the eye's sphere swept through the static world.
constexpr float kSpecialEyeRadius = 10.f; // cm
struct FSpecialWorld
{
	UWorld* World = nullptr;
	const FCollisionQueryParams* Params = nullptr;
};
bool SpecialWorldBlocked(const ftcore::CutsceneVec3& From, const ftcore::CutsceneVec3& To, void* User)
{
	const FSpecialWorld* W = static_cast<const FSpecialWorld*>(User);
	if (!W || !W->World || !W->Params)
	{
		return false;
	}
	const FVector A = Cm(From), B = Cm(To);
	FHitResult Hit;
	return W->World->SweepSingleByObjectType(Hit, A, B.Equals(A) ? A + FVector(0.0, 0.0, 0.1) : B, FQuat::Identity,
		FCollisionObjectQueryParams(ECC_WorldStatic), FCollisionShape::MakeSphere(kSpecialEyeRadius), *W->Params);
}

void LogWarnings(const std::vector<std::string>& Warnings)
{
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s"), UTF8_TO_TCHAR(W.c_str()));
	}
}
} // namespace

void UFTArenaRunner::ReadSpecialTable()
{
#if !UE_BUILD_SHIPPING
	const TCHAR* Cmd = FCommandLine::Get();
	SpecialTestAt = -1.f;
	SpecialTestUntil = -1.f;
	FParse::Value(Cmd, TEXT("ArenaSpecialTest="), SpecialTestAt);
	bSpecialShots = FParse::Param(Cmd, TEXT("ArenaSpecialShots"));
	// A test press (-ArenaSpecialTest) plays with no clip too: the check runs on a checkout without the lead's clip.
	bSpecialClipStandIn = FParse::Param(Cmd, TEXT("ArenaSpecialNoClip")) || SpecialTestAt >= 0.f;
	SpecialPauseAt = -1.f;
	FParse::Value(Cmd, TEXT("ArenaSpecialPause="), SpecialPauseAt);
	SpecialSkipAt = -1.f;
	FParse::Value(Cmd, TEXT("ArenaSpecialSkipAt="), SpecialSkipAt);
	bSpecialHitFx = !FParse::Param(Cmd, TEXT("ArenaSpecialNoHitFx"));
	FString HitFxName;
	SpecialHitEffect = FParse::Value(Cmd, TEXT("ArenaSpecialHitFx="), HitFxName) && !HitFxName.IsEmpty() ? FName(*HitFxName) : NAME_None;
	bSpecialHitHoldRuns = FParse::Param(Cmd, TEXT("ArenaSpecialHitHoldRuns"));
	bSpecialHitLate = FParse::Param(Cmd, TEXT("ArenaSpecialHitLate"));
#endif
	if (bSpecialTableRead)
	{
		return;
	}
	bSpecialTableRead = true; // once, read or not
	std::string Text, Error;
	std::vector<std::string> Warnings;
	if (!ReadData(TEXT("Cutscene/specials.json"), Text))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: Data/Cutscene/specials.json not found; no special moves."));
		return;
	}
	if (!ftcore::importSpecialCineTable(Text, SpecialTable, &Error, &Warnings))
	{
		LogWarnings(Warnings);
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s; no special moves."), UTF8_TO_TCHAR(Error.c_str()));
		return;
	}
	for (uint32 I = 0; I < SpecialTable.size(); ++I)
	{
		ftcore::SpecialCineMove& Move = SpecialTable.edit(I);
		// The near set (its own shot file), played under the move's near_within (SpecialCineTable::find).
		if (const std::string Near(Move.nearFile()); !Near.empty())
		{
			const FString NearFile = FString(TEXT("Cutscene/")) + UTF8_TO_TCHAR(Near.c_str()) + TEXT(".json");
			Error.clear();
			if (!ReadData(NearFile, Text))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s not found; that move plays its far set at every range."), *NearFile);
			}
			else if (!ftcore::importSpecialCineShots(Text, SpecialTable.editNear(I), &Error, &Warnings))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s"), UTF8_TO_TCHAR(Error.c_str()));
			}
		}
		const FString File = FString(TEXT("Cutscene/")) + UTF8_TO_TCHAR(std::string(Move.shotFile()).c_str()) + TEXT(".json");
		Error.clear();
		if (!ReadData(File, Text))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s not found; that move has no shots."), *File);
			continue;
		}
		if (!ftcore::importSpecialCineShots(Text, Move, &Error, &Warnings))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s"), UTF8_TO_TCHAR(Error.c_str()));
		}
	}
	LogWarnings(Warnings);
	UE_LOG(LogForgottenThird, Log, TEXT("Arena: specials: %u move(s) read."), SpecialTable.size());
	// Core's moves: the gauge, the press, the results, the end point.
	Warnings.clear();
	Error.clear();
	if (!ReadData(TEXT("Battle/arena_special.json"), Text))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: Data/Battle/arena_special.json not found; no special moves in the fight."));
		return;
	}
	bArenaSpecials = ftcore::importArenaSpecials(Text, ArenaSpecials, &Error, &Warnings);
	LogWarnings(Warnings);
	if (!bArenaSpecials)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s; no special moves in the fight."), UTF8_TO_TCHAR(Error.c_str()));
		return;
	}
	// Core's end point and the shot file's end feet are one number twice: they must agree, else the lead ends away
	// from where the last shot frames it.
	for (std::size_t I = 0; I < ArenaSpecials.count; ++I)
	{
		const ftcore::ArenaSpecialRules& Rules = ArenaSpecials.moves[I];
		const std::string Id(Rules.idView());
		const ftcore::SpecialCineMove* Shots = SpecialTable.find(Id);
		if (!Shots || Shots->empty())
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s has no shots: it plays under the play camera."), UTF8_TO_TCHAR(Id.c_str()));
			continue;
		}
		const ftcore::CutsceneVec3 Feet = Shots->endFeet();
		// The feet may end up to 20 cm behind the body (the clip's pelvis over its last step); not to the side.
		if (Feet.x > Rules.endTravel + 0.005f || Rules.endTravel - Feet.x > 0.2f || FMath::Abs(Feet.y - Rules.endSide) > 0.005f)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s: Core ends it at (%.2f, %.2f) m but its shots at end_feet (%.2f, %.2f) m."),
				UTF8_TO_TCHAR(Id.c_str()), Rules.endTravel, Rules.endSide, Feet.x, Feet.y);
		}
		if (FMath::Abs(Shots->length() - Rules.duration) > 0.05f)
		{
			UE_LOG(LogForgottenThird, Warning,
				TEXT("Arena: specials: %s: Core cuts back at %.2f s, its shots end at %.2f s: the fight is held for the shots' tail."),
				UTF8_TO_TCHAR(Id.c_str()), Rules.duration, Shots->length());
		}
		if (FMath::Abs(Shots->release() - Rules.strikeAt) > 0.05f)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s: Core strikes at %.2f s, its shots' release beat at %.2f s."),
				UTF8_TO_TCHAR(Id.c_str()), Rules.strikeAt, Shots->release());
		}
	}
	UE_LOG(LogForgottenThird, Log, TEXT("Arena: specials: %u move(s) in the fight."), static_cast<uint32>(ArenaSpecials.count));
}

void UFTArenaRunner::GiveSpecials()
{
	if (!bArenaSpecials)
	{
		return;
	}
	auto Id = [](const FFTLeadLook* Look) { return Look ? std::string(TCHAR_TO_UTF8(*Look->Name.ToLower())) : std::string(); };
	const std::string Fighter = Id(PlayerLook);
	const std::string Partner = bPartner ? Id(AllyLook) : std::string();
	if (!Fight.setSpecials(&ArenaSpecials, Fighter, Partner, SpecialGauges))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: the fight refused the table."));
		return;
	}
	const ftcore::ArenaSpecialRules* Mine = ArenaSpecials.forLead(Fighter);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: specials: %s (move %s), partner %s."), UTF8_TO_TCHAR(Fighter.c_str()),
		Mine ? UTF8_TO_TCHAR(std::string(Mine->idView()).c_str()) : TEXT("none"), Partner.empty() ? TEXT("none") : UTF8_TO_TCHAR(Partner.c_str()));
}

bool UFTArenaRunner::LoadSpecialClip()
{
	if (bSpecialOn)
	{
		return SpecialClip != nullptr; // not mid-move (Core refuses a lead switch while frozen)
	}
	PlaySpecialClip(false);
	SpecialClip = nullptr;
	SpecialClipSought.Reset();
	SpecialClipFor.Reset();
	const ftcore::ArenaSpecialRules* Rules =
		bArenaSpecials && PlayerLook ? ArenaSpecials.forLead(std::string(TCHAR_TO_UTF8(*PlayerLook->Name.ToLower()))) : nullptr;
	const ftcore::SpecialCineMove* Move = Rules ? SpecialTable.find(std::string(Rules->idView())) : nullptr;
	if (!Move || Move->empty() || Move->clip().empty())
	{
		SpecialClipFor = PlayerLook ? PlayerLook->Name : FString(); // no move or no shots: nothing to find
		return false;
	}
	const AFTArenaFighter* Lead = PlayerFigure.Get();
	const USkeletalMeshComponent* Skin = Lead ? Lead->GetLeadSkin() : nullptr;
	const USkeletalMesh* Body = Skin ? Skin->GetSkeletalMeshAsset() : nullptr;
	if (!Body)
	{
		// Left unloaded: the next press tries again.
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: special: %s wears no body yet: its clip is looked for at the press."),
			PlayerLook ? *PlayerLook->Name : TEXT("the lead"));
		return false;
	}
	SpecialClipFor = PlayerLook->Name;
	// From the worn body's folder up to the lead's own (/Game/Characters/<Name>): <folder>/Anims/Special/<clip>, then
	// <folder>/Anims/<clip>. Loaded here, at the fight's begin or a lead switch, never at the press.
	const FString Name = UTF8_TO_TCHAR(std::string(Move->clip()).c_str());
	const FString Root = FString::Printf(TEXT("/Game/Characters/%s"), *SpecialClipFor);
	FString Folder = FPackageName::GetLongPackagePath(Body->GetPackage()->GetName());
	FString Found;
	for (int32 Depth = 0; Depth < 8 && !SpecialClip && Folder.StartsWith(Root, ESearchCase::IgnoreCase); ++Depth)
	{
		for (const TCHAR* Sub : {TEXT("/Anims/Special/"), TEXT("/Anims/")})
		{
			const FString Package = Folder + Sub + Name;
			if (!FPackageName::DoesPackageExist(Package))
			{
				continue;
			}
			UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *(Package + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Clip && Clip->GetPlayLength() > 0.f && UFTLocomotionAnimInstance::FitsBody(*Clip, *Body, TEXT("Arena: special")))
			{
				SpecialClip = Clip;
				Found = Package;
				break;
			}
		}
		if (Folder.Equals(Root, ESearchCase::IgnoreCase))
		{
			break;
		}
		Folder = FPaths::GetPath(Folder);
	}
	if (!SpecialClip)
	{
		SpecialClipSought = FString::Printf(TEXT("%s under %s"), *Name, *Root);
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: special: %s: no clip %s under %s for the worn body: the press is dropped%s."),
			*SpecialClipFor, *Name, *Root, bSpecialClipStandIn ? TEXT(" (but -ArenaSpecialNoClip: it plays with the lead's own pose)") : TEXT(""));
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: special: %s plays %s (%.2f s; the shots run %.2f s)."), *SpecialClipFor, *Found,
		SpecialClip->GetPlayLength(), Move->length());
	return true;
}

void UFTArenaRunner::PlaySpecialClip(bool bOn)
{
	const AFTArenaFighter* Lead = PlayerFigure.Get();
	USkeletalMeshComponent* Skin = Lead ? Lead->GetLeadSkin() : nullptr;
	UFTLocomotionAnimInstance* Anim = Skin ? Cast<UFTLocomotionAnimInstance>(Skin->GetAnimInstance()) : nullptr;
	if (!Anim)
	{
		bSpecialClipPlaying = false;
		return;
	}
	if (bOn && SpecialClip)
	{
		bSpecialClipPlaying = Anim->SetActionClip(UFTLocomotionAnimInstance::SpecialActionSlot, SpecialClip);
		return;
	}
	if (bSpecialClipPlaying || Anim->GetActionClip(UFTLocomotionAnimInstance::SpecialActionSlot))
	{
		Anim->SetActionClip(UFTLocomotionAnimInstance::SpecialActionSlot, nullptr);
		Anim->ClearActions(); // the lead's weapons paint this frame's own next frame
	}
	bSpecialClipPlaying = false;
}

ftcore::SpecialCineStage UFTArenaRunner::SpecialStage() const
{
	ftcore::SpecialCineStage Stage = SpecialStageAt;
	if (const AFTArenaFighter* Lead = PlayerFigure.Get())
	{
		if (const USkeletalMeshComponent* Skin = Lead->GetLeadSkin(); Skin && Skin->GetBoneIndex(kSpecialBone) != INDEX_NONE)
		{
			const FVector Bone = Skin->GetBoneLocation(kSpecialBone) / 100.0;
			Stage.bone = {static_cast<float>(Bone.X), static_cast<float>(Bone.Y), static_cast<float>(Bone.Z)};
		}
		else
		{
			const FVector Feet = Lead->GetActorLocation() / 100.0; // no hand: the chest
			Stage.bone = {static_cast<float>(Feet.X), static_cast<float>(Feet.Y), static_cast<float>(Feet.Z) + 1.2f};
		}
	}
	return Stage;
}

void UFTArenaRunner::ClampSpecialTravel(const ftcore::Vec2& From, const ftcore::Vec2& Forward, const ftcore::CutsceneVec3& EndFeet,
	int32 ContactTarget)
{
	SpecialContact = ftcore::ArenaSpecialContact();
	SpecialContactKept = TEXT("no solve");
	const ftcore::Vec2 Right{-Forward.y, Forward.x};
	const ftcore::Vec2 Move{Forward.x * EndFeet.x + Right.x * EndFeet.y, Forward.y * EndFeet.x + Right.y * EndFeet.y};
	const float Length = FMath::Sqrt(Move.x * Move.x + Move.y * Move.y);
	const float Radius = Fight.def().fighter.bodyRadius;
	SpecialBack = 0.f;
	SpecialTravel = 1.f;
	SpecialLimit = TEXT("none");
	SpecialEnd = {From.x + Move.x, From.y + Move.y};
	if (Length < 0.01f)
	{
		return;
	}
	const ftcore::Vec2 Dir{Move.x / Length, Move.y / Length};
	// A point the lead may stand on: inside the ring, clear of every standing body.
	auto Clear = [&](const ftcore::Vec2& P, const TCHAR*& Why) {
		if (!Fight.ring().contains(P, Radius))
		{
			Why = TEXT("the ring's edge");
			return false;
		}
		const int32 Count = FMath::Min(static_cast<int32>(Fight.def().enemyCount), static_cast<int32>(MaxEnemies));
		for (int32 I = 0; I < Count; ++I)
		{
			if (Fight.enemyHp(static_cast<size_t>(I)) <= 0)
			{
				continue;
			}
			const ftcore::Vec2 E = Bodies.enemies[static_cast<size_t>(I)];
			const float Need = Radius + Fight.def().enemies[static_cast<size_t>(I)].bodyRadius;
			if ((P.x - E.x) * (P.x - E.x) + (P.y - E.y) * (P.y - E.y) < Need * Need)
			{
				Why = TEXT("an enemy's body");
				return false;
			}
		}
		return true;
	};
	// The static world: the lead's capsule swept along the ground, lifted clear of the floor.
	UWorld* World = GetWorld();
	const float FeetZ = PlayerFigure ? static_cast<float>(PlayerFigure->GetActorLocation().Z) : 0.f;
	auto Sweep = [&](const ftcore::Vec2& A, const ftcore::Vec2& B) {
		if (!World)
		{
			return 1.f;
		}
		constexpr float HalfHeight = 80.f, Lift = 15.f;
		const FVector Up(0.0, 0.0, FeetZ + HalfHeight + Lift);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(FTSpecialTravel), false);
		if (PlayerFigure)
		{
			Params.AddIgnoredActor(PlayerFigure);
		}
		FHitResult Hit;
		const bool bHit = World->SweepSingleByObjectType(Hit, FVector(A.x * 100.f, A.y * 100.f, 0.f) + Up, FVector(B.x * 100.f, B.y * 100.f, 0.f) + Up,
			FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic), FCollisionShape::MakeCapsule(Radius * 100.f, HalfHeight), Params);
		return bHit ? Hit.Time : 1.f;
	};
	// The share of the path that is clear, from From toward Dir over Dist (in steps of 5 cm, then the sweep).
	auto ClearShare = [&](const ftcore::Vec2& A, const ftcore::Vec2& D, float Dist, const TCHAR*& Why) {
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Dist / 0.05f));
		float Share = 1.f;
		for (int32 K = 1; K <= Steps; ++K)
		{
			const float U = static_cast<float>(K) / static_cast<float>(Steps);
			if (!Clear({A.x + D.x * Dist * U, A.y + D.y * Dist * U}, Why))
			{
				Share = static_cast<float>(K - 1) / static_cast<float>(Steps);
				break;
			}
		}
		const float Static = Sweep(A, {A.x + D.x * Dist * Share, A.y + D.y * Dist * Share});
		if (Static < 1.f)
		{
			Why = TEXT("the static world");
			Share *= Static;
		}
		return Share;
	};
	const ftcore::Vec2 Back{-Forward.x, -Forward.y};
	[&]() {
		const TCHAR* Why = TEXT("none");
		const float Share = ClearShare(From, Dir, Length, Why);
		if (Share >= 0.999f)
		{
			return;
		}
		SpecialLimit = Why;
		// Room behind for the missing part: the performer starts back by it (along Core's way, -Forward) and travels the
		// whole way from there, if that path is clear too.
		const float Missing = FMath::Min((1.f - Share) * Length + 0.02f, ftcore::kArenaSpecialMaxBack);
		const TCHAR* BackWhy = TEXT("none");
		const ftcore::Vec2 Start{From.x - Forward.x * Missing, From.y - Forward.y * Missing};
		if (ClearShare(From, Back, Missing, BackWhy) >= 0.999f && ClearShare(Start, Dir, Length, BackWhy) >= 0.999f)
		{
			SpecialBack = Missing;
			SpecialEnd = {Start.x + Move.x, Start.y + Move.y};
			return;
		}
		SpecialTravel = FMath::Max(0.f, Share - 0.01f); // a hair short of the obstacle
		SpecialEnd = {From.x + Move.x * SpecialTravel, From.y + Move.y * SpecialTravel};
	}();
	// The fist at the contact: a start further back when the clip's fist would be inside the target's body there (the
	// whole way travelled only: a shortened one moves the body, not the clip's reach), if that room is clear too.
	const int32 Count = FMath::Min(static_cast<int32>(Fight.def().enemyCount), static_cast<int32>(MaxEnemies));
	if (ContactTarget < 0 || ContactTarget >= Count || !bSpecialReach || SpecialTravel < 0.999f)
	{
		SpecialContactKept = ContactTarget < 0 ? TEXT("no target") : !bSpecialReach ? TEXT("no clip reach") : TEXT("a shortened way");
		return;
	}
	const ftcore::Vec2 E = Bodies.enemies[static_cast<size_t>(ContactTarget)];
	const ftcore::Vec2 To{E.x - From.x, E.y - From.y};
	SpecialContact = ftcore::arenaSpecialContact(To.x * Forward.x + To.y * Forward.y, To.x * Right.x + To.y * Right.y,
		Fight.def().enemies[static_cast<size_t>(ContactTarget)].bodyRadius, SpecialReach, SpecialBack);
	if (!SpecialContact.moved)
	{
		SpecialContactKept = SpecialContact.inside() ? TEXT("inside, no room") : TEXT("not inside");
		return;
	}
	const TCHAR* ContactWhy = TEXT("none");
	const ftcore::Vec2 Start{From.x - Forward.x * SpecialContact.back, From.y - Forward.y * SpecialContact.back};
	if (ClearShare(From, Back, SpecialContact.back, ContactWhy) >= 0.999f && ClearShare(Start, Dir, Length, ContactWhy) >= 0.999f)
	{
		SpecialBack = SpecialContact.back;
		SpecialEnd = {Start.x + Move.x, Start.y + Move.y};
		SpecialContactKept = TEXT("moved back");
		return;
	}
	SpecialContactKept = ContactWhy; // no room behind: the clamp's start stands
}

void UFTArenaRunner::SampleSpecialReach(const ftcore::ArenaSpecialRules& Rules, float Distance)
{
	bSpecialReach = false;
	const AFTArenaFighter* Lead = PlayerFigure.Get();
	const USkeletalMeshComponent* Skin = Lead ? Lead->GetLeadSkin() : nullptr;
	if (!SpecialClip || !Skin)
	{
		return;
	}
	// The clip plays on the shots' clock (TickSpecial) with the lead's actor at the start feet: its fist at the hit,
	// through the mesh's place on the actor, is the reach from there (the clip's own travel in it).
	SpecialReachAt = FMath::Clamp(SpecialHitTime(Rules, ftcore::arenaSpecialImpactAt(Rules, Distance)), 0.f, SpecialClip->GetPlayLength());
	const FTransform ToActor = Skin->GetComponentTransform().GetRelativeTransform(Lead->GetActorTransform());
	double Best = -1.0e9;
	for (int32 Side = 0; Side < 2; ++Side)
	{
		FVector Pad;
		if (!SpecialClipPad(*SpecialClip, Side, SpecialReachAt, Pad))
		{
			continue;
		}
		const FVector Local = ToActor.TransformPosition(Pad) / 100.0;
		if (Local.X > Best)
		{
			Best = Local.X;
			SpecialReach = {static_cast<float>(Local.X), static_cast<float>(Local.Y)};
			SpecialReachSide = Side;
			bSpecialReach = true;
		}
	}
}

void UFTArenaRunner::LogSpecialContact()
{
	bSpecialContactDue = false;
	const AFTArenaFighter* Lead = PlayerFigure.Get();
	const USkeletalMeshComponent* Skin = Lead ? Lead->GetLeadSkin() : nullptr;
	const AFTArenaFighter* Enemy = SpecialTarget >= 0 && SpecialTarget < MaxEnemies ? GetEnemyFigure(SpecialTarget) : nullptr;
	if (!Skin || !Enemy)
	{
		return;
	}
	// The pose drawn now is the one asked for last frame (the clip's time then); the clip's own pad at the hit through
	// the drawn mesh is the contact frame itself.
	const FVector Centre = Enemy->GetActorLocation();
	const float Radius = SpecialStageAt.targetRadius * 100.f;
	FVector Drawn;
	const float DrawnGap = SpecialDrawnPad(*Skin, SpecialReachSide, Drawn) ? static_cast<float>(FVector::Dist2D(Drawn, Centre)) - Radius : std::numeric_limits<float>::quiet_NaN();
	FVector Pad;
	const float ClipGap = SpecialClip && SpecialClipPad(*SpecialClip, SpecialReachSide, SpecialHit.time, Pad)
		? static_cast<float>(FVector::Dist2D(Skin->GetComponentTransform().TransformPosition(Pad), Centre)) - Radius : std::numeric_limits<float>::quiet_NaN();
	const bool bTouch = FMath::IsFinite(ClipGap) && ClipGap >= -ftcore::ArenaSpecialContact::kIn * 100.f && ClipGap <= ftcore::ArenaSpecialContact::kOut * 100.f;
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena check: special contact at %.3f s: the %s fist is %.1f cm off the target's surface (clip at the hit, on the drawn mesh), %.1f cm drawn at %.3f s; touching (-%.0f..+%.0f cm): %s."),
		SpecialHit.time, SpecialReachSide == 0 ? TEXT("left") : TEXT("right"), ClipGap, DrawnGap, SpecialPlayer.time(),
		ftcore::ArenaSpecialContact::kIn * 100.f, ftcore::ArenaSpecialContact::kOut * 100.f, bTouch ? TEXT("yes") : TEXT("no"));
}

void UFTArenaRunner::PressSpecial(ftcore::ArenaInput& In)
{
	// A skip read while frozen (PollSpecialSkip) on the cut back's own step: Core reads it there too.
	In.specialSkip = bSpecialSkip;
	bSpecialSkip = false;
	if (!bSpecialPress)
	{
		return;
	}
	bSpecialPress = false;
	SpecialBack = 0.f;
	SpecialTravel = 1.f;
	SpecialLimit = TEXT("none");
	SpecialTarget = -1;
	const ftcore::ArenaSpecialRules* Rules =
		bArenaSpecials && PlayerLook ? ArenaSpecials.forLead(std::string(TCHAR_TO_UTF8(*PlayerLook->Name.ToLower()))) : nullptr;
	if (Rules && PlayerLook && SpecialClipFor != PlayerLook->Name)
	{
		LoadSpecialClip(); // not looked for yet for this lead (a hitch here, logged by the load)
	}
	const ftcore::SpecialCineMove* Shots = Rules ? SpecialTable.find(std::string(Rules->idView())) : nullptr;
	if (Shots && !Shots->empty() && !SpecialClip && !bSpecialClipStandIn && !Fight.frozen())
	{
		// A shot list over a lead that holds its pose is no special: the press is dropped, the gauge kept. Loud on
		// purpose: one line per dropped press, naming the missing asset, and (not in Shipping) a line on screen.
		const FString Missing = SpecialClipSought.IsEmpty() ? FString(UTF8_TO_TCHAR(std::string(Shots->clip()).c_str())) : SpecialClipSought;
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %.2f: special pressed, dropped: missing asset %s for %s (-ArenaSpecialNoClip plays it posed)."),
			Fight.time(), *Missing, *SpecialClipFor);
#if !UE_BUILD_SHIPPING
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Orange,
				FString::Printf(TEXT("Special dropped: missing asset %s (dev builds only)"), *Missing));
		}
#endif
		return;
	}
	bSpecialPressedNow = true;
	In.special = ftcore::kArenaPlayer;
	if (!Rules)
	{
		return; // Core refuses it (NoMove), logged after the tick
	}
	// Core's target (ArenaFight.cpp, specialTarget): the lock at any range, else the soft target within range (none if
	// it is out of range), else the nearest living enemy in front within range.
	const ftcore::ArenaPlayerOut& P = Fight.output().player;
	const int32 Count = FMath::Min(static_cast<int32>(Fight.def().enemyCount), static_cast<int32>(MaxEnemies));
	auto Alive = [&](int32 I) { return I >= 0 && I < Count && Fight.enemyHp(static_cast<size_t>(I)) > 0; };
	auto Away = [&](int32 I) {
		const ftcore::Vec2 E = Bodies.enemies[static_cast<size_t>(I)];
		return ftcore::Vec2{E.x - Bodies.player.x, E.y - Bodies.player.y};
	};
	auto Within = [&](int32 I) {
		const ftcore::Vec2 D = Away(I);
		return D.x * D.x + D.y * D.y <= Rules->range * Rules->range;
	};
	const int32 Lock = P.lock == ftcore::kArenaNobody ? -1 : static_cast<int32>(P.lock);
	const int32 Soft = P.soft == ftcore::kArenaNobody ? -1 : static_cast<int32>(P.soft);
	int32 Target = -1;
	if (Alive(Lock))
	{
		Target = Lock;
	}
	else if (Alive(Soft))
	{
		Target = Within(Soft) ? Soft : -1;
	}
	else
	{
		float Best = TNumericLimits<float>::Max();
		for (int32 I = 0; I < Count; ++I)
		{
			const ftcore::Vec2 D = Away(I);
			const float Dist = D.x * D.x + D.y * D.y;
			if (Alive(I) && Within(I) && D.x * P.facing.x + D.y * P.facing.y >= 0.f && Dist < Best)
			{
				Best = Dist;
				Target = I;
			}
		}
	}
	if (Target < 0 || Rules->endTravel <= 0.f)
	{
		return; // no target (Core refuses it), or a move that ends where it starts
	}
	SpecialTarget = Target;
	const ftcore::Vec2 To = Away(Target);
	const float Dist = FMath::Sqrt(To.x * To.x + To.y * To.y);
	SpecialForward = Dist > 1.0e-4f ? ftcore::Vec2{To.x / Dist, To.y / Dist} : P.facing;
	SampleSpecialReach(*Rules, Dist);
	ClampSpecialTravel(Bodies.player, SpecialForward, ftcore::CutsceneVec3{Rules->endTravel, Rules->endSide, 0.f}, Target);
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: %.2f: special contact: the %s fist at %.3f s reaches (%.2f, %.2f) m from the start feet%s; enemy %d's surface %.1f cm away at the clamp's back, %.1f cm at back %.2f m (%s; kept back %.2f m)."),
		Fight.time(), SpecialReachSide == 0 ? TEXT("left") : TEXT("right"), SpecialReachAt, SpecialReach.x, SpecialReach.y,
		bSpecialReach ? TEXT("") : TEXT(" (no clip reach)"), Target, SpecialContact.gapBefore * 100.f, SpecialContact.gap * 100.f,
		SpecialContact.back, SpecialContactKept, SpecialBack);
	In.specialBack = SpecialBack;
	In.specialTravel = SpecialTravel * Rules->endTravel;
}

void UFTArenaRunner::PlaceSpecialPerformer(const ftcore::Vec2& At)
{
	// Put there, not walked there: no step drawn between, the feet start over.
	if (Fight.special().performer == ftcore::kArenaAlly)
	{
		Bodies.ally = PrevBodies.ally = At;
		return;
	}
	Bodies.player = PrevBodies.player = At;
	PlayerStepAt = -1;
	if (PlayerFigure)
	{
		PlayerFigure->ResetFeet();
	}
}

bool UFTArenaRunner::AfterSpecialTick(const ftcore::ArenaOutput& Out)
{
	const ftcore::ArenaSpecialView& V = Fight.special();
	bool bStarted = false;
	for (const ftcore::ArenaEvent& E : Out.events)
	{
		const TCHAR* Who = E.source == ftcore::kArenaAlly ? TEXT("the partner") : TEXT("the lead");
		switch (E.type)
		{
		case ftcore::ArenaEventType::SpecialReady:
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: special ready: %s (slot %d)%s."), Fight.time(), Who, E.amount,
				E.move ? TEXT(", its ready line") : TEXT(""));
			break;
		case ftcore::ArenaEventType::SpecialUnready:
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: special no longer shown: %s (slot %d)."), Fight.time(), Who, E.amount);
			break;
		case ftcore::ArenaEventType::SpecialStarted:
			bStarted = true;
#if !UE_BUILD_SHIPPING
			SpecialTestAt = -1.f; // the test press is done
#endif
			SpecialRefusalLogged = -1;
			break;
		case ftcore::ArenaEventType::SpecialEnded:
			bSpecialSkipped = E.amount != 0; // TickSpecial ends the shots with it
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: special cut back%s: %s at (%.2f, %.2f) m, %d result(s)."), Fight.time(),
				E.amount ? TEXT(" (skipped)") : TEXT(""), Who, V.end.x, V.end.y, static_cast<int32>(V.resultCount));
			break;
		default:
			break;
		}
	}
	if (bSpecialPressedNow && !bStarted)
	{
		// NotControlled (the partner's move), NotReady, NoTarget, NoSight, Down, Running, Over, Unhurt or NoMove; each new reason once.
		const ftcore::ArenaSpecialRefusal Why = Fight.specialRefusal(ftcore::kArenaPlayer);
		if (static_cast<int32>(Why) != SpecialRefusalLogged)
		{
			SpecialRefusalLogged = static_cast<int32>(Why);
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: special refused: %s."), Fight.time(), UTF8_TO_TCHAR(ftcore::arenaSpecialRefusalName(Why)));
		}
	}
	bSpecialPressedNow = false;
	if (const ftcore::ArenaSpecialRules* Rules = Fight.specialMove())
	{
		for (std::uint8_t I = 0; I < V.cueCount; ++I)
		{
			if (V.cuesFired[I] < Rules->cueCount)
			{
				UE_LOG(LogForgottenThird, Verbose, TEXT("Arena: special cue %s at %.2f s."), UTF8_TO_TCHAR(Rules->cues[V.cuesFired[I]].name.data()), V.time);
			}
		}
	}
	if (bStarted)
	{
		PlaceSpecialPerformer(V.origin);
		BeginSpecial();
		return true;
	}
	if (V.endedThisTick)
	{
		PlaceSpecialPerformer(V.end);
		bSpecialPlaced = true;
		return true;
	}
	return false;
}

bool UFTArenaRunner::StepFrozen()
{
	const ftcore::ArenaSpecialView& V = Fight.special();
	// The cut back's tick takes the whole step (Core's own test, ArenaFight.cpp specialTick): the results land on it and
	// the fight runs on from it. Every other frozen tick moves nothing but the special's clock: no input, no body.
	if (!V.running || V.time + StepSeconds >= V.duration - kCoreSpecialEps)
	{
		return false;
	}
	PrevBodies = Bodies;
	PrevHeldTime = static_cast<double>(Fight.heldTime());
	ftcore::ArenaInput In; // nothing but the skip
	In.specialSkip = bSpecialSkip;
	bSpecialSkip = false;
	const ftcore::ArenaOutput& Out = Fight.tick(StepSeconds, In, Bodies);
	if (AfterSpecialTick(Out) && V.endedThisTick)
	{
		// Skipped: Core jumped to its end on this step (its results land on it). Else it ended on a tick it was not
		// due (its move gone from the host's table). Its events either way; TickSpecial ends a skipped move's shots.
		if (!In.specialSkip)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: special ended on a frozen step it was not due (%.2f of %.2f s)."), V.time, V.duration);
		}
		HandleEvents(Out.events);
	}
	return true;
}

void UFTArenaRunner::PollSpecialSkip()
{
	const APlayerController* PC = GetPC();
	using ftcore::InputAction;
	// Not the press that closed the pause menu (Interact or its own key resumes it).
	const bool bPressed = bSpecialPress || (PC && !bAutoPlay && !UFTPauseSubsystem::IsBlocking(this) && !UFTPauseSubsystem::IsJustResumed(this)
		&& (FTInputBindings::WasJustPressed(*PC, InputAction::Special) || FTInputBindings::WasJustPressed(*PC, InputAction::Interact)));
	bSpecialPress = false; // a press while one plays is a skip, never a second special
	if (!bPressed)
	{
		return;
	}
	if (Fight.frozen())
	{
		if (Fight.special().skippable)
		{
			bSpecialSkip = true; // sent on the next step
			return;
		}
		if (!bSpecialSkipRefusedLogged)
		{
			bSpecialSkipRefusedLogged = true;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: special skip refused: the move has not been seen to its end yet."), Fight.time());
		}
		return;
	}
	if (bSpecialOn)
	{
		EndSpecial(TEXT("skipped in the tail")); // Core has cut back: only the shots' blend out is left
	}
}

// Each shot tested once at the start (the world is frozen) by Core's framing check (ftcore::specialCineFixShot): its
// camera inside a body or the static world, a body or the world in its line to the subject, or the subject out of the
// safe frame or too small, or the contact or most of the lead hidden behind a body. A failing shot gets the first fix
// that passes (mirrored, raised, aimed, pulled, then orbited about the subject; Core's order); none passes: it plays as
// written and is logged as still failing. The bodies are Core's upright cylinders: the living enemies
// (the target marked) and the partner; the lead is the subject, not a body.
uint32 UFTArenaRunner::SpecialCineBodies(int32 Target, ftcore::SpecialCineBody* List)
{
	uint32 Count = 0;
	const int32 Enemies = FMath::Min(static_cast<int32>(Fight.def().enemyCount), static_cast<int32>(MaxEnemies));
	for (int32 I = 0; I < Enemies; ++I)
	{
		const AFTArenaFighter* Figure = GetEnemyFigure(I);
		if (!Figure || Fight.enemyHp(static_cast<size_t>(I)) <= 0)
		{
			continue;
		}
		const ftcore::Vec2 E = Bodies.enemies[static_cast<size_t>(I)];
		ftcore::SpecialCineBody& B = List[Count++];
		B.feet = {E.x, E.y, static_cast<float>(Figure->GetActorLocation().Z / 100.0)};
		B.radius = Fight.def().enemies[static_cast<size_t>(I)].bodyRadius;
		B.height = FMath::Max(0.5f, Figure->GetHeight() / 100.f);
		B.target = I == Target;
	}
	if (const AFTArenaFighter* Ally = AllyFigure.Get(); Ally && bPartner && Fight.allyHp() > 0)
	{
		ftcore::SpecialCineBody& B = List[Count++];
		B.feet = {Bodies.ally.x, Bodies.ally.y, static_cast<float>(Ally->GetActorLocation().Z / 100.0)};
		B.radius = Fight.def().fighter.bodyRadius;
		B.height = FMath::Max(0.5f, Ally->GetHeight() / 100.f);
	}
	return Count;
}

void UFTArenaRunner::SpecialCineFixShots(const ftcore::SpecialCineMove& Move, int32 Target)
{
	SpecialShotsMoved = SpecialShotsBlocked = SpecialShotsSkipped = 0;
	ftcore::SpecialCineStage Stage = SpecialStageAt;
	Stage.bone = {Stage.origin.x, Stage.origin.y, Stage.origin.z + 1.2f}; // the hand's not known ahead: the chest
	ftcore::SpecialCineBody List[MaxEnemies + 1];
	const uint32 Count = SpecialCineBodies(Target, List);
	// The static world only: the figures are tested as the cylinders above.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FTSpecialShots), false);
	for (const AActor* Figure : {static_cast<const AActor*>(PlayerFigure.Get()), static_cast<const AActor*>(AllyFigure.Get())})
	{
		if (Figure)
		{
			Params.AddIgnoredActor(Figure);
		}
	}
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		if (const AActor* Figure = GetEnemyFigure(I))
		{
			Params.AddIgnoredActor(Figure);
		}
	}
	FSpecialWorld World{GetWorld(), &Params};
	const ftcore::SpecialCineWorldTest Test = World.World ? &SpecialWorldBlocked : nullptr;
	// Two passes: every shot's fix first, then a cover (the shot next to it played over its window) for a shot no fix
	// frames, tested against the fixes of the shots around it. A shot neither frames is played as written and logged.
	ftcore::SpecialCineShotCheck Blocked[ftcore::SpecialCineMove::kMaxShots];
	uint32 BlockedShot[ftcore::SpecialCineMove::kMaxShots];
	uint32 BlockedCount = 0;
	for (uint32 Shot = 0; Shot < Move.size(); ++Shot)
	{
		const ftcore::SpecialCineShotCheck Check = ftcore::specialCineFixShot(Move, Shot, Stage, List, Count, Test, &World);
		if (Check.first == ftcore::SpecialCineProblem::None)
		{
			continue;
		}
		const ftcore::SpecialCineShot& S = Move.shot(Shot);
		const FString First = UTF8_TO_TCHAR(std::string(ftcore::specialCineProblemName(Check.first)).c_str());
		if (!Check.ok)
		{
			Blocked[BlockedCount] = Check;
			BlockedShot[BlockedCount++] = Shot;
			continue;
		}
		const ftcore::SpecialCineShotFix& Fix = Check.fix;
		SpecialPlayer.setFix(Shot, Fix);
		++SpecialShotsMoved;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: special shot %u (%.2f-%.2f s): %s at %.2f s: moved (try %u, %u refused as like another shot): %s%sorbited %.0f deg, raised %.1f m, pulled %.1f m."),
			Shot + 1, S.in, S.out, *First, Check.firstAt, Check.tries, Check.alike, Fix.mirror ? TEXT("mirrored, ") : TEXT(""),
			Fix.aim ? TEXT("aimed, ") : TEXT(""), FMath::IsFinite(Fix.orbit) ? Fix.orbit : 0.f, Fix.raise > 0.f ? Fix.raise : 0.f,
			Fix.pull > 0.f ? Fix.pull : 0.f);
	}
	for (uint32 K = 0; K < BlockedCount; ++K)
	{
		const uint32 Shot = BlockedShot[K];
		const ftcore::SpecialCineShotCheck& Check = Blocked[K];
		const ftcore::SpecialCineShot& S = Move.shot(Shot);
		const FString First = UTF8_TO_TCHAR(std::string(ftcore::specialCineProblemName(Check.first)).c_str());
		const FString Last = UTF8_TO_TCHAR(std::string(ftcore::specialCineProblemName(Check.last)).c_str());
		const ftcore::SpecialCineCover Cover = ftcore::specialCineCoverShot(Move, Shot, Stage, SpecialPlayer.fixes(), List, Count, Test, &World);
		ftcore::SpecialCineShotFix Skip;
		Skip.cover = Cover;
		if (Cover != ftcore::SpecialCineCover::None && SpecialPlayer.setFix(Shot, Skip))
		{
			++SpecialShotsSkipped;
			const uint32 Shown = ftcore::specialCineShownShot(Move, Shot, SpecialPlayer.fixes());
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: special shot %u (%.2f-%.2f s): %s at %.2f s; no fix of %u clears it (last: %s; %u like another shot): skipped, shot %u plays over its window (%s)."),
				Shot + 1, S.in, S.out, *First, Check.firstAt, Check.tries, *Last, Check.alike, Shown + 1,
				Cover == ftcore::SpecialCineCover::Next ? TEXT("starting early") : TEXT("held on"));
			continue;
		}
		++SpecialShotsBlocked;
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: special shot %u (%.2f-%.2f s): %s at %.2f s; no fix of %u clears it (last: %s; %u like another shot) and no shot next to it covers it: played as written."),
			Shot + 1, S.in, S.out, *First, Check.firstAt, Check.tries, *Last, Check.alike);
	}
	// The cut list as it will play: each shown shot with its span, and the shots it plays over.
	ftcore::SpecialCineCut Cuts[ftcore::SpecialCineMove::kMaxShots];
	const uint32 CutCount = FMath::Min(ftcore::specialCineCutList(Move, SpecialPlayer.fixes(), Cuts, ftcore::SpecialCineMove::kMaxShots),
		static_cast<uint32>(ftcore::SpecialCineMove::kMaxShots));
	FString CutText;
	for (uint32 C = 0; C < CutCount; ++C)
	{
		CutText += FString::Printf(TEXT("%s%u %.2f-%.2f"), C ? TEXT(", ") : TEXT(""), Cuts[C].shot + 1, Cuts[C].in, Cuts[C].out);
		if (Cuts[C].skipped)
		{
			// The shots it plays over, by number.
			CutText += TEXT(" (skips");
			for (uint32 T = 0; T < Move.size(); ++T)
			{
				if (T != Cuts[C].shot && ftcore::specialCineShownShot(Move, T, SpecialPlayer.fixes()) == Cuts[C].shot)
				{
					CutText += FString::Printf(TEXT(" %u"), T + 1);
				}
			}
			CutText += TEXT(")");
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: special cut list: %u cuts for %u shots, %d skipped: %s."), CutCount, Move.size(),
		SpecialShotsSkipped, *CutText);
}

bool UFTArenaRunner::BeginSpecial()
{
	const ftcore::ArenaSpecialView& V = Fight.special();
	const ftcore::ArenaSpecialRules* Rules = Fight.specialMove();
	const std::string MoveId = Rules ? std::string(Rules->idView()) : std::string();
	const FString Id = UTF8_TO_TCHAR(MoveId.c_str());
	const ftcore::SpecialCineMove* Move = SpecialTable.find(MoveId); // the far set; the near one is picked below
	const int32 Target = static_cast<int32>(V.target);
	const bool bSameTarget = SpecialTarget == Target; // the press's clamp was for Core's target
	const AFTArenaFighter* Enemy = Target < MaxEnemies ? GetEnemyFigure(Target) : nullptr;
	const AFTArenaFighter* Performer = V.performer == ftcore::kArenaAlly ? nullptr : PlayerFigure.Get();
	if (bSpecialOn || !Move || Move->empty() || !Performer || !Enemy)
	{
		// Core runs it all the same (frozen, its results at the cut back) under the play camera.
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %.2f: special %s runs with no shots (%s)."), Fight.time(), *Id,
			bSpecialOn ? TEXT("one is playing") : !Move || Move->empty() ? TEXT("no shots read for it")
			: !Performer ? TEXT("the partner performs it") : TEXT("no target figure"));
		return false;
	}
	const double Began = FPlatformTime::Seconds();
	if (!bSameTarget && Rules)
	{
		// Core took another target than the press's guess: the clear room measured again along Core's way, from
		// Core's start (Core's end stands; the log below says how far it is from the clear one).
		ClampSpecialTravel(V.origin, V.dir, ftcore::CutsceneVec3{Rules->endTravel, Rules->endSide, 0.f});
	}
	SpecialTarget = Target;
	SpecialForward = V.dir;
	bSpecialSkip = bSpecialSkipped = bSpecialSkipRefusedLogged = false;
	const ftcore::Vec2 To = Bodies.enemies[static_cast<size_t>(Target)];
	const float Dist = FMath::Sqrt((To.x - V.origin.x) * (To.x - V.origin.x) + (To.y - V.origin.y) * (To.y - V.origin.y));
	const float FeetZ = static_cast<float>(Performer->GetActorLocation().Z / 100.0);
	SpecialStageAt = ftcore::SpecialCineStage();
	SpecialStageAt.origin = {V.origin.x, V.origin.y, FeetZ};
	SpecialStageAt.forward = {V.dir.x, V.dir.y, 0.f};
	SpecialStageAt.target = {To.x, To.y, static_cast<float>(Enemy->GetActorLocation().Z / 100.0)};
	SpecialStageAt.targetHeight = FMath::Max(0.5f, Enemy->GetHeight() / 100.f);
	SpecialStageAt.targetRadius = Fight.def().enemies[static_cast<size_t>(Target)].bodyRadius;
	SpecialStageAt.leadHeight = FMath::Max(0.5f, Performer->GetHeight() / 100.f);
	SpecialStageAt.travel = Rules && Rules->endTravel > 0.f ? FMath::Clamp(V.travel / Rules->endTravel, 0.f, 1.f) : 1.f;
	// The near set inside its range, else the far (SpecialCineTable::find).
	const float Range = ftcore::specialCineDistance(SpecialStageAt);
	const ftcore::SpecialCineMove* Far = Move;
	Move = SpecialTable.find(MoveId, Range);
	if (!Move || Move->empty())
	{
		Move = Far;
	}
	const TCHAR* Set = Move == Far ? TEXT("far") : TEXT("near");
	// The hit: the impact row's time (the impact itself when the move has none), at the contact the framing tests.
	const TCHAR* HitRow = TEXT("the impact");
	const float HitAt = Rules ? SpecialHitTime(*Rules, V.impactTime, &HitRow) : V.impactTime;
	bSpecialHitPlayed = false;
	bSpecialContactDue = false;
	bSpecialHitProbed = false;
	SpecialHitPlayedWall = SpecialHitPlayedWorld = SpecialHitHeldFrom = -1.0;
	SpecialHitHeldFor = 0.f;
	SpecialHitUnseen = 0;
	// Nothing paused on the performer, nothing shaking the camera.
	LeadWeapons.HitPause(false);
	Jolt = ftcore::ArenaJolt();
	SpecialFreeze.Begin(GetWorld(), PlayerFigure, kSpecialFreeze);
	if (UWorld* World = GetWorld())
	{
		// The rain is no actor the freeze holds: slowed here, given back at the end.
		if (UFTRainSubsystem* Rain = World->GetSubsystem<UFTRainSubsystem>())
		{
			Rain->SetTimeScale(kSpecialRain);
		}
	}
	APlayerController* PC = GetPC();
	if (APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr; Camera && !SpecialCamera)
	{
		SpecialCamera = Cast<UFTSpecialCineModifier>(Camera->AddNewCameraModifier(UFTSpecialCineModifier::StaticClass()));
	}
	SpecialPlayer.begin(Move);
	const double FixFrom = FPlatformTime::Seconds();
	SpecialCineFixShots(*Move, Target); // after the begin (it clears the fixes)
	const double FixMs = (FPlatformTime::Seconds() - FixFrom) * 1000.0;
	// After the fixes: a skipped shot's window shows the shot over it, and the hit's shot is the one shown.
	SpecialHit = ftcore::specialCineHit(*Move, SpecialStageAt, HitAt, SpecialStageAt.targetRadius, SpecialPlayer.fixes());
	// Whether the contact reads in the shot shown at the hit: a count for the log (the fix search does not use it); a
	// hit out of frame or hidden is a shot still failing.
	ftcore::SpecialCineProblem HitSeen = ftcore::SpecialCineProblem::None;
	{
		ftcore::SpecialCineBody Seen[MaxEnemies + 1];
		const uint32 SeenCount = SpecialCineBodies(Target, Seen);
		HitSeen = ftcore::specialCineHitProblem(*Move, SpecialStageAt, SpecialHit, SpecialPlayer.fixes(), Seen, SeenCount);
		SpecialHitUnseen = HitSeen != ftcore::SpecialCineProblem::None ? 1 : 0;
	}
	// The hit's effect made now, ahead of the hit: its pipeline states compile over the shots before it (the engine
	// draws nothing of an effect whose states are still compiling). -ArenaSpecialHitLate makes it at the hit instead.
	if (bSpecialHitFx && !bSpecialHitLate)
	{
		SpecialHitFxOn(PlayerFigure.Get(), TEXT("the special's start"));
	}
	PlaySpecialClip(true);
	bSpecialOn = true;
	bSpecialPlaced = false;
	// The play camera under the blend out, moved to the end while the lead still stands at the start (Core's cut back
	// after the shots' release); once Core has put the lead there it needs no move.
	SpecialPlayShift = FVector((V.end.x - V.origin.x) * 100.f, (V.end.y - V.origin.y) * 100.f, 0.f);
	SpecialBeganAt = Now;
	SpecialBeginMs = (FPlatformTime::Seconds() - Began) * 1000.0;
#if !UE_BUILD_SHIPPING
	SpecialFrames = SpecialCuts = SpecialAfterFrames = 0;
	SpecialStepShots = SpecialStepOut = SpecialStepAfter = 0.f;
	SpecialEnemyDrift = 0.f;
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		const AFTArenaFighter* Figure = GetEnemyFigure(I);
		SpecialEnemyFrom[I] = Figure ? Figure->GetActorLocation() : FVector::ZeroVector;
	}
	// The stills: each shot's middle and just past the release.
	SpecialStillCount = SpecialStillNext = 0;
	SpecialHitStills = 0;
	if (bSpecialShots)
	{
		// The hit's stills (its first frames to its fade) kept room for first, with the release's.
		SpecialHitStills = ftcore::specialCineHitStills(*Move, SpecialHit, SpecialHitStillAt, ftcore::SpecialCineHit::kStills);
		const int32 Room = static_cast<int32>(UE_ARRAY_COUNT(SpecialStillAt)) - 1 - static_cast<int32>(SpecialHitStills);
		for (uint32 I = 0; I < Move->size() && SpecialStillCount < Room; ++I)
		{
			SpecialStillAt[SpecialStillCount++] = 0.5f * (Move->shot(I).in + Move->shot(I).out);
		}
		SpecialStillAt[SpecialStillCount++] = Move->release() + 0.05f;
		for (uint32 K = 0; K < SpecialHitStills; ++K)
		{
			SpecialStillAt[SpecialStillCount++] = SpecialHitStillAt[K];
		}
		Algo::Sort(MakeArrayView(SpecialStillAt, SpecialStillCount));
	}
#endif
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: special %s: the hit at %.3f s (%s; Core's impact %.3f s), shot %u, its still at %.3f s; contact (%.2f, %.2f, %.2f) m%s."),
		*Id, SpecialHit.time, HitRow, V.impactTime, SpecialHit.shot + 1, SpecialHit.stillAt, SpecialHit.at.x, SpecialHit.at.y, SpecialHit.at.z,
		bSpecialHitFx ? TEXT("") : TEXT(" (-ArenaSpecialNoHitFx: no effect)"));
	if (HitSeen != ftcore::SpecialCineProblem::None)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: special %s: the hit's contact does not read in shot %u, the one shown at %.3f s (%s): counted as a shot still failing."),
			*Id, SpecialHit.shot + 1, SpecialHit.time, UTF8_TO_TCHAR(std::string(ftcore::specialCineProblemName(HitSeen)).c_str()));
	}
	const float Off = FMath::Sqrt((V.end.x - SpecialEnd.x) * (V.end.x - SpecialEnd.x) + (V.end.y - SpecialEnd.y) * (V.end.y - SpecialEnd.y));
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: %.2f: special %s begins on enemy %d (%.2f m off, radius %.2f, height %.2f m): the %s set (%.2f m, near within %.2f m), %u shots (%d moved, %d skipped, %d still failing; search %.2f ms), %.2f s (Core %.2f s); ")
		TEXT("froze %d actors, %d effects, %d sounds, %d bodies, %d cloths in %.2f ms. ")
		TEXT("Clamp%s: travel %.2f m (%.0f%%), back %.2f m (limited by %s); Core: from (%.2f, %.2f) to (%.2f, %.2f) m (travel share %.2f), %.2f m from the clamped end."),
		Fight.time(), *Id, Target, Dist, SpecialStageAt.targetRadius, SpecialStageAt.targetHeight, Set, Range, Far->nearWithin(), Move->size(), SpecialShotsMoved,
		SpecialShotsSkipped, SpecialShotsBlocked + SpecialHitUnseen, FixMs, Move->length(), V.duration, SpecialFreeze.ActorCount(), SpecialFreeze.EffectCount(), SpecialFreeze.SoundCount(),
		SpecialFreeze.BodyCount(), SpecialFreeze.ClothCount(), SpecialBeginMs, bSameTarget ? TEXT("") : TEXT(" (again, for Core's target)"),
		V.travel, SpecialTravel * 100.f, SpecialBack, SpecialLimit, V.origin.x, V.origin.y, V.end.x, V.end.y, SpecialStageAt.travel, Off);
	if (Off > 0.05f)
	{
		// Core fixed its end at the press; the host can only say so (a corrected travel would need Core to take one now).
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: special %s: Core's end is %.2f m from the clear end along its way (%s; limited by %s)."),
			*Id, Off, bSameTarget ? TEXT("the press's target") : TEXT("Core took another target"), SpecialLimit);
	}
	if (SpecialClip)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: special %s: the lead plays %s (%.2f s) on the shots' clock."), *Id, *SpecialClip->GetName(),
			SpecialClip->GetPlayLength());
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: special %s: no clip %s (-ArenaSpecialNoClip): the lead holds its own pose."), *Id,
			UTF8_TO_TCHAR(std::string(Move->clip()).c_str()));
	}
	return true;
}

void UFTArenaRunner::TickSpecial(float DeltaSeconds)
{
	if (!bSpecialOn)
	{
#if !UE_BUILD_SHIPPING
		if (SpecialTestAt >= 0.f && Fight.time() >= SpecialTestAt && !Fight.over() && !Fight.frozen())
		{
			// Pressed each step until it starts (a hit stun, the air or no target in front refuses one tick), given up after 2 s.
			if (SpecialTestUntil < 0.f)
			{
				SpecialTestUntil = Fight.time() + 2.f;
				if (bArenaSpecials)
				{
					Fight.setSpecialGaugeForTest(0, ArenaSpecials.gauge.full); // both kit slots: whichever the lead drives
					Fight.setSpecialGaugeForTest(1, ArenaSpecials.gauge.full);
				}
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: -ArenaSpecialTest: the gauges full, the special pressed."), Fight.time());
			}
			if (Fight.time() > SpecialTestUntil)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("Arena: %.2f: -ArenaSpecialTest: the special never started in 2 s: given up (%s)."), Fight.time(),
					UTF8_TO_TCHAR(ftcore::arenaSpecialRefusalName(Fight.specialRefusal(ftcore::kArenaPlayer))));
				SpecialTestAt = -1.f;
			}
			else
			{
				RequestSpecial();
			}
		}
		// The first frames after the hand-back: the drawn eye's step.
		APlayerController* PC = GetPC();
		const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr;
		if (Camera)
		{
			const FVector Eye = Camera->GetCameraLocation();
			if (bSpecialEyeWas && DeltaSeconds > 0.f)
			{
				const float Step = static_cast<float>(FVector::Dist(Eye, SpecialEyeWas));
				if (SpecialEndedAt >= 0.0 && SpecialAfterFrames < kSpecialAfterFrames)
				{
					SpecialStepAfter = FMath::Max(SpecialStepAfter, Step);
					if (++SpecialAfterFrames == kSpecialAfterFrames)
					{
						UE_LOG(LogForgottenThird, Display, TEXT("Arena check: special hand-back: the eye's worst step over the %d frames after was %.1f cm (the play camera's own worst over the fight's first 30 frames %.1f cm)."),
							kSpecialAfterFrames, SpecialStepAfter, SpecialStepPlay);
					}
				}
				else if (SpecialBeganAt < 0.0)
				{
					SpecialStepPlay = SpecialFrames++ < 30 ? FMath::Max(SpecialStepPlay, Step) : SpecialStepPlay;
				}
			}
			SpecialEyeWas = Eye;
			bSpecialEyeWas = true;
		}
#endif
		return;
	}
	if (bSpecialSkipped && !Fight.frozen())
	{
		EndSpecial(TEXT("skipped")); // Core cut to its end: the shots go with it
		return;
	}
	if (bSpecialContactDue)
	{
		LogSpecialContact(); // before the step: the pose drawn is the hit frame's
	}
	const double Began = FPlatformTime::Seconds();
	SpecialPlayer.step(DeltaSeconds);
	if (!bSpecialHitPlayed && SpecialPlayer.time() >= SpecialHit.time)
	{
		PlaySpecialHit();
	}
	else if (bSpecialHitPlayed && !bSpecialHitProbed && SpecialPlayer.time() >= SpecialHit.time + kSpecialHitProbeLag)
	{
		bSpecialHitProbed = true;
		// The verdict on its particle count (a lightweight emitter's is worked from its age), its scene proxy and its
		// last time drawn on screen: "played" means drawing.
		const bool bDrawing = ProbeSpecialHit(TEXT("the hit + 0.1 s"));
		if (bDrawing)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena check: special hit effect at the hit + 0.1 s: drawing (particles alive, drawn on screen since its play)."));
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena check: special hit effect at the hit + 0.1 s: NOT DRAWING (see the probe above: no particles, no scene proxy or not drawn since its play)."));
		}
	}
	const ftcore::SpecialCineFrame F = SpecialPlayer.frame(SpecialStage());
	const ftcore::SpecialCineMove& Move = *SpecialPlayer.move();
	// The lead's clip on the shots' clock, over what its weapons painted this frame (FFTLeadWeapons::Paint runs
	// before this): the whole body, its only action.
	if (bSpecialClipPlaying)
	{
		const AFTArenaFighter* Lead = PlayerFigure.Get();
		USkeletalMeshComponent* Skin = Lead ? Lead->GetLeadSkin() : nullptr;
		if (UFTLocomotionAnimInstance* Anim = Skin ? Cast<UFTLocomotionAnimInstance>(Skin->GetAnimInstance()) : nullptr)
		{
			Anim->ClearActions();
			Anim->SetActionUpper(0.f);
			Anim->SetActionLeg(-1);
			Anim->AddAction(UFTLocomotionAnimInstance::SpecialActionSlot, FMath::Min(SpecialPlayer.time(), SpecialClip ? SpecialClip->GetPlayLength() : 0.f), 1.f);
		}
	}
	if (SpecialCamera)
	{
		UFTSpecialCineModifier::FShot S;
		S.Location = Cm(F.pose.position);
		S.Rotation = (Cm(F.pose.lookAt) - S.Location).Rotation();
		S.FovDegrees = F.fovDegrees;
		S.FocusCm = F.focus * 100.f;
		S.FStop = F.fStop;
		S.ShakeCm = Cm(F.shake);
		S.PlayWeight = F.playWeight;
		S.bCut = F.cut;
		// The play camera moved to the end over the blend out while the lead still stands at the start (none once Core
		// has put it there).
		S.PlayShiftCm = !bSpecialPlaced && SpecialPlayer.time() >= Move.length() - Move.blendOut() ? SpecialPlayShift : FVector::ZeroVector;
		SpecialCamera->SetShot(S);
	}
	if (F.release)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: special: release at %.2f s (the hit is not dealt here)."), SpecialPlayer.time());
	}
#if !UE_BUILD_SHIPPING
	SpecialCuts += F.cut ? 1 : 0;
	++SpecialFrames;
	if (APlayerController* PC = GetPC(); PC && PC->PlayerCameraManager && DeltaSeconds > 0.f)
	{
		const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
		if (bSpecialEyeWas && !F.cut)
		{
			const float Step = static_cast<float>(FVector::Dist(Eye, SpecialEyeWas));
			float& Worst = F.playWeight > 0.f ? SpecialStepOut : SpecialStepShots;
			Worst = FMath::Max(Worst, Step);
		}
		SpecialEyeWas = Eye;
		bSpecialEyeWas = true;
	}
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		if (const AFTArenaFighter* Figure = GetEnemyFigure(I))
		{
			SpecialEnemyDrift = FMath::Max(SpecialEnemyDrift, static_cast<float>(FVector::Dist(Figure->GetActorLocation(), SpecialEnemyFrom[I])));
		}
	}
#endif
	const double Ms = (FPlatformTime::Seconds() - Began) * 1000.0;
	SpecialTickMs += Ms;
	SpecialTickMaxMs = FMath::Max(SpecialTickMaxMs, Ms);
#if !UE_BUILD_SHIPPING
	// -ArenaSpecialPause=<s>: the game paused at that time on the shots' clock for 1 s of real time; neither clock
	// may move while paused (nothing in this module ticks then).
	if (SpecialPauseAt >= 0.f && SpecialPlayer.time() >= SpecialPauseAt && !SpecialPauseTicker.IsValid())
	{
		SpecialPauseAt = -1.f; // once
		SpecialPauseCoreWas = Fight.special().time;
		SpecialPauseShotsWas = SpecialPlayer.time();
		if (UGameplayStatics::SetGamePaused(this, true))
		{
			SpecialPauseTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float) {
				const float CoreNow = Fight.special().time, ShotsNow = SpecialPlayer.time();
				const bool bHeld = FMath::Abs(CoreNow - SpecialPauseCoreWas) < 1.0e-4f && FMath::Abs(ShotsNow - SpecialPauseShotsWas) < 1.0e-4f;
				UE_LOG(LogForgottenThird, Display, TEXT("Arena check: special pause: 1 s paused at %.2f s: Core's clock %.3f -> %.3f s, the shots' %.3f -> %.3f s: %s."),
					SpecialPauseShotsWas, SpecialPauseCoreWas, CoreNow, SpecialPauseShotsWas, ShotsNow, bHeld ? TEXT("held") : TEXT("MOVED"));
				SpecialPauseTicker.Reset();
				UGameplayStatics::SetGamePaused(this, false);
				return false;
			}), 1.0f);
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena check: special pause: the world would not pause."));
		}
	}
	// -ArenaSpecialSkipAt=<s>: a skip press then, taken by PollSpecialSkip like the pad's.
	if (SpecialSkipAt >= 0.f && SpecialPlayer.time() >= SpecialSkipAt)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: special skip press at %.2f s (skippable: %s)."), SpecialPlayer.time(),
			Fight.special().skippable ? TEXT("yes") : TEXT("no"));
		SpecialSkipAt = -1.f; // once
		bSpecialPress = true;
	}
#endif
	// Over when both are: the shots played out and Core's cut back come (the fight held for any tail of shots past it).
	if (SpecialPlayer.finished() && !Fight.frozen())
	{
		EndSpecial(TEXT("played out"));
	}
}

void UFTArenaRunner::PlaySpecialHit()
{
	bSpecialHitPlayed = true;
	bSpecialContactDue = true; // the fist against the surface, once this frame's pose is drawn
	AFTArenaFighter* Lead = PlayerFigure.Get();
	if (!bSpecialHitFx || !Lead)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: special hit at %.3f s: played false (%s)."), SpecialPlayer.time(),
			!bSpecialHitFx ? TEXT("-ArenaSpecialNoHitFx") : TEXT("no lead figure"));
		return;
	}
	UFTVfxComponent* Vfx = SpecialHitFxOn(Lead, bSpecialHitLate ? TEXT("the hit (-ArenaSpecialHitLate)") : TEXT("the hit"));
	const FVector At = Cm(SpecialHit.at);
	Vfx->SetWorldLocationAndRotation(At, FFTLeadWeapons::ImpactFacing(Lead->GetActorLocation(), At));
	const bool bPlayed = Vfx->Play();
	SpecialHitPlayedWall = FPlatformTime::Seconds();
	SpecialHitPlayedWorld = GetWorld() ? GetWorld()->GetTimeSeconds() : -1.0;
	// Measured on the drawn figures: the effect's distance from the contact, and from the target's surface (its centre
	// less its radius, on the ground plane).
	const FVector Drawn = Vfx->GetComponentLocation();
	float Surface = -1.f;
	if (const AFTArenaFighter* Enemy = SpecialTarget >= 0 && SpecialTarget < MaxEnemies ? GetEnemyFigure(SpecialTarget) : nullptr)
	{
		Surface = static_cast<float>(FVector::Dist2D(Drawn, Enemy->GetActorLocation())) - SpecialStageAt.targetRadius * 100.f;
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena check: special hit at %.3f s (shot %u): played %s, %s at (%.0f, %.0f, %.0f) cm, %.1f cm from the contact, %.1f cm off the target's surface."),
		SpecialPlayer.time(), SpecialPlayer.move() ? SpecialPlayer.move()->shotAt(SpecialPlayer.time()) + 1 : 0u, bPlayed ? TEXT("true") : TEXT("false (no effect asset)"),
		*Vfx->Effect.ToString(), Drawn.X, Drawn.Y, Drawn.Z, FVector::Dist(Drawn, At), Surface);
	ProbeSpecialHit(TEXT("the hit"));
	// The partner at the hit (held where the press found it): its strike point (as PartnerImpact puts one) against the
	// contact line, and its body's distance from the drawn camera's sight line to the contact.
	const AFTArenaFighter* Ally = AllyFigure.Get();
	if (Ally && bPartner && Fight.allyHp() > 0 && SpecialPlayer.move())
	{
		const FVector Chest = Ally->GetChest() / 100.0;
		const ftcore::SpecialCinePartnerStrike S = ftcore::specialCinePartnerStrike(*SpecialPlayer.move(), SpecialStageAt, SpecialHit,
			ftcore::CutsceneVec3{static_cast<float>(Chest.X), static_cast<float>(Chest.Y), static_cast<float>(Chest.Z)}, SpecialStageAt.targetRadius);
		float Sight = -1.f;
		if (const APlayerController* PC = GetPC(); PC && PC->PlayerCameraManager)
		{
			const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
			const FVector Axis = Ally->GetActorLocation();
			Sight = static_cast<float>(FMath::PointDistToSegment(FVector(Axis.X, Axis.Y, At.Z), Eye, At)) / 100.f;
		}
		UE_LOG(LogForgottenThird, Display,
			TEXT("Arena check: special hit: the partner's strike point (%.2f, %.2f, %.2f) m is %.2f m off the contact line (at %.2f along it; on it within %.2f: %s), the partner %.1f deg round the target's axis from the lead's side; the partner's axis %.2f m from the camera's sight line to the contact."),
			S.at.x, S.at.y, S.at.z, S.offLine, S.along, ftcore::SpecialCinePartnerStrike::kOnLine, S.onLine() ? TEXT("yes") : TEXT("no"), S.bearing, Sight);
		if (!S.onLine())
		{
			// The park check: the partner stood where its blow would pass (the bearing that puts it kOnLine off the line),
			// at its own range from the target's axis, either side: would its body hide the contact in the shot shown?
			const ftcore::SpecialCineStage& St = SpecialStageAt;
			const float Out = St.targetRadius + ftcore::SpecialCineFraming::kContactOut;
			const float Pass = FMath::Asin(FMath::Min(1.f, ftcore::SpecialCinePartnerStrike::kOnLine / Out));
			const FVector Feet = Ally->GetActorLocation() / 100.0;
			const float Own = Fight.def().fighter.bodyRadius;
			const float Range = FMath::Max(FMath::Sqrt(FMath::Square(static_cast<float>(Feet.X) - St.target.x) + FMath::Square(static_cast<float>(Feet.Y) - St.target.y)),
				St.targetRadius + Own + 0.05f);
			const float Side = FMath::Atan2(SpecialHit.at.y - St.target.y, SpecialHit.at.x - St.target.x);
			ftcore::SpecialCineBody Two[2];
			Two[0].feet = St.target;
			Two[0].radius = St.targetRadius;
			Two[0].height = St.targetHeight;
			Two[0].target = true;
			Two[1].radius = Own;
			Two[1].height = FMath::Max(0.5f, Ally->GetHeight() / 100.f);
			const ftcore::SpecialCineProblem Alone = ftcore::specialCineHitProblem(*SpecialPlayer.move(), St, SpecialHit, SpecialPlayer.fixes(), Two, 1);
			FString Sides[2];
			for (int32 K = 0; K < 2; ++K)
			{
				const float B = Side + (K == 0 ? Pass : -Pass);
				Two[1].feet = ftcore::CutsceneVec3{St.target.x + Range * FMath::Cos(B), St.target.y + Range * FMath::Sin(B), static_cast<float>(Feet.Z)};
				const ftcore::SpecialCineProblem With = ftcore::specialCineHitProblem(*SpecialPlayer.move(), St, SpecialHit, SpecialPlayer.fixes(), Two, 2);
				Sides[K] = FString::Printf(TEXT("(%.2f, %.2f) m: %s"), Two[1].feet.x, Two[1].feet.y,
					With == ftcore::SpecialCineProblem::ContactHidden && Alone != ftcore::SpecialCineProblem::ContactHidden ? TEXT("HIDES the contact")
					: With == ftcore::SpecialCineProblem::None ? TEXT("the contact reads")
					: UTF8_TO_TCHAR(std::string(ftcore::specialCineProblemName(With)).c_str()));
			}
			UE_LOG(LogForgottenThird, Display,
				TEXT("Arena check: special hit: park check: the partner where its blow passes (%.1f deg from the lead's side, %.2f m from the target's axis; the target alone: %s): one side %s; the other %s."),
				FMath::RadiansToDegrees(Pass), Range, Alone == ftcore::SpecialCineProblem::None ? TEXT("the contact reads")
				: UTF8_TO_TCHAR(std::string(ftcore::specialCineProblemName(Alone)).c_str()), *Sides[0], *Sides[1]);
		}
	}
}

UFTVfxComponent* UFTArenaRunner::SpecialHitFxOn(AFTArenaFighter* Lead, const TCHAR* Why)
{
	UFTVfxComponent* Vfx = SpecialHitFx.Get();
	if (Vfx && Vfx->GetOwner() != Lead)
	{
		Vfx->DestroyComponent(); // the figures swapped: made again on the performer's
		Vfx = nullptr;
	}
	if (Vfx || !Lead)
	{
		return Vfx;
	}
	const double From = FPlatformTime::Seconds();
	// On the performer's figure, placed in the world: the freeze holds every other actor (and what it spawns).
	Vfx = NewObject<UFTVfxComponent>(Lead);
	Vfx->Effect = SpecialHitEffect.IsNone() ? kSpecialHitStandIn : SpecialHitEffect;
	Vfx->bPlayOnBeginPlay = false;
	Vfx->SetupAttachment(Lead->GetRootComponent());
	Vfx->SetUsingAbsoluteLocation(true);
	Vfx->SetUsingAbsoluteRotation(true);
	Vfx->SetUsingAbsoluteScale(true);
	Vfx->RegisterComponent();
	Vfx->SetWorldScale3D(FVector(kSpecialHitScale));
	Vfx->SetParameters(kSpecialHitScale, kSpecialHitScale);
	SpecialHitFx = Vfx;
	// Its system loaded and its instance made, not started (its pipeline states precache from here).
	const bool bLoaded = Vfx->Prepare();
	UNiagaraSystem* System = Vfx->System.Get();
	const bool bReadyFirst = System && System->IsReadyToRun();
	FString Waited;
#if WITH_EDITORONLY_DATA
	if (System && !bReadyFirst)
	{
		System->WaitForCompilationComplete(true, false);
		Vfx->Prepare();
		Waited = FString::Printf(TEXT(", waited for its compile: ready %s"), System->IsReadyToRun() ? TEXT("yes") : TEXT("NO"));
	}
#endif
	const UNiagaraComponent* Nc = Vfx->GetNiagara();
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: special hit effect %s made at %s in %.2f ms: system %s, ready to run %s%s; instance %s, its pipeline states precaching %s."),
		*Vfx->Effect.ToString(), Why, (FPlatformTime::Seconds() - From) * 1000.0, bLoaded ? *GetNameSafe(System) : TEXT("NOT FOUND"),
		bReadyFirst ? TEXT("yes") : TEXT("no"), *Waited, Nc && Nc->GetSystemInstanceController().IsValid() ? TEXT("made") : TEXT("NOT made"),
		Nc && Nc->IsPSOPrecaching() ? TEXT("yes") : TEXT("no"));
	return Vfx;
}

void UFTArenaRunner::HoldSpecialHitFx(bool bHold)
{
	// The wall time held since the play (the probe's third clock).
	const double WallNow = FPlatformTime::Seconds();
	if (bSpecialOn && SpecialHitPlayedWall >= 0.0)
	{
		if (bHold && SpecialHitHeldFrom < 0.0)
		{
			SpecialHitHeldFrom = WallNow;
		}
		else if (!bHold && SpecialHitHeldFrom >= 0.0)
		{
			SpecialHitHeldFor += static_cast<float>(WallNow - SpecialHitHeldFrom);
			SpecialHitHeldFrom = -1.0;
		}
	}
	UFTVfxComponent* Vfx = SpecialHitFx.Get();
	UNiagaraComponent* Nc = Vfx ? Vfx->GetNiagara() : nullptr;
	const bool bWant = bHold && bSpecialOn && bSpecialHitPlayed && !bSpecialHitHoldRuns && Nc;
	if (bWant == bSpecialHitFxHeld)
	{
		return;
	}
	bSpecialHitFxHeld = bWant;
	if (!Nc)
	{
		return;
	}
	Nc->SetPaused(bWant);
	if (!bWant)
	{
		ProbeSpecialHit(TEXT("the hold's end"));
	}
}

bool UFTArenaRunner::ProbeSpecialHit(const TCHAR* When)
{
#if UE_BUILD_SHIPPING
	(void)When;
	return true;
#else
	const UFTVfxComponent* Vfx = SpecialHitFx.Get();
	UNiagaraComponent* Nc = Vfx ? Vfx->GetNiagara() : nullptr;
	const double WallNow = FPlatformTime::Seconds();
	const float Wall = SpecialHitPlayedWall >= 0.0 ? static_cast<float>(WallNow - SpecialHitPlayedWall) : -1.f;
	const float Held = SpecialHitHeldFor + (SpecialHitHeldFrom >= 0.0 ? static_cast<float>(WallNow - SpecialHitHeldFrom) : 0.f);
	const float Clock = SpecialPlayer.time();
	const ftcore::SpecialCineMove* Move = SpecialPlayer.move();
	const uint32 Shown = Move ? ftcore::specialCineShownShot(*Move, Move->shotAt(Clock), SpecialPlayer.fixes()) + 1 : 0u;
	if (!Nc)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: special hit probe (%s): clock %.3f s (shot %u shown), %.3f s since the play: no effect component."),
			When, Clock, Shown, Wall);
		return false;
	}
	// The effect: its instance's age and particles (synced with its tick first), its state and render state.
	float Age = -1.f;
	int32 Particles = 0, Emitters = 0, Running = 0, Waiting = 0;
	FNiagaraSystemInstanceControllerPtr Controller = Nc->GetSystemInstanceController();
	if (FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr)
	{
		Instance->WaitForConcurrentTickAndFinalize();
		Age = Instance->GetAge();
		for (const FNiagaraEmitterInstanceRef& Emitter : Instance->GetEmitters())
		{
			++Emitters;
			Particles += Emitter->GetNumParticles();
			Running += Emitter->IsComplete() ? 0 : 1;
			Waiting += Emitter->IsWaitingForPSOCaching() ? 1 : 0;
		}
	}
	const AActor* Owner = Nc->GetOwner();
	const UWorld* World = GetWorld();
	const float Drawn = Nc->GetLastRenderTimeOnScreen();
	const FString DrawnText = World && Drawn > 0.f ? FString::Printf(TEXT("%.3f s ago"), World->GetTimeSeconds() - Drawn) : FString(TEXT("never"));
	const FVector At = Cm(SpecialHit.at);
	// The contact in the drawn frame, and what stands between the drawn eye and it.
	FString Screen = TEXT("no camera");
	FString Sight = TEXT("not traced");
	float EyeM = -1.f;
	if (APlayerController* PC = GetPC(); PC && PC->PlayerCameraManager)
	{
		const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
		EyeM = static_cast<float>(FVector::Dist(Eye, At) / 100.0);
		int32 Width = 0, Height = 0;
		PC->GetViewportSize(Width, Height);
		FVector2D Pixel;
		if (PC->ProjectWorldLocationToScreen(At, Pixel, false) && Width > 0 && Height > 0)
		{
			const double U = Pixel.X / Width, V = Pixel.Y / Height;
			Screen = FString::Printf(TEXT("at (%.2f, %.2f) of the frame%s"), U, V, U >= 0.0 && U <= 1.0 && V >= 0.0 && V <= 1.0 ? TEXT("") : TEXT(" (OUT)"));
		}
		else
		{
			Screen = TEXT("behind the camera");
		}
		if (World)
		{
			FHitResult Hit;
			const FCollisionQueryParams Params(SCENE_QUERY_STAT(FTSpecialHitProbe), true);
			Sight = World->LineTraceSingleByChannel(Hit, Eye, At, ECC_Visibility, Params)
				? FString::Printf(TEXT("blocked by %s (%s) %.2f m short of it"), *GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()),
					FVector::Dist(Hit.ImpactPoint, At) / 100.0)
				: FString(TEXT("clear"));
		}
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena check: special hit probe (%s): clock %.3f s (shot %u shown), age %.3f s, wall %.3f s since the play (%.3f s of it held); %s: %s, active %s, paused %s, ")
		TEXT("%d particles in %d emitters (%d running, %d waiting on pipeline states), precaching %s, scene proxy %s, last drawn on screen %s; visible %s, ")
		TEXT("owner %s hidden %s (dilation %.3f); bounds radius %.0f cm, %.0f cm from the contact; the contact %s, %.2f m from the eye; the sight to it %s."),
		When, Clock, Shown, Age, Wall, Held, *Vfx->Effect.ToString(), *UEnum::GetValueAsString(Nc->GetExecutionState()), Nc->IsActive() ? TEXT("yes") : TEXT("no"),
		Nc->IsPaused() ? TEXT("yes") : TEXT("no"), Particles, Emitters, Running, Waiting, Nc->IsPSOPrecaching() ? TEXT("yes") : TEXT("no"),
		Nc->SceneProxy ? TEXT("made") : TEXT("NONE"), *DrawnText, Nc->IsVisible() && !Nc->bHiddenInGame ? TEXT("yes") : TEXT("NO"), *GetNameSafe(Owner),
		Owner && Owner->IsHidden() ? TEXT("YES") : TEXT("no"), Owner ? Owner->CustomTimeDilation : -1.f, Nc->Bounds.SphereRadius,
		FVector::Dist(Nc->Bounds.Origin, At), *Screen, EyeM, *Sight);
	return Particles > 0 && Nc->SceneProxy && Drawn > 0.f && SpecialHitPlayedWorld >= 0.0 && Drawn >= SpecialHitPlayedWorld;
#endif
}

void UFTArenaRunner::EndSpecial(const TCHAR* Why)
{
	if (!bSpecialOn)
	{
		return;
	}
	const double Began = FPlatformTime::Seconds();
	HoldSpecialHitFx(false); // a still's hold cut short: the effect runs on
	const int32 Changed = SpecialFreeze.CountChanged();
	const int32 Held[5] = {SpecialFreeze.SoundCount(), SpecialFreeze.BodyCount(), SpecialFreeze.ClothCount(), SpecialFreeze.SpawnCount(), SpecialFreeze.ActorCount()};
	SpecialFreeze.End();
	PlaySpecialClip(false);
	StopSpecialPauseTest();
	bSpecialSkip = bSpecialSkipped = false;
	if (UWorld* World = GetWorld())
	{
		if (UFTRainSubsystem* Rain = World->GetSubsystem<UFTRainSubsystem>())
		{
			Rain->SetTimeScale(1.f);
		}
	}
	if (SpecialCamera)
	{
		SpecialCamera->Clear();
		if (APlayerController* PC = GetPC(); PC && PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->RemoveCameraModifier(SpecialCamera);
			if (!SpecialPlayer.finished())
			{
				PC->PlayerCameraManager->SetGameCameraCutThisFrame(); // cut short: a hard cut to the play camera
			}
		}
		SpecialCamera = nullptr;
	}
	bSpecialOn = false;
	const float Played = SpecialPlayer.time();
	const FString Id = SpecialPlayer.move() ? FString(UTF8_TO_TCHAR(std::string(SpecialPlayer.move()->id()).c_str())) : FString();
	SpecialPlayer.stop();
	SpecialEndMs = (FPlatformTime::Seconds() - Began) * 1000.0;
	SpecialEndedAt = Now;
	// The hand-back: the lead where Core's cut back put it.
	const float Step = FMath::Sqrt((SpecialEnd.x - Bodies.player.x) * (SpecialEnd.x - Bodies.player.x) +
		(SpecialEnd.y - Bodies.player.y) * (SpecialEnd.y - Bodies.player.y));
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: special %s ends (%s) at %.2f s: the lead %s at (%.2f, %.2f) m, %.2f m from the clamped end; ")
		TEXT("gave back %d actors, %d sounds, %d bodies, %d cloths (%d spawned while frozen)."),
		*Id, Why, Played, bSpecialPlaced ? TEXT("put by Core's cut back") : TEXT("not put (no cut back)"), Bodies.player.x, Bodies.player.y, Step,
		Held[4], Held[0], Held[1], Held[2], Held[3]);
	bSpecialPlaced = false;
	if (Changed > 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: special %s: %d held actor(s) had their clock changed while frozen (given back as found at the begin)."), *Id, Changed);
	}
#if !UE_BUILD_SHIPPING
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena check: special %s: %.2f s, %d frames, %d cuts; enemies drifted %.1f cm; eye step per frame worst: in the shots %.1f cm, in the blend out %.1f cm."),
		*Id, Played, SpecialFrames, SpecialCuts, SpecialEnemyDrift, SpecialStepShots, SpecialStepOut);
	SpecialAfterFrames = 0;
	SpecialStepAfter = 0.f;
#endif
}

void UFTArenaRunner::StopSpecialPauseTest()
{
#if !UE_BUILD_SHIPPING
	if (SpecialPauseTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(SpecialPauseTicker);
		SpecialPauseTicker.Reset();
		if (UWorld* World = GetWorld(); World && !World->bIsTearingDown)
		{
			UGameplayStatics::SetGamePaused(this, false); // ended while the test held the game paused
		}
	}
#endif
}

void UFTArenaRunner::EndPlay(const EEndPlayReason::Type Reason)
{
	EndSpecial(TEXT("torn down")); // the freeze, the rain and the camera given back before the world goes
	StopSpecialPauseTest();
	Super::EndPlay(Reason);
}

void UFTArenaRunner::TickSpecialCost(float DeltaSeconds, bool bHold)
{
	// A still's frames, the two after it (the timings read are the last frame's: the screenshot's hitch) and the first
	// second (the load) are left out.
	if (bHold || DeltaSeconds <= 0.f || Fight.time() < 1.f)
	{
		SpecialCostSkip = 2;
		return;
	}
	if (SpecialCostSkip > 0)
	{
		--SpecialCostSkip;
		return;
	}
	int32 Phase = -1;
	if (bSpecialOn)
	{
		Phase = 1;
	}
	else if (SpecialBeganAt < 0.0)
	{
		Phase = 0;
	}
	else if (SpecialEndedAt >= 0.0 && Now - SpecialEndedAt <= kSpecialCostWindow)
	{
		Phase = 2;
	}
	if (Phase < 0)
	{
		if (SpecialEndedAt >= 0.0 && SpecialCost[2].Frames > 0)
		{
			auto Mean = [](const FSpecialCost& C, bool bGpu) { return C.Frames > 0 ? (bGpu ? C.Gpu : C.Game) / C.Frames : 0.0; };
			FVector2D View(0.0, 0.0);
			if (const UWorld* World = GetWorld(); World && World->GetGameViewport())
			{
				World->GetGameViewport()->GetViewportSize(View);
			}
			UE_LOG(LogForgottenThird, Display,
				TEXT("Arena check: special frame cost (game thread / GPU ms, mean and worst, %dx%d): before %.2f / %.2f (worst %.2f / %.2f, %d frames); "
					 "during %.2f / %.2f (worst %.2f / %.2f, %d frames); after %.2f / %.2f (worst %.2f / %.2f, %d frames). "
					 "The special's own tick %.3f ms a frame (worst %.3f); the freeze's begin %.2f ms, its end %.2f ms."),
				static_cast<int32>(View.X), static_cast<int32>(View.Y),
				Mean(SpecialCost[0], false), Mean(SpecialCost[0], true), SpecialCost[0].GameMax, SpecialCost[0].GpuMax, SpecialCost[0].Frames,
				Mean(SpecialCost[1], false), Mean(SpecialCost[1], true), SpecialCost[1].GameMax, SpecialCost[1].GpuMax, SpecialCost[1].Frames,
				Mean(SpecialCost[2], false), Mean(SpecialCost[2], true), SpecialCost[2].GameMax, SpecialCost[2].GpuMax, SpecialCost[2].Frames,
				SpecialCost[1].Frames > 0 ? SpecialTickMs / SpecialCost[1].Frames : 0.0, SpecialTickMaxMs, SpecialBeginMs, SpecialEndMs);
			for (FSpecialCost& C : SpecialCost)
			{
				C = FSpecialCost();
			}
			SpecialTickMs = SpecialTickMaxMs = 0.0;
		}
		return;
	}
	const double Game = FPlatformTime::ToMilliseconds(GGameThreadTime);
	const double Gpu = FPlatformTime::ToMilliseconds64(RHIGetGPUFrameCycles(0));
	FSpecialCost& C = SpecialCost[Phase];
	C.Game += Game;
	C.Gpu += Gpu;
	C.GameMax = FMath::Max(C.GameMax, Game);
	C.GpuMax = FMath::Max(C.GpuMax, Gpu);
	++C.Frames;
}

bool UFTArenaRunner::TakeSpecialStill(FString& OutName)
{
#if UE_BUILD_SHIPPING
	(void)OutName;
	return false;
#else
	if (!bSpecialOn || SpecialStillNext >= SpecialStillCount || SpecialPlayer.time() < SpecialStillAt[SpecialStillNext])
	{
		return false;
	}
	const ftcore::SpecialCineMove& Move = *SpecialPlayer.move();
	const uint32 Timed = Move.shotAt(SpecialPlayer.time());
	const uint32 Shot = ftcore::specialCineShownShot(Move, Timed, SpecialPlayer.fixes()); // the shot shown (a cover's)
	// One of the hit's stills: named by its lag after the hit (on the shots' clock), the effect probed as it is taken.
	const float StillAt = SpecialStillAt[SpecialStillNext];
	int32 LagMs = -1;
	for (uint32 K = 0; K < SpecialHitStills && LagMs < 0; ++K)
	{
		LagMs = StillAt == SpecialHitStillAt[K] ? FMath::RoundToInt((StillAt - SpecialHit.time) * 1000.f) : -1;
	}
	const FString Over = Shot != Timed ? FString::Printf(TEXT("_over%u"), Timed + 1) : FString();
	const FString Hit = LagMs >= 0 ? FString::Printf(TEXT("_hit+%dms"), LagMs) : FString();
	OutName = FString::Printf(TEXT("sp_%05.2f_shot%u%s%s"), SpecialPlayer.time(), Shot + 1, *Over, *Hit);
	++SpecialStillNext;
	if (LagMs >= 0)
	{
		ProbeSpecialHit(*OutName);
	}
	return true;
#endif
}
