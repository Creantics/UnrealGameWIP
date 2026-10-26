#include "FTBuildSkinCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SubsurfaceProfile.h"
#include "Engine/Texture2D.h"
#include "FTImportMaterials.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/SkinDetail.h"
#endif

#if WITH_EDITOR
// Named, not anonymous: in a unity build the other material commandlets' own AddExpression and PackageFile share this
// translation unit.
namespace FTSkinBuild
{
const TCHAR* const kProfile = TEXT("/Game/Characters/Shared/Materials/SP_FTSkin");
const TCHAR* const kPores = TEXT("/Game/Characters/Shared/Materials/T_FTSkinPores");
const TCHAR* const kPoresMask = TEXT("/Game/Characters/Shared/Materials/T_FTSkinPoresMask");
const TCHAR* const kNeutralMask = TEXT("/Game/Characters/Shared/Materials/T_FTSkinMaskNeutral");

// The master's scalar parameters and their defaults; -Scalars overrides them on the instance.
struct FScalarSpec
{
	const TCHAR* Name;
	float Default;
};
const FScalarSpec kScalars[] = {
	{TEXT("DetailTiling"), 70.f},     // pore tile repeats per 0-1 UV (the atlas spans about 1.8 m of skin)
	{TEXT("DetailStrength"), 1.f},    // pore normal tilt scale
	{TEXT("Roughness"), 0.5f},        // open skin, before the region mask
	{TEXT("CavityRoughness"), 0.35f}, // extra roughness at the bottom of a pore
	{TEXT("MicroRoughness"), 0.6f},   // micro variation, as a fraction of Roughness
	{TEXT("Specular"), 0.35f},        // about 2.8% reflectance at normal incidence
	{TEXT("CavitySpecular"), 0.45f},  // specular left at the bottom of a pore
	{TEXT("Scatter"), 1.f},           // subsurface amount (Opacity), before the region mask
};

FString PackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

template <typename T>
T* AddExpression(UMaterial* Material, int32 X, int32 Y)
{
	T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

UMaterialExpressionScalarParameter* Scalar(UMaterial* Material, const TCHAR* Name, int32 Y)
{
	float Default = 0.f;
	for (const FScalarSpec& Spec : kScalars)
	{
		if (FCString::Strcmp(Spec.Name, Name) == 0)
		{
			Default = Spec.Default;
		}
	}
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -1400, Y);
	P->ParameterName = Name;
	P->DefaultValue = Default;
	P->Group = TEXT("Skin");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionTextureSampleParameter2D* Sampler(UMaterial* Material, const TCHAR* Name, UTexture* Default,
	EMaterialSamplerType Type, UMaterialExpression* Coordinates, int32 Y)
{
	UMaterialExpressionTextureSampleParameter2D* S =
		AddExpression<UMaterialExpressionTextureSampleParameter2D>(Material, -1000, Y);
	S->ParameterName = Name;
	S->Texture = Default;
	S->SamplerType = Type;
	S->Group = TEXT("Skin");
	if (Coordinates)
	{
		S->Coordinates.Connect(0, Coordinates);
	}
	S->UpdateParameterGuid(true, false);
	return S;
}

// A Custom HLSL node; each input is (name, expression, output index).
struct FIn
{
	const TCHAR* Name;
	UMaterialExpression* From;
	int32 Output;
};
UMaterialExpressionCustom* Custom(UMaterial* Material, const TCHAR* Description, ECustomMaterialOutputType Type,
	const TCHAR* Code, std::initializer_list<FIn> Inputs, int32 Y)
{
	UMaterialExpressionCustom* C = AddExpression<UMaterialExpressionCustom>(Material, -500, Y);
	C->Description = Description;
	C->OutputType = Type;
	C->Code = Code;
	C->Inputs.Reset();
	for (const FIn& In : Inputs)
	{
		FCustomInput& Slot = C->Inputs.AddDefaulted_GetRef();
		Slot.InputName = In.Name;
		Slot.Input.Connect(In.Output, In.From);
	}
	return C;
}

UTexture2D* NewTexture(const FString& Path, int32 Size, const TArray<uint8>& Bgra, bool bSRGB,
	TextureCompressionSettings Compression)
{
	UPackage* Package = CreatePackage(*Path);
	UTexture2D* Texture = NewObject<UTexture2D>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, Bgra.GetData());
	Texture->SRGB = bSRGB;
	Texture->CompressionSettings = Compression;
	Texture->VirtualTextureStreaming = false;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	return Texture;
}

// Imports a PNG into Path, reusing the texture there if one exists.
UTexture2D* ImportPng(const FString& File, const FString& Path, bool bSRGB, TextureCompressionSettings Compression)
{
	FImage Image;
	if (!FImageUtils::LoadImage(*File, Image))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSkin: couldn't read %s."), *File);
		return nullptr;
	}
	const FString Name = FPackageName::GetShortName(Path);
	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *(Path + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Texture)
	{
		UPackage* Package = CreatePackage(*Path);
		Texture = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Texture);
	}
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(Image);
	Texture->SRGB = bSRGB;
	Texture->CompressionSettings = Compression;
	Texture->VirtualTextureStreaming = false;
	Texture->PostEditChange();
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildSkin: %s <- %s (%dx%d)."), *Path, *File, Image.SizeX, Image.SizeY);
	return Texture;
}

