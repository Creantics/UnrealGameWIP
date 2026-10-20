#include "FTBattleComponent.h"

#include "FTConductorComponent.h"
#include "FTText.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogFTBattle, Log, All);

static_assert(static_cast<int>(EFTTeam::Party) == static_cast<int>(ftcore::Team::Party));
static_assert(static_cast<int>(EFTTeam::Enemies) == static_cast<int>(ftcore::Team::Enemies));
static_assert(static_cast<int>(EFTTargeting::SingleEnemy) == static_cast<int>(ftcore::Targeting::SingleEnemy));
static_assert(static_cast<int>(EFTTargeting::AllEnemies) == static_cast<int>(ftcore::Targeting::AllEnemies));
static_assert(static_cast<int>(EFTBattlePhase::NotStarted) == static_cast<int>(ftcore::Phase::NotStarted));
static_assert(static_cast<int>(EFTBattlePhase::AwaitingCommand) == static_cast<int>(ftcore::Phase::AwaitingCommand));
static_assert(static_cast<int>(EFTBattlePhase::PartySkill) == static_cast<int>(ftcore::Phase::PartySkill));
static_assert(static_cast<int>(EFTBattlePhase::EnemySkill) == static_cast<int>(ftcore::Phase::EnemySkill));
static_assert(static_cast<int>(EFTBattlePhase::Recovery) == static_cast<int>(ftcore::Phase::Recovery));
static_assert(static_cast<int>(EFTBattlePhase::Victory) == static_cast<int>(ftcore::Phase::Victory));
static_assert(static_cast<int>(EFTBattlePhase::Defeat) == static_cast<int>(ftcore::Phase::Defeat));
static_assert(static_cast<int>(EFTBattleEventType::TurnStarted) == static_cast<int>(ftcore::EventType::TurnStarted));
static_assert(static_cast<int>(EFTBattleEventType::SkillStarted) == static_cast<int>(ftcore::EventType::SkillStarted));
static_assert(static_cast<int>(EFTBattleEventType::NoteJudged) == static_cast<int>(ftcore::EventType::NoteJudged));
static_assert(static_cast<int>(EFTBattleEventType::Hit) == static_cast<int>(ftcore::EventType::Hit));
static_assert(static_cast<int>(EFTBattleEventType::Blocked) == static_cast<int>(ftcore::EventType::Blocked));
static_assert(static_cast<int>(EFTBattleEventType::ComboFinished) == static_cast<int>(ftcore::EventType::ComboFinished));
static_assert(static_cast<int>(EFTBattleEventType::Counter) == static_cast<int>(ftcore::EventType::Counter));
static_assert(static_cast<int>(EFTBattleEventType::Downed) == static_cast<int>(ftcore::EventType::Downed));
static_assert(static_cast<int>(EFTBattleEventType::Victory) == static_cast<int>(ftcore::EventType::Victory));
static_assert(static_cast<int>(EFTBattleEventType::Defeat) == static_cast<int>(ftcore::EventType::Defeat));
static_assert(static_cast<int>(EFTCommandError::None) == static_cast<int>(ftcore::CommandError::None));
static_assert(static_cast<int>(EFTCommandError::WrongPhase) == static_cast<int>(ftcore::CommandError::WrongPhase));
static_assert(static_cast<int>(EFTCommandError::NoSuchSkill) == static_cast<int>(ftcore::CommandError::NoSuchSkill));
static_assert(static_cast<int>(EFTCommandError::NotEnoughAp) == static_cast<int>(ftcore::CommandError::NotEnoughAp));
static_assert(static_cast<int>(EFTCommandError::InvalidTarget) == static_cast<int>(ftcore::CommandError::InvalidTarget));

