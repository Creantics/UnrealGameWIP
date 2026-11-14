#pragma once

// The player's walking and fighting bindings (ftcore::InputBindings): the defaults, with the player's overrides from
// Saved/InputBindings.json read over them (a refused file is logged and the defaults stand). Readers ask for the key of
// an action on each device and whether a held action is pressed once to toggle.
//
// The file is read at first use and again whenever ReloadIfChanged finds it added, removed or edited: the player
// controller asks as its input is set up (each map), when the game window gets the focus back and as the pause menu
// closes. A reload rebuilds everything built from the bindings in place and moves Generation(), so a reader that keeps
// something it built from them (the fight HUD's prompt keys, the explore pawn's and the controller's mapping contexts)
// rebuilds when it moves.
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
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
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

// Everything built from the bindings. One instance (BindingsStore), rewritten in place by Reload, so the addresses
// readers hold (Get(), Unbound(), a Key()) stay good across a reload. Game thread only.
struct FBindingsStore
{
	ftcore::InputBindings Bindings = ftcore::defaultInputBindings();
	// What the saved file's import left unbound (a newer default on a key the player gave to something else); a
	// settings screen shows each as "unbound: choose a key".
	ftcore::InputUnbound Unbound;
	// The engine key for each action on each device; EKeys::Invalid when unbound.
	std::array<std::array<FKey, ftcore::kInputActionCount>, ftcore::kInputDeviceCount> Keys;
	// The bindings' generation, their change signal: each load moves it, so it is never 0 once loaded.
	uint32 Generation = 0;
	bool bLoaded = false;
	// The file as the last load found it (bFileThere false: there was none), for ReloadIfChanged.
	bool bFileThere = false;
	FDateTime FileTime;
	int64 FileSize = -1;
};

inline FBindingsStore& BindingsStore()
{
	static FBindingsStore Store;
	return Store;
}

// The bindings changed: readers rebuild what they built from them. Reload calls it; on its own it rebuilds nothing
// here, so a change to the bindings goes through Reload.
inline void NoteChanged()
{
	++BindingsStore().Generation;
}

// (Re)reads Saved/InputBindings.json over the defaults, rebuilds the key table, and moves the generation (which drops
// the controls lines HintLine and UnboundLine keep). The hook for a rebinding screen: it writes the file, then calls
// this. Reads the file: never per frame.
inline void Reload()
{
	FBindingsStore& Store = BindingsStore();
	const FString Path = GetFilePath();
	// The stat first, so a refused file is not re-read (and re-logged) at every check.
	const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
	Store.bFileThere = Stat.bIsValid && !Stat.bIsDirectory;
	Store.FileTime = Store.bFileThere ? Stat.ModificationTime : FDateTime::MinValue();
	Store.FileSize = Store.bFileThere ? Stat.FileSize : -1;
	const bool bWasLoaded = Store.bLoaded;
	Store.bLoaded = true;
	Store.Bindings = ftcore::defaultInputBindings();
	Store.Unbound = ftcore::InputUnbound();
	FString Text;
	if (FFileHelper::LoadFileToString(Text, *Path))
	{
		std::string Error;
		if (!ftcore::importInputBindings(FT::ToUtf8(Text), Store.Bindings, &Error, &Store.Unbound))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("InputBindings: %s refused (%s); using the defaults."), *Path, *FT::ToFString(Error));
			Store.Bindings = ftcore::defaultInputBindings();
			Store.Unbound = ftcore::InputUnbound();
		}
		const ftcore::InputUnbound& Left = Store.Unbound;
		for (size_t D = 0; D < ftcore::kInputDeviceCount && Left.count > 0; ++D)
		{
			for (size_t A = 0; A < ftcore::kInputActionCount; ++A)
			{
				if (Left.left[D][A])
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("InputBindings: %s unbound on the %s: your own key sits on its default; choose a key."),
						*FT::ToFString(ftcore::inputActionKey(static_cast<ftcore::InputAction>(A))), D == 0 ? TEXT("keyboard") : TEXT("pad"));
				}
			}
		}
	}
	for (size_t D = 0; D < ftcore::kInputDeviceCount; ++D)
	{
		for (size_t A = 0; A < ftcore::kInputActionCount; ++A)
		{
			const char* Name = ftcore::inputKey(Store.Bindings, static_cast<ftcore::InputAction>(A), static_cast<ftcore::InputDevice>(D));
			FKey& Slot = Store.Keys[D][A];
			Slot = Name[0] ? FKey(FName(Name)) : EKeys::Invalid;
			if (Name[0] && !Slot.IsValid())
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("InputBindings: %s is not a key the engine knows; %s is unbound there."),
					*FT::ToFString(Name), *FT::ToFString(ftcore::inputActionKey(static_cast<ftcore::InputAction>(A))));
			}
		}
	}
	if (bWasLoaded)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("InputBindings: %s %s; the bindings are read again."), *Path,
			Store.bFileThere ? TEXT("changed") : TEXT("is gone"));
	}
	NoteChanged();
}

// The store, loaded (the first use reads the file). Per frame this is one flag check.
inline const FBindingsStore& Loaded()
{
	const FBindingsStore& Store = BindingsStore();
	if (!Store.bLoaded)
	{
		Reload();
	}
	return Store;
}

