#pragma once

// Rain under cover, the material side (ftcore/RainCover.h is its reference). A top-down depth capture around the camera
// (UFTRainCoverSubsystem) fills RT_RainCover and writes MPC_RainCover's CoverFrame (centre x, y, size, top Z, cm) and
// CoverOn (1 with a capture). Materials read it through Term: a sprite's openness (0 under cover: no streak or splash)
// or a surface's wet share (0 deep under cover, a fringe at the open sides). With no capture CoverOn is 0 and every
// point reads open, so a material built with the term looks as it did before.
// Off by default (ftcore::rainCoverEnabled): only with -RainCover do the builds put the term in (Term and Sheltered;
// off, Term is null and Sheltered passes the wetness through) and make the assets, and the game run the capture.
// The builds make both assets when missing (LoadOrBuild, LoadOrBuildTarget); they are saved in place, never deleted.

#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"

#include "ForgottenThird.h"
#include "ftcore/RainCover.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

namespace FTRainCover
{
inline constexpr const TCHAR* Collection = TEXT("/Game/Rendering/Rain/MPC_RainCover");

// -RainCover given and -NoRainCover not (FTLookSwitches: the command line in a commandlet, the boot row too in a game).
inline bool Enabled()
{
	const TCHAR* Switches = FTLookSwitches();
	return ftcore::rainCoverEnabled(FParse::Param(Switches, UTF8_TO_TCHAR(ftcore::kRainCoverSwitch)),
		FParse::Param(Switches, UTF8_TO_TCHAR(ftcore::kRainCoverOffSwitch)));
}

inline FName Frame() { return FName(UTF8_TO_TCHAR(ftcore::kRainCoverParamFrame)); }
inline FName On() { return FName(UTF8_TO_TCHAR(ftcore::kRainCoverParamOn)); }
inline FString TargetPackage() { return UTF8_TO_TCHAR(ftcore::kRainCoverTarget); }

template <typename T>
T* LoadAt(const FString& Package)
{
	const FString Path = Package + TEXT(".") + FPackageName::GetShortName(Package);
	return LoadObject<T>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

inline UMaterialParameterCollection* Load() { return LoadAt<UMaterialParameterCollection>(Collection); }
inline UTextureRenderTarget2D* LoadTarget() { return LoadAt<UTextureRenderTarget2D>(TargetPackage()); }

// The collection, resolved once per process (a tick never goes to disk); a miss is remembered.
inline UMaterialParameterCollection* Resolved()
{
	static TWeakObjectPtr<UMaterialParameterCollection> Cached;
	static bool bMissed = false;
	if (UMaterialParameterCollection* Asset = Cached.Get())
	{
		return Asset;
	}
	if (bMissed)
	{
		return nullptr;
	}
	UMaterialParameterCollection* Asset = Load();
	Cached = Asset;
	bMissed = Asset == nullptr;
	return Asset;
}

// World's instance of the collection, or null when it was never built.
inline UMaterialParameterCollectionInstance* Params(UWorld& World)
{
	UMaterialParameterCollection* Asset = Resolved();
	return Asset ? World.GetParameterCollectionInstance(Asset) : nullptr;
}

#if WITH_EDITOR
namespace Detail
{
inline FGuid IdFor(const FName& Name) { return FGuid::NewDeterministicGuid(FString(TEXT("FTRainCover.")) + Name.ToString()); }

inline bool Save(UPackage* Package, UObject* Asset)
{
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	return UPackage::SavePackage(Package, Asset, *File, Args);
}

template <typename T>
T* Node(UMaterial* Material, int32 X, int32 Y)
{
	T* E = NewObject<T>(Material, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(E);
	E->Material = Material;
	E->MaterialExpressionEditorX = X;
	E->MaterialExpressionEditorY = Y;
	E->UpdateMaterialExpressionGuid(true, false);
	return E;
}
} // namespace Detail

// The collection, built and saved when missing (or a parameter is). Null if it could not be saved.
inline UMaterialParameterCollection* LoadOrBuild()
{
	UMaterialParameterCollection* Asset = Load();
	if (Asset && Asset->GetParameterId(Frame()).IsValid() && Asset->GetParameterId(On()).IsValid())
	{
		return Asset;
	}
	UPackage* Package = Asset ? Asset->GetPackage() : CreatePackage(Collection);
	if (!Asset)
	{
		Asset = NewObject<UMaterialParameterCollection>(Package, *FPackageName::GetShortName(Collection), RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
	}
	if (!Asset->GetParameterId(Frame()).IsValid())
	{
		FCollectionVectorParameter& P = Asset->VectorParameters.AddDefaulted_GetRef();
		P.ParameterName = Frame();
		P.DefaultValue = FLinearColor(0.f, 0.f, 0.f, 0.f);
		P.Id = Detail::IdFor(Frame());
	}
	if (!Asset->GetParameterId(On()).IsValid())
	{
		FCollectionScalarParameter& P = Asset->ScalarParameters.AddDefaulted_GetRef();
		P.ParameterName = On();
		P.DefaultValue = 0.f; // no capture: every point open
		P.Id = Detail::IdFor(On());
	}
	Asset->PostEditChange();
	return Detail::Save(Package, Asset) ? Asset : nullptr;
}

// The render target, built and saved when missing: resolution² R32F (depth, cm), clamped, cleared far (open).
inline UTextureRenderTarget2D* LoadOrBuildTarget()
{
	const ftcore::RainCoverSpec Spec;
	if (UTextureRenderTarget2D* Existing = LoadTarget())
	{
		return Existing;
	}
	const FString PackageName = TargetPackage();
	UPackage* Package = CreatePackage(*PackageName);
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
	Target->RenderTargetFormat = RTF_R32f;
	Target->SizeX = Spec.resolution;
	Target->SizeY = Spec.resolution;
	Target->AddressX = TA_Clamp;
	Target->AddressY = TA_Clamp;
	Target->ClearColor = FLinearColor(1e7f, 1e7f, 1e7f, 1.f); // beyond the far plane: open
	Target->bAutoGenerateMips = false;
	Target->Filter = TF_Nearest; // a depth: never blended across an eave
	FAssetRegistryModule::AssetCreated(Target);
	Target->PostEditChange();
	return Detail::Save(Package, Target) ? Target : nullptr;
}

// The cover term as a custom node (ftcore::rainCoverHlsl): bSprite gives the openness at the pixel (0 under cover),
// else the wet share with the fringe, read out from a wall's face along Normal (the vertex normal when null: never the
// normal-mapped one, which would feed back into the normal). Null when either asset is missing, and without -RainCover
// (Enabled: then nothing is built or saved).
inline UMaterialExpression* Term(UMaterial* Material, bool bSprite, int32 X, int32 Y, UMaterialExpression* Normal = nullptr)
{
	if (!Enabled())
	{
		return nullptr;
	}
	UMaterialParameterCollection* Asset = LoadOrBuild();
	UTextureRenderTarget2D* Target = LoadOrBuildTarget();
	if (!Asset || !Target)
	{
		return nullptr;
	}
	UMaterialExpressionTextureObject* Tex = Detail::Node<UMaterialExpressionTextureObject>(Material, X - 300, Y - 120);
	Tex->Texture = Target;
	Tex->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Target);
	auto Read = [&](const FName& Name, int32 Dy)
	{
		UMaterialExpressionCollectionParameter* P = Detail::Node<UMaterialExpressionCollectionParameter>(Material, X - 300, Y + Dy);
		P->Collection = Asset;
		P->ParameterName = Name;
		P->ParameterId = Asset->GetParameterId(Name);
		return P;
	};
	UMaterialExpressionCustom* C = Detail::Node<UMaterialExpressionCustom>(Material, X, Y);
	C->Description = bSprite ? TEXT("RainCoverOpen") : TEXT("RainCoverWet");
	C->OutputType = CMOT_Float1;
	C->Code = UTF8_TO_TCHAR(ftcore::rainCoverHlsl(ftcore::RainCoverSpec{}, bSprite).c_str());
	C->Inputs.Reset();
	auto In = [C](const TCHAR* Name, UMaterialExpression* From)
	{
		FCustomInput& Slot = C->Inputs.AddDefaulted_GetRef();
		Slot.InputName = Name;
		Slot.Input.Connect(0, From);
	};
	In(TEXT("CoverTex"), Tex);
	In(TEXT("CoverFrame"), Read(Frame(), -60));
	In(TEXT("CoverOn"), Read(On(), 0));
	In(TEXT("WorldPos"), Detail::Node<UMaterialExpressionWorldPosition>(Material, X - 300, Y + 60));
	if (!bSprite)
	{
		In(TEXT("Normal"), Normal ? Normal : Detail::Node<UMaterialExpressionVertexNormalWS>(Material, X - 300, Y + 120));
	}
	return C;
}

// Wetness under cover: Wetness times the cover's wet share (Term, surface kind); Wetness itself without -RainCover (as
// before the cover), or, logged, when the cover assets are missing. Normal as in Term; a deferred decal (no vertex
// normal) passes a constant up vector.
inline UMaterialExpression* Sheltered(UMaterial* Material, UMaterialExpression* Wetness, int32 X, int32 Y, UMaterialExpression* Normal = nullptr)
{
	if (!Wetness || !Enabled())
	{
		return Wetness;
	}
	UMaterialExpression* Cover = Term(Material, false, X, Y, Normal);
	if (!Cover)
	{
		UE_LOG(LogTemp, Warning, TEXT("FTRainCover: the cover assets are missing; %s soaks under cover too."), *Material->GetName());
		return Wetness;
	}
	UMaterialExpressionMultiply* M = Detail::Node<UMaterialExpressionMultiply>(Material, X + 250, Y);
	M->A.Connect(0, Wetness);
	M->B.Connect(0, Cover);
	return M;
}

// A constant world up, the normal a ground decal's cover reads with.
inline UMaterialExpression* Up(UMaterial* Material, int32 X, int32 Y)
{
	UMaterialExpressionConstant3Vector* N = Detail::Node<UMaterialExpressionConstant3Vector>(Material, X, Y);
	N->Constant = FLinearColor(0.f, 0.f, 1.f);
	return N;
}
#endif
} // namespace FTRainCover
