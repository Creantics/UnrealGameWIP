#include "FTBattleDemoActor.h"

#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

#include "ForgottenThird.h"
#include "FTConductorComponent.h"
#include "FTGameInstance.h"
#include "FTMeshFacing.h"
#include "FTPlayerController.h"
#include "FTText.h"

#include "ftcore/BattleDemo.h"

#include <string>

namespace
{
// Global time dilation of an autopilot fight under -AlphaFast.
constexpr float kAlphaFastDilation = 8.f;

// -AlphaFast. UFTGameInstance::IsAlphaFast() (stream 1) is the same switch; read here so this file
// doesn't depend on it landing first.
bool IsAlphaFastRequested()
{
	return FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
}

// Data/Battle/enemies.json and encounters.json; false (and logged) when either can't be read or
// is refused. Ids and numbers only: nothing story-related is logged.
bool LoadBattleData(ftcore::EnemyRoster& Roster, ftcore::EncounterTable& Table)
{
	const FString Dir = FPaths::ProjectDir() / TEXT("Data/Battle");
	TArray<uint8> EnemyBytes, EncounterBytes;
	if (!FFileHelper::LoadFileToArray(EnemyBytes, *(Dir / TEXT("enemies.json")))
		|| !FFileHelper::LoadFileToArray(EncounterBytes, *(Dir / TEXT("encounters.json"))))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: Data/Battle not found under %s; using the built-in line-up."), *Dir);
		return false;
	}
	std::string Error;
	if (!ftcore::importEnemyRoster(FT::BytesAsText(EnemyBytes), Roster, &Error)
		|| !ftcore::importEncounterTable(FT::BytesAsText(EncounterBytes), Roster, Table, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: Data/Battle refused (%s); using the built-in line-up."), UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	return true;
}

FFTCombatant FromCore(const ftcore::Combatant& C)
{
	FFTCombatant Out;
	Out.Name = FT::ToFString(C.name);
	Out.Stats.MaxHp = C.stats.maxHp;
	Out.Stats.Attack = C.stats.attack;
	Out.Stats.Defense = C.stats.defense;
	Out.Stats.Speed = C.stats.speed;
	Out.Stats.Technique = C.stats.technique;
	Out.Hp = C.hp;
	Out.Ap = C.ap;
	Out.MaxAp = C.maxAp;
	for (const ftcore::Skill& S : C.skills)
	{
		FFTSkill& D = Out.Skills.AddDefaulted_GetRef();
		D.Name = FT::ToFString(S.name);
		D.ApCost = S.apCost;
		D.ApGain = S.apGain;
		D.Targeting = static_cast<EFTTargeting>(S.targeting);
		D.Power = S.power;
		D.Notes = S.notes;
		D.Difficulty = S.difficulty;
	}
	return Out;
}

// Figure colours: party members first, then enemies.
const FLinearColor kPartyColors[] = {FLinearColor(0.85f, 0.22f, 0.28f), FLinearColor(0.25f, 0.45f, 0.85f), FLinearColor(0.92f, 0.74f, 0.28f)};
const FLinearColor kEnemyColors[] = {FLinearColor(0.48f, 0.33f, 0.25f), FLinearColor(0.22f, 0.42f, 0.44f), FLinearColor(0.38f, 0.3f, 0.42f)};

constexpr double kFlashSeconds = 0.35;
constexpr double kLungeSeconds = 0.3;
constexpr double kFallSeconds = 0.6;
constexpr double kBannerSeconds = 1.6;

const FString kEmpty;

double Wall() { return FPlatformTime::Seconds(); }
} // namespace

AFTBattleDemoActor::AFTBattleDemoActor()
{
	PrimaryActorTick.bCanEverTick = true;
	// Before the battle component (TG_DuringPhysics) so autoplay presses land before notes expire.
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Conductor = CreateDefaultSubobject<UFTConductorComponent>(TEXT("Conductor"));
	Conductor->Bpm = ftcore::demo::kBpm;
	Conductor->bWallClockWithoutAudio = true;

	Battle = CreateDefaultSubobject<UFTBattleComponent>(TEXT("Battle"));
	Battle->bSyncToMusic = true;
	for (const ftcore::Combatant& C : ftcore::demo::party())
	{
		Battle->Party.Add(FromCore(C));
	}
	for (const ftcore::Combatant& C : ftcore::demo::enemies())
	{
		Battle->Enemies.Add(FromCore(C));
	}
	const ftcore::BattleTuning Tuning = ftcore::demo::tuning();
	Battle->Tuning.LeadIn = Tuning.leadIn;
	Battle->Tuning.Windup = Tuning.windup;
	Battle->Tuning.Recovery = Tuning.recovery;
	Battle->Tuning.Lockout = Tuning.lockout;
	Battle->Tuning.StartingAp = Tuning.startingAp;

	// Looking along +X at the two lines: the party on the left (-Y), the enemies on the right (+Y).
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Root);
	Camera->SetRelativeLocation(FVector(-1000.0, 0.0, 380.0));
	Camera->SetRelativeRotation(FRotator(-13.0, 0.0, 0.0));
	Camera->SetFieldOfView(60.f);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> Devin(TEXT("/Game/Characters/Devin/devin/SkeletalMeshes/devin.devin"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> DevinIdle(TEXT("/Game/Characters/Devin/devin/SkeletalMeshes/devinidle.devinidle"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> Duy(TEXT("/Game/Characters/Duy/duy/SkeletalMeshes/duy.duy"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> DuyIdle(TEXT("/Game/Characters/Duy/duy/SkeletalMeshes/duyidle.duyidle"));
	PartyMeshes = {Devin.Object, Duy.Object};
	PartyIdles = {DevinIdle.Object, DuyIdle.Object};
	PartyBodyMesh = Cylinder.Object;
	PartyHeadMesh = Sphere.Object;
	EnemyMesh = Cube.Object;
	FigureMaterial = Material.Object;
}

void AFTBattleDemoActor::BeginPlay()
{
	Super::BeginPlay();
	// The exploration demo's autoplay runs its fights too.
	bAutoPlay = bAutoPlay || FParse::Param(FCommandLine::Get(), TEXT("BattleAutoPlay")) || FParse::Param(FCommandLine::Get(), TEXT("ExploreAutoPlay"));
	bQuitOnOutcome = bQuitOnOutcome || FParse::Param(FCommandLine::Get(), TEXT("BattleQuitOnOutcome"));
	// So does the story's, when it hands a fight here.
	bool bStoryAutoPlay = false;
	if (const UFTGameInstance* StoryGI = GetGameInstance<UFTGameInstance>())
	{
		bStoryAutoPlay = StoryGI->StoryCarry.bValid && StoryGI->StoryCarry.bAutoPlay;
		bAutoPlay = bAutoPlay || bStoryAutoPlay;
	}

	// A headless alpha run: play the fight 8x faster. The song clock has to follow the dilated game
	// time, or the combo timing and the autopilot's presses would fall out of step.
	if (bStoryAutoPlay && IsAlphaFastRequested())
	{
		Conductor->bWallClockFollowsGameTime = true;
		Conductor->ResetClock(); // its BeginPlay already seeded the clock from platform time
		UGameplayStatics::SetGlobalTimeDilation(this, kAlphaFastDilation);
		bDilated = true;
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: -AlphaFast, game time x%.0f."), kAlphaFastDilation);
	}

	Battle->OnBattleEvent.AddDynamic(this, &AFTBattleDemoActor::HandleBattleEvent);
	Battle->OnTurnStarted.AddDynamic(this, &AFTBattleDemoActor::HandleTurnStarted);
	Battle->OnOutcome.AddDynamic(this, &AFTBattleDemoActor::HandleOutcome);

	LoadLineup();
	ApplyLineup();
	BuildFigures();
	// Next tick: the player controller and its pawn are settled by then.
	GetWorldTimerManager().SetTimerForNextTick(this, &AFTBattleDemoActor::StartRound);
}

void AFTBattleDemoActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AFTPlayerController* PC = GetPlayer())
	{
		PC->EndBattleInput();
	}
	if (bDilated && GetWorld())
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
		bDilated = false;
	}
	Super::EndPlay(EndPlayReason);
}

void AFTBattleDemoActor::LoadLineup()
{
	CoreParty = ftcore::demo::party();
	CoreFoes = ftcore::demo::enemies();

	ftcore::EnemyRoster Roster;
	ftcore::EncounterTable Table;
	if (!LoadBattleData(Roster, Table))
	{
		return;
	}
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const bool bCarry = GI && GI->StoryCarry.bValid && GI->StoryCarry.Beat >= 0;
	const std::string Scene = bCarry ? FT::ToUtf8(GI->StoryCarry.Scene) : std::string();
	const uint32 Beat = bCarry ? static_cast<uint32>(GI->StoryCarry.Beat) : 0u;
	const uint32 Step = bCarry ? static_cast<uint32>(FMath::Max(0, GI->StoryCarry.Step)) : 0u;
	const ftcore::EncounterEntry* Entry = bCarry ? Table.findEntry(Scene, Beat, Step) : nullptr;
	const ftcore::EncounterLineup& Lineup = Entry ? Entry->lineup : Table.fallback;
	ftcore::makeEnemies(Roster, Lineup, CoreFoes);
	if (CoreFoes.empty()) // can't happen with a validated table; the demo fight is the safe answer
	{
		CoreFoes = ftcore::demo::enemies();
	}
	if (Entry)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: line-up of %d (tier %d) for %s beat %d step %d."), static_cast<int32>(CoreFoes.size()),
			Lineup.tier, *GI->StoryCarry.Scene, GI->StoryCarry.Beat, GI->StoryCarry.Step);
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: default line-up of %d (tier %d)%s."), static_cast<int32>(CoreFoes.size()), Lineup.tier,
			bCarry ? TEXT(", no entry for this step") : TEXT(""));
	}
}

