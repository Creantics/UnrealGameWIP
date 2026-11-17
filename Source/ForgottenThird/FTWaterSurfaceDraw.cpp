#include "FTWaterSurfaceDraw.h"

#include "FTSceneLook.h"
#include "FTWaterSurfaceBuild.h"
#include "FTWaterSubsystem.h"
#include "ForgottenThird.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerStart.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

namespace
{
// Around the bodies the half-plane's tiles are laid across (m).
constexpr float kWaterTileMarginM = 400.f;
// How far the tide moves before it is pushed again (cm).
constexpr float kWaterTideStepCm = 0.2f;

template <typename T>
T* WaterLoadPath(const TCHAR* Path)
{
	const FString P(Path);
	return LoadObject<T>(nullptr, *(P + TEXT(".") + FPackageName::GetShortName(P)), nullptr, LOAD_NoWarn | LOAD_Quiet);
}

struct FWaterParamNames
{
	FName Waves[ftcore::kSeaWaveCount];
	FName Q = TEXT("Q");
	FName FadeCm = TEXT("FadeCm");
	FName LapShare = TEXT("LapShare");
	FName EdgeA = TEXT("EdgeA");
	FName EdgeB = TEXT("EdgeB");
	FName SurfaceOn = TEXT("SurfaceOn");
	FName WaveVerts = TEXT("WaveVerts");
	FName TideCm = TEXT("TideCm");
	FName ReachCm = TEXT("ReachCm");
	FWaterParamNames()
	{
		for (int32 I = 0; I < ftcore::kSeaWaveCount; ++I)
		{
			Waves[I] = FName(*FString::Printf(TEXT("W%d"), I));
		}
	}
};

const FWaterParamNames& WaterParamNames()
{
	static const FWaterParamNames N;
	return N;
}

ftcore::Vec2 WaterToM(const FVector& Cm)
{
	return {static_cast<float>(Cm.X * 0.01), static_cast<float>(Cm.Y * 0.01)};
}

void WaterGrow(ftcore::WaterTileBounds& B, bool& bAny, ftcore::Vec2 P)
{
	if (!bAny)
	{
		B.min = B.max = P;
		bAny = true;
		return;
	}
	B.min.x = FMath::Min(B.min.x, P.x);
	B.min.y = FMath::Min(B.min.y, P.y);
	B.max.x = FMath::Max(B.max.x, P.x);
	B.max.y = FMath::Max(B.max.y, P.y);
}

bool WaterIsWired(const UMaterialInterface* M)
{
	float V = 0.f;
	return M && M->GetScalarParameterValue(FHashedMaterialParameterInfo(WaterParamNames().SurfaceOn), V);
}
} // namespace

