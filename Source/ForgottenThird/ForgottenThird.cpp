// Copyright Epic Games, Inc. All Rights Reserved.

#include "ForgottenThird.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

#include "ftcore/Version.h"

DEFINE_LOG_CATEGORY(LogForgottenThird);

namespace
{
uint8 GFTExitCode = 0;
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
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE( FForgottenThirdModule, ForgottenThird, "ForgottenThird" );
