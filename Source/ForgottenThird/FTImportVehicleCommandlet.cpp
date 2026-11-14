#include "FTImportVehicleCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "FTBuildKeiCars.h"
#include "FTImportMaterials.h"
#include "FTSceneLook.h"
#include "FTWorldWave.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/ParkedCarLook.h"

namespace FTKeiCarImport
{
// The five maps: the file's and texture's name part, how each is stored, and how the master samples it.
struct FMapSpec
{
	const TCHAR* Name;
	const TCHAR* Param;
	bool bSRGB;
	TextureCompressionSettings Compression;
	TextureGroup Group;
	EMaterialSamplerType Sampler;
};
const FMapSpec kMaps[] = {
	{TEXT("BaseColor"), TEXT("BaseColorTex"), true, TC_Default, TEXTUREGROUP_Vehicle, SAMPLERTYPE_Color},
	{TEXT("Wear"), TEXT("WearTex"), true, TC_Default, TEXTUREGROUP_Vehicle, SAMPLERTYPE_Color},
	{TEXT("Normal"), TEXT("NormalTex"), false, TC_Normalmap, TEXTUREGROUP_VehicleNormalMap, SAMPLERTYPE_Normal},
	{TEXT("ORM"), TEXT("OrmTex"), false, TC_Masks, TEXTUREGROUP_Vehicle, SAMPLERTYPE_Masks},
	{TEXT("Mask"), TEXT("MaskTex"), false, TC_Masks, TEXTUREGROUP_Vehicle, SAMPLERTYPE_Masks},
};
constexpr int32 kMapCount = UE_ARRAY_COUNT(kMaps);

// The slots by the FBX material's name's last part, in Slot order (the master's Slot parameter: 4 glass, 5 lens).
const TCHAR* const kSlots[] = {TEXT("Paint"), TEXT("Trim"), TEXT("Rubber"), TEXT("Metal"), TEXT("Glass"), TEXT("Lens")};
constexpr int32 kSlotCount = UE_ARRAY_COUNT(kSlots);
constexpr int32 kPaintSlot = 0;
constexpr int32 kGlassSlot = 4;

// The export's LOD triangle counts, and the screen sizes the parked cars switch at (the explore camera's 70 degrees:
// LOD1 from about 15 m, LOD2 from about 43 m; the cells cull cars at 200 m).
constexpr int32 kLodTriangles[] = {27786, 8120, 1632};
constexpr float kLodScreenSize[] = {1.f, 0.35f, 0.12f};
constexpr int32 kLodCount = UE_ARRAY_COUNT(kLodTriangles);

// The contract's collision box (cm), used when the file's UBX_ box doesn't come through as the one simple shape.
const FVector kBoxMin(-171.13, -74.0, 15.2);
const FVector kBoxMax(173.1, 74.0, 155.02);

// The car material contract's maths for every slot: the paint tint where the mask's green says paint, the wear map and
// rougher finish by the mask's wear times DirtAmount, glass smooth and see-through by its own formula, the lens lit by
// LampOn at night. Texture2DSample, never Tex.Sample: a ray-tracing hit shader has no derivatives.
const TCHAR* const kCarCode = TEXT(R"HLSL(float3 bc = Texture2DSample(BaseColorTex, BaseColorTexSampler, UV).rgb;
float3 wear = Texture2DSample(WearTex, WearTexSampler, UV).rgb;
float3 orm = Texture2DSample(OrmTex, OrmTexSampler, UV).rgb;
float4 m = Texture2DSample(MaskTex, MaskTexSampler, UV);
float d = saturate(m.r * Dirt);
bool IsGlass = abs(Slot - 4.0) < 0.5;
bool IsLens = abs(Slot - 5.0) < 0.5;
RoughOut = IsGlass ? GlassRoughness : lerp(orm.g, 0.9, saturate(d * 0.8));
MetalOut = IsGlass ? 0.0 : orm.b;
AOOut = orm.r;
NormalOut = UnpackNormalMap(Texture2DSample(NormalTex, NormalTexSampler, UV)).xyz;
EmissiveOut = IsLens ? bc * m.b * saturate(Lamp) * saturate(Night) * LampGain : float3(0.0, 0.0, 0.0);
OpacityOut = saturate(0.30 - 0.22 * m.b + 0.5 * d);
float3 col = lerp(bc, bc * Paint.rgb * 1.25, m.g);
return lerp(col, wear, d);)HLSL");

FString PackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

bool SaveAsset(UObject* Asset)
{
	UPackage* Package = Asset->GetPackage();
	const FString File = PackageFile(Package->GetName());
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: couldn't save %s."), *File);
		return false;
	}
	return true;
}

