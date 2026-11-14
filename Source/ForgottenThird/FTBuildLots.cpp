#include "FTBuildLots.h"

#include "ForgottenThird.h"
#include "FTCellInstancer.h"
#include "FTSceneLook.h"
#include "FTText.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionPixelDepth.h"
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

#include "ftcore/GlassSurface.h"
#include "ftcore/WindowFacade.h"

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

const TCHAR* const kKitCodeA = TEXT(R"(
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
float spec = 0.5;
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
	bool proof = CD8 > 1.5;
	bool lit = proof || r < (shop ? 0.85 : LitShare);
	float3 warm = r < 0.12 ? float3(0.75, 0.85, 1.0) : float3(1.0, 0.78, 0.52);
	// The lamps' light: what the flat glow gave (GlassGlow, twice in a shop), soft-capped at the city's night cap (the
	// window master's: ShopCap in a shop, WinCap upstairs), or the proof front's own level; then no front over that cap.
	float cap = shop ? ShopCap : WinCap;
	float3 lamp = warm * NightGlow * GlassGlow * (shop ? 2.0 : 1.0);
	lamp = lamp / (1.0 + dot(lamp, float3(0.2126, 0.7152, 0.0722)) / max(cap, 1e-4));
	if (proof)
		lamp = float3(1.0, 0.9, 0.76) * ProofNight;
	lamp = lamp * min(1.0, cap / max(dot(lamp, float3(0.2126, 0.7152, 0.0722)), 1e-4));
	float3 V = -normalize(CamV);
	float yLoc = 1.0 - UV.y;
	float2 iA = float2(dot(WorldPos, h), WorldPos.z) / 100.0;
	float iFloor = iA.y - yLoc;
	float3 iV = float3(dot(V, h), V.z, dot(V, -n));
	float iShop = shop ? 1.0 : 0.0;
	float iSeed = CD6;
	float iLit = lit ? Night : 0.0;
	float iDay = Daylight;
	float iDayLevel = DayLevel;
	float3 iLamp = lamp * 1.5;
	float iEyeM = PixDepth / 100.0;
)");
const TCHAR* const kKitCodeB = TEXT(R"(	float px0 = PaneRect.x;
	float px1 = PaneRect.z;
	bool hasPanes = px1 > px0;
	float py0 = PaneRect.y;
	float py1 = PaneRect.w;
	bool overDoor = false;
	if (PaneTransom > py0 && PaneTransom < py1)
	{
		overDoor = yLoc >= PaneTransom;
		if (overDoor)
			py0 = PaneTransom;
		else
			py1 = PaneTransom;
	}
	float4 cutsA = overDoor ? PaneColsC : PaneColsA;
	float4 cutsB = overDoor ? PaneColsD : PaneColsB;
	float lo = px0;
	float hi = px1;
	float cuts[8] = {cutsA.x, cutsA.y, cutsA.z, cutsA.w, cutsB.x, cutsB.y, cutsB.z, cutsB.w};
	for (int k = 0; k < 8; ++k)
	{
		if (cuts[k] > px0 && cuts[k] < px1)
		{
			if (UV.x >= cuts[k])
				lo = max(lo, cuts[k]);
			else
				hi = min(hi, cuts[k]);
		}
	}
	float2 gUV = hasPanes ? saturate(float2((UV.x - lo) / max(hi - lo, 0.01), (yLoc - py0) / max(py1 - py0, 0.01)))
		: float2(0.25 + 0.5 * frac(UV.x / 0.9), 0.45 + 0.3 * frac(yLoc / 1.2));
	float2 gSize = hasPanes ? float2(hi - lo, py1 - py0) : float2(0.9, 1.2);
	float gSeed = frac(CD6 * 3.7 + lo * 0.37);
	float gAge = shop ? 0.15 : 0.3 + 0.5 * CD7;
	float gWet = Wet;
	float gThick = shop ? 0.008 : 0.005;
	float gCos = abs(dot(n, V));
	bool doorPane = hasPanes && !overDoor && (abs(lo - PaneDoor.x) < 0.02 || abs(lo - PaneDoor.y) < 0.02);
	float2 gHandle = doorPane ? PaneDoor.zw : float2(-1.0, 0.0);
)");
const TCHAR* const kKitCodeC = TEXT(R"(	glow = iCol * (1.0 - gMirror) * gTint * gDim * (1.0 - gCover) * (1.0 - gSeal) * lerp(float3(1.0, 1.0, 1.0), float3(0.25, 0.45, 0.35), gEdge);
	// No point of any front's interior over the city's cap (the lamps' panels and pale goods included), the proof front too.
	glow *= min(1.0, cap / max(dot(glow, float3(0.2126, 0.7152, 0.0722)), 1e-4));
	base = lerp(gFilm * gCover, float3(0.02, 0.02, 0.02), gSeal);
	rough = lerp(gRough, 0.6, gSeal);
	metal = 0.0;
	spec = lerp(0.96, 0.3, gSeal);
	tn = normalize(float3(gWave.x, gWave.y, 1.0));
}
else if (s == 8)
{
	base = bc * Tint.rgb;
	rough = 0.8;
}
// The glass's light is already what the eye gets through the pane: its lamps took the cap above.
if (s != 7)
	glow = glow / (1.0 + dot(glow, float3(0.2126, 0.7152, 0.0722)) / max(GlowCap, 1e-4));
