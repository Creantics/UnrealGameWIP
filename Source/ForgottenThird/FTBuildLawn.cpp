#include "FTBuildLawn.h"

#include "ForgottenThird.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "ImageUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/LawnSheet.h"
#include "ftcore/MassingPlan.h"

#include <string>

namespace FTLawn::Impl
{
constexpr double kStepM = 1.0;        // the sheet's grid and the paths' stations, as the height field's own spacing
constexpr double kLawnLiftM = 0.05;   // the sheet over the ground: clear of the ground's own triangulation
constexpr double kPathLiftM = 0.09;   // a path over the ground (4 cm proud of the lawn)
constexpr double kPathStaggerM = 0.004; // each later path this much higher, so crossing paths never fight
constexpr double kSkirtM = 0.10;      // a path's skirt reaches this far under the ground
constexpr double kEdgeM = 0.10;       // the edging strip's width beside a path
constexpr double kEdgeLiftM = 0.125;  // the edging over the ground: a centimetre or two proud of the paving
constexpr int32 kMaxShaderPaths = 6;  // paths the lawn master takes (P0..P5)
constexpr float kPathWeather = 0.6f;  // the green's paths' share of the weather (puddles only in the noise's low spots)
const FName kSlot(TEXT("Lawn"));

struct FMap
{
	const TCHAR* File;
	const TCHAR* Suffix;
	bool bSRGB;
	TextureCompressionSettings Compression;
};
constexpr FMap kMaps[] = {
	{TEXT("lawn_D.jpg"), TEXT("D"), true, TC_Default},
	{TEXT("lawn_N.jpg"), TEXT("N"), false, TC_Normalmap},
	{TEXT("lawn_ARM.jpg"), TEXT("ARM"), false, TC_Masks},
};

UTexture2D* ImportMap(const FString& Dir, const FMap& Map)
{
	const FString Path = FString::Printf(TEXT("%s/T_LawnF_%s"), TextureFolder, Map.Suffix);
	if (UTexture2D* Have = LoadObject<UTexture2D>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		return Have;
	}
	const FString File = Dir / Map.File;
	TArray<uint8> Bytes;
	FImage Image;
	if (!FFileHelper::LoadFileToArray(Bytes, *File) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image) ||
		(Image.Format != ERawImageFormat::BGRA8 && Image.Format != ERawImageFormat::G8))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -LawnTex: couldn't read %s as an 8-bit JPEG."), *File);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*Path);
	UTexture2D* Texture = NewObject<UTexture2D>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone);
	Texture->PreEditChange(nullptr);
	Texture->Source.InitWithCompressedSourceData(Image.SizeX, Image.SizeY, 1, Image.Format == ERawImageFormat::G8 ? TSF_G8 : TSF_BGRA8,
		TArrayView64<uint8>(Bytes.GetData(), Bytes.Num()), ETextureSourceCompressionFormat::TSCF_JPEG);
	Texture->SRGB = Map.bSRGB;
	Texture->CompressionSettings = Map.Compression;
	Texture->LODGroup = TEXTUREGROUP_World;
	// A world projection doesn't use the mesh's UVs, which the streamer judges a texture's need by: load it whole.
	Texture->NeverStream = true;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Out = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Texture, *Out, Save))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -LawnTex: couldn't save %s."), *Out);
		return nullptr;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -LawnTex: imported %s (%d x %d)."), *Path, Image.SizeX, Image.SizeY);
	return Texture;
}

