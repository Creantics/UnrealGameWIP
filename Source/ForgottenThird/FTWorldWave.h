#pragma once

// The world wave's shared names: the assets FTBuildWaveMaterials writes, the collection parameters the host
// (UFTWorldWaveSubsystem) sets each frame, and the custom primitive data slots that carry a placed thing's world tag
// (ftcore::WorldTag, Core/include/ftcore/WorldWave.h) to its material.
//
// The parameters live in the look's collection, MPC_Look (FTScene::LookCollection), the same way FTSeason's Season does:
// a material may read two collections at most, and the foliage master already reads MPC_Look and MPC_Env, the kei car
// MPC_Windows. A build adds whichever are missing (saved in place, never deleted; ids come from the names, so a rebuild
// changes none); FTBuildLookMaterials builds them in. Adding them changes the collection's layout once, so every
// material reading MPC_Look recompiles once. They are not look parameters: SceneLook and looks.json never set them.
//
// MPC_Look's wave parameters (engine units, cm):
//   WorldWaveOrigin        vector  the wave's origin, world cm (w the front's look: 0 the refraction band, 1 ink in water (A), 2 blot dissolve (B): ftcore::WorldWaveParams::front, ink by default)
//                                  (every wave constant here comes from ftcore::worldWaveShaderConstants)
//   WorldWaveOriginScreen  vector  xy the origin's viewport UV, z +1 in front of the camera / -1 behind, w 1 while a
//                                  wave is set (0: every wave material passes through)
//   WorldWaveStreakFoot    vector  xy the streak's foot (the origin) in viewport UV, z 1 when drawable, w its view
//                                  depth cm
//   WorldWaveStreakTip     vector  the same for the streak's tip (StreakHeightCm over the origin)
//   WorldWaveRadius        scalar  the tongues' reach, cm (r + tongueLead w; 0 before the front moves and with no wave)
//   WorldWaveBand          scalar  the whole span, cm (w + tongueLead w); the inner edge is Radius - Band (r - w)
//   WorldWavePrelude       scalar  0..1 the streak's growth
//   WorldWaveDirection     scalar  0 Forward (the second world inside the sphere), 1 Reverse (outside)
//   WorldWaveInside        scalar  0..1 how far the camera is inside (ftcore::updateWorldWave's cameraInside): the
//                                  second world's sky and fog use it for Forward and 1 - it for Reverse
//   WorldWaveTime          scalar  seconds on the wave clock, for the moving noise
//   WorldWaveGround        scalar  the ground's height under the origin, world cm (the ground fog's floor)
//   WorldWaveKeep0, Keep1  vector  the second world's kept light sources (WorldWaveParams::keep): xyz world cm, w radius cm (0 off); every
//                                  other emissive dies behind the front
//   WorldWaveGain          vector  x the first world's lift, y the second's (Core waveGains: the sun and sky the second
//                                  world is taking down over the whole view, given back where it has not arrived), z the
//                                  cover, w the post-process's own overcast over the second world's sky (1 while the
//                                  cloud layer has not yet changed)
//
// MF_WorldWaveState (inputs BaseColor, Roughness, Emissive; outputs the same three) is the surface state behind the
// front, wired into the surface masters between their own values and the outputs (AddState below); PP_WorldWave is the
// post-process: the second world's grade, ground fog and the front's refraction band, masked per pixel.
//
// The tag reaches a material through custom primitive data (CPD) on the component, the same for a static mesh
// component and for a one-instance ISM split off a cell's HISM (per-instance data stays free for paint and the like):
//   slot kTagSlot    0 Both (never cut; also every primitive that never set it), 1 OldOnly, 2 NewOnly
//   slot kLocalSlot  a local front's radius, cm from the origin (a held thing cutting in on its own front); <= 0: the
//                    collection's WorldWaveRadius
//
// MF_WorldWaveCut (a material function, no inputs) gives three outputs a master wires in (AddCut below):
//   0 Clip  1 to keep the pixel, 0 to clip it (OpacityMask, or Opacity on a translucent master)
//   1 Ink   0 (kept so the masters wired to it need no rebuild)
//   2 Core  float3 0 (the same)

#include "CoreMinimal.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"

#if WITH_EDITOR
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

class UMaterial;

