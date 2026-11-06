#include "FTBuildMassingWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Components/StaticMeshComponent.h"
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
#include "ftcore/WorldLayout.h"

#include <array>
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
	FString HeightFile, BoxesFile, WallsFile, GoalsFile, DumpFile, EntryText, LookText, WorldText;
	FString MapName = TEXT("World_Massing");
	FParse::Value(*Params, TEXT("Height="), HeightFile);
	FParse::Value(*Params, TEXT("Boxes="), BoxesFile);
	FParse::Value(*Params, TEXT("Walls="), WallsFile);
	FParse::Value(*Params, TEXT("Goals="), GoalsFile);
	FParse::Value(*Params, TEXT("Dump="), DumpFile);
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
	if (!LoadHeightField(HeightFile, Field) || !LoadBoxes(BoxesFile, Boxes) || (!WallsFile.IsEmpty() && !LoadBoxes(WallsFile, Walls)) ||
		(!GoalsFile.IsEmpty() && !LoadGoals(GoalsFile, Goals)))
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
		double RuleX = 0.0, RuleY = 0.0; // the generator's spawn (spawnFor), plan frame
		int32 SpawnBox = INDEX_NONE;     // the row's spawn_box: the spawn is a temporary placement outside it
	};
	TArray<FDistrictPlace> Places;
	FString WorldId;
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
		std::vector<bool> Visible(Boxes.size(), false);
		for (size_t I = 0; I < Boxes.size(); ++I)
		{
			Visible[I] = ftcore::massingVisible(ftcore::massingRole(Boxes[I]));
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
			Place.SpawnFloor = ftcore::massingFloorAt(Boxes, Visible, Place.SpawnX, Place.SpawnY, Ground,
				FMath::Max(Place.Floor, Ground) + kSpawnStepUp, &OnBox);
			Place.OnBox = OnBox;
			Place.Inside = ftcore::massingBlockingAt(Boxes, Visible, Place.SpawnX, Place.SpawnY, Place.SpawnFloor, kLeadHeight);
			if (Place.Inside < 0)
			{
				Place.Inside = ftcore::massingBlockingAt(Walls, EveryWall, Place.SpawnX, Place.SpawnY, Place.SpawnFloor, kLeadHeight);
				Place.bInsideWall = Place.Inside >= 0;
			}
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
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: district %d: plaza (%.1f, %.1f) at %.1f m; spawn (%.1f, %.1f) on %s at %.1f m."),
				D, Place.CentreX, Place.CentreY, Place.Floor, Place.SpawnX, Place.SpawnY, *On, Place.SpawnFloor);
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
	const FString MapPath = FString::Printf(TEXT("/Game/Maps/%s"), *MapName);
	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
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

	// The boxes: each built one as an actor labelled with its id.
	std::vector<bool> Flat(Boxes.size(), false);
	for (size_t I = 0; I < Boxes.size(); ++I)
	{
		Flat[I] = ftcore::massingVisible(ftcore::massingRole(Boxes[I])) && !Boxes[I].sloped;
	}
	std::vector<double> Nudges;
	ftcore::massingTopNudges(Boxes, Flat, Nudges);
	int32 Named = 0, Kit = 0, Sloped = 0, Nudged = 0, Skipped = 0;
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
		UMaterialInterface* Material = Finish[static_cast<int32>(ftcore::massingFinish(Box))];
		const FString Id = Utf8(Box.id);
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
			Block(World, Spawn, Mesh, nullptr, Id, Pivot, FVector::OneVector, 0.0);
			++Sloped;
			continue;
		}
		const double Height = FMath::Max(Box.z1 + Nudges[I] - Box.z0, ftcore::kMassingMinHeight);
		Nudged += Nudges[I] < 0.0 ? 1 : 0;
		Block(World, Spawn, Cube, Material, Id, Cm(ftcore::massingEnginePoint(Side, Box.cx, Box.cy, Box.z0 + Height * 0.5)),
			FVector(Box.sx, Box.sy, Height), ftcore::massingEngineYaw(Box.rot));
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

	const FVector EntryAt = Cm(ftcore::massingEnginePoint(Side, Entry[0], Entry[1], Entry[2]));
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
		Director->SetActorLabel(TEXT("ExploreDirector"));
	}

	// Whatever is still building (the sloped meshes' collision, the ground's) finishes before the navmesh reads it.
	FAssetCompilingManager::Get().FinishAllCompilation();

	int32 Reached = 0;
	int32 SpawnsReached = 0;
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
		FNavLocation From;
		if (!Nav->ProjectPointToNavigation(EntryAt, From, kNavProjectExtent))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: the entry (%.1f, %.1f, %.1f) is not on the navmesh."), Entry[0], Entry[1],
				Entry[2]);
		}
		else
		{
			// Whether a full path runs from the entry to a plan point, logged either way.
			const auto Walk = [&](const FString& What, double X, double Y, double Z) {
				FNavLocation To;
				const UNavigationPath* Route = nullptr;
				const bool bOnMesh = Nav->ProjectPointToNavigation(Cm(ftcore::massingEnginePoint(Side, X, Y, Z)), To, kNavProjectExtent);
				// A point on the entry's own spot: the engine answers with a one-point path, which is not a valid one.
				const bool bAtEntry = bOnMesh && FVector::Dist(From.Location, To.Location) < 100.0;
				if (bOnMesh && !bAtEntry)
				{
					Route = Nav->FindPathToLocationSynchronously(World, From.Location, To.Location);
				}
				const bool bReached = bAtEntry || (Route && Route->IsValid() && !Route->IsPartial());
				// Engine cm back to the plan's metres, for the log.
				const auto PlanOf = [Side](const FVector& At) {
					return FVector(At.X / 100.0 + Side * 0.5, Side * 0.5 - At.Y / 100.0, At.Z / 100.0);
				};
				if (bReached)
				{
					UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: %s (%.0f, %.0f, %+.0f): reached, %.0f m of path."), *What, X, Y, Z,
						bAtEntry ? 0.0 : Route->GetPathLength() / 100.0);
				}
				else if (!bOnMesh)
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: %s (%.0f, %.0f, %+.0f): not on the navmesh."), *What, X, Y, Z);
				}
				else
				{
					// Where the point met the navmesh, and where the nearest path from the entry stops short of it.
					const FVector Met = PlanOf(To.Location);
					FString Stop = TEXT("no path at all");
					if (Route && Route->PathPoints.Num() > 0)
					{
						const FVector End = PlanOf(Route->PathPoints.Last());
						Stop = FString::Printf(TEXT("the path stops at (%.1f, %.1f, %+.1f), %.1f m short"), End.X, End.Y, End.Z,
							FVector::Dist(Route->PathPoints.Last(), To.Location) / 100.0);
					}
					UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: %s (%.0f, %.0f, %+.0f): no full path from the entry; on the navmesh at "
						"(%.1f, %.1f, %+.1f); %s."), *What, X, Y, Z, Met.X, Met.Y, Met.Z, *Stop);
				}
				return bReached;
			};
			for (const FGoal& Goal : Goals)
			{
				// A held goal is listed apart (logged with the table), never walked or counted.
				Reached += Goal.HeldAt == INDEX_NONE && Walk(TEXT("goal ") + Goal.Id, Goal.X, Goal.Y, Goal.Z) ? 1 : 0;
			}
			for (int32 D = 0; D < Places.Num(); ++D)
			{
				const FDistrictPlace& Place = Places[D];
				SpawnsReached += Walk(FString::Printf(TEXT("district %d's spawn"), D), Place.SpawnX, Place.SpawnY, Place.SpawnFloor) ? 1 : 0;
			}
		}
	}

	int32 Actors = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++Actors;
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
	return bStrict && bNav && (Reached < Counted || SpawnsReached < Places.Num()) ? 1 : 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld needs an editor build."));
	return 1;
#endif
}
