#pragma once

// Builds the IK Rig and IK Retargeter for an imported character, so retargeting needs no editor
// clicks:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildRig
//       -Mesh=/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head
//       -Dest=/Game/Characters/ProofHead/Rig -NullRHI -unattended -nopause -nosplash
//
// First it checks the clips that came in with the mesh (every AnimSequence in the mesh's folder):
// frame count, how many bone tracks move over time or differ from the bind pose, and how many
// float (morph) curves change. Then it writes IK_<mesh> with the retarget root at the pelvis and
// chains for the spine, neck, head, arms, legs and fingers (auto-characterized from the skeleton,
// with a hand-written fallback for any chain the template misses), and RTG_<mesh> with that rig as
// its source. No stock mannequin ships in Engine/Content, so the target side is left empty.
// Returns 1 when the mesh can't be loaded, a chain is missing or invalid, or a save fails.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildRigCommandlet.generated.h"

UCLASS()
class UFTBuildRigCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildRigCommandlet();

	virtual int32 Main(const FString& Params) override;
};