UStaticMesh* SheetMesh(UPackage* Package, const FString& Name, const ftcore::LawnMesh& Source, double Side, const FVector& Pivot,
	UMaterialInterface* Material)
{
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, *Name, RF_Public);
	FMeshDescription Desc;
	FStaticMeshAttributes Attributes(Desc);
	Attributes.Register();
	const FPolygonGroupID Group = Desc.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = kSlot;
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector2f> Uvs = Attributes.GetVertexInstanceUVs();
	TArray<FVertexID> Verts;
	TArray<FVector> Points;
	Verts.Reserve(static_cast<int32>(Source.v.size()));
	Points.Reserve(static_cast<int32>(Source.v.size()));
	for (const ftcore::MassingVec& V : Source.v)
	{
		const ftcore::MassingVec E = ftcore::massingEnginePoint(Side, V.x, V.y, V.z);
		Points.Add(FVector(E.x, E.y, E.z) * 100.0);
		const FVertexID Id = Desc.CreateVertex();
		Positions[Id] = FVector3f(Points.Last() - Pivot);
		Verts.Add(Id);
	}
	// The plan frame's counter-clockwise turns clockwise in the engine's (its y runs the other way), the winding the
	// engine draws a face up by; check it on the first upward triangle all the same and turn every one if it's wrong.
	bool bFlip = false;
	for (const auto& T : Source.tris)
	{
		const FVector N = FVector::CrossProduct(Points[static_cast<int32>(T[1])] - Points[static_cast<int32>(T[0])], Points[static_cast<int32>(T[2])] - Points[static_cast<int32>(T[0])]);
		if (FMath::Abs(N.Z) > 0.5 * N.Size())
		{
			bFlip = N.Z > 0.0; // the engine wants an up face's first cross product pointing down
			break;
		}
	}
	for (const auto& T : Source.tris)
	{
		FVertexInstanceID Corners[3];
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 Index = static_cast<int32>(T[static_cast<size_t>(bFlip ? 2 - K : K)]);
			Corners[K] = Desc.CreateVertexInstance(Verts[Index]);
			Uvs[Corners[K]] = FVector2f(FVector2D(Points[Index].X, Points[Index].Y) / 100.0);
		}
		Desc.CreatePolygon(Group, TArrayView<const FVertexInstanceID>(Corners, 3));
	}
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, kSlot, kSlot));
	FStaticMeshSourceModel& Model = Mesh->AddSourceModel();
	Model.BuildSettings.bRecomputeNormals = true;
	Model.BuildSettings.bRecomputeTangents = true;
	Model.BuildSettings.bGenerateLightmapUVs = false;
	Mesh->bGenerateMeshDistanceField = true; // else 0 Lumen cards, as the massing meshes
	Mesh->CreateMeshDescription(0, MoveTemp(Desc));
	Mesh->CommitMeshDescription(0);
	Mesh->CreateBodySetup();
	Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
	Mesh->Build(true);
	Mesh->PostEditChange();
	return Mesh;
}

void SpawnSheet(UWorld* World, const FActorSpawnParameters& Params, UStaticMesh* Mesh, const FVector& Pivot, const FString& Label)
{
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Pivot, FRotator::ZeroRotator, Params);
	Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Actor->SetActorLabel(Label);
	Actor->Tags.Add(TEXT("FTLawn"));
}

FVector PivotOf(const ftcore::LawnMesh& M, double Side)
{
	FBox Box(ForceInit);
	for (const ftcore::MassingVec& V : M.v)
	{
		const ftcore::MassingVec E = ftcore::massingEnginePoint(Side, V.x, V.y, V.z);
		Box += FVector(E.x, E.y, E.z) * 100.0;
	}
	return FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z);
}

