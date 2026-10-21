#include "FTImportCharacterCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "FTText.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ftcore/Face.h"
#include "ftcore/FaceImport.h"
#include "ftcore/Glb.h"

namespace
{
const TCHAR* const kFaceBones[] = {
	TEXT("head"), TEXT("jaw"), TEXT("tongue_01"), TEXT("tongue_02"), TEXT("tongue_03"), TEXT("eye_l"), TEXT("eye_r"),
};

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
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: made %s."), *Material->GetPathName());
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: %s samples as type %d, but %s needs type %d."),
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
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %s has no material resource to check."), *Master->GetName());
		return 0;
	}
	Resource->FinishCompilation();
	for (const FString& Error : Resource->GetCompileErrors())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: %s: %s"), *Master->GetName(), *Error);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %s compiled with %d errors."), *Master->GetName(),
		Resource->GetCompileErrors().Num());
	return Resource->GetCompileErrors().Num();
}

// False when a texture couldn't be set up or a master doesn't compile.
bool UseCharacterMaster(const USkeletalMesh& Mesh)
{
	TArray<UPackage*> Dirty;
	UMaterial* Flat = nullptr;
	UMaterial* Textured = nullptr;
	int32 Moved = 0;
	bool bOk = true;
	for (const FSkeletalMaterial& Slot : Mesh.GetMaterials())
	{
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Slot.MaterialInterface);
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
			UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %s texture parameter %s = %s -> %s"),
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
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: moved %d materials onto the character masters."), Moved);
	return bOk;
}

// Checks a glb against the asset rules (ftcore::checkGlb) before anything is imported.
bool CheckGlbFile(const FString& Source, const FString& Pack, const FString& Role)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: couldn't read %s."), *Source);
		return false;
	}
	ftcore::GlbInfo Info;
	std::string Error;
	if (!ftcore::readGlb(FT::BytesAsText(Bytes), Info, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: %s: %s"), *Source, *FT::ToFString(Error));
		return false;
	}
	const ftcore::AssetRules Rules = ftcore::characterRules(
		ftcore::assetPackFromName(TCHAR_TO_UTF8(*Pack)), ftcore::assetRoleFromName(TCHAR_TO_UTF8(*Role)));
	const std::vector<ftcore::AssetIssue> Issues = ftcore::checkGlb(Info, Rules);
	for (const ftcore::AssetIssue& Issue : Issues)
	{
		const FString Line = FString::Printf(TEXT("FTImportCharacter: [%s] %s"), *FT::ToFString(Issue.code), *FT::ToFString(Issue.message));
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
		TEXT("FTImportCharacter: asset rules (pack %s, role %s): %d materials, %d images, %d errors, %d warnings."),
		Pack.IsEmpty() ? TEXT("none") : *Pack, Role.IsEmpty() ? TEXT("supporting") : *Role,
		static_cast<int32>(Info.materials.size()), static_cast<int32>(Info.images.size()), Errors,
		static_cast<int32>(Issues.size()) - Errors);
	return Errors == 0;
}

// Logs the rig check; false when a face bone or (with bCheckFace) a face shape's morph is missing.
bool CheckRig(const USkeletalMesh& Mesh, bool bCheckFace)
{
	bool bOk = true;

	TSet<FName> Morphs;
	for (const UMorphTarget* Morph : Mesh.GetMorphTargets())
	{
		if (Morph)
		{
			Morphs.Add(Morph->GetFName());
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %s has %d morph targets."), *Mesh.GetPathName(), Morphs.Num());

	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	int32 BonesFound = 0;
	for (const TCHAR* Bone : kFaceBones)
	{
		if (Skeleton.FindBoneIndex(Bone) != INDEX_NONE)
		{
			++BonesFound;
		}
		else
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: face bone '%s' is missing."), Bone);
			bOk = false;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %d/%d face bones, %d bones in all."), BonesFound,
		UE_ARRAY_COUNT(kFaceBones), Skeleton.GetNum());

	if (!bCheckFace)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: -Face=0, skipping the face shape check."));
		return bOk;
	}

	const FString PresetsPath = FPaths::ProjectDir() / TEXT("Data/Rig/face_presets.json");
	TArray<uint8> Bytes;
	ftcore::FaceShapeSet Shapes;
	std::string Error;
	if (!FFileHelper::LoadFileToArray(Bytes, *PresetsPath) || !ftcore::importFaceShapeSet(FT::BytesAsText(Bytes), Shapes, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: couldn't load %s: %s"), *PresetsPath, *FT::ToFString(Error));
		return false;
	}
	int32 Bound = 0;
	for (const std::string& Shape : Shapes.shapes)
	{
		const FName Name(FT::ToFString(Shape));
		if (Morphs.Contains(Name))
		{
			++Bound;
		}
		else
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: no morph target for face shape '%s'."), *Name.ToString());
			bOk = false;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: %d/%d face shapes bound."), Bound, static_cast<int32>(Shapes.shapes.size()));
	return bOk;
}
} // namespace
#endif

UFTImportCharacterCommandlet::UFTImportCharacterCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTImportCharacterCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString Source, Dest = TEXT("/Game/Characters/ProofHead");
	if (!FParse::Value(*Params, TEXT("Source="), Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: pass -Source=<file>."));
		return 1;
	}
	FParse::Value(*Params, TEXT("Dest="), Dest);
	bool bCheckFace = true;
	FParse::Bool(*Params, TEXT("Face="), bCheckFace);
	FString Pack, Role;
	FParse::Value(*Params, TEXT("Pack="), Pack);
	FParse::Value(*Params, TEXT("Role="), Role);
	if (FPaths::IsRelative(Source))
	{
		Source = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Source);
	}
	if (!FPaths::FileExists(Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: %s doesn't exist."), *Source);
		return 1;
	}
	if (FPaths::GetExtension(Source).Equals(TEXT("glb"), ESearchCase::IgnoreCase) && !CheckGlbFile(Source, Pack, Role))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: %s breaks the asset rules (docs/ASSET_INTAKE.md); nothing imported."),
			*Source);
		return 1;
	}

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Source;
	Task->DestinationPath = Dest;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bSave = true;
	Task->bAsync = false;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.ImportAssetTasks({ Task });

	const USkeletalMesh* Mesh = nullptr;
	for (const UObject* Object : Task->GetObjects())
	{
		if (!Object)
		{
			continue;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: imported %s (%s)"), *Object->GetPathName(),
			*Object->GetClass()->GetName());
		if (!Mesh)
		{
			Mesh = Cast<USkeletalMesh>(Object);
		}
	}
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter: no skeletal mesh came out of %s."), *Source);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportCharacter: mesh is %s"), *Mesh->GetPathName());
	const bool bMaterialsOk = UseCharacterMaster(*Mesh);
	const bool bRigOk = CheckRig(*Mesh, bCheckFace);
	return bMaterialsOk && bRigOk ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTImportCharacter needs an editor build."));
	return 1;
#endif
}