namespace
{
// Assigns in place so a same-shaped line-up reuses the strings and skill lists.
void ToCore(const FFTCombatant& In, ftcore::Combatant& Out)
{
	Out.name = FT::ToUtf8(In.Name);
	Out.team = static_cast<ftcore::Team>(In.Team);
	Out.stats.maxHp = In.Stats.MaxHp;
	Out.stats.attack = In.Stats.Attack;
	Out.stats.defense = In.Stats.Defense;
	Out.stats.speed = In.Stats.Speed;
	Out.stats.technique = In.Stats.Technique;
	Out.hp = In.Hp;
	Out.ap = In.Ap;
	Out.maxAp = In.MaxAp;
	Out.skills.resize(static_cast<size_t>(In.Skills.Num()));
	for (int32 I = 0; I < In.Skills.Num(); ++I)
	{
		const FFTSkill& S = In.Skills[I];
		ftcore::Skill& D = Out.skills[static_cast<size_t>(I)];
		D.name = FT::ToUtf8(S.Name);
		D.apCost = S.ApCost;
		D.apGain = S.ApGain;
		D.targeting = static_cast<ftcore::Targeting>(S.Targeting);
		D.power = S.Power;
		D.notes = S.Notes;
		D.difficulty = S.Difficulty;
	}
}

void ToCore(const TArray<FFTCombatant>& In, std::vector<ftcore::Combatant>& Out)
{
	Out.resize(static_cast<size_t>(In.Num()));
	for (int32 I = 0; I < In.Num(); ++I)
	{
		ToCore(In[I], Out[static_cast<size_t>(I)]);
	}
}

ftcore::BattleTuning ToCore(const FFTBattleTuning& In)
{
	ftcore::BattleTuning Out;
	Out.leadIn = FMath::Max(In.LeadIn, 0.0);
	Out.windup = FMath::Max(In.Windup, 0.0);
	Out.recovery = FMath::Max(In.Recovery, 0.0);
	Out.lockout = FMath::Max(In.Lockout, 0.0);
	Out.windows = FT::SafeWindows(FT::ToCore(In.Windows));
	Out.weights = FT::ToCore(In.Weights);
	Out.fullComboBonus = In.FullComboBonus;
	Out.counterAccuracy = In.CounterAccuracy;
	Out.counterPower = In.CounterPower;
	Out.apOnCounter = In.ApOnCounter;
	Out.startingAp = In.StartingAp;
	Out.seed = static_cast<uint64>(In.Seed);
	Out.music.startOnBar = In.bStartOnBar;
	Out.music.beatsPerBar = FMath::Max(In.BeatsPerBar, 1);
	return Out;
}

FFTBattleEvent FromCore(const ftcore::BattleEvent& E)
{
	FFTBattleEvent Out;
	Out.Type = static_cast<EFTBattleEventType>(E.type);
	Out.Source = static_cast<int32>(E.source);
	Out.Target = static_cast<int32>(E.target);
	Out.Amount = E.amount;
	Out.Grade = FT::FromCore(E.grade);
	Out.Beat = static_cast<int32>(E.beat);
	Out.Time = E.time;
	Out.Label = FT::ToFString(E.label);
	return Out;
}
} // namespace

UFTBattleComponent::UFTBattleComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false; // on while a battle runs
}

UFTBattleComponent::~UFTBattleComponent() = default;

void UFTBattleComponent::BeginPlay()
{
	Super::BeginPlay();
	if (!Conductor && GetOwner())
	{
		Conductor = GetOwner()->FindComponentByClass<UFTConductorComponent>();
	}
	if (Conductor)
	{
		AddTickPrerequisiteComponent(Conductor);
	}
}

bool UFTBattleComponent::StartBattle()
{
	if (bDispatching)
	{
		UE_LOG(LogFTBattle, Error, TEXT("%s: StartBattle from inside a battle event; call it later"), *GetPathName());
		return false;
	}
	if (Party.Num() == 0 || Enemies.Num() == 0)
	{
		UE_LOG(LogFTBattle, Error, TEXT("%s: a battle needs at least one party member and one enemy"), *GetPathName());
		return false;
	}
	for (const FFTCombatant& E : Enemies)
	{
		if (E.Skills.Num() == 0)
		{
			UE_LOG(LogFTBattle, Error, TEXT("%s: enemy '%s' has no skills"), *GetPathName(), *E.Name);
			return false;
		}
	}

	ftcore::BattleTuning CoreTuning = ToCore(Tuning);
	bUsingMusic = bSyncToMusic && Conductor != nullptr;
	if (bSyncToMusic && !Conductor)
	{
		UE_LOG(LogFTBattle, Warning, TEXT("%s: bSyncToMusic without a conductor; using the platform clock"), *GetPathName());
	}
	if (bUsingMusic)
	{
		const ftcore::ConductorConfig& Clock = Conductor->GetConductor().config();
		CoreTuning.music.bpm = Clock.bpm;
		CoreTuning.music.offset = Clock.offsetSeconds;
	}

	ToCore(Party, CoreParty);
	ToCore(Enemies, CoreEnemies);
	if (Battle)
	{
		Battle->reset(CoreParty, CoreEnemies, CoreTuning);
	}
	else
	{
		Battle = MakeUnique<ftcore::Battle>(CoreParty, CoreEnemies, CoreTuning);
	}

	Epoch = FPlatformTime::Seconds();
	LastPhase = EFTBattlePhase::NotStarted;
	Battle->start(Now());
	SetComponentTickEnabled(true);
	Flush();
	return true;
}

