#include "FTArenaHost.h"

#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ForgottenThird.h"
#include "FTArenaFighter.h"
#include "FTArenaHUD.h"
#include "FTArenaRunner.h"
#include "FTExploreCharacter.h"
#include "FTExploreDirector.h"
#include "FTGameInstance.h"
#include "FTPartyFollower.h"
#include "FTStepHost.h"
#include "FTText.h"

#include "ftcore/WorldLayout.h"

#include <cmath>
#include <string>
#include <vector>

namespace
{
// How long the result stays on the screen before the explore character comes back (the autopilot's runs: shorter).
constexpr double kArenaHostHold = 3.0;
constexpr double kArenaHostAutoHold = 1.0;
// How far inside the zone's walls a follower stands (metres).
constexpr float kParkMargin = 0.4f;
// The line-up when arenas.json can't give one.
const char* const kHostWave[] = {"brute", "skirmisher", "breaker"};

bool ReadData(const TCHAR* Relative, TArray<uint8>& Bytes)
{
	const FString File = FPaths::ProjectDir() / Relative;
	return IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File);
}
} // namespace

AFTArenaHost::AFTArenaHost()
{
	PrimaryActorTick.bCanEverTick = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void AFTArenaHost::HostSteps(AFTExploreDirector* Explore)
{
	ExploreDirector = Explore;
	UFTStepHostSubsystem* Hosts = GetWorld() ? GetWorld()->GetSubsystem<UFTStepHostSubsystem>() : nullptr;
	if (Hosts)
	{
		Hosts->Register(EFTStepHost::Arena, FFTStartStepHere::CreateUObject(this, &AFTArenaHost::StartStepHere));
		bRegistered = true;
	}
}

const UFTArenaRunner* AFTArenaHost::GetRunner() const
{
	return (bFighting && Runner && Runner->IsBegun()) ? Runner.Get() : nullptr;
}

void AFTArenaHost::EndPlay(const EEndPlayReason::Type Reason)
{
	if (bRegistered)
	{
		if (UFTStepHostSubsystem* Hosts = GetWorld() ? GetWorld()->GetSubsystem<UFTStepHostSubsystem>() : nullptr)
		{
			Hosts->Unregister(EFTStepHost::Arena, this);
		}
		bRegistered = false;
	}
	// Destroyed mid-fight on a map that stays: the lead gets its body, view and party back. A world being torn down
	// (the director destroys this on its way out, a quit) gets nothing back: nothing may spawn then, the HUD included.
	if (bFighting && Reason == EEndPlayReason::Destroyed && GetWorld() && !GetWorld()->bIsTearingDown)
	{
		EndInPlace();
	}
	else if (bFighting && Runner)
	{
		bFighting = false;
		Runner->End();
	}
	Super::EndPlay(Reason);
}

bool AFTArenaHost::LoadTables()
{
	if (bTablesTried)
	{
		return bTablesLoaded;
	}
	bTablesTried = true;
	TArray<uint8> Bytes;
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ReadData(TEXT("Data/Battle/arena.json"), Bytes) || !ftcore::importArenaTable(FT::BytesAsText(Bytes), Table, &Error, &Warnings))
	{
		// The loader's errors name paths and keys only.
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/arena.json for the arena host; the fight uses the built-in one.\n%s"),
			*FT::ToFString(Error));
		Table = ftcore::ArenaTable();
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s"), *FT::ToFString(W));
	}
	bTablesLoaded = true;
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	Bytes.Reset();
	Error.clear();
	Warnings.clear();
	if (GI && ReadData(TEXT("Data/Battle/arenas.json"), Bytes)
		&& !ftcore::importArenaZoneTable(FT::BytesAsText(Bytes), Table, GI->GetWorlds(), Zones, &Error, &Warnings))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: Data/Battle/arenas.json refused; the arena host uses the default line-up:\n%s"),
			*FT::ToFString(Error));
		Zones = ftcore::ArenaZoneTable();
	}
	return true;
}