// An 8-bit PNG as a new texture at Path, its pixels as they are (no gamma change: the linear maps keep their values).
UTexture2D* ImportMap(const FString& File, const FString& Path, const FMapSpec& Map)
{
	TArray<uint8> Bytes;
	FImage Image;
	if (!FFileHelper::LoadFileToArray(Bytes, *File) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: couldn't read %s as an image."), *File);
		return nullptr;
	}
	if (Image.Format != ERawImageFormat::BGRA8 && Image.Format != ERawImageFormat::G8)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: %s isn't an 8-bit image."), *File);
		return nullptr;
	}
	if (Image.Format != ERawImageFormat::BGRA8)
	{
		Image.ChangeFormat(ERawImageFormat::BGRA8, Image.GammaSpace);
	}
	UPackage* Package = CreatePackage(*Path);
	UTexture2D* Texture =
		NewObject<UTexture2D>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(Image.SizeX, Image.SizeY, 1, 1, TSF_BGRA8, Image.RawData.GetData());
	Texture->SRGB = Map.bSRGB;
	Texture->CompressionSettings = Map.Compression;
	Texture->LODGroup = Map.Group;
	// The master samples in a Custom node (Texture2DSample), which can't read a virtual texture.
	Texture->VirtualTextureStreaming = false;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportVehicle: %s: %d x %d, %s."), *FPaths::GetCleanFilename(File), Image.SizeX,
		Image.SizeY, Map.bSRGB ? TEXT("sRGB") : TEXT("linear"));
	return Texture;
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

UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, FName Name, float Value, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -1100, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("KeiCar");
	P->UpdateParameterGuid(true, false);
	return P;
}

// A per-instance custom data node (its class isn't exported from the engine's module, so it is found by name) reading
// slot DataIndex, falling back to Default when the mesh isn't instanced.
UMaterialExpression* AddPerInstance(UMaterial* Material, bool bVector, uint32 DataIndex, UMaterialExpression* Default, int32 Y)
{
	const TCHAR* const ClassName =
		bVector ? TEXT("MaterialExpressionPerInstanceCustomData3Vector") : TEXT("MaterialExpressionPerInstanceCustomData");
	UClass* const Class = FindObject<UClass>(FTopLevelAssetPath(TEXT("/Script/Engine"), ClassName));
	if (!Class)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: no %s node class."), ClassName);
		return nullptr;
	}
	UMaterialExpression* Node = NewObject<UMaterialExpression>(Material, Class, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Node);
	Node->Material = Material;
	Node->MaterialExpressionEditorX = -800;
	Node->MaterialExpressionEditorY = Y;
	Node->UpdateMaterialExpressionGuid(true, false);
	if (bVector)
	{
		UMaterialExpressionPerInstanceCustomData3Vector* V = static_cast<UMaterialExpressionPerInstanceCustomData3Vector*>(Node);
		V->DataIndex = DataIndex;
		V->DefaultValue.Connect(0, Default);
	}
	else
	{
		UMaterialExpressionPerInstanceCustomData* S = static_cast<UMaterialExpressionPerInstanceCustomData*>(Node);
		S->DataIndex = DataIndex;
		S->DefaultValue.Connect(0, Default);
	}
	return Node;
}

void AddInput(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(0, From);
}

void AddOutput(UMaterialExpressionCustom* Custom, FName Name, ECustomMaterialOutputType Type)
{
	FCustomOutput& Out = Custom->AdditionalOutputs.AddDefaulted_GetRef();
	Out.OutputName = Name;
	Out.OutputType = Type;
}

