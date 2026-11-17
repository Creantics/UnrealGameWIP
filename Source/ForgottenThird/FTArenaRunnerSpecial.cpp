// UFTArenaRunner's special move (FTArenaRunner.h, "A special move"): the shot list over a frozen fight.
//
// Core runs the move (ftcore/ArenaSpecial.h): the gauge, the press, the target, the freeze, the results at the cut back.
// The press carries the end point this host allows: Core's endTravel/endSide (the shot file's end_feet), forward toward
// the target, clamped to walkable ground first (the capsule swept along the path through the static world, the ring,
// the standing bodies). Blocked: the performer starts back by the missing distance when there is room behind
// (specialBack), else the travel is shortened (specialTravel). Core puts the performer at view.origin at the press and
// at view.end at the cut back; this host draws it there with no step that tick. While frozen the fight keeps stepping
// (only the special's clock moves); the world but the performer is held here, the camera plays the shots, and a tail
// of shots past Core's cut back holds the fight until the last shot has blended into the play camera.

#include "FTArenaRunner.h"

#include "Algo/Sort.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "CollisionShape.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "FTArenaFighter.h"
#include "FTLook.h"
#include "FTRainSubsystem.h"
#include "FTSpecialCine.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RenderCore.h"

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
	FParse::Value(Cmd, TEXT("ArenaSpecialTest="), SpecialTestAt);
	bSpecialShots = FParse::Param(Cmd, TEXT("ArenaSpecialShots"));
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
		if (FMath::Abs(Feet.x - Rules.endTravel) > 0.005f || FMath::Abs(Feet.y - Rules.endSide) > 0.005f)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: specials: %s: Core ends it at (%.2f, %.2f) m but its shots at end_feet (%.2f, %.2f) m."),
				UTF8_TO_TCHAR(Id.c_str()), Rules.endTravel, Rules.endSide, Feet.x, Feet.y);
		}
		if (FMath::Abs(Shots->release() - Rules.duration) > 0.05f)
		{
			UE_LOG(LogForgottenThird, Warning,
				TEXT("Arena: specials: %s: Core cuts back at %.2f s, its shots at %.2f s (%.2f s long): the fight is held for the shots' tail."),
				UTF8_TO_TCHAR(Id.c_str()), Rules.duration, Shots->release(), Shots->length());
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

void UFTArenaRunner::ClampSpecialTravel(const ftcore::Vec2& From, const ftcore::Vec2& Forward, const ftcore::CutsceneVec3& EndFeet)
{
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
	const ftcore::Vec2 Back{-Forward.x, -Forward.y};
	const ftcore::Vec2 Start{From.x - Forward.x * Missing, From.y - Forward.y * Missing};
	if (ClearShare(From, Back, Missing, BackWhy) >= 0.999f && ClearShare(Start, Dir, Length, BackWhy) >= 0.999f)
	{
		SpecialBack = Missing;
		SpecialEnd = {Start.x + Move.x, Start.y + Move.y};
		return;
	}
	SpecialTravel = FMath::Max(0.f, Share - 0.01f); // a hair short of the obstacle
	SpecialEnd = {From.x + Move.x * SpecialTravel, From.y + Move.y * SpecialTravel};
}

void UFTArenaRunner::PressSpecial(ftcore::ArenaInput& In)
{
	if (!bSpecialPress)
	{
		return;
	}
	bSpecialPress = false;
	bSpecialPressedNow = true;
	In.special = ftcore::kArenaPlayer;
	SpecialBack = 0.f;
	SpecialTravel = 1.f;
	SpecialLimit = TEXT("none");
	SpecialTarget = -1;
	const ftcore::ArenaSpecialRules* Rules =
		bArenaSpecials && PlayerLook ? ArenaSpecials.forLead(std::string(TCHAR_TO_UTF8(*PlayerLook->Name.ToLower()))) : nullptr;
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
	ClampSpecialTravel(Bodies.player, SpecialForward, ftcore::CutsceneVec3{Rules->endTravel, Rules->endSide, 0.f});
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
			break;
		case ftcore::ArenaEventType::SpecialEnded:
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: special cut back%s: %s at (%.2f, %.2f) m, %d result(s)."), Fight.time(),
				E.amount ? TEXT(" (skipped)") : TEXT(""), Who, V.end.x, V.end.y, static_cast<int32>(V.resultCount));
			break;
		default:
			break;
		}
	}
	if (bSpecialPressedNow && !bStarted)
	{
		// NotControlled (the partner's move), NotReady, NoTarget, NoSight, Down, Running, Over, Unhurt or NoMove.
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: special refused: %s."), Fight.time(),
			UTF8_TO_TCHAR(ftcore::arenaSpecialRefusalName(Fight.specialRefusal(ftcore::kArenaPlayer))));
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
	const ftcore::ArenaOutput& Out = Fight.tick(StepSeconds, ftcore::ArenaInput(), Bodies);
	if (AfterSpecialTick(Out) && V.endedThisTick)
	{
		// Ended on a tick it was not due (its move gone from the host's table): its events all the same.
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: special ended on a frozen step it was not due (%.2f of %.2f s)."), V.time, V.duration);
		HandleEvents(Out.events);
	}
	return true;
}