double UFTBattleComponent::Now()
{
	return bUsingMusic ? Conductor->GetSongTime() : FPlatformTime::Seconds() - Epoch;
}

double UFTBattleComponent::InputNow() const
{
	return bUsingMusic ? Conductor->GetInputSongTime() : FPlatformTime::Seconds() - Epoch;
}

double UFTBattleComponent::GetBattleTime()
{
	return Battle ? Now() : 0.0;
}

EFTCommandError UFTBattleComponent::ChooseSkill(int32 SkillIndex, int32 Target)
{
	if (!Battle)
	{
		return EFTCommandError::WrongPhase;
	}
	if (SkillIndex < 0)
	{
		return EFTCommandError::NoSuchSkill;
	}
	// Area skills ignore the target, so a negative one only matters for single-target skills.
	const ftcore::CombatantId CoreTarget = ValidId(Target) ? static_cast<ftcore::CombatantId>(Target)
	                                                       : static_cast<ftcore::CombatantId>(Battle->combatants().size());
	const ftcore::CommandError Err = Battle->command(static_cast<size_t>(SkillIndex), CoreTarget, Now());
	Flush();
	return static_cast<EFTCommandError>(Err);
}

void UFTBattleComponent::Press(EFTArrow Arrow)
{
	if (!Battle)
	{
		return;
	}
	Battle->press(FT::ToCore(Arrow), InputNow());
	Flush();
}

void UFTBattleComponent::PressAtTime(EFTArrow Arrow, double BattleSeconds)
{
	if (!Battle)
	{
		return;
	}
	Battle->press(FT::ToCore(Arrow), BattleSeconds);
	Flush();
}

void UFTBattleComponent::Flush()
{
	if (bDispatching)
	{
		return; // a handler acted: the loop below picks its events up
	}
	bDispatching = true;
	// By index, re-reading the span: a handler may choose a skill or press, appending events.
	for (size_t I = 0; I < Battle->events().size(); ++I)
	{
		const FFTBattleEvent E = FromCore(Battle->events()[I]);
		OnBattleEvent.Broadcast(E);
		switch (E.Type)
		{
		case EFTBattleEventType::TurnStarted:
			OnTurnStarted.Broadcast(E.Source);
			break;
		case EFTBattleEventType::Victory:
			OnOutcome.Broadcast(EFTBattleOutcome::Victory);
			break;
		case EFTBattleEventType::Defeat:
			OnOutcome.Broadcast(EFTBattleOutcome::Defeat);
			break;
		default:
			break;
		}
	}
	Battle->clearEvents();
	bDispatching = false;

	const EFTBattlePhase Phase = GetPhase();
	if (Phase != LastPhase)
	{
		LastPhase = Phase;
		OnPhaseChanged.Broadcast(Phase);
	}
	if (IsBattleOver())
	{
		SetComponentTickEnabled(false);
	}
}

void UFTBattleComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Battle || IsBattleOver())
	{
		return;
	}
	Battle->update(Now());
	Flush();
}

bool UFTBattleComponent::ValidId(int32 Id) const
{
	return Battle && Id >= 0 && static_cast<size_t>(Id) < Battle->combatants().size();
}

EFTBattlePhase UFTBattleComponent::GetPhase() const
{
	return Battle ? static_cast<EFTBattlePhase>(Battle->phase()) : EFTBattlePhase::NotStarted;
}

int32 UFTBattleComponent::GetActor() const
{
	return GetPhase() == EFTBattlePhase::NotStarted ? -1 : static_cast<int32>(Battle->actor());
}

bool UFTBattleComponent::IsBattleOver() const
{
	const EFTBattlePhase Phase = GetPhase();
	return Phase == EFTBattlePhase::Victory || Phase == EFTBattlePhase::Defeat;
}

int32 UFTBattleComponent::GetCombatantCount() const
{
	return Battle ? static_cast<int32>(Battle->combatants().size()) : 0;
}

FFTCombatantState UFTBattleComponent::GetCombatant(int32 Id) const
{
	FFTCombatantState Out;
	if (!ValidId(Id))
	{
		return Out;
	}
	const ftcore::Combatant& C = Battle->combatant(static_cast<ftcore::CombatantId>(Id));
	Out.Id = Id;
	Out.Name = FT::ToFString(C.name);
	Out.Team = static_cast<EFTTeam>(C.team);
	Out.Hp = C.hp;
	Out.MaxHp = C.stats.maxHp;
	Out.Ap = C.ap;
	Out.MaxAp = C.maxAp;
	Out.bAlive = C.alive();
	return Out;
}

