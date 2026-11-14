#pragma once

// The plants' season: one scalar, Season, in the look's collection (FTScene::LookCollection), which the foliage and bark
// masters (FTImportPlant) read. 0 spring (the cherries in blossom, every other tree a fresh light green), 1 summer,
// 2 autumn (each species' own palette, staggered leaf by leaf and tree by tree), 3 winter (the leaves gone, the twigs
// left). Values in between blend. The collection's default is spring.
//
// It lives in MPC_Look because a material may read two collections at most and the foliage master already reads the
// environment's (MPC_Env, for the wind). An import adds the scalar to the collection if it is not there yet (saved in
// place, never deleted); FTBuildLookMaterials builds it in. At run time -Season=<0..3, or spring, summer, autumn, winter>
// sets it in any world, still or played.

#include "CoreMinimal.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

namespace FTSeason
{
inline constexpr const TCHAR* Collection = TEXT("/Game/Look/MPC_Look"); // FTScene::LookCollection
inline constexpr const TCHAR* Name = TEXT("Season");
inline constexpr float Default = 0.f; // spring

inline UMaterialParameterCollection* Load()
{
	const FString Path = FString(Collection) + TEXT(".") + FPackageName::GetShortName(Collection);
	return LoadObject<UMaterialParameterCollection>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

// A season by name (spring, summer, autumn or fall, winter) or number (0..3, fractions blend). False if neither.
inline bool Parse(const FString& Text, float& Out)
{
	static const TCHAR* const Names[] = {TEXT("spring"), TEXT("summer"), TEXT("autumn"), TEXT("winter")};
	for (int32 I = 0; I < 4; ++I)
	{
		if (Text.Equals(Names[I], ESearchCase::IgnoreCase))
		{
			Out = static_cast<float>(I);
			return true;
		}
	}
	if (Text.Equals(TEXT("fall"), ESearchCase::IgnoreCase))
	{
		Out = 2.f;
		return true;
	}
	if (!Text.IsNumeric())
	{
		return false;
	}
	Out = FMath::Clamp(FCString::Atof(*Text), 0.f, 3.f);
	return true;
}

// -Season=<...> on the command line, written to World's instance of the collection. False if not given or not set.
inline bool ApplyCommandLine(UWorld& World)
{
	FString Text;
	float Value = 0.f;
	if (!FParse::Value(FCommandLine::Get(), TEXT("Season="), Text) || !Parse(Text, Value))
	{
		return false;
	}
	UMaterialParameterCollection* Asset = Load();
	UMaterialParameterCollectionInstance* Params = Asset ? World.GetParameterCollectionInstance(Asset) : nullptr;
	return Params && Params->SetScalarParameterValue(Name, Value);
}

#if WITH_EDITOR
// The scalar as a collection holds it: its id comes from its name, so a rebuild changes no id.
inline void AddScalarTo(UMaterialParameterCollection& Asset)
{
	FCollectionScalarParameter& Scalar = Asset.ScalarParameters.AddDefaulted_GetRef();
	Scalar.ParameterName = Name;
	Scalar.DefaultValue = Default;
	Scalar.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTSeason.")) + Name);
}

// The look's collection with the Season scalar, added and saved in place if it is not there yet. Null if the collection
// is missing (build it first with FTBuildLookMaterials) or could not be saved.
inline UMaterialParameterCollection* LoadOrBuild()
{
	UMaterialParameterCollection* Found = Load();
	if (!Found || Found->GetParameterId(Name).IsValid())
	{
		return Found;
	}
	AddScalarTo(*Found);
	Found->PostEditChange();
	UPackage* Package = Found->GetPackage();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	return UPackage::SavePackage(Package, Found, *File, Args) ? Found : nullptr;
}
#endif
} // namespace FTSeason