void AFTBattleDemoActor::ApplyLineup()
{
	Battle->Party.Reset();
	for (const ftcore::Combatant& C : CoreParty)
	{
		Battle->Party.Add(FromCore(C));
	}
	Battle->Enemies.Reset();
	for (const ftcore::Combatant& C : CoreFoes)
	{
		Battle->Enemies.Add(FromCore(C));
	}
}

AFTPlayerController* AFTBattleDemoActor::GetPlayer() const
{
	return Cast<AFTPlayerController>(UGameplayStatics::GetPlayerController(this, 0));
}

void AFTBattleDemoActor::StartRound()
{
	Conductor->ResetClock(); // song time from ~0, so the beat grid starts with the round
	ApplyLineup(); // both sides at full HP: a retry starts from the state the fight began in
	if (!Battle->StartBattle())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("BattleDemo: the battle didn't start."));
		return;
	}
	RoundStartWall = Wall();
	RoundStartGame = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	++Attempt;
	bWaitingForRestart = false;
	bCanQuitToTitle = false;
	SelectedSkill = 0;
	SelectedTarget = -1;
	EnsureTarget();
	for (FPopup& P : Popups)
	{
		P.WallStart = -1.0;
	}
	Banner.Reset();
	Notice.Reset();
	CacheText();
	ResetFigures();

	if (AFTPlayerController* PC = GetPlayer())
	{
		if (!bInputBound)
		{
			bInputBound = true;
			PC->OnBattleArrow.AddDynamic(this, &AFTBattleDemoActor::HandleArrow);
			PC->OnBattleSkill.AddDynamic(this, &AFTBattleDemoActor::HandleSkill);
			PC->OnBattleTarget.AddDynamic(this, &AFTBattleDemoActor::HandleTarget);
			PC->OnBattleConfirm.AddDynamic(this, &AFTBattleDemoActor::HandleConfirm);
			// Story-context keys, live again once the battle input is off: fight again.
			PC->OnConfirm.AddDynamic(this, &AFTBattleDemoActor::HandleRestart);
			PC->OnAdvance.AddDynamic(this, &AFTBattleDemoActor::HandleRestart);
			// Esc / Backspace / pad B (Q is polled in Tick: it has no story-context binding).
			PC->OnCancel.AddDynamic(this, &AFTBattleDemoActor::HandleQuit);
			PC->SetViewTarget(this);
		}
		PC->BeginBattleInput();
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: no AFTPlayerController; there will be no battle input."));
	}
	if (Attempt > 1)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: battle started, attempt %d%s."), Attempt, bAutoPlay ? TEXT(" (autoplay)") : TEXT(""));
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: battle started%s."), bAutoPlay ? TEXT(" (autoplay)") : TEXT(""));
	}
}

