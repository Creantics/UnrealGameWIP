#pragma once

// Turns an imported skeletal mesh (a skirt, a sash's tails) into Chaos cloth:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildCloth -Mesh=<object path> -Physics=<object path>
//       -NullRHI -unattended -nopause -nosplash
//
// Strips any clothing the mesh already has, makes a clothing asset from LOD 0 section 0, paints its max distance
// by height (0 within -Pin cm of the mesh's top, so the waistband stays skinned, rising smoothly to -Max cm at the
// hem), takes the collision capsules from the physics asset (the body's, so the cloth falls over the legs) and
// saves the mesh. It also turns on the clothing usage of every base material on the mesh (saving those packages),
// since the engine draws a clothed section with its default material otherwise. A fresh import (the piece's folder
// deleted first, as textures and materials aren't replaced) has no cloth, so run this again after every import.
//
//   -Mesh=<object path>     the skeletal mesh to clothe
//   -Physics=<object path>  the physics asset the cloth collides with (default: none)
//   -Max=<cm>               max distance at the hem (default 25)
//   -Pin=<cm>               pinned band at the top (default 3)
//   -Verify                 write nothing; report the mesh's clothing
//
// Or, to make a physics asset for cloth to collide with from one made for a ragdoll:
//
//   -FitPhysics=<object path> -Physics=<source> -Body=<skeletal mesh> [-Fit=0.7] [-Pad=1] [-Drop=hand,thumb]
//
// copies -Physics to -FitPhysics and shrinks each capsule to the body: the radius becomes the -Fit quantile of the
// distances from its axis of the vertices nearest it, plus -Pad cm (for the clothes underneath), never larger. Bodies
// whose bone name contains a -Drop word (comma separated) lose their shapes.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildClothCommandlet.generated.h"

UCLASS()
class UFTBuildClothCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildClothCommandlet();

	virtual int32 Main(const FString& Params) override;
};
