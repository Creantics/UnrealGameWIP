#include "FTBuildMassingWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EditorWorldUtils.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Polys.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Model.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "FTExploreDirector.h"
#include "FTExploreGameMode.h"
#include "FTSceneLook.h"

#include "ftcore/Json.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/WorldChecks.h"
#include "ftcore/WorldLayout.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#endif

namespace
{
#if WITH_EDITOR
// A Landscape component is 2 x 63 quads.
constexpr int32 kLandSubsections = 2;
constexpr int32 kLandSubsectionQuads = 63;
// The sea's plane reaches this far (m) east of its west edge, and as far each way along the square.
constexpr double kSeaReach = 20000.0;
// How far (cm) the entry and a goal may lie from the navmesh and still count as on it.
const FVector kNavProjectExtent(300.0, 300.0, 600.0);
// The material slot of a sloped box's mesh.
const FName kMassingSlot(TEXT("Massing"));
// The sun's angle, as in the big worlds.
const FRotator kMassingSun(-38, 35, 0);
// With -World: a district's spawn stands on the highest deck or landing at most this far (m) over the higher of its
// plaza's floor and the ground there, so a raised landing counts and a roof over the spawn doesn't.
constexpr double kSpawnStepUp = 6.0;
// The lead's height (m), for a spawn that would stand inside a box.
constexpr double kLeadHeight = 1.8;
// The tag on the walls table's hidden blockers: the tour's edge legs tell a hidden wall from a built one by it.
const FName kHiddenWallTag(TEXT("FTHiddenWall"));
// -Coverage logs a line for each of this many of the largest islands it leaves out as roofs and interiors of built boxes.
constexpr size_t kCoverageInBoxLines = 12;

FVector Cm(const ftcore::MassingVec& V)
{
	return FVector(V.x * 100.0, V.y * 100.0, V.z * 100.0);
}

FString Utf8(const std::string& Text)
{
	return FString(UTF8_TO_TCHAR(Text.c_str()));
}

std::string_view View(const TArray<uint8>& Bytes)
{
	return std::string_view(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
}

bool LoadBytes(const FString& File, TArray<uint8>& Bytes)
{
	if (File.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: couldn't read '%s'."), *File);
		return false;
	}
	return true;
}

bool LoadBoxes(const FString& File, std::vector<ftcore::MassingBox>& Boxes)
{
	TArray<uint8> Bytes;
	if (!LoadBytes(File, Bytes))
	{
		return false;
	}
	std::string Error;
	if (!ftcore::importMassingBoxes(View(Bytes), Boxes, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s"), *File, *Utf8(Error));
		return false;
	}
	return true;
}

// The height field: its grid (south-west sample first, rows running north) and where it lies.
struct FHeightField
{
	int32 SamplesX = 0;
	int32 SamplesY = 0;
	double Spacing = 0.0;
	double OriginX = 0.0;
	double OriginY = 0.0;
	TArray<uint16> Grid;
};

bool LoadHeightField(const FString& JsonFile, FHeightField& Out)
{
	TArray<uint8> Bytes;
	if (!LoadBytes(JsonFile, Bytes))
	{
		return false;
	}
	ftcore::JsonDocument Doc;
	std::string Error;
	if (!Doc.parse(View(Bytes), &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s"), *JsonFile, *Utf8(Error));
		return false;
	}
	const ftcore::JsonValue Root = Doc.root();
	Out.SamplesX = static_cast<int32>(Root["samples_x"].integer(0));
	Out.SamplesY = static_cast<int32>(Root["samples_y"].integer(0));
	Out.Spacing = Root["spacing_m"].number(0.0);
	Out.OriginX = Root["origin_of_sample_0_0_m"][0u].number(0.0);
	Out.OriginY = Root["origin_of_sample_0_0_m"][1u].number(0.0);
	const std::string Name(Root["file"].string());
	if (Out.SamplesX < 2 || Out.SamplesY < 2 || Out.Spacing <= 0.0 || Name.empty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s needs samples_x, samples_y, spacing_m and file."), *JsonFile);
		return false;
	}
	TArray<uint8> Raw;
	if (!LoadBytes(FPaths::Combine(FPaths::GetPath(JsonFile), Utf8(Name)), Raw))
	{
		return false;
	}
	const int64 Count = static_cast<int64>(Out.SamplesX) * Out.SamplesY;
	if (static_cast<int64>(Raw.Num()) != Count * 2)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: the height field holds %d bytes, not %lld."), Raw.Num(), Count * 2);
		return false;
	}
	Out.Grid.SetNumUninitialized(static_cast<int32>(Count));
	for (int32 I = 0; I < static_cast<int32>(Count); ++I)
	{
		Out.Grid[I] = static_cast<uint16>(Raw[2 * I] | (Raw[2 * I + 1] << 8)); // little-endian
	}
	return true;
}

struct FGoal
{
	FString Id;
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	bool bSpawn = false; // the row names a spawn (spawn_x, spawn_y)
	double SpawnX = 0.0;
	double SpawnY = 0.0;
	// Massing ids, each optional: held, a box held for its own piece that the goal stands inside or on (the goal is
	// listed apart, not walked or counted); goal_box and spawn_box, a box the goal or the spawn is a temporary
	// placement just outside of. Their rows in the massing table once it is read, else INDEX_NONE.
	FString Held;
	FString GoalBox;
	FString SpawnBox;
	int32 HeldAt = INDEX_NONE;
	int32 GoalBoxAt = INDEX_NONE;
	int32 SpawnBoxAt = INDEX_NONE;
};

// A CSV with the columns id, x, y, z and optionally spawn_x, spawn_y, held, goal_box, spawn_box (in any order,
// others skipped; a row may leave the optional fields empty); '#' lines are comments.
bool LoadGoals(const FString& File, TArray<FGoal>& Goals)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: couldn't read '%s'."), *File);
		return false;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	int32 Col[4] = {INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE};
	const TCHAR* const Names[4] = {TEXT("id"), TEXT("x"), TEXT("y"), TEXT("z")};
	int32 SpawnCol[2] = {INDEX_NONE, INDEX_NONE};
	const TCHAR* const SpawnNames[2] = {TEXT("spawn_x"), TEXT("spawn_y")};
	int32 BoxCol[3] = {INDEX_NONE, INDEX_NONE, INDEX_NONE};
	const TCHAR* const BoxNames[3] = {TEXT("held"), TEXT("goal_box"), TEXT("spawn_box")};
	bool bHeader = false;
	for (int32 L = 0; L < Lines.Num(); ++L)
	{
		const FString Line = Lines[L].TrimStartAndEnd();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("\"#")))
		{
			continue;
		}
		TArray<FString> Fields;
		Line.ParseIntoArray(Fields, TEXT(","), false);
		if (!bHeader)
		{
			for (int32 C = 0; C < 4; ++C)
			{
				Col[C] = Fields.IndexOfByPredicate([&](const FString& F) { return F.TrimStartAndEnd() == Names[C]; });
				if (Col[C] == INDEX_NONE)
				{
					UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s has no column %s."), *File, Names[C]);
					return false;
				}
			}
			for (int32 C = 0; C < 2; ++C)
			{
				SpawnCol[C] = Fields.IndexOfByPredicate([&](const FString& F) { return F.TrimStartAndEnd() == SpawnNames[C]; });
			}
			if ((SpawnCol[0] == INDEX_NONE) != (SpawnCol[1] == INDEX_NONE))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s has one of spawn_x and spawn_y without the other."), *File);
				return false;
			}
			for (int32 C = 0; C < 3; ++C)
			{
				BoxCol[C] = Fields.IndexOfByPredicate([&](const FString& F) { return F.TrimStartAndEnd() == BoxNames[C]; });
			}
			bHeader = true;
			continue;
		}
		FGoal Goal;
		Goal.Id = Fields.IsValidIndex(Col[0]) ? Fields[Col[0]].TrimStartAndEnd() : FString();
		double* const Values[3] = {&Goal.X, &Goal.Y, &Goal.Z};
		for (int32 C = 1; C < 4; ++C)
		{
			if (Goal.Id.IsEmpty() || !Fields.IsValidIndex(Col[C]) || !LexTryParseString(*Values[C - 1], *Fields[Col[C]].TrimStartAndEnd()))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: a goal row needs an id and numbers for x, y and z: '%s'."), *File,
					*Line);
				return false;
			}
		}
		if (SpawnCol[0] != INDEX_NONE)
		{
			const FString SX = Fields.IsValidIndex(SpawnCol[0]) ? Fields[SpawnCol[0]].TrimStartAndEnd() : FString();
			const FString SY = Fields.IsValidIndex(SpawnCol[1]) ? Fields[SpawnCol[1]].TrimStartAndEnd() : FString();
			Goal.bSpawn = !SX.IsEmpty() || !SY.IsEmpty();
			if (Goal.bSpawn && (!LexTryParseString(Goal.SpawnX, *SX) || !LexTryParseString(Goal.SpawnY, *SY)))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: a goal's spawn needs numbers for both spawn_x and spawn_y: '%s'."),
					*File, *Line);
				return false;
			}
		}
		FString* const Refs[3] = {&Goal.Held, &Goal.GoalBox, &Goal.SpawnBox};
		for (int32 C = 0; C < 3; ++C)
		{
			if (Fields.IsValidIndex(BoxCol[C]))
			{
				*Refs[C] = Fields[BoxCol[C]].TrimStartAndEnd();
			}
		}
		if (!Goal.Held.IsEmpty() && !Goal.GoalBox.IsEmpty())
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: a goal is held or a temporary placement, not both: '%s'."), *File, *Line);
			return false;
		}
		if (!Goal.SpawnBox.IsEmpty() && !Goal.bSpawn)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: a spawn_box needs the row's spawn_x and spawn_y: '%s'."), *File, *Line);
			return false;
		}
		Goals.Add(Goal);
	}
	return true;
}

// One row of the -Kit table: a finished mesh that stands in for a box.
struct FKitRow
{
	FString Box;
	FString Asset;
	bool bFit = false;
	double Turn = 0.0;
	double Dz = 0.0;
	int32 BoxAt = INDEX_NONE;
	UStaticMesh* Mesh = nullptr;
};

// A CSV with the columns box (a massing id) and asset (a static mesh's object path) and optionally fit ("real", the
// default, or "footprint"), turn and dz (ftcore::massingKitPlacement), in any order, others skipped; a row may leave
// the optional fields empty; '#' lines are comments.
bool LoadKit(const FString& File, TArray<FKitRow>& Rows)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: couldn't read '%s'."), *File);
		return false;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	int32 BoxCol = INDEX_NONE, AssetCol = INDEX_NONE, FitCol = INDEX_NONE, TurnCol = INDEX_NONE, DzCol = INDEX_NONE;
	bool bHeader = false;
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("\"#")))
		{
			continue;
		}
		TArray<FString> Fields;
		Line.ParseIntoArray(Fields, TEXT(","), false);
		for (FString& Field : Fields)
		{
			Field.TrimStartAndEndInline();
		}
		if (!bHeader)
		{
			BoxCol = Fields.IndexOfByKey(TEXT("box"));
			AssetCol = Fields.IndexOfByKey(TEXT("asset"));
			FitCol = Fields.IndexOfByKey(TEXT("fit"));
			TurnCol = Fields.IndexOfByKey(TEXT("turn"));
			DzCol = Fields.IndexOfByKey(TEXT("dz"));
			if (BoxCol == INDEX_NONE || AssetCol == INDEX_NONE)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s needs the columns box and asset."), *File);
				return false;
			}
			bHeader = true;
			continue;
		}
		FKitRow Row;
		Row.Box = Fields.IsValidIndex(BoxCol) ? Fields[BoxCol] : FString();
		Row.Asset = Fields.IsValidIndex(AssetCol) ? Fields[AssetCol] : FString();
		const FString Fit = Fields.IsValidIndex(FitCol) ? Fields[FitCol] : FString();
		const FString Turn = Fields.IsValidIndex(TurnCol) ? Fields[TurnCol] : FString();
		const FString Dz = Fields.IsValidIndex(DzCol) ? Fields[DzCol] : FString();
		Row.bFit = Fit == TEXT("footprint");
		if (Row.Box.IsEmpty() || Row.Asset.IsEmpty() || !(Fit.IsEmpty() || Fit == TEXT("real") || Row.bFit) ||
			!(Turn.IsEmpty() || LexTryParseString(Row.Turn, *Turn)) || !(Dz.IsEmpty() || LexTryParseString(Row.Dz, *Dz)))
		{
			UE_LOG(LogForgottenThird, Error,
				TEXT("FTBuildMassingWorld: %s: a kit row needs a box and an asset, fit real or footprint, and numbers for turn and dz: '%s'."), *File,
				*Line);
			return false;
		}
		Rows.Add(Row);
	}
	return true;
}