bool UFTWaterSurfaceDraw::Build(UWorld& World, const ftcore::WaterBodies& Water, const ftcore::WaterWaveSpec& Waves)
{
	if (bTried)
	{
		return bBuilt;
	}
	bTried = true;
	UStaticMesh* Tile = WaterLoadPath<UStaticMesh>(UFTWaterSubsystem::TileMeshPath);
	UMaterialInterface* SeaMaster = WaterLoadPath<UMaterialInterface>(FTScene::LookWaterMaterial);
	UMaterialInterface* CanalMaster = WaterLoadPath<UMaterialInterface>(FTScene::LookCanalWaterMaterial);
	if (!Tile || !WaterIsWired(SeaMaster))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Water: no moving surface (%s); FTBuildWaterSurface builds the tile, FTBuildLookMaterials -Water wires the masters."),
			!Tile ? TEXT("the tile mesh is missing") : TEXT("the water master has no surface inputs"));
		return false;
	}
	const UMaterial* SeaBase = SeaMaster->GetBaseMaterial();
	const UMaterial* CanalBase = CanalMaster ? CanalMaster->GetBaseMaterial() : nullptr;
	const int32 Count = static_cast<int32>(Water.count());

	// The flat planes and the bodies under them.
	struct FFlat
	{
		UStaticMeshComponent* Component = nullptr;
		uint32 Bodies = 0; // bit per body
		bool bOutside = false; // an instance on no body
	};
	TArray<FFlat> Found;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		TInlineComponentArray<UStaticMeshComponent*> Components(*It);
		for (UStaticMeshComponent* C : Components)
		{
			UMaterialInterface* M = C && C->GetNumMaterials() > 0 ? C->GetMaterial(0) : nullptr;
			const UMaterial* Base = M ? M->GetBaseMaterial() : nullptr;
			if (!Base || (Base != SeaBase && Base != CanalBase))
			{
				continue;
			}
			FFlat F;
			F.Component = C;
			// A plane whose material has no surface inputs keeps its bodies flat (it would vanish with nothing in its place).
			F.bOutside = !WaterIsWired(M);
			auto Mark = [&](const FVector& At)
			{
				const int B = Water.bodyAt(WaterToM(At));
				if (B < 0)
				{
					F.bOutside = true;
				}
				else
				{
					F.Bodies |= 1u << B;
				}
			};
			if (const UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(C))
			{
				FTransform T;
				for (int32 I = 0; I < Ism->GetInstanceCount(); ++I)
				{
					if (Ism->GetInstanceTransform(I, T, true))
					{
						Mark(T.GetLocation());
					}
				}
			}
			else
			{
				Mark(C->Bounds.Origin);
			}
			if (F.Bodies != 0 || F.bOutside)
			{
				Found.Add(F);
			}
		}
	}

	// Where the tiles go: across the boxes and polygons and the party's start, with a margin (a sea runs along it).
	ftcore::WaterTileBounds Bounds;
	bool bAny = false;
	for (int32 I = 0; I < Count; ++I)
	{
		const ftcore::WaterBody& B = Water.body(static_cast<std::size_t>(I));
		if (B.shape == ftcore::WaterShape::Box)
		{
			const float Ex = FMath::Abs(B.cosYaw) * B.half.x + FMath::Abs(B.sinYaw) * B.half.y;
			const float Ey = FMath::Abs(B.sinYaw) * B.half.x + FMath::Abs(B.cosYaw) * B.half.y;
			WaterGrow(Bounds, bAny, {B.centre.x - Ex, B.centre.y - Ey});
			WaterGrow(Bounds, bAny, {B.centre.x + Ex, B.centre.y + Ey});
		}
		else if (B.shape == ftcore::WaterShape::Polygon)
		{
			for (std::size_t P = 0; P < B.pointCount; ++P)
			{
				WaterGrow(Bounds, bAny, B.points[P]);
			}
		}
		else
		{
			WaterGrow(Bounds, bAny, B.centre);
		}
	}
	for (TActorIterator<APlayerStart> It(&World); It; ++It)
	{
		WaterGrow(Bounds, bAny, WaterToM(It->GetActorLocation()));
	}
	Bounds.min.x -= kWaterTileMarginM;
	Bounds.min.y -= kWaterTileMarginM;
	Bounds.max.x += kWaterTileMarginM;
	Bounds.max.y += kWaterTileMarginM;

	// Laid a whole body at a time under the cap: a body that doesn't fit (a sea stretched by a far start) keeps its flat
	// plane and is read flat.
	const ftcore::WaterTileSpec Spec;
	TArray<ftcore::WaterTile> Laid;
	Laid.SetNumUninitialized(static_cast<int32>(ftcore::kWaterMaxTiles));
	uint32 Dropped = 0;
	uint32 NeedPerBody[ftcore::kWaterMaxBodies] = {};
	const std::size_t LaidCount = ftcore::waterTileLayoutWhole(Water, Waves, Spec, Bounds, Laid.GetData(), ftcore::kWaterMaxTiles, &Dropped, NeedPerBody);
	Laid.SetNum(static_cast<int32>(LaidCount), EAllowShrinking::Yes);
	uint32 Tiled = 0;
	for (const ftcore::WaterTile& T : Laid)
	{
		Tiled |= 1u << T.body;
	}
	for (int32 I = 0; I < Count; ++I)
	{
		if (NeedPerBody[I] > 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Water: body %d wants %u tiles%s."), I, NeedPerBody[I],
				(Dropped & (1u << I)) != 0 ? TEXT(": over the cap, it keeps its flat plane") : TEXT(""));
		}
	}

	// A body is drawn by tiles only if every plane over it can be hidden: each of those planes lies on tiled bodies only.
	uint32 Drawn = Tiled;
	for (bool bChanged = true; bChanged;)
	{
		bChanged = false;
		for (const FFlat& F : Found)
		{
			if ((F.Bodies & Drawn) != 0 && (F.bOutside || (F.Bodies & ~Drawn) != 0))
			{
				Drawn &= ~F.Bodies;
				bChanged = true;
			}
		}
	}
	uint32 Covered = 0;
	for (const FFlat& F : Found)
	{
		Covered |= F.Bodies;
	}
	Drawn &= Covered;
	if (Drawn == 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Water: no body's flat plane can give way to the moving surface (%d planes found, %d bodies tiled)."), Found.Num(),
			FMath::CountBits(Tiled));
		return false;
	}

	Host = World.SpawnActor<AActor>();
	if (!Host)
	{
		return false;
	}