USubsurfaceProfile* BuildProfile(float ScatterScale)
{
	UPackage* Package = CreatePackage(kProfile);
	USubsurfaceProfile* Profile = NewObject<USubsurfaceProfile>(Package, *FPackageName::GetShortName(kProfile),
		RF_Public | RF_Standalone | RF_Transactional);
	// The struct's defaults are Unreal's human skin, matched to its Burley form; only the scatter distance scales.
	FSubsurfaceProfileStruct& S = Profile->Settings;
	S.bEnableBurley = true;
	S.bEnableMeanFreePath = true;
	S.MeanFreePathDistance *= ScatterScale;
	Profile->Guid = FGuid::NewGuid();
	FAssetRegistryModule::AssetCreated(Profile);
	return Profile;
}

UMaterial* BuildMaster(USubsurfaceProfile* Profile, UTexture* Pores, UTexture* PoresMask, UTexture* NeutralMask)
{
	const FString Path = FT::SkinMasterPath();
	UPackage* Package = CreatePackage(*Path);
	UMaterial* Material = NewObject<UMaterial>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Material->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
	Material->SetUsageByFlag(MATUSAGE_MorphTargets, true);
	Material->SetShadingModel(MSM_SubsurfaceProfile);
	Material->SubsurfaceProfile = Profile;
	Material->TwoSided = false;

	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Game/Characters/Shared/Materials/T_FTWhite.T_FTWhite"));
	UTexture* Flat = LoadObject<UTexture>(nullptr, TEXT("/Game/Characters/Shared/Materials/T_FTFlatNormal.T_FTFlatNormal"));

	UMaterialExpressionTextureCoordinate* UV = AddExpression<UMaterialExpressionTextureCoordinate>(Material, -1700, 0);
	UMaterialExpressionMultiply* DetailUV = AddExpression<UMaterialExpressionMultiply>(Material, -1250, 300);
	DetailUV->A.Connect(0, UV);
	DetailUV->B.Connect(0, Scalar(Material, TEXT("DetailTiling"), 350));

	UMaterialExpressionTextureSampleParameter2D* Base = Sampler(Material, TEXT("BaseColorTexture"), White, SAMPLERTYPE_Color, nullptr, -600);
	UMaterialExpressionTextureSampleParameter2D* Normal = Sampler(Material, TEXT("NormalTexture"), Flat, SAMPLERTYPE_Normal, nullptr, -300);
	UMaterialExpressionTextureSampleParameter2D* Region = Sampler(Material, TEXT("SkinMaskTexture"), NeutralMask, SAMPLERTYPE_Masks, nullptr, 0);
	UMaterialExpressionTextureSampleParameter2D* Pore = Sampler(Material, TEXT("PoreNormal"), Pores, SAMPLERTYPE_Normal, DetailUV, 300);
	UMaterialExpressionTextureSampleParameter2D* PoreMask = Sampler(Material, TEXT("PoreMask"), PoresMask, SAMPLERTYPE_Masks, DetailUV, 600);

	UMaterialExpressionVectorParameter* Tint = AddExpression<UMaterialExpressionVectorParameter>(Material, -1000, -800);
	Tint->ParameterName = TEXT("BaseColor");
	Tint->DefaultValue = FLinearColor::White;
	Tint->Group = TEXT("Skin");
	Tint->UpdateParameterGuid(true, false);
	UMaterialExpressionMultiply* Albedo = AddExpression<UMaterialExpressionMultiply>(Material, -500, -700);
	Albedo->A.Connect(0, Base);
	Albedo->B.Connect(0, Tint);

	// Texture sample outputs: 0 RGB, 1 R, 2 G, 3 B, 4 A.
	UMaterialExpressionCustom* NormalOut = Custom(Material, TEXT("SkinNormal"), CMOT_Float3,
		TEXT("float2 d = Pore.xy * Strength;\nreturn normalize(float3(Base.xy + d, Base.z * Pore.z));"),
		{{TEXT("Base"), Normal, 0}, {TEXT("Pore"), Pore, 0}, {TEXT("Strength"), Scalar(Material, TEXT("DetailStrength"), 450), 0}}, -300);
	UMaterialExpressionCustom* RoughnessOut = Custom(Material, TEXT("SkinRoughness"), CMOT_Float1,
		TEXT("float r = Rough * Region * 2.0;\nr *= 1.0 + CavityRough * (1.0 - Cavity);\nr *= 1.0 + Micro * (MicroMask * 2.0 - 1.0) * 3.3;\nreturn saturate(r);"),
		{{TEXT("Rough"), Scalar(Material, TEXT("Roughness"), 550), 0}, {TEXT("Region"), Region, 2}, {TEXT("Cavity"), PoreMask, 1},
			{TEXT("CavityRough"), Scalar(Material, TEXT("CavityRoughness"), 650), 0}, {TEXT("MicroMask"), PoreMask, 2},
			{TEXT("Micro"), Scalar(Material, TEXT("MicroRoughness"), 750), 0}},
		0);
	UMaterialExpressionCustom* SpecularOut = Custom(Material, TEXT("SkinSpecular"), CMOT_Float1,
		TEXT("return Spec * lerp(CavitySpec, 1.0, Cavity);"),
		{{TEXT("Spec"), Scalar(Material, TEXT("Specular"), 850), 0}, {TEXT("CavitySpec"), Scalar(Material, TEXT("CavitySpecular"), 950), 0},
			{TEXT("Cavity"), PoreMask, 1}},
		300);
	UMaterialExpressionMultiply* ScatterOut = AddExpression<UMaterialExpressionMultiply>(Material, -500, 600);
	ScatterOut->A.Connect(3, Region);
	ScatterOut->B.Connect(0, Scalar(Material, TEXT("Scatter"), 1050));

	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Albedo);
	Inputs->Normal.Connect(0, NormalOut);
	Inputs->Roughness.Connect(0, RoughnessOut);
	Inputs->Specular.Connect(0, SpecularOut);
	Inputs->AmbientOcclusion.Connect(1, Region);
	Inputs->Opacity.Connect(0, ScatterOut);

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	FAssetRegistryModule::AssetCreated(Material);
	return Material;
}

