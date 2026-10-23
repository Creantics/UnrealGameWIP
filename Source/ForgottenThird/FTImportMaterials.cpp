#include "FTImportMaterials.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "ForgottenThird.h"
#include "FTText.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"

namespace
{
// The calling commandlet's name, for the log lines.
const TCHAR* GTag = TEXT("FTImport");

// Interchange gives each glTF material an instance of a plugin master without the skeletal mesh and morph target
// usage. The editor adds them in memory, but a game run draws the default material instead. Plugin content can't be
// saved, so each instance is moved onto a small project master that takes the same values and has both usages: the
// flat master for factor-only materials, the textured one when the glTF material has any texture.
const TCHAR* const kCharacterMaster = TEXT("/Game/Characters/Shared/Materials/M_FTCharacter");
const TCHAR* const kTexturedMaster = TEXT("/Game/Characters/Shared/Materials/M_FTCharacterTex");
const TCHAR* const kSharedMaterials = TEXT("/Game/Characters/Shared/Materials/");

// The glTF texture slots. Interchange names its texture parameters after them; an override is matched to a slot by
// that name, and the imported texture is set up so its sampler type matches the textured master's.
struct FTextureSlot
{
	const TCHAR* Match; // in the Interchange parameter name
	const TCHAR* Param; // on the textured master
	EMaterialSamplerType Sampler;
};
const FTextureSlot kTextureSlots[] = {
	{ TEXT("BaseColor"), TEXT("BaseColorTexture"), SAMPLERTYPE_Color },
	{ TEXT("MetallicRoughness"), TEXT("MetallicRoughnessTexture"), SAMPLERTYPE_Masks },
	{ TEXT("Normal"), TEXT("NormalTexture"), SAMPLERTYPE_Normal },
	{ TEXT("Occlusion"), TEXT("OcclusionTexture"), SAMPLERTYPE_Masks },
	{ TEXT("Emissive"), TEXT("EmissiveTexture"), SAMPLERTYPE_Color },
};

const FTextureSlot* SlotFor(FName InterchangeParam)
{
	const FString Name = InterchangeParam.ToString();
	for (const FTextureSlot& Slot : kTextureSlots)
	{
		if (Name.Contains(Slot.Match))
		{
			return &Slot;
		}
	}
	return nullptr;
}

UObject* LoadShared(const FString& Path, UClass* Class)
{
	return StaticLoadObject(Class, nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr,
		LOAD_NoWarn | LOAD_Quiet);
}

template <typename T>
T* AddParameter(UMaterial* Material, FName Name, int32 Y)
{
	T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = -400;
	Expression->MaterialExpressionEditorY = Y;
	Expression->ParameterName = Name;
	Material->GetExpressionCollection().AddExpression(Expression);
	return Expression;
}

UMaterialExpressionMultiply* Multiply(UMaterial* Material, UMaterialExpression* A, int32 AOutput, UMaterialExpression* B, int32 Y)
{
	UMaterialExpressionMultiply* Product = NewObject<UMaterialExpressionMultiply>(Material, NAME_None, RF_Transactional);
	Product->Material = Material;
	Product->MaterialExpressionEditorX = -200;
	Product->MaterialExpressionEditorY = Y;
	Product->A.Connect(AOutput, A);
	Product->B.Connect(0, B);
	Material->GetExpressionCollection().AddExpression(Product);
	return Product;
}

UMaterial* NewMaster(const TCHAR* Path, TArray<UPackage*>& Dirty)
{
	UPackage* Package = CreatePackage(Path);
	UMaterial* Material =
		NewObject<UMaterial>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Material->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
	Material->SetUsageByFlag(MATUSAGE_MorphTargets, true);
	Material->SetUsageByFlag(MATUSAGE_Clothing, true);
	Material->TwoSided = true;
	Dirty.AddUnique(Package);
	return Material;
}

void FinishMaster(UMaterial* Material)
{
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	FAssetRegistryModule::AssetCreated(Material);
	Material->MarkPackageDirty();
	UE_LOG(LogForgottenThird, Display, TEXT("%s: made %s."), GTag, *Material->GetPathName());
}

UMaterial* CharacterMaster(TArray<UPackage*>& Dirty)
{
	if (UMaterial* Existing = Cast<UMaterial>(LoadShared(kCharacterMaster, UMaterial::StaticClass())))
	{
		return Existing;
	}
	UMaterial* Material = NewMaster(kCharacterMaster, Dirty);
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	UMaterialExpressionVectorParameter* Color = AddParameter<UMaterialExpressionVectorParameter>(Material, TEXT("BaseColor"), 0);
	Color->DefaultValue = FLinearColor::White;
	Inputs->BaseColor.Expression = Color;
	UMaterialExpressionScalarParameter* Metallic = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Metallic"), 200);
	Metallic->DefaultValue = 0.0f;
	Inputs->Metallic.Expression = Metallic;
	UMaterialExpressionScalarParameter* Roughness = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Roughness"), 300);
	Roughness->DefaultValue = 0.6f;
	Inputs->Roughness.Expression = Roughness;
	UMaterialExpressionVectorParameter* Emissive = AddParameter<UMaterialExpressionVectorParameter>(Material, TEXT("Emissive"), 400);
	Emissive->DefaultValue = FLinearColor::Black;
	UMaterialExpressionScalarParameter* Strength = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("EmissiveStrength"), 600);
	Strength->DefaultValue = 1.0f;
	Inputs->EmissiveColor.Expression = Multiply(Material, Emissive, 0, Strength, 450);
	FinishMaster(Material);
	return Material;
}

