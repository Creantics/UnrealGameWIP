#include "FTImportMaterials.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "ForgottenThird.h"
#include "FTSceneLook.h"
#include "FTText.h"
#include "FTWorldWave.h"
#include "ImageCore.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "MaterialShared.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"

#include "ftcore/PropGloss.h"

#include <array>
#include <cstdint>
#include <string>

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
const TCHAR* const kSkinMaster = TEXT("/Game/Characters/Shared/Materials/M_FTSkin");
const TCHAR* const kHairMaster = TEXT("/Game/Characters/Shared/Materials/M_FTHair");
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
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true); // props drawn by the cell instancer
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

// The world wave's character state (FTWorldWave::AddState with Character on), wired in last. A master that already has
// it is left as it was; one saved before the state existed, or with its Character flag off, takes it here and is saved
// again.
void AddCharacterState(UMaterial* Material, bool bExisting, TArray<UPackage*>& Dirty)
{
	const int32 Before = Material->GetExpressionCollection().Expressions.Num();
	const bool bWasOn = FTWorldWave::CharacterOn(*Material);
	FTWorldWave::AddState(GTag, Material, 300, 1000, true);
	if (bExisting && (Material->GetExpressionCollection().Expressions.Num() != Before || bWasOn != FTWorldWave::CharacterOn(*Material)))
	{
		Material->PreEditChange(nullptr);
		Material->PostEditChange();
		Material->MarkPackageDirty();
		Dirty.AddUnique(Material->GetPackage());
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s takes the world wave's character state."), GTag, *Material->GetPathName());
	}
}

