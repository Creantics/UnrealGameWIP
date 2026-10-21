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
static_assert(static_cast<int>(EFTBattlePhase::PartyAim) == static_cast<int>(ftcore::Phase::PartyAim));
static_assert(static_cast<int>(EFTBattleEventType::TurnStarted) == static_cast<int>(ftcore::EventType::TurnStarted));
static_assert(static_cast<int>(EFTBattleEventType::SkillStarted) == static_cast<int>(ftcore::EventType::SkillStarted));
static_assert(static_cast<int>(EFTBattleEventType::NoteJudged) == static_cast<int>(ftcore::EventType::NoteJudged));
static_assert(static_cast<int>(EFTBattleEventType::Hit) == static_cast<int>(ftcore::EventType::Hit));
static_assert(static_cast<int>(EFTBattleEventType::Defended) == static_cast<int>(ftcore::EventType::Defended));
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
static_assert(static_cast<int>(EFTDefenceInput::Dodge) == static_cast<int>(ftcore::DefenceInput::Dodge));
static_assert(static_cast<int>(EFTDefenceInput::Parry) == static_cast<int>(ftcore::DefenceInput::Parry));
static_assert(static_cast<int>(EFTDefenceInput::Jump) == static_cast<int>(ftcore::DefenceInput::Jump));
static_assert(static_cast<int>(EFTDefenceOutcome::Pending) == static_cast<int>(ftcore::DefenceOutcome::Pending));
static_assert(static_cast<int>(EFTDefenceOutcome::Perfect) == static_cast<int>(ftcore::DefenceOutcome::Perfect));
static_assert(static_cast<int>(EFTDefenceOutcome::Dodged) == static_cast<int>(ftcore::DefenceOutcome::Dodged));
static_assert(static_cast<int>(EFTDefenceOutcome::Jumped) == static_cast<int>(ftcore::DefenceOutcome::Jumped));
static_assert(static_cast<int>(EFTDefenceOutcome::Hit) == static_cast<int>(ftcore::DefenceOutcome::Hit));

namespace
{
// An empty list is the default single hit; otherwise at most ftcore::kMaxTimelineHits, sanitized
// the way the JSON loader would refuse them (times clamped, later hits after earlier ones).
void ToCore(const TArray<FFTAttackHit>& In, ftcore::AttackTimeline& Out)
{
	Out = ftcore::AttackTimeline();
	if (In.Num() == 0)
	{
		return;
	}
	Out.clear();
	double Previous = 0.0;
	for (const FFTAttackHit& H : In)
	{
		ftcore::TimelineHit D;
		D.at = FMath::Clamp(H.At, Previous + 0.01, ftcore::kMaxHitOffset);
		D.tell = FMath::Clamp(H.Tell, 0.0, FMath::Min(ftcore::kMaxTell, D.at));
		D.telegraph = FMath::Clamp(H.Telegraph, 0.0, FMath::Min(ftcore::kMaxTelegraph, D.at));
		D.weight = FMath::Max(H.Weight, 0.01f);
		D.jumpable = H.bJumpable;
		Previous = D.at;
		if (!Out.add(D))
		{
			break;
		}
	}
}

// At most ftcore::kMaxWeakPoints, clamped to the limits the JSON loader enforces.
void ToCore(const TArray<FFTWeakPoint>& In, float HalfWidth, ftcore::WeakPoints& Out)
{
	Out = ftcore::WeakPoints();
	Out.halfWidth = FMath::Clamp(HalfWidth, 0.05f, 1.0f);
	for (const FFTWeakPoint& P : In)
	{
		ftcore::WeakPoint D;
		D.setId(FT::ToUtf8(P.Id.ToLower()));
		D.x = FMath::Clamp(P.X, -1.0f, 1.0f);
		D.y = FMath::Clamp(P.Y, 0.0f, 1.0f);
		D.radius = FMath::Clamp(P.Radius, ftcore::kMinWeakRadius, ftcore::kMaxWeakRadius);
		D.multiplier = FMath::Clamp(P.Multiplier, ftcore::kMinWeakMultiplier, ftcore::kMaxWeakMultiplier);
		if (!Out.add(D))
		{
			break;
		}
	}
}

FFTWeakPoint FromCore(const ftcore::WeakPoint& In)
{
	FFTWeakPoint Out;
	Out.Id = FT::ToFString(In.id());
	Out.X = In.x;
	Out.Y = In.y;
	Out.Radius = In.radius;
	Out.Multiplier = In.multiplier;
	return Out;
}

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
		ToCore(S.Hits, D.timeline);
	}
	ToCore(In.WeakPoints, In.HalfWidth, Out.weakPoints);
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
	Out.recovery = FMath::Max(In.Recovery, 0.0);
	Out.lockout = FMath::Max(In.Lockout, 0.0);
	Out.windows = FT::SafeWindows(FT::ToCore(In.Windows));
	Out.weights = FT::ToCore(In.Weights);
	Out.fullComboBonus = In.FullComboBonus;
	Out.defence.dodge = FMath::Max(In.DodgeWindow, 0.0);
	Out.defence.perfectDodge = FMath::Clamp(In.PerfectDodgeWindow, 0.0, Out.defence.dodge);
	Out.defence.parry = FMath::Max(In.ParryWindow, 0.0);
	Out.defence.jump = FMath::Max(In.JumpWindow, 0.0);
	Out.defence.parryLockout = FMath::Max(In.ParryLockout, 0.0);
	Out.defence.late = FMath::Max(In.LateWindow, 0.0);
	Out.aim.apCost = FMath::Max(In.AimApCost, 0);
	Out.aim.power = FMath::Max(In.AimPower, 0.0f);
	Out.aim.timeLimit = FMath::Max(In.AimTimeLimit, 0.5);
	Out.aim.bodyMultiplier = FMath::Max(In.AimBodyMultiplier, 0.0f);
	Out.aim.grazeMultiplier = FMath::Max(In.AimGrazeMultiplier, 0.0f);
	Out.aim.swayX = FMath::Max(In.AimSwayX, 0.0f);
	Out.aim.swayY = FMath::Max(In.AimSwayY, 0.0f);
	Out.apOnPerfect = FMath::Max(In.ApOnPerfect, 0);
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

