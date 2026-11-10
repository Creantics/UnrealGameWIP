#include "FTBuildRoads.h"

#include "ForgottenThird.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"

#include <string>
#include <string_view>

namespace
{
constexpr const TCHAR* kRoadLampMesh = TEXT("/Game/Props/TQ-04/sodium_lamp_TQ-04_import/StaticMeshes/sodium_lamp_TQ-04_import.sodium_lamp_TQ-04_import");
constexpr const TCHAR* kRoadScooterMesh = TEXT("/Game/Props/TQ-16/scooter_downpipe_TQ-16_e4_import/StaticMeshes/SM_TQ16_scooter.SM_TQ16_scooter");
constexpr const TCHAR* kRoadLampTag = TEXT("TQ-04");
constexpr double kRoadDoorClear = 1.0; // m: vehicles keep this far off a door's foot
constexpr double kRoadTwoWheelReach = 40.0; // m: with no -LaneLamps box, two-wheelers go this far round each road

FString RoadsUtf8(const std::string& Text)
{
	return FString(UTF8_TO_TCHAR(Text.c_str()));
}

bool RoadsReadText(const FString& File, std::string& Out)
{
	TArray<uint8> Bytes;
	if (File.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: roads: couldn't read '%s'."), *File);
		return false;
	}
	Out.assign(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
	return true;
}

bool RoadsLoadDoors(const FTBuildRoads::FOptions& Options, std::vector<ftcore::MassingFoot>& Doors)
{
	if (Options.DoorsFile.IsEmpty())
	{
		return true;
	}
	std::string Text, Error;
	if (!RoadsReadText(Options.DoorsFile, Text))
	{
		return false;
	}
	if (!ftcore::importRoadDoors(Text, kRoadDoorClear, Doors, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s"), *Options.DoorsFile, *RoadsUtf8(Error));
		return false;
	}
	return true;
}

// The roads' own copies of the builder's helpers (they live in its file's anonymous namespace).
struct FRoadPainter
{
	const FFTRoadsHost& Host;
	UStaticMesh* Cylinder = nullptr;
	UMaterialInterface* Tints[static_cast<int32>(ftcore::RoadTint::Count)] = {};

	UMaterialInterface* Tint(ftcore::RoadTint Which)
	{
		UMaterialInterface*& Slot = Tints[static_cast<int32>(Which)];
		if (!Slot && Host.Base)
		{
			const ftcore::RoadColor C = ftcore::roadTint(Which);
			UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Host.Package,
				*FString::Printf(TEXT("MI_Road_%s"), UTF8_TO_TCHAR(ftcore::roadTintName(Which))), RF_Public);
			Mic->SetParentEditorOnly(Host.Base);
			Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), FLinearColor(C.r, C.g, C.b));
			if (C.roughness >= 0.f)
			{
				Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), C.roughness);
			}
			Mic->PostEditChange();
			Slot = Mic;
		}
		return Slot;
	}

	AStaticMeshActor* Spawn(UStaticMesh* Mesh, UMaterialInterface* Material, const FString& Label, const FVector& Location, const FVector& Scale,
		const FRotator& Rotation) const
	{
		AStaticMeshActor* Actor = Host.World->SpawnActor<AStaticMeshActor>(Location, Rotation, *Host.Spawn);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		if (Material)
		{
			Actor->GetStaticMeshComponent()->SetMaterial(0, Material);
		}
		Actor->SetActorScale3D(Scale);
		Actor->SetActorLabel(Label);
		return Actor;
	}

	// A solid as the engine's centred cube (posed as the builder poses its dressing) or 1 m cylinder (its axis up,
	// along the box's u or along its w).
	bool Lay(const ftcore::RoadSolid& Solid)
	{
		const ftcore::MassingBox& Box = Solid.box;
		const double Height = FMath::Max(Box.z1 - Box.z0, ftcore::kMassingMinHeight);
		AStaticMeshActor* Actor = nullptr;
		if (Solid.shape == ftcore::RoadShape::Cylinder && Cylinder)
		{
			const ftcore::MassingVec At = ftcore::massingEnginePoint(Host.Side, Box.cx, Box.cy, Box.z0 + Height * 0.5);
			const double Yaw = ftcore::massingEngineYaw(Box.rot);
			FVector Scale(Box.sx, Box.sy, Height);
			FRotator Rotation(0.0, Yaw, 0.0);
			if (Solid.axis == ftcore::RoadAxis::AlongU)
			{
				Scale = FVector(Height, Box.sy, Box.sx); // pitched over: its axis along u, its local x up
				Rotation.Pitch = 90.0;
			}
			else if (Solid.axis == ftcore::RoadAxis::AlongW)
			{
				Scale = FVector(Box.sx, Height, Box.sy); // rolled over: its axis along w, its local y up
				Rotation.Roll = 90.0;
			}
			Actor = Spawn(Cylinder, Tint(Solid.tint), RoadsUtf8(Box.id), FVector(At.x, At.y, At.z) * 100.0, Scale, Rotation);
		}
		else
		{
			const ftcore::MassingCubePose Pose = ftcore::massingCubePose(Box, Height);
			const ftcore::MassingVec At = ftcore::massingEnginePoint(Host.Side, Box.cx, Box.cy, Box.z0 + Height * 0.5 + Pose.lift);
			Actor = Spawn(Host.Cube, Tint(Solid.tint), RoadsUtf8(Box.id), FVector(At.x, At.y, At.z) * 100.0,
				FVector(Pose.scale.x, Pose.scale.y, Pose.scale.z), FRotator(Pose.pitch, Pose.yaw, Pose.roll));
		}
		if (!Actor)
		{
			return false;
		}
		UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
		if (!Solid.collide)
		{
			Mesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
			Mesh->SetCanEverAffectNavigation(false);
		}
		const bool bFlat = Solid.tint == ftcore::RoadTint::LineWhite || Solid.tint == ftcore::RoadTint::LineYellow ||
			Solid.tint == ftcore::RoadTint::LensRed || Solid.tint == ftcore::RoadTint::LensAmber || Solid.tint == ftcore::RoadTint::LensGreen ||
			Solid.tint == ftcore::RoadTint::Wire;
		if (bFlat)
		{
			Mesh->SetCastShadow(false);
		}
		Actor->Tags.Add(TEXT("FTRoad"));
		if (!Solid.tag.empty())
		{
			Actor->Tags.Add(FName(RoadsUtf8(Solid.tag)));
		}
		return true;
	}

	// A vehicle: the TQ-16 scooter mesh where one is (its longest side scaled to the stand-in's length), else its
	// stand-in solids.
	bool Vehicle(const ftcore::RoadVehicle& V, UStaticMesh* Scooter, std::vector<ftcore::RoadSolid>& Scratch)
	{
		const FName Tag(*FString::Printf(TEXT("FTVehicle_%s"), UTF8_TO_TCHAR(ftcore::roadVehicleName(V.kind))));
		if (V.kind == ftcore::RoadVehicleKind::Scooter && Scooter)
		{
			const FBox Bounds = Scooter->GetBoundingBox();
			const FVector Size = Bounds.GetSize();
			const double Long = FMath::Max(Size.X, Size.Y);
			if (Long > 1.0)
			{
				const double Scale = ftcore::roadVehicleSize(V.kind).x * 100.0 / Long;
				const double Yaw = ftcore::massingEngineYaw(V.rot) + (Size.Y > Size.X ? 90.0 : 0.0);
				const FVector Centre = FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, 0.0) * Scale;
				const ftcore::MassingVec At = ftcore::massingEnginePoint(Host.Side, V.x, V.y, V.z);
				const FVector Location = FVector(At.x * 100.0, At.y * 100.0, At.z * 100.0 - Bounds.Min.Z * Scale) -
					FRotator(0.0, Yaw, 0.0).RotateVector(Centre);
				AStaticMeshActor* Actor = Spawn(Scooter, nullptr, RoadsUtf8(V.id), Location, FVector(Scale), FRotator(0.0, Yaw, 0.0));
				Actor->GetStaticMeshComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
				Actor->GetStaticMeshComponent()->SetCanEverAffectNavigation(false);
				Actor->Tags.Add(TEXT("FTRoad"));
				Actor->Tags.Add(Tag);
				return true;
			}
		}
		Scratch.clear();
		ftcore::roadVehicleSolids(V, Scratch);
		for (const ftcore::RoadSolid& Part : Scratch)
		{
			if (!Lay(Part))
			{
				return false;
			}
		}
		return true;
	}
};