// What a box is, for the log.
const TCHAR* BoxKind(const ftcore::MassingBox& Box)
{
	const ftcore::MassingRole Role = ftcore::massingRole(Box);
	if (ftcore::massingVisible(Role))
	{
		return TEXT("a built box");
	}
	return Role == ftcore::MassingRole::Hidden ? TEXT("a hidden kind, not built") : TEXT("a row the build skips");
}

// Where a plan point stands against a box, for the log: off its footprint, over its top or under its bottom (true), or
// inside it (false).
bool Against(const ftcore::MassingBox& Box, double X, double Y, double Z, FString& Out)
{
	const double Gap = ftcore::massingFootprintGap(Box, X, Y);
	if (Gap > 0.0)
	{
		Out = FString::Printf(TEXT("%.1f m off its footprint"), Gap);
		return true;
	}
	if (Z >= Box.z1)
	{
		Out = FString::Printf(TEXT("%.1f m over its top"), Z - Box.z1);
		return true;
	}
	if (Z <= Box.z0)
	{
		Out = FString::Printf(TEXT("%.1f m under its bottom"), Box.z0 - Z);
		return true;
	}
	Out = TEXT("inside it");
	return false;
}

// "x,y" or "x,y,z" (metres); false if malformed.
bool ParsePoint(const FString& Text, int32 Count, double* Out)
{
	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT(","), false);
	if (Parts.Num() != Count)
	{
		return false;
	}
	for (int32 I = 0; I < Count; ++I)
	{
		if (!LexTryParseString(Out[I], *Parts[I].TrimStartAndEnd()))
		{
			return false;
		}
	}
	return true;
}

// A tint of the world surface material, saved inside the map package; a Roughness of 0 or more replaces the master's.
UMaterialInterface* Tint(UPackage* Package, UMaterialInterface* Base, const TCHAR* Name, const FLinearColor& Color, float Roughness = -1.f)
{
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
	Mic->SetParentEditorOnly(Base);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), Color);
	if (Roughness >= 0.f)
	{
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), Roughness);
	}
	Mic->PostEditChange();
	return Mic;
}

AStaticMeshActor* Block(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Mesh, UMaterialInterface* Material, const FString& Label,
	const FVector& Location, const FVector& Scale, double Yaw)
{
	AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Location, FRotator(0.0, Yaw, 0.0), Spawn);
	Prop->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	if (Material)
	{
		Prop->GetStaticMeshComponent()->SetMaterial(0, Material);
	}
	Prop->SetActorScale3D(Scale);
	Prop->SetActorLabel(Label);
	return Prop;
}

// A sloped box's own mesh, its vertices relative to Pivot (cm), each face flat-shaded, with planar UVs in metres on
// the world and collision from its triangles.
UStaticMesh* SlopedMesh(UPackage* Package, const FString& Id, const ftcore::MassingMesh& Shape, const FVector& Pivot, UMaterialInterface* Material)
{
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, *FString::Printf(TEXT("SM_Massing_%s"), *Id), RF_Public);
	FMeshDescription Desc;
	FStaticMeshAttributes Attributes(Desc);
	Attributes.Register();
	const FPolygonGroupID Group = Desc.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = kMassingSlot;
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> Uvs = Attributes.GetVertexInstanceUVs();
	std::array<FVertexID, 8> Verts;
	std::array<FVector, 8> Points;
	for (int32 I = 0; I < 8; ++I)
	{
		Points[I] = Cm(Shape.v[static_cast<size_t>(I)]);
		Verts[I] = Desc.CreateVertex();
		Positions[Verts[I]] = FVector3f(Points[I] - Pivot);
	}
	for (const auto& Quad : Shape.quads)
	{
		const FVector& A = Points[Quad[0]];
		const FVector& B = Points[Quad[1]];
		const FVector& C = Points[Quad[2]];
		const FVector& D = Points[Quad[3]];
		// The engine winds a face so its first triangle's cross product points in; the diagonals' cross product has
		// that orientation for a planar quad and doesn't fail on a thin one.
		const FVector Out = -FVector::CrossProduct(C - A, D - B).GetSafeNormal();
		const FVector AbsOut = Out.GetAbs();
		FVertexInstanceID Corners[4];
		for (int32 K = 0; K < 4; ++K)
		{
			const FVector& P = Points[Quad[K]];
			const FVertexInstanceID Inst = Desc.CreateVertexInstance(Verts[Quad[K]]);
			Normals[Inst] = FVector3f(Out);
			// Planar on the face's main axis, a unit a metre, on the world so neighbours line up.
			const FVector2D Uv = AbsOut.Z >= AbsOut.X && AbsOut.Z >= AbsOut.Y ? FVector2D(P.X, P.Y)
				: AbsOut.X >= AbsOut.Y                                          ? FVector2D(P.Y, -P.Z)
																				: FVector2D(P.X, -P.Z);
			Uvs[Inst] = FVector2f(Uv / 100.0);
			Corners[K] = Inst;
		}
		Desc.CreatePolygon(Group, TArrayView<const FVertexInstanceID>(Corners, 4));
	}
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, kMassingSlot, kMassingSlot));
	FStaticMeshSourceModel& Source = Mesh->AddSourceModel();
	Source.BuildSettings.bRecomputeNormals = false;
	Source.BuildSettings.bRecomputeTangents = true;
	// Lit by the shared look's dynamic lights (as the builders' cubes are), so no lightmap channel.
	Source.BuildSettings.bGenerateLightmapUVs = false;
	Mesh->CreateMeshDescription(0, MoveTemp(Desc));
	Mesh->CommitMeshDescription(0);
	Mesh->CreateBodySetup();
	Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
	Mesh->Build(true);
	Mesh->PostEditChange();
	return Mesh;
}

ANavMeshBoundsVolume* NavBounds(UWorld* World, const FActorSpawnParameters& Spawn, const FVector& Centre, const FVector& Size)
{
	ANavMeshBoundsVolume* Bounds = World->SpawnActor<ANavMeshBoundsVolume>(Centre, FRotator::ZeroRotator, Spawn);
	// The builder fills the volume's brush model, so it needs one first (as the editor's volume factory does).
	Bounds->Brush = NewObject<UModel>(Bounds);
	Bounds->Brush->Initialize(nullptr, true);
	Bounds->Brush->Polys = NewObject<UPolys>(Bounds->Brush);
	Bounds->GetBrushComponent()->Brush = Bounds->Brush;
	UCubeBuilder* Box = NewObject<UCubeBuilder>();
	Box->X = Size.X;
	Box->Y = Size.Y;
	Box->Z = Size.Z;
	Box->Build(World, Bounds);
	Bounds->SetActorLabel(TEXT("NavMeshBounds"));
	return Bounds;
}

// The world's actors as a CSV, read back from the actors themselves, one row each, and how many rows. A mesh held in
// the map's own package is a sloped box's, so its row gives the vertices; the director's row gives its floors. Labels
// are read without creating one, so an unlabelled actor stays unlabelled and its row's label is empty.
FString ActorDump(UWorld* World, int32& Rows)
{
	FString Text = TEXT("# FTBuildMassingWorld -Dump: every actor of the saved map, in the engine frame; location in cm, rotation in degrees.\n"
						"# mesh_vertices_cm: a sloped box's own mesh, each vertex 'x y z' relative to the actor, ';' between them, in massingMesh's\n"
						"# order: 0-3 the bottom ring, 4-7 the top ring over them; 0 to 1 runs along the box's +u axis, whose heading here is minus the plan's rot.\n"
						"# district_floors_cm: the director's floors, D0 first, ';' between them.\n"
						"label,class,x_cm,y_cm,z_cm,pitch,yaw,roll,scale_x,scale_y,scale_z,hidden,mesh_vertices_cm,district_floors_cm\n");
	Rows = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		FString Vertices;
		if (const AStaticMeshActor* Prop = Cast<AStaticMeshActor>(Actor))
		{
			const UStaticMesh* Mesh = Prop->GetStaticMeshComponent()->GetStaticMesh().Get();
			const FMeshDescription* Desc = Mesh && Mesh->GetPackage() == World->GetPackage() ? Mesh->GetMeshDescription(0) : nullptr;
			if (Desc)
			{
				const TVertexAttributesRef<const FVector3f> Positions = Desc->GetVertexPositions();
				for (const FVertexID Vertex : Desc->Vertices().GetElementIDs())
				{
					const FVector3f At = Positions[Vertex];
					Vertices += FString::Printf(TEXT("%s%.3f %.3f %.3f"), Vertices.IsEmpty() ? TEXT("") : TEXT(";"), At.X, At.Y, At.Z);
				}
			}
		}
		FString Floors;
		if (const AFTExploreDirector* Director = Cast<AFTExploreDirector>(Actor))
		{
			for (const float Floor : Director->DistrictFloors)
			{
				Floors += FString::Printf(TEXT("%s%.3f"), Floors.IsEmpty() ? TEXT("") : TEXT(";"), Floor);
			}
		}
		const FVector Location = Actor->GetActorLocation();
		const FRotator Rotation = Actor->GetActorRotation();
		const FVector Scale = Actor->GetActorScale3D();
		Text += FString::Printf(TEXT("%s,%s,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%s,%s\n"), *Actor->GetActorLabel(false),
			*Actor->GetClass()->GetName(), Location.X, Location.Y, Location.Z, Rotation.Pitch, Rotation.Yaw, Rotation.Roll, Scale.X, Scale.Y, Scale.Z,
			Actor->IsHidden() ? 1 : 0, *Vertices, *Floors);
		++Rows;
	}
	return Text;
}

// The reach check's path search budget: well over the engine's default of 2048 nodes, and under the 65535 a search
// node's 16-bit index allows.
constexpr uint32 kReachSearchNodes = 32768;

// What the reach check's path queries came back with, for its summary line.
struct FReachTally
{
	int32 Reached = 0, Full = 0, Partial = 0, NoPath = 0, OffMesh = 0, AtEntry = 0, LimitHit = 0;
};

FString ReachLine(const TCHAR* What, const FReachTally& Tally, int32 Of)
{
	return FString::Printf(TEXT("%s %d of %d reached (%d full paths, %d at the entry; %d partial, %d no path, %d off the navmesh; the node limit hit on %d)"),
		What, Tally.Reached, Of, Tally.Full, Tally.AtEntry, Tally.Partial, Tally.NoPath, Tally.OffMesh, Tally.LimitHit);
}

