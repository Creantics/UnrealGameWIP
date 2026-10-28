#pragma once

// Player input. The Enhanced Input actions and their mapping context are built here at runtime, so
// there are no input assets to keep in sync. Every press is broadcast; story input (advance,
// choose, confirm) also drives UFTStorySubsystem directly while bRouteInputToStory is set.
//
// Keys: advance Space / left mouse / pad bottom; choose 1-4 the number keys / pad d-pad (up, right,
// down, left); confirm Enter (pad Start confirms only in the menu context, on the title: on a gameplay
// screen it pauses); cancel Escape / Backspace / pad right; rhythm tap F / J / pad shoulders / pad
// left. Story routing is off by default: AFTStoryDirector takes the presses and drives the story; set
// bRouteInputToStory to drive it from here instead.
//
// Reading keys (the story director and the settings overlay listen): backlog B / mouse wheel up (OnBacklog,
// OnWheel +1; wheel down is OnWheel -1); skip, held, Ctrl / S / pad right trigger (OnSkip true while
// held, false on release). Escape and pad Start (OnBack) close the backlog, else pause.
//
// Battle input is a second mapping context, added on top (higher priority, so its keys hide the
// story mappings they share) only between BeginBattleInput and EndBattleInput. Its presses are
// broadcast as OnBattleArrow / OnBattleSkill / OnBattleTarget / OnBattleConfirm for whoever runs
// the battle (AFTBattleDemoActor):
//   arrows   arrow keys / W A S D / pad d-pad (bFaceButton false), pad X A Y B (bFaceButton true),
//            in lane order Left, Down, Up, Right
//   skill    1-4 / numpad 1-4 (the story's choose keys, reported as OnBattleSkill too)
//   target   Tab / E / pad right shoulder: next; Q / pad left shoulder: previous
//   confirm  Enter / Space

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "FTComboComponent.h"

#include "FTPlayerController.generated.h"

class UEnhancedInputComponent;
class UInputAction;
class UInputMappingContext;
class UFTStorySubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFTInputEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTChooseEvent, int32, Option);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTTapEvent, double, WallSeconds);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFTBattleArrowEvent, EFTArrow, Arrow, bool, bFaceButton);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTBattleStepEvent, int32, Step);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTHeldEvent, bool, bHeld);