bool RoadsParseBox(const FString& Text, double Out[4])
{
	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT(","));
	if (Parts.Num() != 4)
	{
		return false;
	}
	for (int32 I = 0; I < 4; ++I)
	{
		if (!LexTryParseString(Out[I], *Parts[I].TrimStartAndEnd()))
		{
			return false;
		}
	}
	return true;
}
} // namespace

namespace FTBuildRoads
{
FOptions ParseOptions(const TCHAR* Params)
{
	FOptions Options;
	FParse::Value(Params, TEXT("Roads="), Options.RoadsFile);
	FParse::Value(Params, TEXT("RoadDoors="), Options.DoorsFile);
	Options.bVehicles = FParse::Param(Params, TEXT("Vehicles"));
	FString Box;
	if (FParse::Value(Params, TEXT("LaneLamps="), Box, false)) // false: the commas are part of the value
	{
		Options.bLaneLamps = RoadsParseBox(Box, Options.LaneBox);
		if (!Options.bLaneLamps)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -LaneLamps=%s isn't x0,y0,x1,y1; no lane lamps."), *Box);
		}
	}
	uint32 Seed = Options.Seed;
	if (FParse::Value(Params, TEXT("RoadSeed="), Seed))
	{
		Options.Seed = Seed;
	}
	return Options;
}

