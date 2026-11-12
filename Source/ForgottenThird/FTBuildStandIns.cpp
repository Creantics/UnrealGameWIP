#include "FTBuildStandIns.h"

#include "Engine/StaticMesh.h"
#include "FTCellInstancer.h"
#include "ForgottenThird.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "StaticMeshAttributes.h"

#include "ftcore/MassingPlan.h"
#include "ftcore/StandIns.h"
#include "ftcore/TreeMeshes.h"
#include "ftcore/LawnSheet.h"
#include "Misc/Paths.h"

#include <string>
#include <string_view>
#include <vector>

// Named, not anonymous: the module builds in unity files.
namespace FTStandIns::Impl
{
constexpr int32 kKinds = static_cast<int32>(ftcore::kStandInKinds);
constexpr int32 kShapes = static_cast<int32>(ftcore::StandInShape::Count);
constexpr int32 kPaints = static_cast<int32>(ftcore::kStandInPaints);
constexpr int32 kCanopies = static_cast<int32>(ftcore::kStandInCanopyVariants);

struct FSwitch
{
	const TCHAR* Name;
	ftcore::StandInKind Kind; // Count: the table's kind column is required
};
const FSwitch kSwitches[] = {
	{TEXT("StandIns="), ftcore::StandInKind::Count},
	{TEXT("StandInTrees="), ftcore::StandInKind::StreetTree},
	{TEXT("StandInHouses="), ftcore::StandInKind::House},
	{TEXT("StandInTowers="), ftcore::StandInKind::Count},
	{TEXT("QuayPoints="), ftcore::StandInKind::Count},
};

// Each paint's colour and roughness, in ftcore::StandInPaint's order.
struct FPaint
{
	const TCHAR* Name;
	FLinearColor Color;
	float Roughness;
};
const FPaint kPaintLook[] = {
	{TEXT("MI_StandIn_Bark"), FLinearColor(0.06f, 0.045f, 0.03f), 0.9f},
	// The four leaf greens (round 10: lighter; round 9's read black in shade): mid, fresh light, deep blue-green
	// (still lighter than round 9's mid) and, last in the list, yellow-olive.
	{TEXT("MI_StandIn_Leaf"), FLinearColor(0.085f, 0.16f, 0.04f), 0.8f},
	{TEXT("MI_StandIn_LeafLight"), FLinearColor(0.15f, 0.24f, 0.055f), 0.78f},
	{TEXT("MI_StandIn_LeafDark"), FLinearColor(0.05f, 0.11f, 0.06f), 0.82f},
	{TEXT("MI_StandIn_Steel"), FLinearColor(0.20f, 0.21f, 0.22f), 0.45f},
	{TEXT("MI_StandIn_LampHead"), FLinearColor(0.70f, 0.68f, 0.60f), 0.3f},
	{TEXT("MI_StandIn_Deck"), FLinearColor(0.32f, 0.31f, 0.29f), 0.85f},
	{TEXT("MI_StandIn_Concrete"), FLinearColor(0.42f, 0.41f, 0.39f), 0.85f},
	{TEXT("MI_StandIn_Glass"), FLinearColor(0.05f, 0.08f, 0.10f), 0.08f},
	{TEXT("MI_StandIn_GlassDark"), FLinearColor(0.03f, 0.045f, 0.06f), 0.06f},
	{TEXT("MI_StandIn_Spandrel"), FLinearColor(0.14f, 0.15f, 0.16f), 0.5f},
	{TEXT("MI_StandIn_RoofTile"), FLinearColor(0.18f, 0.07f, 0.05f), 0.7f},
	{TEXT("MI_StandIn_RoofSlate"), FLinearColor(0.08f, 0.08f, 0.09f), 0.6f},
	{TEXT("MI_StandIn_RoofGlazed"), FLinearColor(0.07f, 0.12f, 0.11f), 0.35f},
	{TEXT("MI_StandIn_Shell"), FLinearColor(0.55f, 0.55f, 0.53f), 0.35f},
	{TEXT("MI_StandIn_Lantern"), FLinearColor(0.75f, 0.45f, 0.15f), 0.4f},
	{TEXT("MI_StandIn_LeafWarm"), FLinearColor(0.15f, 0.17f, 0.05f), 0.8f},
};
static_assert(UE_ARRAY_COUNT(kPaintLook) == static_cast<SIZE_T>(kPaints), "a look for every stand-in paint");

// The engine's basic shapes; the canopies and the gable (the last two shapes) are built here (BuiltMesh).
constexpr int32 kBasicShapes = static_cast<int32>(ftcore::StandInShape::Canopy);
const TCHAR* const kShapePaths[kBasicShapes] = {
	TEXT("/Engine/BasicShapes/Cube.Cube"),
	TEXT("/Engine/BasicShapes/Sphere.Sphere"),
	TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),
};
static_assert(kBasicShapes + 2 == kShapes && static_cast<int32>(ftcore::StandInShape::Gable) == kBasicShapes + 1,
	"every shape but the canopy and the gable is a basic shape");

