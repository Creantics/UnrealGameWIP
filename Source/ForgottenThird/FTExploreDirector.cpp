#include "FTExploreDirector.h"

#include "AI/Navigation/NavQueryFilter.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/PrimitiveComponent.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "TimerManager.h"

#include "ForgottenThird.h"
#include "FTArenaHost.h"
#include "FTCutsceneDirector.h"
#include "FTExploreCharacter.h"
#include "FTExploreSpot.h"
#include "FTFieldGuard.h"
#include "FTGameInstance.h"
#include "FTLocomotionAnimInstance.h"
#include "FTPartyFollower.h"
#include "FTStepHost.h"
#include "FTStorySubsystem.h"
#include "FTStyleController.h"
#include "FTText.h"

#include "ftcore/BattleDemo.h"
#include "ftcore/ExploreDemo.h"
#include "ftcore/FieldGoals.h"
#include "ftcore/PathFollow.h"
#include "ftcore/StoryDatabase.h"

#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr float kEncounterDelay = 1.2f;
// The autopilot gives up when it hasn't got 10 cm closer to its target for this long, or has walked slower than
// kPilotStallSpeed for this long. On a pinned world tour's full navmesh path, getting kPilotPathGain closer along the path
// that is left also counts as closer, so a lead walking a detour runs on, under kTourLegCapSeconds.
constexpr float kPilotStuckSeconds = 15.f;
constexpr float kPilotStallSpeed = 1.f; // m/s
constexpr float kPilotPathGain = 0.5f;  // m
// A pinned world tour gives a leg up after this long (s of game time) whatever its path, so a lead that keeps finding a
// shorter way along a path that circles is still caught. The longest leg of a clean tour took 167 s.
constexpr double kTourLegCapSeconds = 300.0;
// The pilot's last navmesh answer (PilotPathState), for the log.
const TCHAR* const kPilotPathStates[] = {TEXT("never asked"), TEXT("no path"), TEXT("a partial path"), TEXT("a full path")};
// Turning to face a spot (TickFace): how long (s) the movement before it may linger.
constexpr float kFaceGrace = 0.15f;
// -WorldAutoPlay: a district counts as reached this close to its plaza's centre (metres); the
// tour's pilot targets get ids from here up, apart from the demo spots' and the field goal's.
constexpr float kTourReach = 4.f;
constexpr int32 kTourTargetBase = 300;
// The tour's stop legs (TickTourStop): pilot targets from here up; the walk's path search budget (the engine's default
// is 2048; the map builder's reach check keeps 32768); how far round an edge stop's target the navmesh's nearest point
// is looked for (cm), and how close the walk comes to it (m) before the push.
// The budget is Detour's most: its node pool indexes nodes with a 16-bit dtNodeIndex whose all-ones value is
// DT_NULL_IDX (DetourNode.h), and dtNodePool::getNode casts the node count to it unchecked, so a pool of more than
// 65535 nodes wraps its indices on a long search instead of running out.
constexpr int32 kTourStopTargetBase = 500;
constexpr uint32 kTourSearchNodes = 65535;
const FVector kEdgeNavExtent(2000.0, 2000.0, 1000.0);
constexpr float kEdgeWalkReach = 1.f;
// After a set-down the lead settles at least this long and until it stands on the ground, at most the second (s of
// game time).
constexpr double kStopSettleSeconds = 1.0;
constexpr double kStopSettleMaxSeconds = 3.0;
// An edge stop's push and a push stop's (s of game time); a push stop holds when the lead comes no closer than
// kPushHeldMetres to its target. The push steers at the target until the lead is within kPushAimMetres of it, then
// holds that way.
constexpr double kEdgePushSeconds = 3.0;
constexpr double kPushSeconds = 8.0;
constexpr float kPushHeldMetres = 3.f;
constexpr float kPushAimMetres = 0.25f;
// An edge leg's judging (JudgeEdgeStop): the ground past a hidden wall is looked for this far past where the sweep met
// it, and along the probe at this step (cm); each down-trace runs from 2 m over the feet to 100 m under them. A fall
// of more than kStopFellCm on the push is noted.
constexpr double kEdgeWallPastCm = 150.0;
constexpr double kEdgeGroundStepCm = 50.0;
constexpr double kEdgeGroundAboveCm = 200.0;
constexpr double kEdgeGroundBelowCm = 10000.0;
constexpr double kStopFellCm = 50.0;
// A tour leg is wet when the lead's feet go this far under the sea's level (cm).
constexpr double kTourWetBand = 5.0;
// A still is looked for on disk this often, for this long (real seconds).
constexpr double kStillCheckSeconds = 0.25;
constexpr double kStillWaitSeconds = 5.0;
enum class ETourStopPhase : uint8 { Begin, Settle, Walk, Push, Still };
// An edge's letter to its tally (W, S, N, E), -1 for none.
int32 EdgeIndex(uint8 Letter)
{
	switch (Letter)
	{
	case 'W': return 0;
	case 'S': return 1;
	case 'N': return 2;
	case 'E': return 3;
	default: return -1;
	}
}
const TCHAR kEdgeLetters[4] = {TEXT('W'), TEXT('S'), TEXT('N'), TEXT('E')};
// Out of the square across each edge, in the engine frame (plan north is engine -Y).
constexpr double kEdgeOutX[4] = {-1.0, 0.0, 0.0, 1.0};
constexpr double kEdgeOutY[4] = {0.0, 1.0, -1.0, 0.0};
// The overworld's pilot targets (a hop's gates, the tour's gates) get ids from here up.
constexpr int32 kOverTargetBase = 400;
// A follower nearer the camera than this share of the lead's distance is hidden: it would loom over the lead.
constexpr double kFollowerCameraShare = 0.8;
// Each follower turns to face where the lead turned on the spot this long (s) after the one before it.
constexpr float kFollowerFaceDelay = 0.3f;

const TCHAR* const kObjectiveNames[ftcore::demo::kObjCount] = {
	TEXT("Talk to Duy"),
	TEXT("Find the light shards"),
	TEXT("Open the old gate"),
	TEXT("Win the ambush"),
};

const TCHAR* KindPrompt(ftcore::InteractKind Kind)
{
	switch (Kind)
	{
	case ftcore::InteractKind::Talk:
		return TEXT("E  Talk to Duy");
	case ftcore::InteractKind::Pickup:
		return TEXT("E  Pick up the light shard");
	case ftcore::InteractKind::Gate:
		return TEXT("E  Open the gate");
	default:
		return TEXT("");
	}
}

// Stream 1 adds UFTGameInstance::IsAlphaFast(); until then read the flag here.
bool IsAlphaFast()
{
	static const bool bFast = FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
	return bFast;
}

ftcore::Vec2 ToGround(const FVector& V)
{
	// Unreal centimetres to the demo's metres.
	return {static_cast<float>(V.X / 100.0), static_cast<float>(V.Y / 100.0)};
}

// An edge stop's walk aim (TickTourStop): the point (cm) nearest Target among the polys of one island (Islands, corners
// in engine metres) whose corners' box meets Target +- Extent (cm), as the navmesh's nearest-point query looks within
// that box, but on the one island; false for none there. A scan of every gathered poly, once per edge stop; it
// allocates nothing, and a poly whose box (with a metre's slack in z for the detail mesh) is no nearer than the best so
// far isn't asked.
bool NearestOnIsland(const FFTNavIslands& Islands, const ARecastNavMesh& Recast, uint32 Island, const FVector& Target, const FVector& Extent,
	FVector& Out)
{
	const FVector Lo = (Target - Extent) / 100.0;
	const FVector Hi = (Target + Extent) / 100.0;
	const FVector TargetM = Target / 100.0;
	const size_t Count = FMath::Min(Islands.Polys.size(), Islands.Set.islandOf.size());
	double Best = TNumericLimits<double>::Max(); // squared cm
	bool bFound = false;
	for (size_t i = 0; i < Count && i < static_cast<size_t>(Islands.Refs.Num()); ++i)
	{
		const ftcore::NavPolyRef& Ref = Islands.Polys[i];
		if (Islands.Set.islandOf[i] != Island || Ref.count == 0 || static_cast<size_t>(Ref.first) + Ref.count > Islands.Corners.size())
		{
			continue;
		}
		FBox Box(ForceInit);
		for (uint32 k = 0; k < Ref.count; ++k)
		{
			const ftcore::MassingVec& C = Islands.Corners[Ref.first + k];
			Box += FVector(C.x, C.y, C.z);
		}
		if (Box.Min.X > Hi.X || Box.Max.X < Lo.X || Box.Min.Y > Hi.Y || Box.Max.Y < Lo.Y || Box.Min.Z > Hi.Z || Box.Max.Z < Lo.Z)
		{
			continue;
		}
		if (Box.ExpandBy(FVector(0.0, 0.0, 1.0)).ComputeSquaredDistanceToPoint(TargetM) * 1e4 >= Best)
		{
			continue;
		}
		FVector On;
		if (!Recast.GetClosestPointOnPoly(Islands.Refs[static_cast<int32>(i)], Target, On))
		{
			continue;
		}
		const double D = FVector::DistSquared(On, Target);
		if (D < Best)
		{
			Best = D;
			Out = On;
			bFound = true;
		}
	}
	return bFound;
}

// After a teleport the camera boom would ease its lag in from where the pawn stood before, sweeping the
// view across the map, and a cutscene starting there would blend in from that sweep. Settle it on the new
// spot; the boom's lag tuning is untouched.
void SnapExploreBoomLag(const AActor& Pawn)
{
	if (USpringArmComponent* Boom = Pawn.FindComponentByClass<USpringArmComponent>())
	{
		const FVector ArmOrigin = Boom->GetComponentLocation() + Boom->TargetOffset;
		Boom->PreviousArmOrigin = ArmOrigin;
		Boom->PreviousDesiredLoc = ArmOrigin;
	}
}
}

AFTExploreDirector::AFTExploreDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Exploration = ftcore::demo::makeExploration();
	Guards = ftcore::demo::makeGuards();
}

double AFTExploreDirector::Now() const
{
	return FPlatformTime::Seconds();
}

void AFTExploreDirector::BeginPlay()
{
	Super::BeginPlay();
	PilotPath.Reserve(64);
	bAutoPlay = bAutoPlay || FParse::Param(FCommandLine::Get(), TEXT("ExploreAutoPlay"));
	bQuitOnComplete = bQuitOnComplete || FParse::Param(FCommandLine::Get(), TEXT("ExploreQuitOnComplete"));

	Exploration = ftcore::demo::makeExploration();
	Guards = ftcore::demo::makeGuards();
	StartTime = Now();
	FindSpots();
	CacheText();
	RestoreFromCarry();
	BeginWorld();
	BeginOverworld();
	BeginStoryWalk();
	for (int32 I = 0; I < static_cast<int32>(Exploration.objectiveCount()); ++I)
	{
		UpdateObjectiveText(I);
	}
	if (bAutoPlay)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: autoplay on."));
	}
	UWorld* World = GetWorld();
	if (World && !Overworld && !bOverworld)
	{
		// The beat's next field step, its CG steps and its arena fights can play on this map (FTStepHost.h).
		if (UFTStepHostSubsystem* Hosts = World->GetSubsystem<UFTStepHostSubsystem>())
		{
			Hosts->Register(EFTStepHost::Field, FFTStartStepHere::CreateUObject(this, &AFTExploreDirector::StartStepHere));
		}
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		CutsceneDirector = World->SpawnActor<AFTCutsceneDirector>(GetActorLocation(), FRotator::ZeroRotator, Spawn);
		if (CutsceneDirector)
		{
			CutsceneDirector->HostSteps(true, true);
		}
		ArenaHost = World->SpawnActor<AFTArenaHost>(GetActorLocation(), FRotator::ZeroRotator, Spawn);
		if (ArenaHost)
		{
			ArenaHost->HostSteps(this);
		}
		const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
		const FFTStoryCarry* Step = (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone) ? &GI->StoryCarry : nullptr;
		const bool bArenaStep = Step && Step->Screen == ftcore::GameplayScreen::Battle && Step->Combat == ftcore::CombatKind::Arena;
		if (Step && (Step->Screen == ftcore::GameplayScreen::Cutscene || bArenaStep))
		{
			// Opened for a CG step or an arena fight (FTStepHost.h): the lead starts at the scene's district
			// spawn and the step's host begins once it stands there (Tick).
			bStepCarry = true;
			PendingHost = bArenaStep ? EFTStepHost::Arena : EFTStepHost::Cutscene;
			if (Layout)
			{
				PlaceInDistrict(GI->StoryCarry, bArenaStep ? TEXT("arena step") : TEXT("CG step"));
			}
		}
		else if (!Step && FParse::Param(FCommandLine::Get(), TEXT("ArenaHere")))
		{
			PendingHost = EFTStepHost::Arena;
		}
	}
	// Back from the last fight: everything may be done already.
	CheckComplete();
}

void AFTExploreDirector::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UFTStepHostSubsystem* Hosts = GetWorld() ? GetWorld()->GetSubsystem<UFTStepHostSubsystem>() : nullptr)
	{
		Hosts->Unregister(EFTStepHost::Field, this);
	}
	if (ArenaHost)
	{
		ArenaHost->Destroy();
		ArenaHost = nullptr;
	}
	Super::EndPlay(Reason);
}

void AFTExploreDirector::FindSpots()
{
	if (!WorldId.IsEmpty() || bOverworld)
	{
		return; // a big world's map has no demo spots
	}
	const int32 Count = static_cast<int32>(Exploration.count());
	bool bHaveAll = Spots.Num() == Count;
	for (int32 I = 0; bHaveAll && I < Count; ++I)
	{
		bHaveAll = Spots[I] && Spots[I]->SpotId == I;
	}
	if (bHaveAll)
	{
		return;
	}
	Spots.Reset();
	Spots.SetNum(Count);
	for (TActorIterator<AFTExploreSpot> It(GetWorld()); It; ++It)
	{
		if (Spots.IsValidIndex(It->SpotId))
		{
			Spots[It->SpotId] = *It;
		}
	}
	for (int32 I = 0; I < Count; ++I)
	{
		if (!Spots[I])
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("ExploreDemo: no spot actor for spot %d."), I);
		}
	}
}

AFTExploreSpot* AFTExploreDirector::GetSpot(int32 Id) const
{
	return Spots.IsValidIndex(Id) ? Spots[Id].Get() : nullptr;
}

void AFTExploreDirector::CacheText()
{
	const int32 Objectives = static_cast<int32>(Exploration.objectiveCount());
	ObjectiveText.SetNum(Objectives);
	const int32 Count = static_cast<int32>(Exploration.count());
	PromptText.SetNum(Count);
	LockedText.SetNum(Count);
	for (int32 Id = 0; Id < Count; ++Id)
	{
		const ftcore::Interactable& It = Exploration.item(Id);
		PromptText[Id] = KindPrompt(It.kind);
		// Locked: name the first objective it waits on.
		LockedText[Id].Reset();
		for (int32 O = 0; O < Objectives; ++O)
		{
			if ((It.needs >> O) & 1u)
			{
				LockedText[Id] = FString::Printf(TEXT("Locked: %s first"), kObjectiveNames[O]);
				break;
			}
		}
	}
}

void AFTExploreDirector::UpdateObjectiveText(int32 Objective)
{
	if (!ObjectiveText.IsValidIndex(Objective))
	{
		return;
	}
	const int32 Required = Exploration.required(Objective);
	if (Required > 1)
	{
		ObjectiveText[Objective] = FString::Printf(TEXT("%s  %d/%d"), kObjectiveNames[Objective], Exploration.progress(Objective), Required);
	}
	else
	{
		ObjectiveText[Objective] = kObjectiveNames[Objective];
	}
}

const FString& AFTExploreDirector::GetObjectiveText(int32 Objective) const
{
	static const FString Empty;
	return ObjectiveText.IsValidIndex(Objective) ? ObjectiveText[Objective] : Empty;
}

const FString& AFTExploreDirector::GetPrompt() const
{
	static const FString Empty;
	static const FString BackToStory(TEXT("E  Back to the story"));
	static const FString UseMarker(TEXT("E  Use the marker"));
	if (bStoryWalk)
	{
		if (bStoryReturning || !ftcore::demo::fieldStepUsable(bFieldReached, FieldElapsed))
		{
			return Empty;
		}
		return bFieldReached ? UseMarker : BackToStory;
	}
	if (Focus < 0 || !PromptText.IsValidIndex(Focus))
	{
		return Empty;
	}
	return Exploration.isAvailable(Focus) ? PromptText[Focus] : LockedText[Focus];
}