// ---- figures ----

void AFTBattleDemoActor::BuildFigures()
{
	const int32 PartyCount = Battle->Party.Num();
	const int32 Count = PartyCount + Battle->Enemies.Num();
	Figures.SetNum(Count);
	for (int32 Id = 0; Id < Count; ++Id)
	{
		const bool bParty = Id < PartyCount;
		const int32 Slot = bParty ? Id : Id - PartyCount;
		FFigure& F = Figures[Id];
		F.Color = bParty ? kPartyColors[Slot % UE_ARRAY_COUNT(kPartyColors)] : kEnemyColors[Slot % UE_ARRAY_COUNT(kEnemyColors)];
		// Two staggered rows facing each other across the middle of the screen.
		// A third enemy gets a tighter stagger so the row stays in frame.
		const double EnemyStepX = Battle->Enemies.Num() > 2 ? 140.0 : 180.0;
		const double EnemyStepY = Battle->Enemies.Num() > 2 ? 95.0 : 130.0;
		F.Home = bParty ? FVector(-80.0 + 180.0 * Slot, -400.0 + 130.0 * Slot, 0.0) : FVector(-80.0 + EnemyStepX * Slot, 360.0 + EnemyStepY * Slot, 0.0);
		F.FallSign = bParty ? 1.f : -1.f;

		F.Pivot = NewObject<USceneComponent>(this);
		F.Pivot->SetupAttachment(Root);
		F.Pivot->SetRelativeLocation(F.Home);
		F.Pivot->RegisterComponent();
		FigureObjects.Add(F.Pivot);

		F.Material = FigureMaterial ? UMaterialInstanceDynamic::Create(FigureMaterial, this) : nullptr;
		if (F.Material)
		{
			F.Material->SetVectorParameterValue(TEXT("Color"), F.Color);
			FigureObjects.Add(F.Material);
		}

		auto MakePart = [&](UStaticMesh* Mesh, const FVector& At, const FVector& Scale)
		{
			UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
			Part->SetStaticMesh(Mesh);
			Part->SetupAttachment(F.Pivot);
			Part->SetRelativeLocation(At);
			Part->SetRelativeScale3D(Scale);
			Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			if (F.Material)
			{
				Part->SetMaterial(0, F.Material);
			}
			Part->RegisterComponent();
			FigureObjects.Add(Part);
			return Part;
		};

		USkeletalMesh* Skin = bParty && PartyMeshes.IsValidIndex(Slot) ? PartyMeshes[Slot].Get() : nullptr;
		if (Skin)
		{
			// The lead, turned to face the enemies (+Y), looping its idle clip.
			F.Height = 190.f;
			F.Skin = NewObject<USkeletalMeshComponent>(this);
			F.Skin->SetSkeletalMesh(Skin);
			F.Skin->SetupAttachment(F.Pivot);
			F.Skin->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Skin) + 90.0, 0.0));
			F.Skin->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			F.Skin->RegisterComponent();
			if (UAnimSequence* Idle = PartyIdles.IsValidIndex(Slot) ? PartyIdles[Slot].Get() : nullptr)
			{
				F.Skin->PlayAnimation(Idle, true);
			}
			FigureObjects.Add(F.Skin);
		}
		else if (bParty)
		{
			// A capsule stand-in: a cylinder body (meshes are 100 units, centred) with a sphere head.
			F.Height = 190.f;
			F.Body = MakePart(PartyBodyMesh, FVector(0, 0, 70), FVector(0.55, 0.55, 1.4));
			F.Head = MakePart(PartyHeadMesh, FVector(0, 0, 165), FVector(0.5, 0.5, 0.5));
		}
		else
		{
			const float Size = Slot == 0 ? 1.5f : 1.1f;
			F.Height = 100.f * Size;
			F.Body = MakePart(EnemyMesh, FVector(0, 0, 50.f * Size), FVector(Size));
			F.Head = nullptr;
		}
	}
}