// A mesh of the stand-ins' own (ftcore::standInCanopyMesh, standInGableMesh) in Package: the unit box as a metre cube
// (100 cm a side) centred on the origin (a canopy's fork at the origin's x and y, its foot at z -50); slot 0 is
// Materials[0] (a canopy's leaves, the gable's roof), slot 1, where Materials has two, Materials[1] (the limbs' bark).
// Shaded round by the source's vertex normals where it has them (a canopy's clumps and limbs; no more triangles), flat
// by its triangles' where it has none (the gable). No collision (a tree's trunk cylinder collides), no lightmap channel
// (lit by the look's dynamic lights, as the shapes are). Its triangle count in Triangles.
UStaticMesh* BuiltMesh(UPackage* Package, const FString& Name, const ftcore::StandInCanopyMesh& Source,
	TArrayView<UMaterialInterface* const> Materials, int32& Triangles)
{
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, *Name, RF_Public);
	FMeshDescription Desc;
	FStaticMeshAttributes Attributes(Desc);
	Attributes.Register();
	const FName SlotNames[2] = {FName(TEXT("Leaf")), FName(TEXT("Bark"))};
	const int32 SlotCount = FMath::Clamp(Materials.Num(), 1, 2);
	const bool bSmooth = Source.vertexNormals.size() == Source.positions.size();
	FPolygonGroupID Groups[2];
	for (int32 G = 0; G < SlotCount; ++G)
	{
		Groups[G] = Desc.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Groups[G]] = SlotNames[G];
	}
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> Uvs = Attributes.GetVertexInstanceUVs();
	const int32 VertexCount = static_cast<int32>(Source.positions.size() / 3);
	TArray<FVertexID> Verts;
	Verts.Reserve(VertexCount);
	for (int32 I = 0; I < VertexCount; ++I)
	{
		const FVertexID V = Desc.CreateVertex();
		Positions[V] = FVector3f(Source.positions[3 * I], Source.positions[3 * I + 1], Source.positions[3 * I + 2]) * 100.f;
		Verts.Add(V);
	}
	Triangles = static_cast<int32>(Source.slots.size());
	for (int32 T = 0; T < Triangles; ++T)
	{
		const FVector3f Out(Source.normals[3 * T], Source.normals[3 * T + 1], Source.normals[3 * T + 2]);
		int32 Index[3] = {static_cast<int32>(Source.triangles[3 * T]), static_cast<int32>(Source.triangles[3 * T + 1]),
			static_cast<int32>(Source.triangles[3 * T + 2])};
		// The engine winds a face so its first triangle's cross product points in (as the massing build's sloped boxes).
		const FVector3f A = Positions[Verts[Index[0]]];
		const FVector3f B = Positions[Verts[Index[1]]];
		const FVector3f C = Positions[Verts[Index[2]]];
		if (FVector3f::DotProduct(FVector3f::CrossProduct(B - A, C - A), Out) > 0.f)
		{
			Swap(Index[1], Index[2]);
		}
		FVertexInstanceID Corners[3];
		for (int32 K = 0; K < 3; ++K)
		{
			const FVertexInstanceID Inst = Desc.CreateVertexInstance(Verts[Index[K]]);
			Normals[Inst] = bSmooth
				? FVector3f(Source.vertexNormals[3 * Index[K]], Source.vertexNormals[3 * Index[K] + 1], Source.vertexNormals[3 * Index[K] + 2])
				: Out;
			const FVector3f P = Positions[Verts[Index[K]]];
			Uvs[Inst] = FVector2f(P.X, P.Y) / 100.f;
			Corners[K] = Inst;
		}
		Desc.CreatePolygon(Groups[Source.slots[T] == 0 || SlotCount < 2 ? 0 : 1], TArrayView<const FVertexInstanceID>(Corners, 3));
	}
	for (int32 G = 0; G < SlotCount; ++G)
	{
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Materials[G], SlotNames[G], SlotNames[G]));
	}
	FStaticMeshSourceModel& Model = Mesh->AddSourceModel();
	Model.BuildSettings.bRecomputeNormals = false;
	Model.BuildSettings.bRecomputeTangents = true;
	Model.BuildSettings.bGenerateLightmapUVs = false;
	Mesh->CreateMeshDescription(0, MoveTemp(Desc));
	Mesh->CommitMeshDescription(0);
	Mesh->CreateBodySetup(); // as the sloped boxes' meshes; these meshes' cell groups take no collision
	Mesh->Build(true);
	Mesh->PostEditChange();
	return Mesh;
}

