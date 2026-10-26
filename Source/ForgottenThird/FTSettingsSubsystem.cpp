#include "FTSettingsSubsystem.h"

#include "Engine/GameInstance.h"
#include "InputCoreTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "ForgottenThird.h"
#include "FTPlayerController.h"
#include "FTStorySubsystem.h"
#include "FTText.h"

#include <string>

void UFTSettingsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	RowLabels.SetNum(RowCount);
	RowValues.SetNum(RowCount);
	RowRects.Init(FBox2D(ForceInit), RowCount);
	for (int32 I = 0; I < RowCount; ++I)
	{
		RowLabels[I] = FString(UTF8_TO_TCHAR(ftcore::settingsRowLabel(static_cast<ftcore::SettingsRow>(I))));
	}
	Load();
	MarkChanged();
}

void UFTSettingsSubsystem::Deinitialize()
{
	if (bOpen)
	{
		Close();
	}
	Super::Deinitialize();
}

FString UFTSettingsSubsystem::GetFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Settings.json"));
}

void UFTSettingsSubsystem::Load()
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *GetFilePath()))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Settings: no file, using defaults."));
		return;
	}
	ftcore::Settings Loaded;
	std::string Error;
	if (!ftcore::decodeSettings(FT::BytesAsText(Bytes), Loaded, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Settings: the file was refused, using defaults."));
		return;
	}
	Values = Loaded;
	UE_LOG(LogForgottenThird, Log, TEXT("Settings: loaded."));
}

bool UFTSettingsSubsystem::Save() const
{
	const FString Text = FT::ToFString(ftcore::encodeSettings(Values));
	if (!FFileHelper::SaveStringToFile(Text, *GetFilePath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Settings: writing the file failed."));
		return false;
	}
	return true;
}

void UFTSettingsSubsystem::MarkChanged()
{
	++Revision;
	char Buffer[40];
	for (int32 I = 0; I < RowCount; ++I)
	{
		ftcore::settingsRowValue(Values, static_cast<ftcore::SettingsRow>(I), Buffer, sizeof Buffer);
		RowValues[I] = FString(UTF8_TO_TCHAR(Buffer));
	}
}

void UFTSettingsSubsystem::Open(AFTPlayerController* PC)
{
	if (bOpen || !PC)
	{
		return;
	}
	bOpen = true;
	OpenFrame = GFrameCounter;
	Menu.reset();
	LastMouse = FVector2D(-1.0, -1.0);
	Controller = PC;
	bBeganMenu = !PC->IsMenuInputActive();
	PC->BeginMenuInput();
	PC->OnMenuStep.AddUniqueDynamic(this, &UFTSettingsSubsystem::HandleStep);
	PC->OnMenuAdjust.AddUniqueDynamic(this, &UFTSettingsSubsystem::HandleAdjust);
	PC->OnAdvance.AddUniqueDynamic(this, &UFTSettingsSubsystem::HandlePress);
	PC->OnConfirm.AddUniqueDynamic(this, &UFTSettingsSubsystem::HandlePress);
	PC->OnCancel.AddUniqueDynamic(this, &UFTSettingsSubsystem::HandleCancel);
	UE_LOG(LogForgottenThird, Log, TEXT("Settings: opened."));
}

void UFTSettingsSubsystem::Close()
{
	if (!bOpen)
	{
		return;
	}
	bOpen = false;
	CloseFrame = GFrameCounter;
	if (AFTPlayerController* PC = Controller.Get())
	{
		PC->OnMenuStep.RemoveDynamic(this, &UFTSettingsSubsystem::HandleStep);
		PC->OnMenuAdjust.RemoveDynamic(this, &UFTSettingsSubsystem::HandleAdjust);
		PC->OnAdvance.RemoveDynamic(this, &UFTSettingsSubsystem::HandlePress);
		PC->OnConfirm.RemoveDynamic(this, &UFTSettingsSubsystem::HandlePress);
		PC->OnCancel.RemoveDynamic(this, &UFTSettingsSubsystem::HandleCancel);
		if (bBeganMenu)
		{
			PC->EndMenuInput();
		}
	}
	Controller.Reset();
	bBeganMenu = false;
	Save(); // applied as it changed; written once, when it closes
	UE_LOG(LogForgottenThird, Log, TEXT("Settings: closed."));
}

void UFTSettingsSubsystem::Apply(ftcore::SettingsAction Action)
{
	switch (Action)
	{
	case ftcore::SettingsAction::Changed:
		MarkChanged();
		if (UFTStorySubsystem* Story = GetGameInstance()->GetSubsystem<UFTStorySubsystem>())
		{
			Story->ApplyOptionFlags(Values);
		}
		break;
	case ftcore::SettingsAction::Close:
		Close();
		break;
	case ftcore::SettingsAction::Moved:
	case ftcore::SettingsAction::None:
		break;
	}
}

void UFTSettingsSubsystem::HandleStep(int32 Step)
{
	if (bOpen)
	{
		Apply(Menu.press(Values, Step < 0 ? ftcore::SettingsKey::Up : ftcore::SettingsKey::Down));
	}
}

void UFTSettingsSubsystem::HandleAdjust(int32 Direction)
{
	if (bOpen)
	{
		Apply(Menu.press(Values, Direction < 0 ? ftcore::SettingsKey::Left : ftcore::SettingsKey::Right));
	}
}

void UFTSettingsSubsystem::HandlePress()
{
	// The press that opened the overlay is still being broadcast: ignore it.
	if (!bOpen || OpenFrame == GFrameCounter)
	{
		return;
	}
	const AFTPlayerController* PC = Controller.Get();
	float MouseX = 0.f;
	float MouseY = 0.f;
	if (PC && PC->WasInputKeyJustPressed(EKeys::LeftMouseButton))
	{
		// A click: act on the row under the cursor, nothing when it is outside every row.
		if (!PC->GetMousePosition(MouseX, MouseY))
		{
			return;
		}
		const int32 Row = HitRow(FVector2D(MouseX, MouseY));
		if (Row < 0)
		{
			return;
		}
		const int32 Direction = (MouseX >= ValueLeft && MouseX < ValueMid) ? -1 : 1;
		Apply(Menu.click(Values, static_cast<ftcore::SettingsRow>(Row), Direction));
		return;
	}
	Apply(Menu.press(Values, ftcore::SettingsKey::Confirm));
}

void UFTSettingsSubsystem::HandleCancel()
{
	if (bOpen && OpenFrame != GFrameCounter)
	{
		Close();
	}
}

int32 UFTSettingsSubsystem::HitRow(const FVector2D& Point) const
{
	for (int32 I = 0; I < RowRects.Num(); ++I)
	{
		if (RowRects[I].IsInside(Point))
		{
			return I;
		}
	}
	return -1;
}

void UFTSettingsSubsystem::UpdateHover(const FVector2D& Mouse)
{
	if (Mouse == LastMouse)
	{
		return;
	}
	LastMouse = Mouse;
	const int32 Row = HitRow(Mouse);
	if (Row >= 0)
	{
		Menu.select(static_cast<ftcore::SettingsRow>(Row));
	}
}
