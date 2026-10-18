#pragma once

// Anim class for a face skeletal mesh: rotates the jaw and tongue_01..03 bones from the owning
// actor's UFTFaceComponent (GetJawRotation / GetTongueRotation), in local space on top of the rest
// pose. No Anim Blueprint needed. Bones the skeleton lacks are skipped and logged once.

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "BoneContainer.h"
#include "BoneIndices.h"
#include "Animation/BoneReference.h"

#include "FTFaceAnimInstance.generated.h"

class UFTFaceComponent;

// Runs on the anim worker thread: the game thread copies the rotations in PreUpdate, Evaluate only
// reads them. Nothing here allocates per frame.
struct FFTFaceAnimProxy : public FAnimInstanceProxy
{
	FFTFaceAnimProxy() = default;
	explicit FFTFaceAnimProxy(UAnimInstance* InInstance) : FAnimInstanceProxy(InInstance) {}

	static constexpr int32 NumBones = 4; // jaw, tongue_01..03

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

	FQuat Delta[NumBones] = { FQuat::Identity, FQuat::Identity, FQuat::Identity, FQuat::Identity };

private:
	FBoneReference Bones[NumBones];
	uint16 BoundSerial = 0;
	bool bBound = false;
	bool bLoggedMissing[NumBones] = {};
};

UCLASS(Transient)
class FORGOTTENTHIRD_API UFTFaceAnimInstance : public UAnimInstance
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