// Box rows (pads, slabs, plinths, footbridges under 4.8 m) standing in a road's carriageway: hidden like the rows the
// road clears, logged by id; an Error (and false) when any still clash after that.
static bool HideRoadClashes(const ftcore::RoadBuild& Build, std::vector<ftcore::MassingBox>& Boxes, const FString& What)
{
	const std::vector<size_t> Clash = ftcore::roadBoxClashes(Build, Boxes);
	if (Clash.empty())
	{
		return true;
	}
	FString Ids;
	for (const size_t I : Clash)
	{
		Boxes[I].kind = "underpass";
		Ids += TEXT(" ") + RoadsUtf8(Boxes[I].id);
	}
	UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: roads %s: hides %d box rows in the carriageway:%s."), *What,
		static_cast<int32>(Clash.size()), *Ids);
	const size_t Left = ftcore::roadBoxClashes(Build, Boxes).size();
	if (Left > 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: roads %s: %d box rows still in the carriageway."), *What,
			static_cast<int32>(Left));
		return false;
	}
	return true;
}

bool Prepare(const FOptions& Options, const ftcore::MassingGround& Ground, std::vector<ftcore::MassingBox>& Boxes,
	std::vector<ftcore::RoadSpec>& Roads, std::vector<ftcore::RoadBuild>& Builds)
{
	Roads.clear();
	Builds.clear();
	if (Options.RoadsFile.IsEmpty())
	{
		return true;
	}
	std::string Text, Error;
	if (!RoadsReadText(Options.RoadsFile, Text))
	{
		return false;
	}
	if (ftcore::roadNetTable(Text))
	{
		std::vector<ftcore::RoadNetStreet> Net;
		if (!ftcore::importRoadNet(Text, Net, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s"), *Options.RoadsFile, *RoadsUtf8(Error));
			return false;
		}
		if (Net.empty())
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: %s: a road network table with no rows; no roads."), *Options.RoadsFile);
			return true;
		}
		Builds.resize(Net.size());
		ftcore::RoadNetReport Report;
		ftcore::roadNetBuild(Net, Ground, Builds[0], &Report);
		for (const ftcore::RoadNetStreet& Street : Net)
		{
			ftcore::RoadSpec Stub; // DropRoadStreets' id, nothing else; its build stays empty
			Stub.id = Street.id;
			Stub.kind = "net";
			Roads.push_back(std::move(Stub));
		}
		const std::vector<size_t> Gone = ftcore::roadClearedBoxes(Builds[0], Boxes, 0.0);
		for (const size_t I : Gone)
		{
			Boxes[I].kind = "underpass";
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: road network %s: %d streets, %d junctions, %d bends, %d ends, %d pieces, "
			"%d piers; %d steep (max %.1f%%), %d pokes (%d over 0.3 m, max %.2f m), %d floats, %d tight; hides %d rows."),
			*Options.RoadsFile, static_cast<int32>(Net.size()), static_cast<int32>(Report.junctions), static_cast<int32>(Report.bends),
			static_cast<int32>(Report.ends), static_cast<int32>(Report.pieces), static_cast<int32>(Report.piers), static_cast<int32>(Report.steep),
			Report.maxGrade * 100.0, static_cast<int32>(Report.pokes), static_cast<int32>(Report.pokesOver), Report.maxPoke,
			static_cast<int32>(Report.floats), static_cast<int32>(Report.tight), static_cast<int32>(Gone.size()));
		for (const std::string& Note : Report.notes)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: road network: %s"), *RoadsUtf8(Note));
		}
		if (Report.pruned > 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: road network: %d kerb and pavement pieces pruned at mouths."),
				static_cast<int32>(Report.pruned));
		}
		return HideRoadClashes(Builds[0], Boxes, TEXT("network"));
	}
	if (!ftcore::importRoads(Text, Roads, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s"), *Options.RoadsFile, *RoadsUtf8(Error));
		return false;
	}
	Builds.resize(Roads.size());
	for (size_t R = 0; R < Roads.size(); ++R)
	{
		ftcore::roadBuild(Roads[R], Builds[R]);
		const std::vector<size_t> Gone = ftcore::roadClearedBoxes(Builds[R], Boxes, 0.0);
		FString Ids;
		for (const size_t I : Gone)
		{
			Boxes[I].kind = "underpass"; // hidden (ftcore::massingRole); the table stays positional
			Ids += TEXT(" ") + RoadsUtf8(Boxes[I].id);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: roads %s: hides %d deck and pier rows under it:%s."), *RoadsUtf8(Roads[R].id),
			static_cast<int32>(Gone.size()), *Ids);
		if (!HideRoadClashes(Builds[R], Boxes, RoadsUtf8(Roads[R].id)))
		{
			return false;
		}
	}
	return true;
}

