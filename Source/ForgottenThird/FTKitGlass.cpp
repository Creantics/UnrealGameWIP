#include "FTKitGlass.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "FTBuildShopInteriors.h"
#include "ForgottenThird.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"

// Named, not anonymous: the module builds in unity files.
namespace FTKitGlass::Impl
{
// The imported kits' textured master (FTImportMaterials): the slots worn on it are the ones without night glow.
constexpr const TCHAR* kTexturedMaster = TEXT("/Game/Characters/Shared/Materials/M_FTCharacterTex");
constexpr const TCHAR* kDefaultKits = TEXT("BK-B2+BK-D1");
// M_KitGlass's parameters, named as on the textured master.
constexpr const TCHAR* kTextures[] = {TEXT("BaseColorTexture"), TEXT("MetallicRoughnessTexture"), TEXT("NormalTexture"), TEXT("OcclusionTexture")};
constexpr const TCHAR* kScalars[] = {TEXT("Metallic"), TEXT("Roughness")};
constexpr const TCHAR* kColor = TEXT("BaseColor");
} // namespace FTKitGlass::Impl

namespace FTKitGlass
{
FOverrides::FOverrides(UPackage* InPackage, const FString& Params) : Package(InPackage)
{
	if (!FParse::Param(*Params, TEXT("KitGlassFrontage")))
	{
		return;
	}
	const FString Path = FString::Printf(TEXT("%s.%s"), FTShopInteriors::KitGlassMaterial, *FPackageName::GetShortName(FTShopInteriors::KitGlassMaterial));
	Master = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Master)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -KitGlassFrontage: %s isn't built (FTBuildSurfaces -Windows -KitGlass); the "
			"frontage keeps its own materials."), FTShopInteriors::KitGlassMaterial);
		return;
	}
	FString KitList = Impl::kDefaultKits;
	FParse::Value(*Params, TEXT("KitGlassKits="), KitList);
	KitList.ParseIntoArray(Kits, TEXT("+"), true);
	for (FString& Kit : Kits)
	{
		Kit = FString::Printf(TEXT("/%s/"), *Kit.TrimStartAndEnd());
	}
}

UMaterialInterface* FOverrides::InstanceOf(UMaterialInterface* Source)
{
	if (UMaterialInterface** Known = PerSource.Find(Source))
	{
		return *Known;
	}
	const FName Name = MakeUniqueObjectName(Package, UMaterialInstanceConstant::StaticClass(), FName(*(TEXT("MI_KitGlass_") + Source->GetName())));
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
	Mic->SetParentEditorOnly(Master);
	for (const TCHAR* Param : Impl::kTextures)
	{
		UTexture* Texture = nullptr;
		if (Source->GetTextureParameterValue(FHashedMaterialParameterInfo(Param), Texture) && Texture)
		{
			Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Param), Texture);
		}
	}
	for (const TCHAR* Param : Impl::kScalars)
	{
		float Value = 0.f;
		if (Source->GetScalarParameterValue(FHashedMaterialParameterInfo(Param), Value))
		{
			Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Param), Value);
		}
	}
	FLinearColor Color = FLinearColor::White;
	if (Source->GetVectorParameterValue(FHashedMaterialParameterInfo(Impl::kColor), Color))
	{
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Impl::kColor), Color);
	}
	Mic->PostEditChange();
	PerSource.Add(Source, Mic);
	return Mic;
}

TArrayView<UMaterialInterface* const> FOverrides::For(UStaticMesh* Mesh)
{
	if (!Master || !Mesh)
	{
		return {};
	}
	if (const TArray<UMaterialInterface*>* Known = PerMesh.Find(Mesh))
	{
		return *Known;
	}
	TArray<UMaterialInterface*>& Out = PerMesh.Add(Mesh);
	const FString MeshPath = Mesh->GetPathName();
	if (!Kits.ContainsByPredicate([&MeshPath](const FString& Kit) { return MeshPath.Contains(Kit); }))
	{
		return {};
	}
	const TArray<FStaticMaterial>& Own = Mesh->GetStaticMaterials();
	bool bAny = false;
	Out.SetNumZeroed(Own.Num());
	for (int32 S = 0; S < Own.Num(); ++S)
	{
		UMaterialInterface* Source = Own[S].MaterialInterface.Get();
		const UMaterial* Base = Source ? Source->GetMaterial() : nullptr;
		if (Base && Base->GetOutermost()->GetName() == Impl::kTexturedMaster)
		{
			Out[S] = InstanceOf(Source);
			++Slots;
			bAny = true;
		}
	}
	if (!bAny)
	{
		Out.Reset();
		return {};
	}
	++Meshes;
	return Out;
}

void FOverrides::Log() const
{
	if (Master)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -KitGlassFrontage: %d kit meshes, %d slots on %s, %d instances made."), Meshes,
			Slots, FTShopInteriors::KitGlassMaterial, PerSource.Num());
	}
}
} // namespace FTKitGlass
