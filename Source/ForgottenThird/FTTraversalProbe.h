#pragma once

// What a body has ahead of it to vault or climb, for the lead (AFTExploreCharacter) and the party followers
// (AFTPartyFollower) alike: forward traces from the feet find the near face, a down trace just past it the top, steps
// along the top its depth, then the floor beyond a thin top or room to stand on a deep one (ftcore::TraversalObstacle).
// And the mark the lead leaves where it begins a vault or a climb, for its followers to go the same way where they meet
// it.

#include "CoreMinimal.h"

#include "ftcore/Traversal.h"

class UWorld;
struct FCollisionQueryParams;

// Where a vault or a climb began: the point its way in met the near face, on the ground under it (cm), and that way
// (level, unit).
struct FFTTraversalMark
{
	FVector Face = FVector::ZeroVector;
	FVector Way = FVector::ForwardVector;
};

namespace FTTraversalProbe
{
// Probes from Feet (cm) along Fwd (level, unit) for a body Radius wide and twice HalfHeight tall (cm: room to stand on a
// deep top), ignoring what Query ignores. On the ground: a top to go over (thin, with a floor beyond no lower than
// Tuning's deepest drop) or onto (deep, with room); with bAir, a deep top to catch, from below the feet to past the
// hands. False when there is none. In is the way in: on the ground Fwd (the clips go the way the body faces), in the air
// square into the face (the body turns to it as it catches).
FORGOTTENTHIRD_API bool Probe(const UWorld& World, const FCollisionQueryParams& Query, const FVector& Feet, const FVector& Fwd, float Radius,
	float HalfHeight, const ftcore::TraversalTuning& Tuning, bool bAir, ftcore::TraversalObstacle& Out, FVector& In);
}
