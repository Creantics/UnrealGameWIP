#pragma once

// The player's walking and fighting bindings (ftcore::InputBindings): the defaults, with the player's overrides from
// Saved/InputBindings.json read over them once (a refused file is logged and the defaults stand). Readers ask for the
// key of an action on each device and whether a held action is pressed once to toggle.
//
// The arena's Guard reads it today; the other actions move over to it as their keys are settled.

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

// The engine key for the action on that device; EKeys::Invalid when unbound.
inline FKey Key(ftcore::InputAction Action, ftcore::InputDevice Device)
{
	const char* Name = ftcore::inputKey(Get(), Action, Device);
	return Name[0] ? FKey(FName(Name)) : EKeys::Invalid;
}

// Whether the action's key is down on either device.
inline bool IsDown(const APlayerController& PC, ftcore::InputAction Action)
{
	const FKey Keys = Key(Action, ftcore::InputDevice::Keys);
	const FKey Pad = Key(Action, ftcore::InputDevice::Pad);
	return (Keys.IsValid() && PC.IsInputKeyDown(Keys)) || (Pad.IsValid() && PC.IsInputKeyDown(Pad));
}

// Whether a held action is pressed once to toggle: its own mode, else the game-wide hold-toggle setting.
inline bool Toggles(const UWorld* World, ftcore::InputAction Action)
{
	const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	const UFTSettingsSubsystem* Settings = GI ? GI->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	return ftcore::inputHoldToggles(Get(), Action, Settings && Settings->Get().holdToggle);
}
} // namespace FTInputBindings
