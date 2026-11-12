#pragma once

// A cast speaker on screen in a dialogue scene: a lead look (FFTLeadLook: body, pieces, grooms) standing in its
// idle, with a UFTFaceComponent of the look's model. The face is the piece whose mesh has a post-process anim
// blueprint (a MetaHuman face's rig logic): it leaves the body's leader pose, copies the body's pose itself and
// takes the face's control curves (UFTFaceCurveAnimInstance). A look without such a piece keeps a face component
// that binds morph targets on the body as before. AFTStoryDirector spawns one per cast model and reuses it.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "FTCastMember.generated.h"

class USceneComponent;
class USkeletalMeshComponent;
class UFTFaceComponent;
struct FFTLeadLook;

UCLASS(NotPlaceable, Transient)
class FORGOTTENTHIRD_API AFTCastMember : public AActor
{
	GENERATED_BODY()

public:
	AFTCastMember();

	// Puts on the look as model ModelId. False (and logged) when its body doesn't load.
	bool Wear(const FFTLeadLook& Look, const FString& ModelId);

	UFTFaceComponent* GetFace() const { return Face; }
	// Whether the face is driven through its rig's curves.
	bool HasRigFace() const { return RigFace != nullptr; }

private:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> Body;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFTFaceComponent> Face;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> Pieces;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USceneComponent>> Grooms;
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> RigFace;
};