// How far each kind is drawn and whether it casts a shadow: trees, roofs, towers and bridges at any distance (they make
// the aerial and the skyline); quay furniture as props; shrubs as props without a shadow.
EFTCellCull CullOf(ftcore::StandInKind Kind)
{
	switch (Kind)
	{
	case ftcore::StandInKind::Shrub:
	case ftcore::StandInKind::Railing:
	case ftcore::StandInKind::LampMast:
		return EFTCellCull::Prop;
	default:
		return EFTCellCull::None;
	}
}

bool ShadowOf(ftcore::StandInKind Kind)
{
	return Kind != ftcore::StandInKind::Shrub && Kind != ftcore::StandInKind::Railing;
}

EFTCellKind CellKindOf(ftcore::StandInCollide Collide)
{
	switch (Collide)
	{
	case ftcore::StandInCollide::Solid:
		return EFTCellKind::Solid;
	case ftcore::StandInCollide::Walk:
		return EFTCellKind::Walk;
	default:
		return EFTCellKind::NoCollide;
	}
}

// The tables a switch names, read and appended to Points. False (logged) on a file that doesn't read or parse.
bool LoadTables(const FSwitch& Switch, std::vector<ftcore::StandInPoint>& Points, int32& Tables)
{
	FString Value;
	if (!FParse::Value(FCommandLine::Get(), Switch.Name, Value, false) || Value.IsEmpty())
	{
		return true;
	}
	TArray<FString> Files;
	Value.ParseIntoArray(Files, TEXT(";"), true);
	for (FString File : Files)
	{
		File.TrimStartAndEndInline();
		TArray<uint8> Bytes;
		if (File.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *File))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("StandIns: -%s couldn't read '%s'."), Switch.Name, *File);
			return false;
		}
		std::string Error;
		ftcore::StandInImportStats Stats;
		const std::string_view Csv(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
		if (!ftcore::importStandIns(Csv, Switch.Kind, Points, &Error, &Stats))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("StandIns: %s: %s"), *File, UTF8_TO_TCHAR(Error.c_str()));
			return false;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("StandIns: -%s%s: %d rows, %d placed, %d of kinds not placed here."), Switch.Name, *File,
			static_cast<int32>(Stats.rows), static_cast<int32>(Stats.kept), static_cast<int32>(Stats.skipped));
		++Tables;
	}
	return true;
}

