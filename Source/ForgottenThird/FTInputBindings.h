#pragma once

// The player's walking and fighting bindings (ftcore::InputBindings): the defaults, with the player's overrides from
// Saved/InputBindings.json read over them once (a refused file is logged and the defaults stand). Readers ask for the
// key of an action on each device and whether a held action is pressed once to toggle.
//
// The arena, the explore character and the pause key read it; the quick-time and Beat screens keep their action
// buttons (FTActionButtons.h), menus their own keys.

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FTSettingsSubsystem.h"
#include "FTText.h"
#include "FTUiText.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "ftcore/InputBindings.h"

#include <array>
#include <string>

namespace FTInputBindings
{
inline FString GetFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("InputBindings.json"));
}

inline const ftcore::InputBindings& Get()
{
	static const ftcore::InputBindings Bindings = []
	{
		ftcore::InputBindings B = ftcore::defaultInputBindings();
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *GetFilePath()))
		{
			std::string Error;
			if (!ftcore::importInputBindings(FT::ToUtf8(Text), B, &Error))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("InputBindings: %s refused (%s); using the defaults."), *GetFilePath(),
					*FT::ToFString(Error));
			}
		}
		return B;
	}();
	return Bindings;
}

// The engine key for the action on that device; EKeys::Invalid when unbound. Built once with Get().
inline const FKey& Key(ftcore::InputAction Action, ftcore::InputDevice Device)
{
	static const std::array<std::array<FKey, ftcore::kInputActionCount>, ftcore::kInputDeviceCount> Keys = []
	{
		std::array<std::array<FKey, ftcore::kInputActionCount>, ftcore::kInputDeviceCount> K;
		for (size_t D = 0; D < ftcore::kInputDeviceCount; ++D)
		{
			for (size_t A = 0; A < ftcore::kInputActionCount; ++A)
			{
				const char* Name = ftcore::inputKey(Get(), static_cast<ftcore::InputAction>(A), static_cast<ftcore::InputDevice>(D));
				K[D][A] = Name[0] ? FKey(FName(Name)) : EKeys::Invalid;
				if (Name[0] && !K[D][A].IsValid())
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("InputBindings: %s is not a key the engine knows; %s is unbound there."),
						*FT::ToFString(Name), *FT::ToFString(ftcore::inputActionKey(static_cast<ftcore::InputAction>(A))));
				}
			}
		}
		return K;
	}();
	return Keys[static_cast<size_t>(Device) % ftcore::kInputDeviceCount][static_cast<size_t>(Action) % ftcore::kInputActionCount];
}

// Whether the action's key is down on either device.
inline bool IsDown(const APlayerController& PC, ftcore::InputAction Action)
{
	const FKey& Keys = Key(Action, ftcore::InputDevice::Keys);
	const FKey& Pad = Key(Action, ftcore::InputDevice::Pad);
	return (Keys.IsValid() && PC.IsInputKeyDown(Keys)) || (Pad.IsValid() && PC.IsInputKeyDown(Pad));
}

// Whether the action's key went down this frame on either device.
inline bool WasJustPressed(const APlayerController& PC, ftcore::InputAction Action)
{
	const FKey& Keys = Key(Action, ftcore::InputDevice::Keys);
	const FKey& Pad = Key(Action, ftcore::InputDevice::Pad);
	return (Keys.IsValid() && PC.WasInputKeyJustPressed(Keys)) || (Pad.IsValid() && PC.WasInputKeyJustPressed(Pad));
}

// The device the player used last: a bound pad button or a pushed stick picks the pad; a bound key, a mouse button or
// a mouse move picks the keyboard. Starts on the keyboard. Call it each frame the controls line is drawn.
inline ftcore::InputDevice NoteDevice(const APlayerController* PC)
{
	static ftcore::InputDevice Last = ftcore::InputDevice::Keys;
	if (!PC)
	{
		return Last;
	}
	const auto AnyDown = [PC](ftcore::InputDevice Device)
	{
		for (size_t A = 0; A < ftcore::kInputActionCount; ++A)
		{
			const FKey& K = Key(static_cast<ftcore::InputAction>(A), Device);
			if (K.IsValid() && PC->IsInputKeyDown(K))
			{
				return true;
			}
		}
		return false;
	};
	const float Stick = FMath::Max(FMath::Max(FMath::Abs(PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX)), FMath::Abs(PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY))),
		FMath::Max(FMath::Abs(PC->GetInputAnalogKeyState(EKeys::Gamepad_RightX)), FMath::Abs(PC->GetInputAnalogKeyState(EKeys::Gamepad_RightY))));
	float MouseX = 0.f;
	float MouseY = 0.f;
	PC->GetInputMouseDelta(MouseX, MouseY);
	if (Stick > 0.3f || AnyDown(ftcore::InputDevice::Pad))
	{
		Last = ftcore::InputDevice::Pad;
	}
	else if (FMath::Abs(MouseX) + FMath::Abs(MouseY) > 0.5f || AnyDown(ftcore::InputDevice::Keys))
	{
		Last = ftcore::InputDevice::Keys;
	}
	return Last;
}

// A screen's controls line on a device, built from the table (ftcore::inputHintLine) with the words from ui.json
// ("input.light"), so it names only keys the player has. Built once per screen and device, then kept.
inline const FString& HintLine(ftcore::InputHintScreen Screen, ftcore::InputDevice Device)
{
	static TOptional<FString> Lines[2][ftcore::kInputDeviceCount];
	TOptional<FString>& Line = Lines[static_cast<size_t>(Screen) % 2][static_cast<size_t>(Device) % ftcore::kInputDeviceCount];
	if (!Line.IsSet())
	{
		const std::string Text = ftcore::inputHintLine(Get(), Screen, Device,
			[](const char* LabelKey, void*) { return FT::ToUtf8(FTUiText::Lookup(*FT::ToFString(LabelKey))); });
		Line = FT::ToFString(Text);
	}
	return Line.GetValue();
}

// Whether the game window has lost focus: the engine lets go of every key then, so a hold read as a release would fire
// a press nobody made. Hosts treat it as a pause and clear their press state. Never true on an unattended (headless) run.
inline bool LostFocus()
{
	return !FApp::IsUnattended() && !FApp::HasFocus();
}

// Whether a held action is pressed once to toggle: its own mode, else the game-wide hold-toggle setting.
inline bool Toggles(const UWorld* World, ftcore::InputAction Action)
{
	const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	const UFTSettingsSubsystem* Settings = GI ? GI->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	return ftcore::inputHoldToggles(Get(), Action, Settings && Settings->Get().holdToggle);
}
} // namespace FTInputBindings