// The scanned lawn's own master. The scan's maps are flattened before import (their low-frequency blotches divided out:
// those, repeating every TileCm, were the grid read from above), so the large-scale colour comes from here: two samples
// of the scan (one at TileCm turned 0.21 rad, one 2.7 times larger turned 0.81 rad, so no repeat lines up with the
// paths) blended by a low noise, a brightness mottle and a macro shift between a drier and a lusher green, each noise
// three rotated octaves at non-integer ratios. Far off (FarFrom..FarTo cm of depth) the scan fades towards its own mean
// colour, keeping FarDetail of its detail, so nothing tiles at any distance. With the paths given (P0..P5, engine cm,
// half widths in W0/W1), a worn, thinner band along each path's edge with a ragged line and a little soil, and rougher
// unmown patches away from the paths. World-planar; the normal is world space (the sheet is near level).
const TCHAR* const kLawnCode = TEXT(R"(
float2 w = WorldPos.xy;
float nz[4];
float sc[4] = {3100.0, VariationScale, MacroScale, 47.0};
for (int k = 0; k < 4; ++k)
{
	float acc = 0.0, amp = 0.5, tot = 0.0;
	float2 g = w / sc[k];
	for (int o = 0; o < 3; ++o)
	{
		float an = 0.5 + 1.13 * o + 0.71 * k;
		float2 r = float2(cos(an) * g.x - sin(an) * g.y, sin(an) * g.x + cos(an) * g.y) + float2(k * 17.3 + o * 3.7, k * 5.1 + o * 9.2);
		float2 i = floor(r);
		float2 f = frac(r);
		f = f * f * (3.0 - 2.0 * f);
		float h00 = frac(sin(dot(i, float2(127.1, 311.7))) * 43758.5453);
		float h10 = frac(sin(dot(i + float2(1.0, 0.0), float2(127.1, 311.7))) * 43758.5453);
		float h01 = frac(sin(dot(i + float2(0.0, 1.0), float2(127.1, 311.7))) * 43758.5453);
		float h11 = frac(sin(dot(i + float2(1.0, 1.0), float2(127.1, 311.7))) * 43758.5453);
		acc += amp * lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
		tot += amp;
		amp *= 0.5;
		g *= 2.13;
	}
	nz[k] = saturate((acc / tot - 0.5) * 1.7 + 0.5);
}
float ca = cos(0.21), sa = sin(0.21);
float cb = cos(0.81), sb = sin(0.81);
float2 uvA = float2(ca * w.x - sa * w.y, sa * w.x + ca * w.y) / TileCm;
float2 uvB = float2(cb * w.x - sb * w.y, sb * w.x + cb * w.y) / (TileCm * 2.7) + float2(0.37, 0.61);
float t = smoothstep(0.3, 0.7, nz[0]);
float3 a = Texture2DSample(Albedo, AlbedoSampler, uvA).rgb;
float3 b = Texture2DSample(Albedo, AlbedoSampler, uvB).rgb;
float3 ra = Texture2DSample(Arm, ArmSampler, uvA).rgb;
float3 rb = Texture2DSample(Arm, ArmSampler, uvB).rgb;
float3 na = UnpackNormalMap(Texture2DSample(Nrm, NrmSampler, uvA)).xyz;
float3 nb = UnpackNormalMap(Texture2DSample(Nrm, NrmSampler, uvB)).xyz;
na.xy = float2(ca * na.x + sa * na.y, -sa * na.x + ca * na.y);
nb.xy = float2(cb * nb.x + sb * nb.y, -sb * nb.x + cb * nb.y);
float3 arm = lerp(ra, rb, t);
float far = smoothstep(FarFrom, FarTo, Depth);
float3 mean = Texture2DSampleLevel(Albedo, AlbedoSampler, float2(0.5, 0.5), 12.0).rgb;
float3 col = lerp(a, b, t);
col = lerp(col, lerp(mean, col, FarDetail), far) * Color.rgb;
float2 nxy = lerp(na.xy, nb.xy, t) * NormalStrength * (1.0 - 0.6 * far);
col *= 1.0 + Variation * (nz[1] - 0.5) * 2.0;
col *= 1.0 + MacroVariation * (nz[2] - 0.5);
col *= lerp(float3(1.07, 1.0, 0.82), float3(0.92, 1.03, 1.02), nz[2]);
float rough = max(saturate(arm.g), RoughFloor);
float ao = arm.r;
float4 P[6] = {P0, P1, P2, P3, P4, P5};
float Hw[6] = {W0.x, W0.y, W0.z, W0.w, W1.x, W1.y};
float dp = 1e9;
for (int s = 0; s < 6; ++s)
{
	if (Hw[s] > 0.0)
	{
		float2 pa = P[s].xy;
		float2 ab = P[s].zw - pa;
		float u = saturate(dot(w - pa, ab) / max(dot(ab, ab), 1.0));
		dp = min(dp, length(w - pa - ab * u) - Hw[s]);
	}
}
if (dp < 1e8)
{
	// Worn: a band WornCm wide (ragged by the fine noise) of thinner, yellower grass with soil showing through.
	float worn = 1.0 - smoothstep(0.0, WornCm * (0.55 + 0.9 * nz[3]), max(dp, 0.0));
	col *= lerp(float3(1.0, 1.0, 1.0), float3(1.06, 0.93, 0.74), worn);
	col = lerp(col, Soil.rgb, saturate(worn * worn * (0.25 + 0.5 * nz[3])));
	rough = lerp(rough, max(rough, 0.86), worn);
	nxy *= 1.0 - 0.5 * worn;
	// Unmown: patches more than 3 to 6 m from any path, darker and deeper with more relief.
	float unm = smoothstep(300.0 + 300.0 * nz[2], 650.0 + 300.0 * nz[2], dp) * smoothstep(0.42, 0.62, nz[1]);
	col *= lerp(float3(1.0, 1.0, 1.0), float3(0.84, 0.93, 0.78), unm * Unmown);
	ao *= 1.0 - 0.22 * unm * Unmown;
	nxy *= 1.0 + 0.6 * unm * Unmown;
}
RoughOut = rough;
AoOut = ao;
NormalOut = normalize(normalize(VNormal) + float3(nxy.x, -nxy.y, 0.0));
return saturate(col);
)");
UMaterialInterface* LawnMaster(UPackage* Package, UTexture* const Maps[3])
{
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_Massing_Lawn"), RF_Public);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Material->bTangentSpaceNormal = false;
	int32 Y = 0;
	auto Add = [Material, &Y](UMaterialExpression* E)
	{
		Material->GetExpressionCollection().AddExpression(E);
		E->Material = Material;
		E->MaterialExpressionEditorX = -700;
		E->MaterialExpressionEditorY = (Y += 100);
		E->UpdateMaterialExpressionGuid(true, false);
		return E;
	};
	UMaterialExpressionCustom* Code = NewObject<UMaterialExpressionCustom>(Material);
	Add(Code);
	Code->Description = TEXT("FTLawn");
	Code->OutputType = CMOT_Float3;
	Code->Code = kLawnCode;
	Code->Inputs.Reset();
	auto In = [Code](FName Name, UMaterialExpression* From)
	{
		FCustomInput& Input = Code->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(0, From);
	};
	static const TCHAR* const TexNames[3] = {TEXT("Albedo"), TEXT("Nrm"), TEXT("Arm")};
	static const EMaterialSamplerType Types[3] = {SAMPLERTYPE_Color, SAMPLERTYPE_Normal, SAMPLERTYPE_Masks};
	for (int32 K = 0; K < 3; ++K)
	{
		UMaterialExpressionTextureObjectParameter* P = NewObject<UMaterialExpressionTextureObjectParameter>(Material);
		Add(P);
		P->ParameterName = TexNames[K];
		P->Texture = Maps[K];
		P->SamplerType = Types[K];
		P->UpdateParameterGuid(true, false);
		In(TexNames[K], P);
	}
	In(TEXT("WorldPos"), Add(NewObject<UMaterialExpressionWorldPosition>(Material)));
	In(TEXT("VNormal"), Add(NewObject<UMaterialExpressionVertexNormalWS>(Material)));
	UMaterialExpressionVectorParameter* Color = NewObject<UMaterialExpressionVectorParameter>(Material);
	Add(Color);
	Color->ParameterName = TEXT("Color");
	Color->DefaultValue = FLinearColor(0.74f, 1.45f, 0.78f);
	Color->UpdateParameterGuid(true, false);
	In(TEXT("Color"), Color);
	In(TEXT("Depth"), Add(NewObject<UMaterialExpressionPixelDepth>(Material)));
	// The paths' segments (engine cm) and half widths; all zero (no paths) leaves no worn band and no unmown patches.
	const TPair<const TCHAR*, FLinearColor> Vectors[] = {{TEXT("P0"), FLinearColor::Transparent}, {TEXT("P1"), FLinearColor::Transparent},
		{TEXT("P2"), FLinearColor::Transparent}, {TEXT("P3"), FLinearColor::Transparent}, {TEXT("P4"), FLinearColor::Transparent},
		{TEXT("P5"), FLinearColor::Transparent}, {TEXT("W0"), FLinearColor::Transparent}, {TEXT("W1"), FLinearColor::Transparent},
		{TEXT("Soil"), FLinearColor(0.16f, 0.115f, 0.075f)}};
	for (const TPair<const TCHAR*, FLinearColor>& V : Vectors)
	{
		UMaterialExpressionVectorParameter* P = NewObject<UMaterialExpressionVectorParameter>(Material);
		Add(P);
		P->ParameterName = V.Key;
		P->DefaultValue = V.Value;
		P->UpdateParameterGuid(true, false);
		FCustomInput& Input = Code->Inputs.AddDefaulted_GetRef();
		Input.InputName = V.Key;
		Input.Input.Connect(5, P); // RGBA
	}
	const TPair<const TCHAR*, float> Scalars[] = {{TEXT("TileCm"), 200.f}, {TEXT("NormalStrength"), 1.f}, {TEXT("Variation"), 0.14f},
		{TEXT("VariationScale"), 420.f}, {TEXT("MacroVariation"), 0.22f}, {TEXT("MacroScale"), 1700.f}, {TEXT("FarFrom"), 2500.f},
		{TEXT("FarTo"), 9000.f}, {TEXT("FarDetail"), 0.4f}, {TEXT("RoughFloor"), 0.72f}, {TEXT("WornCm"), 45.f}, {TEXT("Unmown"), 1.f}};
	for (const TPair<const TCHAR*, float>& S : Scalars)
	{
		UMaterialExpressionScalarParameter* P = NewObject<UMaterialExpressionScalarParameter>(Material);
		Add(P);
		P->ParameterName = S.Key;
		P->DefaultValue = S.Value;
		P->UpdateParameterGuid(true, false);
		In(S.Key, P);
	}
	static const TCHAR* const Outs[3] = {TEXT("RoughOut"), TEXT("AoOut"), TEXT("NormalOut")};
	static const ECustomMaterialOutputType OutTypes[3] = {CMOT_Float1, CMOT_Float1, CMOT_Float3};
	for (int32 K = 0; K < 3; ++K)
	{
		FCustomOutput& Output = Code->AdditionalOutputs.AddDefaulted_GetRef();
		Output.OutputName = Outs[K];
		Output.OutputType = OutTypes[K];
	}
	Code->RebuildOutputs();
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Code);
	Inputs->Roughness.Connect(1, Code);
	Inputs->AmbientOcclusion.Connect(2, Code);
	Inputs->Normal.Connect(3, Code);
	// Grass is a poor mirror: a low specular keeps the shade side green rather than the sky's grey-blue.
	UMaterialExpressionScalarParameter* Spec = NewObject<UMaterialExpressionScalarParameter>(Material);
	Add(Spec);
	Spec->ParameterName = TEXT("Specular");
	Spec->DefaultValue = 0.3f;
	Spec->UpdateParameterGuid(true, false);
	Inputs->Specular.Connect(0, Spec);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

