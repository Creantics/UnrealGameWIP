#pragma once

// The one night spec (ftcore::NightLook) the world clock, the lamp pool and the world eye's night still all read, so a
// running night and a night still look the same. ftcore::NightLook's defaults, with these switches over them (read once):
//   -NightMoon=<share>  the directional light at full night, a share of its day intensity
//   -NightSky=<share>   the sky light's fill at full night (net of the moon for a real-time capture)
//   -NightFog=<share>   the height fog's own inscattering colour kept
//   -NightGlow=<scale>  the city glow added to the fog's inscattering (1 = the spec's warm glow, 0 = none)
//   -NightEV=<ev100>    the exposure locked from half night on
//   -NightLamp=<cd> -NightLampReach=<m> -NightLampK=<kelvin>   the street lamps' pool lights
//   -NightShop=<cd> -NightShopReach=<m> -NightShopK=<kelvin>   the shop spill lights
//   -NightCap=<share> -NightCapFloor=<share> -NightCapRadius=<m>  the crowding cap (0 share turns it off)

#include "CoreMinimal.h"

#include "ftcore/NightLook.h"

namespace FTNight
{
FORGOTTENTHIRD_API const ftcore::NightLook& Look();

inline FLinearColor Color(const std::array<float, 3>& C)
{
	return FLinearColor(C[0], C[1], C[2]);
}
} // namespace FTNight