double AFTExploreDirector::GetNoticeAge() const
{
	return Notice.IsEmpty() ? 1e9 : Now() - NoticeTime;
}

double AFTExploreDirector::GetBannerAge() const
{
	return Banner.IsEmpty() ? 1e9 : Now() - BannerTime;
}

void AFTExploreDirector::SetNotice(const FString& Text)
{
	Notice = Text;
	NoticeTime = Now();
}

void AFTExploreDirector::SetBanner(const FString& Text)
{
	Banner = Text;
	BannerTime = Now();
}

AFTExploreCharacter* AFTExploreDirector::GetPlayerCharacter() const
{
	return Cast<AFTExploreCharacter>(UGameplayStatics::GetPlayerPawn(this, 0));
}

void AFTExploreDirector::RestoreFromCarry()
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI || !GI->ExploreCarry.bValid)
	{
		return;
	}
	FFTExploreCarry& Carry = GI->ExploreCarry;
	if (Carry.ReturnMap != UGameplayStatics::GetCurrentLevelName(this, true))
	{
		return;
	}
	Exploration.load(Carry.State);
	if (Carry.bWon && Carry.Encounter >= 0)
	{
		Exploration.clearEncounter(Carry.Encounter);
		SetBanner(TEXT("Ambush cleared"));
		UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: back from the fight, ambush cleared."));
	}
	Guards.load(Carry.Guards);
	if (Carry.Guard >= 0)
	{
		if (Carry.bWon)
		{
			Guards.defeat(Carry.Guard);
			SetBanner(TEXT("Guard defeated"));
			UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: back from the fight, guard %d defeated."), Carry.Guard);
		}
		else
		{
			Guards.reset(Carry.Guard);
		}
	}
	// Nobody pounces the moment the field opens again.
	Guards.holdOff(ftcore::demo::kGuardHoldOffSeconds);
	for (int32 Id = 0; Id < static_cast<int32>(Exploration.count()); ++Id)
	{
		if (Exploration.isUsed(Id))
		{
			if (AFTExploreSpot* Spot = GetSpot(Id))
			{
				Spot->MarkUsed(true);
			}
		}
	}
	StartTime = Now() - Carry.Seconds;
	bRestorePending = true;
	Carry.bValid = false;
	// The pawn may not exist yet; Tick moves it once it does.
	if (AFTExploreCharacter* Player = GetPlayerCharacter())
	{
		Player->SetActorLocationAndRotation(Carry.Location, Carry.Rotation, false, nullptr, ETeleportType::TeleportPhysics);
		SnapExploreBoomLag(*Player);
		bRestorePending = false;
	}
}

void AFTExploreDirector::BeginStoryWalk()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (Overworld || !GI || !GI->StoryCarry.bValid || GI->StoryCarry.bDone || GI->StoryCarry.Screen != ftcore::GameplayScreen::Field)
	{
		return;
	}
	bStoryWalk = true;
	bAutoPlay = bAutoPlay || GI->StoryCarry.bAutoPlay;
	StoryWalkStart = Now();
	for (const TObjectPtr<AFTExploreSpot>& Spot : Spots)
	{
		if (Spot)
		{
			Spot->SetActorHiddenInGame(true);
			Spot->SetActorEnableCollision(false);
		}
	}
	FieldElapsed = 0.f;
	bFieldReached = false;
	const FFTStoryCarry& Carry = GI->StoryCarry;
	const FTCHARToUTF8 SceneUtf8(*Carry.Scene);
	const size_t GoalIndex = ftcore::demo::fieldGoalIndex(std::string_view(SceneUtf8.Get(), static_cast<size_t>(SceneUtf8.Length())), Carry.Beat, Carry.Step);
	FieldGoal = ftcore::demo::kFieldGoals[GoalIndex];
	bFieldHasVia = false;
	bFieldViaDone = false;
	bool bFromTable = false;
	{
		// Data/Field/goals.json: this step's own marker (and waypoint); the hashed default when the
		// file or the entry is missing.
		ftcore::FieldGoalTable Table;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Field/goals.json");
		TArray<uint8> Bytes;
		if (IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File))
		{
			std::string Error;
			if (ftcore::importFieldGoalTable(FT::BytesAsText(Bytes), Table, &Error))
			{
				const ftcore::FieldGoalEntry* Entry = Table.findEntry(std::string_view(SceneUtf8.Get(), static_cast<size_t>(SceneUtf8.Length())),
					static_cast<uint32>(FMath::Max(Carry.Beat, 0)), static_cast<uint32>(FMath::Max(Carry.Step, 0)));
				if (Entry)
				{
					FieldGoal = Entry->goal;
					FieldVia = Entry->via;
					bFieldHasVia = Entry->hasVia;
					bFromTable = true;
				}
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("ExploreDemo: Data/Field/goals.json refused; using the default markers: %s"), *FT::ToFString(Error).TrimEnd());
			}
		}
	}
	if (Layout)
	{
		// A big world: the goal and waypoint are in the district's frame; the player starts at its spawn.
		PlaceInDistrict(Carry, TEXT("field step"));
		const size_t D = static_cast<size_t>(WorldDistrict);
		FieldGoal = Layout->toWorld(D, FieldGoal);
		FieldVia = Layout->toWorld(D, FieldVia);
	}
	SpawnFieldGoal();
	FieldObjective = Carry.StepLabel.IsEmpty()
		? FString::Printf(TEXT("Reach the marker  %s"), *Carry.Kind)
		: FString::Printf(TEXT("Reach the marker  %s  %s"), *Carry.Kind, *Carry.StepLabel);
	SetBanner(TEXT("Reach the marker"));
	if (bAutoPlay && IsAlphaFast())
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 8.f);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: field step for %s at %s beat %d step %d, marker %d%s."), *Carry.Kind, *Carry.Scene, Carry.Beat, Carry.Step, static_cast<int32>(GoalIndex), bFromTable ? TEXT(" (table)") : TEXT(""));
}

void AFTExploreDirector::PlaceInDistrict(const FFTStoryCarry& Carry, const TCHAR* What)
{
	const int32 Count = static_cast<int32>(Layout->districtCount());
	const bool bPlaced = Carry.World == WorldId && Carry.District >= 0 && Carry.District < Count;
	WorldDistrict = bPlaced ? Carry.District : 0;
	if (!bPlaced)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("World %s: the %s's scene %s isn't placed here; using district 0."), *WorldId, What, *Carry.Scene);
	}
	QueueDistrictSpawn(WorldDistrict);
	// The maps are photoreal unless built with -Style=, so a carried style lands only on a style controller.
	int32 Styled = 0;
	if (!Carry.Style.IsEmpty())
	{
		for (TActorIterator<AFTStyleController> It(GetWorld()); It; ++It)
		{
			// Before its BeginPlay the controller applies StyleSpec itself; after it, switch now.
			It->StyleSpec = Carry.Style;
			if (It->HasActorBegunPlay())
			{
				It->SetStyleSpec(Carry.Style);
			}
			++Styled;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("World %s: %s in district %d (%s%s)."), *WorldId, What, WorldDistrict,
		Styled > 0 ? TEXT("style ") : TEXT("photoreal look"), Styled > 0 ? *Carry.Style : TEXT(""));
}

void AFTExploreDirector::SpawnFieldGoal()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AFTExploreSpot> It(World); It; ++It)
	{
		if (It->SpotId == ftcore::demo::kFieldGoalSpotId)
		{
			GoalSpot = *It;
		}
	}
	// On a big world the marker stands on its district's floor (the goals lie on the pad's flat).
	const FTransform At(FVector(FieldGoal.x * 100.0, FieldGoal.y * 100.0, DistrictFloors.IsValidIndex(WorldDistrict) ? DistrictFloors[WorldDistrict] : 0.0));
	if (GoalSpot)
	{
		GoalSpot->SetActorTransform(At);
	}
	else
	{
		GoalSpot = World->SpawnActorDeferred<AFTExploreSpot>(AFTExploreSpot::StaticClass(), At, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (GoalSpot)
		{
			GoalSpot->SpotId = ftcore::demo::kFieldGoalSpotId;
			GoalSpot->FinishSpawning(At);
		}
	}
	if (GoalSpot)
	{
		GoalSpot->SetActorHiddenInGame(false);
	}
}

void AFTExploreDirector::TickFollowers(AFTExploreCharacter& Player, float DeltaSeconds)
{
	if (!bFollowersSpawned)
	{
		SpawnFollowers(Player);
		if (bPartyHeld)
		{
			for (AFTPartyFollower* Follower : Followers)
			{
				if (Follower)
				{
					Follower->SetHeld(true);
					Follower->SetActorHiddenInGame(Player.IsPartyHidden());
				}
			}
		}
	}
	else if (!bPartyHeld)
	{
		PlaceFollowers(Player, DeltaSeconds);
	}
}

void AFTExploreDirector::SpawnFollowers(AFTExploreCharacter& Player)
{
	bFollowersSpawned = true;
	UWorld* World = GetWorld();
	const TArray<FFTLeadLook>& Party = Player.PartyLooks;
	const int32 Lead = FMath::Max(0, Player.GetLeadIndex());
	const size_t Members = Party.IsEmpty() ? ftcore::demo::kPartyNames.size() : static_cast<size_t>(Party.Num());
	const size_t Count = FMath::Min(Members - 1u, ftcore::PartyTrail::kMaxFollowers);
	const FVector2D Facing = Player.GetFacing2D();
	Trail.reset(ToGround(Player.GetActorLocation()), {static_cast<float>(Facing.X), static_cast<float>(Facing.Y)},
		static_cast<float>(Count) * ftcore::PartyTrail::kFollowerSpacing);
	Followers.Reserve(static_cast<int32>(Count));
	TurnCursors.Init(0u, static_cast<int32>(Count));
	JumpCursors.Init(0u, static_cast<int32>(Count));
	// Each a step further back than the one before, on alternate sides of the lead's path.
	Slots.SetNum(static_cast<int32>(Count));
	for (int32 I = 0; I < Slots.Num(); ++I)
	{
		ftcore::PartySlotTuning& Tune = Slots[I].tuning();
		Tune.spacing *= static_cast<float>(I + 1);
		Tune.side *= (I % 2 == 0) ? 1.f : -1.f;
		Slots[I].reset(Trail);
	}
	for (size_t I = 0; World && I < Count; ++I)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AFTPartyFollower* Follower = World->SpawnActor<AFTPartyFollower>(AFTPartyFollower::StaticClass(), Player.GetActorTransform(), Params);
		if (!Follower)
		{
			continue;
		}
		// The members after the lead, in order.
		FFTLeadLook Default;
		const FFTLeadLook* Look = &Default;
		if (Party.IsEmpty())
		{
			Default.Name = UTF8_TO_TCHAR(ftcore::demo::kPartyNames[I + 1u]);
		}
		else
		{
			Look = &Party[(Lead + 1 + static_cast<int32>(I)) % Party.Num()];
		}
		if (Follower->Setup(*Look, &Player))
		{
			Follower->SetActorHiddenInGame(Player.IsPartyHidden());
		}
		// On a map with a sea it keeps out of it: no follower stands on or over the water (AFTPartyFollower::Place).
		if (bHasSeaLevel)
		{
			Follower->SetSea(SeaLevel);
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("ExploreDemo: no mesh for follower %s."), *Look->Name);
		}
		Followers.Add(Follower);
	}
	PlaceFollowers(Player, 0.f);
	// -LocoWatchFollower: the camera follows the first follower instead, for locomotion captures.
	if (FParse::Param(FCommandLine::Get(), TEXT("LocoWatchFollower")) && !Followers.IsEmpty() && Followers[0])
	{
		bWatchFollower = true;
		Player.WatchFrom(Followers[0]->GetRootComponent());
	}
	bLogGap = FParse::Param(FCommandLine::Get(), TEXT("LocoLog"));
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: %d following the lead%s."), Followers.Num(),
		Player.IsPartyHidden() ? TEXT(" (hidden)") : TEXT(""));
}

void AFTExploreDirector::ClearFollowers()
{
	for (AFTPartyFollower* Follower : Followers)
	{
		if (Follower)
		{
			Follower->Destroy();
		}
	}
	Followers.Reset();
	bFollowersSpawned = false;
}

void AFTExploreDirector::OnLeadChanged(const FString& Lead)
{
	// The tick that places them spawns them again behind the new lead.
	ClearFollowers();
	SetNotice(FString::Printf(TEXT("Lead: %s"), *Lead));
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: lead is %s."), *Lead);
}

void AFTExploreDirector::SetPartyHidden(bool bHide)
{
	for (AFTPartyFollower* Follower : Followers)
	{
		if (Follower)
		{
			Follower->SetActorHiddenInGame(bHide);
		}
	}
	SetNotice(bHide ? TEXT("Party hidden") : TEXT("Party shown"));
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: party %s."), bHide ? TEXT("hidden") : TEXT("shown"));
}

