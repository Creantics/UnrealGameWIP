#include "FTBuildDecals.h"

#include "FTCellInstancer.h"
#include "FTSceneLook.h"
#include "ForgottenThird.h"

#if WITH_EDITOR
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "MaterialShared.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"

#include "ftcore/MassingPlan.h"
#include "ftcore/RoadDecals.h"

// Named, not anonymous: in a unity build the material commandlets' own helpers share this translation unit.
namespace FTDecalBuild
{
// The plate's depth for its pose (it has none: the plane lies on its top face).
constexpr double kPoseDepth = 0.02;
// The instance custom data's parameter names, for the custom primitive data a fallback actor carries (same slots).
const TCHAR* const kDataNames[ftcore::kRoadDecalData] = {TEXT("DecalKind"), TEXT("DecalSeed"), TEXT("DecalSizeU"), TEXT("DecalSizeW")};
const TCHAR* const kKindNames[ftcore::kRoadDecalKinds] = {TEXT("cracks"), TEXT("stains"), TEXT("manholes"), TEXT("patches"),
	TEXT("kerb grime"), TEXT("oil drips"), TEXT("tactile strips"), TEXT("drain grates"), TEXT("white lines"), TEXT("yellow lines"),
	TEXT("stop lines"), TEXT("zebra bars")};

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

void AddCustomInput(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(0, From);
}

void AddCustomOutput(UMaterialExpressionCustom* Custom, FName Name, ECustomMaterialOutputType Type)
{
	FCustomOutput& Out = Custom->AdditionalOutputs.AddDefaulted_GetRef();
	Out.OutputName = Name;
	Out.OutputType = Type;
}

// The decals' one material, M_RoadDecal in Package: masked, drawn instanced, its Custom node fed the plate's local
// position, the instance's four custom data floats (falling back to custom primitive data) and the look's Wetness (0 when
// the collection is missing).
UMaterial* BuildMaterial(UPackage* Package)
{
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_RoadDecal"), RF_Public | RF_Transactional);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Material->BlendMode = BLEND_Masked;
	Material->OpacityMaskClipValue = 0.5f;

	UMaterialExpression* Data[ftcore::kRoadDecalData] = {};
	// The per-instance node's class isn't exported from the engine's module, so it is found by name.
	UClass* const PerInstanceClass = FindObject<UClass>(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("MaterialExpressionPerInstanceCustomData")));
	for (int32 D = 0; D < static_cast<int32>(ftcore::kRoadDecalData); ++D)
	{
		UMaterialExpressionScalarParameter* Param = AddNode<UMaterialExpressionScalarParameter>(Material, -900, D * 100);
		Param->ParameterName = kDataNames[D];
		Param->DefaultValue = 0.f;
		Param->Group = TEXT("Decal");
		Param->bUseCustomPrimitiveData = true;
		Param->PrimitiveDataIndex = static_cast<uint8>(D);
		Param->UpdateParameterGuid(true, false);
		Data[D] = Param;
		if (PerInstanceClass)
		{
			UMaterialExpressionPerInstanceCustomData* PerInstance = static_cast<UMaterialExpressionPerInstanceCustomData*>(
				NewObject<UMaterialExpression>(Material, PerInstanceClass, NAME_None, RF_Transactional));
			Material->GetExpressionCollection().AddExpression(PerInstance);
			PerInstance->Material = Material;
			PerInstance->MaterialExpressionEditorX = -700;
			PerInstance->MaterialExpressionEditorY = D * 100;
			PerInstance->UpdateMaterialExpressionGuid(true, false);
			PerInstance->DataIndex = static_cast<uint32>(D);
			PerInstance->DefaultValue.Connect(0, Param);
			Data[D] = PerInstance;
		}
	}
	if (!PerInstanceClass)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: -RoadDecals: no per-instance custom data node class; the decals read custom primitive data only."));
	}

	UMaterialExpression* Wetness = nullptr;
	const FString LookPath = FString(FTScene::LookCollection) + TEXT(".") + FPackageName::GetShortName(FTScene::LookCollection);
	UMaterialParameterCollection* Look = LoadObject<UMaterialParameterCollection>(nullptr, *LookPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	const FName WetName(TEXT("Wetness"));
	if (Look && Look->GetParameterId(WetName).IsValid())
	{
		UMaterialExpressionCollectionParameter* Wet = AddNode<UMaterialExpressionCollectionParameter>(Material, -900, 500);
		Wet->Collection = Look;
		Wet->ParameterName = WetName;
		Wet->ParameterId = Look->GetParameterId(WetName);
		Wetness = Wet;
	}
	else
	{
		UMaterialExpressionConstant* Dry = AddNode<UMaterialExpressionConstant>(Material, -900, 500);
		Dry->R = 0.f;
		Wetness = Dry;
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: -RoadDecals: %s or its Wetness is missing; the decals stay dry."), *LookPath);
	}
	UMaterialExpressionLocalPosition* LocalPos = AddNode<UMaterialExpressionLocalPosition>(Material, -900, 600);
	UMaterialExpressionWorldPosition* WorldPos = AddNode<UMaterialExpressionWorldPosition>(Material, -900, 700);
	UMaterialExpressionVertexNormalWS* VertexNormal = AddNode<UMaterialExpressionVertexNormalWS>(Material, -900, 800);
	// The plate's local x and y in world space (the instance's transform): the paint's lip turns with its plate.
	const auto Axis = [Material](const FLinearColor& Local, int32 Y) {
		UMaterialExpressionConstant3Vector* Along = AddNode<UMaterialExpressionConstant3Vector>(Material, -1200, Y);
		Along->Constant = Local;
		UMaterialExpressionTransform* World = AddNode<UMaterialExpressionTransform>(Material, -900, Y);
		World->Input.Connect(0, Along);
		World->TransformSourceType = TRANSFORMSOURCE_Instance;
		World->TransformType = TRANSFORM_World;
		return World;
	};

	UMaterialExpressionCustom* Custom = AddNode<UMaterialExpressionCustom>(Material, -400, 200);
	Custom->Description = TEXT("FTRoadDecal");
	Custom->OutputType = CMOT_Float3;
	Custom->Code = FString(UTF8_TO_TCHAR(ftcore::roadDecalHlsl()));
	Custom->Inputs.Reset();
	AddCustomInput(Custom, TEXT("LocalPos"), LocalPos);
	AddCustomInput(Custom, TEXT("Kind"), Data[0]);
	AddCustomInput(Custom, TEXT("Seed"), Data[1]);
	AddCustomInput(Custom, TEXT("SizeU"), Data[2]);
	AddCustomInput(Custom, TEXT("SizeW"), Data[3]);
	AddCustomInput(Custom, TEXT("Wetness"), Wetness);
	AddCustomInput(Custom, TEXT("WorldPos"), WorldPos);
	AddCustomInput(Custom, TEXT("VN"), VertexNormal);
	AddCustomInput(Custom, TEXT("AxisX"), Axis(FLinearColor(1.f, 0.f, 0.f), 900));
	AddCustomInput(Custom, TEXT("AxisY"), Axis(FLinearColor(0.f, 1.f, 0.f), 1000));
	// Output 0 is the colour, then these in order.
	AddCustomOutput(Custom, TEXT("RoughOut"), CMOT_Float1);
	AddCustomOutput(Custom, TEXT("MaskOut"), CMOT_Float1);
	AddCustomOutput(Custom, TEXT("NormalOut"), CMOT_Float3);
	Custom->RebuildOutputs();

	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Custom);
	Inputs->Roughness.Connect(1, Custom);
	Inputs->OpacityMask.Connect(2, Custom);
	// The normal is world space (the wear kinds pass the vertex normal through).
	Material->bTangentSpaceNormal = false;
	Inputs->Normal.Connect(3, Custom);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