UMaterial* CharacterMaster(TArray<UPackage*>& Dirty)
{
	if (UMaterial* Existing = Cast<UMaterial>(LoadShared(kCharacterMaster, UMaterial::StaticClass())))
	{
		AddCharacterState(Existing, true, Dirty);
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
	AddCharacterState(Material, false, Dirty);
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
		AddCharacterState(Existing, true, Dirty);
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
	AddCharacterState(Material, false, Dirty);
	FinishMaster(Material);
	return Material;
}

// Hair cards: a glTF material with alphaMode MASK. Masked and two-sided, the base colour's alpha clipping each strand at
// glTF's default cutoff. The specular is anisotropic about the mesh tangent, which follows UV U, so the strand textures
// run along U; a negative Anisotropy makes the surface rough across the strands and smooth along them, as a fibre is,
// which draws the highlight as a band across the hair.
UMaterial* HairMaster(TArray<UPackage*>& Dirty)
{
	if (UMaterial* Existing = Cast<UMaterial>(LoadShared(kHairMaster, UMaterial::StaticClass())))
	{
		AddCharacterState(Existing, true, Dirty);
		return Existing;
	}
	UTexture2D* White = SolidTexture(TEXT("T_FTWhite"), FColor::White, true, TC_Default, Dirty);
	UTexture2D* WhiteMasks = SolidTexture(TEXT("T_FTWhiteMasks"), FColor::White, false, TC_Masks, Dirty);

	UMaterial* Material = NewMaster(kHairMaster, Dirty);
	Material->BlendMode = BLEND_Masked;
	Material->OpacityMaskClipValue = 0.5f;
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	auto Sampler = [Material](const FTextureSlot& Slot, UTexture2D* Default, int32 Y)
	{
		UMaterialExpressionTextureSampleParameter2D* Sample =
			AddParameter<UMaterialExpressionTextureSampleParameter2D>(Material, Slot.Param, Y);
		Sample->Texture = Default;
		Sample->SamplerType = Slot.Sampler;
		return Sample;
	};
	UMaterialExpressionTextureSampleParameter2D* Base = Sampler(kTextureSlots[0], White, -600);
	UMaterialExpressionTextureSampleParameter2D* MetalRough = Sampler(kTextureSlots[1], WhiteMasks, -300);
	UMaterialExpressionTextureSampleParameter2D* Occlusion = Sampler(kTextureSlots[3], WhiteMasks, 300);

	UMaterialExpressionVectorParameter* Color = AddParameter<UMaterialExpressionVectorParameter>(Material, TEXT("BaseColor"), -700);
	Color->DefaultValue = FLinearColor::White;
	UMaterialExpressionScalarParameter* Metallic = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Metallic"), -200);
	Metallic->DefaultValue = 0.0f;
	UMaterialExpressionScalarParameter* Roughness = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Roughness"), -100);
	Roughness->DefaultValue = 0.45f;
	UMaterialExpressionScalarParameter* Specular = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Specular"), 0);
	Specular->DefaultValue = 0.4f;
	UMaterialExpressionScalarParameter* Anisotropy = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Anisotropy"), 100);
	Anisotropy->DefaultValue = -0.8f;

	Inputs->BaseColor.Connect(0, Multiply(Material, Base, 0, Color, -650));
	Inputs->OpacityMask.Connect(4, Base);
	Inputs->Roughness.Connect(0, Multiply(Material, MetalRough, 2, Roughness, -250));
	Inputs->Metallic.Connect(0, Multiply(Material, MetalRough, 3, Metallic, -150));
	Inputs->Specular.Connect(0, Specular);
	Inputs->Anisotropy.Connect(0, Anisotropy);
	Inputs->AmbientOcclusion.Connect(1, Occlusion);
	AddCharacterState(Material, false, Dirty);
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
int32 MasterCompileErrors(UMaterial* Master)
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

// Named, not anonymous: in a unity build other files' helpers share this translation unit.
namespace FTPropMasterBuild
{
// The props' masters (FT::UsePropMasters). They take the character masters' parameters under the same names, with the
// prop surface's three layers over the authored values (ftcore::propSurfaceHlsl: break-up, grime, wet), each neutral at
// its parameter's 0. Content/Props is local-only, so the masters are build outputs there like the props themselves.
const TCHAR* const kFlatMaster = TEXT("/Game/Props/Shared/Materials/M_FTPropFlat");
const TCHAR* const kTexMaster = TEXT("/Game/Props/Shared/Materials/M_FTPropTex");
// The surface node's Description, by which an existing master's node is found to refresh its code.
const TCHAR* const kSurfaceNode = TEXT("FTPropSurface");
// The textured masters' default roughness/metallic map (SolidTexture): a slot bound to it has no map of its own.
const TCHAR* const kWhiteMasks = TEXT("T_FTWhiteMasks");
const TCHAR* const kGroup = TEXT("Prop surface");

template <typename T>
T* AddNode(UMaterial* Material, int32 X, int32 Y)
{
	T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

void AddInput(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From, int32 Output)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(Output, From);
}

UMaterialExpressionScalarParameter* Scalar(UMaterial* Material, const TCHAR* Name, float Default, int32 Y)
{
	UMaterialExpressionScalarParameter* Param = AddParameter<UMaterialExpressionScalarParameter>(Material, Name, Y);
	Param->DefaultValue = Default;
	Param->Group = FName(kGroup);
	return Param;
}

FString SurfaceCode()
{
	return FT::ToFString(ftcore::propSurfaceHlsl());
}

UMaterialExpressionCustom* FindSurface(UMaterial& Material)
{
	for (UMaterialExpression* Expression : Material.GetExpressionCollection().Expressions)
	{
		UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression);
		if (Custom && Custom->Description == kSurfaceNode)
		{
			return Custom;
		}
	}
	return nullptr;
}

// The look's Wetness (FTScene::LookCollection), or a constant 0 (logged) when the collection or its parameter is missing:
// the master then stays dry.
UMaterialExpression* Wetness(UMaterial* Material, int32 Y)
{
	const FString LookPath = FString(FTScene::LookCollection) + TEXT(".") + FPackageName::GetShortName(FTScene::LookCollection);
	UMaterialParameterCollection* Look = LoadObject<UMaterialParameterCollection>(nullptr, *LookPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	const FName WetName(TEXT("Wetness"));
	if (Look && Look->GetParameterId(WetName).IsValid())
	{
		UMaterialExpressionCollectionParameter* Wet = AddNode<UMaterialExpressionCollectionParameter>(Material, -400, Y);
		Wet->Collection = Look;
		Wet->ParameterName = WetName;
		Wet->ParameterId = Look->GetParameterId(WetName);
		return Wet;
	}
	UMaterialExpressionConstant* Dry = AddNode<UMaterialExpressionConstant>(Material, -400, Y);
	Dry->R = 0.f;
	UE_LOG(LogForgottenThird, Warning, TEXT("%s: %s or its Wetness is missing; %s stays dry in the rain."), GTag, *LookPath, *Material->GetName());
	return Dry;
}

// The surface node between the master's own values and its BaseColor and Roughness pins: Albedo, Rough and Metal from
// (expression, output), AO likewise. Its own parameters: RoughBreakup 0, Grime 0, DustColor, WetRough 0.35 and Weather 1
// (1 out in the open; an instance under cover sets 0 and stays dry). Wired before the world wave's state, which then takes
// both pins as fed.
void AddSurface(UMaterial* Material, UMaterialExpression* Albedo, int32 AlbedoOut, UMaterialExpression* Rough, int32 RoughOut,
	UMaterialExpression* Metal, int32 MetalOut, UMaterialExpression* AO, int32 AOOut)
{
	UMaterialExpressionScalarParameter* Breakup = Scalar(Material, TEXT("RoughBreakup"), 0.f, 1000);
	UMaterialExpressionScalarParameter* Grime = Scalar(Material, TEXT("Grime"), 0.f, 1100);
	UMaterialExpressionVectorParameter* Dust = AddParameter<UMaterialExpressionVectorParameter>(Material, TEXT("DustColor"), 1200);
	Dust->DefaultValue = FLinearColor(ftcore::kPropDustColor[0], ftcore::kPropDustColor[1], ftcore::kPropDustColor[2]);
	Dust->Group = FName(kGroup);
	UMaterialExpressionScalarParameter* WetRough = Scalar(Material, TEXT("WetRough"), ftcore::kPropWetRoughDefault, 1400);
	UMaterialExpressionScalarParameter* Weather = Scalar(Material, TEXT("Weather"), 1.f, 1500);
	UMaterialExpression* Wet = Wetness(Material, 1600);
	UMaterialExpressionWorldPosition* WorldPos = AddNode<UMaterialExpressionWorldPosition>(Material, -400, 1700);
	UMaterialExpressionVertexNormalWS* VertexNormal = AddNode<UMaterialExpressionVertexNormalWS>(Material, -400, 1800);

	UMaterialExpressionCustom* Custom = AddNode<UMaterialExpressionCustom>(Material, 0, 0);
	Custom->Description = kSurfaceNode;
	Custom->OutputType = CMOT_Float3;
	Custom->Code = SurfaceCode();
	Custom->Inputs.Reset();
	AddInput(Custom, TEXT("Albedo"), Albedo, AlbedoOut);
	AddInput(Custom, TEXT("Rough"), Rough, RoughOut);
	AddInput(Custom, TEXT("Metal"), Metal, MetalOut);
	AddInput(Custom, TEXT("AO"), AO, AOOut);
	AddInput(Custom, TEXT("WorldPos"), WorldPos, 0);
	AddInput(Custom, TEXT("VertexNormal"), VertexNormal, 0);
	AddInput(Custom, TEXT("Wetness"), Wet, 0);
	AddInput(Custom, TEXT("Weather"), Weather, 0);
	AddInput(Custom, TEXT("Breakup"), Breakup, 0);
	AddInput(Custom, TEXT("Grime"), Grime, 0);
	AddInput(Custom, TEXT("DustColor"), Dust, 0);
	AddInput(Custom, TEXT("WetRough"), WetRough, 0);
	// Output 0 is the albedo, then the roughness.
	FCustomOutput& Out = Custom->AdditionalOutputs.AddDefaulted_GetRef();
	Out.OutputName = TEXT("RoughOut");
	Out.OutputType = CMOT_Float1;
	Custom->RebuildOutputs();

	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Custom);
	Inputs->Roughness.Connect(1, Custom);
}

// An existing prop master: its surface node takes the current code (the Core maths may have moved on since it was saved),
// and the world wave's surface state goes in when it was saved without it; saved again when either changed. False
// (logged) when the node is missing: the asset is not one this build made.
bool Refresh(UMaterial* Material, TArray<UPackage*>& Dirty)
{
	UMaterialExpressionCustom* Custom = FindSurface(*Material);
	if (!Custom)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s has no %s node; delete it and import again to rebuild it."), GTag,
			*Material->GetPathName(), kSurfaceNode);
		return false;
	}
	const int32 Before = Material->GetExpressionCollection().Expressions.Num();
	const FString Code = SurfaceCode();
	const bool bCode = !Custom->Code.Equals(Code, ESearchCase::CaseSensitive);
	if (bCode)
	{
		Custom->Code = Code;
	}
	FTWorldWave::AddState(GTag, Material, 300, 1000, false);
	const bool bState = Material->GetExpressionCollection().Expressions.Num() != Before;
	if (bCode || bState)
	{
		Material->PreEditChange(nullptr);
		Material->PostEditChange();
		Material->MarkPackageDirty();
		Dirty.AddUnique(Material->GetPackage());
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s saved again (%s%s)."), GTag, *Material->GetPathName(),
			bCode ? TEXT("the current surface code") : TEXT(""), bState ? (bCode ? TEXT(", the world wave's surface state") : TEXT("the world wave's surface state")) : TEXT(""));
	}
	return true;
}