// A 4x4 solid texture for a textured-master default, so every slot's default passes the sampler type check.
UTexture2D* SolidTexture(const TCHAR* Name, FColor Pixel, bool bSRGB, TextureCompressionSettings Compression,
	TArray<UPackage*>& Dirty)
{
	const FString Path = FString(kSharedMaterials) + Name;
	if (UTexture2D* Existing = Cast<UTexture2D>(LoadShared(Path, UTexture2D::StaticClass())))
	{
		return Existing;
	}
	UPackage* Package = CreatePackage(*Path);
	UTexture2D* Texture = NewObject<UTexture2D>(Package, Name, RF_Public | RF_Standalone | RF_Transactional);
	TArray<FColor> Pixels;
	Pixels.Init(Pixel, 16);
	Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
	Texture->SRGB = bSRGB;
	Texture->CompressionSettings = Compression;
	Texture->MipGenSettings = TMGS_NoMipmaps;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	Texture->MarkPackageDirty();
	Dirty.AddUnique(Package);
	return Texture;
}

UMaterial* TexturedMaster(TArray<UPackage*>& Dirty)
{
	if (UMaterial* Existing = Cast<UMaterial>(LoadShared(kTexturedMaster, UMaterial::StaticClass())))
	{
		return Existing;
	}
	UTexture2D* White = SolidTexture(TEXT("T_FTWhite"), FColor::White, true, TC_Default, Dirty);
	UTexture2D* WhiteMasks = SolidTexture(TEXT("T_FTWhiteMasks"), FColor::White, false, TC_Masks, Dirty);
	UTexture2D* FlatNormal = SolidTexture(TEXT("T_FTFlatNormal"), FColor(128, 128, 255), false, TC_Normalmap, Dirty);

	UMaterial* Material = NewMaster(kTexturedMaster, Dirty);
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	auto Sampler = [Material](const FTextureSlot& Slot, UTexture2D* Default, int32 Y)
	{
		UMaterialExpressionTextureSampleParameter2D* Sample =
			AddParameter<UMaterialExpressionTextureSampleParameter2D>(Material, Slot.Param, Y);
		Sample->Texture = Default;
		Sample->SamplerType = Slot.Sampler;
		return Sample;
	};
	// A texture sample's outputs: 0 RGB, 1 R, 2 G, 3 B, 4 A. glTF packs occlusion in R, roughness in G, metallic in B.
	UMaterialExpressionTextureSampleParameter2D* Base = Sampler(kTextureSlots[0], White, -600);
	UMaterialExpressionTextureSampleParameter2D* MetalRough = Sampler(kTextureSlots[1], WhiteMasks, -300);
	UMaterialExpressionTextureSampleParameter2D* Normal = Sampler(kTextureSlots[2], FlatNormal, 0);
	UMaterialExpressionTextureSampleParameter2D* Occlusion = Sampler(kTextureSlots[3], WhiteMasks, 300);
	UMaterialExpressionTextureSampleParameter2D* EmissiveTex = Sampler(kTextureSlots[4], White, 600);

	UMaterialExpressionVectorParameter* Color = AddParameter<UMaterialExpressionVectorParameter>(Material, TEXT("BaseColor"), -700);
	Color->DefaultValue = FLinearColor::White;
	UMaterialExpressionScalarParameter* Metallic = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Metallic"), -200);
	Metallic->DefaultValue = 1.0f;
	UMaterialExpressionScalarParameter* Roughness = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Roughness"), -100);
	Roughness->DefaultValue = 1.0f;
	UMaterialExpressionVectorParameter* Emissive = AddParameter<UMaterialExpressionVectorParameter>(Material, TEXT("Emissive"), 700);
	Emissive->DefaultValue = FLinearColor::Black;
	UMaterialExpressionScalarParameter* Strength = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("EmissiveStrength"), 800);
	Strength->DefaultValue = 1.0f;

	Inputs->BaseColor.Connect(0, Multiply(Material, Base, 0, Color, -650));
	Inputs->Roughness.Connect(0, Multiply(Material, MetalRough, 2, Roughness, -250));
	Inputs->Metallic.Connect(0, Multiply(Material, MetalRough, 3, Metallic, -150));
	Inputs->Normal.Connect(0, Normal);
	Inputs->AmbientOcclusion.Connect(1, Occlusion);
	Inputs->EmissiveColor.Connect(0, Multiply(Material, Multiply(Material, EmissiveTex, 0, Emissive, 650), 0, Strength, 750));
	FinishMaster(Material);
	return Material;
}

