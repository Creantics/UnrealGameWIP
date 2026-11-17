#pragma once

// The character rim as a material term (ftcore::look::rimWeight is its reference). Its inputs live in their own
// collection, MPC_FTRim: RimDir (the virtual direction, set once a frame by UFTRimComponent), RimColour (cool white by
// default; the world lane may drive it from the nearest lamp, its only write), RimStrength (0 by default: no rim), RimHairCap and RimHead (the
// writer's head point, for the hair's head normal). The
// garment and skin masters take the term through AddTerm, each instance scaling it by its own RimScale (1 by default).
// The masters' builds make the collection when it is missing (LoadOrBuild); it is saved in place, never deleted.

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/PackageName.h"

#include "ftcore/LookReview.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTwoSidedSign.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

namespace FTRim
{
inline constexpr const TCHAR* Collection = TEXT("/Game/Characters/Shared/Materials/MPC_FTRim");
inline constexpr const TCHAR* Dir = TEXT("RimDir");
inline constexpr const TCHAR* Colour = TEXT("RimColour");
inline constexpr const TCHAR* Strength = TEXT("RimStrength");
inline constexpr const TCHAR* HairCap = TEXT("RimHairCap");
inline constexpr const TCHAR* Head = TEXT("RimHead");
inline const FLinearColor DefaultColour(0.82f, 0.88f, 1.f); // a cool edge, against warm street light

// The side the rim leans to. RimDir is always camera-relative and written by UFTRimComponent alone (a rim only reads as
// an edge from the far side of the lead); the world lane writes RimColour only, and may say which side of the camera its
// nearest lamp is on: +1 camera-right, -1 camera-left, 0 the default (right). The component eases a flip over
// ftcore::look::kRimSideFlipSeconds.
inline float& PreferredSideSlot()
{
	static float Sign = 0.f;
	return Sign;
}

inline float GetPreferredSide() { return PreferredSideSlot(); }
inline void SetPreferredSide(float Sign) { PreferredSideSlot() = Sign; }

inline UMaterialParameterCollection* Load()
{
	const FString Path = FString(Collection) + TEXT(".") + FPackageName::GetShortName(Collection);
	return LoadObject<UMaterialParameterCollection>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

// The collection, resolved once per process: Load builds a path and goes to disk, so a tick never calls it. A miss is
// remembered (the game never builds it; the masters' builds call LoadOrBuild, not this).
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

// World's instance of the collection, or null when the collection was never built.
inline UMaterialParameterCollectionInstance* Params(UWorld& World)
{
	UMaterialParameterCollection* Asset = Resolved();
	return Asset ? World.GetParameterCollectionInstance(Asset) : nullptr;
}

#if WITH_EDITOR
namespace Detail
{
inline FGuid IdFor(const TCHAR* Name) { return FGuid::NewDeterministicGuid(FString(TEXT("FTRim.")) + Name); }

inline void AddScalar(UMaterialParameterCollection& Asset, const TCHAR* Name, float Default)
{
	FCollectionScalarParameter& P = Asset.ScalarParameters.AddDefaulted_GetRef();
	P.ParameterName = Name;
	P.DefaultValue = Default;
	P.Id = IdFor(Name);
}

inline void AddVector(UMaterialParameterCollection& Asset, const TCHAR* Name, const FLinearColor& Default)
{
	FCollectionVectorParameter& P = Asset.VectorParameters.AddDefaulted_GetRef();
	P.ParameterName = Name;
	P.DefaultValue = Default;
	P.Id = IdFor(Name);
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

inline UMaterialExpressionCollectionParameter* Read(UMaterial* Material, UMaterialParameterCollection* Asset, const TCHAR* Name, int32 X, int32 Y)
{
	UMaterialExpressionCollectionParameter* P = Node<UMaterialExpressionCollectionParameter>(Material, X, Y);
	P->Collection = Asset;
	P->ParameterName = Name;
	P->ParameterId = Asset->GetParameterId(Name);
	return P;
}
} // namespace Detail

// The collection, built and saved when missing (or when a parameter is missing from it). Null if it could not be saved.
inline UMaterialParameterCollection* LoadOrBuild()
{
	UMaterialParameterCollection* Asset = Load();
	if (Asset && Asset->GetParameterId(Dir).IsValid() && Asset->GetParameterId(Colour).IsValid() &&
		Asset->GetParameterId(Strength).IsValid() && Asset->GetParameterId(HairCap).IsValid() && Asset->GetParameterId(Head).IsValid())
	{
		return Asset;
	}
	UPackage* Package = Asset ? Asset->GetPackage() : CreatePackage(Collection);
	if (!Asset)
	{
		Asset = NewObject<UMaterialParameterCollection>(Package, *FPackageName::GetShortName(Collection), RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
	}
	if (!Asset->GetParameterId(Dir).IsValid())
	{
		Detail::AddVector(*Asset, Dir, FLinearColor(0.f, 0.f, 1.f, 0.f));
	}
	if (!Asset->GetParameterId(Colour).IsValid())
	{
		Detail::AddVector(*Asset, Colour, DefaultColour);
	}
	if (!Asset->GetParameterId(Strength).IsValid())
	{
		Detail::AddScalar(*Asset, Strength, ftcore::look::rimStrength(0.f));
	}
	if (!Asset->GetParameterId(HairCap).IsValid())
	{
		Detail::AddScalar(*Asset, HairCap, ftcore::look::kRimHairCap);
	}
	if (!Asset->GetParameterId(Head).IsValid())
	{
		Detail::AddVector(*Asset, Head, FLinearColor(0.f, 0.f, 0.f, 0.f));
	}
	Asset->PostEditChange();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	return UPackage::SavePackage(Package, Asset, *File, Args) ? Asset : nullptr;
}

// The rim term, as an expression (ftcore::look::rimWeight): pow(1 - saturate(N.V), kRimExponent) *
// smoothstep(kRimSideLow, kRimSideHigh, saturate(N.L)) * RimStrength * RimScale * RimColour, N the vertex normal turned
// to the face drawn (TwoSidedSign: the garment masters are two-sided, and a back face's own normal reads as grazing and
// glows whole), not the normal-mapped one, so fold ridges, seams and stitches inside the silhouette catch nothing. A
// zero-length normal gives no rim, never NaN. RimScale is a scalar parameter on the master (DefaultScale; an instance sets
// its own). Returns null (and the master gets no term) when the collection is missing.
inline UMaterialExpression* Term(UMaterial* Material, const TCHAR* Group, float DefaultScale, int32 X, int32 Y)
{
	UMaterialParameterCollection* Asset = LoadOrBuild();
	if (!Asset)
	{
		return nullptr;
	}
	UMaterialExpressionScalarParameter* Scale = Detail::Node<UMaterialExpressionScalarParameter>(Material, X - 300, Y + 240);
	Scale->ParameterName = TEXT("RimScale");
	Scale->DefaultValue = DefaultScale;
	Scale->Group = Group;
	Scale->UpdateParameterGuid(true, false);
	UMaterialExpressionCustom* C = Detail::Node<UMaterialExpressionCustom>(Material, X, Y);
	C->Description = TEXT("CharacterRim");
	C->OutputType = CMOT_Float3;
	C->Code = FString::Printf(TEXT("float3 n = N * Sign * rsqrt(max(dot(N, N), 1e-8));\n"
								   "float e = pow(1.0 - saturate(dot(n, V)), %.3f);\n"
								   "float s = smoothstep(%.3f, %.3f, saturate(dot(n, L.xyz)));\n"
								   "return e * s * max(Strength, 0.0) * Scale * Colour.rgb;\n"),
							  ftcore::look::kRimExponent, ftcore::look::kRimSideLow, ftcore::look::kRimSideHigh);
	C->Inputs.Reset();
	auto In = [C](const TCHAR* Name, UMaterialExpression* From)
	{
		FCustomInput& Slot = C->Inputs.AddDefaulted_GetRef();
		Slot.InputName = Name;
		Slot.Input.Connect(0, From);
	};
	In(TEXT("N"), Detail::Node<UMaterialExpressionVertexNormalWS>(Material, X - 300, Y - 120));
	In(TEXT("Sign"), Detail::Node<UMaterialExpressionTwoSidedSign>(Material, X - 300, Y - 180));
	In(TEXT("V"), Detail::Node<UMaterialExpressionCameraVectorWS>(Material, X - 300, Y - 60));
	In(TEXT("L"), Detail::Read(Material, Asset, Dir, X - 300, Y));
	In(TEXT("Colour"), Detail::Read(Material, Asset, Colour, X - 300, Y + 60));
	In(TEXT("Strength"), Detail::Read(Material, Asset, Strength, X - 300, Y + 120));
	In(TEXT("Scale"), Scale);
	return C;
}

// Adds the term to the master's emissive (summed with whatever already feeds it). Call before FTWorldWave::AddState,
// which then takes the sum through the wave's state. Returns whether the term went in.
inline bool AddTerm(UMaterial* Material, const TCHAR* Group, float DefaultScale, int32 X, int32 Y)
{
	UMaterialExpression* Rim = Term(Material, Group, DefaultScale, X, Y);
	if (!Rim)
	{
		return false;
	}
	FExpressionInput& Emissive = Material->GetEditorOnlyData()->EmissiveColor;
	if (Emissive.Expression)
	{
		UMaterialExpressionAdd* Sum = Detail::Node<UMaterialExpressionAdd>(Material, X + 250, Y);
		Sum->A.Connect(Emissive.OutputIndex, Emissive.Expression);
		Sum->B.Connect(0, Rim);
		Emissive.Connect(0, Sum);
	}
	else
	{
		Emissive.Connect(0, Rim);
	}
	return true;
}
#endif
} // namespace FTRim