// M_FTPropFlat: M_FTCharacter's parameters and defaults (BaseColor white, Metallic 0, Roughness 0.6, Emissive black,
// EmissiveStrength 1), AO 1, under the surface layers.
UMaterial* FlatMaster(TArray<UPackage*>& Dirty, bool& bOk)
{
	if (UMaterial* Existing = Cast<UMaterial>(LoadShared(kFlatMaster, UMaterial::StaticClass())))
	{
		bOk &= Refresh(Existing, Dirty);
		return Existing;
	}
	UMaterial* Material = NewMaster(kFlatMaster, Dirty);
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	UMaterialExpressionVectorParameter* Color = AddParameter<UMaterialExpressionVectorParameter>(Material, TEXT("BaseColor"), 0);
	Color->DefaultValue = FLinearColor::White;
	UMaterialExpressionScalarParameter* Metallic = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Metallic"), 200);
	Metallic->DefaultValue = 0.0f;
	Inputs->Metallic.Expression = Metallic;
	UMaterialExpressionScalarParameter* Roughness = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("Roughness"), 300);
	Roughness->DefaultValue = 0.6f;
	UMaterialExpressionVectorParameter* Emissive = AddParameter<UMaterialExpressionVectorParameter>(Material, TEXT("Emissive"), 400);
	Emissive->DefaultValue = FLinearColor::Black;
	UMaterialExpressionScalarParameter* Strength = AddParameter<UMaterialExpressionScalarParameter>(Material, TEXT("EmissiveStrength"), 600);
	Strength->DefaultValue = 1.0f;
	Inputs->EmissiveColor.Expression = Multiply(Material, Emissive, 0, Strength, 450);
	UMaterialExpressionConstant* NoCavity = AddNode<UMaterialExpressionConstant>(Material, -400, 800);
	NoCavity->R = 1.f;
	AddSurface(Material, Color, 0, Roughness, 0, Metallic, 0, NoCavity, 0);
	FTWorldWave::AddState(GTag, Material, 300, 1000, false);
	FinishMaster(Material);
	return Material;
}