void AFTExploreDirector::SetPartyHeld(bool bHold)
{
	if (bHold == bPartyHeld)
	{
		return;
	}
	bPartyHeld = bHold;
	const AFTExploreCharacter* Player = GetPlayerCharacter();
	for (AFTPartyFollower* Follower : Followers)
	{
		if (Follower)
		{
			Follower->SetHeld(bHold);
			if (bHold)
			{
				Follower->SetActorHiddenInGame(Player && Player->IsPartyHidden());
			}
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: party %s."), bHold ? TEXT("held") : TEXT("let go"));
	if (bHold)
	{
		LeadFacedAgo = -1.f;
		return;
	}
	if (!Player || !bFollowersSpawned)
	{
		return;
	}
	// The trail starts over behind the lead as it stands, long enough to reach the farthest follower (as far as it
	// keeps), and each slot takes it up at the point nearest where its follower stands.
	const ftcore::Vec2 LeadAt = ToGround(Player->GetActorLocation());
	float Seed = static_cast<float>(Slots.Num()) * ftcore::PartyTrail::kFollowerSpacing;
	for (const AFTPartyFollower* Follower : Followers)
	{
		if (Follower)
		{
			const ftcore::Vec2 At = ToGround(Follower->GetActorLocation());
			Seed = FMath::Max(Seed, FMath::Sqrt(FMath::Square(At.x - LeadAt.x) + FMath::Square(At.y - LeadAt.y)) + 1.f);
		}
	}
	Seed = FMath::Min(Seed, static_cast<float>(ftcore::PartyTrail::kCapacity - 1u) * ftcore::PartyTrail::kSampleSpacing);
	const FVector2D Facing = Player->GetFacing2D();
	Trail.reset(LeadAt, {static_cast<float>(Facing.X), static_cast<float>(Facing.Y)}, Seed);
	for (int32 I = 0; I < Slots.Num(); ++I)
	{
		const AFTPartyFollower* Follower = Followers.IsValidIndex(I) ? Followers[I].Get() : nullptr;
		if (Follower)
		{
			Slots[I].resetAt(Trail, ToGround(Follower->GetActorLocation()));
		}
		else
		{
			Slots[I].reset(Trail);
		}
	}
}

const AFTCutsceneDirector* AFTExploreDirector::GetCutsceneDirector() const
{
	return CutsceneDirector;
}

bool AFTExploreDirector::GetDistrictSpawn(int32 District, FVector2D& Spot, float& Yaw) const
{
	if (!Layout || District < 0 || District >= static_cast<int32>(Layout->districtCount()))
	{
		return false;
	}
	const size_t D = static_cast<size_t>(District);
	const ftcore::Vec2 Origin = Layout->district(D).origin;
	Spot = FVector2D(Origin.x * 100.0, Origin.y * 100.0);
	Yaw = FMath::RadiansToDegrees(Layout->spawnYaw(D));
	return true;
}

bool AFTExploreDirector::QueueDistrictSpawn(int32 District)
{
	FVector2D Spot;
	float Yaw = 0.f;
	if (!GetDistrictSpawn(District, Spot, Yaw))
	{
		return false;
	}
	// The spawn is the district's origin: on a map that places its own districts, on the floor the builder found
	// there; on a generated world, on its pad's flat (the district's floor).
	double Floor = 0.0;
	bWorldSpawnOnFloor = true;
	if (bDistrictsPinned && DistrictSpawns.IsValidIndex(District))
	{
		Floor = DistrictSpawns[District].Z;
	}
	else if (DistrictFloors.IsValidIndex(District))
	{
		Floor = DistrictFloors[District];
	}
	else
	{
		bWorldSpawnOnFloor = false;
	}
	WorldSpawn = FVector(Spot.X, Spot.Y, Floor);
	WorldSpawnRotation = FRotator(0.f, Yaw, 0.f);
	bWorldSpawnPending = true;
	return true;
}

void AFTExploreDirector::PlaceFollowers(AFTExploreCharacter& Player, float DeltaSeconds)
{
	const float LeadSpeed = static_cast<float>(Player.GetVelocity().Size2D()) / 100.f;
	Trail.push(ToGround(Player.GetActorLocation()), LeadSpeed);
	// The lead's running turns and turning starts go on the trail, for each follower to play where the lead did.
	if (const UFTLocomotionAnimInstance* LeadAnim = Player.GetMesh() ? Cast<UFTLocomotionAnimInstance>(Player.GetMesh()->GetAnimInstance()) : nullptr)
	{
		const ftcore::Locomotion& Lead = LeadAnim->GetLocomotion();
		if (Lead.turnsBegun() != LeadTurns)
		{
			LeadTurns = Lead.turnsBegun();
			if (Lead.turning() && !ftcore::isLocoStandTurn(Lead.turnClip()))
			{
				Trail.markTurn(Lead.turnAngle());
			}
			else if (Lead.turning())
			{
				LeadFacedYaw = static_cast<float>(Player.GetActorRotation().Yaw) + Lead.turnAngle();
				LeadFacedAgo = 0.f;
			}
		}
	}
	// The lead's jumps (off the ground going up, not a step off a ledge) go on the trail too.
	const UCharacterMovementComponent* LeadMove = Player.GetCharacterMovement();
	const bool bInAir = LeadMove && LeadMove->IsFalling();
	if (bInAir && !bLeadInAir && Player.GetVelocity().Z > 0.0)
	{
		Trail.markJump();
	}
	bLeadInAir = bInAir;
	if (LeadFacedAgo >= 0.f)
	{
		LeadFacedAgo += DeltaSeconds;
	}
	// Braking to a stand, how far the lead has left (m), for the followers to plan their own stops by.
	float LeadWant = -1.f, LeadStop = -1.f;
	if (LeadMove)
	{
		UFTLocomotionAnimInstance::MovementIntent(*LeadMove, LeadWant, LeadStop);
	}
	LeadStop = LeadWant == 0.f && LeadStop >= 0.f ? LeadStop * 0.01f : -1.f;
	const float FeetZ = static_cast<float>(Player.GetActorLocation().Z - Player.GetSimpleCollisionHalfHeight());
	const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0);
	for (int32 I = 0; I < Followers.Num(); ++I)
	{
		if (AFTPartyFollower* Follower = Followers[I]; Follower && Slots.IsValidIndex(I))
		{
			// Its slot moves on its own along the trail: it sets off and stops after the lead, and starts and
			// stops its own legs by where its slot is heading.
			ftcore::PartySlot& Slot = Slots[I];
			Slot.step(Trail, LeadSpeed, DeltaSeconds, LeadStop);
			const float Arc = Slot.arc();
			const float StopDistance = Slot.stopDistance();
			if (TurnCursors.IsValidIndex(I))
			{
				if (const float Delta = Trail.turnsReached(Arc, TurnCursors[I]); Delta != 0.f)
				{
					Follower->ReplayTurn(Delta);
				}
			}
			if (JumpCursors.IsValidIndex(I) && LeadMove && Trail.jumpsReached(Arc, JumpCursors[I]) > 0)
			{
				Follower->ReplayJump(LeadMove->JumpZVelocity, static_cast<float>(-LeadMove->GetGravityZ()));
			}
			// When the lead turns on the spot to face something, each follower looks the same way a beat later.
			const float FaceAfter = kFollowerFaceDelay * static_cast<float>(I + 1);
			if (LeadFacedAgo >= FaceAfter && LeadFacedAgo - DeltaSeconds < FaceAfter)
			{
				Follower->FaceTo(LeadFacedYaw);
			}
			const ftcore::Vec2 At = Slot.position();
			// Where the lead walked at its place on the trail: a slot out over the water is held short of the sea on the
			// way out from there.
			const ftcore::Vec2 Path = Trail.pointBehind(Arc);
			const FVector2D PathCm(Path.x * 100.0, Path.y * 100.0);
			Follower->Place(FVector2D(At.x * 100.0, At.y * 100.0), FeetZ, DeltaSeconds, Slot.wantSpeed() * 100.f,
				StopDistance < 0.f ? -1.f : StopDistance * 100.f, &PathCm);
			const bool bInLens = Camera && !(bWatchFollower && I == 0)
				&& FVector::Dist2D(Camera->GetCameraLocation(), Follower->GetActorLocation())
					< kFollowerCameraShare * FVector::Dist2D(Camera->GetCameraLocation(), Player.GetActorLocation());
			// Hidden too while the party is (P or LB), walking the trail all the same.
			Follower->SetActorHiddenInGame(bInLens || Player.IsPartyHidden());
			if (bLogGap && I == 0 && DeltaSeconds > 0.f)
			{
				NearestGap = FMath::Min(NearestGap, static_cast<float>(FVector::Dist2D(Player.GetActorLocation(), Follower->GetActorLocation())));
				if ((GapLogTimer -= DeltaSeconds) <= 0.f)
				{
					UE_LOG(LogForgottenThird, Display, TEXT("Party: follower 0 came within %.0f cm of the lead (slot %.0f cm behind on the trail)."),
						NearestGap, Arc * 100.f);
					GapLogTimer = 1.f;
					NearestGap = TNumericLimits<float>::Max();
				}
			}
		}
	}
}

void AFTExploreDirector::SpawnGuards(AFTExploreCharacter& Player)
{
	bGuardsSpawned = true;
	UWorld* World = GetWorld();
	const int32 Count = static_cast<int32>(Guards.count());
	GuardActors.SetNum(Count);
	int32 OnPatrol = 0;
	for (int32 I = 0; World && I < Count; ++I)
	{
		if (!Guards.active(I))
		{
			continue;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AFTFieldGuard* Guard = World->SpawnActor<AFTFieldGuard>(AFTFieldGuard::StaticClass(), FTransform::Identity, Params);
		if (Guard)
		{
			Guard->Setup(&Player);
			++OnPatrol;
		}
		GuardActors[I] = Guard;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: %d guards on patrol."), OnPatrol);
}

int32 AFTExploreDirector::TickGuards(AFTExploreCharacter& Player, float DeltaSeconds)
{
	if (!bGuardsSpawned)
	{
		SpawnGuards(Player);
	}
	const ftcore::Vec2 Pos = ToGround(Player.GetActorLocation());
	// Core does the cone and the distances; a line trace answers for the walls, only for the guards
	// close enough to need one.
	uint32 Sight = 0;
	for (int32 I = 0; I < GuardActors.Num(); ++I)
	{
		const AFTFieldGuard* Guard = GuardActors[I];
		if (Guard && Guards.inSenseRange(I, Pos) && Guard->CanSee(Player))
		{
			Sight |= 1u << I;
		}
	}
	ftcore::GuardEvents Events;
	const int32 Caught = Guards.step(Pos, Sight, DeltaSeconds, &Events);
	const float FeetZ = static_cast<float>(Player.GetActorLocation().Z - Player.GetSimpleCollisionHalfHeight());
	for (int32 I = 0; I < GuardActors.Num(); ++I)
	{
		AFTFieldGuard* Guard = GuardActors[I];
		if (!Guard)
		{
			continue;
		}
		if (!Guards.active(I))
		{
			Guard->SetActorHiddenInGame(true);
			continue;
		}
		const ftcore::Vec2 At = Guards.position(I);
		const ftcore::Vec2 Facing = Guards.facing(I);
		Guard->Place(FVector2D(At.x * 100.0, At.y * 100.0), FVector2D(Facing.x, Facing.y), Guards.state(I), FeetZ);
		const uint8 Bits = Events.bits[static_cast<size_t>(I)];
		if (Bits & ftcore::kGuardNoticed)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: guard %d noticed you."), I);
		}
		if (Bits & ftcore::kGuardChaseStarted)
		{
			SetNotice(TEXT("A guard has spotted you"));
			UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: guard %d gives chase."), I);
		}
		if (Bits & ftcore::kGuardLost)
		{
			SetNotice(TEXT("The guard lost you"));
			UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: guard %d lost you."), I);
		}
	}
	return Caught;
}

void AFTExploreDirector::TickStoryWalk(AFTExploreCharacter& Player, float DeltaSeconds)
{
	FieldElapsed += DeltaSeconds;
	TickFollowers(Player, DeltaSeconds);
	const ftcore::Vec2 Pos = ToGround(Player.GetActorLocation());
	const FVector2D Facing = Player.GetFacing2D();
	const bool bReached = ftcore::demo::fieldGoalInReach(Pos, {static_cast<float>(Facing.X), static_cast<float>(Facing.Y)}, FieldGoal);
	if (bReached != bFieldReached)
	{
		bFieldReached = bReached;
		if (GoalSpot)
		{
			GoalSpot->SetFocused(bReached);
		}
	}
	if (!bAutoPlay || bStoryReturning)
	{
		return;
	}
	if (bFieldReached)
	{
		Interact();
		return;
	}
	const ftcore::Vec2 Aim = ftcore::demo::fieldPilotTarget(Pos, FieldGoal, FieldVia, bFieldHasVia, bFieldViaDone);
	// The waypoint and the marker count as separate targets for the stuck detection.
	PilotWalk(Player, Pos, Aim, bFieldHasVia && !bFieldViaDone ? ftcore::demo::kFieldGoalSpotId + 1 : ftcore::demo::kFieldGoalSpotId, DeltaSeconds);
}

void AFTExploreDirector::ReturnToStory()
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (bStoryReturning || !GI || !GI->StoryCarry.bValid)
	{
		return;
	}
	bStoryReturning = true;
	GI->StoryCarry.bDone = true;
	GI->StoryCarry.bWon = true;
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: field step done at %s beat %d step %d after %.1f s."), *GI->StoryCarry.Scene, GI->StoryCarry.Beat, GI->StoryCarry.Step, FieldElapsed);
	// The beat's next step plays on this map when it can (a field step, or a CG step around the lead).
	if (FTStep::ContinueHere(this))
	{
		if (GoalSpot)
		{
			GoalSpot->SetActorHiddenInGame(true);
		}
		return;
	}
	SetBanner(TEXT("Back to the story"));
	FTimerHandle Timer;
	GetWorldTimerManager().SetTimer(Timer, this, &AFTExploreDirector::OpenStoryMap, 0.8f, false);
}

void AFTExploreDirector::OpenStoryMap()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid)
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
		FTStep::OpenMap(this, GI->StoryCarry.ReturnMap);
	}
}

void AFTExploreDirector::StartStepHere()
{
	// The autopilot starts afresh, and the lead walks on from where it stands: no move to the
	// district's spawn.
	PilotTarget = -1;
	PilotBestDist = 0.f;
	PilotStuck = 0.f;
	PilotStall = 0.f;
	PilotPath.Reset();
	PilotPathTarget = -1;
	PilotPathNext = -1;
	PilotPathRefreshAt = 0.0;
	bStoryReturning = false;
	bStepCarry = false;
	PendingHost = EFTStepHost::None;
	BeginStoryWalk();
	bWorldSpawnPending = false;
	bFieldReached = false;
	if (GoalSpot)
	{
		GoalSpot->SetFocused(false);
	}
}

void AFTExploreDirector::Interact()
{
	if (Overworld)
	{
		const AFTExploreCharacter* Player = GetPlayerCharacter();
		const int32 Node = Player ? Overworld->nodeAt(ToGround(Player->GetActorLocation())) : -1;
		if (Node < 0 || bEntering)
		{
			return;
		}
		if (bHop ? Node == Hop.to : ((OpenWorlds >> Node) & 1u) != 0)
		{
			EnterWorld(Node);
		}
		else if (!bHop)
		{
			SetNotice(FString::Printf(TEXT("%s  closed"), *WorldName(Node)));
		}
		return;
	}
	if (bStoryWalk)
	{
		if (ftcore::demo::fieldStepUsable(bFieldReached, FieldElapsed))
		{
			ReturnToStory();
		}
		return;
	}
	if (Focus < 0 && !bComplete)
	{
		// A spot in reach off to the side: turn on the spot to face it, then use it (TickFace).
		if (const AFTExploreCharacter* Player = GetPlayerCharacter())
		{
			const int32 Near = Exploration.focus(ToGround(Player->GetActorLocation()), {});
			if (Near >= 0)
			{
				FaceTarget = Near;
			}
		}
		return;
	}
	if (Focus < 0 || bComplete)
	{
		return;
	}
	const int32 Id = Focus;
	const ftcore::InteractKind Kind = Exploration.item(Id).kind;
	const ftcore::InteractResult Result = Exploration.interact(Id);
	if (Result == ftcore::InteractResult::Locked)
	{
		SetNotice(LockedText[Id]);
		return;
	}
	if (Result != ftcore::InteractResult::Used)
	{
		return;
	}
	if (AFTExploreSpot* Spot = GetSpot(Id))
	{
		Spot->MarkUsed(false);
	}
	const int32 Objective = Exploration.item(Id).objective;
	UpdateObjectiveText(Objective);
	switch (Kind)
	{
	case ftcore::InteractKind::Talk:
		SetNotice(TEXT("Talked to Duy"));
		break;
	case ftcore::InteractKind::Pickup:
		SetNotice(FString::Printf(TEXT("Light shard  %d/%d"), Exploration.progress(Objective), Exploration.required(Objective)));
		break;
	case ftcore::InteractKind::Gate:
		SetNotice(TEXT("The gate opens"));
		break;
	default:
		break;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: used spot %d after %.1f s."), Id, Now() - StartTime);
	Focus = -1;
	CheckComplete();
}

void AFTExploreDirector::StartEncounter(int32 Id)
{
	PendingEncounter = Id;
	SetBanner(TEXT("Ambush!"));
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: ambush at spot %d after %.1f s."), Id, Now() - StartTime);
	FTimerHandle Timer;
	GetWorldTimerManager().SetTimer(Timer, this, &AFTExploreDirector::OpenBattle, kEncounterDelay, false);
}

void AFTExploreDirector::StartGuardFight(int32 Id)
{
	PendingGuard = Id;
	SetBanner(TEXT("Caught!"));
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: guard %d caught you after %.1f s."), Id, Now() - StartTime);
	FTimerHandle Timer;
	GetWorldTimerManager().SetTimer(Timer, this, &AFTExploreDirector::OpenBattle, kEncounterDelay, false);
}

void AFTExploreDirector::OpenBattle()
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("ExploreDemo: the game instance isn't UFTGameInstance, so the fight can't return here."));
		return;
	}
	FFTExploreCarry& Carry = GI->ExploreCarry;
	Carry.bValid = true;
	Carry.ReturnMap = UGameplayStatics::GetCurrentLevelName(this, true);
	Carry.State = Exploration.save();
	Carry.Encounter = PendingEncounter;
	Carry.Guard = PendingGuard;
	Carry.Guards = Guards.save();
	Carry.bWon = false;
	Carry.Seconds = Now() - StartTime;
	if (const AFTExploreCharacter* Player = GetPlayerCharacter())
	{
		Carry.Location = Player->GetActorLocation();
		Carry.Rotation = Player->GetActorRotation();
	}
	bBattleOpening = true;
	FTStep::OpenMap(this, BattleMap.ToString());
}

void AFTExploreDirector::CheckComplete()
{
	if (bComplete || !Exploration.allDone())
	{
		return;
	}
	bComplete = true;
	SetBanner(TEXT("Demo complete"));
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: COMPLETE after %.1f s."), Now() - StartTime);
	if (bQuitOnComplete)
	{
		FPlatformMisc::RequestExit(false, TEXT("ExploreDemo"));
	}
}

void AFTExploreDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	AFTExploreCharacter* Player = GetPlayerCharacter();
	if (!Player)
	{
		return;
	}
	if (bRestorePending)
	{
		if (const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
		{
			Player->SetActorLocationAndRotation(GI->ExploreCarry.Location, GI->ExploreCarry.Rotation, false, nullptr, ETeleportType::TeleportPhysics);
			SnapExploreBoomLag(*Player);
		}
		bRestorePending = false;
		CheckComplete();
	}
	if (bWorldSpawnPending)
	{
		bWorldSpawnPending = false;
		// Set down on the district's floor on a big world's ground; on a flat map the lead keeps its height.
		const double Z = bWorldSpawnOnFloor ? WorldSpawn.Z + Player->GetSimpleCollisionHalfHeight() + 2.0 : Player->GetActorLocation().Z;
		const FVector At(WorldSpawn.X, WorldSpawn.Y, Z);
		Player->SetActorLocationAndRotation(At, WorldSpawnRotation, false, nullptr, ETeleportType::TeleportPhysics);
		SnapExploreBoomLag(*Player);
		if (AController* Controller = Player->GetController())
		{
			Controller->SetControlRotation(WorldSpawnRotation);
		}
		if (bWorldSpawnOnFloor)
		{
			FloorLogAt = GetWorld()->GetTimeSeconds() + 1.0;
		}
	}
	if (Layout && FloorLogAt >= 0.0 && GetWorld()->GetTimeSeconds() >= FloorLogAt)
	{
		// A second after the lead is placed on a big world, in any step or walk: its feet against the floor of the district it
		// stands in, which agree to a few cm when the ground under it is the one the builder imported.
		FloorLogAt = -1.0;
		const int32 At = Layout->districtAt(ToGround(Player->GetActorLocation()));
		// On a map that places its own districts the spawn may stand on a deck above the plaza's floor.
		const FString SpawnFloor = bDistrictsPinned && DistrictSpawns.IsValidIndex(At)
			? FString::Printf(TEXT(" (its spawn's %.0f cm)"), DistrictSpawns[At].Z)
			: FString();
		UE_LOG(LogForgottenThird, Display, TEXT("World %s: the lead's feet at z %.0f cm in district %d, whose floor is %.0f cm%s."), *WorldId,
			Player->GetActorLocation().Z - Player->GetSimpleCollisionHalfHeight(), At, DistrictFloors.IsValidIndex(At) ? DistrictFloors[At] : 0.f,
			*SpawnFloor);
	}
	if (PendingHost != EFTStepHost::None)
	{
		// The lead stands at the spawn now (or where it started, when the district has none): the CG or the
		// fight this map was opened for begins around it, or on its own map when there is no host here.
		const EFTStepHost Host = PendingHost;
		PendingHost = EFTStepHost::None;
		const FString Stage = FTStep::ScreenMap(Host == EFTStepHost::Arena ? ftcore::GameplayScreen::Battle : ftcore::GameplayScreen::Cutscene);
		UFTStepHostSubsystem* Hosts = GetWorld()->GetSubsystem<UFTStepHostSubsystem>();
		if (!Hosts || !Hosts->StartHere(Host, Stage))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("ExploreDemo: no %s host for the step; opening %s."), FTStep::HostName(Host), *Stage);
			FTStep::OpenMap(this, Stage);
			return;
		}
	}
	if (Overworld)
	{
		TickOverworld(*Player, DeltaSeconds);
		return;
	}
	if (bStoryWalk)
	{
		TickStoryWalk(*Player, DeltaSeconds);
		return;
	}
	if (Layout)
	{
		// A big world has no demo spots or guards: a free walk, or the -WorldAutoPlay tour.
		TickWorld(*Player, DeltaSeconds);
		return;
	}
	if (bStepCarry)
	{
		// A CG or an arena fight plays here: no demo walk, guards or fights.
		return;
	}
	if (!Player->PartyLooks.IsEmpty())
	{
		// The demo walk has followers only with a party in config, so the default demo is unchanged.
		TickFollowers(*Player, DeltaSeconds);
	}
	if (bBattleOpening || PendingEncounter >= 0 || PendingGuard >= 0)
	{
		return;
	}

	const ftcore::Vec2 Pos = ToGround(Player->GetActorLocation());
	const FVector2D Facing = Player->GetFacing2D();
	const int32 NewFocus = bComplete ? -1 : Exploration.focus(Pos, {static_cast<float>(Facing.X), static_cast<float>(Facing.Y)});
	if (NewFocus != Focus)
	{
		if (AFTExploreSpot* Old = GetSpot(Focus))
		{
			Old->SetFocused(false);
		}
		if (AFTExploreSpot* New = GetSpot(NewFocus))
		{
			New->SetFocused(true);
		}
		Focus = NewFocus;
	}
	if (TickFace(*Player, Pos, DeltaSeconds))
	{
		return;
	}

	const int32 Caught = TickGuards(*Player, DeltaSeconds);
	if (Caught >= 0)
	{
		StartGuardFight(Caught);
		return;
	}
	const int32 Encounter = Exploration.encounterAt(Pos);
	if (Encounter >= 0)
	{
		StartEncounter(Encounter);
		return;
	}
	if (bAutoPlay && !bComplete)
	{
		TickAutoPlay(*Player, DeltaSeconds);
	}
}

void AFTExploreDirector::TickAutoPlay(AFTExploreCharacter& Player, float DeltaSeconds)
{
	const ftcore::Vec2 Pos = ToGround(Player.GetActorLocation());
	const int32 Target = Exploration.pilotTarget(Pos);
	if (Target < 0)
	{
		return;
	}
	if (Target == Focus && Exploration.item(Target).kind != ftcore::InteractKind::Encounter)
	{
		Interact();
		return;
	}
	if (Exploration.item(Target).kind != ftcore::InteractKind::Encounter && Exploration.focus(Pos, {}) == Target)
	{
		// In reach but not faced: stop and turn to it rather than circling in.
		FaceTarget = Target;
		return;
	}
	PilotWalk(Player, Pos, Exploration.item(Target).pos, Target, DeltaSeconds);
}

bool AFTExploreDirector::TickFace(AFTExploreCharacter& Player, ftcore::Vec2 Pos, float DeltaSeconds)
{
	if (FaceTarget < 0)
	{
		FaceTime = 0.f;
		return false;
	}
	FaceTime += DeltaSeconds;
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Player.GetMesh()->GetAnimInstance());
	// Moving again gives it up; the movement already under way when it began takes a frame or two to clear.
	const bool bMoving = FaceTime > kFaceGrace && Player.GetCharacterMovement()
		&& Player.GetCharacterMovement()->GetCurrentAcceleration().SizeSquared2D() > 1.0;
	if (bMoving || Exploration.focus(Pos, {}) != FaceTarget || !Anim)
	{
		FaceTarget = -1; // walked off or out of reach: give up
		if (Anim)
		{
			Anim->SetFace(false, 0.f);
		}
		return false;
	}
	if (Focus == FaceTarget)
	{
		FaceTarget = -1;
		Anim->SetFace(false, 0.f);
		Interact();
		return true;
	}
	const ftcore::Vec2 At = Exploration.item(FaceTarget).pos;
	Anim->SetFace(true, FMath::RadiansToDegrees(FMath::Atan2(At.y - Pos.y, At.x - Pos.x)));
	return true;
}

void AFTExploreDirector::PilotWalk(AFTExploreCharacter& Player, ftcore::Vec2 Pos, ftcore::Vec2 Goal, int32 Target, float DeltaSeconds,
	bool bUseNav, TOptional<double> GoalFloor, bool bPathProgress)
{
	const FVector2D To(Goal.x - Pos.x, Goal.y - Pos.y);
	const float Dist = static_cast<float>(To.Size());
	// The waypoint first: it refreshes the path the progress test reads.
	const ftcore::Vec2* Waypoint = bUseNav ? PilotWaypoint(Player, Pos, Goal, Target, GoalFloor) : nullptr;
	PilotPathLeft = bPathProgress ? PilotPathLeftFrom(Pos, Target) : -1.f;
	const bool bNewTarget = Target != PilotTarget;
	if (bNewTarget)
	{
		PilotTarget = Target;
		PilotBestDist = Dist;
		PilotBestPath = PilotPathLeft;
		PilotStall = 0.f;
	}
	else if (PilotBestPath < 0.f)
	{
		PilotBestPath = PilotPathLeft; // the leg's first full path sets the mark; it isn't a new best
	}
	// A new best: 10 cm closer in a straight line, or kPilotPathGain closer along a full path.
	const bool bCloser = !bNewTarget && Dist < PilotBestDist - 0.1f;
	const bool bAlong = !bNewTarget && PilotPathLeft >= 0.f && PilotPathLeft < PilotBestPath - kPilotPathGain;
	PilotBestDist = bCloser ? Dist : PilotBestDist;
	PilotBestPath = bAlong ? PilotPathLeft : PilotBestPath;
	if (bNewTarget || bCloser || bAlong)
	{
		PilotStuck = 0.f;
		bPilotGaveUp = false;
	}
	else
	{
		PilotStuck += DeltaSeconds;
	}
	// A stall is timed apart from the bests: under kPilotStallSpeed for kPilotStuckSeconds, whatever the path. After it
	// fires it stays negative (logged once) until the lead walks at speed again.
	const bool bSlow = Player.GetVelocity().Size2D() < kPilotStallSpeed * 100.0;
	PilotStall = bSlow && !bNewTarget ? PilotStall + DeltaSeconds : 0.f;
	if (!bPilotGaveUp && (PilotStuck > kPilotStuckSeconds || PilotStall > kPilotStuckSeconds))
	{
		const bool bStall = PilotStall > kPilotStuckSeconds;
		LogPilotStuck(Dist, Target,
			bStall ? FString::Printf(TEXT("%.0f s under %.1f m/s"), kPilotStuckSeconds, kPilotStallSpeed)
				   : FString::Printf(TEXT("%.0f s without a new best"), kPilotStuckSeconds));
		PilotStall = bStall ? -1e9f : PilotStall;
		bPilotGaveUp = true;
		// A world map's tour gives the leg up itself (TickWorld) and exits 1 when it ends.
		if (bQuitOnComplete && !(bWorldTour && !bOverworld))
		{
			FPlatformMisc::RequestExit(false, TEXT("ExploreDemo"));
		}
	}
	FVector2D Dir = To.GetSafeNormal();
	if (Waypoint)
	{
		Dir = FVector2D(Waypoint->x - Pos.x, Waypoint->y - Pos.y).GetSafeNormal();
	}
	Player.AddMovementInput(FVector(Dir.X, Dir.Y, 0.0), 1.f);
}

float AFTExploreDirector::PilotPathLeftFrom(ftcore::Vec2 Pos, int32 Target) const
{
	if (PilotPathTarget != Target || PilotPathState != 3 || !PilotPath.IsValidIndex(PilotPathNext))
	{
		return -1.f;
	}
	const auto Leg = [](ftcore::Vec2 A, ftcore::Vec2 B) { return static_cast<float>(FVector2D(B.x - A.x, B.y - A.y).Size()); };
	float Left = Leg(Pos, PilotPath[PilotPathNext]);
	for (int32 i = PilotPathNext; i + 1 < PilotPath.Num(); ++i)
	{
		Left += Leg(PilotPath[i], PilotPath[i + 1]);
	}
	return Left;
}

void AFTExploreDirector::LogPilotStuck(float Dist, int32 Target, const FString& Why) const
{
	// The path the pilot had when it gave up: its last navmesh answer for this target, if it asked one.
	const bool bAsked = PilotPathTarget == Target && PilotPathState > 0;
	const FString Path = !bAsked ? FString(TEXT("walking straight at it"))
		: PilotPathLeft >= 0.f ? FString::Printf(TEXT("with a full path, %.1f m of it left, best %.1f m"), PilotPathLeft, PilotBestPath)
							   : FString::Printf(TEXT("with %s"), kPilotPathStates[FMath::Min<int32>(PilotPathState, 3)]);
	UE_LOG(LogForgottenThird, Error, TEXT("ExploreDemo: STUCK %.1f m from spot %d (%s; %s%s)."), Dist, Target, *Why, *Path,
		bAsked && bPilotPathLimit ? TEXT("; its last search ran out of nodes") : TEXT(""));
}

const ftcore::Vec2* AFTExploreDirector::PilotWaypoint(const AFTExploreCharacter& Player, ftcore::Vec2 Pos, ftcore::Vec2 Goal, int32 Target,
	TOptional<double> GoalFloor)
{
	UWorld* World = GetWorld();
	UNavigationSystemV1* Nav = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
	if (!Nav)
	{
		return nullptr;
	}
	// The path is found about once a second (and on a new target), not every frame.
	const double Now = World->GetTimeSeconds();
	if (Target != PilotPathTarget || Now >= PilotPathRefreshAt)
	{
		PilotPathTarget = Target;
		PilotPathRefreshAt = Now + 1.0;
		PilotPath.Reset();
		PilotPathNext = 1;
		const FVector From = Player.GetActorLocation();
		// On a big world's ground a goal in another district stands at that district's floor, which may be metres off
		// the lead's height, too far for the navmesh to find under a point at the lead's. A spawn on a pinned map has its
		// own floor (a deck or a terrace above the plaza's), which the caller passes.
		const int32 GoalDistrict = Layout ? Layout->districtAt(Goal) : -1;
		if (!GoalFloor.IsSet() && DistrictFloors.IsValidIndex(GoalDistrict))
		{
			GoalFloor = DistrictFloors[GoalDistrict];
		}
		const FVector To(Goal.x * 100.0, Goal.y * 100.0, GoalFloor.IsSet() ? GoalFloor.GetValue() + Player.GetSimpleCollisionHalfHeight() : From.Z);
		const ANavigationData* NavData = bPilotWideSearch ? Nav->GetDefaultNavDataInstance() : nullptr;
		if (NavData)
		{
			// A tour stop's walk: the same query with a wider search budget, as the map builder's reach check asks.
			if (!PilotWideFilter.IsValid() && NavData->GetDefaultQueryFilter().IsValid())
			{
				FSharedNavQueryFilter Wide = NavData->GetDefaultQueryFilter()->GetCopy();
				Wide->SetMaxSearchNodes(kTourSearchNodes);
				PilotWideFilter = Wide;
			}
			const FPathFindingResult Found =
				Nav->FindPathSync(FPathFindingQuery(static_cast<const UObject*>(nullptr), *NavData, From, To, PilotWideFilter), EPathFindingMode::Regular);
			const bool bFound = Found.IsSuccessful() && Found.Path.IsValid() && Found.Path->IsValid();
			PilotPathGoal = To;
			PilotPathState = !bFound ? 1 : Found.Path->IsPartial() ? 2 : 3;
			bPilotPathLimit = Found.Path.IsValid() && Found.Path->DidSearchReachedLimit();
			if (bFound)
			{
				const TArray<FNavPathPoint>& Points = Found.Path->GetPathPoints();
				if (Points.Num() > 0)
				{
					PilotPathEnd = Points.Last().Location;
				}
				for (const FNavPathPoint& Point : Points)
				{
					PilotPath.Add(ToGround(Point.Location));
				}
				if (!bPilotPathLogged)
				{
					bPilotPathLogged = true;
					UE_LOG(LogForgottenThird, Display, TEXT("Explore: pilot follows a navmesh path (%d points)."), PilotPath.Num());
				}
			}
		}
		else
		{
			UNavigationPath* Path = Nav->FindPathToLocationSynchronously(World, From, To);
			PilotPathGoal = To;
			PilotPathState = !(Path && Path->IsValid()) ? 1 : Path->IsPartial() ? 2 : 3;
			// Whether the search ran out of nodes (the engine's default budget); logged, not acted on.
			bPilotPathLimit = Path && Path->GetPath().IsValid() && Path->GetPath()->DidSearchReachedLimit();
			if (PilotPathState > 1 && Path->PathPoints.Num() > 0)
			{
				PilotPathEnd = Path->PathPoints.Last();
			}
			if (Path && Path->IsValid())
			{
				for (const FVector& Point : Path->PathPoints)
				{
					PilotPath.Add(ToGround(Point));
				}
				if (!bPilotPathLogged)
				{
					bPilotPathLogged = true;
					UE_LOG(LogForgottenThird, Display, TEXT("Explore: pilot follows a navmesh path (%d points)."), PilotPath.Num());
				}
			}
		}
	}
	if (PilotPath.Num() < 2)
	{
		return nullptr;
	}
	PilotPathNext = ftcore::advanceWaypoint(PilotPath.GetData(), PilotPath.Num(), PilotPathNext, Pos, 0.6f);
	return PilotPathNext >= 0 ? &PilotPath[PilotPathNext] : nullptr;
}