// The corridor rule (ftcore::standInCorridorCheck): no part of a stand-in stands in the clear way of a street of
// -Streets or a road of -Roads (a canal's row left out). Every part found in one is logged with its row's id, the street
// and how far in it reaches; false then, unless -StandInCorridorWarn makes them warnings. True, with nothing checked,
// without either table. Field: the ground the rows stand over.
bool CheckCorridors(const std::vector<ftcore::StandInPoint>& Points, const ftcore::HeightField& Field)
{
	std::vector<ftcore::StandInCorridor> Corridors;
	std::vector<std::string> Streets;
	const TCHAR* const Names[] = {TEXT("Streets="), TEXT("Roads=")};
	for (const TCHAR* const Name : Names)
	{
		FString File;
		if (!FParse::Value(FCommandLine::Get(), Name, File, false) || File.IsEmpty())
		{
			continue;
		}
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("StandIns: corridors: couldn't read -%s'%s'."), Name, *File);
			return false;
		}
		std::string Error;
		const std::string_view Csv(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
		if (!ftcore::importStandInCorridors(Csv, Corridors, &Streets, "canal", &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("StandIns: corridors: %s: %s"), *File, UTF8_TO_TCHAR(Error.c_str()));
			return false;
		}
	}
	if (Corridors.empty() || Points.empty())
	{
		return true;
	}
	std::vector<double> Foot(Points.size(), 0.0);
	for (size_t I = 0; I < Points.size(); ++I)
	{
		Foot[I] = Points[I].hasZ && Field.grid ? Points[I].z - Field.at(Points[I].x, Points[I].y) : 0.0;
	}
	constexpr int32 kListed = 40;
	ftcore::StandInCorridorHit Hits[kListed];
	const ftcore::StandInCorridorSpec Spec;
	const int32 Found = static_cast<int32>(ftcore::standInCorridorCheck(Points.data(), Foot.data(), Points.size(), Corridors.data(),
		Corridors.size(), Spec, Hits, static_cast<size_t>(kListed)));
	const bool bWarn = FParse::Param(FCommandLine::Get(), TEXT("StandInCorridorWarn"));
	for (int32 H = 0; H < FMath::Min(Found, kListed); ++H)
	{
		const ftcore::StandInPoint& Point = Points[Hits[H].point];
		const FString Line = FString::Printf(TEXT("StandIns: corridors: %s '%s' at %.1f, %.1f: part %d reaches %.2f m into %s."),
			UTF8_TO_TCHAR(ftcore::standInKindName(Point.kind)), UTF8_TO_TCHAR(Point.id.c_str()), Point.x, Point.y, static_cast<int32>(Hits[H].part),
			Hits[H].depth, UTF8_TO_TCHAR(Streets[Hits[H].corridor].c_str()));
		if (bWarn)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s"), *Line);
		}
	}
	if (Found > 0 && !bWarn)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("StandIns: corridors: FAILED: %d stand-in parts stand in a street's clear way (%d listed; under %.1f m "
			"over the ground, over %.2f m in). Move or drop the rows, or pass -StandInCorridorWarn."), Found, FMath::Min(Found, kListed),
			Spec.clearance, Spec.slack);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("StandIns: corridors: %d points against %d street segments: %d parts in a clear way%s."),
		static_cast<int32>(Points.size()), static_cast<int32>(Corridors.size()), Found, Found > 0 ? TEXT(" (warned, not failed)") : TEXT(""));
	return true;
}
// A plant's mesh by asset path (/Game/...; the object name added when missing) or by name (FTImportPlant's
// /Game/Props/<name>/<name>). Null, quietly, when it doesn't load.
UStaticMesh* LoadPlant(const FString& Asset)
{
	FString Path = Asset;
	if (!Path.StartsWith(TEXT("/")))
	{
		Path = FString::Printf(TEXT("/Game/Props/%s/%s"), *Asset, *Asset);
	}
	if (!Path.Contains(TEXT(".")))
	{
		Path += TEXT(".") + FPaths::GetBaseFilename(Path);
	}
	return LoadObject<UStaticMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

// The modelled trees of -TreeMeshes=<table> (ftcore/TreeMeshes.h), each model's height and crown filled from its
// mesh's bounds, and the tree pit's grate (-TreeGrate=<asset>, default TREE-grate; none when it doesn't load). True
// with no models without the switch; false (logged) on a table that doesn't read or a model that doesn't load.
struct FTreeMeshes
{
	std::vector<ftcore::TreeModel> Models;
	TArray<UStaticMesh*> Meshes;
	UStaticMesh* Grate = nullptr;
};
bool LoadTreeMeshes(FTreeMeshes& Out)
{
	FString File;
	if (!FParse::Value(FCommandLine::Get(), TEXT("TreeMeshes="), File, false) || File.IsEmpty())
	{
		return true;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("StandIns: -TreeMeshes couldn't read '%s'."), *File);
		return false;
	}
	std::string Error;
	const std::string_view Csv(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
	if (!ftcore::parseTreeModels(Csv, Out.Models, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("StandIns: -TreeMeshes %s: %s"), *File, UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	for (ftcore::TreeModel& Model : Out.Models)
	{
		const FString Asset = UTF8_TO_TCHAR(Model.asset.c_str());
		UStaticMesh* Mesh = LoadPlant(Asset);
		if (!Mesh)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("StandIns: -TreeMeshes: couldn't load %s (import it with FTImportPlant)."), *Asset);
			return false;
		}
		const FBox Bounds = Mesh->GetBoundingBox();
		Model.height = FMath::Max(Bounds.Max.Z, 1.0) / 100.0;
		Model.crown = FMath::Max(Bounds.GetSize().X, Bounds.GetSize().Y) / 100.0;
		Out.Meshes.Add(Mesh);
		UE_LOG(LogForgottenThird, Display, TEXT("StandIns: tree model %s for %s rows, weight %.2f: %.1f m tall, %.1f m crown, %d LODs, %d triangles at LOD0."),
			*Asset, UTF8_TO_TCHAR(Model.kind.c_str()), Model.weight, Model.height, Model.crown, Mesh->GetNumLODs(), Mesh->GetNumTriangles(0));
	}
	FString GrateAsset = TEXT("TREE-grate");
	FParse::Value(FCommandLine::Get(), TEXT("TreeGrate="), GrateAsset, false);
	Out.Grate = GrateAsset.IsEmpty() ? nullptr : LoadPlant(GrateAsset);
	if (!Out.Grate)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("StandIns: no tree grate (%s doesn't load): street trees stand without one."), *GrateAsset);
	}
	return true;
}
} // namespace FTStandIns::Impl