int32 UFTBattleComponent::GetSkillCount(int32 Id) const
{
	return ValidId(Id) ? static_cast<int32>(Battle->combatant(static_cast<ftcore::CombatantId>(Id)).skills.size()) : 0;
}

FFTSkill UFTBattleComponent::GetSkill(int32 Id, int32 SkillIndex) const
{
	FFTSkill Out;
	if (SkillIndex < 0 || SkillIndex >= GetSkillCount(Id))
	{
		return Out;
	}
	const ftcore::Skill& S = Battle->combatant(static_cast<ftcore::CombatantId>(Id)).skills[static_cast<size_t>(SkillIndex)];
	Out.Name = FT::ToFString(S.name);
	Out.ApCost = S.apCost;
	Out.ApGain = S.apGain;
	Out.Targeting = static_cast<EFTTargeting>(S.targeting);
	Out.Power = S.power;
	Out.Notes = S.notes;
	Out.Difficulty = S.difficulty;
	return Out;
}

TArray<int32> UFTBattleComponent::GetLiving(EFTTeam Team) const
{
	TArray<int32> Out;
	if (!Battle)
	{
		return Out;
	}
	const ftcore::Team CoreTeam = static_cast<ftcore::Team>(Team);
	const std::vector<ftcore::Combatant>& All = Battle->combatants();
	for (size_t I = 0; I < All.size(); ++I)
	{
		if (All[I].team == CoreTeam && All[I].alive())
		{
			Out.Add(static_cast<int32>(I));
		}
	}
	return Out;
}

int32 UFTBattleComponent::GetLivingCount(EFTTeam Team) const
{
	return Battle ? static_cast<int32>(Battle->livingCount(static_cast<ftcore::Team>(Team))) : 0;
}

std::span<const ftcore::CombatantId> UFTBattleComponent::PeekTurnOrder(size_t Count) const
{
	if (!Battle)
	{
		return {};
	}
	const size_t N = Battle->turnOrder(std::span<ftcore::CombatantId>(OrderScratch.data(), std::min(Count, OrderScratch.size())));
	return std::span<const ftcore::CombatantId>(OrderScratch.data(), N);
}

TArray<int32> UFTBattleComponent::GetTurnOrder(int32 Count) const
{
	TArray<int32> Out;
	const std::span<const ftcore::CombatantId> Order = PeekTurnOrder(static_cast<size_t>(FMath::Max(Count, 0)));
	Out.Reserve(static_cast<int32>(Order.size()));
	for (const ftcore::CombatantId Id : Order)
	{
		Out.Add(static_cast<int32>(Id));
	}
	return Out;
}

bool UFTBattleComponent::IsComboActive() const
{
	const EFTBattlePhase Phase = GetPhase();
	return Phase == EFTBattlePhase::PartySkill || Phase == EFTBattlePhase::EnemySkill;
}

FString UFTBattleComponent::GetActiveSkillName() const
{
	const ftcore::Skill* Skill = Battle ? Battle->activeSkill() : nullptr;
	return Skill ? FT::ToFString(Skill->name) : FString();
}

TArray<int32> UFTBattleComponent::GetActiveTargets() const
{
	TArray<int32> Out;
	if (Battle)
	{
		for (const ftcore::CombatantId Id : Battle->activeTargets())
		{
			Out.Add(static_cast<int32>(Id));
		}
	}
	return Out;
}

int32 UFTBattleComponent::GetComboNoteCount() const
{
	return Battle ? static_cast<int32>(Battle->combo().size()) : 0;
}

FFTComboNote UFTBattleComponent::GetComboNote(int32 Index) const
{
	if (Index < 0 || Index >= GetComboNoteCount())
	{
		return FFTComboNote();
	}
	const ftcore::ComboSequence& Combo = Battle->combo();
	const size_t I = static_cast<size_t>(Index);
	return FT::FromCore(Combo.note(I), Combo.grade(I));
}

int32 UFTBattleComponent::GetComboNextNoteIndex() const
{
	return Battle ? static_cast<int32>(Battle->combo().nextIndex()) : 0;
}

float UFTBattleComponent::GetComboAccuracy() const
{
	return Battle ? Battle->combo().accuracy() : 0.0f;
}

float UFTBattleComponent::GetComboRating() const
{
	return Battle ? Battle->comboParams().rating : 0.0f;
}