void AFTExploreDirector::BeginWorld()
{
	if (WorldId.IsEmpty())
	{
		return;
	}
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI)
	{
		return;
	}
	const ftcore::WorldTable& Worlds = GI->GetWorlds();
	const FTCHARToUTF8 IdUtf8(*WorldId);
	const int32 Index = Worlds.findWorld(std::string_view(IdUtf8.Get(), static_cast<size_t>(IdUtf8.Length())));
	if (Index < 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("World %s: not in Data/Field/worlds.json; the map plays as a plain field."), *WorldId);
		return;
	}
	Layout = MakeUnique<ftcore::WorldLayout>();
	std::string Error;
	if (!ftcore::generateWorldLayout(Worlds.worlds[static_cast<size_t>(Index)], *Layout, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("World %s: %s"), *WorldId, *FT::ToFString(Error).TrimEnd());
		Layout.Reset();
		return;
	}
	WorldDistrict = 0;
	UE_LOG(LogForgottenThird, Display, TEXT("World %s: %d districts, %d roads, %d buildings, %d props over %.0f m."), *WorldId,
		static_cast<int32>(Layout->districtCount()), static_cast<int32>(Layout->roadCount()), static_cast<int32>(Layout->blockCount()),
		static_cast<int32>(Layout->propCount()), Layout->side());
	if (!DistrictCentres.IsEmpty())
	{
		// The map's own districts (FTBuildMassingWorld -World=): every district's frame moves there, so a story step's
		// goals and the tour land in the map's plazas. The generated roads and buildings aren't on this map.
		const int32 Count = DistrictCentres.Num();
		const bool bSpawns = DistrictSpawns.Num() == Count;
		ftcore::DistrictPin Pins[ftcore::WorldLayout::kMaxDistricts];
		Error.clear();
		if (Count > static_cast<int32>(UE_ARRAY_COUNT(Pins)))
		{
			Error = "the map places more districts than a world holds";
		}
		else
		{
			for (int32 i = 0; i < Count; ++i)
			{
				Pins[i].centre = ftcore::Vec2{static_cast<float>(DistrictCentres[i].X / 100.0), static_cast<float>(DistrictCentres[i].Y / 100.0)};
				Pins[i].hasSpawn = bSpawns;
				if (bSpawns)
				{
					Pins[i].spawn = ftcore::Vec2{static_cast<float>(DistrictSpawns[i].X / 100.0), static_cast<float>(DistrictSpawns[i].Y / 100.0)};
				}
			}
		}
		if (Error.empty() && Layout->pinDistricts(Pins, static_cast<size_t>(Count), &Error))
		{
			bDistrictsPinned = true;
			UE_LOG(LogForgottenThird, Display, TEXT("World %s: %d districts placed by the map%s."), *WorldId, Count,
				bSpawns ? TEXT(", with their spawns") : TEXT(""));
		}
		else
		{
			UE_LOG(LogForgottenThird, Error, TEXT("World %s: the map's own districts don't fit the layout (%s); the generated places stand."), *WorldId,
				*FT::ToFString(Error).TrimEnd());
		}
	}
	// The lead starts on the PlayerStart (a story step moves it to its district's spawn on the first tick, which logs again).
	if (!DistrictFloors.IsEmpty())
	{
		FloorLogAt = GetWorld()->GetTimeSeconds() + 1.0;
	}
	const FFTStoryCarry& Carry = GI->StoryCarry;
	const bool bStoryStep = Carry.bValid && !Carry.bDone
		&& (Carry.Screen == ftcore::GameplayScreen::Field || Carry.Screen == ftcore::GameplayScreen::Cutscene
			|| (Carry.Screen == ftcore::GameplayScreen::Battle && Carry.Combat == ftcore::CombatKind::Arena));
	// -WorldDistrict=<n>: outside a story step the lead starts on district n's spawn, on its floor, instead of the
	// PlayerStart, and a tour starts there: for looking round a district, or from it with -ExplorePortrait.
	int32 StartDistrict = -1;
	if (!bStoryStep && FParse::Value(FCommandLine::Get(), TEXT("WorldDistrict="), StartDistrict))
	{
		if (QueueDistrictSpawn(StartDistrict))
		{
			WorldDistrict = StartDistrict;
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("World %s: -WorldDistrict=%d names no district; the lead starts on the PlayerStart."), *WorldId,
				StartDistrict);
		}
	}
	// -WorldStartAt=<x>,<y>,<z>,<yaw>: outside a story step the lead is set down there instead, facing yaw (degrees, world),
	// its party behind it: for a still of one spot. x and y are plan metres on a map in the plan frame (engine metres
	// otherwise), z the floor there (m).
	FString StartAt;
	if (!bStoryStep && FParse::Value(FCommandLine::Get(), TEXT("WorldStartAt="), StartAt, false))
	{
		TArray<FString> Parts;
		StartAt.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 4)
		{
			const double X = FCString::Atod(*Parts[0]);
			const double Y = FCString::Atod(*Parts[1]);
			const bool bPlan = PlanSide > 0.f;
			const FVector At((bPlan ? X - PlanSide * 0.5 : X) * 100.0, (bPlan ? PlanSide * 0.5 - Y : Y) * 100.0, FCString::Atod(*Parts[2]) * 100.0);
			QueueStopSetDown(At, FCString::Atof(*Parts[3]));
			UE_LOG(LogForgottenThird, Display, TEXT("World %s: -WorldStartAt: the lead starts at (%.0f, %.0f, %.0f) cm, facing %.0f."), *WorldId, At.X, At.Y,
				At.Z, FCString::Atof(*Parts[3]));
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("World %s: -WorldStartAt=%s isn't x,y,z,yaw; the lead starts where it would."), *WorldId, *StartAt);
		}
	}
	bWorldTour = !bStoryStep && FParse::Param(FCommandLine::Get(), TEXT("WorldAutoPlay"));
	if (!bWorldTour)
	{
		return;
	}
	TourCount = static_cast<int32>(Layout->tour(static_cast<size_t>(WorldDistrict), Tour, UE_ARRAY_COUNT(Tour)));
	TourLeg = 0;
	TourLegFor = -1;
	TourMisses = 0;
	bTourQuitOnDone = bQuitOnComplete || FParse::Param(FCommandLine::Get(), TEXT("WorldQuitOnDone"));
	BeginTourStops();
	TourStart = Now();
	TourGameStart = GetWorld()->GetTimeSeconds();
	if (IsAlphaFast())
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 8.f);
	}
	if (TourStopOrder.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("World %s: tour of %d legs %s."), *WorldId, TourCount,
			bDistrictsPinned ? TEXT("on the navmesh, spawn to spawn") : TEXT("over the roads"));
		return;
	}
	const FString Districts = TourFirstLeg == 0 ? FString::Printf(TEXT("%d district legs spawn to spawn"), TourCount)
												: FString(TEXT("the district legs skipped (-WorldTourStops=only)"));
	UE_LOG(LogForgottenThird, Display,
		TEXT("World %s: tour of %d legs on the navmesh: %s, then %d of the map's %d stops (%d reach, %d edge, %d push; %d end in a still)."), *WorldId,
		TourLegTotal, *Districts, TourStopOrder.Num(), TourStopIds.Num(), TourStopsPicked[0], TourStopsPicked[1], TourStopsPicked[2], TourStillsPicked);
}

void AFTExploreDirector::BeginTourStops()
{
	// Without stops (or on a map whose stops can't be walked) the tour is its district legs, as before.
	TourFirstLeg = 0;
	TourEndLeg = TourCount;
	TourLegTotal = TourCount;
	TourStopOrder.Reset();
	TourMissLegs.Reset();
	for (int32 k = 0; k < static_cast<int32>(ftcore::kTourStopKinds); ++k)
	{
		TourStopsPicked[k] = 0;
		TourStopsRun[k] = 0;
	}
	TourStillsPicked = 0;
	TourStopsReached = 0;
	TourEdgesMet = 0;
	TourBareWalls = 0;
	TourUnjudged = 0;
	TourPushHeld = 0;
	TourGotThrough = 0;
	for (ftcore::EdgeTally& Tally : TourEdges)
	{
		Tally = ftcore::EdgeTally();
	}
	TourSetDownStop = -1;
	TourStopPhase = static_cast<uint8>(ETourStopPhase::Begin);
	TourWetLegs = 0;
	TourWetLeg = -1;
	TourIslands.Reset();
	bTourIslandsTried = false;
	const TCHAR* const Line = FCommandLine::Get();
	FString StopsMode;
	const bool bStopsMode = FParse::Value(Line, TEXT("WorldTourStops="), StopsMode);
	const bool bStopsOnly = bStopsMode && StopsMode.Equals(TEXT("only"), ESearchCase::IgnoreCase);
	if (bStopsMode && !bStopsOnly)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("World %s: -WorldTourStops=%s isn't a mode (only); the district legs run."), *WorldId, *StopsMode);
	}
	uint8 Kinds = ftcore::kTourKindsAll;
	FString KindList;
	if (FParse::Value(Line, TEXT("WorldTourKinds="), KindList, false))
	{
		const FTCHARToUTF8 KindUtf8(*KindList);
		if (!ftcore::parseTourKinds(std::string_view(KindUtf8.Get(), static_cast<size_t>(KindUtf8.Length())), Kinds))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("World %s: -WorldTourKinds=%s names a kind that isn't reach, edge or push; every kind runs."), *WorldId,
				*KindList);
			Kinds = ftcore::kTourKindsAll;
		}
	}
	TourStillsDir.Reset();
	if (FParse::Value(Line, TEXT("WorldTourStills="), TourStillsDir, false) && !TourStillsDir.IsEmpty())
	{
		TourStillsDir = FPaths::ConvertRelativePathToFull(TourStillsDir);
		if (FApp::CanEverRender())
		{
			IFileManager::Get().MakeDirectory(*TourStillsDir, true);
		}
	}
	const int32 Count = TourStopIds.Num();
	const bool bLinedUp = TourStopKinds.Num() == Count && TourStopTargets.Num() == Count && TourStopHasStart.Num() == Count
		&& TourStopStarts.Num() == Count && TourStopEdges.Num() == Count && TourStopStills.Num() == Count;
	if (Count > 0 && !bDistrictsPinned)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("World %s: the map has %d tour stops, but its districts aren't placed by it; the tour walks the districts only."),
			*WorldId, Count);
	}
	else if (Count > 0 && !bLinedUp)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("World %s: the map's %d tour stops don't line up (%d kinds, %d targets, %d starts, %d edges, %d stills); ")
			TEXT("the tour walks the districts only."), *WorldId, Count, TourStopKinds.Num(), TourStopTargets.Num(), TourStopStarts.Num(),
			TourStopEdges.Num(), TourStopStills.Num());
	}
	else if (Count > 0)
	{
		TourStopOrder.Reserve(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			const uint8 Kind = TourStopKinds[i];
			if (Kind < ftcore::kTourStopKinds && ftcore::tourKindIn(Kinds, static_cast<ftcore::TourStopKind>(Kind)))
			{
				TourStopOrder.Add(i);
				++TourStopsPicked[Kind];
				TourStillsPicked += TourStopStills[i] ? 1 : 0;
			}
		}
		if (TourStopOrder.IsEmpty())
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("World %s: -WorldTourKinds=%s picks none of the map's %d tour stops."), *WorldId, *KindList, Count);
		}
	}
	if (bStopsOnly && TourStopOrder.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("World %s: -WorldTourStops=only, but the tour has no stops to walk; the district legs run."), *WorldId);
	}
	else if (bStopsOnly)
	{
		TourFirstLeg = TourCount;
	}
	TourLeg = TourFirstLeg;
	TourEndLeg = TourCount + TourStopOrder.Num();
	TourLegTotal = TourEndLeg - TourFirstLeg;
	TourMissLegs.Reserve(TourLegTotal);
}

void AFTExploreDirector::UpdateWorldLine(ftcore::Vec2 Pos)
{
	const int32 At = Layout->districtAt(Pos);
	if (At == WorldLineDistrict)
	{
		return;
	}
	WorldLineDistrict = At;
	WorldLine = At >= 0
		? FString::Printf(TEXT("%s  district %d of %d"), *WorldId, At + 1, static_cast<int32>(Layout->districtCount()))
		: FString::Printf(TEXT("%s  open ground"), *WorldId);
}

void AFTExploreDirector::TickWorld(AFTExploreCharacter& Player, float DeltaSeconds)
{
	TickFollowers(Player, DeltaSeconds);
	const ftcore::Vec2 Pos = ToGround(Player.GetActorLocation());
	UpdateWorldLine(Pos);
	if (!bWorldTour || bWorldTourDone || TourLeg >= TourEndLeg)
	{
		return;
	}
	const double Game = GetWorld()->GetTimeSeconds() - TourGameStart;
	// Dry: on a map with a sea, a leg whose lead's feet go under its level is wet (logged once a leg). A stop leg's first
	// tick still sees the lead where the last leg left it, before its set-down.
	const bool bStopBegins = TourLeg >= TourCount && TourStopPhase == static_cast<uint8>(ETourStopPhase::Begin);
	if (bHasSeaLevel && TourWetLeg != TourLeg && !bStopBegins && !bWorldSpawnPending)
	{
		const double Feet = Player.GetActorLocation().Z - Player.GetSimpleCollisionHalfHeight();
		if (Feet < SeaLevel - kTourWetBand)
		{
			TourWetLeg = TourLeg;
			++TourWetLegs;
			float PlanX = 0.f;
			float PlanY = 0.f;
			TourPlanXY(Pos, PlanX, PlanY);
			UE_LOG(LogForgottenThird, Error, TEXT("World %s: WET on leg %d/%d at (%.1f, %.1f) m in the %s frame, feet z %.0f cm, under the sea's level %.0f cm."),
				*WorldId, TourLegNumber(), TourLegTotal, PlanX, PlanY, TourFrame(), Feet, SeaLevel);
		}
	}
	if (TourLeg >= TourCount)
	{
		TickTourStop(Player, Pos, Game, DeltaSeconds);
		return;
	}
	const int32 District = Tour[TourLeg];
	// On a map that places its own districts the tour walks spawn to spawn: the builder checks every spawn for a path
	// from the entry, while a plaza's centre may stand inside a piece that is still a box.
	const ftcore::WorldDistrict& Place = Layout->district(static_cast<size_t>(District));
	const ftcore::Vec2 Target = bDistrictsPinned ? Place.origin : Place.centre;
	const int32 Spot = kTourTargetBase + TourLeg;
	const float Dist = static_cast<float>(FVector2D(Target.x - Pos.x, Target.y - Pos.y).Size());
	if (TourLegFor != TourLeg)
	{
		TourLegFor = TourLeg;
		TourLegStartAt = Game;
		bTourLegCapped = false;
	}
	// Once a game second, what the pilot's stuck test looks at, so a STUCK can be read from the log as a stall or as a
	// lead still walking. Log only.
	if (Game >= TourTraceAt)
	{
		TourTraceAt = FMath::FloorToDouble(Game) + 1.0;
		const bool bAsked = PilotTarget == Spot; // a new leg's first tick comes before the pilot has seen its target
		const bool bPathAsked = bAsked && PilotPathTarget == Spot;
		UE_LOG(LogForgottenThird, Display,
			TEXT("World %s: trace leg %d/%d (spot %d) at %.1f s game time: the lead at (%.1f, %.1f) m in the engine frame, feet at z %.0f cm, %.2f m/s; ")
			TEXT("%.1f m from the goal, best %.1f m, %.1f s without a new best; path: %s%s, %.1f m left, best %.1f m; %.1f s slow; %.1f s on the leg."),
			*WorldId, TourLeg + 1, TourLegTotal, Spot, Game, Pos.x, Pos.y, Player.GetActorLocation().Z - Player.GetSimpleCollisionHalfHeight(),
			Player.GetVelocity().Size2D() / 100.0, Dist, bAsked ? PilotBestDist : Dist, bAsked ? FMath::Max(PilotStuck, 0.f) : 0.f,
			kPilotPathStates[bPathAsked ? FMath::Min<int32>(PilotPathState, 3) : 0], bPathAsked && bPilotPathLimit ? TEXT(" (out of nodes)") : TEXT(""),
			bAsked ? PilotPathLeft : -1.f, bAsked ? PilotBestPath : -1.f, bAsked ? FMath::Max(PilotStall, 0.f) : 0.f, Game - TourLegStartAt);
	}
	if (Dist > kTourReach)
	{
		// Roads run straight between plaza centres and nothing stands on them, so no navmesh is needed; a map that places
		// its own districts has none of those roads, so there the pilot follows the navmesh.
		const TOptional<double> SpawnFloor =
			bDistrictsPinned && DistrictSpawns.IsValidIndex(District) ? TOptional<double>(DistrictSpawns[District].Z) : TOptional<double>();
		PilotWalk(Player, Pos, Target, Spot, DeltaSeconds, bDistrictsPinned, SpawnFloor, bDistrictsPinned);
		// On a pinned map a lead still finding a shorter way runs on, but no leg runs past the cap.
		if (bDistrictsPinned && !bTourLegCapped && !bPilotGaveUp && PilotTarget == Spot && Game - TourLegStartAt > kTourLegCapSeconds)
		{
			bTourLegCapped = true;
			LogPilotStuck(Dist, Spot, FString::Printf(TEXT("%.0f s on the leg, past its cap of %.0f s"), Game - TourLegStartAt, kTourLegCapSeconds));
			bPilotGaveUp = true;
		}
		// A run that quits when the tour is done gives a stuck leg up and goes on from where the lead stands, so one stall
		// can't hold the run open; without the flag the lead stays put (someone may be watching).
		if (!bTourQuitOnDone || !bPilotGaveUp || PilotTarget != Spot)
		{
			return;
		}
		UE_LOG(LogForgottenThird, Error, TEXT("World %s: gave up on district %d (leg %d/%d), %.1f m short, at %.1f s game time."), *WorldId,
			District, TourLeg + 1, TourLegTotal, Dist, Game);
		LogLegStopped(Player, Pos);
		CountTourMiss();
		// The next leg starts on this leg's spawn, where the tour meant it to start, so one stall is given up once and not
		// again on every leg after it. The leg still counts as given up. A stop leg with its own start sets the lead down there.
		if (TourLeg + 1 < TourEndLeg && !NextLegSetsDown() && QueueDistrictSpawn(District))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("World %s: leg %d given up; the lead is set down on district %d's spawn for leg %d."), *WorldId,
				TourLeg + 1, District, TourLeg + 2);
		}
	}
	else if (bDistrictsPinned && DistrictSpawns.IsValidIndex(District))
	{
		// The reach is on the ground plan, so the feet say whether the lead stands on the spawn's own deck or terrace.
		UE_LOG(LogForgottenThird, Display, TEXT("World %s: reached district %d (leg %d/%d) at %.1f s game time, feet at z %.0f cm (its spawn's %.0f cm)."),
			*WorldId, District, TourLeg + 1, TourLegTotal, Game, Player.GetActorLocation().Z - Player.GetSimpleCollisionHalfHeight(),
			DistrictSpawns[District].Z);
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("World %s: reached district %d (leg %d/%d) at %.1f s game time."), *WorldId, District,
			TourLeg + 1, TourLegTotal, Game);
	}
	NextTourLeg(Game);
}