// Whether a full path runs from the entry (From, on NavData) to a plan point (m), logged either way with the query's own
// answer: a full path, a partial one or none, and whether its search ran out of nodes. Filter carries the budget.
bool WalkFromEntry(UNavigationSystemV1& Nav, const ANavigationData& NavData, const FSharedConstNavQueryFilter& Filter, const FNavLocation& From,
	double Side, const FString& What, double X, double Y, double Z, FReachTally& Tally)
{
	FNavLocation To;
	const bool bOnMesh = Nav.ProjectPointToNavigation(Cm(ftcore::massingEnginePoint(Side, X, Y, Z)), To, kNavProjectExtent, &NavData);
	// A point on the entry's own spot: the engine answers with a one-point path, which is not a valid one.
	const bool bAtEntry = bOnMesh && FVector::Dist(From.Location, To.Location) < 100.0;
	FPathFindingResult Route;
	if (bOnMesh && !bAtEntry)
	{
		Route = Nav.FindPathSync(FPathFindingQuery(static_cast<const UObject*>(nullptr), NavData, From.Location, To.Location, Filter),
			EPathFindingMode::Regular);
	}
	const bool bPath = Route.IsSuccessful() && Route.Path.IsValid() && Route.Path->IsValid();
	const bool bFull = bPath && !Route.Path->IsPartial();
	const bool bLimit = Route.Path.IsValid() && Route.Path->DidSearchReachedLimit();
	const bool bReached = bAtEntry || bFull;
	Tally.Reached += bReached ? 1 : 0;
	Tally.AtEntry += bAtEntry ? 1 : 0;
	Tally.Full += bFull ? 1 : 0;
	Tally.Partial += bPath && !bFull ? 1 : 0;
	Tally.NoPath += bOnMesh && !bAtEntry && !bPath ? 1 : 0;
	Tally.OffMesh += bOnMesh ? 0 : 1;
	Tally.LimitHit += bLimit ? 1 : 0;
	const FString Query = !bOnMesh || bAtEntry
		? FString(TEXT("no query"))
		: FString::Printf(TEXT("query %s, node limit %s"), bFull ? TEXT("full") : bPath ? TEXT("partial") : TEXT("none"), bLimit ? TEXT("hit") : TEXT("not hit"));
	// Engine cm back to the plan's metres, for the log.
	const auto PlanOf = [Side](const FVector& At) { return FVector(At.X / 100.0 + Side * 0.5, Side * 0.5 - At.Y / 100.0, At.Z / 100.0); };
	if (bReached)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: %s (%.0f, %.0f, %+.0f): reached, %.0f m of path; %s."), *What, X, Y, Z,
			bAtEntry ? 0.0 : Route.Path->GetLength() / 100.0, *Query);
	}
	else if (!bOnMesh)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: %s (%.0f, %.0f, %+.0f): not on the navmesh; %s."), *What, X, Y, Z, *Query);
	}
	else
	{
		// Where the point met the navmesh, and where the nearest path from the entry stops short of it.
		const FVector Met = PlanOf(To.Location);
		FString Stop = TEXT("no path at all");
		if (bPath && Route.Path->GetPathPoints().Num() > 0)
		{
			const FVector Last = Route.Path->GetPathPoints().Last().Location;
			const FVector End = PlanOf(Last);
			Stop = FString::Printf(TEXT("the path stops at (%.1f, %.1f, %+.1f), %.1f m short"), End.X, End.Y, End.Z, FVector::Dist(Last, To.Location) / 100.0);
		}
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: %s (%.0f, %.0f, %+.0f): no full path from the entry; on the navmesh at "
			"(%.1f, %.1f, %+.1f); %s; %s."), *What, X, Y, Z, Met.X, Met.Y, Met.Z, *Stop, *Query);
	}
	return bReached;
}

// The reach check's filter: the navmesh's own, with the raised search budget.
FSharedConstNavQueryFilter ReachFilter(const ANavigationData& NavData)
{
	FSharedNavQueryFilter Filter = NavData.GetDefaultQueryFilter()->GetCopy();
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: reach check: the path search budget is %u nodes (the navmesh's own %u)."),
		kReachSearchNodes, NavData.GetDefaultQueryFilter()->GetMaxSearchNodes());
	Filter->SetMaxSearchNodes(kReachSearchNodes);
	return Filter;
}

// -Tour: the stops the director's tour walks after its districts (ftcore::importTourStops).
bool LoadTour(const FString& File, std::vector<ftcore::TourStop>& Stops)
{
	TArray<uint8> Bytes;
	if (!LoadBytes(File, Bytes))
	{
		return false;
	}
	std::string Error;
	if (!ftcore::importTourStops(View(Bytes), Stops, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s"), *File, *Utf8(Error));
		return false;
	}
	return true;
}

// -Coverage: the points that declare the navmesh's islands (ftcore::importCoveragePoints).
bool LoadCoverage(const FString& File, std::vector<ftcore::CoveragePoint>& Points)
{
	TArray<uint8> Bytes;
	if (!LoadBytes(File, Bytes))
	{
		return false;
	}
	std::string Error;
	if (!ftcore::importCoveragePoints(View(Bytes), Points, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s"), *File, *Utf8(Error));
		return false;
	}
	return true;
}

// -Coverage: the navmesh's islands against the coverage points. The islands are its ground polys (each tile's, with
// their corners in the plan frame), joined where a poly's neighbours list another, or an off-mesh link whose own
// neighbours do (ftcore::navIslands). District 0's island holds the poly its spawn (HomeAt, engine cm) projects to; an
// island over ftcore::kNavIslandBig m² besides it must hold a pocket point, and each on-point must project into it
// (ftcore::classifyNavIslands). With a sea (bSea: SeaLevel in m, SeaFrom saying where it came from), the polys under it
// are left out first (ftcore::navPolysAboveSea): the sea plane has no collision, so the ground under it gets navmesh too.
// A point on a poly left out is off the navmesh. An island over the size on a built box's roof or inside built boxes
// (Solids: the boxes and walls tables' rows; ftcore::navIslandsInBoxes) is no fault of the coverage, which leaves out
// interiors and roofs no stair row reaches: it is counted apart, with a line for each of the largest. Logs a summary
// line, then a line per undeclared island and per on-point off district 0's island or the navmesh; returns how many of
// those there are. Allocates: build time only.
int32 CheckCoverage(const UNavigationSystemV1& Nav, const ARecastNavMesh& NavMesh, double Side, const FVector& HomeAt,
	const std::vector<ftcore::CoveragePoint>& Points, const std::vector<ftcore::MassingBox>& Solids, bool bSea, double SeaLevel,
	const TCHAR* SeaFrom)
{
	const auto PlanOf = [Side](const FVector& At) { return ftcore::MassingVec{At.X / 100.0 + Side * 0.5, Side * 0.5 - At.Y / 100.0, At.Z / 100.0}; };
	TArray<FNavTileRef> Tiles;
	NavMesh.GetAllNavMeshTiles(Tiles);
	std::vector<ftcore::MassingVec> Corners;
	std::vector<ftcore::NavPolyRef> AllPolys;
	TArray<NavNodeRef> AllRefs;
	TMap<NavNodeRef, uint32> AllIndex;
	TArray<FNavPoly> TilePolys;
	TArray<FVector> Verts;
	for (const FNavTileRef& Tile : Tiles)
	{
		TilePolys.Reset();
		if (!NavMesh.GetPolysInTile(Tile, TilePolys))
		{
			continue;
		}
		for (const FNavPoly& Poly : TilePolys)
		{
			Verts.Reset();
			if (!NavMesh.GetPolyVerts(Poly.Ref, Verts) || Verts.IsEmpty())
			{
				continue;
			}
			ftcore::NavPolyRef Ref;
			Ref.first = static_cast<uint32>(Corners.size());
			Ref.count = static_cast<uint32>(Verts.Num());
			for (const FVector& V : Verts)
			{
				Corners.push_back(PlanOf(V));
			}
			AllIndex.Add(Poly.Ref, static_cast<uint32>(AllPolys.size()));
			AllRefs.Add(Poly.Ref);
			AllPolys.push_back(Ref);
		}
	}
	// Without a sea, nothing is left out.
	std::vector<std::uint32_t> Keep;
	ftcore::SeaLeftOut UnderSea;
	ftcore::navPolysAboveSea(Corners, AllPolys, bSea ? SeaLevel : ftcore::kNoSea, ftcore::kNavSeaMargin, Keep, UnderSea);
	std::vector<ftcore::NavPolyRef> Polys;
	Polys.reserve(Keep.size());
	TArray<NavNodeRef> Refs;
	Refs.Reserve(static_cast<int32>(Keep.size()));
	TMap<NavNodeRef, uint32> IndexOf;
	IndexOf.Reserve(static_cast<int32>(Keep.size()));
	for (const std::uint32_t K : Keep)
	{
		IndexOf.Add(AllRefs[static_cast<int32>(K)], static_cast<uint32>(Polys.size()));
		Refs.Add(AllRefs[static_cast<int32>(K)]);
		Polys.push_back(AllPolys[K]);
	}
	std::vector<ftcore::NavLink> Links;
	TArray<NavNodeRef> Near;
	TArray<NavNodeRef> Through;
	for (int32 I = 0; I < Refs.Num(); ++I)
	{
		Near.Reset();
		if (!NavMesh.GetPolyNeighbors(Refs[I], Near))
		{
			continue;
		}
		for (const NavNodeRef N : Near)
		{
			ftcore::NavLink Link;
			Link.a = static_cast<uint32>(I);
			if (const uint32* J = IndexOf.Find(N))
			{
				Link.b = *J;
				Links.push_back(Link);
				continue;
			}
			// A ground poly left out under the sea joins nothing.
			if (AllIndex.Contains(N))
			{
				continue;
			}
			// Not a ground poly: an off-mesh link, joined to the ground polys at its ends.
			Through.Reset();
			if (NavMesh.GetPolyNeighbors(N, Through))
			{
				for (const NavNodeRef M : Through)
				{
					if (const uint32* J = IndexOf.Find(M))
					{
						Link.b = *J;
						Links.push_back(Link);
					}
				}
			}
		}
	}
	ftcore::NavIslandSet Set;
	ftcore::navIslands(Corners, Polys, Links, Set);

	const auto PolyAt = [&Nav, &NavMesh, &IndexOf](const FVector& At) -> std::int64_t {
		FNavLocation On;
		if (!Nav.ProjectPointToNavigation(At, On, kNavProjectExtent, &NavMesh))
		{
			return -1;
		}
		const uint32* I = IndexOf.Find(On.NodeRef);
		return I ? static_cast<std::int64_t>(*I) : -1;
	};
	const std::int64_t HomePoly = PolyAt(HomeAt);
	std::vector<std::int64_t> PointPolys;
	PointPolys.reserve(Points.size());
	for (const ftcore::CoveragePoint& Point : Points)
	{
		PointPolys.push_back(PolyAt(Cm(ftcore::massingEnginePoint(Side, Point.x, Point.y, Point.z))));
	}
	std::vector<ftcore::NavIslandInBoxes> InBoxes;
	ftcore::navIslandsInBoxes(Corners, Polys, Set, Solids, InBoxes);
	ftcore::CoverageSummary Sum;
	ftcore::classifyNavIslands(Set, HomePoly, Points, PointPolys, ftcore::kNavIslandBig, Sum, &InBoxes);

	const int32 Fails = static_cast<int32>(Sum.bigUndeclared + Sum.onMissed.size());
	const FString Home = Sum.home < 0
		? FString(TEXT("none (district 0's spawn is off the navmesh)"))
		: FString::Printf(TEXT("#%d, %.0f m2 (%.1f%% of it)"), static_cast<int32>(Sum.home) + 1, Sum.homeArea, Sum.homePercent);
	const FString SeaPart = bSea
		? FString::Printf(TEXT(" (under the sea at %.2f m, from %s: %d polys, %.0f m2, left out)"), SeaLevel, SeaFrom,
			  static_cast<int32>(UnderSea.polys), UnderSea.area)
		: FString();
	const FString Line = FString::Printf(TEXT("FTBuildMassingWorld: coverage: %d navmesh polys in %d tiles%s, %d islands, %.0f m2 walkable; district "
		"0's island %s; %d islands over %.0f m2 besides it: %d declared by a pocket, %d in built boxes (left out: %d roofs, %.0f m2, and %d "
		"interiors, %.0f m2), %d undeclared (%d of them roofs a walk surface meets); on-points on district 0's island %d of %d; pockets %d (%d "
		"in an island over the size, %d off the navmesh)."), static_cast<int32>(AllPolys.size()), Tiles.Num(), *SeaPart,
		static_cast<int32>(Set.islands.size()), Set.area, *Home, static_cast<int32>(Sum.big), ftcore::kNavIslandBig,
		static_cast<int32>(Sum.bigDeclared), static_cast<int32>(Sum.inBoxes.size()), static_cast<int32>(Sum.bigRoof), Sum.roofArea,
		static_cast<int32>(Sum.bigInterior), Sum.interiorArea, static_cast<int32>(Sum.bigUndeclared), static_cast<int32>(Sum.roofsMet),
		static_cast<int32>(Sum.onHome), static_cast<int32>(Sum.onPoints), static_cast<int32>(Sum.pockets), static_cast<int32>(Sum.pocketsMarking),
		static_cast<int32>(Sum.pocketsOffMesh));
	if (Fails == 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s"), *Line);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s"), *Line);
	}
	// The largest islands in built boxes, for a look at what was left out.
	for (size_t I = 0; I < Sum.inBoxes.size() && I < kCoverageInBoxLines; ++I)
	{
		const size_t K = Sum.inBoxes[I];
		const ftcore::NavIsland& Island = Set.islands[K];
		const std::int64_t Box = InBoxes[K].box;
		const FString BoxId = Box >= 0 && static_cast<size_t>(Box) < Solids.size() ? Utf8(Solids[static_cast<size_t>(Box)].id) : FString(TEXT("?"));
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: coverage: left out, the %s of %s and the boxes beside it: island #%d, %.0f m2, "
			"centroid (%.1f, %.1f), z %.1f to %.1f m, %d polys."), InBoxes[K].part == ftcore::NavBoxPart::Roof ? TEXT("roof") : TEXT("inside"), *BoxId,
			static_cast<int32>(K) + 1, Island.area, Island.cx, Island.cy, Island.zMin, Island.zMax, static_cast<int32>(Island.polys));
	}
	for (const size_t K : Sum.undeclared)
	{
		const ftcore::NavIsland& Island = Set.islands[K];
		const bool bMet = K < InBoxes.size() && InBoxes[K].met;
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: coverage: undeclared island #%d: %.0f m2, centroid (%.1f, %.1f), z %.1f to %.1f m, "
			"%d polys%s."), static_cast<int32>(K) + 1, Island.area, Island.cx, Island.cy, Island.zMin, Island.zMax, static_cast<int32>(Island.polys),
			bMet ? TEXT("; a roof a walk surface meets") : TEXT(""));
	}
	for (const size_t I : Sum.onMissed)
	{
		const ftcore::CoveragePoint& Point = Points[I];
		const std::int64_t Poly = PointPolys[I];
		const FString Where = Poly < 0
			? FString(TEXT("off the navmesh"))
			: FString::Printf(TEXT("on island #%d, not district 0's"), static_cast<int32>(Set.islandOf[static_cast<size_t>(Poly)]) + 1);
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: coverage: on-point %s (%.1f, %.1f, %+.1f) is %s."), *Utf8(Point.id), Point.x, Point.y,
			Point.z, *Where);
	}
	return Fails;
}