#if WITH_EDITOR
	Host->SetActorLabel(TEXT("FTWaterSurface"));
#endif
	USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
	Host->SetRootComponent(Root);
	Root->RegisterComponent();

	for (int32 I = 0; I < Count; ++I)
	{
		if ((Drawn & (1u << I)) == 0)
		{
			continue;
		}
		const ftcore::WaterBody& Body = Water.body(static_cast<std::size_t>(I));
		UStaticMeshComponent* Source = nullptr;
		for (const FFlat& F : Found)
		{
			if ((F.Bodies & (1u << I)) != 0)
			{
				Source = F.Component;
				break;
			}
		}
		// Wired, and so the part can't fail from here: a plane wearing an unwired material is bOutside, which took its
		// bodies out of Drawn above; the far sea wears the same tile mesh, already loaded.
		UMaterialInterface* Worn = Source->GetMaterial(0);
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Worn, this);
		const bool bSea = Body.shape == ftcore::WaterShape::HalfPlane;
		SetPartParams(*Mid, Body, true, bSea ? Spec.seaReachM * 100.f : 0.f);
		Mids.Add(Mid);
		MidGroup.Add(static_cast<uint8>(ftcore::waterDrawGroup(Body, Waves)));

		UHierarchicalInstancedStaticMeshComponent* Part = NewObject<UHierarchicalInstancedStaticMeshComponent>(Host);
		Part->SetStaticMesh(Tile);
		Part->SetMaterial(0, Mid);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCastShadow(false);
		Part->bAffectDistanceFieldLighting = false;
		Part->SetVisibleInRayTracing(false); // the traced scene would see the tiles undisplaced
		Part->SetMobility(EComponentMobility::Movable);
		Part->SetupAttachment(Root);
		Part->SetVisibility(false);
		Part->RegisterComponent();
		TArray<FTransform> Transforms;
		for (const ftcore::WaterTile& T : Laid)
		{
			if (T.body != I)
			{
				continue;
			}
			const double Yaw = FMath::RadiansToDegrees(FMath::Atan2(T.sinYaw, T.cosYaw));
			const double Scale = 100.0 / FTWaterSurfaceBuild::kTileCm;
			Transforms.Emplace(FRotator(0.0, Yaw, 0.0), FVector(T.centre.x * 100.0, T.centre.y * 100.0, T.z * 100.0),
				FVector(T.size.x * Scale, T.size.y * Scale, 1.0));
		}
		Part->AddInstances(Transforms, false, true);
		Tiles += Transforms.Num();
		TileParts.Add(Part);

		if (bSea)
		{
			AddFarSea(*Tile, Body, Source->Bounds.GetBox(), Mid, Laid, I);
		}
	}
	for (const FFlat& F : Found)
	{
		if ((F.Bodies & Drawn) != 0)
		{
			Flats.Add(F.Component);
		}
	}
	DrawnMask = 0;
	for (int32 I = 0; I < Count; ++I)
	{
		DrawnMask |= (Drawn & (1u << I)) != 0 ? (1u << I) : 0u;
	}
	bBuilt = TileParts.Num() > 0;
	if (!bBuilt)
	{
		DrawnMask = 0;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Water: the moving surface: %d tiles over %d bodies, %d flat planes give way, %d far sea planes."), Tiles, TileParts.Num(),
		Flats.Num(), FarParts.Num());
	return bBuilt;
}