void AFTExploreDirector::TickTourStop(AFTExploreCharacter& Player, ftcore::Vec2 Pos, double Game, float DeltaSeconds)
{
	const int32 Stop = TourStopOrder[TourLeg - TourCount];
	const ftcore::TourStopKind Kind = static_cast<ftcore::TourStopKind>(TourStopKinds[Stop]);
	const FVector& TargetCm = TourStopTargets[Stop];
	const ftcore::Vec2 Target = ToGround(TargetCm);
	const float Dist = static_cast<float>(FVector2D(Target.x - Pos.x, Target.y - Pos.y).Size());
	const float AimDist = static_cast<float>(FVector2D(TourStopAim.x - Pos.x, TourStopAim.y - Pos.y).Size());
	const int32 Spot = kTourStopTargetBase + TourLeg;
	const double Feet = Player.GetActorLocation().Z - Player.GetSimpleCollisionHalfHeight();
	const ETourStopPhase Phase = static_cast<ETourStopPhase>(TourStopPhase);
	if (TourLegFor != TourLeg)
	{
		TourLegFor = TourLeg;
		TourLegStartAt = Game;
		bTourLegCapped = false;
	}
	// Once a game second, as on a district leg: what the leg is doing, and on the walk what the pilot's stuck test sees.
	if (Game >= TourTraceAt && Phase != ETourStopPhase::Begin && Phase != ETourStopPhase::Still)
	{
		TourTraceAt = FMath::FloorToDouble(Game) + 1.0;
		const double Speed = Player.GetVelocity().Size2D() / 100.0;
		if (Phase == ETourStopPhase::Walk)
		{
			const bool bAsked = PilotTarget == Spot;
			const bool bPathAsked = bAsked && PilotPathTarget == Spot;
			UE_LOG(LogForgottenThird, Display,
				TEXT("World %s: trace leg %d/%d (stop %s, walking) at %.1f s game time: the lead at (%.1f, %.1f) m in the engine frame, feet at z %.0f cm, ")
				TEXT("%.2f m/s; %.1f m from the aim, best %.1f m, %.1f s without a new best; path: %s%s, %.1f m left, best %.1f m; %.1f s slow; ")
				TEXT("%.1f s on the leg."),
				*WorldId, TourLegNumber(), TourLegTotal, *TourStopIds[Stop], Game, Pos.x, Pos.y, Feet, Speed, AimDist, bAsked ? PilotBestDist : AimDist,
				bAsked ? FMath::Max(PilotStuck, 0.f) : 0.f, kPilotPathStates[bPathAsked ? FMath::Min<int32>(PilotPathState, 3) : 0],
				bPathAsked && bPilotPathLimit ? TEXT(" (out of nodes)") : TEXT(""), bAsked ? PilotPathLeft : -1.f, bAsked ? PilotBestPath : -1.f,
				bAsked ? FMath::Max(PilotStall, 0.f) : 0.f, Game - TourLegStartAt);
		}
		else
		{
			UE_LOG(LogForgottenThird, Display,
				TEXT("World %s: trace leg %d/%d (stop %s, %s) at %.1f s game time: the lead at (%.1f, %.1f) m in the engine frame, feet at z %.0f cm, ")
				TEXT("%.2f m/s; %.1f m from the target; %.1f s on the leg."),
				*WorldId, TourLegNumber(), TourLegTotal, *TourStopIds[Stop], Phase == ETourStopPhase::Settle ? TEXT("settling") : TEXT("pushing"), Game,
				Pos.x, Pos.y, Feet, Speed, Dist, Game - TourLegStartAt);
		}
	}
	switch (Phase)
	{
	case ETourStopPhase::Begin:
	{
		// A stop with a start sets the lead down there, facing the target; one without walks on from where the last leg
		// left it, once it stands on the ground.
		++TourStopsRun[TourStopKinds[Stop]];
		bTourStopWalkShort = false;
		bTourStopNoNav = false;
		bTourStopFullPath = false;
		bTourPushLocked = false;
		TourSetDownStop = -1;
		TourStopAim = Target;
		TourStopAimFloor = TargetCm.Z;
		TourStopPhase = static_cast<uint8>(ETourStopPhase::Settle);
		TourPhaseAt = Game - kStopSettleSeconds;
		if (TourStopHasStart[Stop])
		{
			const FVector& Start = TourStopStarts[Stop];
			QueueStopSetDown(Start, static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(TargetCm.Y - Start.Y, TargetCm.X - Start.X))));
			TourPhaseAt = Game;
		}
		return;
	}
	case ETourStopPhase::Settle:
	{
		const UCharacterMovementComponent* Move = Player.GetCharacterMovement();
		const bool bOnGround = Move && Move->IsMovingOnGround();
		const double Settled = Game - TourPhaseAt;
		if (!(Settled >= kStopSettleSeconds && bOnGround) && Settled < kStopSettleMaxSeconds)
		{
			return;
		}
		if (Kind == ftcore::TourStopKind::Push)
		{
			BeginStopPush(Pos, Target, Feet, Game);
			return;
		}
		if (Kind == ftcore::TourStopKind::Edge)
		{
			// An edge stop's walk aims at the navmesh's nearest point to the target on the island the lead stands on, so
			// it never aims at a roof or a shelf it can't walk to; the push does the rest. The islands are gathered at the
			// tour's first edge stop, and that tick waits here.
			if (GatherTourIslands())
			{
				return;
			}
			UWorld* World = GetWorld();
			const UNavigationSystemV1* Nav = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
			const ANavigationData* NavData = Nav ? Nav->GetDefaultNavDataInstance() : nullptr;
			const ARecastNavMesh* Recast = Cast<const ARecastNavMesh>(NavData);
			FNavLocation OnMesh;
			int32 Island = -1;
			bool bOnMesh = false;
			if (Recast && TourIslands)
			{
				const FVector LeadFeet(Player.GetActorLocation().X, Player.GetActorLocation().Y, Feet);
				FNavLocation OnLead;
				const uint32* Index = Nav->ProjectPointToNavigation(LeadFeet, OnLead, INVALID_NAVEXTENT, Recast)
					? TourIslands->IndexOf.Find(OnLead.NodeRef)
					: nullptr;
				if (Index && *Index < TourIslands->Set.islandOf.size())
				{
					Island = static_cast<int32>(TourIslands->Set.islandOf[*Index]);
					bOnMesh = NearestOnIsland(*TourIslands, *Recast, static_cast<uint32>(Island), TargetCm, kEdgeNavExtent, OnMesh.Location);
				}
			}
			// The lead on no gathered island (or none gathered): the navmesh's nearest point to the target, on any island.
			if (Island < 0)
			{
				bOnMesh = NavData && Nav->ProjectPointToNavigation(TargetCm, OnMesh, kEdgeNavExtent, NavData);
			}
			const FString OnIsland = Island >= 0 ? FString::Printf(TEXT("on the lead's island #%d"), Island + 1)
				: TourIslands	 ? FString(TEXT("on any island (the lead stands on none of the navmesh's islands)"))
								 : FString(TEXT("on any island (no islands gathered)"));
			if (!bOnMesh)
			{
				bTourStopNoNav = true;
				UE_LOG(LogForgottenThird, Warning,
					TEXT("World %s: edge stop %s (leg %d/%d): no navmesh within %.0f m of the target %s; the push starts where the lead stands."), *WorldId,
					*TourStopIds[Stop], TourLegNumber(), TourLegTotal, kEdgeNavExtent.X / 100.0, *OnIsland);
				BeginStopPush(Pos, Target, Feet, Game);
				return;
			}
			TourStopAim = ToGround(OnMesh.Location);
			TourStopAimFloor = OnMesh.Location.Z;
			float AimX = 0.f;
			float AimY = 0.f;
			TourPlanXY(TourStopAim, AimX, AimY);
			UE_LOG(LogForgottenThird, Display,
				TEXT("World %s: edge stop %s (leg %d/%d): the walk aims at (%.1f, %.1f) m in the %s frame, z %.0f cm, %s, %.1f m from the target ")
				TEXT("(%.1f m across the ground)."),
				*WorldId, *TourStopIds[Stop], TourLegNumber(), TourLegTotal, AimX, AimY, TourFrame(), OnMesh.Location.Z, *OnIsland,
				FVector::Dist(OnMesh.Location, TargetCm) / 100.0, FVector2D(TourStopAim.x - Target.x, TourStopAim.y - Target.y).Size());
		}
		// The cap starts from the straight line; the walk's first full path re-sets it (the Walk case).
		TourStopStraight = FVector2D(TourStopAim.x - Pos.x, TourStopAim.y - Pos.y).Size();
		TourStopCap = ftcore::tourLegCapSeconds(TourStopStraight);
		bTourStopFullPath = false;
		bPilotWideSearch = true;
		TourStopPhase = static_cast<uint8>(ETourStopPhase::Walk);
		TourPhaseAt = Game;
		return;
	}
	case ETourStopPhase::Walk:
	{
		const bool bEdge = Kind == ftcore::TourStopKind::Edge;
		if (AimDist <= (bEdge ? kEdgeWalkReach : kTourReach))
		{
			bPilotWideSearch = false;
			if (bEdge)
			{
				BeginStopPush(Pos, Target, Feet, Game);
				return;
			}
			++TourStopsReached;
			UE_LOG(LogForgottenThird, Display, TEXT("World %s: reached stop %s (leg %d/%d) at %.1f s game time, %.1f s on the leg, feet at z %.0f cm (the stop's %.0f cm)."),
				*WorldId, *TourStopIds[Stop], TourLegNumber(), TourLegTotal, Game, Game - TourLegStartAt, Feet, TargetCm.Z);
			EndStopLeg(Stop, Game);
			return;
		}
		PilotWalk(Player, Pos, TourStopAim, Spot, DeltaSeconds, true, TOptional<double>(TourStopAimFloor), true);
		// The walk's first full path re-sets its cap by that path's length (ftcore::tourLegBudget: never under the straight
		// line's), still timed from the walk's start; a path over kTourDetourFactor times the straight line is noted once
		// for the plan's owner, never a fail. A partial path or none keeps the straight line's cap.
		if (!bTourStopFullPath && PilotTarget == Spot && PilotPathLeft >= 0.f)
		{
			bTourStopFullPath = true;
			const ftcore::TourLegBudget Budget = ftcore::tourLegBudget(TourStopStraight, PilotPathLeft, true);
			TourStopCap = Budget.capSeconds;
			UE_LOG(LogForgottenThird, Display,
				TEXT("World %s: stop %s (leg %d/%d): a full path of %.1f m (the straight line %.1f m) at %.1f s on the walk; its cap is now %.0f s."), *WorldId,
				*TourStopIds[Stop], TourLegNumber(), TourLegTotal, PilotPathLeft, TourStopStraight, Game - TourPhaseAt, TourStopCap);
			if (Budget.longDetour)
			{
				UE_LOG(LogForgottenThird, Display,
					TEXT("World %s: INFO stop %s (leg %d/%d): its full path, %.1f m, is %.1f times its straight line of %.1f m (over %.0f), for the plan's owner."),
					*WorldId, *TourStopIds[Stop], TourLegNumber(), TourLegTotal, PilotPathLeft, PilotPathLeft / FMath::Max(TourStopStraight, 0.01),
					TourStopStraight, ftcore::kTourDetourFactor);
			}
		}
		// No walk runs past its cap (ftcore::tourLegBudget's, from the straight line it started on or its first full path).
		if (!bTourLegCapped && !bPilotGaveUp && PilotTarget == Spot && Game - TourPhaseAt > TourStopCap)
		{
			bTourLegCapped = true;
			LogPilotStuck(AimDist, Spot, FString::Printf(TEXT("%.0f s on the walk, past its cap of %.0f s"), Game - TourPhaseAt, TourStopCap));
			bPilotGaveUp = true;
		}
		// A reach stop given up without -WorldQuitOnDone stays put, as a district leg does; an edge stop's walk given up
		// counts as given up, and the edge is judged from where the lead stands.
		if (!bPilotGaveUp || PilotTarget != Spot || (!bEdge && !bTourQuitOnDone))
		{
			return;
		}
		UE_LOG(LogForgottenThird, Error, TEXT("World %s: gave up on stop %s (leg %d/%d), %.1f m short, at %.1f s game time."), *WorldId,
			*TourStopIds[Stop], TourLegNumber(), TourLegTotal, AimDist, Game);
		LogLegStopped(Player, Pos);
		CountTourMiss();
		bPilotWideSearch = false;
		if (bEdge)
		{
			bTourStopWalkShort = true;
			BeginStopPush(Pos, Target, Feet, Game);
			return;
		}
		TourSetDownStop = Stop;
		TourSetDownYaw = static_cast<float>(Player.GetActorRotation().Yaw);
		EndStopLeg(Stop, Game);
		return;
	}
	case ETourStopPhase::Push:
	{
		// Straight at the target, no navmesh; once the lead is at it, on the way it was going.
		if (!bTourPushLocked)
		{
			const FVector2D ToTarget(Target.x - Pos.x, Target.y - Pos.y);
			if (ToTarget.Size() > kPushAimMetres)
			{
				TourPushDir = ToTarget.GetSafeNormal();
			}
			else
			{
				bTourPushLocked = true;
			}
		}
		Player.AddMovementInput(FVector(TourPushDir.X, TourPushDir.Y, 0.0), 1.f);
		TourPushClosest = FMath::Min(TourPushClosest, Dist);
		TourPushLowFeet = FMath::Min(TourPushLowFeet, Feet);
		const bool bEdge = Kind == ftcore::TourStopKind::Edge;
		if (Game - TourPhaseAt < (bEdge ? kEdgePushSeconds : kPushSeconds))
		{
			return;
		}
		if (bEdge)
		{
			JudgeEdgeStop(Player, Stop);
			EndStopLeg(Stop, Game);
			return;
		}
		float PlanX = 0.f;
		float PlanY = 0.f;
		TourPlanXY(Pos, PlanX, PlanY);
		const FString Fell =
			TourPushStartFeet - TourPushLowFeet > kStopFellCm ? FString::Printf(TEXT("; it fell %.1f m on the push"), (TourPushStartFeet - TourPushLowFeet) / 100.0) : FString();
		if (TourPushClosest > kPushHeldMetres)
		{
			++TourPushHeld;
			UE_LOG(LogForgottenThird, Display,
				TEXT("World %s: push stop %s (leg %d/%d): held, %.1f m from the target at the closest; the lead at (%.1f, %.1f) m in the %s frame, feet z %.2f m%s."),
				*WorldId, *TourStopIds[Stop], TourLegNumber(), TourLegTotal, TourPushClosest, PlanX, PlanY, TourFrame(), Feet / 100.0, *Fell);
		}
		else
		{
			++TourGotThrough;
			UE_LOG(LogForgottenThird, Error,
				TEXT("World %s: push stop %s (leg %d/%d): GOT THROUGH, %.1f m from the target at the closest (held needs over %.1f m); the lead at (%.1f, %.1f) m ")
				TEXT("in the %s frame, feet z %.2f m%s."),
				*WorldId, *TourStopIds[Stop], TourLegNumber(), TourLegTotal, TourPushClosest, kPushHeldMetres, PlanX, PlanY, TourFrame(), Feet / 100.0, *Fell);
		}
		EndStopLeg(Stop, Game);
		return;
	}
	case ETourStopPhase::Still:
	{
		// HighResShot writes the file a frame or more later: look for it a few times a second, a few seconds at most.
		const double Real = Now();
		if (Real < TourStillCheckAt)
		{
			return;
		}
		TourStillCheckAt = Real + kStillCheckSeconds;
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*TourStillFile);
		if (Stamp != FDateTime::MinValue() && Stamp > TourStillOldStamp)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("World %s: still for stop %s saved: %s."), *WorldId, *TourStopIds[Stop], *TourStillFile);
			NextTourLeg(Game);
			return;
		}
		if (Real < TourStillUntil)
		{
			return;
		}
		UE_LOG(LogForgottenThird, Warning, TEXT("World %s: still for stop %s not on disk after %.0f s: %s."), *WorldId, *TourStopIds[Stop], kStillWaitSeconds,
			*TourStillFile);
		NextTourLeg(Game);
		return;
	}
	}
}