// Stamps the surface master's "Street" scalar (its street layer: asphalt, pavement, kerb, marking) on the road tints'
// instances the roads made in Package (MI_Road_<tint>), so they texture without -RoadDecals too.
int32 StampStreetKinds(UPackage* Package)
{
	int32 Stamped = 0;
	for (int32 T = 0; T < static_cast<int32>(ftcore::RoadTint::Count); ++T)
	{
		const ftcore::RoadTint Which = static_cast<ftcore::RoadTint>(T);
		const int Kind = ftcore::roadStreetKind(Which);
		if (Kind <= 0)
		{
			continue;
		}
		UMaterialInstanceConstant* Mic = FindObject<UMaterialInstanceConstant>(Package,
			*FString::Printf(TEXT("MI_Road_%s"), UTF8_TO_TCHAR(ftcore::roadTintName(Which))));
		if (Mic)
		{
			Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Street")), static_cast<float>(Kind));
			Mic->PostEditChange();
			++Stamped;
		}
	}
	return Stamped;
}

bool Wanted(double& Density)
{
	const TCHAR* Cmd = FCommandLine::Get();
	Density = 1.0;
	if (FParse::Value(Cmd, TEXT("RoadDecals="), Density))
	{
		return Density > 0.0;
	}
	return FParse::Param(Cmd, TEXT("RoadDecals"));
}
} // namespace FTDecalBuild