bool FTStandIns::Place(UPackage* Package, FFTCellInstancer& Cells, UMaterialInterface* Base, const ftcore::HeightField& Field, double Side)
{
	using namespace FTStandIns::Impl;
	std::vector<ftcore::StandInPoint> Points;
	int32 Tables = 0;
	for (const FSwitch& Switch : kSwitches)
	{
		if (!LoadTables(Switch, Points, Tables))
		{
			return false;
		}
	}
	if (Tables == 0)
	{
		return true;
	}
	if (!CheckCorridors(Points, Field))
	{
		return false;
	}
	if (!Base)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("StandIns: no surface material to paint the stand-ins with."));
		return false;
	}

	// The basic shapes, each part's size taken over its own bounds so its centre lands where the part says.
	UStaticMesh* Shapes[kShapes] = {};
	FVector ShapeSize[kShapes], ShapeCentre[kShapes];
	for (int32 S = 0; S < kBasicShapes; ++S)
	{
		Shapes[S] = LoadObject<UStaticMesh>(nullptr, kShapePaths[S], nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Shapes[S])
		{
			UE_LOG(LogForgottenThird, Error, TEXT("StandIns: couldn't load %s."), kShapePaths[S]);
			return false;
		}
		const FBox Bounds = Shapes[S]->GetBoundingBox();
		ShapeSize[S] = Bounds.GetSize().ComponentMax(FVector(1.0));
		ShapeCentre[S] = Bounds.GetCenter();
	}
	UMaterialInterface* Paints[kPaints] = {};
	for (int32 P = 0; P < kPaints; ++P)
	{
		UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, kPaintLook[P].Name, RF_Public);
		Mic->SetParentEditorOnly(Base);
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), kPaintLook[P].Color);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), kPaintLook[P].Roughness);
		Mic->PostEditChange();
		Paints[P] = Mic;
	}
	// The canopies: sized over the unit box round their fork (not over their bounds, which lean to one side), so a
	// canopy part's x and y are its fork's, on the trunk.
	UStaticMesh* Canopies[kCanopies] = {};
	int32 CanopyTriangles[kCanopies] = {};
	constexpr int32 CanopyShape = static_cast<int32>(ftcore::StandInShape::Canopy);
	ShapeSize[CanopyShape] = FVector(100.0);
	ShapeCentre[CanopyShape] = FVector::ZeroVector;
	for (int32 V = 0; V < kCanopies; ++V)
	{
		ftcore::StandInCanopyMesh Source;
		if (!ftcore::standInCanopyMesh(static_cast<std::size_t>(V), Source))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("StandIns: no canopy mesh %d."), V);
			return false;
		}
		UMaterialInterface* const LeafBark[] = {Paints[static_cast<int32>(ftcore::StandInPaint::Leaf)],
			Paints[static_cast<int32>(ftcore::StandInPaint::Bark)]};
		Canopies[V] = BuiltMesh(Package, FString::Printf(TEXT("SM_StandIn_Canopy%d"), V), Source, LeafBark, CanopyTriangles[V]);
	}
	Shapes[CanopyShape] = Canopies[0];
	// The gable: a roof's prism, one instance a back house's roof and each house's gable infill.
	constexpr int32 GableShape = static_cast<int32>(ftcore::StandInShape::Gable);
	ShapeSize[GableShape] = FVector(100.0);
	ShapeCentre[GableShape] = FVector::ZeroVector;
	int32 GableTriangles = 0;
	{
		ftcore::StandInCanopyMesh Source;
		ftcore::standInGableMesh(Source);
		UMaterialInterface* const Roof[] = {Paints[static_cast<int32>(ftcore::StandInPaint::RoofTile)]};
		Shapes[GableShape] = BuiltMesh(Package, TEXT("SM_StandIn_Gable"), Source, Roof, GableTriangles);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("StandIns: canopies of %d, %d, %d triangles (one instance a tree, over its trunk; round-shaded), the gable %d; "
		"the basic sphere %d, cylinder %d."), CanopyTriangles[0], CanopyTriangles[1], CanopyTriangles[2], GableTriangles,
		Shapes[static_cast<int32>(ftcore::StandInShape::Sphere)]->GetNumTriangles(0),
		Shapes[static_cast<int32>(ftcore::StandInShape::Cylinder)]->GetNumTriangles(0));
	FFTCellOptions Options[kKinds];
	for (int32 K = 0; K < kKinds; ++K)
	{
		const ftcore::StandInKind Kind = static_cast<ftcore::StandInKind>(K);
		Options[K].Cull = CullOf(Kind);
		Options[K].bCastShadow = ShadowOf(Kind);
		Options[K].Tag = FName(*FString::Printf(TEXT("StandIn_%s"), UTF8_TO_TCHAR(ftcore::standInKindName(Kind))));
	}

	// The census: points and instances per kind, in all and per cell.
	struct FCount
	{
		int32 Points[kKinds] = {};
		int32 Instances[kKinds] = {};
	};
	FCount All;
	TMap<FIntPoint, FCount> PerCell;
	const TArrayView<const float> NoData;
	ftcore::StandInPart Parts[ftcore::kStandInPartsMax];

	// The modelled trees, in place of the stand-in trees' parts when -TreeMeshes is given.
	FTreeMeshes Trees;
	if (!LoadTreeMeshes(Trees))
	{
		return false;
	}
	FFTCellOptions TreeOptions;
	TreeOptions.Cull = EFTCellCull::Tree;
	TreeOptions.Tag = TEXT("Tree");
	FFTCellOptions GrateOptions;
	GrateOptions.Cull = EFTCellCull::Prop;
	GrateOptions.bCastShadow = false;
	GrateOptions.bRayTraced = true;
	GrateOptions.Tag = TEXT("TreeGrate");
	constexpr double kGrateRise = 1.5; // cm over the footway, so the grate never flickers in it
	TArray<int32> PerModel;
	PerModel.SetNumZeroed(static_cast<int32>(Trees.Models.size()));
	int32 Clamped = 0, Grates = 0, KeptStandIns = 0;
	int64 TreeTriangles = 0;

	for (const ftcore::StandInPoint& Point : Points)
	{
		const int32 K = static_cast<int32>(Point.kind);
		const double Foot = Point.hasZ ? Point.z : (Field.grid ? Field.at(Point.x, Point.y) : 0.0);
		const ftcore::MassingVec At = ftcore::massingEnginePoint(Side, Point.x, Point.y, Foot);
		const FIntPoint Cell(FMath::FloorToInt32(At.x * 100.0 / FTCells::CellSize), FMath::FloorToInt32(At.y * 100.0 / FTCells::CellSize));
		FCount& Here = PerCell.FindOrAdd(Cell);
		const bool bTree = Point.kind == ftcore::StandInKind::StreetTree || Point.kind == ftcore::StandInKind::ParkTree;
		if (bTree && !Trees.Models.empty())
		{
			const ftcore::TreePick Pick = ftcore::pickTreeModel(Point, Trees.Models.data(), Trees.Models.size());
			if (Pick.model >= 0)
			{
				UStaticMesh* const Mesh = Trees.Meshes[Pick.model];
				const FVector Foot3 = FVector(At.x, At.y, At.z) * 100.0;
				const FRotator Yaw(0.0, ftcore::massingEngineYaw(static_cast<float>(Pick.yawDeg)), 0.0);
				Cells.Add(Mesh, TArrayView<UMaterialInterface* const>(), FTransform(Yaw, Foot3, FVector(Pick.scaleXY, Pick.scaleXY, Pick.scaleZ)), NoData, EFTCellKind::Solid,
					TreeOptions);
				int32 Count = 1;
				// A street row's tree stands in a pit, its grate flush with the footway, square to the row.
				if (Point.kind == ftcore::StandInKind::StreetTree && Trees.Grate)
				{
					const FRotator Square(0.0, ftcore::massingEngineYaw(static_cast<float>(Point.rot)), 0.0);
					Cells.Add(Trees.Grate, TArrayView<UMaterialInterface* const>(), FTransform(Square, Foot3 + FVector(0.0, 0.0, kGrateRise)), NoData, EFTCellKind::NoCollide,
						GrateOptions);
					++Grates;
					++Count;
				}
				++PerModel[Pick.model];
				Clamped += Pick.clamped ? 1 : 0;
				TreeTriangles += Mesh->GetNumTriangles(0);
				++All.Points[K];
				++Here.Points[K];
				All.Instances[K] += Count;
				Here.Instances[K] += Count;
				continue;
			}
			++KeptStandIns;
		}
		const std::size_t N = ftcore::standInParts(Point, Parts);
		for (std::size_t I = 0; I < N; ++I)
		{
			const ftcore::StandInPart& Part = Parts[I];
			const int32 S = static_cast<int32>(Part.shape);
			const bool bCanopy = S == CanopyShape;
			UStaticMesh* const Mesh = bCanopy ? Canopies[FMath::Min<int32>(Part.variant, kCanopies - 1)] : Shapes[S];
			const ftcore::MassingVec C = ftcore::massingEnginePoint(Side, Point.x + Part.dx, Point.y + Part.dy, Foot + Part.dz);
			const FRotator Rot(Part.pitch, ftcore::massingEngineYaw(Part.yaw), Part.roll);
			const FVector Scale = FVector(Part.sx, Part.sy, Part.sz) * 100.0 / ShapeSize[S];
			// The mesh's own bounds' centre, scaled and turned, taken off so the part's centre is C.
			const FVector Location = FVector(C.x, C.y, C.z) * 100.0 - Rot.RotateVector(ShapeCentre[S] * Scale);
			// A canopy's second slot is its limbs' bark.
			UMaterialInterface* const M[] = {Paints[static_cast<int32>(Part.paint)], Paints[static_cast<int32>(ftcore::StandInPaint::Bark)]};
			Cells.Add(Mesh, TArrayView<UMaterialInterface* const>(M, bCanopy ? 2 : 1), FTransform(Rot, Location, Scale), NoData,
				CellKindOf(Part.collide), Options[K]);
		}
		++All.Points[K];
		++Here.Points[K];
		All.Instances[K] += static_cast<int32>(N);
		Here.Instances[K] += static_cast<int32>(N);
	}

	const auto Line = [](const FCount& Count) {
		FString Text;
		int32 Total = 0;
		for (int32 K = 0; K < kKinds; ++K)
		{
			if (Count.Points[K] > 0)
			{
				Text += FString::Printf(TEXT("%s%s %d (%d instances)"), Text.IsEmpty() ? TEXT("") : TEXT(", "),
					UTF8_TO_TCHAR(ftcore::standInKindName(static_cast<ftcore::StandInKind>(K))), Count.Points[K], Count.Instances[K]);
				Total += Count.Instances[K];
			}
		}
		return FString::Printf(TEXT("%s; %d instances"), Text.IsEmpty() ? TEXT("none") : *Text, Total);
	};
	UE_LOG(LogForgottenThird, Display, TEXT("StandIns: %d points from %d tables: %s."), static_cast<int32>(Points.size()), Tables, *Line(All));
	if (!Trees.Models.empty())
	{
		FString Models;
		int32 Placed = 0;
		for (int32 M = 0; M < PerModel.Num(); ++M)
		{
			Models += FString::Printf(TEXT("%s%s %d"), Models.IsEmpty() ? TEXT("") : TEXT(", "), *Trees.Meshes[M]->GetName(), PerModel[M]);
			Placed += PerModel[M];
		}
		UE_LOG(LogForgottenThird, Display, TEXT("StandIns: modelled trees: %d (%s); %d scaled past the limits (clamped); %d grates; %d rows "
			"with no model of their kind left as stand-ins; %lld triangles at LOD0 in all."), Placed, *Models, Clamped, Grates, KeptStandIns,
			TreeTriangles);
	}
	TArray<FIntPoint> CellIds;
	PerCell.GetKeys(CellIds);
	CellIds.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });
	for (const FIntPoint& Cell : CellIds)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("StandIns: Cell_%d_%d: %s."), Cell.X, Cell.Y, *Line(PerCell[Cell]));
	}
	return true;
}

