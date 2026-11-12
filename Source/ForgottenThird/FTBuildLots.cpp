#include "FTBuildLots.h"

#include "ForgottenThird.h"
#include "FTCellInstancer.h"
#include "FTSceneLook.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include <string>

namespace FTLots
{
namespace
{
FString Utf8(const std::string& S)
{
	return FString(UTF8_TO_TCHAR(S.c_str()));
}

bool ReadText(const FString& File, std::string& Out)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: couldn't read %s."), *File);
		return false;
	}
	Out = TCHAR_TO_UTF8(*Text);
	return true;
}

// The kit master's slot codes (MI scalar "Slot"): 0..4 read a palette tint from custom data 0..4, 5 a sign, 6 a
// vending face, 7 glass, 8 the dark interior, 9 a fixed tint (the MI's Tint).
struct FSlotLook
{
	const TCHAR* Name;
	float Slot;
	FLinearColor Tint;
	float Rough;
};
const FSlotLook kSlots[] = {
	{TEXT("jpk_wall_tile"), 0.f, FLinearColor(0.86f, 0.84f, 0.79f), 0.6f},
	{TEXT("jpk_wall_concrete"), 0.f, FLinearColor(0.86f, 0.84f, 0.79f), 0.8f},
	{TEXT("jpk_wall_stucco"), 0.f, FLinearColor(0.90f, 0.86f, 0.74f), 0.85f},
	{TEXT("jpk_trim"), 1.f, FLinearColor(0.86f, 0.85f, 0.82f), 0.6f},
	{TEXT("jpk_accent"), 2.f, FLinearColor(0.72f, 0.12f, 0.10f), 0.7f},
	{TEXT("jpk_timber"), 3.f, FLinearColor(0.42f, 0.30f, 0.22f), 0.7f},
	{TEXT("jpk_rooftile"), 4.f, FLinearColor(0.36f, 0.38f, 0.44f), 0.35f},
	{TEXT("jpk_sign"), 5.f, FLinearColor::White, 0.45f},
	{TEXT("jpk_sign_vend"), 6.f, FLinearColor::White, 0.3f},
	{TEXT("jpk_glass"), 7.f, FLinearColor(0.02f, 0.025f, 0.03f), 0.05f},
	{TEXT("jpk_dark"), 8.f, FLinearColor(0.05f, 0.05f, 0.05f), 0.8f},
	{TEXT("jpk_metal"), 9.f, FLinearColor(0.78f, 0.78f, 0.76f), 0.45f},
	{TEXT("jpk_shutter"), 9.f, FLinearColor(0.75f, 0.76f, 0.76f), 0.5f},
	{TEXT("jpk_cloth"), 9.f, FLinearColor(0.20f, 0.24f, 0.36f), 0.9f},
	{TEXT("jpk_plant"), 9.f, FLinearColor::White, 0.7f},
};

