#include "FTNightLook.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
ftcore::NightLook Read()
{
	ftcore::NightLook L;
	const TCHAR* Cmd = FCommandLine::Get();
	FParse::Value(Cmd, TEXT("NightMoon="), L.moonShare);
	FParse::Value(Cmd, TEXT("NightSky="), L.skyFill);
	FParse::Value(Cmd, TEXT("NightFog="), L.fogKeep);
	FParse::Value(Cmd, TEXT("NightEV="), L.exposureEV100);
	FParse::Value(Cmd, TEXT("NightEVRange="), L.exposureRangeEV);
	FParse::Value(Cmd, TEXT("NightWhiteK="), L.whiteKelvin);
	FParse::Value(Cmd, TEXT("NightLamp="), L.lampCandela);
	FParse::Value(Cmd, TEXT("NightLampReach="), L.lampReachM);
	FParse::Value(Cmd, TEXT("NightLampK="), L.lampKelvin);
	FParse::Value(Cmd, TEXT("NightLampCone="), L.lampOuterDeg);
	FParse::Value(Cmd, TEXT("NightLampInner="), L.lampInnerDeg);
	FParse::Value(Cmd, TEXT("NightLampSource="), L.lampSourceCm);
	FParse::Value(Cmd, TEXT("NightShop="), L.shopCandela);
	FParse::Value(Cmd, TEXT("NightShopReach="), L.shopReachM);
	FParse::Value(Cmd, TEXT("NightShopK="), L.shopKelvin);
	FParse::Value(Cmd, TEXT("NightCap="), L.capShare);
	FParse::Value(Cmd, TEXT("NightCapFloor="), L.capFloor);
	FParse::Value(Cmd, TEXT("NightCapRadius="), L.capRadiusM);
	float Glow = 1.f;
	if (FParse::Value(Cmd, TEXT("NightGlow="), Glow))
	{
		for (float& C : L.glow)
		{
			C *= FMath::Max(Glow, 0.f);
		}
	}
	L.moonShare = FMath::Max(L.moonShare, 1e-4f);
	return L;
}
} // namespace

const ftcore::NightLook& FTNight::Look()
{
	static const ftcore::NightLook Spec = Read();
	return Spec;
}
