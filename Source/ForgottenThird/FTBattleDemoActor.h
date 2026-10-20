#pragma once

// The playable battle demo: two party members against two enemies, with the rules in Core
// (ftcore::Battle, line-up from ftcore/BattleDemo.h) and this actor as the stage.
//
// It owns the battle and its song clock (a silent 120 BPM wall clock: no audio is bound), places
// the figures (the lead characters Devin and Duy looping their idle clips, placeholder cube enemies)
// that flash or squash on hits and fall over when downed, frames the fight with its camera and turns the player controller's battle
// input into commands and presses. AFTBattleHUD draws the UI from the getters below.
//
// Command menu (phase AwaitingCommand): Up/Down pick the skill, Left/Right or Tab/Q/E the target,
// Enter/Space/pad A use it; 1-4 use a skill at once. During a combo every arrow is a step.
// After the outcome, Enter/Space/pad A/Start fight again.
//
// bAutoPlay (or -BattleAutoPlay on the command line) lets Core's pilot play perfectly; with
// -BattleQuitOnOutcome the game exits when the battle ends (a headless end-to-end run).

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "FTBattleComponent.h"
#include "FTComboComponent.h"

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

UCLASS()
class FORGOTTENTHIRD_API AFTBattleDemoActor : public AActor
{
	GENERATED_BODY()

public:
	AFTBattleDemoActor();

	static constexpr int32 MaxPopups = 12;

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
	int32 GetSelectedSkill() const { return SelectedSkill; }
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
	bool IsWaitingForRestart() const { return bWaitingForRestart; }
	// World position above a combatant's figure (for markers and numbers).
	FVector GetFigureTop(int32 Id) const;
	// 0..1, high right after the figure was hit.
	float GetFlash(int32 Id) const;
	// Seconds since the player last pressed this lane during a combo.
	double GetLanePressAge(int32 Lane) const;

	struct FPopup
	{
		double WallStart = -1.0;
		// Grade popup: the lane of the judged note. Number popup: the combatant the number is over.
		int32 Lane = 0;
		int32 Combatant = -1;
		EFTGrade Grade = EFTGrade::Pending;
		bool bWrong = false;
		bool bNumber = false;
		bool bBlock = false;
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

	void BuildFigures();
	void ResetFigures();
	void CacheText();
	void UpdateHpText(int32 Id);
	void TickFigures(double Wall);
	void TickAutoPlay();
	void UseSelected();
	void CycleTarget(int32 Step);
	void EnsureTarget();
	void AddPopup(const FPopup& P);
	void SetBanner(const FString& Text);
	void SetNotice(const FString& Text);
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

	FPopup Popups[MaxPopups];
	int32 NextPopup = 0;
	double LanePressWall[4] = {-100.0, -100.0, -100.0, -100.0};

	int32 SelectedSkill = 0;
	int32 SelectedTarget = -1;
	bool bWaitingForRestart = false;
	double OutcomeWall = 0.0;
	bool bInputBound = false;
	double RoundStartWall = 0.0;
};