void UFTWaterSurfaceDraw::AddFarSea(UStaticMesh& Tile, const ftcore::WaterBody& Body, const FBox& PlaneBounds, UMaterialInstanceDynamic* TileMid,
	const TArray<ftcore::WaterTile>& Laid, int32 BodyIndex)
{
	// The shore's frame: N out to sea, S along the shore; the tiles' span in it.
	const FVector2D C(Body.centre.x, Body.centre.y);
	const FVector2D N(Body.cosYaw, Body.sinYaw);
	const FVector2D S(-N.Y, N.X);
	double SMin = TNumericLimits<double>::Max(), SMax = -SMin, DMin = SMin, DMax = -SMin;
	for (const ftcore::WaterTile& T : Laid)
	{
		if (T.body != BodyIndex)
		{
			continue;
		}
		const FVector2D X(T.cosYaw, T.sinYaw);
		const FVector2D Y(-T.sinYaw, T.cosYaw);
		for (int32 K = 0; K < 4; ++K)
		{
			const FVector2D P = FVector2D(T.centre.x, T.centre.y) + X * (T.size.x * ((K & 1) ? 0.5 : -0.5)) + Y * (T.size.y * ((K & 2) ? 0.5 : -0.5)) - C;
			const double Along = FVector2D::DotProduct(P, S);
			const double Out = FVector2D::DotProduct(P, N);
			SMin = FMath::Min(SMin, Along);
			SMax = FMath::Max(SMax, Along);
			DMin = FMath::Min(DMin, Out);
			DMax = FMath::Max(DMax, Out);
		}
	}
	if (SMin > SMax)
	{
		return;
	}
	// The old plane's reach (m): along the shore about its centre, and as far out.
	const FVector2D Mid2(PlaneBounds.GetCenter().X * 0.01, PlaneBounds.GetCenter().Y * 0.01);
	const double Half = FMath::Max(PlaneBounds.GetExtent().X, PlaneBounds.GetExtent().Y) * 0.01;
	const double S0 = FVector2D::DotProduct(Mid2 - C, S);
	const double SLo = FMath::Min(S0 - Half, SMin), SHi = FMath::Max(S0 + Half, SMax);
	const double Far = FMath::Max(Half * 2.0, DMax + 1000.0);
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(TileMid->Parent, this);
	SetPartParams(*Mid, Body, false, 0.f);
	Mids.Add(Mid);
	MidGroup.Add(MidGroup.Last());
	auto Add = [&](double S1, double S2, double D1, double D2)
	{
		if (S2 - S1 < 1.0 || D2 - D1 < 1.0)
		{
			return;
		}
		const FVector2D At = C + S * (0.5 * (S1 + S2)) + N * (0.5 * (D1 + D2));
		UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(Host);
		Part->SetStaticMesh(&Tile);
		Part->SetMaterial(0, Mid);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCastShadow(false);
		Part->bAffectDistanceFieldLighting = false;
		Part->SetVisibleInRayTracing(false); // the traced scene would see the tiles undisplaced
		Part->SetMobility(EComponentMobility::Movable);
		Part->SetupAttachment(Host->GetRootComponent());
		Part->SetVisibility(false);
		// The tile mesh stretched (it is kTileCm square, centred); its x along N, y along S.
		const double Scale = 100.0 / FTWaterSurfaceBuild::kTileCm;
		Part->SetWorldTransform(FTransform(FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(N.Y, N.X)), 0.0), FVector(At.X * 100.0, At.Y * 100.0, Body.level * 100.0),
			FVector((D2 - D1) * Scale, (S2 - S1) * Scale, 1.0)));
		Part->RegisterComponent();
		FarParts.Add(Part);
	};
	Add(SLo, SHi, DMax, Far);  // past the tiles' reach
	Add(SLo, SMin, DMin, DMax); // beside them, either way along the shore
	Add(SMax, SHi, DMin, DMax);
}

