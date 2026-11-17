#pragma once

// -WeatherSpec=key=value[,key=value...] for the commandlets that write the surface weather node (FTBuildSurfaces,
// FTBuildWorldMaterials): ftcore::applySurfaceWeatherOverrides over the Core defaults, so the wet rule converges by
// bakes, not compiles. Every run logs the numbers it writes ("WeatherSpec:"), the proof scripts' read-back.

#include "CoreMinimal.h"
#include "ForgottenThird.h"
#include "FTText.h"
#include "Misc/Parse.h"

#include "ftcore/SurfaceWeather.h"

#include <string>

namespace FT
{
// Out is reset to the Core defaults, then the flag's pairs (if any) set over them. False (logged) on a pair the Core
// parser refuses: the run must stop rather than bake a spec nobody asked for.
inline bool ReadWeatherSpec(const FString& Params, const TCHAR* Who, ftcore::SurfaceWeatherSpec& Out)
{
	Out = ftcore::SurfaceWeatherSpec{};
	FString Text;
	FParse::Value(*Params, TEXT("WeatherSpec="), Text, false);
	std::string Bad;
	if (!ftcore::applySurfaceWeatherOverrides(Out, ToUtf8(Text), &Bad))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s -WeatherSpec: can't read \"%s\" (key=value, the SurfaceWeatherSpec field names)."),
			Who, *ToFString(Bad));
		return false;
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("WeatherSpec: %s [%s] soakDarken %.3f soakRough %.3f soakKeep %.3f trackRough %.3f track %.2f-%.2f m wander %.2f "
			 "puddleRough %.3f puddleChance %.2f puddleCover %.2f puddleCellM %.2f radius %.2f-%.2f kerbBias %.2f dampShare %.2f "
			 "edgePool pitch %.2f phase %.2f side %+.0f inset %.2f across %.2f-%.2f deep %.2f"),
		Who, Text.IsEmpty() ? TEXT("defaults") : *Text, Out.soakDarken, Out.soakRough, Out.soakKeep, Out.trackRough,
		2.f * Out.trackHalfLoM, 2.f * Out.trackHalfHiM, Out.trackWanderM, Out.puddleRough, Out.puddleChance, Out.puddleCover,
		Out.puddleCellM, Out.puddleRadiusLoM, Out.puddleRadiusHiM, Out.kerbBias, Out.dampShare, Out.edgePoolPitchM,
		Out.edgePoolPhaseM, Out.edgePoolSide, Out.edgePoolInsetM, 2.f * Out.edgePoolHalfLoM, 2.f * Out.edgePoolHalfHiM,
		Out.edgePoolDeep);
	return true;
}
} // namespace FT
