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
//
// With -RetargetFrom=<IK Rig path> (and optionally -Clips=<anim path>,<anim path>...) it also
// writes RTG_<source mesh>_to_<mesh> in Dest, with the given rig as source and this mesh's rig as
// target (default op stack, chains mapped by exact name, a foot chain to a source toe chain when
// the source has no foot chain), retargets the clips into
// Dest/Retargeted and logs each result like the clip check, with its feet' planted ground speed
// (the capsule speed at which it plays at rate 1). It returns 1 if a target chain is
// unmapped or a retargeted clip comes out empty. Add -AlignPose when the two rest poses differ (a
// T-pose source onto an A-pose MetaHuman): the target gets a retarget pose with its chains turned
// to the source's directions. Example (proof_head's rom onto test_human):
//
//   -run=FTBuildRig -Mesh=/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_human.test_human
//       -Dest=/Game/Characters/TestHuman/Rig
//       -RetargetFrom=/Game/Characters/ProofHead/Rig/IK_proof_head.IK_proof_head
//       -Clips=/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_headrom.proof_headrom

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
