#include "FTPlateNavNull.h"

#include "AI/NavigationModifier.h"
#include "AI/Navigation/NavigationRelevantData.h"
#include "Engine/CollisionProfile.h"
#include "NavAreas/NavArea_Null.h"

UFTPlateNavNull::UFTPlateNavNull()
{
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);
	CanCharacterStepUpOn = ECB_No;
	bDrawOnlyIfSelected = true; // the shape already hides in game
	bCanEverAffectNavigation = true;
	bDynamicObstacle = true;
	SetAreaClassOverride(UNavArea_Null::StaticClass());
	Mobility = EComponentMobility::Static;
}

void UFTPlateNavNull::GetNavigationData(FNavigationRelevantData& Data) const
{
	// The primitive's own data (nothing here: no fill under it), not the shape's one box of the whole extent.
	UPrimitiveComponent::GetNavigationData(Data);
	const FTransform& LocalToWorld = GetComponentTransform();
	for (const FBox& Box : NullBoxes)
	{
		Data.Modifiers.Add(FAreaNavModifier(Box, LocalToWorld, UNavArea_Null::StaticClass()));
	}
}
