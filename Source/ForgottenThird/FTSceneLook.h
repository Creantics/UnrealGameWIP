#pragma once

// The scene's photographic base, spawned from one ftcore::SceneLook: a movable sun, a sky light that
// captures the atmosphere in real time, the sky atmosphere, exponential height fog (volumetric when the
// look has it), the engine's volumetric cloud layer, and an unbound post-process volume at priority 0
// that carries the camera finish (exposure range and bias, white balance, saturation, contrast, bloom,
// vignette, grain). Every map builder lights its scene through SpawnLook and keeps only its own sun angle
// and local lamps, so all the worlds share one look until each gets its own. The builders' blocks and props
// are tints of one surface master, loaded with LoadSurface; a world map's shapes wear the photographed
// surfaces Data/Field/surfaces.json gives their role instead (LoadSurfaceTable, LoadSurfaceFor). See
// docs/SCENE_LOOK.md.

#include "CoreMinimal.h"

#include "ftcore/SceneLook.h"
#include "ftcore/SeaWaves.h"
#include "ftcore/SurfaceTable.h"

class UMaterialInterface;
class UTexture;
class UTexture2D;
class UWorld;
struct FActorSpawnParameters;

namespace FTScene
{
// The world surface master's package, built by FTBuildWorldMaterials.
inline constexpr const TCHAR* SurfaceMaterial = TEXT("/Game/Rendering/M_WorldSurface");
// The photographed surfaces' master and the folder of their textures and instances, built by FTBuildSurfaces.
inline constexpr const TCHAR* SurfaceTexMaterial = TEXT("/Game/Rendering/M_WorldSurfaceTex");
inline constexpr const TCHAR* SurfaceFolder = TEXT("/Game/Rendering/Surfaces");

// The big worlds' ground master, built by FTBuildGround: the ground, rock and road surfaces blended by slope and a
// road mask.
inline constexpr const TCHAR* GroundMaterial = TEXT("/Game/Rendering/M_WorldGround");

// The look's materials, built by FTBuildLookMaterials: a parameter collection that carries a world's wetness, water
// and glow to every material that reads it (ApplyLookParameters fills it), and the look lab's surfaces over it. See
// docs/PHOTOREAL_LOOK.md.
inline constexpr const TCHAR* LookCollection = TEXT("/Game/Look/MPC_Look");
inline constexpr const TCHAR* LookWaterMaterial = TEXT("/Game/Look/M_Look_Water");
inline constexpr const TCHAR* LookGlassMaterial = TEXT("/Game/Look/M_Look_Glass");
inline constexpr const TCHAR* LookChromeMaterial = TEXT("/Game/Look/M_Look_Chrome");
inline constexpr const TCHAR* LookWetStoneMaterial = TEXT("/Game/Look/M_Look_WetStone");
inline constexpr const TCHAR* LookGlowMaterial = TEXT("/Game/Look/M_Look_Glow");
// The tag of a map's practical lights (lamps, signs, fires): the look sets their colour, intensity and how much they
// light the fog.
inline constexpr const TCHAR* PracticalTag = TEXT("FTPractical");

// The collection's parameters and the SceneLook fields they carry. A vector's alpha is 1.
struct FLookScalarParameter
{
	const TCHAR* Name;
	float ftcore::SceneLook::*Field;
};
inline constexpr FLookScalarParameter LookScalarParameters[] = {
	{TEXT("Wetness"), &ftcore::SceneLook::wetness},
	{TEXT("WaterRoughness"), &ftcore::SceneLook::waterRoughness},
	{TEXT("WaterPhaseG"), &ftcore::SceneLook::waterPhaseG},
	{TEXT("WaterWaveCm"), &ftcore::SceneLook::waterWaveCm},
	{TEXT("WaterWaveSpeed"), &ftcore::SceneLook::waterWaveSpeed},
	{TEXT("WaterWaveStrength"), &ftcore::SceneLook::waterWaveStrength},
	{TEXT("GlowStrength"), &ftcore::SceneLook::glowStrength},
};
struct FLookVectorParameter
{
	const TCHAR* Name;
	float ftcore::SceneLook::*R;
	float ftcore::SceneLook::*G;
	float ftcore::SceneLook::*B;
};
inline constexpr FLookVectorParameter LookVectorParameters[] = {
	// Per metre; the water material takes the engine's per centimetre from them.
	{TEXT("WaterAbsorb"), &ftcore::SceneLook::waterAbsorbR, &ftcore::SceneLook::waterAbsorbG, &ftcore::SceneLook::waterAbsorbB},
	{TEXT("WaterScatter"), &ftcore::SceneLook::waterScatterR, &ftcore::SceneLook::waterScatterG, &ftcore::SceneLook::waterScatterB},
	{TEXT("GlowColor"), &ftcore::SceneLook::glowR, &ftcore::SceneLook::glowG, &ftcore::SceneLook::glowB},
};

// The environment's materials, built by FTBuildEnvMaterials: a second parameter collection for the sky, the clouds and
// the sea, filled beside the look's by ApplyLookParameters, and the materials over it, whose shading code is in
// Plugins/FTLook/Shaders/Private/Env. MPC_Look and its materials stay as they are; a material that needs both reads
// both (the sea does: its water's colour and ripples are the look's), the most a material may. See
// docs/PHOTOREAL_LOOK.md.
inline constexpr const TCHAR* EnvCollection = TEXT("/Game/Look/MPC_Env");
inline constexpr const TCHAR* EnvSkyMaterial = TEXT("/Game/Look/Env/M_Env_Sky");
// The cloud layer's material when a world has cloud cover (ApplyLook puts it on), and the engine's own, which it gives
// back when the cover goes.
inline constexpr const TCHAR* EnvCloudMaterial = TEXT("/Game/Look/Env/M_Env_Clouds");
inline constexpr const TCHAR* EngineCloudMaterial = TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst");
// The sea's surface: a single-layer water material for a grid mesh over open water.
inline constexpr const TCHAR* EnvSeaMaterial = TEXT("/Game/Look/Env/M_Env_Sea");
inline constexpr FLookScalarParameter EnvScalarParameters[] = {
	{TEXT("SkyStars"), &ftcore::SceneLook::skyStars},
	{TEXT("SkyStarMag"), &ftcore::SceneLook::skyStarMag},
	{TEXT("SkyGlow"), &ftcore::SceneLook::skyGlow},
	{TEXT("SkyGlowKelvin"), &ftcore::SceneLook::skyGlowKelvin},
	{TEXT("SkyGlowDeg"), &ftcore::SceneLook::skyGlowDeg},
	{TEXT("SkyGlowWidthDeg"), &ftcore::SceneLook::skyGlowWidthDeg},
	{TEXT("SkyFlat"), &ftcore::SceneLook::skyFlat},
	{TEXT("SkyFlatLuminance"), &ftcore::SceneLook::skyFlatLuminance},
	{TEXT("WindDeg"), &ftcore::SceneLook::windDeg},
	{TEXT("WindMps"), &ftcore::SceneLook::windMps},
	{TEXT("CloudCover"), &ftcore::SceneLook::cloudCover},
	{TEXT("CloudType"), &ftcore::SceneLook::cloudType},
	{TEXT("CloudAlbedo"), &ftcore::SceneLook::cloudAlbedo},
	{TEXT("CloudDensity"), &ftcore::SceneLook::cloudDensity},
	{TEXT("CloudDrift"), &ftcore::SceneLook::cloudDrift},
	{TEXT("CloudUnderglow"), &ftcore::SceneLook::cloudUnderglow},
	{TEXT("CloudUnderglowKelvin"), &ftcore::SceneLook::cloudUnderglowKelvin},
	{TEXT("SeaFoam"), &ftcore::SceneLook::seaFoam},
	{TEXT("SeaFoamBandCm"), &ftcore::SceneLook::seaFoamBandCm},
};
// The sea's waves aren't fields: ApplyLookParameters builds them from the look's swell, wind and fetch
// (ftcore::seaWaves) and writes each wave's packed vector (ftcore::packSeaWave) and the shared steepness.
inline constexpr const TCHAR* SeaWaveParameters[] = {
	TEXT("SeaWave0"), TEXT("SeaWave1"), TEXT("SeaWave2"), TEXT("SeaWave3"),
	TEXT("SeaWave4"), TEXT("SeaWave5"), TEXT("SeaWave6"), TEXT("SeaWave7"),
};
static_assert(UE_ARRAY_COUNT(SeaWaveParameters) == ftcore::kSeaWaveCount, "one vector a wave");
inline constexpr const TCHAR* SeaSteepnessParameter = TEXT("SeaQ");

// The ground master's layers, each wearing the surface its kind gives the role. Its parameters are named after them:
// "<Layer><Map>" for a texture, "<Layer>Tint", "<Layer>TileCm" and "<Layer>Roughness".
struct FGroundLayer
{
	ftcore::SurfaceRole Role;
	const TCHAR* Name;
};
inline constexpr FGroundLayer GroundLayers[] = {
	{ftcore::SurfaceRole::Ground, TEXT("Ground")},
	{ftcore::SurfaceRole::Rock, TEXT("Rock")},
	{ftcore::SurfaceRole::Road, TEXT("Road")},
};
// A surface's maps: the suffix of FTBuildSurfaces' texture and the ground master's name for it.
struct FSurfaceMap
{
	const TCHAR* Suffix;
	const TCHAR* Name;
};
inline constexpr FSurfaceMap SurfaceMaps[] = {
	{TEXT("D"), TEXT("Albedo")},
	{TEXT("N"), TEXT("Normal")},
	{TEXT("ARM"), TEXT("Arm")},
};

// The package of surface Id's instance of the photographed master: SurfaceFolder/<Id>/MI_Surface_<Id>.
FORGOTTENTHIRD_API FString SurfaceInstancePackage(const FString& Id);
// The package of surface Id's texture with Suffix (D, N or ARM: colour, DirectX normal, packed AO/roughness/metal):
// SurfaceFolder/<Id>/T_<Id>_<Suffix>.
FORGOTTENTHIRD_API FString SurfaceTexturePackage(const FString& Id, const TCHAR* Suffix);
// That texture, or null, quietly, when it hasn't been built.
FORGOTTENTHIRD_API UTexture* LoadSurfaceTexture(const FString& Id, const TCHAR* Suffix);

#if WITH_EDITOR
// A new mask for the ground master's "RoadMask", "ShapeMask" or "FlowMask" in Outer: Size x Size bytes, row by row
// along +Y and each row along +X, as ftcore::roadMask, ftcore::shapeMask and ftcore::flowMask fill them. They're kept as
// they are (linear, uncompressed, clamped at the edges, loaded whole), and the package holds them as a PNG.
FORGOTTENTHIRD_API UTexture2D* NewGroundMask(UObject* Outer, const TCHAR* Name, int32 Size, const uint8* Bytes, EObjectFlags Flags);
#endif

// Spawns the sun (pointing along SunRotation), sky light, atmosphere, fog, clouds and post-process volume
// into World, labelled Sun, SkyLight, SkyAtmosphere, HeightFog, VolumetricCloud and PostProcessVolume.
FORGOTTENTHIRD_API void SpawnLook(UWorld* World, const FActorSpawnParameters& Spawn, const ftcore::SceneLook& Look,
	const FRotator& SunRotation);

// Writes Look into the look actors SpawnLook put in World (the unbound priority-0 post-process volume, the atmosphere's
// sun, the sky light, the height fog and the clouds), in place: a field is written only where Look differs from
// Current, what the actors carry now (ftcore::SceneLook{} for a map as built), so a field a world leaves at the shared
// value keeps whatever the map has. The number of fields written, or -1, with nothing written, when World has no such
// volume yet. See docs/PHOTOREAL_LOOK.md.
FORGOTTENTHIRD_API int32 ApplyLook(UWorld* World, const ftcore::SceneLook& Look, const ftcore::SceneLook& Current);

// Writes Look's wetness, water and glow into World's instance of the look's parameter collection (LookCollection), and
// its sky, wind, clouds and sea into the environment's (EnvCollection). The number of parameters written; 0 when neither
// collection has been built (no material reads them then); -1, with nothing written, while World has no instance yet of
// one that has.
FORGOTTENTHIRD_API int32 ApplyLookParameters(UWorld* World, const ftcore::SceneLook& Look);

// Loads the world surface master: a "Color" vector held to a plausible albedo, "Roughness", "Metallic" and
// a low-frequency mottle on the world position ("Variation", "VariationScale"). Null, with an error logged,
// when it hasn't been built.
FORGOTTENTHIRD_API UMaterialInterface* LoadSurface();

// Reads Data/Field/surfaces.json into Table. True with an empty table when the file is missing; false, with
// the problems logged, when it's refused.
FORGOTTENTHIRD_API bool LoadSurfaceTable(ftcore::SurfaceTable& Table);

// The photographed surface Kind's Role wears, with the tint it wears it in: an instance of the photographed
// master taking a "Color" tint like the surface master's. Null when the table gives the role no surface, or,
// with a warning logged, when the surface hasn't been built.
FORGOTTENTHIRD_API UMaterialInterface* LoadSurfaceFor(const ftcore::SurfaceTable& Table, ftcore::WorldKind Kind,
	ftcore::SurfaceRole Role, FLinearColor& OutTint);
}
