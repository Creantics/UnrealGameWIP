// Copyright Epic Games, Inc. All Rights Reserved.

#include "ForgottenThird.h"
#include "Modules/ModuleManager.h"

#include "ftcore/Version.h"

DEFINE_LOG_CATEGORY_STATIC(LogForgottenThird, Log, All);

class FForgottenThirdModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogForgottenThird, Log, TEXT("ftcore %s"), UTF8_TO_TCHAR(ftcore::versionString()));
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE( FForgottenThirdModule, ForgottenThird, "ForgottenThird" );