// M_FTPropTex: M_FTCharacterTex's parameters, texture slots and defaults (Metallic 1, Roughness 1, the solid default
// maps), the occlusion map's R as the cavity, under the surface layers.
UMaterial* TexMaster(TArray<UPackage*>& Dirty, bool& bOk)
{
	if (UMaterial* Existing = Cast<UMaterial>(LoadShared(kTexMaster, UMaterial::StaticClass())))
	{
		bOk &= Refresh(Existing, Dirty);
		return Existing;
	}
	UTexture2D* White = SolidTexture(TEXT("T_FTWhite"), FColor::White, true, TC_Default, Dirty);
	UTexture2D* WhiteMasks = SolidTexture(kWhiteMasks, FColor::White, false, TC_Masks, Dirty);
	UTexture2D* FlatNormal = SolidTexture(TEXT("T_FTFlatNormal"), FColor(128, 128, 255), false, TC_Normalmap, Dirty);

	UMaterial* Material = NewMaster(kTexMaster, Dirty);
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

	UMaterialExpressionMultiply* Albedo = Multiply(Material, Base, 0, Color, -650);
	UMaterialExpressionMultiply* Rough = Multiply(Material, MetalRough, 2, Roughness, -250);
	UMaterialExpressionMultiply* Metal = Multiply(Material, MetalRough, 3, Metallic, -150);
	Inputs->Metallic.Connect(0, Metal);
	Inputs->Normal.Connect(0, Normal);
	Inputs->AmbientOcclusion.Connect(1, Occlusion);
	Inputs->EmissiveColor.Connect(0, Multiply(Material, Multiply(Material, EmissiveTex, 0, Emissive, 650), 0, Strength, 750));
	AddSurface(Material, Albedo, 0, Rough, 0, Metal, 0, Occlusion, 1);
	FTWorldWave::AddState(GTag, Material, 300, 1000, false);
	FinishMaster(Material);
	return Material;
}