SpecOut = spec;
RoughOut = saturate(rough);
MetalOut = saturate(metal);
NormalOut = tn;
GlowOut = glow;
return base;
)");
// The kit master's code: the glass slot (7) is glass over a picture (ftcore/GlassSurface.h): the room behind it and the
// shared glass function sit between the three pieces above.
FString KitCode()
{
	return FString(kKitCodeA) + FT::ToFString(ftcore::glassPictureHlsl()) + kKitCodeB + FT::ToFString(ftcore::glassHlsl()) + kKitCodeC;
}

// A glazed piece's panes, in the piece's own metres (the kit's glass UVs): the glass's rectangle (x0, y0, x1, y1), the
// mullions' lines under the transom (ColsA, ColsB: up to eight) and over it (ColsC, ColsD: the fanlights' own bars), the
// transom's height (its bar's middle) and half its bar's height, and the door leaves' left edges with their handle's
// place in the leaf (u, v). The lines are the bars' middles as modelled. A piece without a row keeps the film alone.
struct FPaneTable
{
	const TCHAR* Piece;
	FLinearColor Rect, ColsA, ColsB, ColsC, ColsD, Door;
	float Transom, TransomHalf;
};
const FPaneTable kPanes[] = {
	{TEXT("SHOP_GLAZED_81"), FLinearColor(0.33f, 0.12f, 7.77f, 2.84f), FLinearColor(1.725f, 3.15f, 4.05f, 4.95f), FLinearColor(6.38f, 0.f, 0.f, 0.f),
		FLinearColor(1.555f, 2.80f, 4.055f, 5.305f), FLinearColor(6.555f, 0.f, 0.f, 0.f), FLinearColor(3.15f, 4.05f, 0.18f, 0.45f), 2.285f, 0.035f},
};

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
	Code->Code = KitCode();
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
	// The glass slot's lamps under the city's night caps (the window master's own values).
	In(Code, TEXT("ShopCap"), Scalar(Material, TEXT("ShopGlowCap"), ftcore::WindowFacadeSpec{}.shopGlowCap, 1410));
	In(Code, TEXT("WinCap"), Scalar(Material, TEXT("WindowGlowCap"), ftcore::WindowFacadeSpec{}.glowCap, 1420));
	In(Code, TEXT("Relief"), Scalar(Material, TEXT("Relief"), 1.f, 1450));
	In(Code, TEXT("DirtAmount"), Scalar(Material, TEXT("DirtAmount"), 1.f, 1475));
	In(Code, TEXT("Night"), Night(Material, Collection, TEXT("Night"), 0.f, 1500));
	In(Code, TEXT("NightGlow"), Night(Material, Collection, TEXT("NightGlow"), 1.f, 1600));
	// The glass slot (glass over a picture): the view, the rooms' daylight, the proof front's own light, the ground's
	// wetness, a room's light by full day and a glazed piece's panes (FPaneTable; a piece's own instance sets them).
	UMaterialParameterCollection* LookParams = LoadObject<UMaterialParameterCollection>(nullptr,
		*(FString(FTScene::LookCollection) + TEXT(".") + FPackageName::GetShortName(FTScene::LookCollection)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	In(Code, TEXT("CamV"), AddNode<UMaterialExpressionCameraVectorWS>(Material, 2700));
	In(Code, TEXT("Daylight"), Night(Material, Collection, TEXT("Daylight"), 1.f, 2800));
	In(Code, TEXT("ProofNight"), Night(Material, Collection, TEXT("ProofShopNight"), ftcore::WindowFacadeSpec{}.shopGlowCap, 2900));
	In(Code, TEXT("Wet"), Night(Material, LookParams, TEXT("Wetness"), 0.f, 3000));
	In(Code, TEXT("DayLevel"), Scalar(Material, TEXT("GlassDayLevel"), 0.35f, 3100));
	// The goods on a shop's shelves are cut from the shelf-front picture (white when it has not been imported); the eye's
	// distance picks its mip (the Nanite slot takes no derivatives).
	In(Code, TEXT("PixDepth"), AddNode<UMaterialExpressionPixelDepth>(Material, 3150));
	UTexture* const Goods = LoadObject<UTexture>(nullptr, *(FString(FTScene::LookGoodsTexture) + TEXT(".") + FPackageName::GetShortName(FTScene::LookGoodsTexture)),
		nullptr, LOAD_NoWarn | LOAD_Quiet);
	In(Code, TEXT("GoodsMap"), Texture(TEXT("GoodsMap"), Goods ? Goods : White, SAMPLERTYPE_Color, 3180));
	In(Code, TEXT("PaneTransom"), Scalar(Material, TEXT("PaneTransom"), 0.f, 3200));
	const TCHAR* const PaneVectors[] = {TEXT("PaneRect"), TEXT("PaneColsA"), TEXT("PaneColsB"), TEXT("PaneColsC"), TEXT("PaneColsD"), TEXT("PaneDoor")};
	for (int32 V = 0; V < 6; ++V)
	{
		UMaterialExpressionVectorParameter* P = AddNode<UMaterialExpressionVectorParameter>(Material, 3300 + 100 * V);
		P->ParameterName = PaneVectors[V];
		P->DefaultValue = FLinearColor(0.f, 0.f, 0.f, 0.f);
		P->UpdateParameterGuid(true, false);
		// Output 5 of a vector parameter is its four channels together.
		FCustomInput& Input = Code->Inputs.AddDefaulted_GetRef();
		Input.InputName = PaneVectors[V];
		Input.Input.Connect(5, P);
	}
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
	Out(Code, TEXT("SpecOut"), CMOT_Float1);
	Code->RebuildOutputs();
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Code);
	Inputs->Roughness.Connect(1, Code);
	Inputs->Metallic.Connect(2, Code);
	Inputs->Normal.Connect(3, Code);
	Inputs->EmissiveColor.Connect(4, Code);
	Inputs->Specular.Connect(5, Code);
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
	FParse::Value(Params, TEXT("GlassProofLot="), Out.GlassProofLot);
	Out.bGlassProofPane = FParse::Param(Params, TEXT("GlassProofPane"));
	if (Out.GlassProofLot >= 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: the glass proof front is lot %d%s."), Out.GlassProofLot,
			Out.GlassProofLot < static_cast<int32>(Out.Plan.lots.size()) ? TEXT("") : TEXT(" (no such lot)"));
	}
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
			UMaterialInstanceConstant* Wears = Found ? *Found : nullptr;
			for (const FPaneTable& Panes : kPanes)
			{
				if (Wears && Name == TEXT("jpk_glass") && Id == Panes.Piece)
				{
					UMaterialInstanceConstant* Own = NewObject<UMaterialInstanceConstant>(Package, *FString::Printf(TEXT("MI_Kit_jpk_glass_%s"), *Id), RF_Public);
					Own->SetParentEditorOnly(Wears);
					Own->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("PaneRect")), Panes.Rect);
					Own->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("PaneColsA")), Panes.ColsA);
					Own->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("PaneColsB")), Panes.ColsB);
					Own->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("PaneColsC")), Panes.ColsC);
					Own->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("PaneColsD")), Panes.ColsD);
					Own->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("PaneDoor")), Panes.Door);
					Own->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("PaneTransom")), Panes.Transom);
					Own->PostEditChange();
					Wears = Own;
					UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: %s: its glass wears its own panes (%s)."), *Id, *Own->GetName());
				}
			}
			Use.Materials.Add(Wears);
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: axis fits: %d pieces as imported with y flipped, %d other."), Fits[0], Fits[1]);

	// The glass proof front's true panes (-GlassProofPane with -GlassProofLot): its glazed shop piece's glass slot is cut
	// out, a pane mesh stands in each of its panes (the true pane's material; Nanite draws no translucent section), the
	// lot's body is cut back behind the glass for a real bay, lined, shelved with goods and kit pieces, lit by its own
	// surfaces (the bay's material), so the eye sees real things through real glass.
	const bool bProofLot = Build.GlassProofLot >= 0 && Build.GlassProofLot < static_cast<int32>(Build.Plan.lots.size());
	bool bProofPane = Build.bGlassProofPane && bProofLot;
	UMaterialInterface* ProofHole = nullptr;
	UMaterialInterface* ProofPane = nullptr;
	UMaterialInterface* ProofBay = nullptr;
	if (bProofPane)
	{
		const auto LoadLook = [](const TCHAR* Path)
		{
			return LoadObject<UMaterialInterface>(nullptr, *(FString(Path) + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
		};
		ProofHole = LoadLook(FTScene::LookGlassHoleMaterial);
		ProofPane = LoadLook(FTScene::LookGlassPaneMaterial);
		ProofBay = LoadLook(FTScene::LookGlassBayMaterial);
		if (!ProofHole || !ProofPane || !ProofBay)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: -GlassProofPane: the glass materials are missing (FTBuildLookMaterials -Glass); "
				"the proof front keeps glass over a picture."));
			bProofPane = false;
		}
	}
	// The proof lot's frame: a lot point (x m along it, y m in from the run's line) in the plan, and back.
	struct FLotFrame
	{
		double Ax = 0.0, Ay = 0.0, Ex = 1.0, Ey = 0.0, Ix = 0.0, Iy = 1.0, S0 = 0.0, Yaw = 0.0;
		void ToPlan(double X, double Y, double& Px, double& Py) const
		{
			Px = Ax + Ex * (S0 + X) + Ix * Y;
			Py = Ay + Ey * (S0 + X) + Iy * Y;
		}
		void ToLot(double Px, double Py, double& X, double& Y) const
		{
			X = (Px - Ax) * Ex + (Py - Ay) * Ey - S0;
			Y = (Px - Ax) * Ix + (Py - Ay) * Iy;
		}
	};
	FLotFrame Proof;
	// The bay behind the proof front's glass, in lot terms (set when its glazed piece is found below).
	double BayX0 = 0.0, BayX1 = 0.0, BayY0 = 0.0, BayY1 = 0.0, BayZ0 = 0.0, BayZ1 = 0.0;
	const FPaneTable* ProofPanes = nullptr;
	const ftcore::KitPlacement* ProofPiece = nullptr;
	if (bProofPane)
	{
		const ftcore::BuildingLot& Lot = Build.Plan.lots[static_cast<size_t>(Build.GlassProofLot)];
		const ftcore::FrontageRun& Run = Build.Plan.runs[static_cast<size_t>(Lot.run)];
		double Bx = 0.0, By = 0.0, Cx = 0.0, Cy = 0.0;
		Run.toPlan(0.0, 0.0, Proof.Ax, Proof.Ay);
		Run.toPlan(1.0, 0.0, Bx, By);
		Run.toPlan(0.0, 1.0, Cx, Cy);
		Proof.Ex = Bx - Proof.Ax;
		Proof.Ey = By - Proof.Ay;
		Proof.Ix = Cx - Proof.Ax;
		Proof.Iy = Cy - Proof.Ay;
		Proof.S0 = Lot.s0;
		Proof.Yaw = Run.yaw();
		for (const ftcore::KitPlacement& P : Build.Placements)
		{
			if (P.lot != Build.GlassProofLot || P.piece < 0 || P.role != ftcore::PlacementRole::Piece)
			{
				continue;
			}
			const FString Id = Utf8(Build.Kit.pieces[static_cast<size_t>(P.piece)].id);
			for (const FPaneTable& Panes : kPanes)
			{
				if (!ProofPiece && Id == Panes.Piece)
				{
					ProofPiece = &P;
					ProofPanes = &Panes;
				}
			}
		}
		if (!ProofPiece)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld -Lots: -GlassProofPane: lot %d has no glazed piece with a pane table; it keeps "
				"glass over a picture."), Build.GlassProofLot);
			bProofPane = false;
		}
		else
		{
			double PieceX = 0.0, PieceY = 0.0;
			Proof.ToLot(ProofPiece->x, ProofPiece->y, PieceX, PieceY);
			const double Depth = FMath::Clamp(FMath::Max(Lot.depth, 3.0) - (PieceY + 0.30) - 1.0, 0.8, 2.4);
			BayX0 = PieceX + 0.30;
			BayX1 = PieceX + ProofPanes->Rect.B + 0.03;
			BayY0 = PieceY + 0.25;
			BayY1 = PieceY + 0.30 + Depth;
			BayZ0 = ProofPiece->z;
			BayZ1 = ProofPiece->z + 2.90;
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: the glass proof front (lot %d): true panes on %s, a real bay %.2f m wide, "
				"%.2f m deep and %.2f m high behind them."), Build.GlassProofLot, ProofPanes->Piece, BayX1 - BayX0, BayY1 - BayY0, BayZ1 - BayZ0);
		}
	}
	// A box of the proof lot (lot terms) as a cube instance.
	int32 ProofBoxes = 0;
	const auto LotBox = [&](double X0, double Y0, double Z0, double X1, double Y1, double Z1, UMaterialInterface* Material, const float* BoxData,
		EFTCellKind Kind, const TCHAR* Tag, double Turn = 0.0)
	{
		if (X1 - X0 < 0.002 || Y1 - Y0 < 0.002 || Z1 - Z0 < 0.002)
		{
			return;
		}
		double Px = 0.0, Py = 0.0;
		Proof.ToPlan(0.5 * (X0 + X1), 0.5 * (Y0 + Y1), Px, Py);
		const ftcore::MassingVec E = ftcore::massingEnginePoint(Side, Px, Py, 0.5 * (Z0 + Z1));
		UMaterialInterface* const Mats[1] = {Material};
		FFTCellOptions BoxOpts;
		BoxOpts.Label = FString::Printf(TEXT("LOT-%d"), Build.GlassProofLot);
		BoxOpts.bCastShadow = Kind != EFTCellKind::NoCollide;
		BoxOpts.Tag = Tag;
		// Turn 90: the cube's x runs in from the facade (a pane's thin axis), its y along it.
		const FVector Scale = Turn != 0.0 ? FVector(Y1 - Y0, X1 - X0, Z1 - Z0) : FVector(X1 - X0, Y1 - Y0, Z1 - Z0);
		Cells.Add(Cube, Mats, FTransform(FRotator(0.0, -Proof.Yaw + Turn, 0.0), FVector(E.x, E.y, E.z) * 100.0, Scale),
			TArrayView<const float>(BoxData, DataCount), Kind, BoxOpts);
		++ProofBoxes;
	};

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
			// The proof front's body is cut back behind its glass: what is left of the box round the bay.
			if (bProofPane && !bPlinth && P.lot == Build.GlassProofLot)
			{
				double Cx = 0.0, Cy = 0.0;
				Proof.ToLot(P.x, P.y, Cx, Cy);
				const double X0 = Cx - 0.5 * P.sx, X1 = Cx + 0.5 * P.sx, Y0 = Cy - 0.5 * P.sy, Y1 = Cy + 0.5 * P.sy, Z0 = P.z, Z1 = P.z + P.sz;
				if (X0 < BayX1 && X1 > BayX0 && Y0 < BayY1 && Y1 > BayY0 && Z0 < BayZ1 && Z1 > BayZ0)
				{
					const double Zc = FMath::Min(Z1, BayZ1);
					LotBox(X0, Y0, Zc, X1, Y1, Z1, Mats[0], Data, EFTCellKind::Solid, TEXT("GlassProofBody"));      // over the bay
					LotBox(X0, BayY1, Z0, X1, Y1, Zc, Mats[0], Data, EFTCellKind::Solid, TEXT("GlassProofBody"));   // behind it
					LotBox(X0, Y0, Z0, BayX0, FMath::Min(Y1, BayY1), Zc, Mats[0], Data, EFTCellKind::Solid, TEXT("GlassProofBody"));
					LotBox(BayX1, Y0, Z0, X1, FMath::Min(Y1, BayY1), Zc, Mats[0], Data, EFTCellKind::Solid, TEXT("GlassProofBody"));
					++Boxes;
					continue;
				}
			}
			const ftcore::MassingVec E = ftcore::massingEnginePoint(Side, P.x, P.y, P.z + 0.5 * P.sz);
			const FVector Mid = FVector(E.x, E.y, E.z) * 100.0;
			Cells.Add(Cube, Mats, FTransform(FRotator(0.0, -P.yaw, 0.0), Mid, FVector(P.sx, P.sy, P.sz)), Data, EFTCellKind::Solid, Opts);
			++Boxes;
			continue;
		}
		const FPieceUse& Use = Uses[P.piece];
		const ftcore::KitPiece& Piece = Build.Kit.pieces[static_cast<size_t>(P.piece)];
		const bool bGround = Piece.group == "ground";
		// 2 on the glass proof front's shop storey (-GlassProofLot=<lot>): its interior takes the look's own night level.
		Data[DataShop] = bGround ? (P.lot == Build.GlassProofLot ? 2.f : 1.f) : 0.f;
		const bool bSolid = (Piece.group == "ground" || Piece.group == "upper") && P.role == ftcore::PlacementRole::Piece;
		Opts.Cull = P.role == ftcore::PlacementRole::Clutter ? EFTCellCull::Prop : Piece.group == "roof" && Piece.id.rfind("TANK", 0) == 0 ? EFTCellCull::Dressing
			: EFTCellCull::None;
		// The proof front's glazed piece: its glass slot cut out, a true pane in each of its panes, the bay behind.
		if (bProofPane && &P == ProofPiece)
		{
			TArray<UMaterialInterface*> Open = Use.Materials;
			const TArray<FStaticMaterial>& Slots = Use.Mesh->GetStaticMaterials();
			for (int32 S = 0; S < Slots.Num() && S < Open.Num(); ++S)
			{
				if (Slots[S].MaterialSlotName == TEXT("jpk_glass") || (Slots[S].MaterialInterface && Slots[S].MaterialInterface->GetName() == TEXT("jpk_glass")))
				{
					Open[S] = ProofHole;
				}
			}
			Cells.Add(Use.Mesh, Open, PieceTransform(P, Use.Fit, Side), Data, EFTCellKind::Solid, Opts);
			++Pieces;
			double PieceX = 0.0, PieceY = 0.0;
			Proof.ToLot(P.x, P.y, PieceX, PieceY);
			const auto BayMaterial = [&](const TCHAR* Name, const FLinearColor& Color, float LampPanel)
			{
				UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
				Mic->SetParentEditorOnly(ProofBay);
				Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), Color);
				Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LampPanel")), LampPanel);
				Mic->PostEditChange();
				return Mic;
			};
			// The panes: the table's columns under the transom, and the fanlights' own over it.
			const FPaneTable& T = *ProofPanes;
			int32 PaneCount = 0;
			for (int32 R = 0; R < 2; ++R)
			{
				const FLinearColor& A = R == 0 ? T.ColsA : T.ColsC;
				const FLinearColor& B = R == 0 ? T.ColsB : T.ColsD;
				// The cuts in order, the unused (0) ones dropped, between the glass's two ends.
				float Cuts[10];
				int32 CutCount = 0;
				Cuts[CutCount++] = T.Rect.R;
				for (const float Cut : {A.R, A.G, A.B, A.A, B.R, B.G, B.B, B.A})
				{
					if (Cut > T.Rect.R && Cut < T.Rect.B)
					{
						Cuts[CutCount++] = Cut;
					}
				}
				Cuts[CutCount++] = T.Rect.B;
				const float Rows[2] = {R == 0 ? T.Rect.G : T.Transom + T.TransomHalf, R == 0 ? T.Transom - T.TransomHalf : T.Rect.A};
				for (int32 C = 0; C + 1 < CutCount; ++C)
				{
					const float W = Cuts[C + 1] - Cuts[C], H = Rows[1] - Rows[0];
					if (W < 0.05f || H < 0.05f)
					{
						continue;
					}
					UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, *FString::Printf(TEXT("MI_GlassProofPane_%d_%d"), C, R), RF_Public);
					Mic->SetParentEditorOnly(ProofPane);
					Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("PaneSize")), FLinearColor(W, H, 0.f, 0.f));
					const bool bDoor = R == 0 && (FMath::IsNearlyEqual(Cuts[C], T.Door.R, 0.02f) || FMath::IsNearlyEqual(Cuts[C], T.Door.G, 0.02f));
					Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Handle")), bDoor ? FLinearColor(T.Door.B, T.Door.A, 0.f, 0.f) : FLinearColor(-1.f, 0.f, 0.f, 0.f));
					Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Seed")), FMath::Frac(0.37f + 0.173f * C + 0.31f * R));
					Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Age")), 0.15f);
					Mic->PostEditChange();
					LotBox(PieceX + Cuts[C], PieceY + 0.181, P.z + Rows[0], PieceX + Cuts[C + 1], PieceY + 0.189, P.z + Rows[1], Mic, Data, EFTCellKind::NoCollide,
						TEXT("GlassProofPane"), 90.0);
					++PaneCount;
				}
			}
			// The bay's shell (paint, tiles and ceiling of albedo 0.3 to 0.4, so by day the bay is dim beside the street): floor,
			// ceiling with three light panels, back and side walls.
			const TCHAR* const BayTag = TEXT("GlassProofBay");
			UMaterialInterface* const WallPaint = BayMaterial(TEXT("MI_GlassProofBay_Wall"), FLinearColor(0.38f, 0.37f, 0.34f), 0.f);
			UMaterialInterface* const FloorPaint = BayMaterial(TEXT("MI_GlassProofBay_Floor"), FLinearColor(0.32f, 0.31f, 0.29f), 0.f);
			UMaterialInterface* const CeilPaint = BayMaterial(TEXT("MI_GlassProofBay_Ceiling"), FLinearColor(0.40f, 0.40f, 0.39f), 0.f);
			UMaterialInterface* const PanelPaint = BayMaterial(TEXT("MI_GlassProofBay_Panel"), FLinearColor(0.9f, 0.9f, 0.88f), 1.f);
			UMaterialInterface* const BoardPaint = BayMaterial(TEXT("MI_GlassProofBay_Board"), FLinearColor(0.36f, 0.35f, 0.33f), 0.f);
			LotBox(BayX0, BayY0, BayZ0, BayX1, BayY1, BayZ0 + 0.04, FloorPaint, Data, EFTCellKind::NoCollide, BayTag);
			LotBox(BayX0, BayY0, BayZ1 - 0.04, BayX1, BayY1, BayZ1, CeilPaint, Data, EFTCellKind::NoCollide, BayTag);
			LotBox(BayX0, BayY1 - 0.04, BayZ0, BayX1, BayY1, BayZ1, WallPaint, Data, EFTCellKind::NoCollide, BayTag);
			LotBox(BayX0, BayY0, BayZ0, BayX0 + 0.04, BayY1, BayZ1, WallPaint, Data, EFTCellKind::NoCollide, BayTag);
			LotBox(BayX1 - 0.04, BayY0, BayZ0, BayX1, BayY1, BayZ1, WallPaint, Data, EFTCellKind::NoCollide, BayTag);
			const double BayW = BayX1 - BayX0, BayMidY = 0.5 * (BayY0 + BayY1);
			for (int32 L = 0; L < 3; ++L)
			{
				const double Lx = BayX0 + BayW * (0.5 + L) / 3.0;
				LotBox(Lx - 0.6, BayMidY - 0.15, BayZ1 - 0.07, Lx + 0.6, BayMidY + 0.15, BayZ1 - 0.04, PanelPaint, Data, EFTCellKind::NoCollide, BayTag);
				// Its fixture: a rect light just under the panel, facing down, its width along the front (its local y,
				// which a yaw of -Yaw - 90 lays along the cube's x above).
				double Lpx = 0.0, Lpy = 0.0;
				Proof.ToPlan(Lx, BayMidY, Lpx, Lpy);
				const ftcore::MassingVec Le = ftcore::massingEnginePoint(Side, Lpx, Lpy, BayZ1 - 0.075);
				Build.BayLights.Add({FVector(Le.x, Le.y, Le.z) * 100.0, FRotator(-90.0, -Proof.Yaw - 90.0, 0.0), 120.f, 30.f});
			}
			// Shelving along the back wall and a free-standing run in the middle of the bay, each row's goods a card cut from
			// the shelf-front picture (a band per row, ftcore::glassGoodsRow), kept at its shape by the row's height.
			int32 GoodsCount = 0;
			const float GoodsSeed = FMath::Frac(static_cast<float>(P.seed % 9973u) * 0.000731f + 0.37f);
			const auto Shelf = [&](double X0, double X1, double Y0, double Y1, double Z, double RowH)
			{
				LotBox(X0, Y0, Z, X1, Y1, Z + 0.03, BoardPaint, Data, EFTCellKind::NoCollide, BayTag);
				const ftcore::GlassGoodsRow Row = ftcore::glassGoodsRow(GoodsCount, GoodsSeed);
				UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, *FString::Printf(TEXT("MI_GlassProofBay_Goods%d"), GoodsCount), RF_Public);
				Mic->SetParentEditorOnly(ProofBay);
				Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("GoodsMix")), 1.f);
				Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("GoodsBand")), static_cast<float>(Row.band));
				Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("GoodsStart")), Row.start);
				Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("GoodsWidthCm")),
					100.f * ftcore::kGlassGoodsWidthM * static_cast<float>(RowH) / ftcore::kGlassGoodsBandM);
				Mic->PostEditChange();
				LotBox(X0 + 0.02, Y0 + 0.08, Z + 0.03, X1 - 0.02, Y0 + 0.10, Z + 0.03 + RowH, Mic, Data, EFTCellKind::NoCollide, BayTag);
				++GoodsCount;
			};
			for (int32 R = 0; R < 5; ++R)
			{
				Shelf(BayX0 + 0.1, BayX1 - 0.1, BayY1 - 0.40, BayY1 - 0.05, BayZ0 + 0.25 + 0.42 * R, 0.39);
			}
			if (BayY1 - BayY0 > 1.6)
			{
				for (int32 S = 0; S < 2; ++S)
				{
					const double Sx0 = BayX0 + 0.6 + S * (0.5 * BayW), Sx1 = Sx0 + 0.5 * BayW - 1.3;
					LotBox(Sx0, BayMidY - 0.22, BayZ0 + 0.04, Sx1, BayMidY + 0.22, BayZ0 + 0.18, BoardPaint, Data, EFTCellKind::NoCollide, BayTag);
					for (int32 R = 0; R < 3; ++R)
					{
						Shelf(Sx0, Sx1, BayMidY - 0.2, BayMidY + 0.2, BayZ0 + 0.18 + 0.38 * R, 0.35);
					}
				}
			}
			// The shop's stock by the glass: real meshes of the local interior models (/Game/Props/Goods/<id>, local-only
			// imports; one that is missing is left out with whatever stands on it), laid out by ftcore::glassBayStock.
			ftcore::GlassBayItem Stock[ftcore::kGlassBayStockMax];
			const bool bDoor = T.Door.G > T.Door.R + 0.1f;
			const int32 StockPlanned = ftcore::glassBayStock(static_cast<float>(BayW), bDoor ? T.Door.R - 0.30f : 1.f, bDoor ? T.Door.G - 0.30f : 0.f,
				Stock, ftcore::kGlassBayStockMax);
			double StockTop[ftcore::kGlassBayStockMax];
			int32 StockCount = 0;
			for (int32 I = 0; I < StockPlanned; ++I)
			{
				StockTop[I] = -1.0;
				const ftcore::GlassBayItem& Item = Stock[I];
				if (Item.on >= 0 && StockTop[Item.on] < 0.0)
				{
					continue;
				}
				const FString Id = UTF8_TO_TCHAR(Item.id);
				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Props/Goods/%s/%s.%s"), *Id, *Id, *Id), nullptr, LOAD_NoWarn | LOAD_Quiet);
				if (!Mesh)
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld -Lots: the proof bay's stock %s isn't imported (/Game/Props/Goods/%s); left out."), *Id, *Id);
					continue;
				}
				// The import keeps the model's origin at its bottom centre (goods\stock_to_glb.py), so its top is its height.
				const FBox Box = Mesh->GetBoundingBox();
				const double Z = Item.on >= 0 ? StockTop[Item.on] : BayZ0 + 0.04;
				StockTop[I] = Z + Box.Max.Z / 100.0;
				double Px = 0.0, Py = 0.0;
				Proof.ToPlan(BayX0 + Item.x, BayY0 + Item.y, Px, Py);
				const ftcore::MassingVec E = ftcore::massingEnginePoint(Side, Px, Py, Z);
				TArray<UMaterialInterface*> StockMats;
				for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
				{
					StockMats.Add(Slot.MaterialInterface);
				}
				FFTCellOptions StockOpts;
				StockOpts.Label = Opts.Label;
				StockOpts.bCastShadow = true;
				StockOpts.Tag = BayTag;
				Cells.Add(Mesh, StockMats, FTransform(FRotator(0.0, -Proof.Yaw + Item.turn, 0.0), FVector(E.x, E.y, E.z) * 100.0), Data,
					EFTCellKind::NoCollide, StockOpts);
				++StockCount;
			}
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: the glass proof front: %d true panes, the bay's shell, %d goods on its shelves, "
				"%d of %d stock meshes; %d boxes in all."), PaneCount, GoodsCount, StockCount, StockPlanned, ProofBoxes);
			continue;
		}
		Cells.Add(Use.Mesh, Use.Materials, PieceTransform(P, Use.Fit, Side), Data,
			bSolid || Piece.id == "VENDING" ? EFTCellKind::Solid : EFTCellKind::NoCollide, Opts);
		++Pieces;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -Lots: placed %d kit pieces and %d plain boxes."), Pieces, Boxes);
	return true;
}
} // namespace FTLots