void AFTBattleDemoActor::ResetFigures()
{
	for (FFigure& F : Figures)
	{
		F.HitWall = -100.0;
		F.LungeWall = -100.0;
		F.DownWall = -1.0;
		if (F.Pivot)
		{
			F.Pivot->SetRelativeLocationAndRotation(F.Home, FRotator::ZeroRotator);
			F.Pivot->SetRelativeScale3D(FVector::OneVector);
		}
	}
}

void AFTBattleDemoActor::TickFigures(double Now)
{
	static const FName ColorParam(TEXT("Color"));
	const ftcore::Battle* B = Battle->GetBattle();
	const bool bRunning = B && !Battle->IsBattleOver();
	const int32 ActorId = bRunning ? static_cast<int32>(B->actor()) : -1;
	for (int32 Id = 0; Id < Figures.Num(); ++Id)
	{
		FFigure& F = Figures[Id];
		if (!F.Pivot)
		{
			continue;
		}
		const float Flash = GetFlash(Id);
		const bool bParty = Id < Battle->Party.Num();

		FVector At = F.Home;
		// Lunge toward the other side when dealing a hit.
		const double Lunge = FMath::Clamp((Now - F.LungeWall) / kLungeSeconds, 0.0, 1.0);
		if (Lunge < 1.0)
		{
			At.Y += (bParty ? 1.0 : -1.0) * 80.0 * FMath::Sin(Lunge * PI);
		}
		// The one whose turn it is bobs.
		if (Id == ActorId && F.DownWall < 0.0)
		{
			At.Z += 12.0 * FMath::Abs(FMath::Sin((Now - RoundStartWall) * 5.0));
		}
		F.Pivot->SetRelativeLocation(At);

		// Squash on a hit.
		F.Pivot->SetRelativeScale3D(FVector(1.0 + 0.25 * Flash, 1.0 + 0.25 * Flash, 1.0 - 0.15 * Flash));

		FLinearColor Color = FLinearColor::LerpUsingHSV(F.Color, FLinearColor::White, Flash);
		if (F.DownWall >= 0.0)
		{
			// Topple sideways about the feet, away from the other side, and grey out.
			const double T = FMath::Clamp((Now - F.DownWall) / kFallSeconds, 0.0, 1.0);
			const double Ease = 1.0 - (1.0 - T) * (1.0 - T);
			F.Pivot->SetRelativeRotation(FRotator(0.0, 0.0, F.FallSign * 85.0 * Ease));
			Color = FLinearColor::LerpUsingHSV(Color, FLinearColor(0.15f, 0.15f, 0.15f), static_cast<float>(Ease) * 0.7f);
		}
		if (F.Material)
		{
			F.Material->SetVectorParameterValue(ColorParam, Color);
		}
	}
}