// Sets an imported texture up for the slot it binds to; false (logged) when its sampler type still doesn't match.
bool PrepareTexture(UTexture* Texture, const FTextureSlot& Slot, TArray<UPackage*>& Dirty)
{
	const bool bSRGB = Slot.Sampler == SAMPLERTYPE_Color;
	const TextureCompressionSettings Compression =
		Slot.Sampler == SAMPLERTYPE_Normal ? TC_Normalmap : Slot.Sampler == SAMPLERTYPE_Masks ? TC_Masks : TC_Default;
	if (Texture->GetPathName().StartsWith(TEXT("/Game/")) &&
		(Texture->SRGB != bSRGB || Texture->CompressionSettings != Compression || Texture->VirtualTextureStreaming))
	{
		Texture->PreEditChange(nullptr);
		Texture->SRGB = bSRGB;
		Texture->CompressionSettings = Compression;
		Texture->VirtualTextureStreaming = false;
		Texture->PostEditChange();
		Texture->MarkPackageDirty();
		Dirty.AddUnique(Texture->GetOutermost());
	}
	const EMaterialSamplerType Actual = MaterialExpressionUtils::GetSamplerTypeForTexture(Texture);
	if (Actual != Slot.Sampler)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s samples as type %d, but %s needs type %d."), GTag,
			*Texture->GetPathName(), static_cast<int32>(Actual), Slot.Param, static_cast<int32>(Slot.Sampler));
		return false;
	}
	return true;
}

// Logs a master's compile errors; returns how many.
int32 CompileErrors(UMaterial* Master)
{
	FMaterialResource* Resource = Master->GetMaterialResource(GMaxRHIShaderPlatform);
	if (!Resource)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s has no material resource to check."), GTag, *Master->GetName());
		return 0;
	}
	Resource->FinishCompilation();
	for (const FString& Error : Resource->GetCompileErrors())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: %s"), GTag, *Master->GetName(), *Error);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s compiled with %d errors."), GTag, *Master->GetName(),
		Resource->GetCompileErrors().Num());
	return Resource->GetCompileErrors().Num();
}

} // namespace

