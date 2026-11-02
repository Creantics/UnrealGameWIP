#include "FTBattleDemoActor.h"

#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

#include "ForgottenThird.h"
#include "FTAudioSubsystem.h"
#include "FTConductorComponent.h"
#include "FTGameInstance.h"
#include "FTMeshFacing.h"
#include "FTPauseSubsystem.h"
#include "FTPlayerController.h"
#include "FTSettingsSubsystem.h"
#include "FTStepHost.h"
#include "FTStorySubsystem.h"
#include "FTText.h"
#include "FTUiText.h"

#include "ftcore/BattleDemo.h"

#include <optional>
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

// Data/Battle/weakpoints.json; false (and logged) when it can't be read or is refused, and the
// enemies then have no weak points (an aimed shot still hits the body).
bool LoadWeakPointData(const ftcore::EnemyRoster& Roster, ftcore::WeakPointTable& Table)
{
	const FString Path = FPaths::ProjectDir() / TEXT("Data/Battle/weakpoints.json");
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: %s not found; enemies have no weak points."), *Path);
		return false;
	}
	std::string Error;
	if (!ftcore::importWeakPointTable(FT::BytesAsText(Bytes), Roster, Table, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: weakpoints.json refused (%s); enemies have no weak points."), UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	return true;
}

// Data/Battle/arrow_lane.json: the arrow lane's layout, timings and colours. Missing or refused (logged), the
// built-in numbers stay, which are the same as the file's.
void LoadLaneLayout(ftcore::ArrowLaneLayout& Layout)
{
	const FString Path = FPaths::ProjectDir() / TEXT("Data/Battle/arrow_lane.json");
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: %s not found; the arrow lane uses its built-in layout."), *Path);
		return;
	}
	std::string Error;
	if (!ftcore::importArrowLaneLayout(FT::BytesAsText(Bytes), Layout, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: arrow_lane.json refused (%s); the arrow lane uses its built-in layout."),
			UTF8_TO_TCHAR(Error.c_str()));
	}
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
	Out.Stats.MaxPoise = C.stats.maxPoise;
	Out.Verbs = C.verbs;
	Out.bChild = C.child;
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
		D.bTorn = S.torn;
		D.TellId = FT::ToFString(S.tellId);
		// The default timeline stays implicit (an empty Hits array means the same single hit).
		if (!ftcore::sameTimeline(S.timeline, ftcore::AttackTimeline()))
		{
			for (size_t I = 0; I < S.timeline.size(); ++I)
			{
				const ftcore::TimelineHit& Hit = S.timeline[I];
				FFTAttackHit& H = D.Hits.AddDefaulted_GetRef();
				H.At = Hit.at;
				H.Tell = Hit.tell;
				H.Telegraph = Hit.telegraph;
				H.Weight = Hit.weight;
				H.bJumpable = Hit.jumpable;
			}
		}
	}
	for (const ftcore::WeakPoint& P : C.weakPoints)
	{
		FFTWeakPoint& W = Out.WeakPoints.AddDefaulted_GetRef();
		W.Id = UTF8_TO_TCHAR(P.idChars.data());
		W.X = P.x;
		W.Y = P.y;
		W.Radius = P.radius;
		W.Multiplier = P.multiplier;
	}
	Out.HalfWidth = C.weakPoints.halfWidth;
	for (size_t I = 0; I < C.phases.size(); ++I)
	{
		const ftcore::EnemyPhase& P = C.phases[I];
		FFTEnemyPhase& D = Out.Phases.AddDefaulted_GetRef();
		D.Label = FT::ToFString(P.label());
		D.HpBelow = P.hpBelow;
		for (uint8 Step = 0; Step < P.length; ++Step)
		{
			D.Pattern.Add(P.pattern[Step]);
		}
		D.LoopFrom = P.loopFrom;
		D.AttackPercent = P.attackPercent;
		D.HealPercent = P.healPercent;
	}
	return Out;
}

// Figure colours: party members first, then enemies.
const FLinearColor kPartyColors[] = {FLinearColor(0.85f, 0.22f, 0.28f), FLinearColor(0.25f, 0.45f, 0.85f), FLinearColor(0.92f, 0.74f, 0.28f)};
const FLinearColor kEnemyColors[] = {FLinearColor(0.48f, 0.33f, 0.25f), FLinearColor(0.22f, 0.42f, 0.44f), FLinearColor(0.38f, 0.3f, 0.42f),
	FLinearColor(0.42f, 0.4f, 0.22f), FLinearColor(0.44f, 0.24f, 0.3f)};

constexpr double kFlashSeconds = 0.35;
constexpr double kLungeSeconds = 0.3;
constexpr double kFallSeconds = 0.6;
constexpr double kBannerSeconds = 1.6;
// The autopilot answers enemy hits whose ideal press time falls before the next battle update:
// this many frames ahead (clamped), so a long dilated frame never steps over a window.
constexpr double kPilotFrames = 2.0;
constexpr double kPilotMinLookahead = 0.05;
constexpr double kPilotMaxLookahead = 0.5;
// The autopilot holds its aim this long (battle seconds) before it fires, so the shot can be seen.
constexpr double kPilotAimSeconds = 0.6;
// The attacker lights up for this long as each hit's tell starts.
constexpr double kTellFlashSeconds = 0.15;
// Reticle speeds in figure heights per second: a stick at full tilt, a held arrow key.
constexpr double kStickAimSpeed = 0.8;
constexpr double kKeyAimSpeed = 0.6;
constexpr float kStickDeadZone = 0.2f;
// Screen height of the target's figure when it can't be projected (mouse pixels per figure height).
constexpr float kFallbackFigurePixels = 300.f;

const FString kEmpty;