void UFTWaterSurfaceDraw::SetPartParams(UMaterialInstanceDynamic& Mid, const ftcore::WaterBody& Body, bool bWaveVerts, float ReachCm)
{
	const FWaterParamNames& N = WaterParamNames();
	float Line[ftcore::kWaterEdgeFloats];
	ftcore::waterEdgeLine(Body, Line);
	Mid.SetVectorParameterValue(N.EdgeA, FLinearColor(Line[0], Line[1], Line[2], Line[3]));
	Mid.SetVectorParameterValue(N.EdgeB, FLinearColor(Line[4], Line[5], 0.f, 0.f));
	Mid.SetScalarParameterValue(N.SurfaceOn, 1.f);
	Mid.SetScalarParameterValue(N.WaveVerts, bWaveVerts ? 1.f : 0.f);
	Mid.SetScalarParameterValue(N.ReachCm, ReachCm);
}

void UFTWaterSurfaceDraw::SetShown(bool bOn)
{
	if (!bBuilt || bOn == bShown)
	{
		return;
	}
	bShown = bOn;
	for (UHierarchicalInstancedStaticMeshComponent* Part : TileParts)
	{
		if (Part)
		{
			Part->SetVisibility(bOn);
		}
	}
	for (UStaticMeshComponent* Part : FarParts)
	{
		if (Part)
		{
			Part->SetVisibility(bOn);
		}
	}
	for (UPrimitiveComponent* Flat : Flats)
	{
		if (Flat)
		{
			Flat->SetVisibility(!bOn);
		}
	}
}

void UFTWaterSurfaceDraw::PushWaves(const ftcore::WaterSurfaceModel& Model)
{
	if (!bBuilt)
	{
		return;
	}
	const FWaterParamNames& N = WaterParamNames();
	for (int32 G = 0; G < ftcore::kWaterDrawGroups; ++G)
	{
		const ftcore::WaterDrawParams P = ftcore::waterDrawParams(Model, static_cast<ftcore::WaterDrawGroup>(G));
		TideCm[G] = P.tideM * 100.f;
		bTideSet[G] = true;
		for (int32 I = 0; I < Mids.Num(); ++I)
		{
			UMaterialInstanceDynamic* Mid = Mids[I];
			if (!Mid || MidGroup[I] != G)
			{
				continue;
			}
			for (int32 W = 0; W < ftcore::kSeaWaveCount; ++W)
			{
				Mid->SetVectorParameterValue(N.Waves[W], FLinearColor(P.waves[W][0], P.waves[W][1], P.waves[W][2], P.waves[W][3]));
			}
			Mid->SetScalarParameterValue(N.Q, P.q);
			Mid->SetScalarParameterValue(N.FadeCm, P.fadeCm);
			Mid->SetScalarParameterValue(N.LapShare, P.lapShare);
			Mid->SetScalarParameterValue(N.TideCm, TideCm[G]);
		}
	}
}

void UFTWaterSurfaceDraw::PushTide(const ftcore::WaterSurfaceModel& Model)
{
	if (!bBuilt)
	{
		return;
	}
	const FWaterParamNames& N = WaterParamNames();
	for (int32 G = 0; G < ftcore::kWaterDrawGroups; ++G)
	{
		const float Cm = ftcore::waterDrawParams(Model, static_cast<ftcore::WaterDrawGroup>(G)).tideM * 100.f;
		if (bTideSet[G] && FMath::Abs(Cm - TideCm[G]) < kWaterTideStepCm)
		{
			continue;
		}
		TideCm[G] = Cm;
		bTideSet[G] = true;
		for (int32 I = 0; I < Mids.Num(); ++I)
		{
			if (Mids[I] && MidGroup[I] == G)
			{
				Mids[I]->SetScalarParameterValue(N.TideCm, Cm);
			}
		}
	}
}
