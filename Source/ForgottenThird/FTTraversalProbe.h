#pragma once

// What a body has ahead of it to vault or climb, for the lead (AFTExploreCharacter) and the party followers
// (AFTPartyFollower) alike: forward traces from the feet find the near face, a down trace just past it the top, steps
// along the top its depth, then the floor beyond a thin top or room to stand on a deep one (ftcore::TraversalObstacle).
// And the mark the lead leaves where it begins a vault, a climb or a catch, for its followers to go the same way where
// they meet it.

#include "CoreMinimal.h"

#include "ftcore/Traversal.h"

class UWorld;
struct FCollisionQueryParams;

// Where a vault, a climb or a catch began: the point its way in met the near face, level with the feet as it began (on
// the ground under it, or in the air for a catch) (cm), that way (level, unit), and whether it was a catch (begun in
// the air, where a vault or a climb is begun on the ground).
struct FFTTraversalMark
{
	FVector Face = FVector::ZeroVector;
	FVector Way = FVector::ForwardVector;
	bool bCatch = false;
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
