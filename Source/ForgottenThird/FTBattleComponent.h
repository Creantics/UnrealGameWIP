#pragma once

// A turn-based battle: party moves are arrow combos, enemy moves are attack timelines the defender
// dodges, parries or jumps (ftcore::Battle, initiative from ftcore::TurnQueue, rules kept in Core).
// This holds the line-up, runs the battle on one clock and turns Core's battle events into
// Blueprint events.
//
// Use:
//  1. Fill Party and Enemies (every enemy needs at least one skill) and Tuning, then StartBattle().
//  2. On OnTurnStarted for a party member (phase AwaitingCommand), show its skills
//     (GetSkillCount / GetSkill) and call ChooseSkill(SkillIndex, Target). Area skills ignore Target.
//     Enemy turns start on their own.
//  3. While the phase is PartySkill, forward arrow input to Press(Arrow). Draw the chart from
//     GetComboNote(i) against GetBattleTime(); notes are in battle-clock seconds.
//     While the phase is EnemySkill, forward Dodge / Parry / Jump to Defend(Input). Draw the attack's
//     tells from GetDefenceHit(i) (impact times in battle-clock seconds).
//  4. OnBattleEvent reports every hit, defended attack hit, judged note, counter and downed combatant;
//     OnPhaseChanged follows the phase; OnOutcome fires once with Victory or Defeat.
//
// Clock: with bSyncToMusic the battle runs on the conductor's song time (Conductor, or the owner's
// UFTConductorComponent), presses are judged at its latency-compensated input time and charts sit on
// its beat grid. Otherwise it runs on FPlatformTime::Seconds() from StartBattle.
//
// Combatant ids are indexes: the party first (in Party order), then the enemies.

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "FTComboComponent.h"

#include "ftcore/Battle.h"

#include <array>
#include <span>
#include <vector>

#include "FTBattleComponent.generated.h"

class UFTConductorComponent;

UENUM(BlueprintType)
enum class EFTTeam : uint8
{
	Party,
	Enemies,
};

UENUM(BlueprintType)
enum class EFTTargeting : uint8
{
	SingleEnemy,
	AllEnemies,
};

// Same order as ftcore::Phase.
UENUM(BlueprintType)
enum class EFTBattlePhase : uint8
{
	NotStarted,
	// A party member is choosing a skill.
	AwaitingCommand,
	// A party member performs an attack combo.
	PartySkill,
	// An enemy attack timeline: the defender dodges, parries or jumps each hit.
	EnemySkill,
	Recovery,
	Victory,
	Defeat,
};

// Same order as ftcore::EventType.
UENUM(BlueprintType)
enum class EFTBattleEventType : uint8
{
	TurnStarted,
	SkillStarted,
	// One combo note resolved: Grade, Beat = note index, Label "WRONG" for a wrong arrow.
	NoteJudged,
	// Damage dealt (Amount) by the note at Beat.
	Hit,
	// Enemy turn: attack hit Beat resolved. Label "PERFECT" (a perfect parry or dodge), "DODGE",
	// "JUMP" or "HIT"; Amount = AP refunded.
	Defended,
	// Party combo: Amount = accuracy in percent; Label "FULL COMBO" when nothing was missed.
	ComboFinished,
	// The defender parried every hit and strikes back.
	Counter,
	Downed,
	Victory,
	Defeat,
};

// Same order as ftcore::CommandError.
UENUM(BlueprintType)
enum class EFTCommandError : uint8
{
	None,
	WrongPhase,
	NoSuchSkill,
	NotEnoughAp,
	InvalidTarget,
};

// Same order as ftcore::DefenceInput.
UENUM(BlueprintType)
enum class EFTDefenceInput : uint8
{
	// Wide window, no lockout. Can't avoid a jumpable hit.
	Dodge,
	// Narrow window; a miss locks parry out for a moment.
	Parry,
	// Jumpable hits only.
	Jump,
};

// Same order as ftcore::DefenceOutcome.
UENUM(BlueprintType)
enum class EFTDefenceOutcome : uint8
{
	Pending,
	Perfect,
	Dodged,
	Jumped,
	Hit,
};

UENUM(BlueprintType)
enum class EFTBattleOutcome : uint8
{
	Victory,
	Defeat,
};

USTRUCT(BlueprintType)
struct FFTStats
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 MaxHp = 100;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 Attack = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 Defense = 5;
	// Initiative: faster combatants act more often.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 Speed = 10;
	// Higher than the opponent's makes this combatant's combos easier, attacking or defending.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 Technique = 10;
};

// One hit of an enemy skill's attack timeline (see ftcore/DefenceTimeline.h).
USTRUCT(BlueprintType)
struct FFTAttackHit
{
	GENERATED_BODY()

	// Seconds after the skill's banner.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double At = 1.2;
	// Tell cue this long before impact (0: none).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double Tell = 0.4;
	// World-space telegraph this long before impact (0: none).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double Telegraph = 0.0;
	// Share of the skill's damage, relative to the other hits.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	float Weight = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	bool bJumpable = false;
};

