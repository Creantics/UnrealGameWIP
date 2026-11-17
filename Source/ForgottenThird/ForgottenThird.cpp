// Copyright Epic Games, Inc. All Rights Reserved.

#include "ForgottenThird.h"
#include "Engine/World.h"
#include "HAL/CriticalSection.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Templates/UniquePtr.h"

#include "ftcore/DemoBoot.h"
#include "ftcore/Version.h"

#include <string>

DEFINE_LOG_CATEGORY(LogForgottenThird);

namespace
{
uint8 GFTExitCode = 0;

FCriticalSection GBootLock;
FString GBootMap;          // the world being played (set by FTDemoBootSetMap), empty until one initialises
bool GBootMapGuessed = false;
// Per map and mode ("<map>|1" with the command line, "<map>|0" without): the switch line handed out. Boxed so a
// pointer handed out stays put as the map grows.
TMap<FString, TUniquePtr<FString>> GBootLines;

const ftcore::DemoBootTable& BootTable()
{
	static const ftcore::DemoBootTable Table = []()
	{
		ftcore::DemoBootTable T;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Field/demo_boot.json");
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("DemoBoot: no %s; no map has boot defaults."), *File);
			return T;
		}
		std::string Error;
		const FTCHARToUTF8 Utf8(*Text);
		if (!ftcore::parseDemoBoot(std::string_view(Utf8.Get(), static_cast<size_t>(Utf8.Length())), T, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("DemoBoot: %s refused: %s"), *File, UTF8_TO_TCHAR(Error.c_str()));
		}
		return T;
	}();
	return Table;
}

// The launch's map before any world initialises: the command line's map (the first bare word that isn't the project),
// else GameDefaultMap. Empty in the editor proper (its play-in-editor worlds set the map as they start).
FString GuessLaunchMap()
{
	if (GIsEditor && !IsRunningGame())
	{
		return FString();
	}
	const TCHAR* Cmd = FCommandLine::Get();
	FString Token;
	while (FParse::Token(Cmd, Token, false))
	{
		if (!Token.StartsWith(TEXT("-")) && !Token.EndsWith(TEXT(".uproject")) && !Token.Contains(TEXT("=")))
		{
			return Token;
		}
	}
	FString Default;
	GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameDefaultMap"), Default, GEngineIni);
	return Default;
}
} // namespace

const TCHAR* FTBootSwitches(bool bWithCommandLine)
{
#if UE_BUILD_SHIPPING
	// A Shipping build reads no switch from its command line: the map's boot row alone (none with -NoDemoBoot).
	bWithCommandLine = false;
#endif
	const TCHAR* Cmd = FCommandLine::Get();
	if (IsRunningCommandlet() || FParse::Param(Cmd, TEXT("NoDemoBoot")))
	{
		return bWithCommandLine ? Cmd : TEXT("");
	}
	FScopeLock Lock(&GBootLock);
	if (GBootMap.IsEmpty() && !GBootMapGuessed)
	{
		GBootMapGuessed = true;
		GBootMap = GuessLaunchMap();
	}
	const FString Key = GBootMap + (bWithCommandLine ? TEXT("|1") : TEXT("|0"));
	if (const TUniquePtr<FString>* Found = GBootLines.Find(Key))
	{
		return **Found->Get();
	}
	const FTCHARToUTF8 Map(*GBootMap);
	const ftcore::DemoBootRow* Row = ftcore::demoBootFind(BootTable(), std::string_view(Map.Get(), static_cast<size_t>(Map.Length())));
	const FTCHARToUTF8 Line(bWithCommandLine ? Cmd : TEXT(""));
	const std::string Merged = ftcore::demoBootSwitches(std::string_view(Line.Get(), static_cast<size_t>(Line.Length())), Row);
	if (Row && bWithCommandLine)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("DemoBoot: %s runs with its boot switches after the command line (-NoDemoBoot turns them off): %s [%s]"),
			*GBootMap, UTF8_TO_TCHAR(Row->switches.c_str()), UTF8_TO_TCHAR(Row->status.c_str()));
	}
	TUniquePtr<FString>& Slot = GBootLines.Add(Key, MakeUnique<FString>(UTF8_TO_TCHAR(Merged.c_str())));
	return **Slot;
}

void FTDemoBootSetMap(const FString& MapName)
{
	FScopeLock Lock(&GBootLock);
	GBootMap = MapName;
	GBootMapGuessed = true;
}

void FTDemoBootNoteWorld(const UObject* WorldOrOuter)
{
	const UWorld* World = Cast<UWorld>(WorldOrOuter);
	if (World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE))
	{
		FTDemoBootSetMap(World->GetMapName());
	}
}

void FTRequestExit(int32 Code, const TCHAR* CallSite)
{
	const uint8 Status = static_cast<uint8>(FMath::Clamp(Code, 0, 255));
	if (Status != 0 && GFTExitCode == 0)
	{
		GFTExitCode = Status;
		FCoreDelegates::OnExit.AddLambda([]()
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Exiting with code %d."), static_cast<int32>(GFTExitCode));
			if (GLog)
			{
				GLog->Flush();
			}
			FPlatformMisc::RequestExitWithStatus(true, GFTExitCode, TEXT("FTRequestExit"));
		});
	}
	FPlatformMisc::RequestExitWithStatus(false, Status, CallSite);
}

class FForgottenThirdModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogForgottenThird, Log, TEXT("ftcore %s"), UTF8_TO_TCHAR(ftcore::versionString()));
		// The demo boot row follows the game or play-in-editor world being initialised (before its subsystems start).
		PreWorldInitHandle = FWorldDelegates::OnPreWorldInitialization.AddLambda([](UWorld* World, const UWorld::InitializationValues)
		{
			if (World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE))
			{
				FTDemoBootSetMap(World->GetMapName());
			}
		});
	}

	virtual void ShutdownModule() override
	{
		FWorldDelegates::OnPreWorldInitialization.Remove(PreWorldInitHandle);
		PreWorldInitHandle.Reset();
	}

private:
	FDelegateHandle PreWorldInitHandle;
};

IMPLEMENT_PRIMARY_GAME_MODULE( FForgottenThirdModule, ForgottenThird, "ForgottenThird" );
