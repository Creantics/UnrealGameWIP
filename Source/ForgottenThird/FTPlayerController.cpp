#include "FTPlayerController.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "HAL/PlatformTime.h"
#include "InputAction.h"
#include "InputMappingContext.h"

#include "ForgottenThird.h"
#include "FTConductorComponent.h"
#include "FTStorySubsystem.h"

void AFTPlayerController::CreateInputObjects()
{
	if (MappingContext)
	{
		return;
	}

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_ForgottenThird"));

	AdvanceAction = MakeAction(TEXT("IA_Advance"));
	MappingContext->MapKey(AdvanceAction, EKeys::SpaceBar);
	MappingContext->MapKey(AdvanceAction, EKeys::LeftMouseButton);
	MappingContext->MapKey(AdvanceAction, EKeys::Gamepad_FaceButton_Bottom);

	static const TCHAR* const ChooseNames[NumChoices] = {TEXT("IA_Choose1"), TEXT("IA_Choose2"), TEXT("IA_Choose3"), TEXT("IA_Choose4")};
	const FKey NumberKeys[NumChoices] = {EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four};
	const FKey NumPadKeys[NumChoices] = {EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour};
	const FKey PadKeys[NumChoices] = {EKeys::Gamepad_DPad_Up, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_DPad_Left};
	ChooseActions.Reset(NumChoices);
	for (int32 I = 0; I < NumChoices; ++I)
	{
		UInputAction* Action = MakeAction(ChooseNames[I]);
		MappingContext->MapKey(Action, NumberKeys[I]);
		MappingContext->MapKey(Action, NumPadKeys[I]);
		MappingContext->MapKey(Action, PadKeys[I]);
		ChooseActions.Add(Action);
	}

	ConfirmAction = MakeAction(TEXT("IA_Confirm"));
	MappingContext->MapKey(ConfirmAction, EKeys::Enter);
	MappingContext->MapKey(ConfirmAction, EKeys::Gamepad_Special_Right);

	CancelAction = MakeAction(TEXT("IA_Cancel"));
	MappingContext->MapKey(CancelAction, EKeys::Escape);
	MappingContext->MapKey(CancelAction, EKeys::BackSpace);
	MappingContext->MapKey(CancelAction, EKeys::Gamepad_FaceButton_Right);

	TapAction = MakeAction(TEXT("IA_RhythmTap"));
	MappingContext->MapKey(TapAction, EKeys::F);
	MappingContext->MapKey(TapAction, EKeys::J);
	MappingContext->MapKey(TapAction, EKeys::Gamepad_LeftShoulder);
	MappingContext->MapKey(TapAction, EKeys::Gamepad_RightShoulder);
	MappingContext->MapKey(TapAction, EKeys::Gamepad_FaceButton_Left);

	// Battle: added on top of the story context while a battle runs. Its actions consume their
	// keys, so the d-pad, face buttons, shoulders, Space and Enter stop reaching the story actions.
	BattleContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Battle"));
	static const TCHAR* const LaneNames[4] = {TEXT("IA_LaneLeft"), TEXT("IA_LaneDown"), TEXT("IA_LaneUp"), TEXT("IA_LaneRight")};
	static const TCHAR* const FaceNames[4] = {TEXT("IA_PadLaneLeft"), TEXT("IA_PadLaneDown"), TEXT("IA_PadLaneUp"), TEXT("IA_PadLaneRight")};
	const FKey ArrowKeys[4] = {EKeys::Left, EKeys::Down, EKeys::Up, EKeys::Right};
	const FKey LetterKeys[4] = {EKeys::A, EKeys::S, EKeys::W, EKeys::D};
	const FKey DPadKeys[4] = {EKeys::Gamepad_DPad_Left, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_DPad_Up, EKeys::Gamepad_DPad_Right};
	const FKey FaceKeys[4] = {EKeys::Gamepad_FaceButton_Left, EKeys::Gamepad_FaceButton_Bottom, EKeys::Gamepad_FaceButton_Top,
		EKeys::Gamepad_FaceButton_Right};
	LaneActions.Reset(4);
	FaceActions.Reset(4);
	for (int32 I = 0; I < 4; ++I)
	{
		UInputAction* Lane = MakeAction(LaneNames[I]);
		BattleContext->MapKey(Lane, ArrowKeys[I]);
		BattleContext->MapKey(Lane, LetterKeys[I]);
		BattleContext->MapKey(Lane, DPadKeys[I]);
		LaneActions.Add(Lane);

		UInputAction* Face = MakeAction(FaceNames[I]);
		BattleContext->MapKey(Face, FaceKeys[I]);
		FaceActions.Add(Face);
	}
	// Skills use the story's choose actions (1-4 and the numpad stay mapped in the story context;
	// its d-pad mapping is hidden by the lanes above). HandleChoose reports them as OnBattleSkill.

	TargetNextAction = MakeAction(TEXT("IA_TargetNext"));
	BattleContext->MapKey(TargetNextAction, EKeys::Tab);
	BattleContext->MapKey(TargetNextAction, EKeys::E);
	BattleContext->MapKey(TargetNextAction, EKeys::Gamepad_RightShoulder);

	TargetPrevAction = MakeAction(TEXT("IA_TargetPrev"));
	BattleContext->MapKey(TargetPrevAction, EKeys::Q);
	BattleContext->MapKey(TargetPrevAction, EKeys::Gamepad_LeftShoulder);

	BattleConfirmAction = MakeAction(TEXT("IA_BattleConfirm"));
	BattleContext->MapKey(BattleConfirmAction, EKeys::Enter);
	BattleContext->MapKey(BattleConfirmAction, EKeys::SpaceBar);
	BattleContext->MapKey(BattleConfirmAction, EKeys::Gamepad_Special_Right);

	// Menu: added on top of the story context while a menu is up. Consumes the d-pad.
	MenuContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Menu"));
	MenuUpAction = MakeAction(TEXT("IA_MenuUp"));
	MenuContext->MapKey(MenuUpAction, EKeys::Up);
	MenuContext->MapKey(MenuUpAction, EKeys::W);
	MenuContext->MapKey(MenuUpAction, EKeys::Gamepad_DPad_Up);
	MenuContext->MapKey(MenuUpAction, EKeys::Gamepad_DPad_Left);
	MenuDownAction = MakeAction(TEXT("IA_MenuDown"));
	MenuContext->MapKey(MenuDownAction, EKeys::Down);
	MenuContext->MapKey(MenuDownAction, EKeys::S);
	MenuContext->MapKey(MenuDownAction, EKeys::Gamepad_DPad_Down);
	MenuContext->MapKey(MenuDownAction, EKeys::Gamepad_DPad_Right);
}