bool Save(UObject* Asset)
{
	UPackage* Package = Asset->GetPackage();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(Package->GetName());
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSkin: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildSkin: saved %s."), *File);
	return true;
}
} // namespace FTSkinBuild
#endif

UFTBuildSkinCommandlet::UFTBuildSkinCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildSkinCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MeshPath, AlbedoFile, MaskFile, ScalarList, DumpDir;
	int32 Slot = 0;
	float ScatterScale = 1.f;
	FParse::Value(*Params, TEXT("Mesh="), MeshPath);
	FParse::Value(*Params, TEXT("Slot="), Slot);
	FParse::Value(*Params, TEXT("Albedo="), AlbedoFile);
	FParse::Value(*Params, TEXT("Mask="), MaskFile);
	FParse::Value(*Params, TEXT("ScatterScale="), ScatterScale);
	FParse::Value(*Params, TEXT("Scalars="), ScalarList, false);
	FParse::Value(*Params, TEXT("DumpDir="), DumpDir);
	if (MeshPath.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSkin: give -Mesh=<skeletal mesh object path>."));
		return 1;
	}
	if (!FApp::CanEverRender())
	{
		// Without a renderer the master never compiles here, so its errors would go unseen.
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSkin: run with -AllowCommandletRendering, so the master compiles and is checked."));
		return 1;
	}

	// The registry's startup scan opens each package file to read its header; let it finish so the deletes don't race it.
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().WaitForCompletion();
	// Start the shared assets from nothing, before anything loads the old ones.
	const FString Shared[] = {
		FTSkinBuild::kProfile, FTSkinBuild::kPores, FTSkinBuild::kPoresMask, FTSkinBuild::kNeutralMask, FT::SkinMasterPath()};
	for (const FString& Package : Shared)
	{
		const FString File = FTSkinBuild::PackageFile(Package);
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSkin: couldn't replace %s."), *File);
			return 1;
		}
	}

	ftcore::SkinDetailSettings Detail;
	std::vector<float> Height;
	std::vector<uint8_t> NormalBytes, MaskBytes;
	if (!ftcore::makeSkinHeight(Detail, Height))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSkin: bad skin detail settings."));
		return 1;
	}
	ftcore::skinNormalBgra(Height, Detail.size, Detail.normalStrength, NormalBytes);
	ftcore::skinMaskBgra(Height, Detail.size, MaskBytes);
	const int32 Size = static_cast<int32>(Detail.size);
	const TArray<uint8> NormalArray(NormalBytes.data(), static_cast<int32>(NormalBytes.size()));
	const TArray<uint8> MaskArray(MaskBytes.data(), static_cast<int32>(MaskBytes.size()));
	if (!DumpDir.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*DumpDir, true);
		FImageUtils::SaveImageByExtension(*(DumpDir / TEXT("skin_pores_normal.png")),
			FImageView(reinterpret_cast<const FColor*>(NormalArray.GetData()), Size, Size, EGammaSpace::Linear));
		FImageUtils::SaveImageByExtension(*(DumpDir / TEXT("skin_pores_mask.png")),
			FImageView(reinterpret_cast<const FColor*>(MaskArray.GetData()), Size, Size, EGammaSpace::Linear));
	}
	TArray<uint8> Neutral;
	for (int32 i = 0; i < 16; ++i)
	{
		Neutral.Append({255, 128, 255, 255}); // B scatter, G roughness scale, R occlusion, A
	}

	TArray<UObject*> ToSave;
	USubsurfaceProfile* Profile = FTSkinBuild::BuildProfile(ScatterScale);
	UTexture2D* Pores = FTSkinBuild::NewTexture(FTSkinBuild::kPores, Size, NormalArray, false, TC_Normalmap);
	UTexture2D* PoresMask = FTSkinBuild::NewTexture(FTSkinBuild::kPoresMask, Size, MaskArray, false, TC_Masks);
	UTexture2D* NeutralMask = FTSkinBuild::NewTexture(FTSkinBuild::kNeutralMask, 4, Neutral, false, TC_Masks);
	NeutralMask->MipGenSettings = TMGS_NoMipmaps;
	NeutralMask->PostEditChange();
	UMaterial* Master = FTSkinBuild::BuildMaster(Profile, Pores, PoresMask, NeutralMask);
	ToSave.Add(Profile);
	ToSave.Add(Pores);
	ToSave.Add(PoresMask);
	ToSave.Add(NeutralMask);
	ToSave.Add(Master);

	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh || !Mesh->GetMaterials().IsValidIndex(Slot))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSkin: no skeletal mesh at %s, or no slot %d."), *MeshPath, Slot);
		return 1;
	}
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Mesh->GetMaterials()[Slot].MaterialInterface);
	if (!Instance || !Instance->GetPathName().StartsWith(TEXT("/Game/")))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSkin: slot %d of %s isn't a project material instance."), Slot, *MeshPath);
		return 1;
	}
	UTexture* Albedo = nullptr;
	Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColorTexture")), Albedo);
	UTexture* RegionMask = nullptr;
	Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("SkinMaskTexture")), RegionMask);

	// Imported maps sit beside the mesh's other textures: <mesh folder>/Textures.
	const FString TextureFolder = FPaths::GetPath(FPackageName::GetLongPackagePath(Instance->GetPackage()->GetName())) / TEXT("Textures");
	const FString MeshName = FPackageName::GetShortName(Mesh->GetPackage()->GetName());
	if (!AlbedoFile.IsEmpty())
	{
		Albedo = FTSkinBuild::ImportPng(AlbedoFile, TextureFolder / (TEXT("T_") + MeshName + TEXT("_SkinAlbedo")), true, TC_Default);
		if (!Albedo)
		{
			return 1;
		}
		ToSave.Add(Albedo);
	}
	if (!MaskFile.IsEmpty())
	{
		RegionMask = FTSkinBuild::ImportPng(MaskFile, TextureFolder / (TEXT("T_") + MeshName + TEXT("_SkinMask")), false, TC_Masks);
		if (!RegionMask)
		{
			return 1;
		}
		ToSave.Add(RegionMask);
	}

	// Overrides left by the importer (blend mode, two-sided, shading model) would beat the master's own settings.
	Instance->BasePropertyOverrides = FMaterialInstanceBasePropertyOverrides();
	Instance->ClearParameterValuesEditorOnly();
	Instance->SetParentEditorOnly(Master);
	if (Albedo)
	{
		Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BaseColorTexture")), Albedo);
	}
	if (RegionMask)
	{
		Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("SkinMaskTexture")), RegionMask);
	}
	TArray<FString> Pairs;
	ScalarList.ParseIntoArray(Pairs, TEXT(","));
	for (const FString& Pair : Pairs)
	{
		FString Name, Value;
		if (!Pair.Split(TEXT(":"), &Name, &Value))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSkin: -Scalars entry \"%s\" isn't Name:Value."), *Pair);
			return 1;
		}
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(*Name), FCString::Atof(*Value));
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildSkin: %s = %s."), *Name, *Value);
	}
	Instance->PostEditChange();
	ToSave.Add(Instance);

	FAssetCompilingManager::Get().FinishAllCompilation();
	if (FT::CompileErrors(TEXT("FTBuildSkin"), Master) > 0)
	{
		return 1;
	}
	bool bOk = true;
	for (UObject* Asset : ToSave)
	{
		bOk &= FTSkinBuild::Save(Asset);
	}
	if (!bOk)
	{
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildSkin: %s slot %d now on %s (%d expressions, scatter x%.2f)."), *MeshPath, Slot,
		*Master->GetName(), Master->GetExpressionCollection().Expressions.Num(), ScatterScale);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSkin needs an editor build."));
	return 1;
#endif
}