bool IsPropMaster(const FString& Package)
{
	return Package == kFlatMaster || Package == kTexMaster;
}

FString ParentPackage(const UMaterialInstanceConstant& Instance)
{
	return Instance.Parent ? Instance.Parent->GetPackage()->GetName() : FString();
}

// Whether the material has a roughness/metallic map of its own (not the textured masters' white default).
UTexture* OwnMap(const UMaterialInterface& Material)
{
	UTexture* Map = nullptr;
	if (!Material.GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("MetallicRoughnessTexture")), Map) || !Map ||
		Map->GetName() == kWhiteMasks)
	{
		return nullptr;
	}
	return Map;
}

// The roughness (G) and metallic (B) histograms of a map's top source mip, the stored bytes as they are (a masks map is
// linear). False when the texture has no source data.
bool MapHistograms(UTexture& Map, std::array<std::uint32_t, 256>& Rough, std::array<std::uint32_t, 256>& Metal)
{
	Rough.fill(0u);
	Metal.fill(0u);
	FImage Image;
	if (!Map.Source.IsValid() || !Map.Source.GetMipImage(Image, 0))
	{
		return false;
	}
	Image.GammaSpace = EGammaSpace::Linear;
	if (Image.Format != ERawImageFormat::BGRA8)
	{
		Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::Linear);
	}
	for (const FColor& Texel : Image.AsBGRA8())
	{
		++Rough[Texel.G];
		++Metal[Texel.B];
	}
	return true;
}
} // namespace FTPropMasterBuild

