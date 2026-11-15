#pragma once

// The surfaces' joint depth switch: one scalar, JointDepth (1), in the look's collection (FTScene::LookCollection),
// which every master that draws joints already reads for its wetness (the photographed surfaces, the window walls, the
// street). A material may reference two collections at most and the window walls already use two, so the switch
// cannot have a collection of its own. A material build adds the scalar to the collection if it is not there yet
// (saved in place, never deleted); FTBuildLookMaterials builds it in. At run time -JointDepth=<0..1> sets it.

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

namespace FTJoint
{
inline constexpr const TCHAR* Collection = TEXT("/Game/Look/MPC_Look"); // FTScene::LookCollection
inline constexpr const TCHAR* Depth = TEXT("JointDepth");

inline UMaterialParameterCollection* Load()
{
	const FString Path = FString(Collection) + TEXT(".") + FPackageName::GetShortName(Collection);
	return LoadObject<UMaterialParameterCollection>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

// -JointDepth=<0..1> on the command line, written to World's instance of the collection. False if not given or not set.
inline bool ApplyCommandLine(UWorld& World)
{
	float Value = -1.f;
	if (!FParse::Value(FCommandLine::Get(), TEXT("JointDepth="), Value) || Value < 0.f)
	{
		return false;
	}
	UMaterialParameterCollection* Asset = Load();
	UMaterialParameterCollectionInstance* Params = Asset ? World.GetParameterCollectionInstance(Asset) : nullptr;
	return Params && Params->SetScalarParameterValue(Depth, FMath::Clamp(Value, 0.f, 1.f));
}

#if WITH_EDITOR
// The scalar as a collection holds it: its id comes from its name, so a rebuild changes no id.
inline void AddScalarTo(UMaterialParameterCollection& Asset)
{
	FCollectionScalarParameter& Scalar = Asset.ScalarParameters.AddDefaulted_GetRef();
	Scalar.ParameterName = Depth;
	Scalar.DefaultValue = 1.f;
	Scalar.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTJoints.")) + Depth);
}

// The look's collection with the JointDepth scalar, added and saved in place if it is not there yet. Null if the
// collection is missing (build it first with FTBuildLookMaterials) or could not be saved.
inline UMaterialParameterCollection* LoadOrBuild()
{
	UMaterialParameterCollection* Found = Load();
	if (!Found || Found->GetParameterId(Depth).IsValid())
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
} // namespace FTJoint