UInputAction* AFTPlayerController::MakeAction(const TCHAR* Name)
{
	UInputAction* Action = NewObject<UInputAction>(this, Name);
	Action->ValueType = EInputActionValueType::Boolean;
	return Action;
}

void AFTPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	CreateInputObjects();

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent);
	if (!Input)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("Input component is not an UEnhancedInputComponent; check DefaultInputComponentClass in DefaultInput.ini"));
		return;
	}

	// Started: once per press.
	Input->BindAction(AdvanceAction, ETriggerEvent::Started, this, &AFTPlayerController::HandleAdvance);
	Input->BindAction(ChooseActions[0], ETriggerEvent::Started, this, &AFTPlayerController::HandleChoose1);
	Input->BindAction(ChooseActions[1], ETriggerEvent::Started, this, &AFTPlayerController::HandleChoose2);
	Input->BindAction(ChooseActions[2], ETriggerEvent::Started, this, &AFTPlayerController::HandleChoose3);
	Input->BindAction(ChooseActions[3], ETriggerEvent::Started, this, &AFTPlayerController::HandleChoose4);
	Input->BindAction(ConfirmAction, ETriggerEvent::Started, this, &AFTPlayerController::HandleConfirm);
	Input->BindAction(CancelAction, ETriggerEvent::Started, this, &AFTPlayerController::HandleCancel);
	Input->BindAction(TapAction, ETriggerEvent::Started, this, &AFTPlayerController::HandleTap);

	Input->BindAction(LaneActions[0], ETriggerEvent::Started, this, &AFTPlayerController::HandleLaneLeft);
	Input->BindAction(LaneActions[1], ETriggerEvent::Started, this, &AFTPlayerController::HandleLaneDown);
	Input->BindAction(LaneActions[2], ETriggerEvent::Started, this, &AFTPlayerController::HandleLaneUp);
	Input->BindAction(LaneActions[3], ETriggerEvent::Started, this, &AFTPlayerController::HandleLaneRight);
	Input->BindAction(FaceActions[0], ETriggerEvent::Started, this, &AFTPlayerController::HandleFaceLeft);
	Input->BindAction(FaceActions[1], ETriggerEvent::Started, this, &AFTPlayerController::HandleFaceDown);
	Input->BindAction(FaceActions[2], ETriggerEvent::Started, this, &AFTPlayerController::HandleFaceUp);
	Input->BindAction(FaceActions[3], ETriggerEvent::Started, this, &AFTPlayerController::HandleFaceRight);
	Input->BindAction(TargetNextAction, ETriggerEvent::Started, this, &AFTPlayerController::HandleTargetNext);
	Input->BindAction(TargetPrevAction, ETriggerEvent::Started, this, &AFTPlayerController::HandleTargetPrev);
	Input->BindAction(BattleConfirmAction, ETriggerEvent::Started, this, &AFTPlayerController::HandleBattleConfirm);
	Input->BindAction(MenuUpAction, ETriggerEvent::Started, this, &AFTPlayerController::HandleMenuUp);
	Input->BindAction(MenuDownAction, ETriggerEvent::Started, this, &AFTPlayerController::HandleMenuDown);
}