// The pause-aware wall clock: banner, popup, judgement and press ages, and the second the results hold
// before a press counts, all stand still while the pause menu is open. AFTBattleHUD's HudWall() reads
// the same clock, since it ages the popups against the times stamped here.
double Wall() { return UFTPauseSubsystem::WallSeconds(); }
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
	Battle->Tuning.Recovery = Tuning.recovery;
	Battle->Tuning.Lockout = Tuning.lockout;
	Battle->Tuning.StartingAp = Tuning.startingAp;

	// The two lines face each other along +Y, the party at -Y. The camera starts behind the party; once play begins
	// ftcore::BattleCamera places it every frame (TickCamera).
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Root);
	Camera->SetRelativeLocation(FVector(-10.0, -750.0, 310.0));
	Camera->SetRelativeRotation(FRotator(-8.0, 90.0, 0.0));
	Camera->SetFieldOfView(56.f);

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
	bIdlePilot = FParse::Param(FCommandLine::Get(), TEXT("BattleIdle"));
	bAutoPlay = bAutoPlay || bIdlePilot;
	if (FParse::Value(FCommandLine::Get(), TEXT("BattleShot="), ShotPrefix, false) && !ShotPrefix.IsEmpty())
	{
		bAutoPlay = true;
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: -BattleShot=%s: five views, then quit."), *ShotPrefix);
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("BattleCamShot="), CamShotPrefix, false) && !CamShotPrefix.IsEmpty())
	{
		bAutoPlay = true;
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: -BattleCamShot=%s: the camera's views, then quit."), *CamShotPrefix);
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("BattleLaneShot="), LaneShotPrefix, false) && !LaneShotPrefix.IsEmpty())
	{
		bAutoPlay = true;
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: -BattleLaneShot=%s: the arrow lane's views, then quit."), *LaneShotPrefix);
	}
	LoadLaneLayout(LaneLayout);
	StreakTexts.Reserve(65);
	for (int32 N = 0; N <= 64; ++N)
	{
		StreakTexts.Add(FString::Printf(TEXT("x%d"), N));
	}
	if (const UFTGameInstance* SettingsGI = GetGameInstance<UFTGameInstance>())
	{
		if (const UFTSettingsSubsystem* SettingsSub = SettingsGI->GetSubsystem<UFTSettingsSubsystem>())
		{
			bReduceFlashes = SettingsSub->Get().reduceFlashes;
			bStickFlicks = SettingsSub->Get().stickFlicks;
		}
	}
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

	// The beat grid follows the story's music when a cue with a tempo is playing (it carries over the
	// hand-off). Not under -AlphaFast: the music isn't dilated, so it would run 8x slow against the game.
	// Otherwise, and always under -nosound, the grid stays on kBpm and the silent clock.
	if (!bDilated)
	{
		const UFTAudioSubsystem* Audio = GetGameInstance() ? GetGameInstance()->GetSubsystem<UFTAudioSubsystem>() : nullptr;
		const ftcore::AudioCue* Cue = Audio ? Audio->GetMusicCue() : nullptr;
		if (Cue && Cue->bpm > 0.f && Audio->GetMusicComponent())
		{
			Conductor->BindMusic(Audio->GetMusicComponent(), *Cue, Audio->GetMusicCursor());
			UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: beat grid from music cue %s (%.2f bpm)."),
				*Audio->GetCurrentMusic(), Conductor->Bpm);
		}
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
	// A story fight puts the party up at its level; other fights (the demo, the field) at level 1.
	const UFTStorySubsystem* Story = GetGameInstance() ? GetGameInstance()->GetSubsystem<UFTStorySubsystem>() : nullptr;
	const UFTGameInstance* StoryGI = GetGameInstance<UFTGameInstance>();
	const bool bStoryFight = Story && StoryGI && StoryGI->StoryCarry.bValid;
	const int32 Level = bStoryFight ? Story->GetPartyLevel() : 1;
	CoreParty = ftcore::demo::party(Level);
	CoreFoes = ftcore::demo::enemies();
	Reward = ftcore::Reward();

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
	FString Wanted;
	if (!bCarry && FParse::Value(FCommandLine::Get(), TEXT("BattleEncounter="), Wanted, false))
	{
		TArray<FString> Parts;
		Wanted.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 3)
		{
			Entry = Table.findEntry(FT::ToUtf8(Parts[0]), static_cast<uint32>(FMath::Max(0, FCString::Atoi(*Parts[1]))),
				static_cast<uint32>(FMath::Max(0, FCString::Atoi(*Parts[2]))));
		}
		if (Entry)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: -BattleEncounter=%s: that entry's line-up (no story carry, so a win pays nothing)."), *Wanted);
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: -BattleEncounter=%s names no entry (scene,beat,step); the default line-up."), *Wanted);
		}
	}
	// -BattleLineup=<id>: a demo line-up from encounters.json "demos" (the Turns demo is "turns").
	const ftcore::EncounterLineup* Demo = nullptr;
	FString DemoId;
	if (!bCarry && !Entry && FParse::Value(FCommandLine::Get(), TEXT("BattleLineup="), DemoId, false))
	{
		Demo = Table.findDemo(FT::ToUtf8(DemoId));
		if (!Demo)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: -BattleLineup=%s names no demo line-up; the default line-up."), *DemoId);
		}
	}
	bTurnsDemo = Demo && DemoId == TEXT("turns");
	PilotSkill = ftcore::demo::PilotSkill();
	PilotSkill.showTorn = bTurnsDemo;
	const ftcore::EncounterLineup& Lineup = Entry ? Entry->lineup : Demo ? *Demo : Table.fallback;
	ftcore::makeEnemies(Roster, Lineup, CoreFoes);
	if (bStoryFight)
	{
		Reward = ftcore::encounterReward(Roster, Lineup);
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: party level %d; a win pays %d xp, %d gold."), Level, Reward.xp, Reward.gold);
	}
	if (CoreFoes.empty()) // can't happen with a validated table; the demo fight is the safe answer
	{
		CoreFoes = ftcore::demo::enemies();
	}
	else
	{
		ftcore::WeakPointTable Points;
		if (LoadWeakPointData(Roster, Points))
		{
			ftcore::applyWeakPoints(Points, Roster, Lineup, CoreFoes);
		}
	}
	if (Demo)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: demo line-up %s of %d (tier %d)."), *DemoId, static_cast<int32>(CoreFoes.size()), Lineup.tier);
	}
	else if (Entry && !bCarry)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: line-up of %d (tier %d)."), static_cast<int32>(CoreFoes.size()), Lineup.tier);
	}
	else if (Entry)
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
	Judgement = EFTJudgement::None;
	JudgementWall = -100.0;
	PilotWeakPoint = ftcore::kMaxWeakPoints;
	PilotState = ftcore::demo::PilotState();
	GuardHolds = 0;
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
	PhaseBanner.Reset();
	CountBeat = 0;
	ResultLineCount = 0;
	ContinuePrompt.Reset();
	CacheText();
	RefreshNextSkills();
	ResetFigures();
	CameraDirector.begin(CameraFigures);
	CameraWall = -1.0;

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
			// Backspace / pad B (Q is polled in Tick: it has no story-context binding). Esc and Start are the
			// pause keys: while aiming they put the shot back first (OnBack).
			PC->OnCancel.AddDynamic(this, &AFTBattleDemoActor::HandleQuit);
			PC->OnBack.AddDynamic(this, &AFTBattleDemoActor::HandleBack);
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
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: battle started, attempt %d%s."), Attempt, bIdlePilot ? TEXT(" (idle pilot)") : bAutoPlay ? TEXT(" (autoplay)") : TEXT(""));
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: battle started%s."), bIdlePilot ? TEXT(" (idle pilot)") : bAutoPlay ? TEXT(" (autoplay)") : TEXT(""));
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
		// Two staggered rows facing each other along +Y, the party at -Y; the camera looks over the party at the
		// enemies. Where they stand is Core's (ftcore::stageHome), which the camera tests use too.
		const ftcore::CamVec Home = ftcore::stageHome(bParty, static_cast<size_t>(Slot), static_cast<size_t>(Battle->Enemies.Num()));
		F.Home = FVector(Home.x, Home.y, Home.z);
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

	// The same figures for the camera director, at rest, in world space: a lead is about 90 cm across, an enemy a cube.
	CameraFigures.assign(static_cast<size_t>(Count), ftcore::CameraFigure());
	for (int32 Id = 0; Id < Count; ++Id)
	{
		const FFigure& F = Figures[Id];
		const FVector At = GetActorLocation() + F.Home;
		ftcore::CameraFigure& C = CameraFigures[static_cast<size_t>(Id)];
		C.pos = {At.X, At.Y, At.Z};
		C.party = Id < PartyCount;
		C.height = F.Height;
		C.radius = C.party ? 45.0 : F.Height * 0.55;
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
	// The attacker lights up as the tell of its next hit starts (the HUD's ring starts closing then).
	float TellFlash = 0.f;
	if (bRunning && Battle->IsDefenceActive())
	{
		const ftcore::DefenceTimeline& T = B->defence();
		const size_t I = T.nextIndex();
		if (I < T.size())
		{
			const double SinceTell = Battle->GetBattleTime() - (T.impact(I) - T.hit(I).tell);
			if (SinceTell >= 0.0)
			{
				TellFlash = static_cast<float>(FMath::Clamp(1.0 - SinceTell / kTellFlashSeconds, 0.0, 1.0));
			}
		}
	}
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

		const float Light = Id == ActorId ? FMath::Max(Flash, TellFlash) : Flash;
		FLinearColor Color = FLinearColor::LerpUsingHSV(F.Color, FLinearColor::White, Light);
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

FVector AFTBattleDemoActor::GetFigurePoint(int32 Id, float X, float Y) const
{
	if (!Figures.IsValidIndex(Id) || !Figures[Id].Pivot)
	{
		return GetActorLocation();
	}
	// Across is the camera's own right, flat: the weak points and the reticle are laid out for the view the player has.
	const ftcore::BattleCameraPose& View = CameraDirector.pose();
	FVector Right(-(View.look.y - View.eye.y), View.look.x - View.eye.x, 0.0);
	Right = Right.GetSafeNormal();
	if (Right.IsNearlyZero())
	{
		Right = FVector(1.0, 0.0, 0.0);
	}
	const float H = Figures[Id].Height;
	return Figures[Id].Pivot->GetComponentLocation() + Right * (X * H) + FVector(0.0, 0.0, Y * H);
}

float AFTBattleDemoActor::GetFigureHeight(int32 Id) const
{
	return Figures.IsValidIndex(Id) ? Figures[Id].Height : 0.f;
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

double AFTBattleDemoActor::GetComboAge() const
{
	return ComboStartWall < 0.0 ? 100.0 : Wall() - ComboStartWall;
}

double AFTBattleDemoActor::GetComboEndAge() const
{
	return ComboEndWall < 0.0 ? -1.0 : Wall() - ComboEndWall;
}

const FString& AFTBattleDemoActor::GetStreakText(int32 N) const
{
	static const FString Empty;
	return StreakTexts.Num() > 0 ? StreakTexts[FMath::Clamp(N, 0, StreakTexts.Num() - 1)] : Empty;
}

double AFTBattleDemoActor::GetBrokenStreakAge() const
{
	return Wall() - BrokenStreakWall;
}

double AFTBattleDemoActor::GetStreakStepAge() const
{
	return Wall() - StreakStepWall;
}

double AFTBattleDemoActor::GetStrayAge() const
{
	return Wall() - StrayWall;
}

double AFTBattleDemoActor::GetDefencePressAge() const
{
	return Wall() - DefencePressWall;
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
	SkillNames.SetNum(Count);
	for (int32 Id = 0; Id < Count; ++Id)
	{
		const ftcore::Combatant& C = B->combatant(static_cast<ftcore::CombatantId>(Id));
		Names[Id] = FT::ToFString(C.name);
		SkillLabels[Id].SetNum(static_cast<int32>(C.skills.size()));
		SkillCosts[Id].SetNum(static_cast<int32>(C.skills.size()));
		SkillNames[Id].SetNum(static_cast<int32>(C.skills.size()));
		for (int32 S = 0; S < SkillLabels[Id].Num(); ++S)
		{
			const ftcore::Skill& Skill = C.skills[static_cast<size_t>(S)];
			SkillNames[Id][S] = FT::ToFString(Skill.name);
			SkillLabels[Id][S] = FString::Printf(TEXT("%d  %s%s"), S + 1, *FT::ToFString(Skill.name),
				Skill.targeting == ftcore::Targeting::AllEnemies ? TEXT(" (all)") : TEXT(""));
			SkillCosts[Id][S] = Skill.apCost > 0 ? FString::Printf(TEXT("%d AP"), Skill.apCost)
				: Skill.apGain > 0 ? FString::Printf(TEXT("+%d AP"), Skill.apGain) : FString();
		}
		UpdateHpText(Id);
	}
	AimLabel = TEXT("Aim");
	AimCost = FString::Printf(TEXT("%d AP"), Battle->Tuning.AimApCost);
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

const FString& AFTBattleDemoActor::GetSkillName(int32 Id, int32 Skill) const
{
	return SkillNames.IsValidIndex(Id) && SkillNames[Id].IsValidIndex(Skill) ? SkillNames[Id][Skill] : kEmpty;
}

const FString& AFTBattleDemoActor::GetSkillCost(int32 Id, int32 Skill) const
{
	return SkillCosts.IsValidIndex(Id) && SkillCosts[Id].IsValidIndex(Skill) ? SkillCosts[Id][Skill] : kEmpty;
}

const FString& AFTBattleDemoActor::GetHpText(int32 Id) const
{
	return HpTexts.IsValidIndex(Id) ? HpTexts[Id] : kEmpty;
}

int32 AFTBattleDemoActor::GetAimRow() const
{
	const int32 Actor = Battle->GetActor();
	return Actor >= 0 ? Battle->GetSkillCount(Actor) : 0;
}

const FString& AFTBattleDemoActor::GetWeakPointLabel(int32 I) const
{
	return I >= 0 && I < static_cast<int32>(ftcore::kMaxWeakPoints) ? WeakPointLabels[I] : kEmpty;
}

double AFTBattleDemoActor::GetJudgementAge() const { return Wall() - JudgementWall; }

void AFTBattleDemoActor::SetJudgement(EFTJudgement Kind, const FString& Text)
{
	Judgement = Kind;
	JudgementText = Text;
	JudgementWall = Wall();
}

double AFTBattleDemoActor::GetBannerAge() const { return Wall() - BannerWall; }
double AFTBattleDemoActor::GetNoticeAge() const { return Wall() - NoticeWall; }
double AFTBattleDemoActor::GetPhaseBannerAge() const { return Wall() - PhaseBannerWall; }
double AFTBattleDemoActor::GetCountAge() const { return Wall() - CountWall; }

const FString& AFTBattleDemoActor::GetNextSkillName(int32 Id) const
{
	static const FString None;
	if (!NextSkill.IsValidIndex(Id) || !SkillNames.IsValidIndex(Id) || !SkillNames[Id].IsValidIndex(NextSkill[Id]))
	{
		return None;
	}
	return SkillNames[Id][NextSkill[Id]];
}

void AFTBattleDemoActor::RefreshNextSkills()
{
	const int32 Count = Battle->GetCombatantCount();
	NextSkill.SetNum(Count);
	for (int32 Id = 0; Id < Count; ++Id)
	{
		NextSkill[Id] = Battle->GetNextEnemySkill(Id);
	}
}

void AFTBattleDemoActor::AddResultLine(const FString& Line)
{
	if (ResultLineCount < MaxResultLines)
	{
		ResultLines[ResultLineCount++] = Line;
	}
}

void AFTBattleDemoActor::BuildResults()
{
	const FFTBattleResults R = Battle->GetResults();
	ResultLineCount = 0;
	AddResultLine(FString::Printf(TEXT("Time %.1f s    Turns %d"), R.Seconds, R.PartyTurns));
	AddResultLine(FString::Printf(TEXT("Damage dealt %d    taken %d"), R.DamageDealt, R.DamageTaken));
	AddResultLine(FString::Printf(TEXT("Notes %d    perfect %d    missed %d    full combos %d"), R.Notes, R.PerfectNotes, R.MissedNotes,
		R.FullCombos));
	AddResultLine(FString::Printf(TEXT("Hits defended %d of %d    perfect %d    counters %d    weak points %d"), R.HitsDefended,
		R.HitsDefended + R.HitsTaken, R.PerfectDefences, R.Counters, R.WeakPointHits));
	UE_LOG(LogForgottenThird, Display,
		TEXT("BattleDemo: results: %.1f s, dealt %d, taken %d, notes %d (%d perfect, %d missed), full combos %d, defended %d of %d (%d perfect), ")
		TEXT("counters %d, weak points %d, phase changes %d, turns %d party / %d enemy."),
		R.Seconds, R.DamageDealt, R.DamageTaken, R.Notes, R.PerfectNotes, R.MissedNotes, R.FullCombos, R.HitsDefended, R.HitsDefended + R.HitsTaken,
		R.PerfectDefences, R.Counters, R.WeakPointHits, R.PhaseChanges, R.PartyTurns, R.EnemyTurns);
}

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
	Slot.bDefence = P.bDefence;
	Slot.bWeak = P.bWeak;
	Slot.bHeal = P.bHeal;
	Slot.Text = P.Text;
}

// ---- battle events ----

void AFTBattleDemoActor::HandleBattleEvent(const FFTBattleEvent& Event)
{
	const ftcore::Battle* B = Battle->GetBattle();
	const double Now = Wall();
	switch (Event.Type)
	{
	case EFTBattleEventType::TurnStarted:
		RefreshNextSkills();
		break;
	case EFTBattleEventType::SkillStarted:
	{
		RefreshNextSkills();
		const bool bEnemy = Battle->GetCombatant(Event.Source).Team == EFTTeam::Enemies;
		// The enemy banner is the attack's caption; the HUD shows the defence keys and tells.
		SetBanner(FString::Printf(TEXT("%s: %s"), *GetFighterName(Event.Source), *Event.Label));
		if (bEnemy)
		{
			DefencePressWall = -100.0;
		}
		if (Event.Label == TEXT("AIM"))
		{
			// The target's weak points, labelled once for the HUD.
			const ftcore::WeakPoints* Points = B ? &B->aimShot().weakPoints() : nullptr;
			for (size_t I = 0; I < ftcore::kMaxWeakPoints; ++I)
			{
				WeakPointLabels[I] = Points && I < Points->size() ? FString::Printf(TEXT("x%.1f"), (*Points)[I].multiplier) : FString();
			}
		}
		else if (!bEnemy)
		{
			BeginLane(); // a party skill or a Release: its combo begins
		}
		break;
	}
	case EFTBattleEventType::NoteJudged:
	{
		if (!B || static_cast<size_t>(Event.Beat) >= B->combo().size())
		{
			break;
		}
		const ftcore::ComboNote& Note = B->combo().note(static_cast<size_t>(Event.Beat));
		FLaneJudge J;
		J.Wall = Now;
		J.Lane = static_cast<int32>(Note.arrow) & 3;
		J.Grade = FT::ToCore(Event.Grade);
		J.bWrong = Event.Label == TEXT("WRONG");
		J.bExpired = J.Grade == ftcore::Grade::Miss && !J.bWrong;
		J.Error = Event.Error;
		J.NoteDt = FMath::Clamp(Note.time - Event.Time, -0.25, 0.25);
		LaneJudges[J.Lane] = J;
		// One word for the lane; a jump pair's second note (the same chart time, judged within a moment of the
		// first) merges into it as the worse grade.
		const bool bPartner = Now - LaneWord.Wall < 0.4 && FMath::IsNearlyEqual(LaneWordNoteTime, Note.time, 1e-9);
		FLaneJudge Word = J;
		if (bPartner && ftcore::worseGrade(LaneWord.Grade, J.Grade) != J.Grade)
		{
			Word = LaneWord; // the first note's judgement was the worse one: it stays, shown again
			Word.Wall = Now;
		}
		LaneWord = Word;
		LaneWordNoteTime = Note.time;
		// The streak: a Miss breaks it (the old number lingers grey), anything else adds; a step pops the badge.
		if (J.Grade == ftcore::Grade::Miss)
		{
			BrokenStreak = Streak;
			BrokenStreakWall = Now;
			Streak = 0;
		}
		else
		{
			++Streak;
			if (ftcore::streakStep(LaneLayout.times, Streak))
			{
				StreakStepWall = Now;
			}
		}
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
		// An aimed shot names what it hit; a weak point is gold.
		P.bWeak = Event.Label == TEXT("WEAK POINT");
		P.Text = Event.Label.IsEmpty() || Event.Label == TEXT("HIT") ? FString::Printf(TEXT("-%d"), Event.Amount)
			: FString::Printf(TEXT("%s  -%d"), *Event.Label, Event.Amount);
		AddPopup(P);
		break;
	}
	case EFTBattleEventType::Defended:
	{
		// One per enemy hit, judged on one line: PERFECT (with the AP refunded), DEFENDED (a dodge or
		// jump), MISSED (the hit landed: its damage number and flash come with the Hit event), then
		// LATE in place of MISSED when a press came just after the impact.
		if (Event.Label == TEXT("LATE"))
		{
			SetJudgement(EFTJudgement::Late, TEXT("LATE"));
		}
		else if (Event.Label == TEXT("HIT"))
		{
			SetJudgement(EFTJudgement::Missed, TEXT("MISSED"));
		}
		else if (Event.Label == TEXT("PERFECT")) // also a perfect parry at full AP (no refund)
		{
			SetJudgement(EFTJudgement::Perfect, Event.Amount > 0 ? FString::Printf(TEXT("PERFECT  +%d AP"), Event.Amount) : FString(TEXT("PERFECT")));
		}
		else if (Event.Label == TEXT("PERFECT GUARD"))
		{
			SetJudgement(EFTJudgement::Perfect, TEXT("PERFECT GUARD"));
		}
		else if (Event.Label == TEXT("PERFECT INTERCEPT")) // Target is the interceptor
		{
			SetJudgement(EFTJudgement::Perfect, FString::Printf(TEXT("PERFECT INTERCEPT  %s"), *GetFighterName(Event.Target)));
		}
		else if (Event.Label == TEXT("INTERCEPT"))
		{
			SetJudgement(EFTJudgement::Defended, FString::Printf(TEXT("INTERCEPT  %s"), *GetFighterName(Event.Target)));
		}
		else
		{
			SetJudgement(EFTJudgement::Defended, TEXT("DEFENDED"));
		}
		break;
	}
	case EFTBattleEventType::ComboFinished:
		// The summary stamp in the lane replaces the top banner. A combo that ended early (the enemies it was aimed
		// at fell) has no stamp: its lane fades at once.
		ComboEndWall = Now;
		bComboStamp = B && B->combo().complete();
		StampAccuracy = Event.Amount;
		bStampFull = !Event.Label.IsEmpty();
		StampBest = B ? B->combo().maxCombo() : 0;
		StampAccuracyText = FString::Printf(TEXT("%d%%"), Event.Amount);
		StampBestText = FString::Format(*FTUiText::Lookup(TEXT("battle.best")), {StampBest});
		break;
	case EFTBattleEventType::Counter:
		SetBanner(Event.Label == TEXT("READ") ? FString::Printf(TEXT("%s strikes the read!"), *GetFighterName(Event.Source))
			: FString::Printf(TEXT("%s counters!"), *GetFighterName(Event.Source)));
		break;
	case EFTBattleEventType::Staggered:
		SetBanner(FString::Printf(TEXT("%s is staggered!"), *GetFighterName(Event.Target)));
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: %s is staggered."), *GetFighterName(Event.Target));
		break;
	case EFTBattleEventType::Steadied:
		SetBanner(FString::Printf(TEXT("%s steadies itself."), *GetFighterName(Event.Source)));
		break;
	case EFTBattleEventType::Overloaded:
		SetBanner(FString::Printf(TEXT("%s is overloaded: Release!"), *GetFighterName(Event.Target)));
		break;
	case EFTBattleEventType::Released:
		SetBanner(FString::Printf(TEXT("%s releases %d Load!"), *GetFighterName(Event.Source), Event.Amount));
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: %s releases %d Load."), *GetFighterName(Event.Source), Event.Amount);
		break;
	case EFTBattleEventType::TornApplied:
		SetBanner(FString::Printf(TEXT("%s is torn for %d turns!"), *GetFighterName(Event.Target), Event.Amount));
		break;
	case EFTBattleEventType::CardsDealt:
		// The reader sees the tell, not the action: the banner's skill name gives way to it.
		if (B && B->readCards().dealt())
		{
			const ftcore::ReadCards& Cards = B->readCards();
			FString Tell = FT::ToFString(Cards[Cards.correct()].tell);
			Tell.ReplaceInline(TEXT("_"), TEXT(" "));
			SetBanner(FString::Printf(TEXT("%s: %s  (read it: %s)"), *GetFighterName(Event.Source), *Tell,
				Cards.size() >= 3 ? TEXT("1 2 3") : TEXT("1 2")));
		}
		break;
	case EFTBattleEventType::CardPicked:
	{
		const FString Action = Battle->GetActiveSkillName();
		if (Event.Grade == EFTGrade::Perfect)
		{
			SetBanner(FString::Printf(TEXT("%s reads %s!  x%d"), *GetFighterName(Event.Source), *Action, Event.Amount));
		}
		else
		{
			SetBanner(FString::Printf(TEXT("Misread: it is %s"), *Action));
		}
		break;
	}
	case EFTBattleEventType::HalfTaken:
		// Two of these, the actor's first: the banner comes with the ally's.
		if (Event.Target != Event.Source)
		{
			SetBanner(FString::Printf(TEXT("%s takes half with %s."), *GetFighterName(Event.Source), *GetFighterName(Event.Target)));
		}
		break;
	case EFTBattleEventType::Downed:
		if (Figures.IsValidIndex(Event.Target))
		{
			Figures[Event.Target].DownWall = Now;
		}
		SetBanner(FString::Printf(TEXT("%s is down!"), *GetFighterName(Event.Target)));
		break;
	case EFTBattleEventType::PhaseChanged:
	{
		// Its own banner: the combo that pushed the enemy over the threshold finishes in the same
		// flush and would overwrite the skill banner.
		const FString& Name = GetFighterName(Event.Target);
		PhaseBanner = Event.Label.IsEmpty() ? FString::Printf(TEXT("%s grows stronger!"), *Name)
		                                    : FString::Printf(TEXT("%s enters %s!"), *Name, *Event.Label);
		PhaseBannerWall = Now;
		if (Figures.IsValidIndex(Event.Target))
		{
			Figures[Event.Target].HitWall = Now;
		}
		UpdateHpText(Event.Target);
		if (Event.Amount > 0)
		{
			FPopup P;
			P.bNumber = true;
			P.bHeal = true;
			P.Combatant = Event.Target;
			P.Text = FString::Printf(TEXT("+%d"), Event.Amount);
			AddPopup(P);
		}
		RefreshNextSkills();
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: %s enters phase %d \"%s\" (+%d HP)."), *Name, Event.Beat, *Event.Label, Event.Amount);
		break;
	}
	case EFTBattleEventType::CountIn:
		// The HUD counts the beats down to the hit, one number per beat.
		CountBeat = Event.Beat;
		CountWall = Now;
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
	if (const ftcore::Battle* B = Battle->GetBattle())
	{
		const ftcore::BattleStats& S = B->stats();
		UE_LOG(LogForgottenThird, Display,
			TEXT("TURNS RESULT: %s in %.1f s, verbs used: Guard %d, Intercept %d, Release %d, Take Half %d, Read %d (%d right)"),
			bWon ? TEXT("Victory") : TEXT("Defeat"), S.endTime - S.startTime, GuardHolds, S.intercepts, S.releases, S.takeHalves,
			S.reads, S.correctReads);
	}
	BuildResults();
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->ExploreCarry.bValid)
	{
		// Started from the exploration demo: a win goes back there; a loss stays for a retry.
		if (bWon)
		{
			GI->ExploreCarry.bWon = true;
			bReturningToStory = true; // (the name covers any return: no restart while leaving)
			bReturnToField = true;
			if (bAutoPlay)
			{
				ContinuePrompt = TEXT("Returning to the field");
				FTimerHandle Timer;
				GetWorldTimerManager().SetTimer(Timer, this, &AFTBattleDemoActor::Continue, 2.5f, false);
			}
			else
			{
				ContinuePrompt = TEXT("Enter  Back to the field");
			}
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
			if (UFTStorySubsystem* Story = GI->GetSubsystem<UFTStorySubsystem>())
			{
				const ftcore::AwardResult Award = Story->AwardBattle(Reward);
				AddResultLine(Award.levelAfter > Award.levelBefore
					? FString::Printf(TEXT("+%d XP    +%d gold    Level %d -> %d  LEVEL UP!"), Reward.xp, Reward.gold, Award.levelBefore, Award.levelAfter)
					: FString::Printf(TEXT("+%d XP    +%d gold    Level %d"), Reward.xp, Reward.gold, Award.levelAfter));
			}
			GI->StoryCarry.bDone = true;
			GI->StoryCarry.bWon = true;
			bReturningToStory = true;
			if (bAutoPlay)
			{
				ContinuePrompt = TEXT("Returning to the story");
				FTimerHandle Timer;
				GetWorldTimerManager().SetTimer(Timer, this, &AFTBattleDemoActor::Continue, 2.5f, false);
			}
			else
			{
				ContinuePrompt = TEXT("Enter  Continue");
			}
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

void AFTBattleDemoActor::Continue()
{
	if (bLeaving)
	{
		return;
	}
	bLeaving = true;
	if (bReturnToField)
	{
		ReturnToField();
	}
	else
	{
		ReturnToStory();
	}
}

void AFTBattleDemoActor::ReturnToStory()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: returning to the story on %s."), *GI->StoryCarry.ReturnMap);
		FTStep::OpenMap(this, GI->StoryCarry.ReturnMap);
	}
}

void AFTBattleDemoActor::RetryAfterDefeat()
{
	if (bWaitingForRestart && !bReturningToStory && !bQuitting)
	{
		StartRound();
	}
}

void AFTBattleDemoActor::HandleBack()
{
	// Esc / pad Start while aiming put the shot back, and the press doesn't also open the pause menu.
	if (!bAutoPlay && Battle->IsAimActive())
	{
		Battle->CancelAim();
		if (AFTPlayerController* PC = GetPlayer())
		{
			PC->ClaimBack();
		}
	}
}

void AFTBattleDemoActor::HandleQuit()
{
	if (UFTPauseSubsystem::IsBlocking(this))
	{
		return;
	}
	// Backspace / pad B while aiming put the shot back (pad B also arrives as a face button).
	if (!bAutoPlay && Battle->IsAimActive())
	{
		Battle->CancelAim();
		return;
	}
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
	FTStep::OpenMap(this, GI->StoryCarry.ReturnMap, TEXT("Title"));
}

void AFTBattleDemoActor::ReturnToField()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->ExploreCarry.bValid)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: returning to %s."), *GI->ExploreCarry.ReturnMap);
		FTStep::OpenMap(this, GI->ExploreCarry.ReturnMap);
	}
}

