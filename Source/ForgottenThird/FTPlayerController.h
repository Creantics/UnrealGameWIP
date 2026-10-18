#pragma once

// Player input. The Enhanced Input actions and their mapping context are built here at runtime, so
// there are no input assets to keep in sync. Every press is broadcast; story input (advance,
// choose, confirm) also drives UFTStorySubsystem directly while bRouteInputToStory is set.
//
// Keys: advance Space / left mouse / pad bottom; choose 1-4 the number keys / pad d-pad (up, right,
// down, left); confirm Enter / pad Start; cancel Escape / Backspace / pad right; rhythm tap F / J /
// pad shoulders / pad left.
//
// Battle input is a second mapping context, added on top (higher priority, so its keys hide the
// story mappings they share) only between BeginBattleInput and EndBattleInput. Its presses are
// broadcast as OnBattleArrow / OnBattleSkill / OnBattleTarget / OnBattleConfirm for whoever runs
// the battle (AFTBattleDemoActor):
//   arrows   arrow keys / W A S D / pad d-pad (bFaceButton false), pad X A Y B (bFaceButton true),
//            in lane order Left, Down, Up, Right
//   skill    1-4 / numpad 1-4 (the story's choose keys, reported as OnBattleSkill too)
//   target   Tab / E / pad right shoulder: next; Q / pad left shoulder: previous
//   confirm  Enter / Space / pad Start

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "FTComboComponent.h"

#include "FTPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UFTStorySubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFTInputEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTChooseEvent, int32, Option);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTTapEvent, double, WallSeconds);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFTBattleArrowEvent, EFTArrow, Arrow, bool, bFaceButton);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTBattleStepEvent, int32, Step);

UCLASS()
class FORGOTTENTHIRD_API AFTPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	static constexpr int32 NumChoices = 4;

	// Off while another mode (battle, menus) owns the story keys; the events still fire.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	bool bRouteInputToStory = true;

	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTInputEvent OnAdvance;
	// Option is 0-based (the "1" key is option 0).
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTChooseEvent OnChoose;
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTInputEvent OnConfirm;
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTInputEvent OnCancel;
	// WallSeconds is FPlatformTime::Seconds() when the press was handled; turn it into song time
	// with UFTConductorComponent::GetEventSongTime. Taps also feed the pawn's conductor while it
	// calibrates.
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTTapEvent OnRhythmTap;

	UFUNCTION(BlueprintPure, Category = "Input")
	UInputMappingContext* GetMappingContext() const { return MappingContext; }

	// ---- battle ----

	// Turns the battle keys on (and story routing off) until EndBattleInput.
	UFUNCTION(BlueprintCallable, Category = "Input|Battle")
	void BeginBattleInput();
	// Turns the battle keys off and restores story routing.
	UFUNCTION(BlueprintCallable, Category = "Input|Battle")
	void EndBattleInput();
	UFUNCTION(BlueprintPure, Category = "Input|Battle")
	bool IsBattleInputActive() const { return bBattleInput; }
	UFUNCTION(BlueprintPure, Category = "Input|Battle")
	UInputMappingContext* GetBattleMappingContext() const { return BattleContext; }

	// A lane arrow. bFaceButton: from the pad's face buttons rather than the d-pad or keyboard.
	UPROPERTY(BlueprintAssignable, Category = "Input|Battle")
	FFTBattleArrowEvent OnBattleArrow;
	// Skill key, 0-based (the "1" key is skill 0).
	UPROPERTY(BlueprintAssignable, Category = "Input|Battle")
	FFTChooseEvent OnBattleSkill;
	// Target cycling: +1 next, -1 previous.
	UPROPERTY(BlueprintAssignable, Category = "Input|Battle")
	FFTBattleStepEvent OnBattleTarget;
	UPROPERTY(BlueprintAssignable, Category = "Input|Battle")
	FFTInputEvent OnBattleConfirm;

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;

private:
	// Creates the actions and the mapping context once.
	void CreateInputObjects();
	UInputAction* MakeAction(const TCHAR* Name);
	UFTStorySubsystem* GetStory() const;

	void HandleAdvance();
	void HandleChoose(int32 Option);
	void HandleChoose1() { HandleChoose(0); }
	void HandleChoose2() { HandleChoose(1); }
	void HandleChoose3() { HandleChoose(2); }
	void HandleChoose4() { HandleChoose(3); }
	void HandleConfirm();
	void HandleCancel();
	void HandleTap();

	void HandleBattleArrow(EFTArrow Arrow, bool bFaceButton);
	void HandleLaneLeft() { HandleBattleArrow(EFTArrow::Left, false); }
	void HandleLaneDown() { HandleBattleArrow(EFTArrow::Down, false); }
	void HandleLaneUp() { HandleBattleArrow(EFTArrow::Up, false); }
	void HandleLaneRight() { HandleBattleArrow(EFTArrow::Right, false); }
	void HandleFaceLeft() { HandleBattleArrow(EFTArrow::Left, true); }
	void HandleFaceDown() { HandleBattleArrow(EFTArrow::Down, true); }
	void HandleFaceUp() { HandleBattleArrow(EFTArrow::Up, true); }
	void HandleFaceRight() { HandleBattleArrow(EFTArrow::Right, true); }
	void HandleTargetNext();
	void HandleTargetPrev();
	void HandleBattleConfirm();
	class UEnhancedInputLocalPlayerSubsystem* GetInputSubsystem() const;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> BattleContext;
	// Lane order (Left, Down, Up, Right): keyboard / d-pad, then face buttons.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInputAction>> LaneActions;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInputAction>> FaceActions;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> TargetNextAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> TargetPrevAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> BattleConfirmAction;
	bool bBattleInput = false;
	bool bStoryRoutingBeforeBattle = true;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> AdvanceAction;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInputAction>> ChooseActions;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ConfirmAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> CancelAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> TapAction;
};