void AFTPlayerController::BeginPlay()
{
	Super::BeginPlay();
	CreateInputObjects();

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem())
	{
		Subsystem->AddMappingContext(MappingContext, 0);
		if (bBattleInput)
		{
			Subsystem->AddMappingContext(BattleContext, 1); // BeginBattleInput ran first
		}
		if (bMenuInput)
		{
			Subsystem->AddMappingContext(MenuContext, 2); // BeginMenuInput ran first
		}
	}
}

void AFTPlayerController::BeginMenuInput()
{
	if (bMenuInput)
	{
		return;
	}
	CreateInputObjects();
	bMenuInput = true;
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem())
	{
		Subsystem->AddMappingContext(MenuContext, 2);
	}
}

void AFTPlayerController::EndMenuInput()
{
	if (!bMenuInput)
	{
		return;
	}
	bMenuInput = false;
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem())
	{
		Subsystem->RemoveMappingContext(MenuContext);
	}
}

void AFTPlayerController::HandleMenuStep(int32 Step)
{
	if (bMenuInput)
	{
		OnMenuStep.Broadcast(Step);
	}
}

UEnhancedInputLocalPlayerSubsystem* AFTPlayerController::GetInputSubsystem() const
{
	// Remote controllers on a server have no local player.
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();
	return LocalPlayer ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
}

void AFTPlayerController::BeginBattleInput()
{
	if (bBattleInput)
	{
		return;
	}
	CreateInputObjects();
	bBattleInput = true;
	bStoryRoutingBeforeBattle = bRouteInputToStory;
	bRouteInputToStory = false;
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem())
	{
		Subsystem->AddMappingContext(BattleContext, 1);
	}
}

void AFTPlayerController::EndBattleInput()
{
	if (!bBattleInput)
	{
		return;
	}
	bBattleInput = false;
	bRouteInputToStory = bStoryRoutingBeforeBattle;
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem())
	{
		Subsystem->RemoveMappingContext(BattleContext);
	}
}

void AFTPlayerController::HandleBattleArrow(EFTArrow Arrow, bool bFaceButton)
{
	if (bBattleInput)
	{
		OnBattleArrow.Broadcast(Arrow, bFaceButton);
	}
}

void AFTPlayerController::HandleTargetNext()
{
	if (bBattleInput)
	{
		OnBattleTarget.Broadcast(1);
	}
}

void AFTPlayerController::HandleTargetPrev()
{
	if (bBattleInput)
	{
		OnBattleTarget.Broadcast(-1);
	}
}

void AFTPlayerController::HandleBattleConfirm()
{
	if (bBattleInput)
	{
		OnBattleConfirm.Broadcast();
	}
}

UFTStorySubsystem* AFTPlayerController::GetStory() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UFTStorySubsystem>() : nullptr;
}

void AFTPlayerController::HandleAdvance()
{
	OnAdvance.Broadcast();
	UFTStorySubsystem* Story = bRouteInputToStory ? GetStory() : nullptr;
	if (Story && (Story->GetState() == EFTStoryState::Line || Story->GetState() == EFTStoryState::Direction))
	{
		Story->Advance();
	}
}

void AFTPlayerController::HandleChoose(int32 Option)
{
	OnChoose.Broadcast(Option);
	if (bBattleInput)
	{
		OnBattleSkill.Broadcast(Option);
	}
	UFTStorySubsystem* Story = bRouteInputToStory ? GetStory() : nullptr;
	if (Story && Story->GetState() == EFTStoryState::Choice && Option < Story->GetOptionCount())
	{
		Story->Choose(Option);
	}
}

void AFTPlayerController::HandleConfirm()
{
	OnConfirm.Broadcast();
	UFTStorySubsystem* Story = bRouteInputToStory ? GetStory() : nullptr;
	if (!Story)
	{
		return;
	}
	switch (Story->GetState())
	{
	case EFTStoryState::Line:
	case EFTStoryState::Direction:
		Story->Advance();
		break;
	case EFTStoryState::Choice:
		if (Story->GetDefaultOption() >= 0)
		{
			Story->Choose(Story->GetDefaultOption());
		}
		break;
	default:
		break;
	}
}

void AFTPlayerController::HandleCancel()
{
	OnCancel.Broadcast();
}

void AFTPlayerController::HandleTap()
{
	const double WallSeconds = FPlatformTime::Seconds();
	if (const APawn* ControlledPawn = GetPawn())
	{
		UFTConductorComponent* Conductor = ControlledPawn->FindComponentByClass<UFTConductorComponent>();
		if (Conductor && Conductor->IsCalibrating())
		{
			Conductor->RecordTap();
		}
	}
	OnRhythmTap.Broadcast(WallSeconds);
}