// ---- input ----

void AFTBattleDemoActor::HandleArrow(EFTArrow Arrow, bool bFaceButton)
{
	if (bAutoPlay || bWaitingForRestart || !Battle->GetBattle() || UFTPauseSubsystem::IsBlocking(this))
	{
		return;
	}
	if (Battle->IsComboActive())
	{
		PressLane(Arrow);
		return;
	}
	if (Battle->IsAimActive())
	{
		// Pad A fires and pad B cancels; held arrows move the reticle (polled in TickAimInput).
		if (bFaceButton && Arrow == EFTArrow::Down)
		{
			Battle->Fire();
		}
		else if (bFaceButton && Arrow == EFTArrow::Right)
		{
			Battle->CancelAim();
		}
		return;
	}
	// Just after an enemy attack a press can still be judged late for its last hit.
	const int32 Actor = Battle->GetActor();
	const bool bAfterAttack = Battle->GetPhase() == EFTBattlePhase::Recovery && Actor >= 0
		&& Battle->GetCombatant(Actor).Team == EFTTeam::Enemies;
	if (Battle->IsDefenceActive() || bAfterAttack)
	{
		// Arrows and face buttons alike: Left/Right (X/B) dodge, Down (A) parries, Up (Y) jumps.
		const EFTDefenceInput Input = Arrow == EFTArrow::Down ? EFTDefenceInput::Parry
			: Arrow == EFTArrow::Up                           ? EFTDefenceInput::Jump
			                                                  : EFTDefenceInput::Dodge;
		DefencePressWall = Wall();
		LastDefenceInput = Input;
		Battle->Defend(Input);
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
	// The skills, then the Aim row.
	const int32 Count = Battle->GetSkillCount(Battle->GetActor()) + 1;
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

void AFTBattleDemoActor::PressLane(EFTArrow Arrow)
{
	const ftcore::Battle* B = Battle->GetBattle();
	const int32 StraysBefore = B ? B->combo().strays() : 0;
	LanePressWall[static_cast<int32>(Arrow) & 3] = Wall();
	Battle->Press(Arrow);
	// A stray press (no note in range) locks input for a moment: the lane says so, except for the press that
	// confirmed the skill (pad A is both Confirm and the Down lane, so that press can reach the combo as a stray).
	if (B && B->combo().strays() > StraysBefore && ftcore::strayCueShows(LaneLayout.times, Wall() - ComboStartWall))
	{
		StrayWall = Wall();
	}
}

void AFTBattleDemoActor::BeginLane()
{
	const double Now = Wall();
	ComboStartWall = Now;
	ComboEndWall = -1.0;
	bComboRunning = true;
	bComboStamp = false;
	Streak = 0;
	BrokenStreak = 0;
	BrokenStreakWall = -100.0;
	StreakStepWall = -100.0;
	StrayWall = -100.0;
	LaneWord = FLaneJudge();
	LaneWordNoteTime = -1.0;
	for (FLaneJudge& J : LaneJudges)
	{
		J = FLaneJudge();
	}
	StickFlick.reset();
	++LaneShotCombos;
	bLaneShotStrayed = false;
	if (const UFTGameInstance* SettingsGI = GetGameInstance<UFTGameInstance>())
	{
		if (const UFTSettingsSubsystem* SettingsSub = SettingsGI->GetSubsystem<UFTSettingsSubsystem>())
		{
			bReduceFlashes = SettingsSub->Get().reduceFlashes;
			bStickFlicks = SettingsSub->Get().stickFlicks;
		}
	}
}

void AFTBattleDemoActor::TickStickFlicks()
{
	AFTPlayerController* PC = GetPlayer();
	if (!bStickFlicks || !PC || bAutoPlay || bWaitingForRestart || !Battle->GetBattle() || !Battle->IsComboActive()
		|| UFTPauseSubsystem::IsBlocking(this) || UFTPauseSubsystem::IsJustResumed(this))
	{
		StickFlick.reset(); // the next combo waits for the stick to be at rest
		return;
	}
	const float X = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
	const float Y = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY);
	if (const std::optional<ftcore::Arrow> Flick = StickFlick.update(X, Y))
	{
		PressLane(FT::FromCore(*Flick));
	}
}

void AFTBattleDemoActor::HandleSkill(int32 Skill)
{
	if (!bAutoPlay && !bWaitingForRestart && !UFTPauseSubsystem::IsBlocking(this) && TryPickCard(Skill))
	{
		return;
	}
	if (bAutoPlay || bWaitingForRestart || Battle->GetPhase() != EFTBattlePhase::AwaitingCommand || UFTPauseSubsystem::IsBlocking(this))
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
	if (!bAutoPlay && !bWaitingForRestart && Battle->GetPhase() == EFTBattlePhase::AwaitingCommand && !UFTPauseSubsystem::IsBlocking(this))
	{
		CycleTarget(Step);
	}
}

void AFTBattleDemoActor::HandleConfirm()
{
	if (bAutoPlay || bWaitingForRestart || UFTPauseSubsystem::IsBlocking(this))
	{
		return;
	}
	if (Battle->IsAimActive())
	{
		Battle->Fire();
	}
	else if (Battle->GetPhase() == EFTBattlePhase::AwaitingCommand)
	{
		UseSelected();
	}
}

void AFTBattleDemoActor::HandleRestart()
{
	// A short pause first, so the press that ended the battle doesn't start the next one (or skip
	// the results).
	if (!bWaitingForRestart || bQuitting || Wall() - OutcomeWall <= 1.0 || UFTPauseSubsystem::IsBlocking(this))
	{
		return;
	}
	if (!bReturningToStory)
	{
		StartRound();
	}
	else if (!bAutoPlay)
	{
		Continue();
	}
}

void AFTBattleDemoActor::UseSelected()
{
	if (SelectedSkill == GetAimRow())
	{
		EnterAim();
		return;
	}
	EnsureTarget();
	const EFTCommandError Err = Battle->ChooseSkill(SelectedSkill, SelectedTarget);
	switch (Err)
	{
	case EFTCommandError::Overloaded:
		SetNotice(TEXT("Overloaded: Release (R / RT)"));
		break;
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

void AFTBattleDemoActor::EnterAim()
{
	EnsureTarget();
	switch (Battle->AimAt(SelectedTarget))
	{
	case EFTCommandError::Overloaded:
		SetNotice(TEXT("Overloaded: Release (R / RT)"));
		break;
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

void AFTBattleDemoActor::TickAutoPlay(float DeltaSeconds)
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (!B || Battle->IsBattleOver())
	{
		return;
	}
	if (B->phase() == ftcore::Phase::AwaitingCommand)
	{
		// -BattleCamShot keeps the command open for the camera's views (and may pick the target or the Aim row itself).
		if (CameraShotHolds())
		{
			return;
		}
		// The Turns demo: Take Half first when the party's Torn is uneven (ftcore::demo::pilotTakeHalf).
		if (bTurnsDemo && !bIdlePilot)
		{
			const ftcore::CombatantId Ally = ftcore::demo::pilotTakeHalf(*B);
			if (Ally != ftcore::demo::kNoAlly && Battle->TakeHalf(static_cast<int32>(Ally)) == EFTCommandError::None)
			{
				return;
			}
		}
		const ftcore::demo::PilotChoice Choice = ftcore::demo::pilotChoice(*B);
		if (bIdlePilot && !Choice.release)
		{
			SelectedSkill = static_cast<int32>(Choice.skill); // never aims
			SelectedTarget = static_cast<int32>(Choice.target);
			UseSelected();
			return;
		}
		if (Choice.release)
		{
			// Under -BattleShot the Release waits for the Load view (the gauge still full) and a moment after it.
			if (!ShotPrefix.IsEmpty() && ((ShotMask & 1) == 0 || Wall() - ShotLastWall < 0.25))
			{
				return;
			}
			Battle->Release();
			return;
		}
		SelectedSkill = Choice.aim ? GetAimRow() : static_cast<int32>(Choice.skill);
		SelectedTarget = static_cast<int32>(Choice.target);
		PilotWeakPoint = Choice.aim ? Choice.weakPoint : ftcore::kMaxWeakPoints;
		UseSelected();
		return;
	}
	if (bIdlePilot)
	{
		return; // no presses, no defence, no reads
	}
	if (B->phase() == ftcore::Phase::PartyAim)
	{
		// Same as ftcore::demo::pilotShoot, after a short visible aim: the reticle glides onto the
		// weak point, then sits exactly where the sway puts the aim point on it and fires.
		const ftcore::AimShot& Aim = B->aimShot();
		const double Now = Battle->GetBattleTime();
		const size_t Point = PilotWeakPoint < Aim.weakPoints().size() ? PilotWeakPoint : Aim.weakPoints().best();
		if (Point >= Aim.weakPoints().size())
		{
			Battle->FireAtTime(Now); // no weak point: the body
			return;
		}
		const ftcore::AimPoint Want = Aim.reticleFor(Point, Now);
		if (Now - Aim.start() < kPilotAimSeconds)
		{
			const ftcore::AimPoint At = Aim.reticle();
			const float K = FMath::Clamp(static_cast<float>(DeltaSeconds) * 8.f, 0.f, 1.f);
			Battle->SetReticle(FVector2D(At.x + (Want.x - At.x) * K, At.y + (Want.y - At.y) * K));
			return;
		}
		if (!CamShotPrefix.IsEmpty() && (!(CamShotMask & 8) || Wall() - CamShotLastWall < 0.5))
		{
			return; // under -BattleCamShot the shot waits for the Aim view, and a moment after it
		}
		Battle->SetReticle(FVector2D(Want.x, Want.y));
		Battle->FireAtTime(Now);
		PilotWeakPoint = ftcore::kMaxWeakPoints;
		return;
	}
	if (Battle->IsDefenceActive())
	{
		// Same as ftcore::demo::pilotRead, never wrong: the card whose tell the coming action shows.
		// Under -BattleShot the pick waits for the Read view (cards still open) and a moment after it.
		const bool bShotWaits = !ShotPrefix.IsEmpty() && ((ShotMask & 2) == 0 || Wall() - ReadShotWall < 0.25);
		const ftcore::ReadCards& Cards = B->readCards();
		const ftcore::Skill* Coming = B->activeSkill();
		if (Cards.dealt() && !Cards.picked() && Coming && !bShotWaits)
		{
			for (size_t C = 0; C < Cards.size(); ++C)
			{
				if (Cards[C].tell == ftcore::tellOf(*Coming))
				{
					Battle->PickCard(static_cast<int32>(C));
					break;
				}
			}
		}
		// Same as ftcore::demo::pilotDefend in parry mode: every hit whose ideal press time comes
		// before the next update is parried at that time, read from the timeline (never from audio).
		const double Horizon = Battle->GetBattleTime()
			+ FMath::Clamp(kPilotFrames * static_cast<double>(DeltaSeconds), kPilotMinLookahead, kPilotMaxLookahead);
		// The Turns demo defends as ftcore::demo::pilotDefend in Guard mode: an Intercept when a party
		// member can take the hit for the defender, else a Guard for a defender with the verb, else a
		// parry; with PilotSkill.showTorn the first tearing attack lands unanswered.
		ftcore::DefenceInput Hold = ftcore::DefenceInput::Guard;
		const bool bHolds = bTurnsDemo && ftcore::demo::pilotHolds(*B, ftcore::DefenceInput::Guard, Hold);
		const bool bLetTear = bTurnsDemo && ftcore::demo::pilotLetsTear(*B, PilotSkill, PilotState);
		if (bHolds || bLetTear)
		{
			PilotHold(Horizon, Hold, bHolds && !bLetTear);
			return;
		}
		while (Battle->IsDefenceActive())
		{
			const ftcore::DefenceTimeline& Defence = B->defence();
			const size_t I = Defence.nextIndex();
			if (I >= Defence.size())
			{
				break;
			}
			const double Ideal = Defence.idealPress(I, ftcore::DefenceInput::Parry);
			if (Ideal > Horizon)
			{
				break;
			}
			DefencePressWall = Wall();
			LastDefenceInput = EFTDefenceInput::Parry;
			Battle->DefendAtTime(EFTDefenceInput::Parry, Ideal);
		}
		return;
	}
	if (!Battle->IsComboActive())
	{
		return;
	}
	const double Now = Battle->GetBattleTime();
	const ftcore::ComboSequence& Combo = B->combo();
	const bool bLaneShot = !LaneShotPrefix.IsEmpty();
	if (bLaneShot && LaneShotCombos >= 2 && !bLaneShotStrayed && GetComboAge() > 0.45 && Now < Combo.note(0).time - Combo.windows().good - 0.05)
	{
		// One stray press well ahead of the first note, for the lockout cue's view.
		bLaneShotStrayed = true;
		PressLane(EFTArrow::Left);
		return;
	}
	for (size_t I = Combo.nextIndex(); I < Combo.size(); ++I)
	{
		// -BattleLaneShot plays some notes early, late or not at all; otherwise every note on its own time.
		double Offset = 0.0;
		bool bSkip = false;
		if (bLaneShot)
		{
			LaneShotPlan(I, Offset, bSkip);
		}
		const double PressTime = Combo.note(I).time + Offset;
		if (PressTime > Now)
		{
			break;
		}
		if (Combo.grade(I) == ftcore::Grade::Pending && !bSkip)
		{
			LanePressWall[static_cast<int32>(Combo.note(I).arrow)] = Wall();
			// At the note's own time, not "now": a long (dilated) frame must not turn PERFECT into late.
			Battle->PressAtTime(FT::FromCore(Combo.note(I).arrow), PressTime);
		}
		if (!Battle->IsComboActive())
		{
			break;
		}
	}
}

void AFTBattleDemoActor::LaneShotPlan(size_t I, double& Offset, bool& bSkip) const
{
	Offset = 0.0;
	bSkip = false;
	const ftcore::Battle* B = Battle->GetBattle();
	if (!B || LaneShotCombos < 2)
	{
		return; // the first combo plays clean
	}
	const ftcore::JudgeWindows& W = B->combo().windows();
	switch (I)
	{
	case 1: Offset = -(W.perfect + W.great) * 0.5; break; // a Great or Good, early
	case 2: Offset = (W.great + W.good) * 0.5; break;     // a Good, late
	case 3: bSkip = true; break;                          // a Miss
	default: break;
	}
}

void AFTBattleDemoActor::PilotHold(double Horizon, ftcore::DefenceInput Hold, bool bAnswer)
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (!B || !Battle->IsDefenceActive())
	{
		return;
	}
	const ftcore::DefenceTimeline& Defence = B->defence();
	const size_t I = Defence.nextIndex();
	if (Defence.guarding() && (I >= Defence.size() || Defence.guardFrom() < Defence.idealPress(I, Hold) - 1e-9))
	{
		Battle->ReleaseGuardAtTime(Defence.now()); // the hold was for a hit that has come
	}
	if (!bAnswer || !Battle->IsDefenceActive() || I >= Defence.size() || Defence.guarding())
	{
		return;
	}
	const double Ideal = Defence.idealPress(I, Hold);
	if (Ideal > Horizon || Ideal < Defence.now())
	{
		return;
	}
	DefencePressWall = Wall();
	LastDefenceInput = static_cast<EFTDefenceInput>(Hold);
	Battle->DefendAtTime(LastDefenceInput, Ideal);
	GuardHolds += Hold == ftcore::DefenceInput::Guard ? 1 : 0;
}

void AFTBattleDemoActor::TickShots()
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (ShotPrefix.IsEmpty() || !B)
	{
		return;
	}
	const double Now = Wall();
	if (ShotStartWall < 0.0)
	{
		ShotStartWall = Now;
	}
	constexpr uint8 kAll = 31;
	if (ShotMask == kAll || Now - ShotStartWall > 300.0 || Battle->IsBattleOver())
	{
		if (ShotMask != kAll)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: only views %d of 31 came; quitting."), ShotMask);
		}
		if (Now - ShotLastWall > 1.0) // the last view is written first
		{
			ShotPrefix.Reset();
			FPlatformMisc::RequestExit(false, TEXT("BattleDemo"));
		}
		return;
	}
	if (Now - ShotStartWall < 3.0 || Now - ShotLastWall < 0.6)
	{
		return;
	}
	const bool bDefence = Battle->IsDefenceActive();
	const ftcore::DefenceTimeline& T = B->defence();
	bool bTorn = false;
	for (const ftcore::Combatant& C : B->combatants())
	{
		bTorn = bTorn || (C.team == ftcore::Team::Party && C.tornTurns > 0);
	}
	if (!(ShotMask & 1) && B->phase() == ftcore::Phase::AwaitingCommand && B->combatant(0).load.value >= 40)
	{
		TakeShot(TEXT("1_load"), 1);
	}
	else if (!(ShotMask & 2) && bDefence && B->readCards().dealt() && !B->readCards().picked() && GetCountBeat() > 0)
	{
		TakeShot(TEXT("2_read"), 2);
		ReadShotWall = Now;
	}
	else if (!(ShotMask & 4) && bTorn && B->phase() == ftcore::Phase::AwaitingCommand)
	{
		TakeShot(TEXT("3_torn"), 4);
	}
	else if (!(ShotMask & 8) && bDefence && GetCountBeat() > 0 && GetCountAge() < 0.12 && (ShotMask & 2))
	{
		TakeShot(TEXT("4_countin"), 8);
	}
	else if (!(ShotMask & 16) && (ShotMask & 4) && bDefence && T.intercepting())
	{
		TakeShot(TEXT("5_intercept"), 16);
	}
}

void AFTBattleDemoActor::TakeShot(const TCHAR* Name, uint8 Bit)
{
	const FString File = FString::Printf(TEXT("%s_%s.png"), *ShotPrefix, Name);
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *File), *GLog);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: view %s."), *File);
	ShotMask |= Bit;
	ShotLastWall = Wall();
}