// One of the car's masters at Path: the shared graph with this shading model and blend mode. Night is the window
// collection's (null: the lamps never light).
UMaterial* BuildMaster(const FString& Path, EMaterialShadingModel Shading, EBlendMode Blend, UTexture2D* const* Maps,
	UMaterialParameterCollection* Collection)
{
	UPackage* Package = CreatePackage(*Path);
	UMaterial* Material = NewObject<UMaterial>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Material->bTangentSpaceNormal = true;
	Material->SetShadingModel(Shading);
	Material->BlendMode = Blend;
	if (Blend == BLEND_Translucent)
	{
		Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
	}

	UMaterialExpressionCustom* Car = AddExpression<UMaterialExpressionCustom>(Material, -400, 0);
	Car->Description = TEXT("FTKeiCar");
	Car->OutputType = CMOT_Float3;
	Car->Code = kCarCode;
	Car->Inputs.Reset();
	for (int32 I = 0; I < kMapCount; ++I)
	{
		UMaterialExpressionTextureObjectParameter* P =
			AddExpression<UMaterialExpressionTextureObjectParameter>(Material, -1100, -900 + 150 * I);
		P->ParameterName = kMaps[I].Param;
		P->Texture = Maps[I];
		P->SamplerType = kMaps[I].Sampler;
		P->Group = TEXT("KeiCar");
		P->UpdateParameterGuid(true, false);
		AddInput(Car, kMaps[I].Param, P);
	}
	UMaterialExpressionTextureCoordinate* Uv = AddExpression<UMaterialExpressionTextureCoordinate>(Material, -1100, -100);
	Uv->CoordinateIndex = 0;
	AddInput(Car, TEXT("UV"), Uv);

	UMaterialExpressionVectorParameter* PaintColor = AddExpression<UMaterialExpressionVectorParameter>(Material, -1100, 0);
	PaintColor->ParameterName = TEXT("PaintColor");
	const ftcore::ParkedCarColor Default = ftcore::parkedCarPaint(0);
	PaintColor->DefaultValue = FLinearColor(Default.r, Default.g, Default.b);
	PaintColor->Group = TEXT("KeiCar");
	PaintColor->UpdateParameterGuid(true, false);
	UMaterialExpression* Paint = AddPerInstance(Material, true, 0, PaintColor, 0);
	UMaterialExpression* Dirt = AddPerInstance(Material, false, 3, AddScalar(Material, TEXT("DirtAmount"), 1.f, 100), 100);
	UMaterialExpression* Lamp = AddPerInstance(Material, false, 4, AddScalar(Material, TEXT("LampOn"), 0.f, 200), 200);
	if (!Paint || !Dirt || !Lamp)
	{
		return nullptr;
	}
	AddInput(Car, TEXT("Paint"), Paint);
	AddInput(Car, TEXT("Dirt"), Dirt);
	AddInput(Car, TEXT("Lamp"), Lamp);
	AddInput(Car, TEXT("Slot"), AddScalar(Material, TEXT("Slot"), 0.f, 300));
	AddInput(Car, TEXT("GlassRoughness"), AddScalar(Material, TEXT("GlassRoughness"), 0.02f, 400));
	AddInput(Car, TEXT("LampGain"), AddScalar(Material, TEXT("LampGain"), 8.f, 500));
	UMaterialExpression* Night = nullptr;
	if (Collection && Collection->GetParameterId(TEXT("Night")).IsValid())
	{
		UMaterialExpressionCollectionParameter* P = AddExpression<UMaterialExpressionCollectionParameter>(Material, -1100, 600);
		P->Collection = Collection;
		P->ParameterName = TEXT("Night");
		P->ParameterId = Collection->GetParameterId(TEXT("Night"));
		Night = P;
	}
	else
	{
		UMaterialExpressionConstant* Off = AddExpression<UMaterialExpressionConstant>(Material, -1100, 600);
		Off->R = 0.f;
		Night = Off;
	}
	AddInput(Car, TEXT("Night"), Night);
	// Outputs 1.. in this order.
	AddOutput(Car, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Car, TEXT("MetalOut"), CMOT_Float1);
	AddOutput(Car, TEXT("AOOut"), CMOT_Float1);
	AddOutput(Car, TEXT("NormalOut"), CMOT_Float3);
	AddOutput(Car, TEXT("EmissiveOut"), CMOT_Float3);
	AddOutput(Car, TEXT("OpacityOut"), CMOT_Float1);
	Car->RebuildOutputs();

	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Car);
	Inputs->Roughness.Connect(1, Car);
	Inputs->Metallic.Connect(2, Car);
	Inputs->AmbientOcclusion.Connect(3, Car);
	Inputs->Normal.Connect(4, Car);
	Inputs->EmissiveColor.Connect(5, Car);
	if (Blend == BLEND_Translucent)
	{
		Inputs->Opacity.Connect(6, Car);
	}
	if (Shading == MSM_ClearCoat)
	{
		Inputs->ClearCoat.Connect(0, AddScalar(Material, TEXT("ClearCoat"), 0.35f, 700));
		Inputs->ClearCoatRoughness.Connect(0, AddScalar(Material, TEXT("ClearCoatRoughness"), 0.08f, 800));
	}
	// The world wave's cut (FTWorldWave.h): a tagged car is clipped on the front's ragged edge (the body and coat turn
	// masked, the glass multiplies its opacity). MF_WorldWaveCut reads the look's collection: with the window collection
	// (Night) that is two, the most a material may.
	FTWorldWave::AddCut(TEXT("FTImportVehicle"), Material, -700, 1000);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	FAssetRegistryModule::AssetCreated(Material);
	return Material;
}