bool AFTArenaHost::BuildFight(const AFTExploreCharacter& Body, int32 District, const FString& World, ftcore::ArenaDef& Def,
	ftcore::ArenaZoneFit& Fit, std::string& ZoneId, FString& KitId) const
{
	const std::string WorldId = FT::ToUtf8(World);
	const AFTExploreDirector* Explore = ExploreDirector.Get();
	const ftcore::WorldLayout* Layout = Explore ? Explore->GetWorldLayout() : nullptr;

	// The zone: the one arenas.json anchors on this district of this world, else the plaza's own, with the default
	// zone's line-up; off a big world, a plain zone about the lead.
	ftcore::ArenaZoneSpec Spec;
	const ftcore::ArenaZoneSpec* Named = nullptr;
	for (const ftcore::ArenaZoneSpec& Z : Zones.zones)
	{
		if (!WorldId.empty() && Z.world == WorldId && Z.district == District)
		{
			Named = &Z;
			break;
		}
	}
	const FVector Feet = Body.GetActorLocation();
	if (Named)
	{
		Spec = *Named;
	}
	else
	{
		Spec.id = Layout && District >= 0 ? WorldId + "_d" + std::to_string(District) : std::string("here");
		Spec.world = Layout ? WorldId : std::string();
		Spec.district = Layout ? District : -1;
		Spec.at = ftcore::Vec2{static_cast<float>(Feet.X / 100.0), static_cast<float>(Feet.Y / 100.0)};
		if (const ftcore::ArenaZoneSpec* Default = Zones.defaultZone())
		{
			Spec.bosses = Default->bosses;
		}
	}

	// The lead's own kit (its attacks and its blocks), as the arena demo picks it.
	Def = bTablesLoaded ? Table.fallback : ftcore::defaultArenaDef();
	const TArray<FFTLeadLook>& Party = Body.PartyLooks;
	const int32 LeadIndex = Body.GetLeadIndex();
	const FString Want = Party.IsValidIndex(LeadIndex) ? Party[LeadIndex].Name.ToLower() : FString(TEXT("default"));
	KitId = bTablesLoaded ? FString(TEXT("default")) : FString(TEXT("built-in"));
	if (bTablesLoaded)
	{
		const std::string WantId = FT::ToUtf8(Want);
		Def.fighter = Table.fighter(WantId);
		for (const std::string& Id : Table.fighterIds)
		{
			KitId = Id == WantId ? Want : KitId;
		}
		// The line-up: the zone's bosses, else the three-kind wave.
		int32 Count = 0;
		const auto Add = [&](const std::string& Kind)
		{
			for (size_t K = 0; K < Table.enemyIds.size() && Count < UFTArenaRunner::MaxEnemies; ++K)
			{
				if (Table.enemyIds[K] == Kind)
				{
					Def.enemies[static_cast<size_t>(Count)] = Table.enemies[K];
					Def.enemyKind[static_cast<size_t>(Count)] = static_cast<uint8>(K);
					++Count;
					return;
				}
			}
		};
		if (Spec.bosses.empty())
		{
			for (const char* Kind : kHostWave)
			{
				Add(Kind);
			}
		}
		for (const std::string& Kind : Spec.bosses)
		{
			Add(Kind);
		}
		if (Count > 0)
		{
			Def.enemyCount = static_cast<uint8>(Count);
			Def.attackers = static_cast<uint8>(FMath::Clamp(Spec.attackers > 0 ? Spec.attackers : 1, 1, Count));
		}
	}
	if (Def.enemyCount == 0)
	{
		return false;
	}

	// Fitted to the world's real layout; a district with no room gives a plain zone about the lead.
	std::string FitError;
	std::vector<std::string> FitWarnings;
	if (!ftcore::fitArenaZone(Spec, Spec.world.empty() ? nullptr : Layout, Def, Fit, &FitError, &FitWarnings))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s\nArena: the fight uses a plain zone about the lead instead."), *FT::ToFString(FitError));
		Spec.id = "here";
		Spec.world.clear();
		Spec.district = -1;
		Spec.at = ftcore::Vec2{static_cast<float>(Feet.X / 100.0), static_cast<float>(Feet.Y / 100.0)};
		Spec.offset = ftcore::Vec2{};
		Spec.kind = ftcore::ZoneKind::Circle;
		Spec.water.clear();
		Spec.cover.clear();
		FitWarnings.clear();
		if (!ftcore::fitArenaZone(Spec, nullptr, Def, Fit, nullptr, nullptr))
		{
			return false;
		}
	}
	for (const std::string& W : FitWarnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s"), *FT::ToFString(W));
	}
	Fit.apply(Def);
	ZoneId = Spec.id;
	return true;
}

