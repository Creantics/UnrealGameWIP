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
//   -MaskFromColour         max distance = -Max x the sim mesh's vertex colour red (0 pinned, 255 free) instead of by
//                           height, so a cape can be pinned along its collar and shoulders; fails when no vertex
//                           has red 0 (nothing would pin the cloth)
//   -SimSlot=<slot> -RenderSlot=<slot>
//                           make the cloth from the section drawn with material slot SimSlot (a coarse sim proxy), take
//                           that section out of the mesh, and drive the RenderSlot section (the fine mesh) with it;
//                           -SimSlot alone draws on its own section, -RenderSlot alone is an error
//   -Density= -Damping= -LocalDamping= -Edge= -Bend= -Tether= -Drag= -Lift= -Thickness= -Friction= -GravityScale=
//   -SelfCollide | -NoSelfCollide
//                           Chaos cloth settings (kg/m2, 0..1 stiffness, cm); any left out keep the asset's own
//                           (an asset with no Chaos config gets one made); each one given is logged as read back;
//                           -SelfCollide with -NoSelfCollide is an error. All flags are checked before the mesh changes
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
