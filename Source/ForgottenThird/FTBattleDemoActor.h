#pragma once

// The playable battle demo: two party members (the leads, from ftcore/BattleDemo.h) against one to
// three enemies, with the rules in Core (ftcore::Battle) and this actor as the stage.
//
// The enemy line-up comes from data: with a story carry (UFTGameInstance::StoryCarry) the entry of
// Data/Battle/encounters.json for the carry's scene, beat and step, else the table's "default"
// (the original demo fight); stats and skills come from Data/Battle/enemies.json, weak points from
// Data/Battle/weakpoints.json (optional: without it enemies have none). If the files can't be read,
// the built-in demo line-up is used. Every round (first and each retry) rebuilds
// both sides from that line-up, so a retry starts at battle-start HP.
//
// It owns the battle and its song clock (a silent 120 BPM wall clock: no audio is bound), places
// the figures (the lead characters Devin and Duy looping their idle clips, placeholder cube enemies)
// that flash or squash on hits and fall over when downed, frames the fight with its camera and turns the player controller's battle
// input into commands and presses. AFTBattleHUD draws the UI from the getters below.
//
// Command menu (phase AwaitingCommand): Up/Down pick the skill, Left/Right or Tab/Q/E the target,
// Enter/Space/pad A use it; 1-4 use a skill at once. The row after the skills is Aim (also F, the
// right mouse button or pad LT): the reticle moves with the mouse, either stick or the held arrows,
// the left mouse button / Enter / Space / pad A fire and Esc / right mouse / pad B / LT cancel (the
// AP comes back); see docs/BATTLE_AIM.md. During a combo every arrow is a step. On an enemy turn the
// defender answers each hit of the attack timeline: Left/Right (pad X/B) dodge, Down (pad A) parries,
// Up (pad Y) jumps (jumpable hits only). The attacker flashes as each hit's tell starts, and each hit
// is judged on one line: PERFECT, DEFENDED, LATE (a press just after the impact) or MISSED.
// After the outcome the screen shows the results (time, damage, notes, defences; a story win adds the
// xp, gold and level). Enter/Space/pad A/Start then fight again, or after a won story or field fight
// continue back there (the autopilot continues on its own after 2.5 s). After a loss in a story fight, Esc / Q /
// pad B quits to the title instead: it opens the story map (carry.ReturnMap) with the URL option
// ?Title and leaves the carry's bDone false.
//
// bAutoPlay (or -BattleAutoPlay on the command line) lets Core's pilot play perfectly (every note
// on time, every enemy hit parried at its ideal time from the timeline, and an aimed shot on the best
// weak point whenever ftcore::demo::pilotChoice prefers it to a skill); with
// -BattleQuitOnOutcome the game exits when the battle ends (a headless end-to-end run). With a carry
// the autopilot always retries after a loss. -BattleEncounter=<scene>,<beat>,<step> plays that entry
// of encounters.json without a story carry (a test hook: the win pays nothing). A story fight on autoplay with -AlphaFast runs at global
// time dilation 8 and its song clock follows game time (UFTConductorComponent::bWallClockFollowsGameTime).

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "FTBattleComponent.h"
#include "FTComboComponent.h"

#include "ftcore/Encounter.h"
#include "ftcore/WeakPoint.h"

#include <vector>

#include "FTBattleDemoActor.generated.h"

class AFTPlayerController;
class UCameraComponent;
class UFTConductorComponent;
class UMaterialInstanceDynamic;
class UAnimSequence;
class UMaterialInterface;
class USkeletalMesh;
class USkeletalMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;

// How the last enemy hit was answered (AFTBattleDemoActor::GetJudgement).
enum class EFTJudgement : uint8
{
	None,
	Perfect,  // a perfect parry or dodge
	Defended, // a dodge or jump
	Late,     // the hit landed and a press came just after it
	Missed,   // the hit landed
};

UCLASS()
class FORGOTTENTHIRD_API AFTBattleDemoActor : public AActor
{
	GENERATED_BODY()

public:
	AFTBattleDemoActor();

	static constexpr int32 MaxPopups = 12;
	static constexpr int32 MaxResultLines = 6;
	static constexpr int32 StatResultLines = 4; // the battle totals; a story win adds the reward after them

	// The Core pilot chooses skills and steps on every note on time.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle Demo")
	bool bAutoPlay = false;
	// Exit the game when the battle ends (for headless runs).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle Demo")
	bool bQuitOnOutcome = false;