// -ReadBack: the built Landscape read back against a height field at the built field's spacing (the field it was built
// from, or a window of it; Window says where it lies, ftcore::readBackWindow). (1) Every sample of the window, read from
// the Landscape's heightmap through its edit data interface, must match the field's exactly. (2) At
// ftcore::kReadBackSurfacePoints fixed points between the samples, the collision surface's height
// (ALandscapeProxy::GetHeightAtLocation) must match the field's two triangles a cell, cut as the collision cuts them
// (north-west to south-east in the plan frame), within ftcore::kReadBackSurfaceTolerance. GetHeightAtLocation gives the
// collision's height as a world Z (cm): the engine moves the collision's local height through the component's transform,
// Z scale and all, so it is only turned into metres here. Logs a line for each; true when both hold. Allocates: build time
// only.
bool ReadBack(const ALandscape& Land, ULandscapeInfo& LandInfo, const FHeightField& Read, const ftcore::ReadBackWindow& Window, double Side)
{
	const size_t NX = Window.nx;
	const size_t NY = Window.ny;
	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	LandInfo.GetLandscapeExtent(MinX, MinY, MaxX, MaxY);
	const int32 X1 = MinX + static_cast<int32>(Window.x1);
	const int32 Y1 = MinY + static_cast<int32>(Window.y1);
	const int32 X2 = MinX + static_cast<int32>(Window.x2);
	const int32 Y2 = MinY + static_cast<int32>(Window.y2);
	TArray<uint16> Got;
	Got.SetNumUninitialized(static_cast<int32>(NX * NY));
	// A cell the read leaves unwritten keeps a value that can't match.
	ftcore::readBackSentinel(Read.Grid.GetData(), NX, NY, Got.GetData());
	{
		// The final heightmap (no edit layer named), read without writing anything back.
		FLandscapeEditDataInterface Edit(&LandInfo, FGuid(), false);
		Edit.GetHeightDataFast(X1, Y1, X2, Y2, Got.GetData(), 0);
	}
	ftcore::HeightCompare Exact;
	ftcore::compareHeightsExact(Read.Grid.GetData(), NX, NY, Read.Spacing, Read.OriginX, Read.OriginY, Got.GetData(), Exact);
	const bool bExact = Exact.mismatched == 0 && Exact.compared == NX * NY;
	const FString ExactLine = FString::Printf(TEXT("FTBuildMassingWorld: read-back: heightmap %d x %d samples from plan (%.1f, %.1f), Landscape "
		"vertices (%d, %d) to (%d, %d): %d of %d match exactly, %d don't; the worst is %.4f m at (%.1f, %.1f)."), static_cast<int32>(NX),
		static_cast<int32>(NY), Read.OriginX, Read.OriginY, X1, Y1, X2, Y2, static_cast<int32>(Exact.compared - Exact.mismatched),
		static_cast<int32>(NX * NY), static_cast<int32>(Exact.mismatched), Exact.worst, Exact.worstX, Exact.worstY);
	if (bExact)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s"), *ExactLine);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s"), *ExactLine);
	}

	std::vector<ftcore::SurfacePoint> Points;
	ftcore::readBackSurfacePoints(NX, NY, Read.Spacing, Read.OriginX, Read.OriginY, ftcore::kReadBackSurfacePoints, Points);
	std::vector<double> Measured(Points.size(), std::numeric_limits<double>::quiet_NaN());
	for (size_t I = 0; I < Points.size(); ++I)
	{
		const TOptional<float> Height = Land.GetHeightAtLocation(Cm(ftcore::massingEnginePoint(Side, Points[I].x, Points[I].y, 0.0)));
		if (Height.IsSet())
		{
			Measured[I] = static_cast<double>(Height.GetValue()) / 100.0;
		}
	}
	ftcore::SurfaceCompare Cut, Other;
	ftcore::compareSurface(Points, Measured, Read.Grid.GetData(), NX, NY, Read.Spacing, Read.OriginX, Read.OriginY,
		ftcore::CellDiagonal::NorthWestSouthEast, ftcore::kReadBackSurfaceTolerance, Cut);
	ftcore::compareSurface(Points, Measured, Read.Grid.GetData(), NX, NY, Read.Spacing, Read.OriginX, Read.OriginY,
		ftcore::CellDiagonal::SouthWestNorthEast, ftcore::kReadBackSurfaceTolerance, Other);
	const bool bSurface = Cut.over == 0 && Cut.compared == Points.size() && !Points.empty();
	const FString SurfaceLine = FString::Printf(TEXT("FTBuildMassingWorld: read-back: surface at %d points between the samples, cut north-west to "
		"south-east: %d within %.3f m, %d over (%d unread); the worst is %.4f m at (%.1f, %.1f). Cut south-west to north-east instead: %d over, "
		"the worst %.4f m."), static_cast<int32>(Points.size()), static_cast<int32>(Cut.compared - Cut.over), ftcore::kReadBackSurfaceTolerance,
		static_cast<int32>(Cut.over), static_cast<int32>(Cut.unread), Cut.worst, Cut.worstX, Cut.worstY, static_cast<int32>(Other.over), Other.worst);
	if (bSurface)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s"), *SurfaceLine);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s"), *SurfaceLine);
	}
	return bExact && bSurface;
}
#endif
} // namespace