FVector AFTBattleDemoActor::GetFigureTop(int32 Id) const
{
	if (!Figures.IsValidIndex(Id) || !Figures[Id].Pivot)
	{
		return GetActorLocation();
	}
	return Figures[Id].Pivot->GetComponentTransform().TransformPosition(FVector(0, 0, Figures[Id].Height + 25.f));
}

float AFTBattleDemoActor::GetFlash(int32 Id) const
{
	if (!Figures.IsValidIndex(Id))
	{
		return 0.f;
	}
	return static_cast<float>(FMath::Clamp(1.0 - (Wall() - Figures[Id].HitWall) / kFlashSeconds, 0.0, 1.0));
}

double AFTBattleDemoActor::GetLanePressAge(int32 Lane) const
{
	return Lane >= 0 && Lane < 4 ? Wall() - LanePressWall[Lane] : 100.0;
}

// ---- text, built when it changes ----

void AFTBattleDemoActor::CacheText()
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (!B)
	{
		return;
	}
	const int32 Count = static_cast<int32>(B->combatants().size());
	Names.SetNum(Count);
	HpTexts.SetNum(Count);
	HpShown.Init(-1, Count);
	SkillLabels.SetNum(Count);
	SkillCosts.SetNum(Count);
	for (int32 Id = 0; Id < Count; ++Id)
	{
		const ftcore::Combatant& C = B->combatant(static_cast<ftcore::CombatantId>(Id));
		Names[Id] = FT::ToFString(C.name);
		SkillLabels[Id].SetNum(static_cast<int32>(C.skills.size()));
		SkillCosts[Id].SetNum(static_cast<int32>(C.skills.size()));
		for (int32 S = 0; S < SkillLabels[Id].Num(); ++S)
		{
			const ftcore::Skill& Skill = C.skills[static_cast<size_t>(S)];
			SkillLabels[Id][S] = FString::Printf(TEXT("%d  %s%s"), S + 1, *FT::ToFString(Skill.name),
				Skill.targeting == ftcore::Targeting::AllEnemies ? TEXT(" (all)") : TEXT(""));
			SkillCosts[Id][S] = Skill.apCost > 0 ? FString::Printf(TEXT("%d AP"), Skill.apCost)
				: Skill.apGain > 0 ? FString::Printf(TEXT("+%d AP"), Skill.apGain) : FString();
		}
		UpdateHpText(Id);
	}
}

void AFTBattleDemoActor::UpdateHpText(int32 Id)
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (!B || !HpTexts.IsValidIndex(Id))
	{
		return;
	}
	const ftcore::Combatant& C = B->combatant(static_cast<ftcore::CombatantId>(Id));
	if (HpShown[Id] != C.hp)
	{
		HpShown[Id] = C.hp;
		HpTexts[Id] = FString::Printf(TEXT("%d / %d"), C.hp, C.stats.maxHp);
	}
}

const FString& AFTBattleDemoActor::GetFighterName(int32 Id) const
{
	return Names.IsValidIndex(Id) ? Names[Id] : kEmpty;
}

const FString& AFTBattleDemoActor::GetSkillLabel(int32 Id, int32 Skill) const
{
	return SkillLabels.IsValidIndex(Id) && SkillLabels[Id].IsValidIndex(Skill) ? SkillLabels[Id][Skill] : kEmpty;
}

const FString& AFTBattleDemoActor::GetSkillCost(int32 Id, int32 Skill) const
{
	return SkillCosts.IsValidIndex(Id) && SkillCosts[Id].IsValidIndex(Skill) ? SkillCosts[Id][Skill] : kEmpty;
}