void UFTBattleComponent::Defend(EFTDefenceInput Input)
{
	if (!Battle)
	{
		return;
	}
	Battle->defend(static_cast<ftcore::DefenceInput>(Input), InputNow());
	Flush();
}

void UFTBattleComponent::DefendAtTime(EFTDefenceInput Input, double BattleSeconds)
{
	if (!Battle)
	{
		return;
	}
	Battle->defend(static_cast<ftcore::DefenceInput>(Input), BattleSeconds);
	Flush();
}

EFTCommandError UFTBattleComponent::AimAt(int32 Target)
{
	if (!Battle)
	{
		return EFTCommandError::WrongPhase;
	}
	if (!ValidId(Target))
	{
		return EFTCommandError::InvalidTarget;
	}
	const ftcore::CommandError Err = Battle->aim(static_cast<ftcore::CombatantId>(Target), Now());
	Flush();
	return static_cast<EFTCommandError>(Err);
}

void UFTBattleComponent::MoveReticle(FVector2D Delta)
{
	if (Battle)
	{
		Battle->moveReticle(static_cast<float>(Delta.X), static_cast<float>(Delta.Y));
	}
}

void UFTBattleComponent::SetReticle(FVector2D At)
{
	if (Battle)
	{
		Battle->setReticle(static_cast<float>(At.X), static_cast<float>(At.Y));
	}
}

bool UFTBattleComponent::Fire()
{
	return Battle && FireAtTime(InputNow());
}

bool UFTBattleComponent::FireAtTime(double BattleSeconds)
{
	if (!Battle)
	{
		return false;
	}
	const bool bFired = Battle->fire(BattleSeconds);
	Flush();
	return bFired;
}

bool UFTBattleComponent::CancelAim()
{
	if (!Battle)
	{
		return false;
	}
	const bool bCancelled = Battle->cancelAim(Now());
	Flush();
	return bCancelled;
}

bool UFTBattleComponent::IsAimActive() const
{
	return GetPhase() == EFTBattlePhase::PartyAim;
}

int32 UFTBattleComponent::GetAimTarget() const
{
	return IsAimActive() ? static_cast<int32>(Battle->aimTarget()) : -1;
}

FVector2D UFTBattleComponent::GetReticle() const
{
	if (!IsAimActive())
	{
		return FVector2D(0.0, 0.5);
	}
	const ftcore::AimPoint P = Battle->aimShot().reticle();
	return FVector2D(P.x, P.y);
}

FVector2D UFTBattleComponent::GetAimPoint()
{
	if (!IsAimActive())
	{
		return FVector2D(0.0, 0.5);
	}
	const ftcore::AimPoint P = Battle->aimShot().aimPoint(Now());
	return FVector2D(P.x, P.y);
}

TArray<FFTWeakPoint> UFTBattleComponent::GetAimWeakPoints() const
{
	TArray<FFTWeakPoint> Out;
	if (IsAimActive())
	{
		for (const ftcore::WeakPoint& P : Battle->aimShot().weakPoints())
		{
			Out.Add(FromCore(P));
		}
	}
	return Out;
}

double UFTBattleComponent::GetAimTimeLeft()
{
	return IsAimActive() ? FMath::Max(Battle->aimShot().deadline() - Now(), 0.0) : 0.0;
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
	// The default single hit stays an empty list, so the skill round-trips unchanged.
	if (!ftcore::sameTimeline(S.timeline, ftcore::AttackTimeline()))
	{
		for (size_t I = 0; I < S.timeline.size(); ++I)
		{
			const ftcore::TimelineHit& H = S.timeline[I];
			FFTAttackHit& D = Out.Hits.AddDefaulted_GetRef();
			D.At = H.at;
			D.Tell = H.tell;
			D.Telegraph = H.telegraph;
			D.Weight = H.weight;
			D.bJumpable = H.jumpable;
		}
	}
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
	return GetPhase() == EFTBattlePhase::PartySkill;
}

bool UFTBattleComponent::IsDefenceActive() const
{
	return GetPhase() == EFTBattlePhase::EnemySkill;
}

int32 UFTBattleComponent::GetDefender() const
{
	return IsDefenceActive() ? static_cast<int32>(Battle->defender()) : -1;
}

int32 UFTBattleComponent::GetDefenceHitCount() const
{
	return IsDefenceActive() ? static_cast<int32>(Battle->defence().size()) : 0;
}

FFTDefenceHit UFTBattleComponent::GetDefenceHit(int32 Index) const
{
	FFTDefenceHit Out;
	if (Index < 0 || Index >= GetDefenceHitCount())
	{
		return Out;
	}
	const ftcore::DefenceTimeline& D = Battle->defence();
	const size_t I = static_cast<size_t>(Index);
	Out.Impact = D.impact(I);
	Out.Tell = D.hit(I).tell;
	Out.Telegraph = D.hit(I).telegraph;
	Out.bJumpable = D.hit(I).jumpable;
	Out.Outcome = static_cast<EFTDefenceOutcome>(D.outcome(I));
	Out.bLate = D.late(I);
	return Out;
}

int32 UFTBattleComponent::GetDefenceNextHitIndex() const
{
	return IsDefenceActive() ? static_cast<int32>(Battle->defence().nextIndex()) : 0;
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