size_t DropRoadStreets(const std::vector<ftcore::RoadSpec>& Roads, std::vector<ftcore::MassingStreet>& Streets)
{
	const size_t Before = Streets.size();
	size_t Keep = 0;
	for (size_t I = 0; I < Streets.size(); ++I)
	{
		bool bRoad = false;
		for (const ftcore::RoadSpec& Road : Roads)
		{
			bRoad = bRoad || Road.id == Streets[I].id;
		}
		if (!bRoad)
		{
			if (Keep != I)
			{
				Streets[Keep] = std::move(Streets[I]);
			}
			++Keep;
		}
	}
	Streets.resize(Keep);
	if (Before != Keep)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: roads: %d streets rows left to -Roads (no -Kerbs kerb on them)."),
			static_cast<int32>(Before - Keep));
	}
	return Before - Keep;
}

bool Lay(FFTRoadsHost& Host, const FOptions& Options, const std::vector<ftcore::RoadSpec>& Roads, const std::vector<ftcore::RoadBuild>& Builds)
{
	Host.RoadsFrom = Host.Taken ? Host.Taken->size() : 0;
	Host.RoadsTo = Host.RoadsFrom;
	if (Roads.empty())
	{
		return true;
	}
	FRoadPainter Paint{Host};
	Paint.Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (!Paint.Cylinder)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: roads: the engine's cylinder didn't load; cylinders are built as cubes."));
	}
	UStaticMesh* Lamp = LoadObject<UStaticMesh>(nullptr, kRoadLampMesh);
	UStaticMesh* Scooter = Options.bVehicles ? LoadObject<UStaticMesh>(nullptr, kRoadScooterMesh) : nullptr;
	std::vector<ftcore::MassingFoot> Doors;
	if (!RoadsLoadDoors(Options, Doors))
	{
		return false;
	}
	// The cars park against the room taken before the roads (their own bands would block every bay).
	const std::vector<ftcore::MassingFoot> Before = Host.Taken ? *Host.Taken : std::vector<ftcore::MassingFoot>();
	std::vector<ftcore::RoadSolid> Scratch;
	for (size_t R = 0; R < Roads.size(); ++R)
	{
		const ftcore::RoadSpec& Road = Roads[R];
		const ftcore::RoadBuild& Build = Builds[R];
		if (Road.kind == "net" && R > 0)
		{
			continue; // a network street's stand-in: the network is all in Builds[0]
		}
		for (const ftcore::RoadSolid& Solid : Build.solids)
		{
			if (!Paint.Lay(Solid))
			{
				return false;
			}
		}
		int32 Lamps = 0;
		for (size_t L = 0; L < Build.lamps.size(); ++L)
		{
			if (Lamp && Host.PlaceLamp && Host.PlaceLamp(Lamp, Build.lamps[L], FString::Printf(TEXT("%s_lamp_%d"), *RoadsUtf8(Road.id), static_cast<int32>(L) + 1)))
			{
				++Lamps;
			}
		}
		ftcore::RoadParkCounts Park;
		int32 Parked = 0;
		if (Options.bVehicles)
		{
			std::vector<ftcore::RoadVehicle> Cars;
			Park = ftcore::roadParkVehicles(Build, Doors, &Before, Options.Seed + static_cast<uint32>(R), Cars);
			for (const ftcore::RoadVehicle& Car : Cars)
			{
				if (!Paint.Vehicle(Car, Scooter, Scratch))
				{
					return false;
				}
				++Parked;
			}
		}
		if (Host.Taken)
		{
			Host.Taken->insert(Host.Taken->end(), Build.band.begin(), Build.band.end());
			Host.Taken->insert(Host.Taken->end(), Build.furniture.begin(), Build.furniture.end());
		}
		const ftcore::RoadCounts& C = Build.counts;
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: roads %s: %d solids (%d collide), %d markings, %d zebras, %d bays, %d of %d lamps "
			"(TQ-04), %d signals, %d signs, %d poles, %d lane mouths, %d junctions; %s."),
			Road.kind == "net" ? TEXT("network") : *RoadsUtf8(Road.id), static_cast<int32>(C.solids), static_cast<int32>(C.collide), static_cast<int32>(C.markings), static_cast<int32>(C.zebras),
			static_cast<int32>(C.bays), Lamps, static_cast<int32>(Build.lamps.size()), static_cast<int32>(C.signals), static_cast<int32>(C.signs),
			static_cast<int32>(C.poles), static_cast<int32>(C.mouths), static_cast<int32>(C.junctions),
			Options.bVehicles ? *FString::Printf(TEXT("%d cars parked in %d bays (%d left empty, %d blocked)"), Parked, static_cast<int32>(Park.bays),
				static_cast<int32>(Park.empty), static_cast<int32>(Park.blocked))
							  : TEXT("no vehicles (no -Vehicles)"));
	}
	Host.RoadsTo = Host.Taken ? Host.Taken->size() : 0;
	return true;
}