void AFTArenaHost::StartStepHere()
{
	if (bFighting)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: an arena step began while a fight was on; ignored."));
		return;
	}
	bFinished = false;
	UWorld* World = GetWorld();
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const FFTStoryCarry* Carry = (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone) ? &GI->StoryCarry : nullptr;
	bHasStep = Carry != nullptr;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	AFTExploreCharacter* Body = Cast<AFTExploreCharacter>(UGameplayStatics::GetPlayerPawn(this, 0));
	if (!PC || !Body)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no explore character to fight from; the step is left undone."));
		return;
	}
	LoadTables();

	// The district: the step's, else the one the lead stands in.
	const AFTExploreDirector* Explore = ExploreDirector.Get();
	const ftcore::WorldLayout* Layout = Explore ? Explore->GetWorldLayout() : nullptr;
	int32 District = Carry ? Carry->District : -1;
	FString WorldName = Carry ? Carry->World : FString();
	if (District < 0 && Layout)
	{
		const FVector At = Body->GetActorLocation();
		District = Layout->districtAt(ftcore::Vec2{static_cast<float>(At.X / 100.0), static_cast<float>(At.Y / 100.0)});
	}
	if (WorldName.IsEmpty() && Layout && GI)
	{
		// The world whose map this is.
		const FString Map = UGameplayStatics::GetCurrentLevelName(this, true);
		for (const ftcore::WorldSpec& Spec : GI->GetWorlds().worlds)
		{
			WorldName = FT::ToFString(Spec.map) == Map ? FT::ToFString(Spec.id) : WorldName;
		}
	}

	FFTArenaRunSetup Setup;
	std::string ZoneId;
	if (!BuildFight(*Body, District, WorldName, Setup.Def, Setup.Fit, ZoneId, Setup.KitId))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no line-up for the arena step; the step is left undone."));
		return;
	}
	Setup.Table = bTablesLoaded ? &Table : nullptr;
	Setup.ZoneId = ZoneId;
	Setup.ZoneZ = UFTArenaRunner::SampleGround(World, Setup.Fit.zone.centre);
	Setup.ZoneStyle = WorldName == TEXT("solhaven") ? 1 : (WorldName == TEXT("lattice") ? 2 : (WorldName == TEXT("unwritten") ? 3 : 0));
	const int32 LeadIndex = Body->GetLeadIndex();
	Setup.Lead = Body->PartyLooks.IsValidIndex(LeadIndex) ? &Body->PartyLooks[LeadIndex] : nullptr;
	const TCHAR* Cmd = FCommandLine::Get();
	Setup.bAutoPlay = (Carry && Carry->bAutoPlay) || FParse::Param(Cmd, TEXT("ArenaAutoPlay"));
	Setup.bIdle = !Setup.bAutoPlay && FParse::Param(Cmd, TEXT("ArenaIdle"));
	// Only a standalone fight may end the process; a story step hands back to the story.
	Setup.bQuitOnFinish = !Carry && FParse::Param(Cmd, TEXT("ArenaQuitOnFinish"));
	Setup.bFightAgain = false;
	Setup.bReplacePawn = false;
	uint32 Seed = 1;
	for (const ftcore::ArenaZoneSpec& Z : Zones.zones)
	{
		Seed = Z.id == ZoneId ? Z.seed : Seed;
	}
	Setup.Seed = Seed;

	const ftcore::ArenaZoneFit& Fit = Setup.Fit;
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: in place on %s, district %d: zone %s, kit %s, %d enemies, ring %.1f m (asked %.1f m), ground at %.2f m, %s."),
		*UGameplayStatics::GetCurrentLevelName(this, true), District, *FT::ToFString(ZoneId), *Setup.KitId, static_cast<int32>(Setup.Def.enemyCount),
		Fit.ring.radius, Fit.askedRing, Setup.ZoneZ / 100.0, UFTArenaRunner::PilotName(Setup.bAutoPlay, Setup.bIdle));

	Lead = Body;
	StepOut(*Body);
	if (AFTExploreDirector* Director = ExploreDirector.Get())
	{
		Director->SetPartyHeld(true);
	}
	ParkFollowers(Fit);
	ExploreHUD = PC->GetHUD() ? PC->GetHUD()->GetClass() : nullptr;
	PC->ClientSetHUD(AFTArenaHUD::StaticClass());

	// A fresh runner per fight: its counts and clocks start at zero.
	if (Runner)
	{
		Runner->DestroyComponent();
	}
	Runner = NewObject<UFTArenaRunner>(this, TEXT("Runner"));
	Runner->RegisterComponent();
	bFighting = true;
	Runner->Begin(Setup);
}