namespace FT
{
bool UseProjectMasters(const TCHAR* Tag, TConstArrayView<UMaterialInterface*> Materials)
{
	GTag = Tag;
	TArray<UPackage*> Dirty;
	UMaterial* Flat = nullptr;
	UMaterial* Textured = nullptr;
	UMaterial* Hair = nullptr;
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
		if (ParentPath.StartsWith(kCharacterMaster) || ParentPath.StartsWith(kTexturedMaster) || ParentPath.StartsWith(kHairMaster) ||
			ParentPath.StartsWith(kSkinMaster))
		{
			continue;
		}
		// Hair cards come in as alphaMode MASK: without Substrate on Interchange's masked glTF master, with it on M_GLTF
		// with the scalar AlphaMode = 1 (Interchange's glTF EAlphaMode: 0 opaque, 1 mask, 2 blend).
		float AlphaMode = 0.0f;
		Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("AlphaMode")), AlphaMode);
		const bool bMasked = Instance->GetBlendMode() == BLEND_Masked || FMath::RoundToInt(AlphaMode) == 1;
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s comes in on %s (%s)."), GTag, *Instance->GetName(), *ParentPath,
			bMasked ? TEXT("masked") : TEXT("opaque"));

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
		if (bMasked && !Textures.IsEmpty())
		{
			Master = Hair ? Hair : (Hair = HairMaster(Dirty));
		}
		else if (Textures.IsEmpty())
		{
			Master = Flat ? Flat : (Flat = CharacterMaster(Dirty));
		}
		else
		{
			Master = Textured ? Textured : (Textured = TexturedMaster(Dirty));
		}
		// Interchange may leave base property overrides (blend mode, clip value, two-sided) from its own master; the
		// project master's settings must win, so they go along with the parameters.
		const FMaterialInstanceBasePropertyOverrides& Old = Instance->BasePropertyOverrides;
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s overrides: blend %d (%d), clip %.2f (%d), two-sided %d (%d)."), GTag,
			*Instance->GetName(), static_cast<int32>(Old.BlendMode), Old.bOverride_BlendMode, Old.OpacityMaskClipValue,
			Old.bOverride_OpacityMaskClipValue, Old.TwoSided, Old.bOverride_TwoSided);
		Instance->BasePropertyOverrides = FMaterialInstanceBasePropertyOverrides();
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
	for (UMaterial* Master : { Flat, Textured, Hair })
	{
		if (Master && MasterCompileErrors(Master) > 0)
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

bool UsePropMasters(const TCHAR* Tag, TConstArrayView<UMaterialInterface*> Materials, float Grime, bool bHeld)
{
	GTag = Tag;
	TArray<UPackage*> Dirty;
	if (bHeld)
	{
		// A held prop keeps the character masters (it moves with a hand, so no grime and no world-fixed noise): an instance
		// an earlier import moved onto a prop master goes back with its values first, since UseProjectMasters would take it
		// for Interchange's and clear them; then the character path runs.
		UMaterial* CharFlat = nullptr;
		UMaterial* CharTex = nullptr;
		int32 Back = 0;
		for (UMaterialInterface* Material : Materials)
		{
			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material);
			if (!Instance || !Instance->GetPathName().StartsWith(TEXT("/Game/")))
			{
				continue;
			}
			const FString Parent = FTPropMasterBuild::ParentPackage(*Instance);
			if (!FTPropMasterBuild::IsPropMaster(Parent))
			{
				continue;
			}
			UMaterial* Master = Parent == FTPropMasterBuild::kTexMaster ? (CharTex ? CharTex : (CharTex = TexturedMaster(Dirty)))
																		: (CharFlat ? CharFlat : (CharFlat = CharacterMaster(Dirty)));
			Instance->SetParentEditorOnly(Master);
			Instance->PostEditChange();
			Instance->MarkPackageDirty();
			Dirty.AddUnique(Instance->GetOutermost());
			UE_LOG(LogForgottenThird, Display, TEXT("%s: -Held: %s goes back from %s onto %s, its values kept."), GTag, *Instance->GetName(),
				*Parent, *Master->GetName());
			++Back;
		}
		if (!Dirty.IsEmpty())
		{
			UEditorLoadingAndSavingUtils::SavePackages(Dirty, false);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("%s: -Held: the character masters (%d materials back from the prop masters)."), GTag, Back);
		return UseProjectMasters(Tag, Materials);
	}

	UMaterial* Flat = nullptr;
	UMaterial* Textured = nullptr;
	UMaterial* Hair = nullptr;
	int32 Moved = 0;
	int32 Reparented = 0;
	int32 Kept = 0;
	bool bOk = true;
	const auto PropMaster = [&Flat, &Textured, &Dirty, &bOk](bool bTextured) -> UMaterial*
	{
		if (bTextured)
		{
			return Textured ? Textured : (Textured = FTPropMasterBuild::TexMaster(Dirty, bOk));
		}
		return Flat ? Flat : (Flat = FTPropMasterBuild::FlatMaster(Dirty, bOk));
	};
	for (UMaterialInterface* Material : Materials)
	{
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material);
		if (!Instance || !Instance->GetPathName().StartsWith(TEXT("/Game/")))
		{
			continue;
		}
		const FString Parent = FTPropMasterBuild::ParentPackage(*Instance);
		const FString ParentPath = Instance->Parent ? Instance->Parent->GetPathName() : FString();
		bool bProp = true;
		if (FTPropMasterBuild::IsPropMaster(Parent))
		{
			// Already on a prop master (an earlier import kept its instance): its values stay; grime and break-up are set
			// again below.
			PropMaster(Parent == FTPropMasterBuild::kTexMaster);
			++Kept;
		}
		else if (Parent == kCharacterMaster || Parent == kTexturedMaster)
		{
			// On a character master (imported before the prop masters existed): the prop master has the same parameters,
			// so every override goes along. The package names are compared whole: one master's name begins the other's.
			UMaterial* Master = PropMaster(Parent == kTexturedMaster);
			Instance->SetParentEditorOnly(Master);
			UE_LOG(LogForgottenThird, Display, TEXT("%s: %s moves from %s onto %s, its values kept."), GTag, *Instance->GetName(), *Parent,
				*Master->GetName());
			++Reparented;
		}
		else if (ParentPath.StartsWith(kHairMaster) || ParentPath.StartsWith(kSkinMaster))
		{
			continue;
		}
		else
		{
			// Interchange's instance, read as UseProjectMasters reads it.
			float AlphaMode = 0.0f;
			Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("AlphaMode")), AlphaMode);
			const bool bMasked = Instance->GetBlendMode() == BLEND_Masked || FMath::RoundToInt(AlphaMode) == 1;
			UE_LOG(LogForgottenThird, Display, TEXT("%s: %s comes in on %s (%s)."), GTag, *Instance->GetName(), *ParentPath,
				bMasked ? TEXT("masked") : TEXT("opaque"));
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
			// A textured material's factors start at glTF's 1 (the map carries the value), so a missing RoughnessFactor
			// can't scale the map by 0.6; a flat one starts at the character masters' 0.6 and 0.
			const bool bTextured = !Textures.IsEmpty();
			FLinearColor Color = FLinearColor::White, Emissive = FLinearColor::Black;
			float Metallic = bTextured ? 1.0f : 0.0f, Roughness = bTextured ? 1.0f : 0.6f, Strength = 1.0f;
			Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColorFactor")), Color);
			Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("EmissiveFactor")), Emissive);
			Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("MetallicFactor")), Metallic);
			Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("RoughnessFactor")), Roughness);
			Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("EmissiveStrength")), Strength);

			UMaterial* Master = nullptr;
			if (bMasked && bTextured)
			{
				// Cards (alphaMode MASK) keep the hair master, as on the character path; no prop surface on them.
				if (!Hair)
				{
					Hair = HairMaster(Dirty);
				}
				Master = Hair;
				bProp = false;
			}
			else
			{
				Master = PropMaster(bTextured);
			}
			const FMaterialInstanceBasePropertyOverrides& Old = Instance->BasePropertyOverrides;
			UE_LOG(LogForgottenThird, Display, TEXT("%s: %s overrides: blend %d (%d), clip %.2f (%d), two-sided %d (%d)."), GTag,
				*Instance->GetName(), static_cast<int32>(Old.BlendMode), Old.bOverride_BlendMode, Old.OpacityMaskClipValue,
				Old.bOverride_OpacityMaskClipValue, Old.TwoSided, Old.bOverride_TwoSided);
			Instance->BasePropertyOverrides = FMaterialInstanceBasePropertyOverrides();
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
			++Moved;
		}
		if (bProp)
		{
			// Break-up on a material with no roughness map of its own (a map carries its own spread); grime per world.
			const bool bOwnMap = FTPropMasterBuild::OwnMap(*Instance) != nullptr;
			const float InstanceGrime = ftcore::propNameLiquid(TCHAR_TO_UTF8(*Instance->GetName())) ? 0.0f : Grime;  // no dust on a liquid
			Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Grime")), InstanceGrime);
			Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("RoughBreakup")), bOwnMap ? 0.0f : 1.0f);
			UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: Grime %.2f, RoughBreakup %d."), GTag, *Instance->GetName(),
				InstanceGrime, bOwnMap ? 0 : 1);
		}
		Instance->PostEditChange();
		Instance->MarkPackageDirty();
		Dirty.AddUnique(Instance->GetOutermost());
	}
	for (UMaterial* Master : { Flat, Textured, Hair })
	{
		if (Master && MasterCompileErrors(Master) > 0)
		{
			bOk = false;
		}
	}
	if (!Dirty.IsEmpty())
	{
		UEditorLoadingAndSavingUtils::SavePackages(Dirty, false);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: prop masters: %d moved from Interchange, %d from the character masters, %d already there."),
		GTag, Moved, Reparented, Kept);
	return bOk;
}