void AFTBattleDemoActor::TickLaneShots()
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (LaneShotPrefix.IsEmpty() || !B)
	{
		return;
	}
	const double Now = Wall();
	if (LaneShotStartWall < 0.0)
	{
		LaneShotStartWall = Now;
	}
	constexpr uint16 kAll = 2047;
	if (LaneShotMask == kAll || Now - LaneShotStartWall > 300.0 || Battle->IsBattleOver())
	{
		if (LaneShotMask != kAll)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: only lane views %d of %d came; quitting."), LaneShotMask, kAll);
		}
		if (Now - LaneShotLastWall > 1.0) // the last view is written first
		{
			LaneShotPrefix.Reset();
			FPlatformMisc::RequestExit(false, TEXT("BattleDemo"));
		}
		return;
	}
	if (Now - LaneShotStartWall < 3.0 || Now - LaneShotLastWall < 0.2)
	{
		return;
	}
	const ftcore::ComboSequence& Combo = B->combo();
	const double Age = GetComboAge();
	const double EndAge = GetComboEndAge();
	const double WordAge = Now - LaneWord.Wall;
	const bool bFirst = LaneShotCombos == 1;
	const bool bLater = LaneShotCombos >= 2;
	const bool bRunning = Battle->IsComboActive();
	auto Want = [&](uint16 Bit) { return !(LaneShotMask & Bit); };

	if (bFirst && bRunning && Want(1) && Age >= 0.1)
	{
		TakeLaneShot(TEXT("1_fadein"), 1);
	}
	else if (bFirst && bRunning && Want(2) && Combo.nextIndex() == 0 && Combo.size() > 0
		&& Combo.note(0).time - Battle->GetBattleTime() <= LaneLayout.times.lookahead - 0.15)
	{
		TakeLaneShot(TEXT("2_entering"), 2);
	}
	else if (bFirst && Want(4) && LaneWord.Grade == ftcore::Grade::Perfect && WordAge >= 0.04 && WordAge < 0.12 && Combo.judged() == 1)
	{
		TakeLaneShot(TEXT("3_perfect"), 4);
	}
	else if (bFirst && bRunning && Want(8) && Combo.judged() >= 2 && Combo.size() - Combo.judged() >= 2 && WordAge >= 0.15 && WordAge < 0.3)
	{
		TakeLaneShot(TEXT("4_mid"), 8);
	}
	else if (bFirst && Want(16) && Streak >= LaneLayout.times.streakEvery && GetStreakStepAge() >= 0.04 && GetStreakStepAge() < 0.12)
	{
		TakeLaneShot(TEXT("5_streak"), 16);
	}
	else if (bFirst && Want(32) && bComboStamp && bStampFull && EndAge >= 0.2 && EndAge < 0.35)
	{
		TakeLaneShot(TEXT("6_exit_full"), 32);
	}
	else if (bLater && Want(64) && LaneWord.Grade != ftcore::Grade::Perfect && LaneWord.Grade != ftcore::Grade::Miss && LaneWord.Error < 0.0
		&& WordAge >= 0.05 && WordAge < 0.14)
	{
		TakeLaneShot(TEXT("7_early"), 64);
	}
	else if (bLater && Want(128) && LaneWord.Grade != ftcore::Grade::Perfect && LaneWord.Grade != ftcore::Grade::Miss && LaneWord.Error > 0.0
		&& WordAge >= 0.05 && WordAge < 0.14)
	{
		TakeLaneShot(TEXT("8_late"), 128);
	}
	else if (bLater && Want(256) && LaneWord.Grade == ftcore::Grade::Miss && LaneWord.bExpired && WordAge >= 0.05 && WordAge < 0.14)
	{
		TakeLaneShot(TEXT("9_miss"), 256);
	}
	else if (bLater && Want(512) && GetStrayAge() >= 0.05 && GetStrayAge() < 0.15)
	{
		TakeLaneShot(TEXT("10_lockout"), 512);
	}
	else if (bLater && Want(1024) && bComboStamp && !bStampFull && EndAge >= 0.2 && EndAge < 0.35)
	{
		TakeLaneShot(TEXT("11_exit_stamp"), 1024);
	}
}