	// Party member i is PartyMeshes[i] looping PartyIdles[i]. A slot without a mesh falls back to
	// the cylinder-and-sphere stand-in below.
	UPROPERTY(EditAnywhere, Category = "Battle Demo|Look")
	TArray<TObjectPtr<USkeletalMesh>> PartyMeshes;
	UPROPERTY(EditAnywhere, Category = "Battle Demo|Look")
	TArray<TObjectPtr<UAnimSequence>> PartyIdles;
	UPROPERTY(EditAnywhere, Category = "Battle Demo|Look")
	TObjectPtr<UStaticMesh> PartyBodyMesh;
	UPROPERTY(EditAnywhere, Category = "Battle Demo|Look")
	TObjectPtr<UStaticMesh> PartyHeadMesh;
	UPROPERTY(EditAnywhere, Category = "Battle Demo|Look")
	TObjectPtr<UStaticMesh> EnemyMesh;
	// Needs a vector parameter named "Color".
	UPROPERTY(EditAnywhere, Category = "Battle Demo|Look")
	TObjectPtr<UMaterialInterface> FigureMaterial;

	UFUNCTION(BlueprintCallable, Category = "Battle Demo")
	void StartRound();

	// ---- for the HUD (no allocation) ----

	UFTBattleComponent* GetBattle() const { return Battle; }
	UFTConductorComponent* GetConductor() const { return Conductor; }
	// The selected menu row: a skill index, or GetAimRow() for Aim.
	int32 GetSelectedSkill() const { return SelectedSkill; }
	// The Aim row of the acting party member's menu (its skill count).
	int32 GetAimRow() const;
	const FString& GetAimLabel() const { return AimLabel; }
	const FString& GetAimCost() const { return AimCost; }
	// "x2.5" style label of weak point I of the enemy being aimed at.
	const FString& GetWeakPointLabel(int32 I) const;
	int32 GetSelectedTarget() const { return SelectedTarget; }
	const FString& GetFighterName(int32 Id) const;
	// "1  Strike" style label of a skill of combatant Id, and "2 AP" style cost.
	const FString& GetSkillLabel(int32 Id, int32 Skill) const;
	const FString& GetSkillCost(int32 Id, int32 Skill) const;
	const FString& GetHpText(int32 Id) const;
	const FString& GetBanner() const { return Banner; }
	double GetBannerAge() const;
	const FString& GetNotice() const { return Notice; }
	double GetNoticeAge() const;
	// An enemy entering a new phase of its fight ("Brass Regent enters Overdrive!").
	const FString& GetPhaseBanner() const { return PhaseBanner; }
	double GetPhaseBannerAge() const;
	// The name of the skill enemy Id uses on its next turn (empty for a party member).
	const FString& GetNextSkillName(int32 Id) const;
	// The results after the outcome (none before it), then what a press does next (empty: the
	// HUD's own fight-again or retry lines).
	int32 GetResultLineCount() const { return ResultLineCount; }
	const FString& GetResultLine(int32 I) const { return ResultLines[I]; }
	const FString& GetContinuePrompt() const { return ContinuePrompt; }
	bool IsWaitingForRestart() const { return bWaitingForRestart; }
	// After a loss in a story fight: the screen offers Retry and Quit to title.
	bool CanQuitToTitle() const { return bCanQuitToTitle; }
	// World position above a combatant's figure (for markers and numbers).
	FVector GetFigureTop(int32 Id) const;
	// World position of a point in figure space (ftcore/WeakPoint.h: X across in figure heights,
	// screen-right positive; Y up from the feet).
	FVector GetFigurePoint(int32 Id, float X, float Y) const;
	float GetFigureHeight(int32 Id) const;
	// 0..1, high right after the figure was hit.
	float GetFlash(int32 Id) const;
	// Seconds since the player last pressed this lane during a combo.
	double GetLanePressAge(int32 Lane) const;
	// Seconds since the last defence press on an enemy turn, and which input it was.
	double GetDefencePressAge() const;
	EFTDefenceInput GetLastDefenceInput() const { return LastDefenceInput; }

	// The judgement of the last enemy hit defended (or not), shown on one line under the pips.
	EFTJudgement GetJudgement() const { return Judgement; }
	const FString& GetJudgementText() const { return JudgementText; }
	double GetJudgementAge() const;

	struct FPopup
	{
		double WallStart = -1.0;
		// Grade popup: the lane of the judged note. Number popup (damage, or a defence outcome with
		// bDefence and Grade Perfect / Good / Miss): the combatant the text is over.
		int32 Lane = 0;
		int32 Combatant = -1;
		EFTGrade Grade = EFTGrade::Pending;
		bool bWrong = false;
		bool bNumber = false;
		bool bDefence = false;
		bool bWeak = false; // an aimed shot on a weak point (drawn gold)
		bool bHeal = false; // HP an enemy healed entering a phase (drawn green)
		FString Text; // numbers only, built when the event arrives
	};
	const FPopup& GetPopup(int32 I) const { return Popups[I]; }

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	struct FFigure
	{
		TObjectPtr<USceneComponent> Pivot;
		TObjectPtr<UStaticMeshComponent> Body;
		TObjectPtr<UStaticMeshComponent> Head;
		TObjectPtr<USkeletalMeshComponent> Skin;
		TObjectPtr<UMaterialInstanceDynamic> Material;
		FLinearColor Color = FLinearColor::White;
		FVector Home = FVector::ZeroVector;
		float Height = 150.f;
		float FallSign = 1.f;
		double HitWall = -100.0;
		double LungeWall = -100.0;
		double DownWall = -1.0;
	};

