#pragma once

// A cover plate's navmesh null (FTBuildMassingWorld; ftcore::massingNavObstacle): a hidden box with no collision,
// attached to the plate's actor, that the navmesh build takes as a dynamic obstacle of NavArea_Null in each of
// NullBoxes (component-local cm; ftcore::massingNullBand's strips). The plate's own mesh keeps its collision, so the
// lead and every trace still meet it; only the navmesh on its top, in its slab and on ground buried in it goes. The
// box's extent is the strips' union, so its bounds (the navigation octree's) hold every strip.
//
// A static navmesh bakes the nulls in when it is built (the area marking runs on every tile build); the component is
// saved with its actor, so a map loaded and built again (-Load) nulls the same strips.

#include "CoreMinimal.h"
#include "Components/BoxComponent.h"

#include "FTPlateNavNull.generated.h"

UCLASS(ClassGroup = (ForgottenThird))
class FORGOTTENTHIRD_API UFTPlateNavNull : public UBoxComponent
{
	GENERATED_BODY()

public:
	UFTPlateNavNull();

	// The strips, in the component's own frame, cm.
	UPROPERTY()
	TArray<FBox> NullBoxes;

	// One NavArea_Null modifier per strip, in place of the shape's single box.
	virtual void GetNavigationData(FNavigationRelevantData& Data) const override;
};