namespace FTWorldWave
{
inline constexpr const TCHAR* Folder = TEXT("/Game/Look/Wave");
inline constexpr const TCHAR* CollectionPath = TEXT("/Game/Look/MPC_Look"); // FTScene::LookCollection
inline constexpr const TCHAR* PostMaterialPath = TEXT("/Game/Look/Wave/PP_WorldWave");
inline constexpr const TCHAR* CutFunctionPath = TEXT("/Game/Look/Wave/MF_WorldWaveCut");
inline constexpr const TCHAR* StateFunctionPath = TEXT("/Game/Look/Wave/MF_WorldWaveState");
// v2's line-art post-process, deleted by the build if it is still on disk.
inline constexpr const TCHAR* OldInkMaterialPath = TEXT("/Game/Look/Wave/PP_WorldWave_Ink");
// A prop master's twin with the cut wired in: /Game/Look/Wave/<master name><TwinSuffix> (M_FTCharacter_WaveCut and
// M_FTCharacterTex_WaveCut; the road furniture's M_WorldSurface_WaveCut and its lenses' M_NightLamp_WaveCut). The
// dressing moves a tagged prop's slots onto MIDs of the twin.
inline constexpr const TCHAR* TwinSuffix = TEXT("_WaveCut");
inline constexpr const TCHAR* PropMasters[] = {
	TEXT("/Game/Characters/Shared/Materials/M_FTCharacter"),
	TEXT("/Game/Characters/Shared/Materials/M_FTCharacterTex"),
	TEXT("/Game/Rendering/M_WorldSurface"),
	TEXT("/Game/Rendering/Windows/M_NightLamp"),
};

// The collection's parameters.
inline constexpr const TCHAR* Origin = TEXT("WorldWaveOrigin");
inline constexpr const TCHAR* OriginScreen = TEXT("WorldWaveOriginScreen");
inline constexpr const TCHAR* StreakFoot = TEXT("WorldWaveStreakFoot");
inline constexpr const TCHAR* StreakTip = TEXT("WorldWaveStreakTip");
inline constexpr const TCHAR* Radius = TEXT("WorldWaveRadius");
inline constexpr const TCHAR* BandWidth = TEXT("WorldWaveBand");
inline constexpr const TCHAR* Prelude = TEXT("WorldWavePrelude");
inline constexpr const TCHAR* Direction = TEXT("WorldWaveDirection");
inline constexpr const TCHAR* CameraInside = TEXT("WorldWaveInside");
inline constexpr const TCHAR* WaveTime = TEXT("WorldWaveTime");
inline constexpr const TCHAR* Ground = TEXT("WorldWaveGround");
inline constexpr const TCHAR* Keep0 = TEXT("WorldWaveKeep0");
inline constexpr const TCHAR* Keep1 = TEXT("WorldWaveKeep1");
inline constexpr const TCHAR* Gain = TEXT("WorldWaveGain");

inline constexpr const TCHAR* VectorNames[] = {Origin, OriginScreen, StreakFoot, StreakTip, Keep0, Keep1, Gain};
inline constexpr const TCHAR* ScalarNames[] = {Radius, BandWidth, Prelude, Direction, CameraInside, WaveTime, Ground};

// The custom primitive data slots (FCustomPrimitiveData holds 36 floats; the window and decal data use the low ones).
inline constexpr int32 kTagSlot = 30;
inline constexpr int32 kLocalSlot = 31;
inline constexpr const TCHAR* TagParameter = TEXT("WorldWaveTag");
inline constexpr const TCHAR* LocalParameter = TEXT("WorldWaveLocalRadius");

// The streak's height over the origin, cm.
inline constexpr double StreakHeightCm = 200.0;

inline UMaterialParameterCollection* Load()
{
	const FString Path = FString(CollectionPath) + TEXT(".") + FPackageName::GetShortName(CollectionPath);
	return LoadObject<UMaterialParameterCollection>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

#if WITH_EDITOR
// Adds the wave parameters the collection lacks (all zero by default: no wave). Each id comes from its name, so a
// rebuild changes no id. Returns how many it added; the caller runs PostEditChange.
inline int32 AddParametersTo(UMaterialParameterCollection& Asset)
{
	int32 Added = 0;
	for (const TCHAR* Name : ScalarNames)
	{
		if (Asset.GetParameterId(FName(Name)).IsValid())
		{
			continue;
		}
		FCollectionScalarParameter& Scalar = Asset.ScalarParameters.AddDefaulted_GetRef();
		Scalar.ParameterName = FName(Name);
		Scalar.DefaultValue = 0.f;
		Scalar.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTWorldWave.")) + Name);
		++Added;
	}
	for (const TCHAR* Name : VectorNames)
	{
		if (Asset.GetParameterId(FName(Name)).IsValid())
		{
			continue;
		}
		FCollectionVectorParameter& Vector = Asset.VectorParameters.AddDefaulted_GetRef();
		Vector.ParameterName = FName(Name);
		Vector.DefaultValue = FLinearColor(0.f, 0.f, 0.f, 0.f);
		Vector.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTWorldWave.")) + Name);
		++Added;
	}
	return Added;
}

// The look's collection with the wave parameters, added and saved in place if any is missing. Null if the collection
// is missing (build it first with FTBuildLookMaterials) or could not be saved.
inline UMaterialParameterCollection* LoadOrBuild()
{
	UMaterialParameterCollection* Found = Load();
	if (!Found || AddParametersTo(*Found) == 0)
	{
		return Found;
	}
	Found->PostEditChange();
	UPackage* Package = Found->GetPackage();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	return UPackage::SavePackage(Package, Found, *File, Args) ? Found : nullptr;
}

// Wires MF_WorldWaveCut into a master being built: OpacityMask times Clip (an opaque master turns masked; a translucent
// one multiplies Opacity instead). Call it before the master's PostEditChange. When the function is missing
// (FTBuildWaveMaterials not yet run) it logs that and leaves the master as it was; returns whether the cut went in.
bool AddCut(const TCHAR* Tag, UMaterial* Material, int32 X, int32 Y);

// Wires MF_WorldWaveState into a master being built: whatever feeds its BaseColor, Roughness and Emissive goes through
// the state function first (an unconnected input takes the master's constant: 0.5 roughness, no emissive). Call it last,
// after AddCut, before PostEditChange. Logs and leaves the master as it was when the function is missing; returns
// whether the state went in. bCharacter: the character state (no leaks or puddles, grime that moves with the body, the
// thin line at the cut for front A); on a master that already has the state it is switched on there. A cloth master's
// fuzz colour (SubsurfaceColor) goes through a second call of the state as its BaseColor.
bool AddState(const TCHAR* Tag, UMaterial* Material, int32 X, int32 Y, bool bCharacter = false);

// Whether the master's MF_WorldWaveState call has its Character input on: a static bool whose value is true, or any
// other expression wired there. False for a master with no state call, an unconnected input or a static false.
bool CharacterOn(const UMaterial& Material);
#endif
} // namespace FTWorldWave