void AFTArenaHost::StepOut(AFTExploreCharacter& Body)
{
	// The figure fights in the body's place: the body waits out of sight, out of the way and still.
	Body.SetActorHiddenInGame(true);
	Body.SetActorEnableCollision(false);
	if (UCharacterMovementComponent* Move = Body.GetCharacterMovement())
	{
		Move->StopMovementImmediately();
		Move->DisableMovement();
	}
}

void AFTArenaHost::ParkFollowers(const ftcore::ArenaZoneFit& Fit)
{
	const AFTExploreDirector* Explore = ExploreDirector.Get();
	if (!Explore)
	{
		return;
	}
	// Core's spots just outside the ring behind the lead's start, pulled back inside the zone's walls (the ring is the
	// zone's bounding circle, which a building may cut), facing the way the lead does.
	const FRotator Facing(0.f, FMath::RadiansToDegrees(std::atan2(Fit.playerFacing.y, Fit.playerFacing.x)), 0.f);
	size_t Slot = 0;
	for (AFTPartyFollower* Follower : Explore->GetFollowers())
	{
		if (!Follower)
		{
			continue;
		}
		const ftcore::Vec2 At = Fit.zone.clamp(ftcore::arenaParkSpot(Fit.ring, Fit.playerStart, Slot), kParkMargin);
		const double Ground = UFTArenaRunner::SampleGround(GetWorld(), At);
		const FVector Location(At.x * 100.0, At.y * 100.0, Ground + Follower->GetSimpleCollisionHalfHeight() + 2.0);
		Follower->SetActorLocationAndRotation(Location, Facing, false, nullptr, ETeleportType::TeleportPhysics);
		++Slot;
	}
}

void AFTArenaHost::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bFighting || !Runner)
	{
		return;
	}
	Runner->TickFight(DeltaSeconds, false);
	const double Hold = Runner->IsAutoPlay() ? kArenaHostAutoHold : kArenaHostHold;
	if (Runner->IsFinished() && Runner->GetResultShowAge() >= Hold && !bFinished)
	{
		Finish();
	}
}

void AFTArenaHost::EndInPlace()
{
	bFighting = false;
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	AFTExploreCharacter* Body = Lead.Get();
	if (Body)
	{
		// Back where the figure stands, facing the way the camera looks.
		FVector At = Body->GetActorLocation();
		float Yaw = Body->GetActorRotation().Yaw;
		if (Runner && Runner->IsBegun())
		{
			const ftcore::Vec2 P = Runner->GetPlayerPosition();
			At = FVector(P.x * 100.0, P.y * 100.0, Runner->GetZoneZ() + Body->GetSimpleCollisionHalfHeight() + 2.0);
			Yaw = PC ? static_cast<float>(PC->GetControlRotation().Yaw) : Yaw;
		}
		Body->SetActorLocationAndRotation(At, FRotator(0.f, Yaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
		Body->SetActorHiddenInGame(false);
		Body->SetActorEnableCollision(true);
		if (UCharacterMovementComponent* Move = Body->GetCharacterMovement())
		{
			Move->SetMovementMode(MOVE_Walking);
		}
		if (PC)
		{
			PC->Possess(Body);
			PC->SetControlRotation(FRotator(-10.f, Yaw, 0.f));
			PC->SetViewTarget(Body);
		}
		Body->ResetHeldInput();
	}
	if (Runner)
	{
		Runner->End();
	}
	if (PC && ExploreHUD)
	{
		PC->ClientSetHUD(ExploreHUD);
	}
	if (AFTExploreDirector* Explore = ExploreDirector.Get())
	{
		Explore->SetPartyHeld(false);
	}
}

void AFTArenaHost::Finish()
{
	bFinished = true;
	const bool bWon = Runner && Runner->GetFight().phase() == ftcore::ArenaPhase::Victory;
	EndInPlace();
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!bHasStep || !GI || !GI->StoryCarry.bValid)
	{
		return; // a standalone fight: stay on the map
	}
	FFTStoryCarry& Carry = GI->StoryCarry;
	Carry.bDone = true;
	Carry.bWon = bWon;
	if (FTStep::ContinueHere(this))
	{
		return; // the beat's next step plays on this map
	}
	UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
	FTStep::OpenMap(this, Carry.ReturnMap);
}