USTRUCT(BlueprintType)
struct FFTSkill
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	FString Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 ApCost = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 ApGain = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	EFTTargeting Targeting = EFTTargeting::SingleEnemy;
	// Damage multiplier for the whole move at 100% accuracy.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	float Power = 1.0f;
	// Combo length before difficulty adds more.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 Notes = 4;
	// Move complexity on the 0-10 combo rating scale.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	float Difficulty = 2.0f;
	// Enemy skills: the attack timeline, at most 8 hits sorted by At. Empty: one default hit 1.2 s
	// after the banner.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	TArray<FFTAttackHit> Hits;
};

// A hit of the attack being defended.
USTRUCT(BlueprintType)
struct FFTDefenceHit
{
	GENERATED_BODY()

	// Battle-clock seconds.
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	double Impact = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	double Tell = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	double Telegraph = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	bool bJumpable = false;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	EFTDefenceOutcome Outcome = EFTDefenceOutcome::Pending;
};

USTRUCT(BlueprintType)
struct FFTCombatant
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	FString Name;
	// Set by the battle from the list the combatant is in.
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	EFTTeam Team = EFTTeam::Party;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	FFTStats Stats;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 Hp = 100;
	// Party members start with Tuning.StartingAp instead.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 Ap = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 MaxAp = 9;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	TArray<FFTSkill> Skills;
};

// A combatant during the battle, without the skill list.
USTRUCT(BlueprintType)
struct FFTCombatantState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	int32 Id = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	EFTTeam Team = EFTTeam::Party;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	int32 Hp = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	int32 MaxHp = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	int32 Ap = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	int32 MaxAp = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	bool bAlive = false;
};

USTRUCT(BlueprintType)
struct FFTBattleTuning
{
	GENERATED_BODY()

	// Seconds between choosing a skill and its first note.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double LeadIn = 1.0;
	// Pause after a skill resolves before the next turn.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double Recovery = 0.7;
	// Input is ignored this long after a stray press.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double Lockout = 0.25;
	// Base windows, scaled by each combo's difficulty.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	FFTJudgeWindows Windows;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	FFTGradeWeights Weights;
	// Attack: extra damage (fraction of full) when no note was missed.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	float FullComboBonus = 0.25f;
	// Defence windows in seconds, each ending at the impact.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double DodgeWindow = 0.30;
	// The last part of the dodge window: a perfect dodge.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double PerfectDodgeWindow = 0.06;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double ParryWindow = 0.12;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double JumpWindow = 0.25;
	// After a parry press that answers nothing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	double ParryLockout = 0.40;
	// AP refunded per perfect parry or perfect dodge.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 ApOnPerfect = 1;
	// A fully parried attack is countered at this power.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	float CounterPower = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 ApOnCounter = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 StartingAp = 2;
	// Combo charts are generated deterministically from this.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int64 Seed = 0x5EEDC0DE;
	// Music sync only: start each chart on a bar line instead of any beat.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	bool bStartOnBar = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	int32 BeatsPerBar = 4;
};

USTRUCT(BlueprintType)
struct FFTBattleEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	EFTBattleEventType Type = EFTBattleEventType::TurnStarted;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	int32 Source = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	int32 Target = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	int32 Amount = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	EFTGrade Grade = EFTGrade::Pending;
	// Index of the combo note (or attack hit) the event belongs to.
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	int32 Beat = 0;
	// Battle-clock seconds.
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	double Time = 0.0;
	// A skill name, "WRONG", "FULL COMBO", a defence outcome or empty.
	UPROPERTY(BlueprintReadOnly, Category = "Battle")
	FString Label;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTBattleEventSignature, const FFTBattleEvent&, Event);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTBattleTurnSignature, int32, Actor);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTBattlePhaseSignature, EFTBattlePhase, Phase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTBattleOutcomeSignature, EFTBattleOutcome, Outcome);