UMaterialInstanceConstant* BuildInstance(const FString& Path, UMaterial* Master, int32 Slot)
{
	UPackage* Package = CreatePackage(*Path);
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, *FPackageName::GetShortName(Path),
		RF_Public | RF_Standalone | RF_Transactional);
	Mic->SetParentEditorOnly(Master);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Slot")), static_cast<float>(Slot));
	Mic->PostEditChange();
	FAssetRegistryModule::AssetCreated(Mic);
	return Mic;
}

// The slot (kSlots index) whose name is in the material slot's name (Interchange may add a suffix), or INDEX_NONE.
int32 SlotOf(FName SlotName)
{
	const FString Name = SlotName.ToString();
	for (int32 I = 0; I < kSlotCount; ++I)
	{
		if (Name.Contains(kSlots[I], ESearchCase::IgnoreCase))
		{
			return I;
		}
	}
	return INDEX_NONE;
}

int32 Run(const FString& Params)
{
	FString Source, Dest = FTBuildKeiCars::Folder;
	if (!FParse::Value(*Params, TEXT("Source="), Source, false) || Source.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: pass -Source=<the export folder>."));
		return 1;
	}
	FParse::Value(*Params, TEXT("Dest="), Dest);
	Source.TrimQuotesInline();
	if (FPaths::IsRelative(Source))
	{
		Source = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Source);
	}
	const FString MeshFile = Source / TEXT("SM_KeiCar.fbx");
	TArray<FString> Files = {MeshFile};
	for (const FMapSpec& Map : kMaps)
	{
		Files.Add(Source / FString::Printf(TEXT("T_KeiCar_%s.png"), Map.Name));
	}
	for (const FString& File : Files)
	{
		if (!FPaths::FileExists(File))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: %s doesn't exist; nothing imported."), *File);
			return 1;
		}
	}

	// Replace every texture, master and instance file: a refreshed export keeps its names.
	TArray<FString> TexturePaths, InstancePaths;
	for (const FMapSpec& Map : kMaps)
	{
		TexturePaths.Add(Dest / TEXT("Textures") / FString::Printf(TEXT("T_KeiCar_%s"), Map.Name));
	}
	for (const TCHAR* Slot : kSlots)
	{
		InstancePaths.Add(Dest / TEXT("Materials") / FString::Printf(TEXT("MI_KeiCar_%s"), Slot));
	}
	const FString MasterPath = Dest / TEXT("Materials/M_KeiCar");
	const FString CoatPath = Dest / TEXT("Materials/M_KeiCar_ClearCoat");
	const FString GlassPath = Dest / TEXT("Materials/M_KeiCar_Glass");
	TArray<FString> Packages = TexturePaths;
	Packages.Append(InstancePaths);
	Packages.Append({MasterPath, CoatPath, GlassPath});
	for (const FString& Package : Packages)
	{
		const FString File = PackageFile(Package);
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: couldn't replace %s."), *File);
			return 1;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	}

	UTexture2D* Maps[kMapCount] = {};
	for (int32 I = 0; I < kMapCount; ++I)
	{
		Maps[I] = ImportMap(Files[1 + I], TexturePaths[I], kMaps[I]);
		if (!Maps[I])
		{
			return 1;
		}
	}
	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr,
		*(FString(FTScene::WindowCollection) + TEXT(".") + FPackageName::GetShortName(FTScene::WindowCollection)), nullptr,
		LOAD_NoWarn | LOAD_Quiet);
	if (!Collection)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTImportVehicle: no %s (FTBuildSurfaces -Windows); the lamps won't light at night."),
			FTScene::WindowCollection);
	}
	UMaterial* Master = BuildMaster(MasterPath, MSM_DefaultLit, BLEND_Opaque, Maps, Collection);
	UMaterial* Coat = BuildMaster(CoatPath, MSM_ClearCoat, BLEND_Opaque, Maps, Collection);
	UMaterial* Glass = BuildMaster(GlassPath, MSM_DefaultLit, BLEND_Translucent, Maps, Collection);
	if (!Master || !Coat || !Glass)
	{
		return 1;
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	if (FT::CompileErrors(TEXT("FTImportVehicle"), Master) > 0 || FT::CompileErrors(TEXT("FTImportVehicle"), Coat) > 0 ||
		FT::CompileErrors(TEXT("FTImportVehicle"), Glass) > 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: a car master doesn't compile; nothing saved."));
		return 1;
	}
	UMaterialInstanceConstant* Instances[kSlotCount] = {};
	for (int32 I = 0; I < kSlotCount; ++I)
	{
		UMaterial* Parent = I == kPaintSlot ? Coat : (I == kGlassSlot ? Glass : Master);
		Instances[I] = BuildInstance(InstancePaths[I], Parent, I);
	}
	bool bSaved = true;
	for (UTexture2D* Texture : Maps)
	{
		bSaved = SaveAsset(Texture) && bSaved;
	}
	bSaved = SaveAsset(Master) && SaveAsset(Coat) && SaveAsset(Glass) && bSaved;
	for (UMaterialInstanceConstant* Instance : Instances)
	{
		bSaved = SaveAsset(Instance) && bSaved;
	}
	if (!bSaved)
	{
		return 1;
	}

	// The mesh: Interchange reads these FBX options (its generic pipeline converts them). No materials or textures from the
	// file (the slots get the instances above), the file's LODs, no Nanite (Interchange's default is on), the file's UBX_
	// box, the file's normals.
	UFbxImportUI* Ui = NewObject<UFbxImportUI>();
	Ui->bIsObjImport = false;
	Ui->bImportMesh = true;
	Ui->bImportAsSkeletal = false;
	Ui->bAutomatedImportShouldDetectType = false;
	Ui->MeshTypeToImport = FBXIT_StaticMesh;
	Ui->OriginalImportType = FBXIT_StaticMesh;
	Ui->bImportMaterials = false;
	Ui->bImportTextures = false;
	Ui->bImportAnimations = false;
	Ui->bOverrideFullName = true;
	UFbxStaticMeshImportData* MeshData = Ui->StaticMeshImportData;
	MeshData->bImportMeshLODs = true;
	MeshData->bBuildNanite = false;
	MeshData->bAutoGenerateCollision = true;
	MeshData->bCombineMeshes = true;
	MeshData->bGenerateLightmapUVs = false;
	MeshData->bRemoveDegenerates = true;
	MeshData->bConvertScene = true;
	MeshData->bForceFrontXAxis = false;
	MeshData->bConvertSceneUnit = true;
	MeshData->NormalImportMethod = FBXNIM_ImportNormals;
	MeshData->NormalGenerationMethod = EFBXNormalGenerationMethod::MikkTSpace;

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = MeshFile;
	Task->DestinationPath = Dest;
	Task->DestinationName = FTBuildKeiCars::MeshName;
	Task->Options = Ui;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bSave = true;
	Task->bAsync = false;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.ImportAssetTasks({Task});

	UStaticMesh* Mesh = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		UStaticMesh* Found = Cast<UStaticMesh>(Object);
		if (!Found)
		{
			continue;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportVehicle: imported %s"), *Found->GetPathName());
		if (Found->GetName() == FTBuildKeiCars::MeshName)
		{
			Mesh = Found;
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTImportVehicle: %s came out as its own mesh; delete it (the car's box is set below)."),
				*Found->GetPathName());
		}
	}
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: no %s came out of %s."), FTBuildKeiCars::MeshName, *MeshFile);
		return 1;
	}
	FAssetCompilingManager::Get().FinishAllCompilation();

	bool bOk = true;
	if (Mesh->GetNumSourceModels() != kLodCount)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: %s has %d LODs, not %d (is the LOD group exported?)."), *Mesh->GetName(),
			Mesh->GetNumSourceModels(), kLodCount);
		return 1;
	}
	// The slots by name, never by index.
	int32 Matched = 0;
	uint32 Seen = 0;
	for (FStaticMaterial& Slot : Mesh->GetStaticMaterials())
	{
		const int32 I = SlotOf(Slot.ImportedMaterialSlotName);
		if (I == INDEX_NONE || (Seen & (1u << I)) != 0)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: slot %s isn't one of the six car slots (or comes twice)."),
				*Slot.ImportedMaterialSlotName.ToString());
			bOk = false;
			continue;
		}
		Seen |= 1u << I;
		Slot.MaterialInterface = Instances[I];
		++Matched;
	}
	if (Matched != kSlotCount)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: %d of the %d car slots found."), Matched, kSlotCount);
		bOk = false;
	}
	Mesh->GetNaniteSettings().bEnabled = false;
	Mesh->SetAutoComputeLODScreenSize(false);
	for (int32 L = 0; L < kLodCount; ++L)
	{
		Mesh->GetSourceModel(L).ScreenSize = FPerPlatformFloat(kLodScreenSize[L]);
	}
	Mesh->PostEditChange();
	FAssetCompilingManager::Get().FinishAllCompilation();

	// One box: the file's UBX_ box when it came through alone, else the contract's.
	if (!Mesh->GetBodySetup())
	{
		Mesh->CreateBodySetup();
	}
	UBodySetup* Body = Mesh->GetBodySetup();
	const bool bFileBox = Body->AggGeom.GetElementCount() == 1 && Body->AggGeom.BoxElems.Num() == 1;
	if (!bFileBox)
	{
		Body->Modify();
		Body->AggGeom.EmptyElements();
		const FVector Size = kBoxMax - kBoxMin;
		FKBoxElem Box(static_cast<float>(Size.X), static_cast<float>(Size.Y), static_cast<float>(Size.Z));
		Box.Center = (kBoxMin + kBoxMax) * 0.5;
		Body->AggGeom.BoxElems.Add(Box);
		Body->InvalidatePhysicsData();
		Body->CreatePhysicsMeshes();
		Mesh->SetCustomizedCollision(true);
	}
	const FKBoxElem& Box = Body->AggGeom.BoxElems[0];
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportVehicle: collision: one box %.0f x %.0f x %.0f cm at (%.0f, %.0f, %.0f) (%s)."),
		Box.X, Box.Y, Box.Z, Box.Center.X, Box.Center.Y, Box.Center.Z, bFileBox ? TEXT("the file's") : TEXT("the contract's"));

	for (int32 L = 0; L < kLodCount; ++L)
	{
		const int32 Triangles = Mesh->GetNumTriangles(L);
		const bool bNear = FMath::Abs(Triangles - kLodTriangles[L]) <= kLodTriangles[L] / 20;
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportVehicle: LOD%d: %d triangles (export %d), screen size %.2f."), L, Triangles,
			kLodTriangles[L], kLodScreenSize[L]);
		if (!bNear)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTImportVehicle: LOD%d's triangle count is more than 5%% off the export's."), L);
		}
	}
	// The origin is the ground centre and the nose +X (the stand-in's pose): the bounds must sit on z 0 around the origin.
	const FBox Bounds = Mesh->GetBoundingBox();
	const FVector Size = Bounds.GetSize();
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportVehicle: bounds x %.1f..%.1f, y %.1f..%.1f, z %.1f..%.1f cm."), Bounds.Min.X,
		Bounds.Max.X, Bounds.Min.Y, Bounds.Max.Y, Bounds.Min.Z, Bounds.Max.Z);
	if (Size.X < 300.0 || Size.X > 400.0 || Size.Y < 120.0 || Size.Y > 180.0 || FMath::Abs(Bounds.Min.Z) > 3.0 ||
		FMath::Abs(Bounds.GetCenter().X) > 10.0 || FMath::Abs(Bounds.GetCenter().Y) > 10.0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle: the car isn't about 3.4 x 1.5 m with its origin at its ground centre (units or axes?)."));
		bOk = false;
	}
	if (!SaveAsset(Mesh))
	{
		return 1;
	}
	const FString Expected = FTBuildKeiCars::FindMeshPath();
	if (Dest == FTBuildKeiCars::Folder && Expected != Mesh->GetPathName())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTImportVehicle: the mesh is %s but FTBuildMassingWorld -KeiCars looks for %s."),
			*Mesh->GetPathName(), Expected.IsEmpty() ? TEXT("nothing") : *Expected);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportVehicle: %s, %s."), *Mesh->GetPathName(), bOk ? TEXT("ok") : TEXT("failed"));
	return bOk ? 0 : 1;
}
} // namespace FTKeiCarImport
#endif

UFTImportVehicleCommandlet::UFTImportVehicleCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTImportVehicleCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	return FTKeiCarImport::Run(Params);
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTImportVehicle needs an editor build."));
	return 1;
#endif
}