bool CheckPropGloss(const TCHAR* Tag, TConstArrayView<UMaterialInterface*> Materials)
{
	GTag = Tag;
	int32 Checked = 0;
	int32 Failed = 0;
	std::array<std::uint32_t, 256> RoughHist{};
	std::array<std::uint32_t, 256> MetalHist{};
	for (UMaterialInterface* Material : Materials)
	{
		if (!Material)
		{
			continue;
		}
		if (!Material->GetPathName().StartsWith(TEXT("/Game/")))
		{
			// An engine or plugin material (a slot the import left on the default material): not the prop's own, not moved.
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: gloss %s: outside /Game (%s), not checked."), GTag, *Material->GetName(),
				*Material->GetPathName());
			continue;
		}
		++Checked;
		float Roughness = 1.0f, Metallic = 0.0f;
		if (!Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Roughness")), Roughness) ||
			!Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Metallic")), Metallic))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: gloss %s: no Roughness or Metallic parameter (not on a project master), so it can't be checked."),
				GTag, *Material->GetName());
			++Failed;
			continue;
		}
		const std::string Name(TCHAR_TO_UTF8(*Material->GetName()));
		ftcore::PropGlossInput In;
		In.name = Name;
		FString From = TEXT("factors only");
		UTexture* Map = FTPropMasterBuild::OwnMap(*Material);
		if (Map && FTPropMasterBuild::MapHistograms(*Map, RoughHist, MetalHist))
		{
			// Effective values: the map's channel times its factor.
			In.hasMap = true;
			In.p10 = ftcore::propHistogramPercentile(RoughHist, 0.1f) * Roughness;
			In.p50 = ftcore::propHistogramPercentile(RoughHist, 0.5f) * Roughness;
			In.p90 = ftcore::propHistogramPercentile(RoughHist, 0.9f) * Roughness;
			In.metallic = ftcore::propHistogramPercentile(MetalHist, 0.5f) * Metallic;
			From = FString::Printf(TEXT("%s: G x %.2f, B x %.2f"), *Map->GetName(), Roughness, Metallic);
		}
		else
		{
			if (Map)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("%s: gloss %s: %s has no source data; checked on its factors alone."), GTag,
					*Material->GetName(), *Map->GetName());
			}
			In.p10 = In.p50 = In.p90 = Roughness;
			In.metallic = Metallic;
		}
		const ftcore::PropGlossResult Result = ftcore::propGlossCheck(In);
		const FString Spread = !In.hasMap ? FString(TEXT("spread rule skipped: flat material (the master's break-up gives it its spread)"))
			: Result.smooth			  ? FString(TEXT("spread rule skipped: smooth by name"))
									  : FString::Printf(TEXT("spread %.2f"), In.p90 - In.p10);
		UE_LOG(LogForgottenThird, Display, TEXT("%s: gloss %s: roughness p10 %.2f p50 %.2f p90 %.2f, metallic %.2f (%s); %s%s%s: %s."), GTag,
			*Material->GetName(), In.p10, In.p50, In.p90, In.metallic, *From, *Spread, Result.smooth ? TEXT("; smooth by name") : TEXT(""),
			Result.metal ? TEXT("; a metal by name") : TEXT(""), Result.pass ? TEXT("pass") : TEXT("FAIL"));
		const auto Fail = [&Material](bool bFailed, ftcore::PropGlossRule Rule)
		{
			if (bFailed)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("%s: gloss %s: %s"), GTag, *Material->GetName(), *FT::ToFString(ftcore::propGlossReason(Rule)));
			}
		};
		Fail(Result.wrongMetal, ftcore::PropGlossRule::Metal);
		Fail(Result.glossy, ftcore::PropGlossRule::Glossy);
		Fail(Result.flatMap, ftcore::PropGlossRule::FlatMap);
		if (!Result.pass)
		{
			++Failed;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: gloss guard: %d materials checked, %d failed."), GTag, Checked, Failed);
	return Failed == 0;
}

void AllowNanite(const TCHAR* Tag, TConstArrayView<UMaterialInterface*> Materials)
{
	TArray<UPackage*> Dirty;
	for (UMaterialInterface* Material : Materials)
	{
		UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
		if (!Base || !Base->GetPathName().StartsWith(TEXT("/Game/")) || Base->GetUsageByFlag(MATUSAGE_Nanite))
		{
			continue;
		}
		Base->SetUsageByFlag(MATUSAGE_Nanite, true);
		Base->PostEditChange();
		Base->MarkPackageDirty();
		Dirty.AddUnique(Base->GetPackage());
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s now draws on Nanite meshes."), Tag, *Base->GetName());
	}
	if (!Dirty.IsEmpty())
	{
		UEditorLoadingAndSavingUtils::SavePackages(Dirty, false);
	}
}

int32 CompileErrors(const TCHAR* Tag, UMaterial* Master)
{
	GTag = Tag;
	return MasterCompileErrors(Master);
}

const TCHAR* SkinMasterPath()
{
	return kSkinMaster;
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