// The green's own lawn: the lawn surface with its paths given (the master's worn band and unmown patches), up to six.
UMaterialInterface* GreenMaterial(UPackage* Package, UMaterialInterface* Lawn, const ftcore::LawnTable& Table, double Side)
{
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_Massing_LawnGreen"), RF_Public);
	Mic->SetParentEditorOnly(Lawn);
	float Half[8] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
	const int32 Count = FMath::Min(static_cast<int32>(Table.paths.size()), kMaxShaderPaths);
	for (int32 P = 0; P < Count; ++P)
	{
		const ftcore::LawnPath& Path = Table.paths[static_cast<size_t>(P)];
		const ftcore::MassingVec A = ftcore::massingEnginePoint(Side, Path.x0, Path.y0, 0.0);
		const ftcore::MassingVec B = ftcore::massingEnginePoint(Side, Path.x1, Path.y1, 0.0);
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(*FString::Printf(TEXT("P%d"), P)),
			FLinearColor(static_cast<float>(A.x * 100.0), static_cast<float>(A.y * 100.0), static_cast<float>(B.x * 100.0), static_cast<float>(B.y * 100.0)));
		Half[P] = static_cast<float>(Path.width * 50.0);
	}
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("W0")), FLinearColor(Half[0], Half[1], Half[2], Half[3]));
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("W1")), FLinearColor(Half[4], Half[5], Half[6], Half[7]));
	if (static_cast<int32>(Table.paths.size()) > kMaxShaderPaths)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: -Lawn: %d paths; the lawn wears the first %d's worn edges only."),
			static_cast<int32>(Table.paths.size()), kMaxShaderPaths);
	}
	Mic->PostEditChange();
	return Mic;
}