const FString& AFTBattleDemoActor::GetHpText(int32 Id) const
{
	return HpTexts.IsValidIndex(Id) ? HpTexts[Id] : kEmpty;
}

double AFTBattleDemoActor::GetBannerAge() const { return Wall() - BannerWall; }
double AFTBattleDemoActor::GetNoticeAge() const { return Wall() - NoticeWall; }

void AFTBattleDemoActor::SetBanner(const FString& Text)
{
	Banner = Text;
	BannerWall = Wall();
}

void AFTBattleDemoActor::SetNotice(const FString& Text)
{
	Notice = Text;
	NoticeWall = Wall();
}

void AFTBattleDemoActor::AddPopup(const FPopup& P)
{
	FPopup& Slot = Popups[NextPopup];
	NextPopup = (NextPopup + 1) % MaxPopups;
	Slot.WallStart = Wall();
	Slot.Lane = P.Lane;
	Slot.Combatant = P.Combatant;
	Slot.Grade = P.Grade;
	Slot.bWrong = P.bWrong;
	Slot.bNumber = P.bNumber;
	Slot.bBlock = P.bBlock;
	Slot.Text = P.Text;
}

// ---- battle events ----

void AFTBattleDemoActor::HandleBattleEvent(const FFTBattleEvent& Event)
{
	const ftcore::Battle* B = Battle->GetBattle();
	const double Now = Wall();
	switch (Event.Type)
	{
	case EFTBattleEventType::SkillStarted:
	{
		const bool bEnemy = Battle->GetCombatant(Event.Source).Team == EFTTeam::Enemies;
		SetBanner(bEnemy ? FString::Printf(TEXT("%s: %s - defend!"), *GetFighterName(Event.Source), *Event.Label)
		                 : FString::Printf(TEXT("%s: %s"), *GetFighterName(Event.Source), *Event.Label));
		break;
	}
	case EFTBattleEventType::NoteJudged:
	{
		FPopup P;
		P.Grade = Event.Grade;
		P.bWrong = Event.Label == TEXT("WRONG");
		if (B && static_cast<size_t>(Event.Beat) < B->combo().size())
		{
			P.Lane = static_cast<int32>(B->combo().note(static_cast<size_t>(Event.Beat)).arrow);
		}
		AddPopup(P);
		break;
	}
	case EFTBattleEventType::Hit:
	{
		if (Figures.IsValidIndex(Event.Target))
		{
			Figures[Event.Target].HitWall = Now;
		}
		if (Figures.IsValidIndex(Event.Source))
		{
			Figures[Event.Source].LungeWall = Now;
		}
		UpdateHpText(Event.Target);
		FPopup P;
		P.bNumber = true;
		P.Combatant = Event.Target;
		P.Text = FString::Printf(TEXT("-%d"), Event.Amount);
		AddPopup(P);
		break;
	}
	case EFTBattleEventType::Blocked:
	{
		FPopup P;
		P.bNumber = true;
		P.bBlock = true;
		P.Combatant = Event.Target;
		P.Text = FString::Printf(TEXT("BLOCK %d"), Event.Amount);
		AddPopup(P);
		break;
	}
	case EFTBattleEventType::ComboFinished:
		SetBanner(Event.Label.IsEmpty() ? FString::Printf(TEXT("Accuracy %d%%"), Event.Amount)
		                                : FString::Printf(TEXT("FULL COMBO!  %d%%"), Event.Amount));
		break;
	case EFTBattleEventType::Counter:
		SetBanner(FString::Printf(TEXT("%s counters!"), *GetFighterName(Event.Source)));
		break;
	case EFTBattleEventType::Downed:
		if (Figures.IsValidIndex(Event.Target))
		{
			Figures[Event.Target].DownWall = Now;
		}
		SetBanner(FString::Printf(TEXT("%s is down!"), *GetFighterName(Event.Target)));
		break;
	default:
		break;
	}
}

void AFTBattleDemoActor::HandleTurnStarted(int32 /*Actor*/)
{
	SelectedSkill = 0;
	EnsureTarget();
}

