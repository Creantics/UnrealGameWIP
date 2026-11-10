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

// The engine's basic shapes; the canopy (the last shape) is built here (CanopyMesh).
constexpr int32 kBasicShapes = static_cast<int32>(ftcore::StandInShape::Canopy);
const TCHAR* const kShapePaths[kBasicShapes] = {
	TEXT("/Engine/BasicShapes/Cube.Cube"),
	TEXT("/Engine/BasicShapes/Sphere.Sphere"),
	TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),
};
static_assert(kBasicShapes + 1 == kShapes, "every shape but the canopy is a basic shape");

// A tree canopy's mesh (ftcore::standInCanopyMesh) in Package: the unit box as a metre cube (100 cm a side) around its
// fork at the origin's x and y, its foot at z -50; flat-shaded, slot 0 the leaves (Leaf), slot 1 the limbs (Bark), no
// collision (the trunk's cylinder collides), no lightmap channel (lit by the look's dynamic lights, as the shapes are).
// Its triangle count in Triangles. Null (logged) when the variant has no mesh.
UStaticMesh* CanopyMesh(UPackage* Package, int32 Variant, UMaterialInterface* Leaf, UMaterialInterface* Bark, int32& Triangles)
{
	ftcore::StandInCanopyMesh Source;
	if (!ftcore::standInCanopyMesh(static_cast<std::size_t>(Variant), Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("StandIns: no canopy mesh %d."), Variant);
		return nullptr;
	}
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, *FString::Printf(TEXT("SM_StandIn_Canopy%d"), Variant), RF_Public);
	FMeshDescription Desc;
	FStaticMeshAttributes Attributes(Desc);
	Attributes.Register();
	const FName SlotNames[2] = {FName(TEXT("Leaf")), FName(TEXT("Bark"))};
	FPolygonGroupID Groups[2];
	for (int32 G = 0; G < 2; ++G)
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
			Normals[Inst] = Out;
			const FVector3f P = Positions[Verts[Index[K]]];
			Uvs[Inst] = FVector2f(P.X, P.Y) / 100.f;
			Corners[K] = Inst;
		}
		Desc.CreatePolygon(Groups[Source.slots[T] == 0 ? 0 : 1], TArrayView<const FVertexInstanceID>(Corners, 3));
	}
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Leaf, SlotNames[0], SlotNames[0]));
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Bark, SlotNames[1], SlotNames[1]));
	FStaticMeshSourceModel& Model = Mesh->AddSourceModel();
	Model.BuildSettings.bRecomputeNormals = false;
	Model.BuildSettings.bRecomputeTangents = true;
	Model.BuildSettings.bGenerateLightmapUVs = false;
	Mesh->CreateMeshDescription(0, MoveTemp(Desc));
	Mesh->CommitMeshDescription(0);
	Mesh->CreateBodySetup(); // as the sloped boxes' meshes; the canopies' cell groups take no collision
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
		Canopies[V] = CanopyMesh(Package, V, Paints[static_cast<int32>(ftcore::StandInPaint::Leaf)],
			Paints[static_cast<int32>(ftcore::StandInPaint::Bark)], CanopyTriangles[V]);
		if (!Canopies[V])
		{
			return false;
		}
	}
	Shapes[CanopyShape] = Canopies[0];
	UE_LOG(LogForgottenThird, Display, TEXT("StandIns: canopies of %d, %d, %d triangles (one instance a tree, over its trunk); the basic sphere %d, cylinder %d."),
		CanopyTriangles[0], CanopyTriangles[1], CanopyTriangles[2], Shapes[static_cast<int32>(ftcore::StandInShape::Sphere)]->GetNumTriangles(0),
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
	for (const ftcore::StandInPoint& Point : Points)
	{
		const int32 K = static_cast<int32>(Point.kind);
		const double Foot = Point.hasZ ? Point.z : (Field.grid ? Field.at(Point.x, Point.y) : 0.0);
		const std::size_t N = ftcore::standInParts(Point, Parts);
		const ftcore::MassingVec At = ftcore::massingEnginePoint(Side, Point.x, Point.y, Foot);
		const FIntPoint Cell(FMath::FloorToInt32(At.x * 100.0 / FTCells::CellSize), FMath::FloorToInt32(At.y * 100.0 / FTCells::CellSize));
		FCount& Here = PerCell.FindOrAdd(Cell);
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
	TArray<FIntPoint> CellIds;
	PerCell.GetKeys(CellIds);
	CellIds.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });
	for (const FIntPoint& Cell : CellIds)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("StandIns: Cell_%d_%d: %s."), Cell.X, Cell.Y, *Line(PerCell[Cell]));
	}
	return true;
}