UFTBuildMassingWorldCommandlet::UFTBuildMassingWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildMassingWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString HeightFile, BoxesFile, WallsFile, GoalsFile, KitFile, DumpFile, EntryText, LookText, WorldText;
	FString TourFile, CoverageFile, ReadBackFile;
	FString MapName = TEXT("World_Massing");
	FParse::Value(*Params, TEXT("Height="), HeightFile);
	FParse::Value(*Params, TEXT("Boxes="), BoxesFile);
	FParse::Value(*Params, TEXT("Walls="), WallsFile);
	FParse::Value(*Params, TEXT("Goals="), GoalsFile);
	FParse::Value(*Params, TEXT("Kit="), KitFile);
	FParse::Value(*Params, TEXT("Dump="), DumpFile);
	FParse::Value(*Params, TEXT("Tour="), TourFile);
	FParse::Value(*Params, TEXT("Coverage="), CoverageFile);
	FParse::Value(*Params, TEXT("ReadBack="), ReadBackFile);
	// The separator flag is off so "x,y,z" keeps its commas.
	FParse::Value(*Params, TEXT("Entry="), EntryText, false);
	FParse::Value(*Params, TEXT("Look="), LookText, false);
	FParse::Value(*Params, TEXT("Map="), MapName);
	FParse::Value(*Params, TEXT("World="), WorldText);
	double Side = 1000.0;
	FParse::Value(*Params, TEXT("Side="), Side);
	double SeaWest = 0.0;
	const bool bSea = FParse::Value(*Params, TEXT("Sea="), SeaWest);
	const bool bNav = !FParse::Param(*Params, TEXT("NoNav"));
	const bool bStrict = FParse::Param(*Params, TEXT("Strict"));
	// The reach check on the saved map as it stands: nothing is built, rebuilt or saved.
	const bool bLoad = FParse::Param(*Params, TEXT("Load"));

	double Entry[3] = {0.0, 0.0, 0.0};
	double Look[2] = {0.0, 0.0};
	const bool bLook = !LookText.IsEmpty();
	if (HeightFile.IsEmpty() || BoxesFile.IsEmpty() || !ParsePoint(EntryText, 3, Entry) || (bLook && !ParsePoint(LookText, 2, Look)) || Side <= 0.0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: needs -Height=<json> -Boxes=<csv> -Entry=<x,y,z> (and a -Look=<x,y> and -Side "
											  "that are well formed)."));
		return 1;
	}
	if (MapName.IsEmpty() || !FName::IsValidXName(MapName, INVALID_LONGPACKAGE_CHARACTERS))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: '%s' is not a valid map name."), *MapName);
		return 1;
	}

	FHeightField Field;
	std::vector<ftcore::MassingBox> Boxes;
	std::vector<ftcore::MassingBox> Walls;
	TArray<FGoal> Goals;
	TArray<FKitRow> KitRows;
	std::vector<ftcore::TourStop> TourStops;
	std::vector<ftcore::CoveragePoint> CoveragePoints;
	FHeightField ReadField;
	if (!LoadHeightField(HeightFile, Field) || !LoadBoxes(BoxesFile, Boxes) || (!WallsFile.IsEmpty() && !LoadBoxes(WallsFile, Walls)) ||
		(!GoalsFile.IsEmpty() && !LoadGoals(GoalsFile, Goals)) || (!KitFile.IsEmpty() && !LoadKit(KitFile, KitRows)) ||
		(!TourFile.IsEmpty() && !LoadTour(TourFile, TourStops)) || (!CoverageFile.IsEmpty() && !LoadCoverage(CoverageFile, CoveragePoints)) ||
		(!ReadBackFile.IsEmpty() && !bLoad && !LoadHeightField(ReadBackFile, ReadField)))
	{
		return 1;
	}
	for (const ftcore::MassingBox& Wall : Walls)
	{
		if (ftcore::massingRole(Wall) != ftcore::MassingRole::Wall)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s is not a WALL row."), *WallsFile, *Utf8(Wall.id));
			return 1;
		}
	}
	// The coverage check's solids: the boxes and the walls, of which ftcore::navIslandsInBoxes takes the built ones.
	std::vector<ftcore::MassingBox> Solids;
	if (!CoverageFile.IsEmpty())
	{
		Solids.reserve(Boxes.size() + Walls.size());
		Solids.insert(Solids.end(), Boxes.begin(), Boxes.end());
		Solids.insert(Solids.end(), Walls.begin(), Walls.end());
	}

	// Each kit row's box and mesh must be there before anything is built.
	for (FKitRow& Row : KitRows)
	{
		for (int32 I = 0; I < static_cast<int32>(Boxes.size()) && Row.BoxAt == INDEX_NONE; ++I)
		{
			Row.BoxAt = Utf8(Boxes[static_cast<size_t>(I)].id) == Row.Box ? I : INDEX_NONE;
		}
		if (Row.BoxAt == INDEX_NONE)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s is not in the massing table."), *KitFile, *Row.Box);
			return 1;
		}
		Row.Mesh = LoadObject<UStaticMesh>(nullptr, *Row.Asset);
		if (!Row.Mesh)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s names %s, which isn't a static mesh in the project."), *KitFile, *Row.Box,
				*Row.Asset);
			return 1;
		}
	}
	FAssetCompilingManager::Get().FinishAllCompilation();

	// The goals table's box ids name rows of the massing table. Each held goal and temporary placement is logged with its
	// box (a temporary spawn with the districts, below).
	int32 Counted = 0, GoalPlacements = 0, SpawnPlacements = 0;
	FString Apart;
	for (FGoal& Goal : Goals)
	{
		const FString* const Refs[3] = {&Goal.Held, &Goal.GoalBox, &Goal.SpawnBox};
		int32* const At[3] = {&Goal.HeldAt, &Goal.GoalBoxAt, &Goal.SpawnBoxAt};
		for (int32 C = 0; C < 3; ++C)
		{
			for (int32 I = 0; I < static_cast<int32>(Boxes.size()) && !Refs[C]->IsEmpty() && *At[C] == INDEX_NONE; ++I)
			{
				*At[C] = Utf8(Boxes[static_cast<size_t>(I)].id) == *Refs[C] ? I : INDEX_NONE;
			}
			if (!Refs[C]->IsEmpty() && *At[C] == INDEX_NONE)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: goal %s names %s, which is not in the massing table."), *GoalsFile,
					*Goal.Id, **Refs[C]);
				return 1;
			}
		}
		SpawnPlacements += Goal.SpawnBoxAt != INDEX_NONE ? 1 : 0;
		if (Goal.HeldAt != INDEX_NONE)
		{
			const ftcore::MassingBox& Box = Boxes[static_cast<size_t>(Goal.HeldAt)];
			const double Gap = ftcore::massingFootprintGap(Box, Goal.X, Goal.Y);
			Apart += FString::Printf(TEXT("%s%s on %s"), Apart.IsEmpty() ? TEXT("") : TEXT(", "), *Goal.Id, *Goal.Held);
			if (Gap > 0.0)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: goal %s (%.1f, %.1f, %+.1f) is listed apart for held box %s, but stands "
					"%.1f m off its footprint."), *Goal.Id, Goal.X, Goal.Y, Goal.Z, *Goal.Held, Gap);
			}
			else
			{
				UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: goal %s (%.1f, %.1f, %+.1f) is listed apart: inside or on held box %s (%s), "
					"so it is not walked or counted."), *Goal.Id, Goal.X, Goal.Y, Goal.Z, *Goal.Held, BoxKind(Box));
			}
			continue;
		}
		++Counted;
		if (Goal.GoalBoxAt != INDEX_NONE)
		{
			++GoalPlacements;
			const ftcore::MassingBox& Box = Boxes[static_cast<size_t>(Goal.GoalBoxAt)];
			FString Where;
			if (Against(Box, Goal.X, Goal.Y, Goal.Z, Where))
			{
				UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: goal %s (%.1f, %.1f, %+.1f) is a temporary placement outside %s (%s, which "
					"holds the plan's own point): %s."), *Goal.Id, Goal.X, Goal.Y, Goal.Z, *Goal.GoalBox, BoxKind(Box), *Where);
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: goal %s (%.1f, %.1f, %+.1f) is listed as a temporary placement outside %s, "
					"but stands inside it."), *Goal.Id, Goal.X, Goal.Y, Goal.Z, *Goal.GoalBox);
			}
		}
	}

	// With -World: the world's districts where the plan puts them. The goals table's rows D0, D1, ... are their plazas'
	// centres and floors; each spawn is the row's spawn_x, spawn_y or else the generator's rule (spawnFor), set down on
	// the highest deck or landing within kSpawnStepUp, and the lot is pinned here as the director pins it at run time.
	struct FDistrictPlace
	{
		double CentreX = 0.0, CentreY = 0.0, Floor = 0.0; // plan frame, m
		double SpawnX = 0.0, SpawnY = 0.0, SpawnFloor = 0.0;
		int32 OnBox = -1;  // the box the spawn stands on, or -1 for the ground
		int32 Inside = -1; // a box (or with bInsideWall, a wall) the lead would stand inside at the spawn
		bool bInsideWall = false;
		ftcore::MassingFooting Footing; // how far the floor steps within the lead's reach of the spawn
		double RuleX = 0.0, RuleY = 0.0; // the generator's spawn (spawnFor), plan frame
		int32 SpawnBox = INDEX_NONE;     // the row's spawn_box: the spawn is a temporary placement outside it
	};
	TArray<FDistrictPlace> Places;
	int32 FootingHeld = 0; // district spawns whose footing is within ftcore::kMassingFootingTolerance
	FString FootingLine;   // the reach summary's count of them
	FString WorldId;
	// What the lead stands on and walks into: the boxes the player sees, not the hidden kinds or references.
	std::vector<bool> Visible(Boxes.size(), false);
	for (size_t I = 0; I < Boxes.size(); ++I)
	{
		Visible[I] = ftcore::massingVisible(ftcore::massingRole(Boxes[I]));
	}
	const auto FootingAt = [&](double X, double Y, double Floor, double Ceiling)
	{
		return ftcore::massingSpawnFooting(Field.Grid.GetData(), static_cast<size_t>(Field.SamplesX), static_cast<size_t>(Field.SamplesY),
			Field.Spacing, Field.OriginX, Field.OriginY, Boxes, Visible, X, Y, Floor, Ceiling, kLeadHeight);
	};
	if (!WorldText.IsEmpty())
	{
		TArray<uint8> WorldBytes;
		ftcore::WorldTable Worlds;
		std::string Error;
		if (!LoadBytes(FPaths::ProjectDir() / TEXT("Data/Field/worlds.json"), WorldBytes) ||
			!ftcore::importWorldTable(View(WorldBytes), Worlds, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: Data/Field/worlds.json refused: %s"), *Utf8(Error).TrimEnd());
			return 1;
		}
		const FTCHARToUTF8 WorldUtf8(*WorldText);
		const int32 Index = Worlds.findWorld(std::string_view(WorldUtf8.Get(), static_cast<size_t>(WorldUtf8.Length())));
		if (Index < 0)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -World=%s is not in Data/Field/worlds.json."), *WorldText);
			return 1;
		}
		const ftcore::WorldSpec& Spec = Worlds.worlds[static_cast<size_t>(Index)];
		WorldId = Utf8(Spec.id);
		const int32 Count = static_cast<int32>(Spec.districts);
		if (Count < 1 || Count > static_cast<int32>(ftcore::WorldLayout::kMaxDistricts))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: world %s has %d districts."), *WorldId, Count);
			return 1;
		}
		const std::vector<bool> EveryWall(Walls.size(), true);
		ftcore::DistrictPin Pins[ftcore::WorldLayout::kMaxDistricts];
		for (int32 D = 0; D < Count; ++D)
		{
			const FString RowId = FString::Printf(TEXT("D%d"), D);
			const FGoal* Row = Goals.FindByPredicate([&](const FGoal& G) { return G.Id == RowId; });
			if (!Row)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -World=%s needs the goals table's row %s (district %d's plaza centre)."),
					*WorldId, *RowId, D);
				return 1;
			}
			FDistrictPlace Place;
			Place.CentreX = Row->X;
			Place.CentreY = Row->Y;
			Place.Floor = Row->Z;
			const ftcore::MassingVec Centre = ftcore::massingEnginePoint(Side, Row->X, Row->Y, 0.0);
			const ftcore::Vec2 Rule = ftcore::WorldLayout::spawnFor({static_cast<float>(Centre.x), static_cast<float>(Centre.y)});
			Place.RuleX = Rule.x + Side * 0.5; // back to the plan frame
			Place.RuleY = Side * 0.5 - Rule.y;
			Place.SpawnX = Row->bSpawn ? Row->SpawnX : Place.RuleX;
			Place.SpawnY = Row->bSpawn ? Row->SpawnY : Place.RuleY;
			Place.SpawnBox = Row->SpawnBoxAt;
			const double Ground = ftcore::massingGroundAt(Field.Grid.GetData(), static_cast<size_t>(Field.SamplesX),
				static_cast<size_t>(Field.SamplesY), Field.Spacing, Field.OriginX, Field.OriginY, Place.SpawnX, Place.SpawnY);
			int OnBox = -1;
			const double Ceiling = FMath::Max(Place.Floor, Ground) + kSpawnStepUp;
			Place.SpawnFloor = ftcore::massingFloorAt(Boxes, Visible, Place.SpawnX, Place.SpawnY, Ground, Ceiling, &OnBox);
			Place.OnBox = OnBox;
			Place.Inside = ftcore::massingBlockingAt(Boxes, Visible, Place.SpawnX, Place.SpawnY, Place.SpawnFloor, kLeadHeight);
			if (Place.Inside < 0)
			{
				Place.Inside = ftcore::massingBlockingAt(Walls, EveryWall, Place.SpawnX, Place.SpawnY, Place.SpawnFloor, kLeadHeight);
				Place.bInsideWall = Place.Inside >= 0;
			}
			Place.Footing = FootingAt(Place.SpawnX, Place.SpawnY, Place.SpawnFloor, Ceiling);
			const ftcore::MassingVec SpawnAt = ftcore::massingEnginePoint(Side, Place.SpawnX, Place.SpawnY, 0.0);
			Pins[D].centre = {static_cast<float>(Centre.x), static_cast<float>(Centre.y)};
			Pins[D].spawn = {static_cast<float>(SpawnAt.x), static_cast<float>(SpawnAt.y)};
			Pins[D].hasSpawn = true;
			Places.Add(Place);
		}
		TUniquePtr<ftcore::WorldLayout> Layout = MakeUnique<ftcore::WorldLayout>();
		if (!ftcore::generateWorldLayout(Spec, *Layout, &Error) || !Layout->pinDistricts(Pins, static_cast<size_t>(Count), &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: world %s's districts don't pin: %s"), *WorldId, *Utf8(Error).TrimEnd());
			return 1;
		}
		if (Spec.side != Side)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: world %s's layout is %.0f m across, the plan %.0f m."), *WorldId,
				Spec.side, Side);
		}
		for (int32 D = 0; D < Count; ++D)
		{
			const FDistrictPlace& Place = Places[D];
			const FString On = Place.OnBox >= 0 ? Utf8(Boxes[static_cast<size_t>(Place.OnBox)].id) : FString(TEXT("the ground"));
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: district %d: plaza (%.1f, %.1f) at %.1f m; spawn (%.1f, %.1f) on %s at %.1f m, "
				"footing %.2f m (%d of %d points skipped as walls)."), D, Place.CentreX, Place.CentreY, Place.Floor, Place.SpawnX, Place.SpawnY, *On,
				Place.SpawnFloor, Place.Footing.step, Place.Footing.skipped, ftcore::kMassingFootingPoints);
			if (ftcore::massingFootingHolds(Place.Footing))
			{
				++FootingHeld;
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: district %d's spawn footing steps %.2f m at (%.2f, %.2f), over %.2f m: the "
					"lead slides off the spawn, and a path asked to it finds no navmesh under it; the goals table can name its spawn_x, spawn_y."),
					D, Place.Footing.step, Place.Footing.x, Place.Footing.y, ftcore::kMassingFootingTolerance);
			}
			if (Place.Inside >= 0)
			{
				const ftcore::MassingBox& In = Place.bInsideWall ? Walls[static_cast<size_t>(Place.Inside)] : Boxes[static_cast<size_t>(Place.Inside)];
				UE_LOG(LogForgottenThird, Warning,
					TEXT("FTBuildMassingWorld: district %d's spawn stands inside %s (%.1f to %.1f m); the goals table can name its spawn_x, spawn_y."),
					D, *Utf8(In.id), In.z0, In.z1);
			}
			if (Place.SpawnBox != INDEX_NONE)
			{
				// The reason the spawn was placed by hand: the generator's own would stand in this box.
				const ftcore::MassingBox& Box = Boxes[static_cast<size_t>(Place.SpawnBox)];
				const double RuleGap = ftcore::massingFootprintGap(Box, Place.RuleX, Place.RuleY);
				FString Where;
				const bool bOutside = Against(Box, Place.SpawnX, Place.SpawnY, Place.SpawnFloor, Where);
				if (RuleGap > 0.0 || !bOutside)
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: district %d's spawn (%.1f, %.1f) is listed as a temporary placement outside "
						"%s, but the generator's spawn (%.1f, %.1f) is %.1f m off its footprint and the placed one is %s."), D, Place.SpawnX,
						Place.SpawnY, *Utf8(Box.id), Place.RuleX, Place.RuleY, RuleGap, *Where);
				}
				else
				{
					UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: district %d's spawn (%.1f, %.1f) is a temporary placement outside %s (%s): "
						"the generator's spawn (%.1f, %.1f) stands in its footprint; the placed one is %s."), D, Place.SpawnX, Place.SpawnY,
						*Utf8(Box.id), BoxKind(Box), Place.RuleX, Place.RuleY, *Where);
				}
			}
		}
		int32 WorstAt = 0;
		for (int32 D = 1; D < Count; ++D)
		{
			WorstAt = Places[D].Footing.step > Places[WorstAt].Footing.step ? D : WorstAt;
		}
		FootingLine = FString::Printf(TEXT("; spawn footing within %.2f m: %d of %d"), ftcore::kMassingFootingTolerance, FootingHeld, Count);
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: spawn footing (%d points within %.1f m, tolerance %.2f m): %d of %d district "
			"spawns hold; the most is district %d's, %.2f m."), ftcore::kMassingFootingPoints, ftcore::kMassingFootingRadius,
			ftcore::kMassingFootingTolerance, FootingHeld, Count, WorstAt, Places[WorstAt].Footing.step);
	}

	// -Tour and -Coverage work from the districts: the tour is the director's, and coverage starts from district 0's spawn.
	if (Places.IsEmpty() && (!TourFile.IsEmpty() || !CoverageFile.IsEmpty()))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -Tour and -Coverage need -World (the director, and district 0's spawn)."));
		return 1;
	}
	if (!TourFile.IsEmpty())
	{
		int32 Kinds[ftcore::kTourStopKinds] = {};
		int32 Stills = 0;
		for (const ftcore::TourStop& Stop : TourStops)
		{
			++Kinds[static_cast<size_t>(Stop.kind)];
			Stills += Stop.still ? 1 : 0;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: tour: %d stops after the districts: %d reach, %d edge, %d push; %d end in a still."),
			static_cast<int32>(TourStops.size()), Kinds[0], Kinds[1], Kinds[2], Stills);
	}
	if (bLoad && !ReadBackFile.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -Load: -ReadBack is for a build; ignored."));
	}

	const FString MapPath = FString::Printf(TEXT("/Game/Maps/%s"), *MapName);
	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	const FVector EntryAt = Cm(ftcore::massingEnginePoint(Side, Entry[0], Entry[1], Entry[2]));
	// The entry is the player start too: it should stand on its own floor, with a footing that holds.
	{
		const double Ground = ftcore::massingGroundAt(Field.Grid.GetData(), static_cast<size_t>(Field.SamplesX),
			static_cast<size_t>(Field.SamplesY), Field.Spacing, Field.OriginX, Field.OriginY, Entry[0], Entry[1]);
		const double Ceiling = FMath::Max(Entry[2], Ground) + kSpawnStepUp;
		const double Floor = ftcore::massingFloorAt(Boxes, Visible, Entry[0], Entry[1], Ground, Ceiling);
		const ftcore::MassingFooting Footing = FootingAt(Entry[0], Entry[1], Floor, Ceiling);
		const double Off = FMath::Abs(Entry[2] - Floor);
		const FString Line = FString::Printf(TEXT("FTBuildMassingWorld: the entry (%.1f, %.1f, %.1f) stands on %.2f m, %.2f m from its z; footing "
			"%.2f m at (%.2f, %.2f)"), Entry[0], Entry[1], Entry[2], Floor, Off, Footing.step, Footing.x, Footing.y);
		if (ftcore::massingFootingHolds(Footing) && Off <= ftcore::kMassingFootingTolerance)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s."), *Line);
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s, over %.2f m: the player start drops or slides there, and the reach check starts off its floor."),
				*Line, ftcore::kMassingFootingTolerance);
		}
	}

	// With -Load: the saved map in an editor world of its own, with the navmesh saved with it, which is the one the game
	// walks. The world is destroyed unsaved; the map's file isn't written. Only a map saved without navmesh tiles has
	// them built, in memory.
	if (bLoad)
	{
		if (!IFileManager::Get().FileExists(*File))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -Load: there is no %s."), *File);
			return 1;
		}
		UWorld::InitializationValues Init;
		Init.RequiresHitProxies(false)
			.ShouldSimulatePhysics(false)
			.EnableTraceCollision(false)
			.CreateNavigation(true)
			.CreateAISystem(false)
			.AllowAudioPlayback(false)
			.CreatePhysicsScene(true);
		FScopedEditorWorld Loaded(FStringView(MapPath), Init);
		UWorld* World = Loaded.GetWorld();
		if (!World)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -Load: couldn't load %s."), *MapPath);
			return 1;
		}
		FNavigationSystem::AddNavigationSystemToWorld(*World, FNavigationSystemRunMode::EditorMode);
		UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		ARecastNavMesh* NavMesh = Nav ? Cast<ARecastNavMesh>(Nav->GetDefaultNavDataInstance(FNavigationSystem::DontCreate)) : nullptr;
		const bool bRegistered = NavMesh != nullptr;
		for (TActorIterator<ARecastNavMesh> It(World); It && !NavMesh; ++It)
		{
			NavMesh = *It;
		}
		if (!Nav || !NavMesh)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -Load: %s has no navigation system or no navmesh."), *MapPath);
			return 1;
		}
		const int32 Tiles = NavMesh->GetNavMeshTilesCount();
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -Load: loaded %s, not to be saved; its navmesh (%s) has %d tiles."), *MapPath,
			bRegistered ? TEXT("the navigation system's own") : TEXT("found among the actors"), Tiles);
		if (Tiles == 0)
		{
			Nav->RemoveNavigationBuildLock(ENavigationBuildLock::AsyncLoadLock, UNavigationSystemV1::ELockRemovalRebuildAction::NoRebuild);
			Nav->Build();
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: -Load: the saved map has no navmesh tiles; built %d in memory, not saved."),
				NavMesh->GetNavMeshTilesCount());
		}

		// The spawns the tour walks to are the director's, saved with the map; the check walks the plan's, worked out
		// above. They should be the same points.
		const AFTExploreDirector* Director = nullptr;
		for (TActorIterator<AFTExploreDirector> It(World); It && !Director; ++It)
		{
			Director = *It;
		}
		if (!Places.IsEmpty())
		{
			double Worst = 0.0;
			const int32 Saved = Director ? Director->DistrictSpawns.Num() : 0;
			for (int32 D = 0; D < FMath::Min(Saved, Places.Num()); ++D)
			{
				const FVector Plan = Cm(ftcore::massingEnginePoint(Side, Places[D].SpawnX, Places[D].SpawnY, Places[D].SpawnFloor));
				Worst = FMath::Max(Worst, FVector::Dist(Plan, Director->DistrictSpawns[D]) / 100.0);
			}
			if (Saved == Places.Num() && Worst <= 1.0)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -Load: the map's director has the plan's %d district spawns, the "
					"furthest %.2f m from the plan's."), Saved, Worst);
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: -Load: the map's director has %d district spawns, the plan %d; the "
					"furthest is %.2f m from the plan's, so the check doesn't walk to the spawns the tour walks to."), Saved, Places.Num(), Worst);
			}
		}
		// The tour's stops are the director's too, saved with the map; -Load doesn't change them.
		if (!TourFile.IsEmpty())
		{
			const int32 SavedStops = Director ? Director->TourStopIds.Num() : 0;
			if (SavedStops == static_cast<int32>(TourStops.size()))
			{
				UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -Load: the map's director has the table's %d tour stops."), SavedStops);
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: -Load: the map's director has %d tour stops, the table %d; build the map "
					"again with -Tour for the tour to walk the table's."), SavedStops, static_cast<int32>(TourStops.size()));
			}
		}

		FReachTally GoalTally, SpawnTally;
		FNavLocation From;
		if (!Nav->ProjectPointToNavigation(EntryAt, From, kNavProjectExtent, NavMesh))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: the entry (%.1f, %.1f, %.1f) is not on the navmesh."), Entry[0], Entry[1],
				Entry[2]);
		}
		else
		{
			const FSharedConstNavQueryFilter Filter = ReachFilter(*NavMesh);
			for (const FGoal& Goal : Goals)
			{
				// A held goal is listed apart (logged with the table), never walked or counted.
				if (Goal.HeldAt == INDEX_NONE)
				{
					WalkFromEntry(*Nav, *NavMesh, Filter, From, Side, TEXT("goal ") + Goal.Id, Goal.X, Goal.Y, Goal.Z, GoalTally);
				}
			}
			for (int32 D = 0; D < Places.Num(); ++D)
			{
				const FDistrictPlace& Place = Places[D];
				WalkFromEntry(*Nav, *NavMesh, Filter, From, Side, FString::Printf(TEXT("district %d's spawn"), D), Place.SpawnX, Place.SpawnY,
					Place.SpawnFloor, SpawnTally);
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: reach (search budget %u nodes): %s; %s%s."), kReachSearchNodes,
			*ReachLine(TEXT("goals"), GoalTally, Counted), *ReachLine(TEXT("district spawns"), SpawnTally, Places.Num()), *FootingLine);
		// The sea for the coverage check: -Sea=<x> when given (the plane stands at the datum), else the saved director's.
		const bool bSavedSea = !bSea && Director && Director->bHasSeaLevel;
		const double LoadSeaLevel = bSea ? 0.0 : bSavedSea ? static_cast<double>(Director->SeaLevel) / 100.0 : ftcore::kNoSea;
		const TCHAR* const LoadSeaFrom = bSea ? TEXT("-Sea") : TEXT("the saved director");
		const int32 LoadCoverageFails = CoverageFile.IsEmpty()
			? 0
			: CheckCoverage(*Nav, *NavMesh, Side, Cm(ftcore::massingEnginePoint(Side, Places[0].SpawnX, Places[0].SpawnY, Places[0].SpawnFloor)),
				  CoveragePoints, Solids, bSea || bSavedSea, LoadSeaLevel, LoadSeaFrom);
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -Load: %s checked, not saved: goals reached %d of %d, district spawns reached %d of %d."),
			*MapPath, GoalTally.Reached, Counted, SpawnTally.Reached, Places.Num());
		const bool bLoadShort =
			GoalTally.Reached < Counted || SpawnTally.Reached < Places.Num() || FootingHeld < Places.Num() || LoadCoverageFails > 0;
		return bStrict && bLoadShort ? 1 : 0;
	}

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Base = FTScene::LoadSurface();
	if (!Cube || !Plane || !Base)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: a /Engine/BasicShapes mesh or the world surface material is missing."));
		return 1;
	}

	// A new editor world (so it gets a navigation system), replacing the map.
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(*MapPath);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, *MapName, Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->DefaultGameMode = AFTExploreGameMode::StaticClass();
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Flat tints until the surfaces and the kit come in.
	UMaterialInterface* const Finish[3] = {
		Tint(Package, Base, TEXT("MI_Massing_Plain"), FLinearColor(0.42f, 0.41f, 0.39f)),
		Tint(Package, Base, TEXT("MI_Massing_Wall"), FLinearColor(0.30f, 0.29f, 0.28f)),
		Tint(Package, Base, TEXT("MI_Massing_Deck"), FLinearColor(0.33f, 0.27f, 0.20f)),
	};
	UMaterialInterface* GroundTint = Tint(Package, Base, TEXT("MI_Massing_Ground"), FLinearColor(0.22f, 0.24f, 0.18f));
	// Still water: dark, and smooth enough to carry the sky's reflection.
	UMaterialInterface* SeaTint = Tint(Package, Base, TEXT("MI_Massing_Sea"), FLinearColor(0.05f, 0.11f, 0.14f), 0.05f);

	// The ground: the height field as a Landscape, padded east and north to whole components, its rows north first.
	const size_t LandSide = ftcore::massingLandscapeSide(static_cast<size_t>(Field.SamplesX), static_cast<size_t>(Field.SamplesY),
		static_cast<size_t>(kLandSubsections * kLandSubsectionQuads));
	const int32 Samples = static_cast<int32>(LandSide);
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(Samples * Samples);
	if (!ftcore::massingLandscapeHeights(Field.Grid.GetData(), static_cast<size_t>(Field.SamplesX), static_cast<size_t>(Field.SamplesY), LandSide,
			Heights.GetData()))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: the height field doesn't fit a %d-sample Landscape."), Samples);
		World->DestroyWorld(false);
		return 1;
	}
	// -ReadBack: where its field lies in the Landscape, before anything is built on it.
	ftcore::ReadBackWindow ReadWindow;
	if (!ReadBackFile.IsEmpty())
	{
		std::string Error;
		if (!ftcore::readBackWindow(LandSide, static_cast<size_t>(Field.SamplesX), static_cast<size_t>(Field.SamplesY), Field.Spacing, Field.OriginX,
				Field.OriginY, static_cast<size_t>(ReadField.SamplesX), static_cast<size_t>(ReadField.SamplesY), ReadField.Spacing, ReadField.OriginX,
				ReadField.OriginY, ReadWindow, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -ReadBack: %s."), *Utf8(Error));
			World->DestroyWorld(false);
			return 1;
		}
	}
	uint16 Lowest = 0xFFFF;
	uint16 Highest = 0;
	for (const uint16 H : Field.Grid)
	{
		Lowest = FMath::Min(Lowest, H);
		Highest = FMath::Max(Highest, H);
	}
	const ftcore::MassingVec Corner = ftcore::massingLandscapeCorner(Side, Field.OriginX, Field.OriginY, Field.Spacing, LandSide);
	ALandscape* Land = World->SpawnActor<ALandscape>(Cm(Corner), FRotator::ZeroRotator, Spawn);
	Land->LandscapeMaterial = GroundTint;
	Land->SetActorRelativeScale3D(FVector(Field.Spacing * 100.0, Field.Spacing * 100.0, 100.0));
	{
		TMap<FGuid, TArray<uint16>> HeightLayers;
		HeightLayers.Add(FGuid(), MoveTemp(Heights));
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> PaintLayers;
		PaintLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
		Land->Import(FGuid::NewGuid(), 0, 0, Samples - 1, Samples - 1, kLandSubsections, kLandSubsectionQuads, HeightLayers, TEXT(""), PaintLayers,
			ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	}
	ULandscapeInfo* LandInfo = Land->GetLandscapeInfo();
	if (!LandInfo)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: the ground's Landscape has no info after its import."));
		World->DestroyWorld(false);
		return 1;
	}
	LandInfo->UpdateLayerInfoMap(Land);
	Land->SetActorLabel(TEXT("Ground"));
	{
		// What the Landscape came to, padding included, read back from it rather than from the height field.
		FIntRect Quads;
		LandInfo->GetLandscapeExtent(Quads);
		const FBox Bounds = Land->GetComponentsBoundingBox(true);
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: ground %d x %d quads of %.1f m, %.1f by %.1f m; plan x %.1f to %.1f, y %.1f to %.1f."),
			Quads.Width(), Quads.Height(), Field.Spacing, Bounds.GetSize().X / 100.0, Bounds.GetSize().Y / 100.0, Bounds.Min.X / 100.0 + Side * 0.5,
			Bounds.Max.X / 100.0 + Side * 0.5, Side * 0.5 - Bounds.Max.Y / 100.0, Side * 0.5 - Bounds.Min.Y / 100.0);
	}

	// The boxes: each built one as an actor labelled with its id.
	std::vector<bool> Flat(Boxes.size(), false);
	for (size_t I = 0; I < Boxes.size(); ++I)
	{
		Flat[I] = ftcore::massingVisible(ftcore::massingRole(Boxes[I])) && !Boxes[I].sloped;
	}
	std::vector<double> Nudges;
	ftcore::massingTopNudges(Boxes, Flat, Nudges);
	std::vector<bool> Replaced(Boxes.size(), false);
	for (const FKitRow& Row : KitRows)
	{
		Replaced[static_cast<size_t>(Row.BoxAt)] = true;
	}
	int32 Named = 0, Kit = 0, Sloped = 0, Nudged = 0, Skipped = 0, BoxesLeft = 0;
	for (size_t I = 0; I < Boxes.size(); ++I)
	{
		const ftcore::MassingBox& Box = Boxes[I];
		const ftcore::MassingRole Role = ftcore::massingRole(Box);
		if (!ftcore::massingVisible(Role))
		{
			// Added walls belong in the walls table; the rest are references.
			++Skipped;
			continue;
		}
		(Role == ftcore::MassingRole::Named ? Named : Kit) += 1;
		BoxesLeft += Replaced[I] ? 0 : 1;
		UMaterialInterface* Material = Finish[static_cast<int32>(ftcore::massingFinish(Box))];
		// A box a finished mesh stands in for stays as its hidden collider: an imported mesh may bring no simple
		// collision, and the lead would fall through it.
		const FString Id = Replaced[I] ? Utf8(Box.id) + TEXT("-COL") : Utf8(Box.id);
		if (Box.sloped)
		{
			ftcore::MassingMesh Shape;
			ftcore::massingMesh(Box, Side, 0.0, Shape);
			double Bottom = Shape.v[0].z;
			for (const ftcore::MassingVec& V : Shape.v)
			{
				Bottom = FMath::Min(Bottom, V.z);
			}
			const FVector Pivot = Cm(ftcore::massingEnginePoint(Side, Box.cx, Box.cy, Bottom));
			UStaticMesh* Mesh = SlopedMesh(Package, Id, Shape, Pivot, Material);
			AStaticMeshActor* Built = Block(World, Spawn, Mesh, nullptr, Id, Pivot, FVector::OneVector, 0.0);
			if (Replaced[I])
			{
				Built->GetStaticMeshComponent()->SetCastShadow(false);
				Built->SetActorHiddenInGame(true);
			}
			++Sloped;
			continue;
		}
		const double Height = FMath::Max(Box.z1 + Nudges[I] - Box.z0, ftcore::kMassingMinHeight);
		Nudged += Nudges[I] < 0.0 ? 1 : 0;
		AStaticMeshActor* Built = Block(World, Spawn, Cube, Material, Id, Cm(ftcore::massingEnginePoint(Side, Box.cx, Box.cy, Box.z0 + Height * 0.5)),
			FVector(Box.sx, Box.sy, Height), ftcore::massingEngineYaw(Box.rot));
		if (Replaced[I])
		{
			Built->GetStaticMeshComponent()->SetCastShadow(false);
			Built->SetActorHiddenInGame(true);
		}
	}

	// The kit: each row's mesh in its box's stead. It collides with nothing, its box stopping the lead instead; a box the
	// build doesn't make leaves its mesh without a collider, and says so.
	int32 KitPlaced = 0, KitNoCollider = 0;
	TMap<int32, int32> KitOnBox;
	TMap<FString, int32> KitPerAsset;
	for (const FKitRow& Row : KitRows)
	{
		const ftcore::MassingBox& Box = Boxes[static_cast<size_t>(Row.BoxAt)];
		const FBox Bounds = Row.Mesh->GetBoundingBox();
		const ftcore::MassingVec MeshMin{Bounds.Min.X / 100.0, Bounds.Min.Y / 100.0, Bounds.Min.Z / 100.0};
		const ftcore::MassingVec MeshMax{Bounds.Max.X / 100.0, Bounds.Max.Y / 100.0, Bounds.Max.Z / 100.0};
		ftcore::MassingKitPlace Place;
		if (!ftcore::massingKitPlacement(Box, Side, MeshMin, MeshMax, Row.bFit, Row.Turn, Row.Dz, Place))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s has no size to place (%.2f x %.2f x %.2f m)."), *Row.Box, *Row.Asset,
				MeshMax.x - MeshMin.x, MeshMax.y - MeshMin.y, MeshMax.z - MeshMin.z);
			World->DestroyWorld(false);
			return 1;
		}
		// A second mesh on one box takes a suffix, as an actor's label is its id.
		const int32 OnBox = ++KitOnBox.FindOrAdd(Row.BoxAt);
		const FString Label = OnBox == 1 ? Row.Box : FString::Printf(TEXT("%s-%d"), *Row.Box, OnBox);
		AStaticMeshActor* Prop = Block(World, Spawn, Row.Mesh, nullptr, Label, Cm(Place.at), FVector(Place.scale.x, Place.scale.y, Place.scale.z), Place.yaw);
		Prop->GetStaticMeshComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Prop->GetStaticMeshComponent()->SetCanEverAffectNavigation(false);
		const bool bCollider = ftcore::massingVisible(ftcore::massingRole(Box));
		KitNoCollider += bCollider ? 0 : 1;
		++KitPlaced;
		++KitPerAsset.FindOrAdd(Row.Asset);
		UE_LOG(LogForgottenThird, Display,
			TEXT("FTBuildMassingWorld: kit %s: %s, %.1f x %.1f x %.1f m in a box %.1f x %.1f m, z %.1f to %.1f; scale %.2f, %.2f, %.2f, yaw %.1f, "
				 "bottom at %.1f m%s."),
			*Label, *Row.Asset, MeshMax.x - MeshMin.x, MeshMax.y - MeshMin.y, MeshMax.z - MeshMin.z, Box.sx, Box.sy, Box.z0, Box.z1, Place.scale.x,
			Place.scale.y, Place.scale.z, Place.yaw, Place.at.z + MeshMin.z * Place.scale.z, bCollider ? TEXT("") : TEXT("; its box isn't built, so no collider"));
	}
	KitPerAsset.KeySort(TLess<FString>());
	for (const TPair<FString, int32>& Pair : KitPerAsset)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: kit %s stands in for %d boxes."), *Pair.Key, Pair.Value);
	}

	// The hidden walls: they stop pawns but no traces on Visibility, and not the camera's arm.
	double NavMinX = 0.0, NavMaxX = Side, NavMinY = 0.0, NavMaxY = Side;
	for (const ftcore::MassingBox& Wall : Walls)
	{
		AStaticMeshActor* Blocker = Block(World, Spawn, Cube, nullptr, Utf8(Wall.id),
			Cm(ftcore::massingEnginePoint(Side, Wall.cx, Wall.cy, (Wall.z0 + Wall.z1) * 0.5)),
			FVector(Wall.sx, Wall.sy, FMath::Max(Wall.z1 - Wall.z0, ftcore::kMassingMinHeight)), ftcore::massingEngineYaw(Wall.rot));
		UStaticMeshComponent* Mesh = Blocker->GetStaticMeshComponent();
		Mesh->SetCollisionProfileName(TEXT("InvisibleWall"));
		Mesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		Mesh->SetCastShadow(false);
		Blocker->SetActorHiddenInGame(true);
		Blocker->Tags.Add(kHiddenWallTag);
		// The navmesh reaches the walls' outer faces (the footprint's bounding box), and no further.
		const double Cos = FMath::Abs(FMath::Cos(FMath::DegreesToRadians(Wall.rot)));
		const double Sin = FMath::Abs(FMath::Sin(FMath::DegreesToRadians(Wall.rot)));
		const double HalfX = 0.5 * (Cos * Wall.sx + Sin * Wall.sy);
		const double HalfY = 0.5 * (Sin * Wall.sx + Cos * Wall.sy);
		NavMinX = FMath::Min(NavMinX, Wall.cx - HalfX);
		NavMaxX = FMath::Max(NavMaxX, Wall.cx + HalfX);
		NavMinY = FMath::Min(NavMinY, Wall.cy - HalfY);
		NavMaxY = FMath::Max(NavMaxY, Wall.cy + HalfY);
	}

	// The sea at the datum, from its west edge out past the horizon. Nothing collides with it.
	if (bSea)
	{
		AStaticMeshActor* Sea = Block(World, Spawn, Plane, SeaTint, TEXT("Sea"),
			Cm(ftcore::massingEnginePoint(Side, SeaWest + kSeaReach * 0.5, Side * 0.5, 0.0)), FVector(kSeaReach, 2.0 * kSeaReach, 1.0), 0.0);
		Sea->GetStaticMeshComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Sea->GetStaticMeshComponent()->SetCanEverAffectNavigation(false);
	}

	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, kMassingSun);

	double EntryYaw = 0.0;
	if (bLook)
	{
		const ftcore::MassingVec To = ftcore::massingEnginePoint(Side, Look[0], Look[1], Entry[2]);
		const ftcore::MassingVec From = ftcore::massingEnginePoint(Side, Entry[0], Entry[1], Entry[2]);
		EntryYaw = FMath::RadiansToDegrees(FMath::Atan2(To.y - From.y, To.x - From.x));
	}
	APlayerStart* Start = World->SpawnActor<APlayerStart>(EntryAt + FVector(0.0, 0.0, 100.0), FRotator(0.0, EntryYaw, 0.0), Spawn);
	Start->SetActorLabel(TEXT("PlayerStart"));

	// With -World, the director that plays the world's story steps and tours here, with the districts where the plan
	// puts them.
	if (!Places.IsEmpty())
	{
		AFTExploreDirector* Director = World->SpawnActor<AFTExploreDirector>(FVector(0.0, 0.0, 200.0), FRotator::ZeroRotator, Spawn);
		Director->WorldId = WorldId;
		for (const FDistrictPlace& Place : Places)
		{
			const ftcore::MassingVec Centre = ftcore::massingEnginePoint(Side, Place.CentreX, Place.CentreY, Place.Floor);
			Director->DistrictFloors.Add(static_cast<float>(Centre.z * 100.0));
			Director->DistrictCentres.Add(FVector2D(Centre.x * 100.0, Centre.y * 100.0));
			Director->DistrictSpawns.Add(Cm(ftcore::massingEnginePoint(Side, Place.SpawnX, Place.SpawnY, Place.SpawnFloor)));
		}
		// The tour's stops, in the table's order, and what the tour judges them by: the sea's level and the plan's square.
		for (const ftcore::TourStop& Stop : TourStops)
		{
			Director->TourStopIds.Add(Utf8(Stop.id));
			Director->TourStopKinds.Add(static_cast<uint8>(Stop.kind));
			Director->TourStopTargets.Add(Cm(ftcore::massingEnginePoint(Side, Stop.x, Stop.y, Stop.z)));
			Director->TourStopHasStart.Add(Stop.hasStart);
			Director->TourStopStarts.Add(
				Stop.hasStart ? Cm(ftcore::massingEnginePoint(Side, Stop.startX, Stop.startY, Stop.startZ)) : FVector::ZeroVector);
			Director->TourStopEdges.Add(static_cast<uint8>(Stop.edge));
			Director->TourStopStills.Add(Stop.still);
		}
		Director->bHasSeaLevel = bSea;
		Director->SeaLevel = 0.f; // the sea plane stands at the datum
		Director->PlanSide = static_cast<float>(Side);
		Director->SetActorLabel(TEXT("ExploreDirector"));
	}

	// Whatever is still building (the sloped meshes' collision, the ground's) finishes before the navmesh reads it.
	FAssetCompilingManager::Get().FinishAllCompilation();

	// -ReadBack: the Landscape as built, its heightmap and its collision, against the field.
	const bool bReadBackHeld = ReadBackFile.IsEmpty() || ReadBack(*Land, *LandInfo, ReadField, ReadWindow, Side);

	int32 Reached = 0;
	int32 SpawnsReached = 0;
	int32 CoverageFails = 0;
	if (bNav && (Goals.Num() > 0 || Places.Num() > 0))
	{
		const double Low = ftcore::massingHeightFromValue(Lowest) - 10.0;
		const double High = ftcore::massingHeightFromValue(Highest) + 20.0;
		const ftcore::MassingVec Centre =
			ftcore::massingEnginePoint(Side, (NavMinX + NavMaxX) * 0.5, (NavMinY + NavMaxY) * 0.5, (Low + High) * 0.5);
		ANavMeshBoundsVolume* Bounds =
			NavBounds(World, Spawn, Cm(Centre), FVector(NavMaxX - NavMinX, NavMaxY - NavMinY, High - Low) * 100.0);
		FNavigationSystem::AddNavigationSystemToWorld(*World, FNavigationSystemRunMode::EditorMode);
		UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		if (!Nav)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: no navigation system in the new world."));
			World->DestroyWorld(false);
			return 1;
		}
		// A commandlet's editor world keeps the async-load lock, and the bounds wait for a tick.
		Nav->RemoveNavigationBuildLock(ENavigationBuildLock::AsyncLoadLock, UNavigationSystemV1::ELockRemovalRebuildAction::NoRebuild);
		Nav->OnNavigationBoundsUpdated(Bounds);
		Nav->Tick(0.f);
		const double BuildStart = FPlatformTime::Seconds();
		Nav->Build();
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: navmesh built in %.0f s."), FPlatformTime::Seconds() - BuildStart);
		const ANavigationData* NavData = Nav->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
		FNavLocation From;
		if (!NavData || !Nav->ProjectPointToNavigation(EntryAt, From, kNavProjectExtent, NavData))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: the entry (%.1f, %.1f, %.1f) is not on the navmesh."), Entry[0], Entry[1],
				Entry[2]);
		}
		else
		{
			const FSharedConstNavQueryFilter Filter = ReachFilter(*NavData);
			FReachTally GoalTally, SpawnTally;
			for (const FGoal& Goal : Goals)
			{
				// A held goal is listed apart (logged with the table), never walked or counted.
				if (Goal.HeldAt == INDEX_NONE)
				{
					WalkFromEntry(*Nav, *NavData, Filter, From, Side, TEXT("goal ") + Goal.Id, Goal.X, Goal.Y, Goal.Z, GoalTally);
				}
			}
			for (int32 D = 0; D < Places.Num(); ++D)
			{
				const FDistrictPlace& Place = Places[D];
				WalkFromEntry(*Nav, *NavData, Filter, From, Side, FString::Printf(TEXT("district %d's spawn"), D), Place.SpawnX, Place.SpawnY,
					Place.SpawnFloor, SpawnTally);
			}
			Reached = GoalTally.Reached;
			SpawnsReached = SpawnTally.Reached;
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: reach (search budget %u nodes): %s; %s%s."), kReachSearchNodes,
				*ReachLine(TEXT("goals"), GoalTally, Counted), *ReachLine(TEXT("district spawns"), SpawnTally, Places.Num()), *FootingLine);
		}
		// -Coverage: the navmesh's islands against the table, from district 0's spawn.
		if (!CoverageFile.IsEmpty())
		{
			const ARecastNavMesh* Recast = Cast<ARecastNavMesh>(NavData);
			if (!Recast)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: coverage: the navigation data isn't a navmesh."));
				CoverageFails = 1;
			}
			else
			{
				CoverageFails = CheckCoverage(*Nav, *Recast, Side,
					Cm(ftcore::massingEnginePoint(Side, Places[0].SpawnX, Places[0].SpawnY, Places[0].SpawnFloor)), CoveragePoints, Solids, bSea,
					0.0, TEXT("-Sea"));
			}
		}
	}
	else if (!CoverageFile.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: coverage: no navmesh (-NoNav), so not checked."));
	}

	int32 Actors = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++Actors;
	}
	if (!KitRows.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: kit: %d meshes placed on %d boxes, %d of the meshes with no collider; %d built boxes left as boxes."),
			KitPlaced, KitOnBox.Num(), KitNoCollider, BoxesLeft);
	}
	const FString ApartLine = Apart.IsEmpty() ? FString() : FString::Printf(TEXT(" (listed apart, not counted: %s)"), *Apart);
	const FString SpawnLine =
		Places.IsEmpty() ? FString() : FString::Printf(TEXT(", district spawns reached %d of %d"), SpawnsReached, Places.Num());
	const FString PlacedLine = GoalPlacements + SpawnPlacements == 0
		? FString()
		: FString::Printf(TEXT("; temporary placements: %d goals, %d spawns"), GoalPlacements, Places.IsEmpty() ? 0 : SpawnPlacements);
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTBuildMassingWorld: %d rows: %d named and %d kit built (%d sloped meshes, %d tops nudged), %d not built; %d hidden walls; "
			 "ground %d x %d samples (%.1f to %.1f m); %d actors; goals reached %d of %d%s%s%s%s."),
		static_cast<int32>(Boxes.size()), Named, Kit, Sloped, Nudged, Skipped, static_cast<int32>(Walls.size()), Samples, Samples,
		ftcore::massingHeightFromValue(Lowest), ftcore::massingHeightFromValue(Highest), Actors, Reached, Counted, *ApartLine, *SpawnLine,
		*PlacedLine, bNav ? TEXT("") : TEXT(" (no navmesh)"));

	FAssetCompilingManager::Get().FinishAllCompilation();
	FAssetRegistryModule::AssetCreated(World);
	Package->MarkPackageDirty();
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, World, *File, Save);
	// Read after the save, from the world that was saved, so asking for the dump can't change the map.
	int32 DumpRows = 0;
	const FString DumpText = bSaved && !DumpFile.IsEmpty() ? ActorDump(World, DumpRows) : FString();
	World->DestroyWorld(false);
	if (!bSaved)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: saved %s."), *File);
	if (!DumpFile.IsEmpty())
	{
		if (!FFileHelper::SaveStringToFile(DumpText, *DumpFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: couldn't write the dump to %s."), *DumpFile);
			return 1;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: dumped %d actors to %s."), DumpRows, *DumpFile);
	}
	const bool bShort = bNav && (Reached < Counted || SpawnsReached < Places.Num() || FootingHeld < Places.Num() || CoverageFails > 0);
	return bStrict && (bShort || !bReadBackHeld) ? 1 : 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld needs an editor build."));
	return 1;
#endif
}