UCLASS(ClassGroup = (ForgottenThird), meta = (BlueprintSpawnableComponent))
class FORGOTTENTHIRD_API UFTBattleComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFTBattleComponent();
	virtual ~UFTBattleComponent() override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	TArray<FFTCombatant> Party;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	TArray<FFTCombatant> Enemies;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	FFTBattleTuning Tuning;

	// Run on the conductor's song time and beat grid (see the header comment).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	bool bSyncToMusic = false;
	// The song clock for bSyncToMusic. None: the owner's UFTConductorComponent, found at BeginPlay.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle")
	TObjectPtr<UFTConductorComponent> Conductor;

	// Starts a battle with Party / Enemies / Tuning, replacing any running one. False (and logged)
	// when the line-up can't fight: no one on a side, or an enemy without skills.
	UFUNCTION(BlueprintCallable, Category = "Battle")
	bool StartBattle();

	// The acting party member uses skill SkillIndex on Target (ignored by area skills).
	UFUNCTION(BlueprintCallable, Category = "Battle")
	EFTCommandError ChooseSkill(int32 SkillIndex, int32 Target);
	// An arrow pressed now, during PartySkill.
	UFUNCTION(BlueprintCallable, Category = "Battle")
	void Press(EFTArrow Arrow);
	// A dodge / parry / jump pressed now, during EnemySkill.
	UFUNCTION(BlueprintCallable, Category = "Battle")
	void Defend(EFTDefenceInput Input);

	UFUNCTION(BlueprintPure, Category = "Battle")
	EFTBattlePhase GetPhase() const;
	// Whose turn it is (-1 before the battle starts).
	UFUNCTION(BlueprintPure, Category = "Battle")
	int32 GetActor() const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	bool IsBattleOver() const;
	// Seconds on the battle's clock now (song time with bSyncToMusic).
	UFUNCTION(BlueprintCallable, Category = "Battle")
	double GetBattleTime();

	UFUNCTION(BlueprintPure, Category = "Battle")
	int32 GetCombatantCount() const;
	// Id's current state (Id -1 when there is no such combatant).
	UFUNCTION(BlueprintPure, Category = "Battle")
	FFTCombatantState GetCombatant(int32 Id) const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	int32 GetSkillCount(int32 Id) const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	FFTSkill GetSkill(int32 Id, int32 SkillIndex) const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	TArray<int32> GetLiving(EFTTeam Team) const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	int32 GetLivingCount(EFTTeam Team) const;
	// The next Count actors from the turn queue, soonest first. At most 32.
	UFUNCTION(BlueprintPure, Category = "Battle")
	TArray<int32> GetTurnOrder(int32 Count = 6) const;

	// ---- the active party combo (PartySkill) ----

	UFUNCTION(BlueprintPure, Category = "Battle")
	bool IsComboActive() const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	FString GetActiveSkillName() const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	TArray<int32> GetActiveTargets() const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	int32 GetComboNoteCount() const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	FFTComboNote GetComboNote(int32 Index) const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	int32 GetComboNextNoteIndex() const;
	// 0..1 over the whole chart.
	UFUNCTION(BlueprintPure, Category = "Battle")
	float GetComboAccuracy() const;
	// The combo's 0-10 difficulty rating.
	UFUNCTION(BlueprintPure, Category = "Battle")
	float GetComboRating() const;

	// ---- the enemy attack being defended (EnemySkill) ----

	UFUNCTION(BlueprintPure, Category = "Battle")
	bool IsDefenceActive() const;
	// The party member defending (-1 outside EnemySkill).
	UFUNCTION(BlueprintPure, Category = "Battle")
	int32 GetDefender() const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	int32 GetDefenceHitCount() const;
	UFUNCTION(BlueprintPure, Category = "Battle")
	FFTDefenceHit GetDefenceHit(int32 Index) const;
	// The earliest hit still pending (GetDefenceHitCount() when none).
	UFUNCTION(BlueprintPure, Category = "Battle")
	int32 GetDefenceNextHitIndex() const;

	// Every event from Core, in order.
	UPROPERTY(BlueprintAssignable, Category = "Battle")
	FFTBattleEventSignature OnBattleEvent;
	// A combatant's turn begins (party members then wait for ChooseSkill).
	UPROPERTY(BlueprintAssignable, Category = "Battle")
	FFTBattleTurnSignature OnTurnStarted;
	UPROPERTY(BlueprintAssignable, Category = "Battle")
	FFTBattlePhaseSignature OnPhaseChanged;
	UPROPERTY(BlueprintAssignable, Category = "Battle")
	FFTBattleOutcomeSignature OnOutcome;

	// An arrow pressed at BattleSeconds on the battle clock (GetBattleTime scale) rather than now:
	// an autopilot steps on a note exactly on time even when a frame is long (time dilation).
	void PressAtTime(EFTArrow Arrow, double BattleSeconds);
	// A defence press at BattleSeconds (the autopilot answers each hit at its ideal time).
	void DefendAtTime(EFTDefenceInput Input, double BattleSeconds);

	// Native access (null before the first StartBattle).
	const ftcore::Battle* GetBattle() const { return Battle.Get(); }
	// Allocation-free turn order for native per-frame code; valid until the next call.
	std::span<const ftcore::CombatantId> PeekTurnOrder(size_t Count) const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

private:
	bool ValidId(int32 Id) const;
	double Now();          // the battle clock
	double InputNow() const; // the battle clock for an input handled now
	void Flush();          // broadcasts Core's events, then a phase change

	TUniquePtr<ftcore::Battle> Battle;
	std::vector<ftcore::Combatant> CoreParty;
	std::vector<ftcore::Combatant> CoreEnemies;
	mutable std::array<ftcore::CombatantId, ftcore::TurnQueue::kPreviewStack> OrderScratch{};
	EFTBattlePhase LastPhase = EFTBattlePhase::NotStarted;
	bool bUsingMusic = false;
	double Epoch = 0.0;
	bool bDispatching = false;
};