void AFTBattleDemoActor::HandleOutcome(EFTBattleOutcome Outcome)
{
	bWaitingForRestart = true;
	OutcomeWall = Wall();
	if (AFTPlayerController* PC = GetPlayer())
	{
		PC->EndBattleInput();
	}
	const bool bWon = Outcome == EFTBattleOutcome::Victory;
	const double Seconds = OutcomeWall - RoundStartWall;
	const double GameSeconds = (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0) - RoundStartGame;
	UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: %s after %.1f s (game %.1f s)."),
		bWon ? TEXT("VICTORY") : TEXT("DEFEAT"), Seconds, GameSeconds);
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->ExploreCarry.bValid)
	{
		// Started from the exploration demo: a win goes back there; a loss stays for a retry.
		if (bWon)
		{
			GI->ExploreCarry.bWon = true;
			bReturningToStory = true; // (the name covers any return: no restart while leaving)
			SetNotice(TEXT("Returning to the field"));
			FTimerHandle Timer;
			GetWorldTimerManager().SetTimer(Timer, this, &AFTBattleDemoActor::ReturnToField, 2.5f, false);
		}
		else if (bAutoPlay)
		{
			FTimerHandle Timer;
			GetWorldTimerManager().SetTimer(Timer, this, &AFTBattleDemoActor::RetryAfterDefeat, 1.2f, false);
		}
		return;
	}
	if (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone)
	{
		// Handed here by the story: a win goes back to it; a loss offers Retry and Quit to title (the
		// autopilot always retries, so a headless run never stops on a defeat).
		if (bWon)
		{
			GI->StoryCarry.bDone = true;
			GI->StoryCarry.bWon = true;
			bReturningToStory = true;
			SetNotice(TEXT("Returning to the story"));
			FTimerHandle Timer;
			GetWorldTimerManager().SetTimer(Timer, this, &AFTBattleDemoActor::ReturnToStory, 2.5f, false);
		}
		else if (bAutoPlay)
		{
			FTimerHandle Timer;
			GetWorldTimerManager().SetTimer(Timer, this, &AFTBattleDemoActor::RetryAfterDefeat, 1.2f, false);
		}
		else
		{
			bCanQuitToTitle = true;
		}
		return;
	}
	if (bQuitOnOutcome)
	{
		FPlatformMisc::RequestExit(false, TEXT("BattleDemo"));
	}
}

void AFTBattleDemoActor::ReturnToStory()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: returning to the story on %s."), *GI->StoryCarry.ReturnMap);
		UGameplayStatics::OpenLevel(this, FName(*GI->StoryCarry.ReturnMap));
	}
}

void AFTBattleDemoActor::RetryAfterDefeat()
{
	if (bWaitingForRestart && !bReturningToStory && !bQuitting)
	{
		StartRound();
	}
}

void AFTBattleDemoActor::HandleQuit()
{
	// Same short pause as a restart, so the press that ended the battle can't quit.
	if (!bWaitingForRestart || !bCanQuitToTitle || bQuitting || Wall() - OutcomeWall <= 1.0)
	{
		return;
	}
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI || !GI->StoryCarry.bValid || GI->StoryCarry.ReturnMap.IsEmpty())
	{
		return;
	}
	bQuitting = true;
	// The carry stays unfinished (bDone false); the story map reads ?Title and shows the title screen.
	UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: quitting to the title via %s."), *GI->StoryCarry.ReturnMap);
	UGameplayStatics::OpenLevel(this, FName(*GI->StoryCarry.ReturnMap), true, TEXT("Title"));
}

void AFTBattleDemoActor::ReturnToField()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->ExploreCarry.bValid)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: returning to %s."), *GI->ExploreCarry.ReturnMap);
		UGameplayStatics::OpenLevel(this, FName(*GI->ExploreCarry.ReturnMap));
	}
}

// ---- input ----

void AFTBattleDemoActor::HandleArrow(EFTArrow Arrow, bool bFaceButton)
{
	if (bAutoPlay || bWaitingForRestart || !Battle->GetBattle())
	{
		return;
	}
	if (Battle->IsComboActive())
	{
		LanePressWall[static_cast<int32>(Arrow)] = Wall();
		Battle->Press(Arrow);
		return;
	}
	if (Battle->GetPhase() != EFTBattlePhase::AwaitingCommand)
	{
		return;
	}
	if (bFaceButton)
	{
		if (Arrow == EFTArrow::Down) // pad A
		{
			UseSelected();
		}
		return;
	}
	const int32 Count = Battle->GetSkillCount(Battle->GetActor());
	switch (Arrow)
	{
	case EFTArrow::Up:
		SelectedSkill = Count > 0 ? (SelectedSkill + Count - 1) % Count : 0;
		break;
	case EFTArrow::Down:
		SelectedSkill = Count > 0 ? (SelectedSkill + 1) % Count : 0;
		break;
	case EFTArrow::Left:
		CycleTarget(-1);
		break;
	case EFTArrow::Right:
		CycleTarget(1);
		break;
	}
}

