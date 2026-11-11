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
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
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

// Whether a held action is pressed once to toggle: its own mode, else the game-wide hold-toggle setting.
inline bool Toggles(const UWorld* World, ftcore::InputAction Action)
{
	const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	const UFTSettingsSubsystem* Settings = GI ? GI->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	return ftcore::inputHoldToggles(Get(), Action, Settings && Settings->Get().holdToggle);
}
} // namespace FTInputBindings
