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

#include "FTExploreGameMode.h"
#include "FTSceneLook.h"

#include "ftcore/Json.h"
#include "ftcore/MassingPlan.h"

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
};

// A CSV with the columns id, x, y, z (in any order, others skipped); '#' lines are comments.
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
		Goals.Add(Goal);
	}
	return true;
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
	FString HeightFile, BoxesFile, WallsFile, GoalsFile, EntryText, LookText;
	FString MapName = TEXT("World_Massing");
	FParse::Value(*Params, TEXT("Height="), HeightFile);
	FParse::Value(*Params, TEXT("Boxes="), BoxesFile);
	FParse::Value(*Params, TEXT("Walls="), WallsFile);
	FParse::Value(*Params, TEXT("Goals="), GoalsFile);
	// The separator flag is off so "x,y,z" keeps its commas.
	FParse::Value(*Params, TEXT("Entry="), EntryText, false);
	FParse::Value(*Params, TEXT("Look="), LookText, false);
	FParse::Value(*Params, TEXT("Map="), MapName);
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

	// Whatever is still building (the sloped meshes' collision, the ground's) finishes before the navmesh reads it.
	FAssetCompilingManager::Get().FinishAllCompilation();

	int32 Reached = 0;
	if (bNav && Goals.Num() > 0)
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
			for (const FGoal& Goal : Goals)
			{
				FNavLocation To;
				const UNavigationPath* Route = nullptr;
				const bool bOnMesh =
					Nav->ProjectPointToNavigation(Cm(ftcore::massingEnginePoint(Side, Goal.X, Goal.Y, Goal.Z)), To, kNavProjectExtent);
				if (bOnMesh)
				{
					Route = Nav->FindPathToLocationSynchronously(World, From.Location, To.Location);
				}
				const bool bReached = Route && Route->IsValid() && !Route->IsPartial();
				Reached += bReached ? 1 : 0;
				if (bReached)
				{
					UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: goal %s (%.0f, %.0f, %+.0f): reached, %.0f m of path."), *Goal.Id,
						Goal.X, Goal.Y, Goal.Z, Route->GetPathLength() / 100.0);
				}
				else
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: goal %s (%.0f, %.0f, %+.0f): %s."), *Goal.Id, Goal.X, Goal.Y, Goal.Z,
						bOnMesh ? TEXT("no full path from the entry") : TEXT("not on the navmesh"));
				}
			}
		}
	}

	int32 Actors = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++Actors;
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTBuildMassingWorld: %d rows: %d named and %d kit built (%d sloped meshes, %d tops nudged), %d not built; %d hidden walls; "
			 "ground %d x %d samples (%.1f to %.1f m); %d actors; goals reached %d of %d%s."),
		static_cast<int32>(Boxes.size()), Named, Kit, Sloped, Nudged, Skipped, static_cast<int32>(Walls.size()), Samples, Samples,
		ftcore::massingHeightFromValue(Lowest), ftcore::massingHeightFromValue(Highest), Actors, Reached, Goals.Num(),
		bNav ? TEXT("") : TEXT(" (no navmesh)"));

	FAssetCompilingManager::Get().FinishAllCompilation();
	FAssetRegistryModule::AssetCreated(World);
	Package->MarkPackageDirty();
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, World, *File, Save);
	World->DestroyWorld(false);
	if (!bSaved)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: saved %s."), *File);
	return bStrict && bNav && Reached < Goals.Num() ? 1 : 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld needs an editor build."));
	return 1;
#endif
}
