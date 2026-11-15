#pragma once

// Pose correctives on dressed pieces (Data/Anim/pose_correctives.json, Core/include/ftcore/PoseCorrective.h): a
// garment morph that a joint's flexion drives, e.g. trousers settled in a crouch. FTLook::AddPieces and SetFront call
// Bind for each worn piece; the owner gets one of these components only when some piece has a listed morph, so a
// lead with no such piece pays nothing. It ticks after the body (whose tick waits for its animation, parallel or not)
// and before the bound pieces, so a piece takes this frame's weight; it works whatever drives the body (the locomotion
// instance, a beat instance or a single clip). Per frame: one bone-pair read per listed joint and a SetMorphTarget per
// bound piece only when its weight moved; no allocation after the first frame.
//
// Not in shipping: -FTPoseCorrectiveWeight=<0..1> holds every weight there (proof stills at 0 and 1);
// -FTPoseCorrectiveLog logs each joint's flexion and weight once a second.

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FTPoseCorrectiveComponent.generated.h"

class USkeletalMeshComponent;

UCLASS(ClassGroup = (ForgottenThird))
class FORGOTTENTHIRD_API UFTPoseCorrectiveComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFTPoseCorrectiveComponent();

	// Binds the listed morphs that Piece's mesh has (dropping what was bound for it before, so a mesh swap re-binds).
	// Creates the owner's component for this body on first need. Returns the number of morphs bound on Piece.
	static int32 Bind(USkeletalMeshComponent& Body, USkeletalMeshComponent& Piece, const TCHAR* Who);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	struct FJoint
	{
		int32 Entry = INDEX_NONE;  // index in the table
		int32 Bone = INDEX_NONE;   // on the body
		int32 Parent = INDEX_NONE; // the bone's parent on the body
		FQuat RefParent = FQuat::Identity;    // the parent's reference rotation in component space
		FVector RefChildLocal = FVector::ZeroVector; // the child's reference offset in the bone's space
		FVector RefDir = FVector::ZeroVector; // the bone-to-child direction in the reference pose, component space
		float Weight = 0.f;
	};
	struct FBound
	{
		TWeakObjectPtr<USkeletalMeshComponent> Piece;
		FName Morph;
		int32 Joint = INDEX_NONE; // in Joints
		float Sent = -1.f;        // last weight set on the piece
	};

	int32 BindPiece(USkeletalMeshComponent& Piece, const TCHAR* Who);
	int32 JointFor(int32 Entry);

	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> Body;

	TArray<FJoint> Joints;
	TArray<FBound> Bound;
	float ForcedWeight = -1.f;
	bool bLog = false;
	float LogClock = 0.f;
};