// Reloads when the file was added, removed or edited (its time stamp or size moved) since the last load; true when it
// did. One stat of the file: call it at a map's start or on a focus change, never per frame.
inline bool ReloadIfChanged()
{
	const FBindingsStore& Store = BindingsStore();
	if (!Store.bLoaded)
	{
		Reload();
		return true;
	}
	const FFileStatData Stat = IFileManager::Get().GetStatData(*GetFilePath());
	const bool bThere = Stat.bIsValid && !Stat.bIsDirectory;
	if (bThere == Store.bFileThere && (!bThere || (Stat.ModificationTime == Store.FileTime && Stat.FileSize == Store.FileSize)))
	{
		return false;
	}
	Reload();
	return true;
}

inline const ftcore::InputBindings& Get()
{
	return Loaded().Bindings;
}

// The bindings' generation now (after the first load, so it is not 0).
inline uint32 Generation()
{
	return Loaded().Generation;
}

// The import's unbound list.
inline const ftcore::InputUnbound& Unbound()
{
	return Loaded().Unbound;
}

// The engine key for the action on that device; EKeys::Invalid when unbound. Read from the table the last load built:
// no file access or allocation here.
inline const FKey& Key(ftcore::InputAction Action, ftcore::InputDevice Device)
{
	return Loaded().Keys[static_cast<size_t>(Device) % ftcore::kInputDeviceCount][static_cast<size_t>(Action) % ftcore::kInputActionCount];
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
// ("input.light"), so it names only keys the player has; an action the import left unbound shows NO KEY. Built per
// screen and device and kept until the bindings' generation moves (a reload); per frame an int compare.
inline const FString& HintLine(ftcore::InputHintScreen Screen, ftcore::InputDevice Device)
{
	static FString Lines[ftcore::kInputHintScreenCount][ftcore::kInputDeviceCount];
	static uint32 BuiltFrom[ftcore::kInputHintScreenCount][ftcore::kInputDeviceCount] = {}; // 0: not built yet
	const size_t ScreenIx = static_cast<size_t>(Screen) % ftcore::kInputHintScreenCount;
	const size_t DeviceIx = static_cast<size_t>(Device) % ftcore::kInputDeviceCount;
	const FBindingsStore& Store = Loaded();
	if (BuiltFrom[ScreenIx][DeviceIx] != Store.Generation)
	{
		const std::string Text = ftcore::inputHintLine(Store.Bindings, Screen, Device,
			[](const char* LabelKey, void*) { return FT::ToUtf8(FTUiText::Lookup(*FT::ToFString(LabelKey))); }, nullptr, &Store.Unbound);
		Lines[ScreenIx][DeviceIx] = FT::ToFString(Text);
		BuiltFrom[ScreenIx][DeviceIx] = Store.Generation;
	}
	return Lines[ScreenIx][DeviceIx];
}

// One line naming each action the import left unbound ("NO KEY: gift_heal (keyboard)"); empty when none. A fight shows
// it on screen at its start. Rebuilt when the generation moves.
inline const FString& UnboundLine()
{
	static FString Line;
	static uint32 BuiltFrom = 0; // 0: not built yet
	const FBindingsStore& Store = Loaded();
	if (BuiltFrom != Store.Generation)
	{
		Line = FT::ToFString(ftcore::inputUnboundLine(Store.Unbound));
		BuiltFrom = Store.Generation;
	}
	return Line;
}

// Whether the focus gate is off for this run: unattended, rendering off-screen or with no RHI (the demo and capture
// runs, which start unfocused), or a build that never renders. The autopilot reads no keys, so it needs no gate.
inline bool FocusGateOff()
{
	static const bool bOff = FApp::IsUnattended() || !FApp::CanEverRender() ||
		FParse::Param(FCommandLine::Get(), TEXT("RenderOffscreen")) || FParse::Param(FCommandLine::Get(), TEXT("nullrhi"));
	return bOff;
}

// Whether the game window has lost focus: the engine lets go of every key then, so a held press read as a release
// would fire a light nobody made. Hosts clear their held presses (PressHold) on it; toggled latches (run, aim, crouch)
// stay as they are, and movement is not stopped. Never true while the gate is off (FocusGateOff).
inline bool LostFocus()
{
	return !FocusGateOff() && !FApp::HasFocus();
}

// One line for the log: whether the gate is on and what the app's focus reads now.
inline void LogFocusGate(const TCHAR* Who)
{
	UE_LOG(LogForgottenThird, Display, TEXT("%s: input focus gate %s; app focus now %s."), Who,
		FocusGateOff() ? TEXT("off (unattended, off-screen, no RHI or no rendering)") : TEXT("on"),
		FApp::HasFocus() ? TEXT("yes") : TEXT("no"));
}

// Whether a held action is pressed once to toggle: its own mode, else the game-wide hold-toggle setting.
inline bool Toggles(const UWorld* World, ftcore::InputAction Action)
{
	const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	const UFTSettingsSubsystem* Settings = GI ? GI->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	return ftcore::inputHoldToggles(Get(), Action, Settings && Settings->Get().holdToggle);
}
} // namespace FTInputBindings