const TCHAR* const kKitCode = TEXT(R"(
float3 n = normalize(VNormal);
float2 uv = UV;
float3 h = normalize(cross(float3(0.0, 0.0, 1.0), n) + float3(1e-4, 0.0, 0.0));
if (WorldUV > 0.5)
{
	float3 p = WorldPos / (TileM * 100.0);
	uv = abs(n.z) > 0.5 ? p.xy : float2(dot(p, h), -p.z);
}
float3 bc = Texture2DSample(BC, BCSampler, uv).rgb;
float3 mr = Texture2DSample(MR, MRSampler, uv).rgb;
float3 tn = WorldUV > 0.5 ? float3(0.0, 0.0, 1.0) : UnpackNormalMap(Texture2DSample(NM, NMSampler, uv)).xyz;
tn = normalize(float3(tn.xy * Relief, max(tn.z, 0.05)));
int s = (int)round(Slot);
float v = s == 0 ? CD0 : s == 1 ? CD1 : s == 2 ? CD2 : s == 3 ? CD3 : s == 4 ? CD4 : 0.0;
float3 tint = Tint.rgb;
if (s <= 4 && v > 0.5)
	tint = float3(floor(v / 65536.0), floor(fmod(v, 65536.0) / 256.0), fmod(v, 256.0)) / 255.0;
// The palette and the slots' tints are display (sRGB) colours: decoded to linear here, so a charcoal stays charcoal.
tint = pow(max(tint, 0.0), 2.2);
// A palette slot's map carries only its pattern: divided by its own mean (its smallest mip), so the tint alone sets
// the wall's colour and a pale map doesn't wash it out.
if (s <= 4)
	bc = bc / max(Texture2DSampleLevel(BC, BCSampler, float2(0.5, 0.5), 14.0).rgb, 0.04);
float dirt = HasMR > 0.5 ? mr.r : 0.0;
float grime = saturate(dirt * (0.5 + CD7) * DirtAmount);
float3 base = bc * tint * lerp(float3(1.0, 1.0, 1.0), float3(0.50, 0.46, 0.40), grime);
float rough = HasMR > 0.5 ? lerp(mr.g, 0.95, grime * 0.5) : Rough0;
float metal = HasMR > 0.5 ? mr.b : 0.0;
float3 glow = float3(0.0, 0.0, 0.0);
float night = Night * NightGlow;
if (s == 5)
{
	float m = saturate(dot(bc, float3(0.333, 0.333, 0.334)));
	float3 fg = CD5 > 0.5 ? float3(floor(CD5 / 65536.0), floor(fmod(CD5, 65536.0) / 256.0), fmod(CD5, 256.0)) / 255.0 : float3(0.95, 0.95, 0.92);
	float3 bg = CD2 > 0.5 ? float3(floor(CD2 / 65536.0), floor(fmod(CD2, 65536.0) / 256.0), fmod(CD2, 256.0)) / 255.0 : float3(0.72, 0.12, 0.1);
	fg = pow(fg, 2.2);
	bg = pow(bg, 2.2);
	base = lerp(bg, fg, m);
	rough = 0.45;
	metal = 0.0;
	glow = lerp(bg * 0.5, fg, m) * night * SignGlow;
}
else if (s == 6)
{
	glow = bc * night * SignGlow * 0.5;
}
else if (s == 7)
{
	float2 cell = floor(float2(dot(WorldPos, h) / 180.0, WorldPos.z / 300.0));
	float r = frac(sin(dot(cell, float2(12.9898, 78.233)) + CD6 * 437.1) * 43758.5453);
	bool shop = CD8 > 0.5;
	bool lit = r < (shop ? 0.85 : LitShare);
	float3 warm = r < 0.12 ? float3(0.75, 0.85, 1.0) : float3(1.0, 0.78, 0.52);
	base = Tint.rgb + warm * 0.02;
	rough = 0.05;
	metal = 0.0;
	tn = float3(0.0, 0.0, 1.0);
	glow = lit ? warm * night * GlassGlow * (shop ? 2.0 : 1.0) : float3(0.0, 0.0, 0.0);
}
else if (s == 8)
{
	base = bc * Tint.rgb;
	rough = 0.8;
}
glow = glow / (1.0 + dot(glow, float3(0.2126, 0.7152, 0.0722)) / max(GlowCap, 1e-4));
RoughOut = saturate(rough);
MetalOut = saturate(metal);
NormalOut = tn;
GlowOut = glow;
return base;
)");