bool FTStandIns::PlacePlants(FFTCellInstancer& Cells, const ftcore::HeightField& Field, double Side)
{
	using namespace FTStandIns::Impl;
	FString File;
	if (!FParse::Value(FCommandLine::Get(), TEXT("PlantPoints="), File, false) || File.IsEmpty())
	{
		return true;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("StandIns: -PlantPoints couldn't read '%s'."), *File);
		return false;
	}
	std::vector<ftcore::PlantPoint> Points;
	std::string Error;
	if (!ftcore::parsePlantPoints(std::string_view(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num())), Points, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("StandIns: -PlantPoints %s: %s"), *File, UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	FFTCellOptions Shrub;
	Shrub.Cull = EFTCellCull::Prop;
	Shrub.Tag = TEXT("Shrub");
	FFTCellOptions Cover;
	Cover.Cull = EFTCellCull::Cover;
	Cover.bCastShadow = false;
	Cover.Tag = TEXT("GroundCover");
	TMap<FString, UStaticMesh*> Meshes;
	TMap<FString, int32> Count;
	int64 Triangles = 0;
	const TArrayView<const float> NoData;
	for (const ftcore::PlantPoint& Point : Points)
	{
		const FString Asset = UTF8_TO_TCHAR(Point.asset.c_str());
		UStaticMesh** Found = Meshes.Find(Asset);
		if (!Found)
		{
			UStaticMesh* Loaded = LoadPlant(Asset);
			if (!Loaded)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("StandIns: -PlantPoints: couldn't load %s (import it with FTImportPlant)."), *Asset);
				return false;
			}
			Found = &Meshes.Add(Asset, Loaded);
		}
		const double Foot = Point.hasZ ? Point.z : (Field.grid ? Field.at(Point.x, Point.y) : 0.0);
		const ftcore::MassingVec At = ftcore::massingEnginePoint(Side, Point.x, Point.y, Foot);
		const FRotator Yaw(0.0, ftcore::massingEngineYaw(Point.yaw), 0.0);
		const bool bCover = Point.kind == ftcore::PlantPointKind::Cover;
		Cells.Add(*Found, TArrayView<UMaterialInterface* const>(), FTransform(Yaw, FVector(At.x, At.y, At.z) * 100.0, FVector(Point.scale)), NoData,
			bCover ? EFTCellKind::NoCollide : EFTCellKind::Solid, bCover ? Cover : Shrub);
		++Count.FindOrAdd(Asset);
		Triangles += (*Found)->GetNumTriangles(0);
	}
	FString Census;
	Count.KeySort([](const FString& A, const FString& B) { return A < B; });
	for (const TPair<FString, int32>& Pair : Count)
	{
		Census += FString::Printf(TEXT("%s%s %d (%d tris)"), Census.IsEmpty() ? TEXT("") : TEXT(", "), *Pair.Key, Pair.Value, Meshes[Pair.Key]->GetNumTriangles(0));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("StandIns: -PlantPoints: %d plants of %d models, %lld triangles at LOD0 in all: %s."), static_cast<int32>(Points.size()),
		Meshes.Num(), Triangles, *Census);
	return true;
}