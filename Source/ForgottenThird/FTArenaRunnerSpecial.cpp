// UFTArenaRunner's special move (FTArenaRunner.h, "A special move"): the shot list over a frozen fight.
//
// Core takes the move's end displacement (the shot file's end_feet, forward toward the target at the press) and puts
// the performer there when the move ends. This host clamps that end point to walkable ground before the move starts:
// the capsule swept along the path (static world), the fight's ring and the standing bodies. Blocked: the performer
// starts back by the missing distance when there is room behind, else the travel is shortened (the clip's root motion
// scaled to it). The last shot blends into the play camera moved to the end point (SpecialPlayShift).
//
// Until Core's calls for that land, the lead stays at its start: the clamp is logged ("travel X of Y m, back Z m") and
// the play camera is not moved, so the hand-back does not pop either way.

#include "FTArenaRunner.h"

#include "Algo/Sort.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "CollisionShape.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "FTArenaFighter.h"
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
	SpecialTestMove = TEXT("devin_special_01");
	FParse::Value(Cmd, TEXT("ArenaSpecialMove="), SpecialTestMove);
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
	// Room behind for the missing part: the performer starts back by it and travels the whole way.
	const float Missing = (1.f - Share) * Length;
	const TCHAR* BackWhy = TEXT("none");
	const ftcore::Vec2 Back{-Dir.x, -Dir.y};
	if (ClearShare(From, Back, Missing, BackWhy) >= 0.999f)
	{
		SpecialBack = Missing;
		SpecialEnd = {From.x - Dir.x * Missing + Move.x, From.y - Dir.y * Missing + Move.y};
		return;
	}
	SpecialTravel = FMath::Max(0.f, Share - 0.01f); // a hair short of the obstacle
	SpecialEnd = {From.x + Move.x * SpecialTravel, From.y + Move.y * SpecialTravel};
}

