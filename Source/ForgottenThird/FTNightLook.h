#pragma once

// The one night spec (ftcore::NightLook) the world clock, the lamp pool and the world eye's night still all read, so a
// running night and a night still look the same. ftcore::NightLook's defaults, with these switches over them (read once):
//   -NightMoon=<share>  the directional light at full night, a share of its day intensity
//   -NightSky=<share>   the sky light's fill at full night (net of the moon for a real-time capture)
//   -NightFog=<share>   the height fog's own inscattering colour kept
//   -NightGlow=<scale>  the city glow added to the fog's inscattering (1 = the spec's warm glow, 0 = none)
//   -NightEV=<ev100>    the night exposure's centre, from half night on
//   -NightEVRange=<ev>  its half-width: auto exposure moves within centre +- this (0 locks it at the centre)
//   -NightWhiteK=<kelvin> the camera's white balance at full night
//   -NightLamp=<cd> -NightLampReach=<m> -NightLampK=<kelvin>   the street lamps' pool lights
//   -NightLampCone=<deg> -NightLampInner=<deg> -NightLampSource=<cm>   their cone half-angles and lens radius
//   -NightShop=<cd> -NightShopReach=<m> -NightShopK=<kelvin>   the shop spill lights
//   -NightCap=<share> -NightCapFloor=<share> -NightCapRadius=<m>  the crowding cap (0 share turns it off)
//
// The dusk (ftcore::DuskLook): a low sun's magenta-purple sky, warm sun, dim fill and lens flare, by the sun's elevation,
// under the night (FSkyFrame). The clock and a still at a dusk hour (-WorldEyeSunHour, -WorldEyeDusk) read the same.
//   -NoDusk  no dusk;  -DuskPeak=<deg> the sun's elevation at full dusk;  -DuskStart=<deg> -DuskEnd=<deg> its ends
//   -DuskFill=<scale> the sky light at full dusk;  -DuskFlare=<intensity> the lens flare at full dusk
// Data/Look/looks.json's dusk row for solhaven, when there is one, replaces the defaults (the switches still go over
// it): it adds the sun's elevation a dusk still stands at, the exposure bias and when the lights come on.

#include "CoreMinimal.h"

#include "ftcore/NightLook.h"
#include "ftcore/WorldClock.h"

class APostProcessVolume;
class USkyAtmosphereComponent;
class UWorld;

namespace FTNight
{
FORGOTTENTHIRD_API const ftcore::NightLook& Look();

inline FLinearColor Color(const std::array<float, 3>& C)
{
	return FLinearColor(C[0], C[1], C[2]);
}

FORGOTTENTHIRD_API const ftcore::DuskLook& Dusk();

// The light at an hour: the night frame (by NightAmount) over the dusk frame (by the sun's elevation).
struct FSkyFrame
{
	float NightAmount = 0.f;
	ftcore::NightFrame Night;
	ftcore::DuskFrame Dusk;
	// The share of the shop, window and street lights on (the night's amount, or the dusk row's lights if more) and the
	// daylight reaching a room behind glass (ftcore::duskLights, duskDaylight).
	float Lights = 0.f;
	float Daylight = 1.f;
	// The dusk row's exposure bias at this frame (EV, 0 by day and by night).
	float ExposureBias = 0.f;
};
FORGOTTENTHIRD_API FSkyFrame FrameAt(const ftcore::WorldClockSpec& Spec, double Hours);
// The window collection's light values at a frame: Night (the lights' share), Daylight, and the glass values a look
// sets (Data/Look/looks.json "glass": ProofShopNight, or -GlassProofNight=<level> over it). False without the collection.
FORGOTTENTHIRD_API bool WriteWindowLights(UWorld& World, float Lights, float Daylight);
// The look table's glass values (read once): the proof front's interior light at night (-GlassProofNight=<level> over it;
// under 0: none set, the collection keeps its default) and the share of the rooms' daylight a look keeps (interiorDay).
FORGOTTENTHIRD_API float ProofShopNight();
FORGOTTENTHIRD_API float InteriorDay();

inline FLinearColor Tint(const FLinearColor& C, const std::array<float, 3>& M)
{
	return FLinearColor(C.R * M[0], C.G * M[1], C.B * M[2], C.A);
}
// A light's or the fog's colour and intensity at the frame, from its day value.
inline FLinearColor SunColor(const FLinearColor& Day, const FSkyFrame& F)
{
	return FMath::Lerp(Tint(Day, F.Dusk.sunMul), Color(Look().moonTint), F.Night.tintMix);
}
inline FLinearColor SkyColor(const FLinearColor& Day, const FSkyFrame& F)
{
	return FMath::Lerp(Tint(Day, F.Dusk.fillMul), Color(Look().skyTint), F.Night.tintMix);
}
inline float SkyScale(const FSkyFrame& F, bool bRealTime)
{
	return (bRealTime ? F.Night.skyScaleRealTime : F.Night.skyScale) * F.Dusk.fillScale;
}
inline FLinearColor FogColor(const FLinearColor& Day, const FSkyFrame& F)
{
	return Tint(Day, F.Dusk.fogMul) * F.Night.fogKeep + Color(F.Night.fogGlow);
}

// The dusk's sky and lens: the sky atmospheres' luminance factor and the post-process volumes' lens flare and bloom.
// Their own values are read as a dusk starts (after the look has written them) and written back as it ends; between,
// they are written when the amount has moved a step. Allocates only as a dusk starts.
class FORGOTTENTHIRD_API FDuskScene
{
public:
	// Returns true when it wrote.
	bool Apply(UWorld& World, const ftcore::DuskFrame& Frame, bool bForce);
	// The look has just written the volumes (its bloom among them): read their bloom again and write the dusk over it.
	void Settled(UWorld& World, const ftcore::DuskFrame& Frame);
	int32 NumSkies() const { return Skies.Num(); }
	int32 NumVolumes() const { return Volumes.Num(); }

private:
	void Gather(UWorld& World);
	struct FSky
	{
		TWeakObjectPtr<USkyAtmosphereComponent> Sky;
		FLinearColor Factor = FLinearColor::White;
	};
	struct FVolume
	{
		TWeakObjectPtr<APostProcessVolume> Volume;
		bool bFlare = false, bThreshold = false, bBloom = false;
		float Flare = 1.f, Threshold = 8.f, Bloom = 0.675f;
	};
	TArray<FSky> Skies;
	TArray<FVolume> Volumes;
	float Applied = 0.f;
};

// A still at an hour (-WorldEyeSunHour): the map's lights, sky lights, fog, sky, lens and windows' Night as the clock
// would set them from their built (day) values. Logs what it did.
FORGOTTENTHIRD_API void ApplyStill(UWorld& World, const FSkyFrame& Frame);
} // namespace FTNight