void AFTBattleDemoActor::TakeLaneShot(const TCHAR* Name, uint16 Bit)
{
	const FString File = FString::Printf(TEXT("%s_%s.png"), *LaneShotPrefix, Name);
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *File), *GLog);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: lane view %s."), *File);
	LaneShotMask |= Bit;
	LaneShotLastWall = Wall();
}

void AFTBattleDemoActor::TickCamera()
{
	const ftcore::Battle* B = Battle->GetBattle();
	const double Now = Wall();
	// The pause-aware clock: the camera stands still with the rest of the battle while the pause menu is up.
	const double Dt = CameraWall < 0.0 ? 0.0 : Now - CameraWall;
	CameraWall = Now;
	if (!B || CameraFigures.size() != B->combatants().size())
	{
		return;
	}
	for (size_t Id = 0; Id < CameraFigures.size(); ++Id)
	{
		CameraFigures[Id].alive = B->combatant(static_cast<ftcore::CombatantId>(Id)).alive();
	}
	ftcore::CameraCue Cue;
	Cue.phase = B->phase();
	Cue.actor = static_cast<int>(B->actor());
	switch (Cue.phase)
	{
	case ftcore::Phase::AwaitingCommand:
	{
		// A skill that hits every enemy frames the whole line; the Aim row and the rest frame the picked enemy.
		const ftcore::Combatant& Actor = B->combatant(B->actor());
		const bool bAll = SelectedSkill >= 0 && static_cast<size_t>(SelectedSkill) < Actor.skills.size()
			&& Actor.skills[static_cast<size_t>(SelectedSkill)].targeting == ftcore::Targeting::AllEnemies;
		Cue.target = bAll ? -1 : SelectedTarget;
		break;
	}
	case ftcore::Phase::PartyAim:
		Cue.target = static_cast<int>(B->aimTarget());
		break;
	case ftcore::Phase::PartySkill:
		Cue.target = B->activeTargets().size() == 1 ? static_cast<int>(B->activeTargets().front()) : -1;
		break;
	case ftcore::Phase::EnemySkill:
	{
		// The camera leans toward whoever takes the hit: the interceptor while an Intercept is held.
		const std::optional<ftcore::CombatantId> Interceptor = B->defence().intercepting() ? B->interceptor() : std::nullopt;
		Cue.defender = static_cast<int>(Interceptor ? *Interceptor : B->defender());
		break;
	}
	default:
		break;
	}
	CameraDirector.update(Dt, Cue, CameraFigures);

	const ftcore::BattleCameraPose& Pose = CameraDirector.pose();
	const FVector Eye(Pose.eye.x, Pose.eye.y, Pose.eye.z);
	const FVector Look(Pose.look.x, Pose.look.y, Pose.look.z);
	Camera->SetWorldLocationAndRotation(Eye, FRotationMatrix::MakeFromX(Look - Eye).Rotator());
	Camera->SetFieldOfView(static_cast<float>(Pose.fov));
}

