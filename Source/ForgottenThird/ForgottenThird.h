// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

// The one gate on the look and debug switches (-Night*, -Dusk*, -NoDusk, -DayShade*, -NoDayShade, -Glass*, -LampPool*,
// -LampShadows, -NightCharFill, -TimeOfDay, -NoDayClock, -WindowParallax, -ShopDayLevel, -ShopNightLevel, -WorldEyeShot
// and so every -WorldEye* still switch, -WorldEyeWet): every reader takes its command line from here. Outside Shipping
// it is the process's; in Shipping it is empty, so a Shipping build ignores every one of them and runs on the defaults.
inline const TCHAR* FTLookSwitches()
{
#if !UE_BUILD_SHIPPING
	return FCommandLine::Get();
#else
	return TEXT("");
#endif
}

// -WorldEyeWet=<0..1>: the ground's wetness for a still or a run, in place of the look's times the clock's share; -1 when
// not given (or in Shipping).
inline float FTWorldEyeWet()
{
	float Wet = -1.f;
	FParse::Value(FTLookSwitches(), TEXT("WorldEyeWet="), Wet);
	return Wet < 0.f ? -1.f : FMath::Clamp(Wet, 0.f, 1.f);
}


FORGOTTENTHIRD_API DECLARE_LOG_CATEGORY_EXTERN(LogForgottenThird, Log, All);

// Asks the engine to exit, the process ending with Code (0 to 255). The engine's graceful exit
// (FPlatformMisc::RequestExitWithStatus(false, ...)) returns 0 from GuardedMain whatever it was asked, so a
// non-zero Code is kept and, once the engine has torn its world down (FCoreDelegates::OnExit, after
// GEngine->PreExit), the process ends with it the way the engine ends a commandlet: the log flushed, then
// RequestExitWithStatus(true, Code). Code 0 keeps the whole graceful shutdown. The first non-zero Code wins.
FORGOTTENTHIRD_API void FTRequestExit(int32 Code, const TCHAR* CallSite);