template <typename T>
T* AddNode(UMaterial* Material, int32 Y)
{
	T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = -900;
	Expression->MaterialExpressionEditorY = Y;
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

void In(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From)
{
	FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
	Input.InputName = Name;
	Input.Input.Connect(0, From);
}

void Out(UMaterialExpressionCustom* Custom, FName Name, ECustomMaterialOutputType Type)
{
	FCustomOutput& Output = Custom->AdditionalOutputs.AddDefaulted_GetRef();
	Output.OutputName = Name;
	Output.OutputType = Type;
}

UMaterialExpressionScalarParameter* Scalar(UMaterial* Material, FName Name, float Value, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddNode<UMaterialExpressionScalarParameter>(Material, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpression* Night(UMaterial* Material, UMaterialParameterCollection* Collection, const TCHAR* Name, float Fallback, int32 Y)
{
	if (Collection && Collection->GetParameterId(Name).IsValid())
	{
		UMaterialExpressionCollectionParameter* P = AddNode<UMaterialExpressionCollectionParameter>(Material, Y);
		P->Collection = Collection;
		P->ParameterName = Name;
		P->ParameterId = Collection->GetParameterId(Name);
		return P;
	}
	return Scalar(Material, Name, Fallback, Y);
}

UMaterial* BuildMaster(UPackage* Package, UTexture* White, UTexture* Masks, UTexture* Flat)
{
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_KitBuilding"), RF_Public);
	// Opaque, and drawn on Nanite pieces: a slot without the Nanite usage sends the whole piece to its fallback.
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Material->SetUsageByFlag(MATUSAGE_Nanite, true);
	Material->bTangentSpaceNormal = true;
	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr,
		*(FString(FTScene::WindowCollection) + TEXT(".") + FPackageName::GetShortName(FTScene::WindowCollection)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	auto Texture = [Material](FName Name, UTexture* Default, EMaterialSamplerType Type, int32 Y)
	{
		UMaterialExpressionTextureObjectParameter* P = AddNode<UMaterialExpressionTextureObjectParameter>(Material, Y);
		P->ParameterName = Name;
		P->Texture = Default;
		P->SamplerType = Type;
		P->UpdateParameterGuid(true, false);
		return P;
	};
	UMaterialExpressionCustom* Code = AddNode<UMaterialExpressionCustom>(Material, 0);
	Code->MaterialExpressionEditorX = -400;
	Code->Description = TEXT("FTKitBuilding");
	Code->OutputType = CMOT_Float3;
	Code->Code = kKitCode;
	Code->Inputs.Reset();
	In(Code, TEXT("UV"), AddNode<UMaterialExpressionTextureCoordinate>(Material, -200));
	In(Code, TEXT("BC"), Texture(TEXT("BaseColorTexture"), White, SAMPLERTYPE_Color, 0));
	In(Code, TEXT("MR"), Texture(TEXT("MetallicRoughnessTexture"), Masks, SAMPLERTYPE_Masks, 100));
	In(Code, TEXT("NM"), Texture(TEXT("NormalTexture"), Flat, SAMPLERTYPE_Normal, 200));
	In(Code, TEXT("WorldPos"), AddNode<UMaterialExpressionWorldPosition>(Material, 300));
	In(Code, TEXT("VNormal"), AddNode<UMaterialExpressionVertexNormalWS>(Material, 400));
	UMaterialExpressionVectorParameter* Tint = AddNode<UMaterialExpressionVectorParameter>(Material, 500);
	Tint->ParameterName = TEXT("Tint");
	Tint->DefaultValue = FLinearColor::White;
	Tint->UpdateParameterGuid(true, false);
	In(Code, TEXT("Tint"), Tint);
	In(Code, TEXT("Slot"), Scalar(Material, TEXT("Slot"), 9.f, 600));
	In(Code, TEXT("HasMR"), Scalar(Material, TEXT("HasMR"), 0.f, 700));
	In(Code, TEXT("Rough0"), Scalar(Material, TEXT("Roughness"), 0.6f, 800));
	In(Code, TEXT("WorldUV"), Scalar(Material, TEXT("WorldUV"), 0.f, 900));
	In(Code, TEXT("TileM"), Scalar(Material, TEXT("TileM"), 2.f, 1000));
	In(Code, TEXT("SignGlow"), Scalar(Material, TEXT("SignGlow"), 2.f, 1100));
	In(Code, TEXT("GlassGlow"), Scalar(Material, TEXT("GlassGlow"), 0.8f, 1200));
	In(Code, TEXT("LitShare"), Scalar(Material, TEXT("LitShare"), 0.35f, 1300));
	In(Code, TEXT("GlowCap"), Scalar(Material, TEXT("GlowCap"), 6.f, 1400));
	In(Code, TEXT("Relief"), Scalar(Material, TEXT("Relief"), 1.f, 1450));
	In(Code, TEXT("DirtAmount"), Scalar(Material, TEXT("DirtAmount"), 1.f, 1475));
	In(Code, TEXT("Night"), Night(Material, Collection, TEXT("Night"), 0.f, 1500));
	In(Code, TEXT("NightGlow"), Night(Material, Collection, TEXT("NightGlow"), 1.f, 1600));
	UClass* const DataClass = FindObject<UClass>(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("MaterialExpressionPerInstanceCustomData")));
	if (!DataClass)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: no per-instance custom data node class."));
		return nullptr;
	}
	for (int32 D = 0; D < DataCount; ++D)
	{
		UMaterialExpression* Node = NewObject<UMaterialExpression>(Material, DataClass, NAME_None, RF_Transactional);
		Material->GetExpressionCollection().AddExpression(Node);
		Node->Material = Material;
		Node->MaterialExpressionEditorX = -900;
		Node->MaterialExpressionEditorY = 1700 + 100 * D;
		Node->UpdateMaterialExpressionGuid(true, false);
		static_cast<UMaterialExpressionPerInstanceCustomData*>(Node)->DataIndex = static_cast<uint32>(D);
		In(Code, *FString::Printf(TEXT("CD%d"), D), Node);
	}
	Out(Code, TEXT("RoughOut"), CMOT_Float1);
	Out(Code, TEXT("MetalOut"), CMOT_Float1);
	Out(Code, TEXT("NormalOut"), CMOT_Float3);
	Out(Code, TEXT("GlowOut"), CMOT_Float3);
	Code->RebuildOutputs();
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Code);
	Inputs->Roughness.Connect(1, Code);
	Inputs->Metallic.Connect(2, Code);
	Inputs->Normal.Connect(3, Code);
	Inputs->EmissiveColor.Connect(4, Code);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

uint32 Pack(const ftcore::KitRgb& C)
{
	auto Byte = [](float V) { return static_cast<uint32>(FMath::Clamp(FMath::RoundToInt(V * 255.f), 1, 255)); };
	return Byte(C.r) * 65536u + Byte(C.g) * 256u + Byte(C.b);
}

// A kit point's plan-frame linear map onto the imported mesh: mesh (x, y) = A * kit (x, y), z unchanged. The candidates
// are the four quarter turns, each with and without a mirror in y.
struct FAxisFit
{
	int32 A[2][2] = {{1, 0}, {0, 1}};
};

bool FitAxes(const ftcore::KitPiece& Piece, const FBox& MeshCm, FAxisFit& Fit, double& Error)
{
	const double Kx0 = Piece.minX, Kx1 = Piece.minX + Piece.sx, Ky0 = Piece.minY, Ky1 = Piece.minY + Piece.sy;
	const FVector Min = MeshCm.Min / 100.0, Max = MeshCm.Max / 100.0;
	Error = TNumericLimits<double>::Max();
	static const int32 Turns[4][2][2] = {{{1, 0}, {0, 1}}, {{0, -1}, {1, 0}}, {{-1, 0}, {0, -1}}, {{0, 1}, {-1, 0}}};
	for (int32 T = 0; T < 4; ++T)
	{
		for (int32 M = 0; M < 2; ++M)
		{
			int32 A[2][2];
			for (int32 R = 0; R < 2; ++R)
			{
				A[R][0] = Turns[T][R][0];
				A[R][1] = Turns[T][R][1] * (M ? -1 : 1);
			}
			double Lo[2], Hi[2];
			for (int32 R = 0; R < 2; ++R)
			{
				const double P[2] = {A[R][0] * Kx0, A[R][0] * Kx1};
				const double Q[2] = {A[R][1] * Ky0, A[R][1] * Ky1};
				Lo[R] = FMath::Min(P[0], P[1]) + FMath::Min(Q[0], Q[1]);
				Hi[R] = FMath::Max(P[0], P[1]) + FMath::Max(Q[0], Q[1]);
			}
			const double E = FMath::Abs(Lo[0] - Min.X) + FMath::Abs(Hi[0] - Max.X) + FMath::Abs(Lo[1] - Min.Y) + FMath::Abs(Hi[1] - Max.Y) +
				FMath::Abs(Piece.minZ - Min.Z) + FMath::Abs(Piece.minZ + Piece.sz - Max.Z);
			if (E < Error)
			{
				Error = E;
				FMemory::Memcpy(Fit.A, A, sizeof A);
			}
		}
	}
	return Error < 0.12;
}

// The kit's Nanite rule: a Nanite piece keeps its raster fallback and its ray-tracing proxy at full detail (the pieces
// are small; a reduced fallback shreds thin sills and rails in ray-traced shadows and wherever the fallback draws).
// Sets both to all triangles (saving the mesh when it changes), logs the fallback's triangles against the kit table's and
// fails a piece whose fallback keeps under 90% of them.
bool KeepFullFallback(UStaticMesh* Mesh, const ftcore::KitPiece& Piece)
{
	bool bChanged = false;
	if (Mesh->IsNaniteEnabled())
	{
		FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
		if (Nanite.FallbackTarget != ENaniteFallbackTarget::PercentTriangles || Nanite.FallbackPercentTriangles < 1.f)
		{
			Nanite.FallbackTarget = ENaniteFallbackTarget::PercentTriangles;
			Nanite.FallbackPercentTriangles = 1.f;
			Nanite.FallbackRelativeError = 0.f;
			Mesh->SetNaniteSettings(Nanite);
			bChanged = true;
		}
		FMeshRayTracingProxySettings Proxy = Mesh->GetRayTracingProxySettings();
		if (Proxy.FallbackTarget != ENaniteFallbackTarget::PercentTriangles || Proxy.FallbackPercentTriangles < 1.f)
		{
			Proxy.FallbackTarget = ENaniteFallbackTarget::PercentTriangles;
			Proxy.FallbackPercentTriangles = 1.f;
			Proxy.FallbackRelativeError = 0.f;
			Mesh->SetRayTracingProxySettings(Proxy);
			bChanged = true;
		}
	}
	if (bChanged)
	{
		Mesh->Build(true);
		Mesh->PostEditChange();
		UPackage* MeshPackage = Mesh->GetPackage();
		MeshPackage->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const FString File = FPackageName::LongPackageNameToFilename(MeshPackage->GetName(), FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(MeshPackage, Mesh, *File, Args))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: couldn't save %s with its full fallback."), *File);
			return false;
		}
	}
	const int32 Fallback = Mesh->GetNumLODs() > 0 ? Mesh->GetNumTriangles(0) : 0;
	const double Share = Piece.triangles > 0 ? static_cast<double>(Fallback) / Piece.triangles : 1.0;
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: %s: Nanite %s, fallback %d of %d source triangles (%.0f%%)%s."),
		UTF8_TO_TCHAR(Piece.id.c_str()), Mesh->IsNaniteEnabled() ? TEXT("on") : TEXT("off"), Fallback, Piece.triangles, 100.0 * Share,
		bChanged ? TEXT(", fallback and ray-tracing proxy set to every triangle") : TEXT(""));
	if (Share < 0.9)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: %s's fallback keeps under 90%% of its triangles."), UTF8_TO_TCHAR(Piece.id.c_str()));
		return false;
	}
	return true;
}