bool AFTBattleDemoActor::CameraShotHolds()
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (CamShotPrefix.IsEmpty() || !B)
	{
		return false;
	}
	const double Now = Wall();
	const int32 Actor = static_cast<int32>(B->actor());
	if (CamChooseWall < 0.0 || CamChooseActor != Actor)
	{
		CamChooseWall = Now;
		CamChooseActor = Actor;
		CamTargetWall = -1.0;
	}
	if (Now - CamShotLastWall < 0.6)
	{
		return true; // a view was just written
	}
	// The camera has settled into the choosing shot on this lead.
	const bool bSettled = Now - CamChooseWall > 1.0 && CameraDirector.shot() == ftcore::CameraShot::Choose
		&& CameraDirector.shotActor() == Actor && CameraDirector.blend() >= 1.0;
	if (!(CamShotMask & 1))
	{
		if (bSettled)
		{
			CamFirstActor = Actor;
			TakeCameraShot(TEXT("1_choose"), 1);
		}
		return true;
	}
	if (!(CamShotMask & 2) && Actor == CamFirstActor)
	{
		// The player moves the pick to the last living enemy; the camera reframes so it shows past the shoulder.
		if (CamTargetWall < 0.0)
		{
			const int32 First = SelectedTarget;
			CycleTarget(-1);
			CamTargetWall = Now;
			UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: camera view: target %d -> %d."), First, SelectedTarget);
		}
		else if (Now - CamTargetWall > 1.0 && bSettled)
		{
			TakeCameraShot(TEXT("2_target"), 2);
		}
		return true;
	}
	if (Actor == CamFirstActor)
	{
		return false; // this lead has had its views: let the autopilot play the turn
	}
	if (!(CamShotMask & 4))
	{
		if (bSettled)
		{
			TakeCameraShot(TEXT("3_ally"), 4);
		}
		return true;
	}
	if (!(CamShotMask & 8) && CamAimWall < 0.0 && B->combatant(B->actor()).ap >= Battle->Tuning.AimApCost)
	{
		// Take the Aim row for this one turn: the autopilot glides the reticle, and TickCameraShots writes the view.
		SelectedSkill = GetAimRow();
		UseSelected();
		if (B->phase() == ftcore::Phase::PartyAim)
		{
			CamAimWall = Now;
			return true;
		}
	}
	return false;
}

