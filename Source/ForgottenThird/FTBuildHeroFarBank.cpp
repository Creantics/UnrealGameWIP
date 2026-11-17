#include "FTBuildHeroFarBank.h"

// Built by FTBuildMassingWorld (a commandlet) only: editor builds only.
#if WITH_EDITOR

#include "Engine/StaticMesh.h"
#include "FTCellInstancer.h"
#include "FTNightLightData.h"
#include "ForgottenThird.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

#include "ftcore/HeroFarBank.h"
#include "ftcore/MassingPlan.h"

namespace FTHeroFarBank
{
namespace
{
// A plan vector (x east, y north) as an engine one (y south), unscaled.
FVector EngineDir(double X, double Y, double Z)
{
	return FVector(X, -Y, Z);
}

FVector EngineCm(double Side, const ftcore::HeroPoint& P)
{
	const ftcore::MassingVec E = ftcore::massingEnginePoint(Side, P.x, P.y, P.z);
	return FVector(E.x, E.y, E.z) * 100.0;
}

// The instance whose mesh axes X, Y, Z land on the engine vectors RowX, RowY, RowZ (scaled), the mesh's bounds centre
// on Centre (cm).
FTransform Frame(const FVector& RowX, const FVector& RowY, const FVector& RowZ, const FVector& Centre, const FVector& LocalCentre)
{
	const FVector Origin = Centre - (LocalCentre.X * RowX + LocalCentre.Y * RowY + LocalCentre.Z * RowZ);
	const FMatrix M(FPlane(RowX.X, RowX.Y, RowX.Z, 0.0), FPlane(RowY.X, RowY.Y, RowY.Z, 0.0), FPlane(RowZ.X, RowZ.Y, RowZ.Z, 0.0),
		FPlane(Origin.X, Origin.Y, Origin.Z, 1.0));
	return FTransform(M);
}

// A kit point's linear map onto the imported mesh (FTBuildLots' FitAxes, on the far bank's kit sizes): mesh (x, y) =
// A * kit (x, y), z unchanged; the four quarter turns, each with and without a mirror in y.
bool FitAxes(const ftcore::FarKitSize& K, const FBox& MeshCm, int32 (&Fit)[2][2])
{
	const double Kx0 = K.minX, Kx1 = K.minX + K.sx, Ky0 = K.minY, Ky1 = K.minY + K.sy;
	const FVector Min = MeshCm.Min / 100.0, Max = MeshCm.Max / 100.0;
	double Error = TNumericLimits<double>::Max();
	static const int32 Turns[4][2][2] = {{{1, 0}, {0, 1}}, {{0, -1}, {1, 0}}, {{-1, 0}, {0, -1}}, {{0, 1}, {-1, 0}}};
	for (int32 T = 0; T < 4; ++T)
	{
		for (int32 Mi = 0; Mi < 2; ++Mi)
		{
			int32 A[2][2];
			for (int32 R = 0; R < 2; ++R)
			{
				A[R][0] = Turns[T][R][0];
				A[R][1] = Turns[T][R][1] * (Mi ? -1 : 1);
			}
			double Lo[2], Hi[2];
			for (int32 R = 0; R < 2; ++R)
			{
				const double P[2] = {A[R][0] * Kx0, A[R][0] * Kx1};
				const double Q[2] = {A[R][1] * Ky0, A[R][1] * Ky1};
				Lo[R] = FMath::Min(P[0], P[1]) + FMath::Min(Q[0], Q[1]);
				Hi[R] = FMath::Max(P[0], P[1]) + FMath::Max(Q[0], Q[1]);
			}
			const double E = FMath::Abs(Lo[0] - Min.X) + FMath::Abs(Hi[0] - Max.X) + FMath::Abs(Lo[1] - Min.Y) + FMath::Abs(Hi[1] - Max.Y) +
				FMath::Abs(K.minZ - Min.Z) + FMath::Abs(K.minZ + K.sz - Max.Z);
			if (E < Error)
			{
				Error = E;
				FMemory::Memcpy(Fit, A, sizeof A);
			}
		}
	}
	return Error < 0.12;
}

UStaticMesh* LoadPath(const FString& Path)
{
	return LoadObject<UStaticMesh>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
}

UMaterialInterface* SlotMaterial(UStaticMesh* Mesh, const TCHAR* Name)
{
	if (Mesh)
	{
		for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			if (Slot.MaterialSlotName == FName(Name))
			{
				return Slot.MaterialInterface;
			}
		}
	}
	return nullptr;
}
} // namespace