void AFTExploreDirector::BeginStopPush(ftcore::Vec2 Pos, ftcore::Vec2 Target, double Feet, double Game)
{
	// Steer at the target; a lead already at it pushes on the way out of the square across the stop's edge.
	const int32 Stop = TourStopOrder[TourLeg - TourCount];
	const int32 Edge = EdgeIndex(TourStopEdges[Stop]);
	const FVector2D ToTarget(Target.x - Pos.x, Target.y - Pos.y);
	const double Away = ToTarget.Size();
	bTourPushLocked = Away <= kPushAimMetres;
	TourPushDir = !bTourPushLocked ? ToTarget.GetSafeNormal() : Edge >= 0 ? FVector2D(kEdgeOutX[Edge], kEdgeOutY[Edge]) : FVector2D(1.0, 0.0);
	TourPushClosest = static_cast<float>(Away);
	TourPushStartFeet = Feet;
	TourPushLowFeet = Feet;
	TourStopPhase = static_cast<uint8>(ETourStopPhase::Push);
	TourPhaseAt = Game;
}

void AFTExploreDirector::JudgeEdgeStop(const AFTExploreCharacter& Player, int32 Stop)
{
	// What stopped the lead: a capsule a little narrower than the lead's, its bottom lifted by half a step so the floor's
	// small rises don't count, swept kEdgeProbeMetres ahead on the pawn channel (what stops the lead), then the ground
	// past a hidden wall, or along the probe when nothing (or only walkable ground) stood in the way. Once a leg.
	UWorld* World = GetWorld();
	const UCharacterMovementComponent* Move = Player.GetCharacterMovement();
	const float Radius = Player.GetSimpleCollisionRadius();
	const float Half = Player.GetSimpleCollisionHalfHeight();
	const float StepUp = Move ? Move->MaxStepHeight : 45.f;
	const FVector At = Player.GetActorLocation();
	const double FeetCm = At.Z - Half;
	const FVector Dir(TourPushDir.X, TourPushDir.Y, 0.0);
	const float ProbeRadius = FMath::Max(Radius - 5.f, 5.f);
	const float ProbeHalf = FMath::Max(Half - StepUp * 0.5f, ProbeRadius);
	const FVector Centre = At + FVector(0.0, 0.0, Half - ProbeHalf);
	const double ProbeCm = ftcore::kEdgeProbeMetres * 100.0;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FTTourEdge), false, &Player);
	for (const TObjectPtr<AFTPartyFollower>& Follower : Followers)
	{
		if (Follower)
		{
			Params.AddIgnoredActor(Follower.Get());
		}
	}
	FHitResult Hit;
	const bool bHit = World
		&& World->SweepSingleByChannel(Hit, Centre, Centre + Dir * ProbeCm, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(ProbeRadius, ProbeHalf), Params);
	ftcore::EdgeProbe Probe;
	Probe.walkableNormalZ = Move ? Move->GetWalkableFloorZ() : 0.71;
	Probe.feetZ = FeetCm / 100.0;
	Probe.hasSea = bHasSeaLevel;
	Probe.seaLevel = SeaLevel / 100.0;
	const AActor* HitActor = bHit ? Hit.GetActor() : nullptr;
	const UPrimitiveComponent* HitComponent = bHit ? Hit.GetComponent() : nullptr;
	if (bHit)
	{
		// The Landscape's collision (a runtime build has no Landscape module here, so it is told by its class's name).
		static const FName HiddenWallTag(TEXT("FTHiddenWall"));
		static const FName LandscapeCollision(TEXT("LandscapeHeightfieldCollisionComponent"));
		bool bLandscape = false;
		for (const UClass* Class = HitComponent ? HitComponent->GetClass() : nullptr; Class && !bLandscape; Class = Class->GetSuperClass())
		{
			bLandscape = Class->GetFName() == LandscapeCollision;
		}
		Probe.hit = HitActor && HitActor->ActorHasTag(HiddenWallTag) ? ftcore::EdgeHit::HiddenWall
			: bLandscape																? ftcore::EdgeHit::Landscape
																						: ftcore::EdgeHit::Built;
		Probe.hitNormalZ = Hit.ImpactNormal.Z;
	}
	// Down-traces on the visibility channel, which the hidden walls don't block.
	const auto GroundAt = [World, FeetCm, &Params](const FVector& Point, double& Z)
	{
		FHitResult Down;
		if (!World
			|| !World->LineTraceSingleByChannel(Down, FVector(Point.X, Point.Y, FeetCm + kEdgeGroundAboveCm),
				FVector(Point.X, Point.Y, FeetCm - kEdgeGroundBelowCm), ECC_Visibility, Params))
		{
			return false;
		}
		Z = Down.ImpactPoint.Z;
		return true;
	};
	bool bGroundLooked = false;
	if (Probe.hit == ftcore::EdgeHit::HiddenWall)
	{
		bGroundLooked = true;
		Params.AddIgnoredActor(HitActor);
		double Z = 0.0;
		Probe.groundFound = GroundAt(Hit.ImpactPoint + Dir * kEdgeWallPastCm, Z);
		Probe.groundZ = Z / 100.0;
	}
	else if (Probe.hit == ftcore::EdgeHit::None || (Probe.hit == ftcore::EdgeHit::Landscape && Probe.hitNormalZ >= Probe.walkableNormalZ))
	{
		bGroundLooked = true;
		Probe.groundFound = true;
		double Low = TNumericLimits<double>::Max();
		for (double Along = Radius + 25.0; Along <= ProbeCm + 0.5; Along += kEdgeGroundStepCm)
		{
			double Z = 0.0;
			if (!GroundAt(At + Dir * Along, Z))
			{
				Probe.groundFound = false;
				break;
			}
			Low = FMath::Min(Low, Z);
		}
		Probe.groundZ = Probe.groundFound ? Low / 100.0 : 0.0;
	}
	const ftcore::EdgeCause Cause = ftcore::judgeEdge(Probe);
	const bool bMet = ftcore::edgeCauseMet(Cause);
	const uint8 Letter = TourStopEdges[Stop];
	const int32 Edge = EdgeIndex(Letter);
	if (Edge >= 0)
	{
		TourEdges[Edge].add(Cause);
	}
	TourEdgesMet += bMet ? 1 : 0;
	TourBareWalls += Cause == ftcore::EdgeCause::BareHiddenWall ? 1 : 0;
	TourUnjudged += Cause == ftcore::EdgeCause::Unjudged ? 1 : 0;

	// One line for the leg (leg end: it may allocate).
	float PlanX = 0.f;
	float PlanY = 0.f;
	TourPlanXY(ToGround(At), PlanX, PlanY);
	const ftcore::Vec2 Target = ToGround(TourStopTargets[Stop]);
	const double FromTarget = FVector2D(Target.x - At.X / 100.0, Target.y - At.Y / 100.0).Size();
	const TCHAR* const WalkNote = bTourStopNoNav ? TEXT("; no navmesh near the target, so it pushed from where it stood")
		: bTourStopWalkShort					 ? TEXT("; its walk gave up short of its aim")
												 : TEXT("");
	FString Swept;
	if (!bHit)
	{
		Swept = FString::Printf(TEXT("nothing within %.1f m"), ftcore::kEdgeProbeMetres);
	}
	else
	{
		const FString Label = HitActor ? HitActor->GetActorNameOrLabel() : HitComponent ? HitComponent->GetName() : FString(TEXT("something"));
		const FString What = Probe.hit == ftcore::EdgeHit::HiddenWall ? FString(TEXT(" (a hidden wall)"))
			: Probe.hit == ftcore::EdgeHit::Landscape
			? FString::Printf(TEXT(" (the ground, its normal z %.2f, walkable from %.2f)"), Probe.hitNormalZ, Probe.walkableNormalZ)
			: FString();
		Swept = FString::Printf(TEXT("%s%s at %.1f m"), *Label, *What, Hit.Distance / 100.0);
	}
	const FString Ground = !bGroundLooked ? FString(TEXT("the ground past it not looked at"))
		: !Probe.groundFound				? FString(TEXT("no ground found"))
		: Probe.hasSea ? FString::Printf(TEXT("ground at z %.2f m (the sea's level %.2f m)"), Probe.groundZ, Probe.seaLevel)
					   : FString::Printf(TEXT("ground at z %.2f m"), Probe.groundZ);
	const FString Fell = TourPushStartFeet - TourPushLowFeet > kStopFellCm
		? FString::Printf(TEXT("; it fell %.1f m on the push"), (TourPushStartFeet - TourPushLowFeet) / 100.0)
		: FString();
	const FString Line = FString::Printf(
		TEXT("World %s: edge stop %s (leg %d/%d, edge %c): %s, %s; the lead at (%.1f, %.1f) m in the %s frame, feet z %.2f m, %.1f m from the target%s; ")
		TEXT("the sweep hit %s; %s%s."),
		*WorldId, *TourStopIds[Stop], TourLegNumber(), TourLegTotal, Letter ? static_cast<TCHAR>(Letter) : TEXT('?'), bMet ? TEXT("met") : TEXT("FAIL"),
		*FT::ToFString(ftcore::edgeCauseName(Cause)), PlanX, PlanY, TourFrame(), Probe.feetZ, FromTarget, WalkNote, *Swept, *Ground, *Fell);
	if (bMet)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s"), *Line);
	}
	else
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s"), *Line);
	}
}

void AFTExploreDirector::EndStopLeg(int32 Stop, double Game)
{
	// A stop marked for a still takes it here, where the leg ended, before the tour goes on.
	bPilotWideSearch = false;
	if (!TourStopStills[Stop])
	{
		NextTourLeg(Game);
		return;
	}
	if (!FApp::CanEverRender())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("World %s: still for stop %s skipped: no renderer (-NullRHI)."), *WorldId, *TourStopIds[Stop]);
		NextTourLeg(Game);
		return;
	}
	if (TourStillsDir.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("World %s: still for stop %s skipped: no -WorldTourStills=<dir>."), *WorldId, *TourStopIds[Stop]);
		NextTourLeg(Game);
		return;
	}
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
	if (!Viewport)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("World %s: still for stop %s skipped: no game viewport."), *WorldId, *TourStopIds[Stop]);
		NextTourLeg(Game);
		return;
	}
	TourStillFile = FPaths::Combine(TourStillsDir, TourStopIds[Stop] + TEXT(".png"));
	TourStillOldStamp = IFileManager::Get().GetTimeStamp(*TourStillFile);
	Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *TourStillFile), *GLog);
	UE_LOG(LogForgottenThird, Display, TEXT("World %s: still for stop %s requested: %s."), *WorldId, *TourStopIds[Stop], *TourStillFile);
	TourStopPhase = static_cast<uint8>(ETourStopPhase::Still);
	const double Real = Now();
	TourStillUntil = Real + kStillWaitSeconds;
	TourStillCheckAt = Real + kStillCheckSeconds;
}

void AFTExploreDirector::LogLegStopped(const AFTExploreCharacter& Player, ftcore::Vec2 Pos) const
{
	// Where it stopped and what the pilot's last navmesh query answered, so the stall can be placed without a rerun.
	const FString PathEnd = PilotPathState > 1
		? FString::Printf(TEXT(", ending %.1f m from the goal at z %.0f cm"),
			  FVector2D::Distance(FVector2D(PilotPathEnd), FVector2D(PilotPathGoal)) / 100.0, PilotPathEnd.Z)
		: FString();
	UE_LOG(LogForgottenThird, Error,
		TEXT("World %s: leg %d stopped with the lead at (%.1f, %.1f) m in the engine frame, feet at z %.0f cm; the goal asked at z %.0f cm; ")
		TEXT("the last query found %s (%d points)%s%s."),
		*WorldId, TourLegNumber(), Pos.x, Pos.y, Player.GetActorLocation().Z - Player.GetSimpleCollisionHalfHeight(), PilotPathGoal.Z,
		kPilotPathStates[FMath::Min<int32>(PilotPathState, 3)], PilotPath.Num(), *PathEnd,
		bPilotPathLimit ? TEXT("; its search ran out of nodes") : TEXT(""));
}

void AFTExploreDirector::CountTourMiss()
{
	TourMissLegs.Add(TourLegNumber()); // reserved for every leg when the tour starts
	++TourMisses;
}

bool AFTExploreDirector::NextLegSetsDown() const
{
	const int32 Next = TourLeg + 1;
	if (Next < TourCount || Next >= TourEndLeg)
	{
		return false;
	}
	const int32 Stop = TourStopOrder[Next - TourCount];
	return TourStopHasStart.IsValidIndex(Stop) && TourStopHasStart[Stop];
}

void AFTExploreDirector::QueueStopSetDown(const FVector& At, float Yaw)
{
	WorldSpawn = At;
	bWorldSpawnOnFloor = true;
	WorldSpawnRotation = FRotator(0.f, Yaw, 0.f);
	bWorldSpawnPending = true;
}

void AFTExploreDirector::TourPlanXY(ftcore::Vec2 Engine, float& X, float& Y) const
{
	// The plan frame: x east from the square's west side, y north from its south side (FTBuildMassingWorld).
	const bool bPlan = PlanSide > 0.f;
	X = bPlan ? Engine.x + PlanSide * 0.5f : Engine.x;
	Y = bPlan ? PlanSide * 0.5f - Engine.y : Engine.y;
}

const TCHAR* AFTExploreDirector::TourFrame() const
{
	return PlanSide > 0.f ? TEXT("plan") : TEXT("engine");
}