// The instance transform of a kit piece placed at P: engine = Rz(-yaw) * S * Mirror * A^-1 * mesh + E(P), where Mirror
// flips y (the plan frame is right-handed, the engine's left-handed).
FTransform PieceTransform(const ftcore::KitPlacement& P, const FAxisFit& Fit, double Side)
{
	// A is orthogonal with integer entries: its inverse is its transpose.
	const double Inv[2][2] = {{static_cast<double>(Fit.A[0][0]), static_cast<double>(Fit.A[1][0])},
		{static_cast<double>(Fit.A[0][1]), static_cast<double>(Fit.A[1][1])}};
	// L = diag(sx, -1) * Inv (the mirror, then the kit-frame stretch along x).
	const double L[2][2] = {{P.sx * Inv[0][0], P.sx * Inv[0][1]}, {-P.sy * Inv[1][0], -P.sy * Inv[1][1]}};
	const double Yaw = FMath::DegreesToRadians(-P.yaw);
	const double C = FMath::Cos(Yaw), S = FMath::Sin(Yaw);
	const double W[2][2] = {{C * L[0][0] - S * L[1][0], C * L[0][1] - S * L[1][1]}, {S * L[0][0] + C * L[1][0], S * L[0][1] + C * L[1][1]}};
	const ftcore::MassingVec E = ftcore::massingEnginePoint(Side, P.x, P.y, P.z);
	// FMatrix rows are the images of the mesh's axes.
	const FMatrix M(FPlane(W[0][0], W[1][0], 0.0, 0.0), FPlane(W[0][1], W[1][1], 0.0, 0.0), FPlane(0.0, 0.0, P.sz, 0.0),
		FPlane(E.x * 100.0, E.y * 100.0, E.z * 100.0, 1.0));
	return FTransform(M);
}
} // namespace

