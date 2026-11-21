#pragma once

// Makes the flesh-contact copies of one character's skin: the material chain of every slot that ends at the skin master,
// copied, with the copy master given a dent (world position offset) and its analytic normal fix (FTFleshContact.h):
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildFlesh -Mesh=<skeletal mesh object path>
//       [-Out=/Game/Local/FleshContact] [-Master=M_skin_unified_baked] [-Dump | -Verify]
//       -AllowCommandletRendering -unattended -nopause -nosplash
//
// The originals are never edited or saved. The copies go to <Out>/<mesh name>/<original name>_Flesh, replacing what is
// there, so the run is repeated as is after the character is rebuilt. They hold engine content: keep <Out> out of git.
//
// Every run first prints the chain of each slot (slot, each instance, the master) and checks, before anything is written:
// - at least one slot's chain ends at a master named -Master;
// - that master uses tangent-space normals and has no world position offset (its graph and its compiled shader);
// - its graph is one of the two shapes the insertion knows: material attributes into the master, or plain pins.
// A failed check exits 1 with the dump. -Dump stops after the checks.
//
// The copy master gets, per contact i in 0..ftcore::flesh::kMaxContacts-1, the vector parameters FleshC<i>_Pos
// (ref-pose point xyz, patch radius w), FleshC<i>_Nrm (ref-pose normal xyz, depth w) and FleshC<i>_Rot (ref to world
// quaternion), and the scalars FleshBulge and FleshBulgeWidth. Depth 0 leaves the skin exactly as it was.
// -Verify loads the copies of -Mesh, prints their chain, every parameter's default and the copy master's world position
// offset use, and exits 1 if one is missing or wrong.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildFleshCommandlet.generated.h"

UCLASS()
class UFTBuildFleshCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildFleshCommandlet();

	virtual int32 Main(const FString& Params) override;
};
