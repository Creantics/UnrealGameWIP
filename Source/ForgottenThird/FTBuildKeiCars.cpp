#include "FTBuildKeiCars.h"
#include "FTCellInstancer.h"

#include "ForgottenThird.h"

#include "Engine/StaticMesh.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"

#include "ftcore/MassingPlan.h"
#include "ftcore/ParkedCarLook.h"

namespace FTBuildKeiCars
{
FString FindMeshPath()
{
	const FString Name(MeshName);
	const FString Base(Folder);
	const FString Dirs[3] = {Base / Name / TEXT("StaticMeshes"), Base / TEXT("StaticMeshes"), Base};
	for (const FString& Dir : Dirs)
	{
		const FString Package = Dir / Name;
		if (FPackageName::DoesPackageExist(Package))
		{
			return Package + TEXT(".") + Name;
		}
	}
	return FString();
}

FSet Load(const TCHAR* Params, uint32 Seed)
{
	FSet Set;
	Set.Seed = Seed;
	FString Mode;
	if (FParse::Value(Params, TEXT("KeiCars="), Mode))
	{
		Set.Mode = Mode.Equals(TEXT("All"), ESearchCase::IgnoreCase) ? EMode::All : EMode::Kei;
	}
	else if (FParse::Param(Params, TEXT("KeiCars")))
	{
		Set.Mode = EMode::Kei;
	}
	if (Set.Mode == EMode::Off)
	{
		return Set;
	}
	float Share = 0.f;
	if (FParse::Value(Params, TEXT("KeiCarLamps="), Share))
	{
		Set.LampShare = FMath::Clamp(Share, 0.f, 1.f);
	}
	const FString Path = FindMeshPath();
	Set.Mesh = Path.IsEmpty() ? nullptr : LoadObject<UStaticMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Set.Mesh)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: -KeiCars: no %s under %s (run FTImportVehicle); the box stand-ins stay."),
			MeshName, Folder);
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -KeiCars%s: parked cars as %s (lamps on %.0f%%)."),
			Set.Mode == EMode::All ? TEXT("=All") : TEXT(""), *Path, Set.LampShare * 100.f);
	}
	return Set;
}

namespace
{
bool Takes(const FSet& Set, ftcore::RoadVehicleKind Kind)
{
	switch (Set.Mode)
	{
	case EMode::Kei:
		return Kind == ftcore::RoadVehicleKind::Kei;
	case EMode::All:
		return Kind == ftcore::RoadVehicleKind::Kei || Kind == ftcore::RoadVehicleKind::Sedan || Kind == ftcore::RoadVehicleKind::Taxi ||
			Kind == ftcore::RoadVehicleKind::Van || Kind == ftcore::RoadVehicleKind::KeiTruck;
	default:
		return false;
	}
}
} // namespace

bool Place(FSet& Set, FFTCellInstancer& Cells, double Side, const ftcore::RoadVehicle& V, bool& bOk)
{
	bOk = true;
	if (!Takes(Set, V.kind))
	{
		return false;
	}
	if (!Set.Mesh)
	{
		++Set.Fallback;
		return false;
	}
	const ftcore::MassingVec At = ftcore::massingEnginePoint(Side, V.x, V.y, V.z);
	const FTransform Transform(FRotator(0.0, ftcore::massingEngineYaw(V.rot), 0.0), FVector(At.x, At.y, At.z) * 100.0);
	const ftcore::ParkedCarLook Look = ftcore::parkedCarLook(V.id, Set.Seed, Set.LampShare);
	float Data[ftcore::kParkedCarCustomFloats] = {};
	ftcore::parkedCarCustomData(Look, Data);
	FFTCellOptions Options;
	Options.Cull = EFTCellCull::Car;
	Options.Tag = FName(*FString::Printf(TEXT("FTVehicle_%s"), UTF8_TO_TCHAR(ftcore::roadVehicleName(V.kind))));
	Options.Label = FString(UTF8_TO_TCHAR(V.id.c_str()));
	bOk = Cells.Add(Set.Mesh, {}, Transform, TArrayView<const float>(Data, ftcore::kParkedCarCustomFloats), EFTCellKind::Solid, Options).IsValid();
	if (bOk)
	{
		++Set.Placed;
	}
	return true;
}

void LogSummary(const FSet& Set)
{
	if (Set.Mode == EMode::Off)
	{
		return;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -KeiCars: %d parked cars as %s, %d left as box stand-ins for want of the mesh."),
		Set.Placed, MeshName, Set.Fallback);
}
} // namespace FTBuildKeiCars