void FTBuildDecals::Lay(UPackage* Package, FFTCellInstancer& Cells, const std::vector<ftcore::RoadSpec>& Roads,
	const std::vector<ftcore::RoadBuild>& Builds, double Side)
{
	using namespace FTDecalBuild;
	if (Package)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: road tints given a street layer: %d."), StampStreetKinds(Package));
	}
	if (Builds.empty())
	{
		return;
	}
	if (!Package)
	{
		// FTBuildRoads lays no marking boxes, so without this the roads would have no lines at all.
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: no package for the road paint; %d roads are laid without their markings."),
			static_cast<int32>(Builds.size()));
		return;
	}
	double Density = 1.0;
	const bool bWear = Wanted(Density);
	// The paint always: every marking box (FTBuildRoads lays none of them) as a flat plate on the road.
	std::vector<ftcore::RoadDecal> Paint;
	ftcore::RoadDecalCounts PaintCounts;
	ftcore::RoadDecalSpec Spec;
	Spec.density = Density;
	std::vector<ftcore::RoadDecal> Decals;
	ftcore::RoadDecalCounts Counts;
	for (size_t R = 0; R < Builds.size(); ++R)
	{
		if (R > 0 && R < Roads.size() && Roads[R].kind == "net")
		{
			continue; // a network street's stand-in: the network is all in Builds[0]
		}
		ftcore::roadMarkingDecals(Builds[R], Paint, PaintCounts);
		if (bWear)
		{
			ftcore::roadDecals(Builds[R], Spec, Decals, Counts);
		}
	}
	if (Paint.empty() && Decals.empty())
	{
		if (bWear)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -RoadDecals: no slab takes a decal, and no road has a marking."));
		}
		return;
	}
	UStaticMesh* Plate = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Plate)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: the engine's plane mesh is missing; no road paint (%d markings) and no decals."),
			static_cast<int32>(Paint.size()));
		return;
	}

	UMaterialInterface* const Mats[1] = {BuildMaterial(Package)};
	float Data[ftcore::kRoadDecalData] = {};
	const auto Place = [&](const ftcore::RoadDecal& D, const FFTCellOptions& Options) {
		// The plane lies on the decal's top face: its centre over (cx, cy) at top, turned to its grade as the road's
		// cubes are (massingCubePose), 1 m stretched to its size along the slope.
		const ftcore::MassingBox Box = ftcore::roadDecalBox(D, kPoseDepth);
		const ftcore::MassingCubePose Pose = ftcore::massingCubePose(Box, kPoseDepth);
		const ftcore::MassingVec At = ftcore::massingEnginePoint(Side, D.cx, D.cy, D.top);
		const FTransform Transform(FRotator(Pose.pitch, Pose.yaw, Pose.roll), FVector(At.x, At.y, At.z) * 100.0,
			FVector(Pose.scale.x, Pose.scale.y, 1.0));
		ftcore::roadDecalData(D, Data);
		Cells.Add(Plate, Mats, Transform, TArrayView<const float>(Data, static_cast<int32>(ftcore::kRoadDecalData)), EFTCellKind::NoCollide, Options);
	};
	// Paint is drawn at any distance and stays in the ray-traced scene (a reflected road shows its lines), as the boxes
	// were; flat, it casts no shadow.
	FFTCellOptions PaintOptions;
	PaintOptions.Cull = EFTCellCull::None;
	PaintOptions.bCastShadow = false;
	PaintOptions.bRayTraced = true;
	PaintOptions.Tag = TEXT("FTRoadPaint");
	PaintOptions.Label = TEXT("RoadPaint");
	for (const ftcore::RoadDecal& D : Paint)
	{
		Place(D, PaintOptions);
	}
	using ftcore::RoadDecalKind;
	const auto PaintOf = [&PaintCounts](RoadDecalKind K) { return static_cast<int32>(PaintCounts.perKind[static_cast<size_t>(K)]); };
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: road paint: %d flat plates (%d triangles; as boxes they were %d): %d %s, %d %s, "
		"%d %s, %d %s. No arrows or words: the roads paint none."),
		static_cast<int32>(Paint.size()), static_cast<int32>(Paint.size()) * 2, static_cast<int32>(Paint.size()) * 12,
		PaintOf(RoadDecalKind::LineWhite), kKindNames[static_cast<size_t>(RoadDecalKind::LineWhite)],
		PaintOf(RoadDecalKind::LineYellow), kKindNames[static_cast<size_t>(RoadDecalKind::LineYellow)],
		PaintOf(RoadDecalKind::StopLine), kKindNames[static_cast<size_t>(RoadDecalKind::StopLine)],
		PaintOf(RoadDecalKind::ZebraBar), kKindNames[static_cast<size_t>(RoadDecalKind::ZebraBar)]);
	if (!bWear)
	{
		return;
	}
	if (Decals.empty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -RoadDecals: no slab takes a decal."));
		return;
	}
	FFTCellOptions Options;
	Options.Cull = EFTCellCull::Prop;
	Options.bCastShadow = false;
	Options.Tag = TEXT("FTRoadDecal");
	Options.Label = TEXT("RoadDecal");
	for (const ftcore::RoadDecal& D : Decals)
	{
		Place(D, Options);
	}
	const double Area = Counts.asphaltM2 + Counts.pavementM2;
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -RoadDecals=%.2f: %d decals on %d slabs (%.0f m2 carriageway, %.0f m2 pavement, "
		"%.2f per 100 m2): %d %s, %d %s, %d %s, %d %s, %d %s, %d %s, %d %s, %d %s; %d left out at the cap of %d."),
		Density, static_cast<int32>(Decals.size()), static_cast<int32>(Counts.slabs), Counts.asphaltM2, Counts.pavementM2,
		Area > 0.0 ? 100.0 * static_cast<double>(Decals.size()) / Area : 0.0,
		static_cast<int32>(Counts.perKind[0]), kKindNames[0], static_cast<int32>(Counts.perKind[1]), kKindNames[1],
		static_cast<int32>(Counts.perKind[2]), kKindNames[2], static_cast<int32>(Counts.perKind[3]), kKindNames[3],
		static_cast<int32>(Counts.perKind[4]), kKindNames[4], static_cast<int32>(Counts.perKind[5]), kKindNames[5],
		static_cast<int32>(Counts.perKind[6]), kKindNames[6], static_cast<int32>(Counts.perKind[7]), kKindNames[7],
		static_cast<int32>(Counts.capped), static_cast<int32>(Spec.maxDecals));
}

#else

void FTBuildDecals::Lay(UPackage*, FFTCellInstancer&, const std::vector<ftcore::RoadSpec>&, const std::vector<ftcore::RoadBuild>&, double)
{
}

#endif
