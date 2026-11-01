// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"


FORGOTTENTHIRD_API DECLARE_LOG_CATEGORY_EXTERN(LogForgottenThird, Log, All);

// Asks the engine to exit, the process ending with Code (0 to 255). The engine's graceful exit
// (FPlatformMisc::RequestExitWithStatus(false, ...)) returns 0 from GuardedMain whatever it was asked, so a
// non-zero Code is kept and, once the engine has torn its world down (FCoreDelegates::OnExit, after
// GEngine->PreExit), the process ends with it the way the engine ends a commandlet: the log flushed, then
// RequestExitWithStatus(true, Code). Code 0 keeps the whole graceful shutdown. The first non-zero Code wins.
FORGOTTENTHIRD_API void FTRequestExit(int32 Code, const TCHAR* CallSite);