bool Plan(const TCHAR* Params, const std::vector<ftcore::MassingBox>& Boxes, FBuild& Out)
{
	Out = FBuild();
	FString RunsFile, KitFile;
	if (!FParse::Value(Params, TEXT("Lots="), RunsFile) || RunsFile.IsEmpty())
	{
		return true;
	}
	if (!FParse::Value(Params, TEXT("KitTable="), KitFile) || KitFile.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: pass -KitTable=<kit csv> too."));
		return false;
	}
	std::string Text, Error;
	std::vector<ftcore::FrontageRun> Runs;
	if (!ReadText(RunsFile, Text) || !ftcore::parseFrontageRuns(Text, Runs, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: %s: %s"), *RunsFile, *Utf8(Error));
		return false;
	}
	if (!ReadText(KitFile, Text) || !ftcore::parseKitTable(Text, Out.Kit, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: %s: %s"), *KitFile, *Utf8(Error));
		return false;
	}
	ftcore::planFrontage(Runs, Boxes, Out.Plan);
	for (size_t I = 0; I < Out.Plan.lots.size(); ++I)
	{
		const ftcore::BuildingLot& Lot = Out.Plan.lots[I];
		if (!ftcore::facadePlacements(Out.Kit, Out.Plan.runs[static_cast<size_t>(Lot.run)], Lot, static_cast<int>(I), Out.Placements, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: lot %d: %s"), static_cast<int32>(I), *Utf8(Error));
			return false;
		}
		FString Parents;
		for (const std::string& P : Lot.parents)
		{
			Parents += (Parents.IsEmpty() ? TEXT("") : TEXT(";")) + Utf8(P);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: lot %d on %s: %.2f to %.2f m (%.1f m), %s, %d storeys, floor %.2f, top %.2f, "
			"wall %u, setback %.2f, signature %08x [%s]."), static_cast<int32>(I), *Utf8(Out.Plan.runs[static_cast<size_t>(Lot.run)].id), Lot.s0, Lot.s1,
			Lot.width(), UTF8_TO_TCHAR(ftcore::lotTypeName(Lot.type)), Lot.floors, Lot.floorZ, Lot.top(), Lot.wallColour, Lot.setback,
			ftcore::lotSignature(Lot), *Parents);
	}
	Out.Replaced.assign(Boxes.size(), false);
	int32 Hidden = 0;
	for (size_t I = 0; I < Boxes.size(); ++I)
	{
		for (const std::string& Id : Out.Plan.replaced)
		{
			if (Boxes[I].id == Id)
			{
				Out.Replaced[I] = true;
				++Hidden;
				break;
			}
		}
	}
	const ftcore::FrontageCheck Check = ftcore::checkFrontage(Out.Plan.lots);
	const ftcore::KitCensus Census = ftcore::kitCensus(Out.Kit, Out.Plan, Out.Placements);
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: %d runs, %d lots (corner %d, pencil %d, walk-up %d, tiled %d, town house %d), "
		"%d boxes replaced, %d repairs; %d pieces, %d plain boxes, %lld triangles."), static_cast<int32>(Runs.size()), Census.lots, Census.types[0],
		Census.types[1], Census.types[2], Census.types[3], Census.types[4], Hidden, Out.Plan.repairs, Census.pieces, Census.boxes, Census.triangles);
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: street rules %s (equal facades within 40 m %d, neighbour twins %d, "
		"60 m windows under three types %d, wall colour triples %d)%s%s"), Check.ok ? TEXT("PASS") : TEXT("FAIL"), Check.equalWithin40,
		Check.neighbourTwins, Check.poorWindows, Check.colourTriples, Check.ok ? TEXT(".") : TEXT(": "), *Utf8(Check.firstFault));
	Out.bOn = true;
	return true;
}