bool UFTArenaRunner::BeginSpecial()
{
	const ftcore::ArenaSpecialView& V = Fight.special();
	const ftcore::ArenaSpecialRules* Rules = Fight.specialMove();
	const std::string MoveId = Rules ? std::string(Rules->idView()) : std::string();
	const FString Id = UTF8_TO_TCHAR(MoveId.c_str());
	const ftcore::SpecialCineMove* Move = SpecialTable.find(MoveId);
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
	SpecialTarget = Target;
	SpecialForward = V.dir;
	const ftcore::Vec2 To = Bodies.enemies[static_cast<size_t>(Target)];
	const float Dist = FMath::Sqrt((To.x - V.origin.x) * (To.x - V.origin.x) + (To.y - V.origin.y) * (To.y - V.origin.y));
	const float FeetZ = static_cast<float>(Performer->GetActorLocation().Z / 100.0);
	SpecialStageAt = ftcore::SpecialCineStage();
	SpecialStageAt.origin = {V.origin.x, V.origin.y, FeetZ};
	SpecialStageAt.forward = {V.dir.x, V.dir.y, 0.f};
	SpecialStageAt.target = {To.x, To.y, static_cast<float>(Enemy->GetActorLocation().Z / 100.0)};
	SpecialStageAt.targetHeight = FMath::Max(0.5f, Enemy->GetHeight() / 100.f);
	SpecialStageAt.targetRadius = Fight.def().enemies[static_cast<size_t>(Target)].bodyRadius;
	// Nothing paused on the performer, nothing shaking the camera.
	LeadWeapons.HitPause(false);
	Jolt = ftcore::ArenaJolt();
	SpecialFreeze.Begin(GetWorld(), PlayerFigure, kSpecialFreeze);
	if (UWorld* World = GetWorld())
	{
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
	if (bSpecialShots)
	{
		for (uint32 I = 0; I < Move->size() && SpecialStillCount < static_cast<int32>(UE_ARRAY_COUNT(SpecialStillAt)) - 1; ++I)
		{
			SpecialStillAt[SpecialStillCount++] = 0.5f * (Move->shot(I).in + Move->shot(I).out);
		}
		SpecialStillAt[SpecialStillCount++] = Move->release() + 0.05f;
		Algo::Sort(MakeArrayView(SpecialStillAt, SpecialStillCount));
	}
#endif
	const float Off = bSameTarget ? FMath::Sqrt((V.end.x - SpecialEnd.x) * (V.end.x - SpecialEnd.x) + (V.end.y - SpecialEnd.y) * (V.end.y - SpecialEnd.y)) : -1.f;
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: %.2f: special %s begins on enemy %d (%.2f m off, radius %.2f, height %.2f m): %u shots, %.2f s (Core %.2f s); froze %d actors and %d effects in %.2f ms. ")
		TEXT("Clamp: travel %.2f m (%.0f%%), back %.2f m (limited by %s); Core: from (%.2f, %.2f) to (%.2f, %.2f) m, %.2f m from the clamped end (-1: Core took another target)."),
		Fight.time(), *Id, Target, Dist, SpecialStageAt.targetRadius, SpecialStageAt.targetHeight, Move->size(), Move->length(), V.duration,
		SpecialFreeze.ActorCount(), SpecialFreeze.EffectCount(), SpecialBeginMs, V.travel, SpecialTravel * 100.f, SpecialBack, SpecialLimit,
		V.origin.x, V.origin.y, V.end.x, V.end.y, Off);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: special %s: the clip %s is not played here yet (the lead holds its own pose): a stand-in."), *Id,
		UTF8_TO_TCHAR(std::string(Move->clip()).c_str()));
	return true;
}

void UFTArenaRunner::TickSpecial(float DeltaSeconds)
{
	if (!bSpecialOn)
	{
#if !UE_BUILD_SHIPPING
		if (SpecialTestAt >= 0.f && Fight.time() >= SpecialTestAt && !Fight.over() && !Fight.frozen())
		{
			SpecialTestAt = -1.f; // once a fight
			if (bArenaSpecials)
			{
				Fight.setSpecialGaugeForTest(0, ArenaSpecials.gauge.full); // both kit slots: whichever the lead drives
				Fight.setSpecialGaugeForTest(1, ArenaSpecials.gauge.full);
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: -ArenaSpecialTest: the gauges full, the special pressed."), Fight.time());
			RequestSpecial();
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
	const double Began = FPlatformTime::Seconds();
	SpecialPlayer.step(DeltaSeconds);
	const ftcore::SpecialCineFrame F = SpecialPlayer.frame(SpecialStage());
	const ftcore::SpecialCineMove& Move = *SpecialPlayer.move();
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
	// Over when both are: the shots played out and Core's cut back come (the fight held for any tail of shots past it).
	if (SpecialPlayer.finished() && !Fight.frozen())
	{
		EndSpecial(TEXT("played out"));
	}
}

void UFTArenaRunner::EndSpecial(const TCHAR* Why)
{
	if (!bSpecialOn)
	{
		return;
	}
	const double Began = FPlatformTime::Seconds();
	const int32 Changed = SpecialFreeze.CountChanged();
	SpecialFreeze.End();
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
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: special %s ends (%s) at %.2f s: the lead %s at (%.2f, %.2f) m, %.2f m from the clamped end."),
		*Id, Why, Played, bSpecialPlaced ? TEXT("put by Core's cut back") : TEXT("not put (no cut back)"), Bodies.player.x, Bodies.player.y, Step);
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
	const uint32 Shot = Move.shotAt(SpecialPlayer.time());
	OutName = FString::Printf(TEXT("sp_%05.2f_shot%u"), SpecialPlayer.time(), Shot + 1);
	++SpecialStillNext;
	return true;
#endif
}