	// Picks the line-up (carry entry or default; the built-in demo when the data can't load) into
	// CoreParty / CoreFoes.
	void LoadLineup();
	// Copies CoreParty / CoreFoes into the battle component: full HP, the state at battle start.
	void ApplyLineup();
	void BuildFigures();
	void ResetFigures();
	void CacheText();
	void UpdateHpText(int32 Id);
	void TickFigures(double Wall);
	void TickAutoPlay(float DeltaSeconds);
	// The player's reticle and the aim keys that are polled (mouse buttons, F, pad LT, sticks).
	void TickAimInput(float DeltaSeconds);
	void UseSelected();
	void EnterAim();
	void SetJudgement(EFTJudgement Kind, const FString& Text);
	void CycleTarget(int32 Step);
	void EnsureTarget();
	void AddPopup(const FPopup& P);
	void SetBanner(const FString& Text);
	void SetNotice(const FString& Text);
	// The next skill of every enemy, after anything that moves a pattern on.
	void RefreshNextSkills();
	// Fills the results panel from the battle's totals and logs them.
	void BuildResults();
	void AddResultLine(const FString& Line);
	// After a won story or field fight: back there (the timer, or a press for a human player).
	void Continue();
	AFTPlayerController* GetPlayer() const;

	UFUNCTION()
	void HandleBattleEvent(const FFTBattleEvent& Event);
	UFUNCTION()
	void HandleTurnStarted(int32 Actor);
	UFUNCTION()
	void HandleOutcome(EFTBattleOutcome Outcome);
	// Opens the exploration map this fight was started from (see UFTGameInstance::ExploreCarry).
	void ReturnToField();
	void ReturnToStory();
	// The story map with ?Title (a lost story fight); the carry stays unfinished.
	UFUNCTION()
	void HandleQuit();
	// The autopilot's answer to a loss.
	void RetryAfterDefeat();
	UFUNCTION()
	void HandleArrow(EFTArrow Arrow, bool bFaceButton);
	UFUNCTION()
	void HandleSkill(int32 Skill);
	UFUNCTION()
	void HandleTarget(int32 Step);
	UFUNCTION()
	void HandleConfirm();
	UFUNCTION()
	void HandleRestart();

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFTBattleComponent> Battle;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFTConductorComponent> Conductor;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

	// Runtime figure components (kept alive by the actor's component list; mirrored here for GC).
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> FigureObjects;
	TArray<FFigure> Figures;

	TArray<FString> Names;
	TArray<FString> HpTexts;
	TArray<int32> HpShown;
	TArray<TArray<FString>> SkillLabels;
	TArray<TArray<FString>> SkillCosts;
	FString Banner;
	double BannerWall = -100.0;
	FString Notice;
	double NoticeWall = -100.0;
	FString PhaseBanner;
	double PhaseBannerWall = -100.0;
	TArray<TArray<FString>> SkillNames;
	TArray<int32> NextSkill;
	FString ResultLines[MaxResultLines];
	int32 ResultLineCount = 0;
	FString ContinuePrompt;

	FPopup Popups[MaxPopups];
	int32 NextPopup = 0;
	double LanePressWall[4] = {-100.0, -100.0, -100.0, -100.0};
	double DefencePressWall = -100.0;
	EFTDefenceInput LastDefenceInput = EFTDefenceInput::Parry;
	EFTJudgement Judgement = EFTJudgement::None;
	FString JudgementText;
	double JudgementWall = -100.0;

	FString AimLabel;
	FString AimCost;
	FString WeakPointLabels[ftcore::kMaxWeakPoints];
	// Press edges of the polled aim keys.
	bool bAimKeyDown = false;
	bool bFireKeyDown = false;
	bool bCancelKeyDown = false;
	// The autopilot's weak point for the aim in progress.
	size_t PilotWeakPoint = ftcore::kMaxWeakPoints;

	int32 SelectedSkill = 0;
	int32 SelectedTarget = -1;
	bool bWaitingForRestart = false;
	bool bReturningToStory = false; // a story fight was won; no restart
	bool bReturnToField = false;    // ...or a field fight: Continue opens the field
	bool bLeaving = false;          // Continue ran: the next map is loading
	bool bCanQuitToTitle = false;
	bool bQuitting = false;
	bool bQKeyDown = false;
	double OutcomeWall = 0.0;
	bool bInputBound = false;
	double RoundStartWall = 0.0;
	double RoundStartGame = 0.0;
	int32 Attempt = 0;
	bool bDilated = false;

	// The line-up in Core form: both sides at full HP, copied into the component each round.
	std::vector<ftcore::Combatant> CoreParty;
	std::vector<ftcore::Combatant> CoreFoes;
	// What winning a story fight pays (the line-up's reward); nothing for other fights.
	ftcore::Reward Reward;
};