bool Place(FBuild& Build, FFTCellInstancer& Cells, UPackage* Package, UStaticMesh* Cube, double Side)
{
	if (!Build.bOn)
	{
		return true;
	}
	const TCHAR* const SharedDir = TEXT("/Game/Characters/Shared/Materials/");
	UTexture* White = LoadObject<UTexture>(nullptr, *(FString(SharedDir) + TEXT("T_FTWhite.T_FTWhite")));
	UTexture* Masks = LoadObject<UTexture>(nullptr, *(FString(SharedDir) + TEXT("T_FTWhiteMasks.T_FTWhiteMasks")));
	UTexture* Flat = LoadObject<UTexture>(nullptr, *(FString(SharedDir) + TEXT("T_FTFlatNormal.T_FTFlatNormal")));
	if (!White || !Masks || !Flat || !Cube)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: the default textures in %s or the cube are missing; import the kit first."), SharedDir);
		return false;
	}
	UMaterial* Master = BuildMaster(Package, White, Masks, Flat);
	if (!Master)
	{
		return false;
	}

	// One instance per slot, its maps from the first imported piece that has the slot.
	TMap<FString, UMaterialInstanceConstant*> SlotMaterial;
	TMap<FString, TArray<UTexture*>> SlotMaps;
	for (const FSlotLook& Look : kSlots)
	{
		UTexture* Maps[3] = {nullptr, nullptr, nullptr};
		for (const ftcore::KitPiece& Piece : Build.Kit.pieces)
		{
			bool bHas = false;
			for (const std::string& S : Piece.slots)
			{
				bHas = bHas || Utf8(S) == Look.Name;
			}
			if (!bHas)
			{
				continue;
			}
			const FString Path = FString::Printf(TEXT("%s/%s/Materials/%s.%s"), PieceFolder, *Utf8(Piece.id), Look.Name, Look.Name);
			if (const UMaterialInstance* Source = LoadObject<UMaterialInstance>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				static const TCHAR* const Names[3] = {TEXT("BaseColorTexture"), TEXT("MetallicRoughnessTexture"), TEXT("NormalTexture")};
				for (int32 K = 0; K < 3; ++K)
				{
					UTexture* T = nullptr;
					if (Source->GetTextureParameterValue(FHashedMaterialParameterInfo(Names[K]), T) && T && T != White && T != Masks && T != Flat)
					{
						Maps[K] = T;
					}
				}
				break;
			}
		}
		UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, *FString::Printf(TEXT("MI_Kit_%s"), Look.Name), RF_Public);
		Mic->SetParentEditorOnly(Master);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Slot")), Look.Slot);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), Look.Rough);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("HasMR")), Maps[1] ? 1.f : 0.f);
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Tint")), Look.Tint);
		// The tiles' grout and the roof tiles' laps read deeper than the map's own normals give.
		const FString SlotName(Look.Name);
		if (SlotName == TEXT("jpk_wall_tile") || SlotName == TEXT("jpk_rooftile"))
		{
			Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Relief")), 1.8f);
		}
		static const TCHAR* const Params[3] = {TEXT("BaseColorTexture"), TEXT("MetallicRoughnessTexture"), TEXT("NormalTexture")};
		for (int32 K = 0; K < 3; ++K)
		{
			if (Maps[K])
			{
				Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Params[K]), Maps[K]);
			}
		}
		Mic->PostEditChange();
		SlotMaterial.Add(Look.Name, Mic);
		SlotMaps.Add(Look.Name, TArray<UTexture*>{Maps[0], Maps[1], Maps[2]});
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: %s: slot %.0f, maps %s / %s / %s."), Look.Name, Look.Slot,
			Maps[0] ? *Maps[0]->GetName() : TEXT("-"), Maps[1] ? *Maps[1]->GetName() : TEXT("-"), Maps[2] ? *Maps[2]->GetName() : TEXT("-"));
	}
	// The plain boxes: a body per wall surface (world-planar maps, the lot's wall tint) and the plinth.
	auto BoxMaterial = [&](const TCHAR* Name, const TCHAR* Surface, float Slot, float TileM, const FLinearColor& Tint)
	{
		UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
		Mic->SetParentEditorOnly(Master);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Slot")), Slot);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("WorldUV")), 1.f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TileM")), TileM);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), 0.8f);
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Tint")), Tint);
		const TArray<UTexture*>* Maps = SlotMaps.Find(Surface);
		if (Maps && (*Maps)[0])
		{
			Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BaseColorTexture")), (*Maps)[0]);
		}
		if (Maps && (*Maps)[1])
		{
			Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MetallicRoughnessTexture")), (*Maps)[1]);
			Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("HasMR")), 1.f);
		}
		Mic->PostEditChange();
		return Mic;
	};
	UMaterialInterface* const Bodies[3] = {BoxMaterial(TEXT("MI_Kit_Body_Tile"), TEXT("jpk_wall_tile"), 0.f, 1.f, FLinearColor::White),
		BoxMaterial(TEXT("MI_Kit_Body_Concrete"), TEXT("jpk_wall_concrete"), 0.f, 2.f, FLinearColor::White),
		BoxMaterial(TEXT("MI_Kit_Body_Stucco"), TEXT("jpk_wall_stucco"), 0.f, 2.f, FLinearColor::White)};
	UMaterialInterface* const Plinth = BoxMaterial(TEXT("MI_Kit_Plinth"), TEXT("jpk_wall_concrete"), 9.f, 2.f, FLinearColor(0.50f, 0.49f, 0.47f));

	// The pieces: their meshes, their slots' materials and the axis fit, checked on every piece.
	struct FPieceUse
	{
		UStaticMesh* Mesh = nullptr;
		TArray<UMaterialInterface*> Materials;
		FAxisFit Fit;
	};
	TArray<FPieceUse> Uses;
	Uses.SetNum(static_cast<int32>(Build.Kit.pieces.size()));
	TArray<bool> Needed;
	Needed.Init(false, Uses.Num());
	for (const ftcore::KitPlacement& P : Build.Placements)
	{
		if (P.piece >= 0)
		{
			Needed[P.piece] = true;
		}
	}
	int32 Fits[2] = {0, 0};
	for (int32 K = 0; K < Uses.Num(); ++K)
	{
		if (!Needed[K])
		{
			continue;
		}
		const ftcore::KitPiece& Piece = Build.Kit.pieces[static_cast<size_t>(K)];
		const FString Id = Utf8(Piece.id);
		const FString Path = FString::Printf(TEXT("%s/%s/StaticMeshes/%s.%s"), PieceFolder, *Id, *Id, *Id);
		FPieceUse& Use = Uses[K];
		Use.Mesh = LoadObject<UStaticMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Use.Mesh)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: %s is missing; import the kit (FTImportProp -Kit) first."), *Path);
			return false;
		}
		if (!KeepFullFallback(Use.Mesh, Piece))
		{
			return false;
		}
		double Error = 0.0;
		if (!FitAxes(Piece, Use.Mesh->GetBoundingBox(), Use.Fit, Error))
		{
			const FBox B = Use.Mesh->GetBoundingBox();
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: %s: no quarter turn maps the kit table's bounds onto the mesh's (%s to %s cm; "
				"best error %.2f m)."), *Id, *B.Min.ToString(), *B.Max.ToString(), Error);
			return false;
		}
		const bool bPlain = Use.Fit.A[0][0] == 1 && Use.Fit.A[1][1] == -1;
		++Fits[bPlain ? 0 : 1];
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: %s: mesh axes [%d %d; %d %d] from the kit's, fit error %.3f m."), *Id,
			Use.Fit.A[0][0], Use.Fit.A[0][1], Use.Fit.A[1][0], Use.Fit.A[1][1], Error);
		for (const FStaticMaterial& Slot : Use.Mesh->GetStaticMaterials())
		{
			FString Name = Slot.MaterialSlotName.ToString();
			UMaterialInstanceConstant* const* Found = SlotMaterial.Find(Name);
			if (!Found && Slot.MaterialInterface)
			{
				Name = Slot.MaterialInterface->GetName();
				Found = SlotMaterial.Find(Name);
			}
			if (!Found)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld -Lots: %s: slot %s has no kit material; it keeps its own."), *Id, *Name);
			}
			Use.Materials.Add(Found ? *Found : nullptr);
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: axis fits: %d pieces as imported with y flipped, %d other."), Fits[0], Fits[1]);

	int32 Pieces = 0, Boxes = 0;
	float Data[DataCount];
	for (const ftcore::KitPlacement& P : Build.Placements)
	{
		const ftcore::BuildingLot& Lot = Build.Plan.lots[static_cast<size_t>(P.lot)];
		Data[DataWall] = static_cast<float>(Pack(ftcore::kitPaletteColour(ftcore::KitPalette::Wall, Lot.wallColour)));
		Data[DataTrim] = static_cast<float>(Pack(ftcore::kitPaletteColour(ftcore::KitPalette::Trim, Lot.trimColour)));
		Data[DataAccent] = static_cast<float>(Pack(ftcore::kitPaletteColour(ftcore::KitPalette::Accent, Lot.accent)));
		Data[DataTimber] = static_cast<float>(Pack(ftcore::kitPaletteColour(ftcore::KitPalette::Timber, Lot.timber)));
		Data[DataRoof] = static_cast<float>(Pack(ftcore::kitPaletteColour(ftcore::KitPalette::Roof, Lot.roofColour)));
		Data[DataSignFg] = static_cast<float>(Pack(ftcore::kitPaletteColour(ftcore::KitPalette::SignFg, Lot.signColour)));
		Data[DataSeed] = static_cast<float>(P.seed % 9973u) / 9973.f;
		Data[DataDirt] = static_cast<float>(Lot.clutterSeed % 101u) / 100.f;
		Data[DataShop] = 0.f;
		FFTCellOptions Opts;
		Opts.Label = FString::Printf(TEXT("LOT-%d"), P.lot);
		if (P.piece < 0)
		{
			const bool bPlinth = P.role == ftcore::PlacementRole::Plinth;
			UMaterialInterface* const Mats[1] = {bPlinth ? Plinth : Bodies[static_cast<int32>(Lot.wall)]};
			const ftcore::MassingVec E = ftcore::massingEnginePoint(Side, P.x, P.y, P.z + 0.5 * P.sz);
			const FVector Mid = FVector(E.x, E.y, E.z) * 100.0;
			Cells.Add(Cube, Mats, FTransform(FRotator(0.0, -P.yaw, 0.0), Mid, FVector(P.sx, P.sy, P.sz)), Data, EFTCellKind::Solid, Opts);
			++Boxes;
			continue;
		}
		const FPieceUse& Use = Uses[P.piece];
		const ftcore::KitPiece& Piece = Build.Kit.pieces[static_cast<size_t>(P.piece)];
		const bool bGround = Piece.group == "ground";
		Data[DataShop] = bGround ? 1.f : 0.f;
		const bool bSolid = (Piece.group == "ground" || Piece.group == "upper") && P.role == ftcore::PlacementRole::Piece;
		Opts.Cull = P.role == ftcore::PlacementRole::Clutter ? EFTCellCull::Prop : Piece.group == "roof" && Piece.id.rfind("TANK", 0) == 0 ? EFTCellCull::Dressing
			: EFTCellCull::None;
		Cells.Add(Use.Mesh, Use.Materials, PieceTransform(P, Use.Fit, Side), Data,
			bSolid || Piece.id == "VENDING" ? EFTCellKind::Solid : EFTCellKind::NoCollide, Opts);
		++Pieces;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: placed %d kit pieces and %d plain boxes."), Pieces, Boxes);
	return true;
}
} // namespace FTLots