void AFTBattleDemoActor::HandleSkill(int32 Skill)
{
	if (bAutoPlay || bWaitingForRestart || Battle->GetPhase() != EFTBattlePhase::AwaitingCommand)
	{
		return;
	}
	if (Skill >= Battle->GetSkillCount(Battle->GetActor()))
	{
		return;
	}
	SelectedSkill = Skill;
	UseSelected();
}

void AFTBattleDemoActor::HandleTarget(int32 Step)
{
	if (!bAutoPlay && !bWaitingForRestart && Battle->GetPhase() == EFTBattlePhase::AwaitingCommand)
	{
		CycleTarget(Step);
	}
}

void AFTBattleDemoActor::HandleConfirm()
{
	if (!bAutoPlay && !bWaitingForRestart && Battle->GetPhase() == EFTBattlePhase::AwaitingCommand)
	{
		UseSelected();
	}
}

void AFTBattleDemoActor::HandleRestart()
{
	// A short pause first, so the press that ended the battle doesn't start the next one.
	if (bWaitingForRestart && !bReturningToStory && !bQuitting && Wall() - OutcomeWall > 1.0)
	{
		StartRound();
	}
}

void AFTBattleDemoActor::UseSelected()
{
	EnsureTarget();
	const EFTCommandError Err = Battle->ChooseSkill(SelectedSkill, SelectedTarget);
	switch (Err)
	{
	case EFTCommandError::NotEnoughAp:
		SetNotice(TEXT("Not enough AP"));
		break;
	case EFTCommandError::InvalidTarget:
		SetNotice(TEXT("Pick a target"));
		break;
	default:
		break;
	}
}

void AFTBattleDemoActor::EnsureTarget()
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (!B)
	{
		return;
	}
	const std::vector<ftcore::Combatant>& All = B->combatants();
	const bool bValid = SelectedTarget >= 0 && static_cast<size_t>(SelectedTarget) < All.size()
		&& All[static_cast<size_t>(SelectedTarget)].team == ftcore::Team::Enemies && All[static_cast<size_t>(SelectedTarget)].alive();
	if (!bValid)
	{
		SelectedTarget = -1;
		CycleTarget(1);
	}
}

void AFTBattleDemoActor::CycleTarget(int32 Step)
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (!B)
	{
		return;
	}
	const int32 Count = static_cast<int32>(B->combatants().size());
	int32 At = SelectedTarget < 0 ? (Step > 0 ? -1 : 0) : SelectedTarget;
	for (int32 I = 0; I < Count; ++I)
	{
		At = (At + (Step >= 0 ? 1 : -1) + Count) % Count;
		const ftcore::Combatant& C = B->combatant(static_cast<ftcore::CombatantId>(At));
		if (C.team == ftcore::Team::Enemies && C.alive())
		{
			SelectedTarget = At;
			return;
		}
	}
}

// ---- per frame ----

void AFTBattleDemoActor::TickAutoPlay()
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (!B || Battle->IsBattleOver())
	{
		return;
	}
	if (B->phase() == ftcore::Phase::AwaitingCommand)
	{
		const ftcore::demo::PilotChoice Choice = ftcore::demo::pilotChoice(*B);
		SelectedSkill = static_cast<int32>(Choice.skill);
		SelectedTarget = static_cast<int32>(Choice.target);
		UseSelected();
		return;
	}
	if (!Battle->IsComboActive())
	{
		return;
	}
	const double Now = Battle->GetBattleTime();
	const ftcore::ComboSequence& Combo = B->combo();
	for (size_t I = Combo.nextIndex(); I < Combo.size() && Combo.note(I).time <= Now; ++I)
	{
		if (Combo.grade(I) == ftcore::Grade::Pending)
		{
			LanePressWall[static_cast<int32>(Combo.note(I).arrow)] = Wall();
			// At the note's own time, not "now": a long (dilated) frame must not turn PERFECT into late.
			Battle->PressAtTime(FT::FromCore(Combo.note(I).arrow), Combo.note(I).time);
		}
		if (!Battle->IsComboActive())
		{
			break;
		}
	}
}

void AFTBattleDemoActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bAutoPlay)
	{
		TickAutoPlay();
	}
	if (bCanQuitToTitle && bWaitingForRestart)
	{
		// Q quits on its press edge. It is polled: after the battle the story keys are live, and Q has
		// no binding there.
		const AFTPlayerController* PC = GetPlayer();
		const bool bDown = PC && PC->IsInputKeyDown(EKeys::Q);
		if (bDown && !bQKeyDown)
		{
			HandleQuit();
		}
		bQKeyDown = bDown;
	}
	else
	{
		bQKeyDown = false;
	}
	TickFigures(Wall());
}
