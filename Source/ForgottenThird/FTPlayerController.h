#pragma once

// Player input. The Enhanced Input actions and their mapping context are built here at runtime, so
// there are no input assets to keep in sync. Every press is broadcast; story input (advance,
// choose, confirm) also drives UFTStorySubsystem directly while bRouteInputToStory is set.
//
// Keys: advance Space / left mouse / pad bottom; choose 1-4 the number keys / pad d-pad (up, right,
// down, left); confirm Enter / pad Start; cancel Escape / Backspace / pad right; rhythm tap F / J /
// pad shoulders / pad left.

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "FTPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UFTStorySubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFTInputEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTChooseEvent, int32, Option);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTTapEvent, double, WallSeconds);

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