// The green's paths: the paving with less of the weather, so water stands only in the low spots of the noise.
UMaterialInterface* PathMaterial(UPackage* Package, UMaterialInterface* Path)
{
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_Massing_LawnPath"), RF_Public);
	Mic->SetParentEditorOnly(Path);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Weather")), kPathWeather);
	Mic->PostEditChange();
	return Mic;
}
} // namespace FTLawn::Impl

UMaterialInterface* FTLawn::LawnMaterial(UPackage* Package, UMaterialInterface* Parent)
{
	using namespace FTLawn::Impl;
	FString Dir;
	if (FParse::Value(FCommandLine::Get(), TEXT("LawnTex="), Dir, false) && !Dir.IsEmpty() && !FParse::Param(FCommandLine::Get(), TEXT("LawnOneScale")))
	{
		// The scan on its own master: two scales blended by noise, a turned second sample and a macro colour shift.
		UTexture* Maps[3] = {nullptr, nullptr, nullptr};
		for (int32 M = 0; M < UE_ARRAY_COUNT(kMaps); ++M)
		{
			Maps[M] = ImportMap(Dir, kMaps[M]);
			if (!Maps[M])
			{
				return nullptr;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: the lawn wears the scanned set from -LawnTex at two blended scales (%s)."), TextureFolder);
		return LawnMaster(Package, Maps);
	}
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_Massing_Lawn"), RF_Public);
	Mic->SetParentEditorOnly(Parent);
	if (FParse::Value(FCommandLine::Get(), TEXT("LawnTex="), Dir, false) && !Dir.IsEmpty())
	{
		static const TCHAR* const Params[] = {TEXT("AlbedoTex"), TEXT("NormalTex"), TEXT("ArmTex")};
		for (int32 M = 0; M < UE_ARRAY_COUNT(kMaps); ++M)
		{
			UTexture2D* Texture = ImportMap(Dir, kMaps[M]);
			if (!Texture)
			{
				return nullptr;
			}
			Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Params[M]), Texture);
		}
		// The scan is 2 m a side and an olive green: pulled to a mid lawn green, with a broad mottle in brightness so a
		// green of 80 m reads as patches, not one tile repeated.
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TileCm")), 200.f);
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), FLinearColor(0.74f, 1.45f, 0.78f));
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("NormalStrength")), 1.f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Variation")), 0.14f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("VariationScale")), 420.f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroVariation")), 0.22f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroScale")), 1700.f);
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: the lawn wears the scanned set from -LawnTex (%s)."), TextureFolder);
	}
	else
	{
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroVariation")), 0.32f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroScale")), 1400.f);
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), FLinearColor(0.8f, 1.2f, 0.7f));
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: the lawn wears the project's grass surface (no -LawnTex)."));
	}
	// Grass soaks rain up: no standing puddles or wet sheen on the lawn (the surface master's per-instance Weather).
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Weather")), 0.f);
	Mic->PostEditChange();
	return Mic;
}