void AFTBattleDemoActor::TickCameraShots()
{
	const ftcore::Battle* B = Battle->GetBattle();
	if (CamShotPrefix.IsEmpty() || !B)
	{
		return;
	}
	const double Now = Wall();
	if (CamShotStartWall < 0.0)
	{
		CamShotStartWall = Now;
	}
	if (B->phase() != ftcore::Phase::AwaitingCommand && B->phase() != ftcore::Phase::PartyAim)
	{
		CamChooseWall = -1.0;
	}
	constexpr uint8 kAll = 127;
	const bool bOver = Battle->IsBattleOver();
	if (CamShotMask == kAll || Now - CamShotStartWall > 300.0 || (bOver && (CamShotMask & 64)))
	{
		if (CamShotMask != kAll)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("BattleDemo: only camera views %d of 127 came; quitting."), CamShotMask);
		}
		if (Now - CamShotLastWall > 1.0) // the last view is written first
		{
			CamShotPrefix.Reset();
			FPlatformMisc::RequestExit(false, TEXT("BattleDemo"));
		}
		return;
	}
	if (Now - CamShotStartWall < 3.0 || Now - CamShotLastWall < 0.6)
	{
		return;
	}
	const double PhaseAge = Battle->GetBattleTime() - B->phaseStart();
	const bool bSettled = CameraDirector.blend() >= 1.0;
	if (bOver)
	{
		if (!(CamShotMask & 64) && bSettled && CameraDirector.shot() == ftcore::CameraShot::Wide && Now - OutcomeWall > 1.0)
		{
			TakeCameraShot(TEXT("7_outcome"), 64);
		}
	}
	else if (!(CamShotMask & 8) && B->phase() == ftcore::Phase::PartyAim)
	{
		if (bSettled && CameraDirector.shot() == ftcore::CameraShot::Aim && PhaseAge > 0.7)
		{
			TakeCameraShot(TEXT("4_aim"), 8);
		}
	}
	else if (!(CamShotMask & 16) && B->phase() == ftcore::Phase::PartySkill && Battle->IsComboActive())
	{
		if (bSettled && CameraDirector.shot() == ftcore::CameraShot::Behind && PhaseAge > 1.0)
		{
			TakeCameraShot(TEXT("5_party"), 16);
		}
	}
	else if (!(CamShotMask & 32) && Battle->IsDefenceActive() && B->readCards().dealt())
	{
		if (bSettled && CameraDirector.shot() == ftcore::CameraShot::Behind && PhaseAge > 0.9)
		{
			TakeCameraShot(TEXT("6_enemy"), 32);
		}
	}
}

void AFTBattleDemoActor::TakeCameraShot(const TCHAR* Name, uint8 Bit)
{
	const FString File = FString::Printf(TEXT("%s_%s.png"), *CamShotPrefix, Name);
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *File), *GLog);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("BattleDemo: camera view %s."), *File);
	CamShotMask |= Bit;
	CamShotLastWall = Wall();
}