bool UFTArenaRunner::BeginSpecial(const FString& MoveId)
{
	const ftcore::SpecialCineMove* Move = SpecialTable.find(TCHAR_TO_UTF8(*MoveId));
	const TCHAR* Refusal = nullptr;
	if (bSpecialOn)
	{
		Refusal = TEXT("one is playing");
	}
	else if (!Move || Move->empty())
	{
		Refusal = TEXT("no such move, or it has no shots");
	}
	else if (Fight.over())
	{
		Refusal = TEXT("the fight is over");
	}
	else if (!PlayerFigure)
	{
		Refusal = TEXT("no lead figure");
	}
	else if (Fight.output().player.action == ftcore::ArenaAction::Downed)
	{
		Refusal = TEXT("the lead is down");
	}
	// The target: the lock, else the nearest standing enemy.
	int32 Target = -1;
	if (!Refusal)
	{
		const int32 Count = FMath::Min(static_cast<int32>(Fight.def().enemyCount), static_cast<int32>(MaxEnemies));
		const std::uint8_t Lock = Fight.lock();
		Target = Lock != ftcore::kArenaNobody && Lock < Count && Fight.enemyHp(Lock) > 0 ? static_cast<int32>(Lock) : -1;
		float Best = TNumericLimits<float>::Max();
		for (int32 I = 0; Target < 0 && I < Count; ++I)
		{
			if (Fight.enemyHp(static_cast<size_t>(I)) <= 0)
			{
				continue;
			}
			const ftcore::Vec2 E = Bodies.enemies[static_cast<size_t>(I)];
			const float D = (E.x - Bodies.player.x) * (E.x - Bodies.player.x) + (E.y - Bodies.player.y) * (E.y - Bodies.player.y);
			Best = FMath::Min(Best, D);
		}
		for (int32 I = 0; Target < 0 && I < Count; ++I)
		{
			const ftcore::Vec2 E = Bodies.enemies[static_cast<size_t>(I)];
			if (Fight.enemyHp(static_cast<size_t>(I)) > 0 &&
				(E.x - Bodies.player.x) * (E.x - Bodies.player.x) + (E.y - Bodies.player.y) * (E.y - Bodies.player.y) <= Best)
			{
				Target = I;
			}
		}
		if (Target < 0 || !GetEnemyFigure(Target))
		{
			Refusal = TEXT("no target standing");
		}
	}
	if (Refusal)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: special %s refused: %s."), Fight.time(), *MoveId, Refusal);
		return false;
	}
	const double Began = FPlatformTime::Seconds();
	SpecialTarget = Target;
	const AFTArenaFighter* Enemy = GetEnemyFigure(Target);
	const ftcore::Vec2 From = Bodies.player;
	const ftcore::Vec2 To = Bodies.enemies[static_cast<size_t>(Target)];
	const float Dist = FMath::Sqrt((To.x - From.x) * (To.x - From.x) + (To.y - From.y) * (To.y - From.y));
	SpecialForward = Dist > 0.01f ? ftcore::Vec2{(To.x - From.x) / Dist, (To.y - From.y) / Dist} : Fight.output().player.facing;
	ClampSpecialTravel(From, SpecialForward, Move->endFeet());
	const float FeetZ = static_cast<float>(PlayerFigure->GetActorLocation().Z / 100.0);
	SpecialStageAt = ftcore::SpecialCineStage();
	// The frame stays on the lead where it stands: the start-back (SpecialBack) needs Core's placement at the begin.
	SpecialStageAt.origin = {From.x, From.y, FeetZ};
	SpecialStageAt.forward = {SpecialForward.x, SpecialForward.y, 0.f};
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
	SpecialPlayShift = FVector::ZeroVector;
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
	const ftcore::CutsceneVec3 Feet = Move->endFeet();
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: %.2f: special %s begins on enemy %d (%.2f m off, radius %.2f, height %.2f m): %u shots, %.2f s; froze %d actors and %d effects in %.2f ms. "
			 "End feet (%.2f, %.2f) m: travel %.2f of %.2f m, back %.2f m (limited by %s); ends at (%.2f, %.2f) m."),
		Fight.time(), *MoveId, Target, Dist, SpecialStageAt.targetRadius, SpecialStageAt.targetHeight, Move->size(), Move->length(),
		SpecialFreeze.ActorCount(), SpecialFreeze.EffectCount(), SpecialBeginMs, Feet.x, Feet.y, SpecialTravel,
		FMath::Sqrt(Feet.x * Feet.x + Feet.y * Feet.y), SpecialBack, SpecialLimit, SpecialEnd.x, SpecialEnd.y);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: special %s: the clip %s is not played here yet (the lead holds its own pose): a stand-in."), *MoveId,
		UTF8_TO_TCHAR(std::string(Move->clip()).c_str()));
	return true;
}

void UFTArenaRunner::TickSpecial(float DeltaSeconds)
{
	if (!bSpecialOn)
	{
#if !UE_BUILD_SHIPPING
		if (SpecialTestAt >= 0.f && Fight.time() >= SpecialTestAt && !Fight.over())
		{
			SpecialTestAt = -1.f; // once a fight
			BeginSpecial(SpecialTestMove);
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
		// The play camera moved to the end feet over the blend out (zero until Core places the lead there).
		S.PlayShiftCm = SpecialPlayer.time() >= Move.length() - Move.blendOut() ? SpecialPlayShift : FVector::ZeroVector;
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
	if (SpecialPlayer.finished())
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
	// The hand-back: Core keeps the lead where it stood (its end-feet call is not in yet): the lead does not move.
	const float Step = FMath::Sqrt((SpecialEnd.x - Bodies.player.x) * (SpecialEnd.x - Bodies.player.x) +
		(SpecialEnd.y - Bodies.player.y) * (SpecialEnd.y - Bodies.player.y));
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: special %s ends (%s) at %.2f s: the lead stays at its start, %.2f m short of the clamped end (%.2f, %.2f) m, until Core places it there."),
		*Id, Why, Played, Step, SpecialEnd.x, SpecialEnd.y);
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
