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
}

void AFTPlayerController::BeginPlay()
{
	Super::BeginPlay();
	CreateInputObjects();

	// Remote controllers on a server have no local player.
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();
	UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	if (Subsystem)
	{
		Subsystem->AddMappingContext(MappingContext, 0);
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