bool AFTExploreDirector::GatherTourIslands()
{
	if (bTourIslandsTried)
	{
		return false;
	}
	bTourIslandsTried = true;
	UWorld* World = GetWorld();
	const UNavigationSystemV1* Nav = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
	const ARecastNavMesh* Recast = Nav ? Cast<const ARecastNavMesh>(Nav->GetDefaultNavDataInstance()) : nullptr;
	if (!Recast)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("World %s: no Recast navmesh to gather islands from; the edge walks aim at the navmesh's nearest point, on any island."),
			*WorldId);
		return false;
	}
	// As the map builder's coverage gathers them (FTGatherNavIslands), but in engine metres: the same polys, links and
	// islands, numbered alike (largest first), with the polys under the sea left out.
	const double Began = FPlatformTime::Seconds();
	TourIslands = MakeUnique<FFTNavIslands>();
	FTGatherNavIslands(*Recast, [](const FVector& V) { return ftcore::MassingVec{V.X / 100.0, V.Y / 100.0, V.Z / 100.0}; },
		bHasSeaLevel ? SeaLevel / 100.0 : ftcore::kNoSea, *TourIslands);
	UE_LOG(LogForgottenThird, Display,
		TEXT("World %s: the navmesh's islands for the edge walks: %d polys in %d tiles, %d of them left out under the sea, %d islands; gathered in %.1f s."),
		*WorldId, TourIslands->Gathered, TourIslands->Tiles, static_cast<int32>(TourIslands->UnderSea.polys),
		static_cast<int32>(TourIslands->Set.islands.size()), FPlatformTime::Seconds() - Began);
	return true;
}

void AFTExploreDirector::NextTourLeg(double Game)
{
	bPilotWideSearch = false;
	TourStopPhase = static_cast<uint8>(ETourStopPhase::Begin);
	// A reach stop given up sets the lead down on its target for the next leg, as a district leg does on its spawn.
	if (TourSetDownStop >= 0 && TourLeg + 1 < TourEndLeg && !NextLegSetsDown())
	{
		QueueStopSetDown(TourStopTargets[TourSetDownStop], TourSetDownYaw);
		UE_LOG(LogForgottenThird, Display, TEXT("World %s: leg %d given up; the lead is set down on stop %s's target for leg %d."), *WorldId,
			TourLegNumber(), *TourStopIds[TourSetDownStop], TourLegNumber() + 1);
	}
	TourSetDownStop = -1;
	if (++TourLeg < TourEndLeg)
	{
		return;
	}
	EndWorldTour(Game);
}

void AFTExploreDirector::EndWorldTour(double Game)
{
	bWorldTourDone = true;
	UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
	// Each edge's tally first, once, when the tour ends.
	for (int32 e = 0; e < static_cast<int32>(UE_ARRAY_COUNT(TourEdges)); ++e)
	{
		const ftcore::EdgeTally& Tally = TourEdges[e];
		if (Tally.legs == 0)
		{
			continue;
		}
		const FString Summary =
			FString::Printf(TEXT("World %s: EDGE %c: %d of %d met a cause (cliff %d, water %d, drop %d, built wall %d); bare hidden wall %d; unjudged %d."),
				*WorldId, kEdgeLetters[e], Tally.met(), Tally.legs, Tally.cliff, Tally.water, Tally.drop, Tally.built, Tally.bare, Tally.unjudged);
		if (Tally.bare == 0 && Tally.unjudged == 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s"), *Summary);
		}
		else
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s"), *Summary);
		}
	}
	FString Legs; // once, when the tour ends
	for (int32 i = 0; i < TourMissLegs.Num(); ++i)
	{
		Legs += FString::Printf(TEXT("%s%d"), i ? TEXT(", ") : TEXT(""), TourMissLegs[i]);
	}
	const double Real = Now() - TourStart;
	if (TourStopOrder.IsEmpty() && TourWetLegs == 0)
	{
		// The district legs alone, as before.
		if (TourMisses == 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("World %s: TOUR DONE, all %d districts in %d legs, %.1f s game time (%.1f s real)."), *WorldId,
				static_cast<int32>(Layout->districtCount()), TourCount, Game, Real);
			if (bTourQuitOnDone)
			{
				FPlatformMisc::RequestExit(false, TEXT("WorldTour"));
			}
			return;
		}
		UE_LOG(LogForgottenThird, Error, TEXT("World %s: TOUR ENDED, %d of %d legs given up (legs %s), %.1f s game time (%.1f s real)."), *WorldId,
			TourMisses, TourCount, *Legs, Game, Real);
		if (bTourQuitOnDone)
		{
			FTRequestExit(1, TEXT("WorldTour"));
		}
		return;
	}
	const int32 Stops = TourStopsRun[0] + TourStopsRun[1] + TourStopsRun[2];
	const FString Counts = FString::Printf(TEXT("%d legs: %d district legs, %d stops (reach %d, reached %d; edge %d, met a cause %d; push %d, held %d)"),
		TourLegTotal, TourCount - TourFirstLeg, Stops, TourStopsRun[0], TourStopsReached, TourStopsRun[1], TourEdgesMet, TourStopsRun[2], TourPushHeld);
	const int32 Fails = TourMisses + TourBareWalls + TourUnjudged + TourGotThrough + TourWetLegs;
	if (Fails == 0)
	{
		UE_LOG(LogForgottenThird, Display,
			TEXT("World %s: TOUR DONE, %s; fails: gave up 0, bare hidden wall 0, unjudged 0, got through 0, wet 0; %.1f s game time (%.1f s real)."),
			*WorldId, *Counts, Game, Real);
		if (bTourQuitOnDone)
		{
			FPlatformMisc::RequestExit(false, TEXT("WorldTour"));
		}
		return;
	}
	const FString GaveUp = TourMisses > 0 ? FString::Printf(TEXT("gave up %d (legs %s)"), TourMisses, *Legs) : FString(TEXT("gave up 0"));
	UE_LOG(LogForgottenThird, Error,
		TEXT("World %s: TOUR ENDED, %s; %d fails: %s, bare hidden wall %d, unjudged %d, got through %d, wet %d; %.1f s game time (%.1f s real)."),
		*WorldId, *Counts, Fails, *GaveUp, TourBareWalls, TourUnjudged, TourGotThrough, TourWetLegs, Game, Real);
	if (bTourQuitOnDone)
	{
		FTRequestExit(1, TEXT("WorldTour"));
	}
}

FString AFTExploreDirector::WorldName(int32 World) const
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI || World < 0 || World >= static_cast<int32>(GI->GetWorlds().worlds.size()))
	{
		return FString();
	}
	return FT::ToFString(GI->GetWorlds().worlds[static_cast<size_t>(World)].id);
}

void AFTExploreDirector::BeginOverworld()
{
	if (!bOverworld)
	{
		return;
	}
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI)
	{
		return;
	}
	const ftcore::WorldTable& Worlds = GI->GetWorlds();
	Overworld = MakeUnique<ftcore::OverworldLayout>();
	std::string Error;
	if (!ftcore::generateOverworldLayout(Worlds, *Overworld, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("Overworld: %s"), *FT::ToFString(Error).TrimEnd());
		Overworld.Reset();
		return;
	}
	const int32 Count = static_cast<int32>(Overworld->nodeCount());
	UE_LOG(LogForgottenThird, Display, TEXT("Overworld: %d worlds, %d roads, %d emblem blocks, %d props over %.0f m."), Count,
		static_cast<int32>(Overworld->roadCount()), static_cast<int32>(Overworld->blockCount()),
		static_cast<int32>(Overworld->propCount()), Overworld->side());

	// Open worlds on a free walk: those whose first placed scene the story has reached; all of them
	// when the story isn't on a scene (a walk started from the map itself).
	OpenWorlds = (1u << Count) - 1u;
	const UFTStorySubsystem* Story = GI->GetSubsystem<UFTStorySubsystem>();
	if (Story && Story->IsLoaded() && Story->GetRunner().state() != ftcore::StoryRunner::State::Idle)
	{
		const ftcore::StoryDatabase& Db = Story->GetDatabase();
		const std::string_view At = Story->GetRunner().position().scene;
		std::vector<std::string_view> Order;
		Order.reserve(Db.sceneCount());
		size_t Reached = 0;
		for (uint32 I = 0; I < static_cast<uint32>(Db.sceneCount()); ++I)
		{
			Order.push_back(Db.view(Db.scene(I).id));
			if (Order.back() == At)
			{
				Reached = I;
			}
		}
		std::vector<size_t> First;
		ftcore::worldFirstScenes(Worlds, Order, First);
		OpenWorlds = ftcore::worldsOpen(First, Reached);
	}

	// Spawn at a gate, facing the way on: the hop's start, or world 0's.
	const FFTStoryCarry& Carry = GI->StoryCarry;
	const bool bStoryStep = Carry.bValid && !Carry.bDone && Carry.Screen == ftcore::GameplayScreen::Field;
	ftcore::Vec2 Spawn = Overworld->node(0).gate;
	float Yaw = Overworld->gateYaw(0);
	if (bStoryStep)
	{
		const FTCHARToUTF8 FromUtf8(*Carry.FromWorld);
		const FTCHARToUTF8 ToUtf8(*Carry.World);
		const int32 From = Worlds.findWorld(std::string_view(FromUtf8.Get(), static_cast<size_t>(FromUtf8.Length())));
		const int32 To = Worlds.findWorld(std::string_view(ToUtf8.Get(), static_cast<size_t>(ToUtf8.Length())));
		bHop = From >= 0 && To >= 0 && ftcore::overworldRoute(*Overworld, static_cast<size_t>(From), static_cast<size_t>(To), Hop);
		if (!bHop && To < 0)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("Overworld: '%s' is not a world in worlds.json (%s); the overworld is a free walk."),
				*Carry.World, *Carry.Scene);
		}
		else if (!bHop)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Overworld: no way from '%s' to '%s' for %s; entering '%s' straight away."),
				*Carry.FromWorld, *Carry.World, *Carry.Scene, *Carry.World);
			bEntering = true;
			FTimerHandle Timer;
			GetWorldTimerManager().SetTimer(Timer, FTimerDelegate::CreateWeakLambda(this, [this, To]()
			{
				bEntering = false;
				EnterWorld(To);
			}), 0.1f, false);
			return;
		}
		else
		{
			bAutoPlay = bAutoPlay || Carry.bAutoPlay;
			HopPoint = 0;
			Spawn = Hop.start;
			Yaw = Hop.startYaw;
			StoryWalkStart = GetWorld()->GetTimeSeconds();
			SetBanner(FString::Printf(TEXT("To %s"), *Carry.World));
			if (bAutoPlay && IsAlphaFast())
			{
				UGameplayStatics::SetGlobalTimeDilation(this, 8.f);
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Overworld: hop from %s to %s for %s beat %d (%d gates on the way)."),
				*Carry.FromWorld, *Carry.World, *Carry.Scene, Carry.Beat, static_cast<int32>(Hop.pointCount));
		}
	}
	WorldSpawn = FVector(Spawn.x * 100.0, Spawn.y * 100.0, 0.0);
	WorldSpawnRotation = FRotator(0.f, FMath::RadiansToDegrees(Yaw), 0.f);
	bWorldSpawnOnFloor = false;
	bWorldSpawnPending = true;

	bWorldTour = !bStoryStep && FParse::Param(FCommandLine::Get(), TEXT("OverworldAutoPlay"));
	if (!bWorldTour)
	{
		return;
	}
	// Every gate in turn from world 0's, along the roads.
	OverTourCount = 0;
	OverTour[OverTourCount++] = 0;
	for (int32 To = 1; To < Count; ++To)
	{
		uint8 Way[ftcore::OverworldLayout::kMaxNodes];
		const size_t N = Overworld->path(OverTour[OverTourCount - 1], static_cast<size_t>(To), Way, UE_ARRAY_COUNT(Way));
		for (size_t K = 1; K < N && OverTourCount < static_cast<int32>(UE_ARRAY_COUNT(OverTour)); ++K)
		{
			OverTour[OverTourCount++] = Way[K];
		}
	}
	TourLeg = 1; // the walk starts at the first gate
	TourStart = Now();
	TourGameStart = GetWorld()->GetTimeSeconds();
	if (IsAlphaFast())
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 8.f);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Overworld: tour of %d gates over the roads."), OverTourCount);
}

void AFTExploreDirector::UpdateOverworldLine(ftcore::Vec2 Pos)
{
	const int32 At = Overworld->nodeAt(Pos);
	if (At == OverLineNode && !WorldLine.IsEmpty())
	{
		return;
	}
	OverLineNode = At;
	if (At >= 0 && (bHop ? At == Hop.to : ((OpenWorlds >> At) & 1u) != 0))
	{
		WorldLine = FString::Printf(TEXT("%s  E  Enter"), *WorldName(At));
	}
	else if (At >= 0 && !bHop)
	{
		WorldLine = FString::Printf(TEXT("%s  closed"), *WorldName(At));
	}
	else if (bHop)
	{
		WorldLine = FString::Printf(TEXT("to %s"), *WorldName(Hop.to));
	}
	else
	{
		WorldLine = TEXT("open road");
	}
}

void AFTExploreDirector::TickOverworld(AFTExploreCharacter& Player, float DeltaSeconds)
{
	TickFollowers(Player, DeltaSeconds);
	const ftcore::Vec2 Pos = ToGround(Player.GetActorLocation());
	UpdateOverworldLine(Pos);
	if (bEntering)
	{
		return;
	}
	if (bHop)
	{
		if (!bAutoPlay)
		{
			return;
		}
		// The roads run straight gate to gate and nothing stands on them, so no navmesh is needed.
		const ftcore::Vec2 Target = Hop.points[static_cast<size_t>(HopPoint)];
		if (FVector2D(Target.x - Pos.x, Target.y - Pos.y).Size() > ftcore::OverworldLayout::kGateReach)
		{
			PilotWalk(Player, Pos, Target, kOverTargetBase + HopPoint, DeltaSeconds, false);
			return;
		}
		if (++HopPoint >= static_cast<int32>(Hop.pointCount))
		{
			EnterWorld(Hop.to);
		}
		return;
	}
	if (!bWorldTour || bWorldTourDone || TourLeg >= OverTourCount)
	{
		return;
	}
	const int32 Node = OverTour[TourLeg];
	const ftcore::Vec2 Target = Overworld->node(static_cast<size_t>(Node)).gate;
	if (FVector2D(Target.x - Pos.x, Target.y - Pos.y).Size() > ftcore::OverworldLayout::kGateReach)
	{
		PilotWalk(Player, Pos, Target, kOverTargetBase + TourLeg, DeltaSeconds, false);
		return;
	}
	const double Game = GetWorld()->GetTimeSeconds() - TourGameStart;
	UE_LOG(LogForgottenThird, Display, TEXT("Overworld: reached the %s gate (leg %d/%d) at %.1f s game time."), *WorldName(Node),
		TourLeg, OverTourCount - 1, Game);
	if (++TourLeg < OverTourCount)
	{
		return;
	}
	bWorldTourDone = true;
	UE_LOG(LogForgottenThird, Display, TEXT("Overworld: OVERWORLD TOUR DONE, all %d gates in %d legs, %.1f s game time (%.1f s real)."),
		static_cast<int32>(Overworld->nodeCount()), OverTourCount - 1, Game, Now() - TourStart);
	UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
	if (bQuitOnComplete || FParse::Param(FCommandLine::Get(), TEXT("WorldQuitOnDone")))
	{
		FPlatformMisc::RequestExit(false, TEXT("OverworldTour"));
	}
}

void AFTExploreDirector::EnterWorld(int32 World)
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (bEntering || !GI || World < 0 || World >= static_cast<int32>(GI->GetWorlds().worlds.size()))
	{
		return;
	}
	bEntering = true;
	const FString Map = FT::ToFString(GI->GetWorlds().worlds[static_cast<size_t>(World)].map);
	FFTStoryCarry& Carry = GI->StoryCarry;
	if (Carry.bValid && !Carry.bDone && !Carry.FromWorld.IsEmpty())
	{
		// The field step itself plays on the world's map; the carry goes on with it.
		UE_LOG(LogForgottenThird, Display, TEXT("Overworld: hop from %s to %s done after %.1f s game time."), *Carry.FromWorld,
			*Carry.World, GetWorld()->GetTimeSeconds() - StoryWalkStart);
		Carry.FromWorld.Empty();
		++GI->Alpha.hops;
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Overworld: entering %s."), *WorldName(World));
	}
	SetBanner(FString::Printf(TEXT("Entering %s"), *WorldName(World)));
	UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
	FTStep::OpenMap(this, Map);
}

