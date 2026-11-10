#include "FTBuildDecals.h"

#include "FTCellInstancer.h"
#include "FTSceneLook.h"
#include "ForgottenThird.h"

#if WITH_EDITOR
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
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
const TCHAR* const kKindNames[ftcore::kRoadDecalKinds] = {TEXT("cracks"), TEXT("stains"), TEXT("manholes"), TEXT("patches"), TEXT("kerb grime")};

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
	// Output 0 is the colour, then these in order.
	AddCustomOutput(Custom, TEXT("RoughOut"), CMOT_Float1);
	AddCustomOutput(Custom, TEXT("MaskOut"), CMOT_Float1);
	Custom->RebuildOutputs();

	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Custom);
	Inputs->Roughness.Connect(1, Custom);
	Inputs->OpacityMask.Connect(2, Custom);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
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
	double Density = 1.0;
	if (!Wanted(Density) || Builds.empty())
	{
		return;
	}
	UStaticMesh* Plate = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Plate || !Package)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: -RoadDecals: the engine's plane mesh is missing; no decals."));
		return;
	}
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
		ftcore::roadDecals(Builds[R].solids.data(), Builds[R].solids.size(), Spec, Decals, Counts);
	}
	if (Decals.empty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -RoadDecals: no slab takes a decal."));
		return;
	}

	UMaterialInterface* const Mats[1] = {BuildMaterial(Package)};
	FFTCellOptions Options;
	Options.Cull = EFTCellCull::Prop;
	Options.bCastShadow = false;
	Options.Tag = TEXT("FTRoadDecal");
	Options.Label = TEXT("RoadDecal");
	float Data[ftcore::kRoadDecalData] = {};
	for (const ftcore::RoadDecal& D : Decals)
	{
		// The plane lies on the decal's top face: its centre over (cx, cy) at top, turned to its grade as the road's
		// cubes are (massingCubePose), 1 m stretched to its size along the slope.
		const ftcore::MassingBox Box = ftcore::roadDecalBox(D, kPoseDepth);
		const ftcore::MassingCubePose Pose = ftcore::massingCubePose(Box, kPoseDepth);
		const ftcore::MassingVec At = ftcore::massingEnginePoint(Side, D.cx, D.cy, D.top);
		const FTransform Transform(FRotator(Pose.pitch, Pose.yaw, Pose.roll), FVector(At.x, At.y, At.z) * 100.0,
			FVector(Pose.scale.x, Pose.scale.y, 1.0));
		ftcore::roadDecalData(D, Data);
		Cells.Add(Plate, Mats, Transform, TArrayView<const float>(Data, static_cast<int32>(ftcore::kRoadDecalData)), EFTCellKind::NoCollide, Options);
	}
	const double Area = Counts.asphaltM2 + Counts.pavementM2;
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -RoadDecals=%.2f: %d decals on %d slabs (%.0f m2 carriageway, %.0f m2 pavement, "
		"%.2f per 100 m2): %d %s, %d %s, %d %s, %d %s, %d %s; %d left out at the cap of %d."),
		Density, static_cast<int32>(Decals.size()), static_cast<int32>(Counts.slabs), Counts.asphaltM2, Counts.pavementM2,
		Area > 0.0 ? 100.0 * static_cast<double>(Decals.size()) / Area : 0.0,
		static_cast<int32>(Counts.perKind[0]), kKindNames[0], static_cast<int32>(Counts.perKind[1]), kKindNames[1],
		static_cast<int32>(Counts.perKind[2]), kKindNames[2], static_cast<int32>(Counts.perKind[3]), kKindNames[3],
		static_cast<int32>(Counts.perKind[4]), kKindNames[4], static_cast<int32>(Counts.capped), static_cast<int32>(Spec.maxDecals));
}

#else

void FTBuildDecals::Lay(UPackage*, FFTCellInstancer&, const std::vector<ftcore::RoadSpec>&, const std::vector<ftcore::RoadBuild>&, double)
{
}

#endif
