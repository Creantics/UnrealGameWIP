#pragma once

// Builds the physics assets from the Core data:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildPhysics -NullRHI -unattended -nopause -nosplash
//
// Reads Data/Physics/materials.json through ftcore::importPhysMaterials and writes one UPhysicalMaterial per
// entry as /Game/Physics/PM_<Name>. Then binds Data/Physics/ragdoll/humanoid_map.json to a skeletal mesh and
// writes a separate ragdoll physics asset, /Game/Physics/PHYS_<mesh>_Ragdoll, tuned by a ragdoll profile. The
// mesh's own imported physics asset is never touched; the ragdoll asset is swapped in at run time by
// UFTPhysicsComponent. Existing files are replaced. See docs/PHYSICS_UE.md.
//
//   -Mesh=<object path>  the skeletal mesh to bind (default: the proof head)
//   -Profile=<name>      Data/Physics/ragdoll/<name>.json (default: gentle)
//   -NoRagdoll           materials only
//   -Verify              write nothing; load the existing assets and check them against the data

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildPhysicsCommandlet.generated.h"

UCLASS()
class UFTBuildPhysicsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildPhysicsCommandlet();

	virtual int32 Main(const FString& Params) override;
};
