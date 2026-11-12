#pragma once

// Anim class for a curve-mode face mesh (UFTFaceComponent's curve mode, a MetaHuman face): copies the body's
// pose (the mesh's attach parent, FAnimNode_CopyPoseFromMesh) and writes the face component's control curves
// on top. The mesh's post-process anim blueprint (the rig logic) then turns the curves into the face. The face
// mesh must not follow a leader pose: a follower never runs its own anim instance or post-process.

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "AnimNodes/AnimNode_CopyPoseFromMesh.h"

#include "FTFaceCurveAnimInstance.generated.h"

class UFTFaceComponent;

// The game thread copies the curve values in PreUpdate; Evaluate only reads them. The name and value arrays
// are sized at the first update after a bind, never per frame.
struct FFTFaceCurveAnimProxy : public FAnimInstanceProxy
{
	FFTFaceCurveAnimProxy() = default;
	explicit FFTFaceCurveAnimProxy(UAnimInstance* InInstance);

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;
	virtual FAnimNode_Base* GetCustomRootNode() override { return &CopyPose; }
	virtual void GetCustomNodes(TArray<FAnimNode_Base*>& OutNodes) override { OutNodes.Add(&CopyPose); }

private:
	FAnimNode_CopyPoseFromMesh CopyPose;
	TArray<FName> Names;
	TArray<float> Values;
};

UCLASS(Transient)
class FORGOTTENTHIRD_API UFTFaceCurveAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	// The face component on the owning actor (cached on first use).
	UFTFaceComponent* GetFace();

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
	TWeakObjectPtr<UFTFaceComponent> Face;
};