namespace FT
{
bool UseProjectMasters(const TCHAR* Tag, TConstArrayView<UMaterialInterface*> Materials)
{
	GTag = Tag;
	TArray<UPackage*> Dirty;
	UMaterial* Flat = nullptr;
	UMaterial* Textured = nullptr;
	int32 Moved = 0;
	bool bOk = true;
	for (UMaterialInterface* Material : Materials)
	{
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material);
		if (!Instance || !Instance->GetPathName().StartsWith(TEXT("/Game/")))
		{
			continue;
		}
		const FString ParentPath = Instance->Parent ? Instance->Parent->GetPathName() : FString();
		if (ParentPath.StartsWith(kCharacterMaster) || ParentPath.StartsWith(kTexturedMaster))
		{
			continue;
		}

		// Every texture override Interchange set, logged so a renamed parameter shows up.
		TArray<TPair<const FTextureSlot*, UTexture*>> Textures;
		for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
		{
			const FTextureSlot* TextureSlot = SlotFor(Value.ParameterInfo.Name);
			UE_LOG(LogForgottenThird, Display, TEXT("%s: %s texture parameter %s = %s -> %s"), GTag,
				*Instance->GetName(), *Value.ParameterInfo.Name.ToString(),
				Value.ParameterValue ? *Value.ParameterValue->GetName() : TEXT("none"),
				TextureSlot ? TextureSlot->Param : TEXT("(not used)"));
			if (TextureSlot && Value.ParameterValue)
			{
				Textures.Emplace(TextureSlot, Value.ParameterValue);
			}
		}

		FLinearColor Color = FLinearColor::White, Emissive = FLinearColor::Black;
		float Metallic = 0.0f, Roughness = 0.6f, Strength = 1.0f;
		Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColorFactor")), Color);
		Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("EmissiveFactor")), Emissive);
		Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("MetallicFactor")), Metallic);
		Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("RoughnessFactor")), Roughness);
		Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("EmissiveStrength")), Strength);

		UMaterial* Master = nullptr;
		if (Textures.IsEmpty())
		{
			Master = Flat ? Flat : (Flat = CharacterMaster(Dirty));
		}
		else
		{
			Master = Textured ? Textured : (Textured = TexturedMaster(Dirty));
		}
		Instance->ClearParameterValuesEditorOnly();
		Instance->SetParentEditorOnly(Master);
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BaseColor")), Color);
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Emissive")), Emissive);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Metallic")), Metallic);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), Roughness);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("EmissiveStrength")), Strength);
		for (const TPair<const FTextureSlot*, UTexture*>& Bound : Textures)
		{
			bOk &= PrepareTexture(Bound.Value, *Bound.Key, Dirty);
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Bound.Key->Param), Bound.Value);
		}
		Instance->PostEditChange();
		Instance->MarkPackageDirty();
		Dirty.AddUnique(Instance->GetOutermost());
		++Moved;
	}
	for (UMaterial* Master : { Flat, Textured })
	{
		if (Master && CompileErrors(Master) > 0)
		{
			bOk = false;
		}
	}
	if (!Dirty.IsEmpty())
	{
		UEditorLoadingAndSavingUtils::SavePackages(Dirty, false);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: moved %d materials onto the project masters."), GTag, Moved);
	return bOk;
}

bool CheckGlbFile(const TCHAR* Tag, const FString& Source, const ftcore::AssetRules& Rules)
{
	GTag = Tag;
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: couldn't read %s."), GTag, *Source);
		return false;
	}
	ftcore::GlbInfo Info;
	std::string Error;
	if (!ftcore::readGlb(FT::BytesAsText(Bytes), Info, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: %s"), GTag, *Source, *FT::ToFString(Error));
		return false;
	}
	const std::vector<ftcore::AssetIssue> Issues = ftcore::checkGlb(Info, Rules);
	for (const ftcore::AssetIssue& Issue : Issues)
	{
		const FString Line = FString::Printf(TEXT("%s: [%s] %s"), GTag, *FT::ToFString(Issue.code), *FT::ToFString(Issue.message));
		if (Issue.severity == ftcore::AssetSeverity::Error)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s"), *Line);
		}
	}
	const int32 Errors = static_cast<int32>(ftcore::countErrors(Issues));
	UE_LOG(LogForgottenThird, Display,
		TEXT("%s: asset rules (textures up to %u px, Lattice emissive split %s, skin %s): %d materials, %d images, %d errors, %d warnings."),
		GTag, Rules.maxTextureSize, Rules.emissiveRule ? TEXT("on") : TEXT("off"), Rules.requireSkin ? TEXT("required") : TEXT("not required"),
		static_cast<int32>(Info.materials.size()), static_cast<int32>(Info.images.size()), Errors,
		static_cast<int32>(Issues.size()) - Errors);
	return Errors == 0;
}
} // namespace FT
#endif