UCLASS()
class FORGOTTENTHIRD_API AFTPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	static constexpr int32 NumChoices = 4;

	// On: presses also drive UFTStorySubsystem directly (off by default; AFTStoryDirector does it,
	// and battle input turns it off while it runs). The events always fire.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	bool bRouteInputToStory = false;

	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTInputEvent OnAdvance;
	// Option is 0-based (the "1" key is option 0).
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTChooseEvent OnChoose;
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTInputEvent OnConfirm;
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTInputEvent OnCancel;
	// WallSeconds is UFTPauseSubsystem::WallSeconds() (the clock the conductor runs on: platform time
	// without the time spent paused) when the press was handled; turn it into song time with
	// UFTConductorComponent::GetEventSongTime. Taps also feed the pawn's conductor while it calibrates.
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTTapEvent OnRhythmTap;

	// B: open or close the dialogue backlog.
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTInputEvent OnBacklog;
	// Mouse wheel: +1 up, -1 down.
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTBattleStepEvent OnWheel;
	// The skip key: true when it goes down, false when it is released.
	UPROPERTY(BlueprintAssignable, Category = "Input")
	FFTHeldEvent OnSkip;

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

	// ---- menu ----

	// Menu navigation (the title screen): a third mapping context, on top of the story's, only
	// between BeginMenuInput and EndMenuInput. Up arrow, W, pad d-pad up
	// and left give OnMenuStep(-1); Down arrow, S, pad d-pad down and right give OnMenuStep(+1).
	// Its actions consume those keys, so the d-pad stops reaching the story's choose actions. Space,
	// Enter, click and pad A still arrive as OnAdvance / OnConfirm, and 1 / 2 as OnChoose. Pad Start
	// arrives as OnConfirm here too: the menu context maps it for the title (which keeps Start as
	// confirm); on a gameplay screen the pause context, one priority up, takes it first.
	UFUNCTION(BlueprintCallable, Category = "Input|Menu")
	void BeginMenuInput();
	UFUNCTION(BlueprintCallable, Category = "Input|Menu")
	void EndMenuInput();
	UFUNCTION(BlueprintPure, Category = "Input|Menu")
	bool IsMenuInputActive() const { return bMenuInput; }
	// -1 up, +1 down.
	UPROPERTY(BlueprintAssignable, Category = "Input|Menu")
	FFTBattleStepEvent OnMenuStep;
	// Left arrow / A give OnMenuAdjust(-1); Right arrow / D give +1 (the d-pad stays on OnMenuStep).
	UPROPERTY(BlueprintAssignable, Category = "Input|Menu")
	FFTBattleStepEvent OnMenuAdjust;
	// BeginMenuInput also shows the mouse cursor (and lets it move); EndMenuInput puts both back.

	// ---- pause ----

	// Esc and pad Start (OnBack / the pause menu): a fourth mapping context, above the explore pawn's, that
	// every gameplay screen has from the start and AFTGameMode adds when the story begins. The title never
	// has it: Start confirms there, and Esc closes its settings overlay as the cancel action. The pause
	// action consumes both keys, so on a gameplay screen Esc is no longer a cancel (Backspace and pad B
	// still are) and Start is no longer a confirm. No effect in a headless run.
	UFUNCTION(BlueprintCallable, Category = "Input|Pause")
	void BeginPauseInput();
	UFUNCTION(BlueprintCallable, Category = "Input|Pause")
	void EndPauseInput();
	UFUNCTION(BlueprintPure, Category = "Input|Pause")
	bool IsPauseInputActive() const { return bPauseInput; }
	// Esc or pad Start with no overlay up: the screen closes a layer of its own (the backlog, an aim in
	// progress) when it has one, and calls ClaimBack() so the press doesn't also open the pause menu.
	UPROPERTY(BlueprintAssignable, Category = "Input|Pause")
	FFTInputEvent OnBack;
	void ClaimBack() { bBackClaimed = true; }

	// While the pause menu is open (UFTPauseSubsystem calls these): a fifth context at the top that maps
	// every key the menu and the settings overlay opened from it need (up / down / left / right, advance,
	// confirm, cancel), so a screen's own context can never hide one while the game is paused.
	UFUNCTION(BlueprintCallable, Category = "Input|Pause")
	void BeginPauseMenuInput();
	UFUNCTION(BlueprintCallable, Category = "Input|Pause")
	void EndPauseMenuInput();

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

	void HandleMenuUp() { HandleMenuStep(-1); }
	void HandleMenuDown() { HandleMenuStep(1); }
	void HandleMenuStep(int32 Step);
	void HandleMenuLeft() { HandleMenuAdjust(-1); }
	void HandleMenuRight() { HandleMenuAdjust(1); }
	void HandleMenuAdjust(int32 Direction);

	// Pause: Esc / pad Start. Backs out of the topmost layer first (the settings overlay, the pause menu
	// itself, then OnBack's listeners), else opens the pause menu.
	void CreatePauseInput();
	void BindPauseInput(UEnhancedInputComponent& Input);
	void HandlePause();

	void HandleBacklog() { OnBacklog.Broadcast(); }
	void HandleWheelUp() { OnWheel.Broadcast(1); }
	void HandleWheelDown() { OnWheel.Broadcast(-1); }
	void HandleSkipStart() { OnSkip.Broadcast(true); }
	void HandleSkipStop() { OnSkip.Broadcast(false); }

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> BacklogAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> WheelUpAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> WheelDownAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> SkipAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MenuLeftAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MenuRightAction;
	bool bPrevShowCursor = false;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MenuContext;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MenuUpAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MenuDownAction;
	bool bMenuInput = false;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> PauseContext;
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> PauseMenuContext;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> PauseAction;
	bool bPauseInput = false;
	bool bPauseMenuInput = false;
	bool bBackClaimed = false;

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