bool FTLawn::Build(UWorld* World, const FActorSpawnParameters& Spawn, UPackage* Package, const ftcore::HeightField& Ground, double Side,
	UMaterialInterface* Lawn, UMaterialInterface* Path, UMaterialInterface* Edge)
{
	using namespace FTLawn::Impl;
	FString File;
	if (!FParse::Value(FCommandLine::Get(), TEXT("Lawn="), File, false) || File.IsEmpty())
	{
		return true;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -Lawn couldn't read '%s'."), *File);
		return false;
	}
	ftcore::LawnTable Table;
	std::string Error;
	if (!ftcore::parseLawnTable(std::string_view(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num())), Table, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -Lawn %s: %s"), *File, UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	if (!Lawn || !Path || !Ground.grid)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -Lawn needs -Surfaces (the lawn and paving surfaces) and the height field."));
		return false;
	}
	int64 Triangles = 0;
	UMaterialInterface* Green = GreenMaterial(Package, Lawn, Table, Side);
	for (int32 R = 0; R < static_cast<int32>(Table.regions.size()); ++R)
	{
		ftcore::LawnMesh M;
		ftcore::lawnSheetMesh(Ground, Table.regions[static_cast<size_t>(R)], kStepM, kLawnLiftM, M);
		const FVector Pivot = PivotOf(M, Side);
		SpawnSheet(World, Spawn, SheetMesh(Package, FString::Printf(TEXT("SM_Lawn_%d"), R), M, Side, Pivot, Green), Pivot, FString::Printf(TEXT("Lawn_%d"), R));
		Triangles += static_cast<int64>(M.tris.size());
	}
	if (!Table.paths.empty())
	{
		ftcore::LawnMesh M;
		for (size_t P = 0; P < Table.paths.size(); ++P)
		{
			ftcore::lawnPathMesh(Ground, Table.paths[P], kStepM, kPathLiftM + kPathStaggerM * static_cast<double>(P), kSkirtM, M);
		}
		const FVector Pivot = PivotOf(M, Side);
		SpawnSheet(World, Spawn, SheetMesh(Package, TEXT("SM_LawnPaths"), M, Side, Pivot, PathMaterial(Package, Path)), Pivot, TEXT("LawnPaths"));
		Triangles += static_cast<int64>(M.tris.size());
		// The edging: a narrow strip of the edge surface on each side of every path, a little proud of the paving.
		std::vector<ftcore::LawnPath> Edges;
		ftcore::lawnPathEdges(Table, kEdgeM, Edges);
		if (Edge && !Edges.empty())
		{
			ftcore::LawnMesh E;
			for (const ftcore::LawnPath& Strip : Edges)
			{
				ftcore::lawnPathMesh(Ground, Strip, kStepM, kEdgeLiftM, kSkirtM, E);
			}
			const FVector EdgePivot = PivotOf(E, Side);
			SpawnSheet(World, Spawn, SheetMesh(Package, TEXT("SM_LawnEdging"), E, Side, EdgePivot, Edge), EdgePivot, TEXT("LawnEdging"));
			Triangles += static_cast<int64>(E.tris.size());
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -Lawn: %d edging strips %.0f cm wide."), static_cast<int32>(Edges.size()), kEdgeM * 100.0);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -Lawn: %d lawn sheets and %d paths, %lld triangles, from %s."),
		static_cast<int32>(Table.regions.size()), static_cast<int32>(Table.paths.size()), Triangles, *File);
	return true;
}