void AFTBattleDemoActor::TickAimInput(float DeltaSeconds)
{
	AFTPlayerController* PC = GetPlayer();
	const bool bLive = PC && !bAutoPlay && !bWaitingForRestart && Battle->GetBattle();
	const bool bAiming = bLive && Battle->IsAimActive();
	// Edges of the polled keys. The right mouse button and pad LT toggle aim; F enters it.
	const bool bAimDown = bLive && (PC->IsInputKeyDown(EKeys::F) || PC->IsInputKeyDown(EKeys::Gamepad_LeftTrigger));
	const bool bFireDown = bLive && PC->IsInputKeyDown(EKeys::LeftMouseButton);
	const bool bCancelDown = bLive && PC->IsInputKeyDown(EKeys::RightMouseButton);
	// What is still held the frame after the pause menu closed (the click on Resume) is not a press of
	// the aim's own: the edges wait until the keys are let go and pressed again.
	const bool bResumed = UFTPauseSubsystem::IsJustResumed(this);
	const bool bAimPressed = bAimDown && !bAimKeyDown && !bResumed;
	const bool bFirePressed = bFireDown && !bFireKeyDown && !bResumed;
	const bool bCancelPressed = bCancelDown && !bCancelKeyDown && !bResumed;
	bAimKeyDown = bAimDown;
	bFireKeyDown = bFireDown;
	bCancelKeyDown = bCancelDown;
	if (!bLive)
	{
		return;
	}
	if (!bAiming)
	{
		const ftcore::Battle* B = Battle->GetBattle();
		const bool bPartyTurn = B->phase() == ftcore::Phase::AwaitingCommand && B->combatant(B->actor()).team == ftcore::Team::Party;
		if (bPartyTurn && (bAimPressed || bCancelPressed))
		{
			SelectedSkill = GetAimRow();
			EnterAim();
		}
		return;
	}
	if (bFirePressed)
	{
		Battle->Fire();
		return;
	}
	if (bCancelPressed || (bAimPressed && PC->IsInputKeyDown(EKeys::Gamepad_LeftTrigger)))
	{
		Battle->CancelAim();
		return;
	}

	// The mouse moves the reticle by its own distance on screen: pixels over the figure's height.
	const int32 Target = Battle->GetAimTarget();
	float FigurePixels = kFallbackFigurePixels;
	FVector2D Feet, Head;
	if (PC->ProjectWorldLocationToScreen(GetFigurePoint(Target, 0.f, 0.f), Feet)
		&& PC->ProjectWorldLocationToScreen(GetFigurePoint(Target, 0.f, 1.f), Head))
	{
		FigurePixels = FMath::Max(static_cast<float>(FVector2D::Distance(Feet, Head)), 40.f);
	}
	float MouseX = 0.f, MouseY = 0.f;
	PC->GetInputMouseDelta(MouseX, MouseY);
	// Mouse Y is up-positive, like figure Y.
	double DX = MouseX / FigurePixels;
	double DY = MouseY / FigurePixels;

	// Either stick, past a dead zone; the arrows (and WASD) while held.
	auto Stick = [&](const FKey& Key)
	{
		const float V = PC->GetInputAnalogKeyState(Key);
		return FMath::Abs(V) > kStickDeadZone ? static_cast<double>(V) : 0.0;
	};
	const double Dt = static_cast<double>(DeltaSeconds);
	DX += (Stick(EKeys::Gamepad_LeftX) + Stick(EKeys::Gamepad_RightX)) * kStickAimSpeed * Dt;
	DY += (Stick(EKeys::Gamepad_LeftY) + Stick(EKeys::Gamepad_RightY)) * kStickAimSpeed * Dt;
	const double KeyX = (PC->IsInputKeyDown(EKeys::Right) || PC->IsInputKeyDown(EKeys::D) ? 1.0 : 0.0)
		- (PC->IsInputKeyDown(EKeys::Left) || PC->IsInputKeyDown(EKeys::A) ? 1.0 : 0.0);
	const double KeyY = (PC->IsInputKeyDown(EKeys::Up) || PC->IsInputKeyDown(EKeys::W) ? 1.0 : 0.0)
		- (PC->IsInputKeyDown(EKeys::Down) || PC->IsInputKeyDown(EKeys::S) ? 1.0 : 0.0);
	DX += KeyX * kKeyAimSpeed * Dt;
	DY += KeyY * kKeyAimSpeed * Dt;
	if (DX != 0.0 || DY != 0.0)
	{
		Battle->MoveReticle(FVector2D(DX, DY));
	}
}

void AFTBattleDemoActor::TickVerbInput()
{
	AFTPlayerController* PC = GetPlayer();
	const bool bLive = PC && !bAutoPlay && !bWaitingForRestart && Battle->GetBattle();
	const bool bGuardDown = bLive && (PC->IsInputKeyDown(EKeys::G) || PC->IsInputKeyDown(EKeys::Gamepad_RightShoulder));
	const bool bInterceptDown = bLive && (PC->IsInputKeyDown(EKeys::I) || PC->IsInputKeyDown(EKeys::Gamepad_LeftShoulder));
	const bool bReleaseDown = bLive && (PC->IsInputKeyDown(EKeys::R) || PC->IsInputKeyDown(EKeys::Gamepad_RightTrigger));
	const bool bTakeHalfDown = bLive && (PC->IsInputKeyDown(EKeys::T) || PC->IsInputKeyDown(EKeys::Gamepad_LeftThumbstick));
	const bool bResumed = UFTPauseSubsystem::IsJustResumed(this);
	const bool bGuardPressed = bGuardDown && !bGuardKeyDown && !bResumed;
	const bool bInterceptPressed = bInterceptDown && !bInterceptKeyDown && !bResumed;
	const bool bReleasePressed = bReleaseDown && !bReleaseKeyDown && !bResumed;
	const bool bTakeHalfPressed = bTakeHalfDown && !bTakeHalfKeyDown && !bResumed;
	bGuardKeyDown = bGuardDown;
	bInterceptKeyDown = bInterceptDown;
	bReleaseKeyDown = bReleaseDown;
	bTakeHalfKeyDown = bTakeHalfDown;
	// A flick of the left stick past 0.6: left, up or right picks card 1, 2 or 3 on its edge.
	const float StickX = bLive ? PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX) : 0.f;
	const float StickY = bLive ? PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY) : 0.f;
	const int32 StickDir = StickX < -0.6f ? 0 : StickY > 0.6f ? 1 : StickX > 0.6f ? 2 : -1;
	const bool bCardFlicked = StickDir >= 0 && StickDir != CardStickDir && !bResumed;
	CardStickDir = StickDir;
	if (!bLive || UFTPauseSubsystem::IsBlocking(this))
	{
		return;
	}
	const ftcore::Battle* B = Battle->GetBattle();
	if (Battle->IsDefenceActive())
	{
		if (bCardFlicked)
		{
			TryPickCard(StickDir);
		}
		// A hold lasts while its own key is down: Guard for a defender with the verb, Intercept when
		// a party member can take the attack for the defender (Battle ignores either otherwise, and
		// one hold at a time).
		if (bGuardPressed)
		{
			DefencePressWall = Wall();
			LastDefenceInput = EFTDefenceInput::Guard;
			Battle->Defend(EFTDefenceInput::Guard);
			GuardHolds += B->defence().guarding() && !B->defence().intercepting() ? 1 : 0;
		}
		else if (bInterceptPressed)
		{
			DefencePressWall = Wall();
			LastDefenceInput = EFTDefenceInput::Intercept;
			Battle->Defend(EFTDefenceInput::Intercept);
		}
		else if (B->defence().guarding() && !(B->defence().intercepting() ? bInterceptDown : bGuardDown))
		{
			Battle->ReleaseGuard();
		}
		return;
	}
	const bool bPartyTurn = B->phase() == ftcore::Phase::AwaitingCommand && B->combatant(B->actor()).team == ftcore::Team::Party;
	if (!bPartyTurn)
	{
		return;
	}
	if (bReleasePressed)
	{
		switch (Battle->Release())
		{
		case EFTCommandError::NoLoad: SetNotice(TEXT("No Load to release")); break;
		case EFTCommandError::NoSuchVerb: SetNotice(TEXT("Can't release")); break;
		default: break;
		}
		return;
	}
	if (!bTakeHalfPressed)
	{
		return;
	}
	// Take Half with the lowest-id living ally (another party member).
	int32 Ally = -1;
	const std::vector<ftcore::Combatant>& All = B->combatants();
	for (size_t Id = 0; Id < All.size(); ++Id)
	{
		if (Id != B->actor() && All[Id].team == ftcore::Team::Party && All[Id].alive())
		{
			Ally = static_cast<int32>(Id);
			break;
		}
	}
	switch (Battle->TakeHalf(Ally))
	{
	case EFTCommandError::NoSuchVerb: SetNotice(TEXT("Can't take half")); break;
	case EFTCommandError::InvalidTarget: SetNotice(TEXT("No ally to take half with")); break;
	case EFTCommandError::NothingToShare: SetNotice(TEXT("Nothing to share")); break;
	default: break;
	}
}

bool AFTBattleDemoActor::TryPickCard(int32 Card)
{
	const ftcore::Battle* B = Battle->GetBattle();
	// A key past the cards dealt (4, or 3 when two are dealt) is no pick: it falls through silently.
	if (!B || !Battle->IsDefenceActive() || !B->readCards().dealt() || B->readCards().picked() || Card < 0
		|| static_cast<size_t>(Card) >= B->readCards().size())
	{
		return false;
	}
	if (Battle->PickCard(Card) == EFTCommandError::WrongPhase)
	{
		SetNotice(TEXT("Too late to read"));
	}
	return true;
}

void AFTBattleDemoActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	TickShots();
	TickCameraShots();
	if (bAutoPlay)
	{
		TickAutoPlay(DeltaSeconds);
	}
	TickAimInput(DeltaSeconds);
	TickVerbInput();
	TickStickFlicks();
	// A combo that ended without a ComboFinished (the last enemy fell to a note): the lane fades at once.
	const bool bComboNow = Battle->GetBattle() && Battle->IsComboActive();
	if (bComboRunning && !bComboNow && ComboEndWall < 0.0)
	{
		ComboEndWall = Wall();
		bComboStamp = false;
	}
	bComboRunning = bComboNow;
	TickLaneShots();
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
	TickCamera();
}
