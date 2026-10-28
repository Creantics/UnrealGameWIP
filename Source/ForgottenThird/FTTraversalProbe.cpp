#include "FTTraversalProbe.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"

namespace
{
// The probes (cm): the forward traces' heights above the feet and their reach (past the run clips' lead), the down
// trace's start above the feet, the step along the top, how far a top may stray from level and still be the same top,
// the gap past a thin top where the far floor is looked for, and how squarely (the cosine) a face must face the body.
constexpr float kProbeHeights[] = {45.f, 95.f};
constexpr float kProbeReach = 560.f;
constexpr float kProbeTop = 360.f;
constexpr float kProbeStep = 5.f;
constexpr float kTopTolerance = 15.f;
constexpr float kBackGap = 50.f;
constexpr float kProbeFacing = 0.8f;
// In the air (cm): the forward traces' heights above the feet (a ledge to catch may be from under the feet to past the
// hands), their reach, and how far below the feet the down trace looks for a top.
constexpr float kCatchHeights[] = {-60.f, 0.f, 60.f, 120.f};
constexpr float kCatchReach = 300.f;
constexpr float kCatchBelow = 120.f;
}

bool FTTraversalProbe::Probe(const UWorld& World, const FCollisionQueryParams& Query, const FVector& Feet, const FVector& Fwd, float Radius,
	float HalfHeight, const ftcore::TraversalTuning& Tuning, bool bAir, ftcore::TraversalObstacle& Out, FVector& In)
{
	const FCollisionObjectQueryParams Static(ECC_WorldStatic);
	auto Trace = [&World, &Query, &Static](const FVector& From, const FVector& To, FHitResult& Hit)
	{
		return World.LineTraceSingleByObjectType(Hit, From, To, Static, Query) && !Hit.bStartPenetrating;
	};
	auto Level = [](const FHitResult& Hit) { return Hit.ImpactNormal.Z > 0.7; };

	// The near face: the nearer hit of the forward traces on a wall facing the body.
	const TArrayView<const float> Heights = bAir ? MakeArrayView(kCatchHeights) : MakeArrayView(kProbeHeights);
	const float Reach = bAir ? kCatchReach : kProbeReach;
	FHitResult Wall;
	bool bWall = false;
	for (const float Height : Heights)
	{
		const FVector From = Feet + FVector(0.0, 0.0, Height);
		FHitResult Hit;
		if (Trace(From, From + Fwd * Reach, Hit) && FMath::Abs(Hit.ImpactNormal.Z) < 0.3
			&& FVector::DotProduct(-Hit.ImpactNormal.GetSafeNormal2D(), Fwd) > kProbeFacing && (!bWall || Hit.Distance < Wall.Distance))
		{
			Wall = Hit;
			bWall = true;
		}
	}
	if (!bWall)
	{
		return false;
	}
	// The way in: on the ground the way the body faces (its clips go that way); in the air square into the face, which
	// the body turns to as it catches.
	In = bAir ? FVector(-Wall.ImpactNormal.GetSafeNormal2D()) : Fwd;
	const float Distance = static_cast<float>(FVector::DotProduct(Wall.ImpactPoint - Feet, In));
	// The top: down just past the face, from above the highest clip (a start inside something is too high); in the air
	// down to below the feet too.
	const FVector Over = Feet + In * (Distance + kProbeStep);
	FHitResult Top;
	if (!Trace(Over + FVector(0.0, 0.0, kProbeTop), Over - FVector(0.0, 0.0, bAir ? kCatchBelow : 0.f), Top) || !Level(Top))
	{
		return false;
	}
	const double TopZ = Top.ImpactPoint.Z;
	// Its depth: on along the top while it stays level, as far as standing on it needs.
	float Depth = 0.f;
	for (float D = 2.f * kProbeStep; D <= Tuning.standDepth + kProbeStep; D += kProbeStep)
	{
		const FVector At = Feet + In * (Distance + D);
		FHitResult Hit;
		if (!Trace(FVector(At.X, At.Y, TopZ + kTopTolerance), FVector(At.X, At.Y, TopZ - kTopTolerance), Hit))
		{
			break;
		}
		Depth = D;
	}
	Out = ftcore::TraversalObstacle();
	Out.distance = Distance;
	Out.height = static_cast<float>(TopZ - Feet.Z);
	Out.depth = Depth;
	if (Depth < Tuning.standDepth)
	{
		// Thin: the floor beyond, no lower than the deepest drop. Nothing to catch in the air.
		if (bAir)
		{
			return false;
		}
		const FVector Beyond = Feet + In * (Distance + Depth + kBackGap);
		FHitResult Floor;
		if (Trace(FVector(Beyond.X, Beyond.Y, TopZ + kTopTolerance), FVector(Beyond.X, Beyond.Y, Feet.Z - Tuning.maxDrop - 20.0), Floor) && Level(Floor))
		{
			Out.backFloor = true;
			Out.backHeight = static_cast<float>(Floor.ImpactPoint.Z - Feet.Z);
		}
	}
	else
	{
		// Deep: room on the top for the body.
		const FVector Stand = Feet + In * (Distance + Radius + kProbeStep);
		if (World.OverlapBlockingTestByChannel(FVector(Stand.X, Stand.Y, TopZ + HalfHeight + 2.0), FQuat::Identity, ECC_Pawn,
				FCollisionShape::MakeCapsule(Radius, HalfHeight), Query))
		{
			return false;
		}
	}
	return true;
}