int32 Build(FFTCellInstancer& Cells, double Side, UMaterialInterface* Stucco, UMaterialInterface* Timber, UMaterialInterface* Dark,
	UMaterialInterface* WindowGlow, UStaticMesh* LanternLarge, UStaticMesh* LanternSmall, UMaterialInterface* LargePaper,
	UMaterialInterface* SmallPaper, UFTNightLightData* LampData, float LampScale)
{
	const ftcore::HeroFarBankSpec Spec;
	static ftcore::HeroFarBankLayout Layout; // big: kept off the stack (build time only)
	ftcore::heroFarBankLayout(Spec, Layout);
	if (Layout.overflow)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld -HeroCorner: far bank: the layout overflowed its arrays (some pieces left out)."));
	}
	int32 Added = 0;

	// The kit pieces, each fitted to the kit table's frame once.
	UStaticMesh* Kit[static_cast<int32>(ftcore::FarKit::Count)] = {};
	int32 KitFit[static_cast<int32>(ftcore::FarKit::Count)][2][2] = {};
	for (int32 K = 0; K < static_cast<int32>(ftcore::FarKit::Count); ++K)
	{
		const ftcore::FarKit Id = static_cast<ftcore::FarKit>(K);
		const FString Name = UTF8_TO_TCHAR(ftcore::farKitName(Id));
		UStaticMesh* const Mesh = LoadPath(FString::Printf(TEXT("/Game/Props/BKIT/%s/StaticMeshes/%s"), *Name, *Name));
		if (!Mesh)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld -HeroCorner: far bank: kit piece %s isn't imported; left out."), *Name);
			continue;
		}
		if (!FitAxes(ftcore::farKitSize(Id), Mesh->GetBoundingBox(), KitFit[K]))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld -HeroCorner: far bank: kit piece %s's bounds don't fit the kit table; left out."), *Name);
			continue;
		}
		Kit[K] = Mesh;
		Stucco = Stucco ? Stucco : SlotMaterial(Mesh, TEXT("jpk_wall_stucco"));
		Timber = Timber ? Timber : SlotMaterial(Mesh, TEXT("jpk_timber"));
		Dark = Dark ? Dark : SlotMaterial(Mesh, TEXT("jpk_dark"));
	}
	const FName Glass(TEXT("jpk_glass"));
	int32 Pieces = 0;
	for (int32 I = 0; I < Layout.pieceCount; ++I)
	{
		const ftcore::FarPiece& P = Layout.pieces[I];
		const int32 K = static_cast<int32>(P.kit);
		UStaticMesh* const Mesh = Kit[K];
		if (!Mesh)
		{
			continue;
		}
		// The kit frame on the plan: x along the face (out turned anticlockwise), -y out of it, z up; a mesh axis i is the
		// kit vector (A[i][0], A[i][1]) (A orthogonal: its inverse its transpose).
		const double AlongX = -P.outY, AlongY = P.outX;
		FVector Rows[2];
		for (int32 R = 0; R < 2; ++R)
		{
			const double Kx = KitFit[K][R][0], Ky = KitFit[K][R][1];
			Rows[R] = EngineDir(Kx * AlongX - Ky * P.outX, Kx * AlongY - Ky * P.outY, 0.0);
		}
		const FVector E = EngineCm(Side, P.at);
		const FMatrix M(FPlane(Rows[0].X, Rows[0].Y, 0.0, 0.0), FPlane(Rows[1].X, Rows[1].Y, 0.0, 0.0), FPlane(0.0, 0.0, 1.0, 0.0),
			FPlane(E.X, E.Y, E.Z, 1.0));
		TArray<UMaterialInterface*, TInlineAllocator<8>> Mats;
		for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			Mats.Add(P.lit && WindowGlow && Slot.MaterialSlotName == Glass ? WindowGlow : nullptr);
		}
		FFTCellOptions Opts;
		Opts.Cull = EFTCellCull::None;
		Opts.Label = FString::Printf(TEXT("HC-FARFRONT-%d-%s"), P.front + 1, UTF8_TO_TCHAR(ftcore::farKitName(P.kit)));
		Cells.Add(Mesh, Mats, FTransform(M), {}, EFTCellKind::Solid, Opts);
		++Pieces;
	}

	// The boxes and cylinders: the fronts' bodies, the boats.
	UStaticMesh* const Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* const Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	int32 Boxes = 0;
	for (int32 I = 0; I < Layout.boxCount; ++I)
	{
		const ftcore::FarBox& B = Layout.boxes[I];
		UStaticMesh* const Mesh = B.shape == ftcore::FarShape::Cube ? Cube : Cylinder;
		if (!Mesh)
		{
			continue;
		}
		const FBox Bounds = Mesh->GetBoundingBox();
		const FVector Size = Bounds.GetSize(), LocalCentre = Bounds.GetCenter();
		const double T = FMath::DegreesToRadians(B.tiltDeg), C = FMath::Cos(T), S = FMath::Sin(T);
		// The box's frame: along, out tilted down by the tilt, up tilted out by it (its top rising towards -out).
		const FVector Along = EngineDir(-B.outY, B.outX, 0.0);
		const FVector Out = EngineDir(C * B.outX, C * B.outY, -S);
		const FVector Up = EngineDir(S * B.outX, S * B.outY, C);
		FVector RowX, RowY, RowZ;
		if (B.shape == ftcore::FarShape::CylinderAlong)
		{
			RowX = Out * (B.depth * 100.0 / Size.X);
			RowY = Up * (B.height * 100.0 / Size.Y);
			RowZ = Along * (B.along * 100.0 / Size.Z);
		}
		else
		{
			RowX = Along * (B.along * 100.0 / Size.X);
			RowY = Out * (B.depth * 100.0 / Size.Y);
			RowZ = Up * (B.height * 100.0 / Size.Z);
		}
		UMaterialInterface* Mat = B.mat == ftcore::FarMat::Stucco ? Stucco : (B.mat == ftcore::FarMat::Timber ? Timber : Dark);
		UMaterialInterface* const Mats[1] = {Mat};
		FFTCellOptions Opts;
		Opts.Cull = B.solid ? EFTCellCull::None : EFTCellCull::Prop;
		Opts.Label = FString::Printf(TEXT("HC-FARBOX-%d"), I + 1);
		Cells.Add(Mesh, MakeArrayView(Mats), Frame(RowX, RowY, RowZ, EngineCm(Side, B.centre), LocalCentre), {},
			B.solid ? EFTCellKind::Solid : EFTCellKind::NoCollide, Opts);
		++Boxes;
	}

	// The rope edge: posts, and the rope a rod per polyline segment.
	int32 Posts = 0, Rods = 0;
	if (Cylinder)
	{
		const FBox Bounds = Cylinder->GetBoundingBox();
		const FVector Size = Bounds.GetSize(), LocalCentre = Bounds.GetCenter();
		UMaterialInterface* const PostMats[1] = {Timber};
		for (int32 I = 0; I < Layout.postCount; ++I)
		{
			const ftcore::QuayPost& P = Layout.posts[I];
			const ftcore::HeroPoint Mid{P.foot.x, P.foot.y, P.foot.z + 0.5 * P.height - 0.05};
			FFTCellOptions Opts;
			Opts.Cull = EFTCellCull::Prop;
			Opts.Label = FString::Printf(TEXT("HC-ROPEPOST-%d"), I + 1);
			Cells.Add(Cylinder, MakeArrayView(PostMats),
				Frame(FVector(P.diameter * 100.0 / Size.X, 0, 0), FVector(0, P.diameter * 100.0 / Size.Y, 0),
					FVector(0, 0, (P.height + 0.1) * 100.0 / Size.Z), EngineCm(Side, Mid), LocalCentre),
				{}, EFTCellKind::Solid, Opts);
			++Posts;
		}
		UMaterialInterface* const RopeMats[1] = {Dark};
		for (int32 I = 0; I < Layout.ropeCount; ++I)
		{
			const ftcore::RopeSpan& R = Layout.ropes[I];
			for (int32 Pt = 1; Pt < ftcore::kRopePoints; ++Pt)
			{
				const FVector From = EngineCm(Side, R.pt[Pt - 1]), To = EngineCm(Side, R.pt[Pt]);
				const FVector Axis = (To - From).GetSafeNormal();
				const double Len = FVector::Dist(From, To) + 1.0; // a centimetre's overlap at each joint
				const FVector Side1 = FVector::CrossProduct(Axis, FVector::UpVector).GetSafeNormal();
				const FVector Side2 = FVector::CrossProduct(Axis, Side1); // (Side1, Side2, Axis) right-handed
				const double D = Spec.ropeDiameter * 100.0;
				FFTCellOptions Opts;
				Opts.Cull = EFTCellCull::Prop;
				Opts.bCastShadow = false;
				Opts.Label = FString::Printf(TEXT("HC-ROPE-%d-%d"), I + 1, Pt);
				Cells.Add(Cylinder, MakeArrayView(RopeMats),
					Frame(Side1 * (D / Size.X), Side2 * (D / Size.Y), Axis * (Len / Size.Z), 0.5 * (From + To), LocalCentre), {},
					EFTCellKind::NoCollide, Opts);
				++Rods;
			}
		}
	}

	// Lanterns, hung by their pivot, their papers glowing.
	int32 Lanterns = 0;
	for (int32 I = 0; I < Layout.lanternCount; ++I)
	{
		const ftcore::FarLantern& L = Layout.lanterns[I];
		UStaticMesh* const Mesh = L.large ? LanternLarge : LanternSmall;
		if (!Mesh)
		{
			continue;
		}
		UMaterialInterface* const Paper = L.large ? LargePaper : SmallPaper;
		TArray<UMaterialInterface*, TInlineAllocator<8>> Mats;
		for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			Mats.Add(Paper && Slot.MaterialSlotName.ToString().Contains(TEXT("paper")) ? Paper : nullptr);
		}
		FFTCellOptions Opts;
		Opts.Cull = EFTCellCull::Prop;
		Opts.bCastShadow = false;
		Opts.Label = FString::Printf(TEXT("HC-FARLANTERN-%d"), I + 1);
		Cells.Add(Mesh, Mats, FTransform(FRotator::ZeroRotator, EngineCm(Side, L.hook), FVector(L.scale)), {}, EFTCellKind::NoCollide, Opts);
		++Lanterns;
	}

	// Lamp slots.
	int32 Lamps = 0;
	if (LampData)
	{
		while (LampData->LampGain.Num() < LampData->LampAt.Num())
		{
			LampData->LampGain.Add(1.f);
		}
		for (int32 I = 0; I < Layout.lampCount; ++I)
		{
			LampData->LampAt.Add(FVector3f(EngineCm(Side, Layout.lamps[I].at)));
			LampData->LampGain.Add(Layout.lamps[I].gain * LampScale);
			++Lamps;
		}
	}

	// Trees: the kit's broadleaf and park trees, scaled to their height.
	static const TCHAR* const TreeNames[ftcore::kFarTreeModels] = {TEXT("broadleaf-a"), TEXT("broadleaf-b"), TEXT("park-a"), TEXT("park-b")};
	UStaticMesh* TreeMeshes[ftcore::kFarTreeModels] = {};
	for (int32 K = 0; K < ftcore::kFarTreeModels; ++K)
	{
		TreeMeshes[K] = LoadPath(FString::Printf(TEXT("/Game/Props/TREE-%s/TREE-%s"), TreeNames[K], TreeNames[K]));
		if (!TreeMeshes[K])
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld -HeroCorner: far bank: tree TREE-%s isn't imported; its trees left out."), TreeNames[K]);
		}
	}
	int32 Trees = 0;
	for (int32 I = 0; I < Layout.treeCount; ++I)
	{
		const ftcore::FarTree& T = Layout.trees[I];
		UStaticMesh* const Mesh = TreeMeshes[T.model];
		if (!Mesh)
		{
			continue;
		}
		const FBox Bounds = Mesh->GetBoundingBox();
		const double Scale = Bounds.GetSize().Z > 1.0 ? T.height * 100.0 / Bounds.GetSize().Z : 1.0;
		const FVector Foot = EngineCm(Side, T.foot) - FVector(0.0, 0.0, Bounds.Min.Z * Scale);
		FFTCellOptions Opts;
		Opts.Cull = EFTCellCull::Tree;
		Opts.Tag = TEXT("Tree");
		Opts.Label = FString::Printf(TEXT("HC-FARTREE-%d"), I + 1);
		Cells.Add(Mesh, {}, FTransform(FRotator(0.0, -T.yawDeg, 0.0), Foot, FVector(Scale)), {}, EFTCellKind::Solid, Opts);
		++Trees;
	}

	Added = Pieces + Boxes + Posts + Rods + Lanterns + Trees;
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld -HeroCorner: far bank: %d fronts (%d kit pieces, %d boxes), %d rope posts, %d rope rods, "
		"%d boats, %d lanterns, %d lamp slots, %d trees (%d instances)."), Layout.frontCount, Pieces, Boxes, Posts, Rods, Layout.boatCount,
		Lanterns, Lamps, Trees, Added);
	return Added;
}
} // namespace FTHeroFarBank

#endif // WITH_EDITOR