bool Finish(const FFTRoadsHost& Host, const FOptions& Options, const std::vector<ftcore::RoadSpec>& Roads,
	const std::vector<ftcore::RoadBuild>& Builds, const FString& StreetsFile, const ftcore::MassingGround& Ground)
{
	if (!Options.bLaneLamps && !(Options.bVehicles && !Roads.empty()))
	{
		return true;
	}
	std::vector<ftcore::MassingStreet> Lanes;
	{
		std::string Text, Error;
		if (!RoadsReadText(StreetsFile, Text))
		{
			return false;
		}
		if (!ftcore::importMassingStreets(Text, Lanes, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: %s: %s"), *StreetsFile, *RoadsUtf8(Error));
			return false;
		}
	}
	std::vector<ftcore::MassingFoot> Doors;
	if (!RoadsLoadDoors(Options, Doors))
	{
		return false;
	}
	FRoadPainter Paint{Host};
	Paint.Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Options.bLaneLamps)
	{
		UStaticMesh* Lamp = LoadObject<UStaticMesh>(nullptr, kRoadLampMesh);
		std::vector<ftcore::RoadPoint> Existing;
		for (TActorIterator<AStaticMeshActor> It(Host.World); It; ++It)
		{
			const UStaticMesh* Mesh = It->GetStaticMeshComponent()->GetStaticMesh();
			if (Mesh && Mesh->GetPathName().Contains(kRoadLampTag))
			{
				const FVector At = It->GetActorLocation() / 100.0;
				Existing.push_back(ftcore::RoadPoint{At.X + Host.Side * 0.5, Host.Side * 0.5 - At.Y});
			}
		}
		ftcore::RoadLaneLampOptions Opts;
		Opts.x0 = Options.LaneBox[0];
		Opts.y0 = Options.LaneBox[1];
		Opts.x1 = Options.LaneBox[2];
		Opts.y1 = Options.LaneBox[3];
		std::vector<ftcore::MassingProp> Lamps;
		ftcore::roadLaneLamps(Lanes, Opts, Ground, Existing, Doors, Host.Taken, Lamps);
		int32 Placed = 0;
		for (size_t L = 0; L < Lamps.size(); ++L)
		{
			if (Lamp && Host.PlaceLamp && Host.PlaceLamp(Lamp, Lamps[L], FString::Printf(TEXT("LaneLamp_%d"), static_cast<int32>(L) + 1)))
			{
				++Placed;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: lane lamps: %d TQ-04 lamps every %.0f m along the lanes up to %.1f m wide in "
			"%.0f,%.0f..%.0f,%.0f (%d lamps there or near before)."), Placed, Opts.spacing, Opts.laneUpTo, Opts.x0, Opts.y0, Opts.x1, Opts.y1,
			static_cast<int32>(Existing.size()));
	}
	if (Options.bVehicles && !Roads.empty())
	{
		UStaticMesh* Scooter = LoadObject<UStaticMesh>(nullptr, kRoadScooterMesh);
		// Everything taken but the roads' own bands and furniture, which the road groups stand inside.
		std::vector<ftcore::MassingFoot> Room;
		if (Host.Taken)
		{
			for (size_t I = 0; I < Host.Taken->size(); ++I)
			{
				if (I < Host.RoadsFrom || I >= Host.RoadsTo)
				{
					Room.push_back((*Host.Taken)[I]);
				}
			}
		}
		std::vector<ftcore::RoadVehicle> Bikes;
		const std::vector<ftcore::MassingStreet> None;
		for (size_t R = 0; R < Roads.size(); ++R)
		{
			const ftcore::RoadSpec& Road = Roads[R];
			const bool bNet = Road.kind == "net";
			if (bNet && R > 0)
			{
				break; // a network: the lanes and its carriageways once, with Builds[0]
			}
			ftcore::RoadTwoWheelOptions Opts;
			if (Options.bLaneLamps)
			{
				Opts.x0 = Options.LaneBox[0];
				Opts.y0 = Options.LaneBox[1];
				Opts.x1 = Options.LaneBox[2];
				Opts.y1 = Options.LaneBox[3];
			}
			else if (bNet)
			{
				continue; // no box to work in: a network needs -LaneLamps' box for its lanes' two-wheelers
			}
			else
			{
				Opts.x0 = FMath::Min(Road.x0, Road.x1) - kRoadTwoWheelReach;
				Opts.y0 = FMath::Min(Road.y0, Road.y1) - kRoadTwoWheelReach;
				Opts.x1 = FMath::Max(Road.x0, Road.x1) + kRoadTwoWheelReach;
				Opts.y1 = FMath::Max(Road.y0, Road.y1) + kRoadTwoWheelReach;
			}
			Opts.seed = Options.Seed + 101u * static_cast<uint32>(R + 1);
			// The lanes once (with the first road), then each further road's back of pavement alone.
			ftcore::roadTwoWheelers(R == 0 ? Lanes : None, Opts, &Builds[R], bNet ? nullptr : &Road, Ground, Doors, &Room, Bikes);
		}
		std::vector<ftcore::RoadSolid> Scratch;
		int32 Scooters = 0, Bicycles = 0;
		for (const ftcore::RoadVehicle& Bike : Bikes)
		{
			if (!Paint.Vehicle(Bike, Scooter, Scratch))
			{
				return false;
			}
			(Bike.kind == ftcore::RoadVehicleKind::Scooter ? Scooters : Bicycles) += 1;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: two-wheelers: %d scooters (%s) and %d bicycles along the lanes and the roads."),
			Scooters, Scooter ? TEXT("the TQ-16 mesh") : TEXT("stand-ins: TQ-16 didn't load"), Bicycles);
	}
	return true;
}
} // namespace FTBuildRoads
