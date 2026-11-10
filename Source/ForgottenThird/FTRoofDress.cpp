#include "FTRoofDress.h"

#include "ForgottenThird.h"

#include "FTCellInstancer.h"

#include "Engine/StaticMesh.h"
#include "Misc/Parse.h"

namespace FTRoofDress
{
FOptions ParseOptions(const TCHAR* Params)
{
	FOptions Options;
	Options.bScatter = FParse::Param(Params, TEXT("RoofScatter"));
	FParse::Value(Params, TEXT("RoofHut="), Options.HutMesh);
	FParse::Value(Params, TEXT("RoofHutTurn="), Options.HutTurn);
	return Options;
}

void ApplyStyle(const FOptions& Options, ftcore::MassingRoofStyle& Style)
{
	Style.scatter = Options.bScatter;
}

bool FHuts::Begin(const FOptions& Options)
{
	Turn = Options.HutTurn;
	if (Options.HutMesh.IsEmpty())
	{
		return true;
	}
	Mesh = LoadObject<UStaticMesh>(nullptr, *Options.HutMesh);
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -RoofHut: couldn't load '%s'; the huts stay cubes."), *Options.HutMesh);
		return false;
	}
	const FBox Bounds = Mesh->GetBoundingBox();
	MeshMin = ftcore::MassingVec{Bounds.Min.X / 100.0, Bounds.Min.Y / 100.0, Bounds.Min.Z / 100.0};
	MeshMax = ftcore::MassingVec{Bounds.Max.X / 100.0, Bounds.Max.Y / 100.0, Bounds.Max.Z / 100.0};
	return true;
}

bool FHuts::Take(const ftcore::MassingBox& Piece, double Side)
{
	if (!Mesh || Piece.kind != "hut")
	{
		return false;
	}
	// The mesh fills the hut's box, its door to the box's -u side (the roof's middle on a scattered roof).
	ftcore::MassingKitPlace Place;
	if (!ftcore::massingKitPlacement(Piece, Side, MeshMin, MeshMax, true, ftcore::massingRoofHutTurn(Piece) + Turn, 0.0, Place))
	{
		return false;
	}
	Pending.Add(FTransform(FRotator(0.0, Place.yaw, 0.0), FVector(Place.at.x * 100.0, Place.at.y * 100.0, Place.at.z * 100.0),
		FVector(Place.scale.x, Place.scale.y, Place.scale.z)));
	return true;
}

int32 FHuts::Finish(FFTCellInstancer& Cells)
{
	if (!Mesh || Pending.Num() == 0)
	{
		return 0;
	}
	FFTCellOptions Options;
	Options.Label = TEXT("Roof_Hut");
	int32 Added = 0;
	for (const FTransform& Hut : Pending)
	{
		if (Cells.Add(Mesh, {}, Hut, {}, EFTCellKind::Solid, Options).IsValid())
		{
			++Added;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: roofs: %d stair-head huts as instances of %s."), Added, *Mesh->GetPathName());
	Pending.Reset();
	return Added;
}
} // namespace FTRoofDress
