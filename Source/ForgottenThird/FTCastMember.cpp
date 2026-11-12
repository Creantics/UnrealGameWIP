#include "FTCastMember.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

#include "ForgottenThird.h"
#include "FTFaceComponent.h"
#include "FTFaceCurveAnimInstance.h"
#include "FTLook.h"

AFTCastMember::AFTCastMember()
{
	PrimaryActorTick.bCanEverTick = false;
	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RootComponent = Body;
	Face = CreateDefaultSubobject<UFTFaceComponent>(TEXT("Face"));
	Face->bBindOnBeginPlay = false; // Wear binds it once the look is on
}

void AFTCastMember::SetOnScreen(bool bOn)
{
	bOnScreen = bOn;
	SetActorHiddenInGame(!bOn);
	Body->SetComponentTickEnabled(bOn);
	for (USkeletalMeshComponent* Piece : Pieces)
	{
		if (Piece)
		{
			Piece->SetComponentTickEnabled(bOn);
		}
	}
	for (USceneComponent* Groom : Grooms)
	{
		if (Groom)
		{
			Groom->SetComponentTickEnabled(bOn);
		}
	}
	if (!bOn)
	{
		Face->StopTrack();
	}
	Face->SetComponentTickEnabled(bOn);
}

bool AFTCastMember::Wear(const FFTLeadLook& Look, const FString& ModelId)
{
	const FString Who = FString::Printf(TEXT("Cast %s"), *ModelId);
	const FFTLoadedLook Loaded = FTLook::LoadLook(Look, *Who);
	if (!Loaded.Body)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: the look's body doesn't load; not on screen."), *Who);
		return false;
	}
	Body->SetSkeletalMesh(Loaded.Body);
	Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	if (UAnimSequence* Idle = Loaded.Clips[static_cast<int32>(ftcore::LocoClip::Idle)])
	{
		Body->PlayAnimation(Idle, true);
	}
	FTLook::AddPieces(*this, *Body, Look.PiecePaths, Pieces, *Who);
	RigFace = nullptr;
	for (USkeletalMeshComponent* Piece : Pieces)
	{
		if (Piece && Piece->GetPostProcessAnimBPClassToBeUsed())
		{
			RigFace = Piece;
			break;
		}
	}
	if (RigFace)
	{
		// A follower runs neither its own anim instance nor its rig: the face copies the body's pose itself.
		RigFace->SetLeaderPoseComponent(nullptr);
		RigFace->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		RigFace->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		RigFace->SetAnimInstanceClass(UFTFaceCurveAnimInstance::StaticClass());
		Face->FaceMeshComponentName = RigFace->GetFName();
	}
	else
	{
		Face->FaceMeshComponentName = Body->GetFName();
	}
	FTLook::AddGrooms(*this, *Body, Pieces, Look.Grooms, Grooms, *Who);
	Face->ModelId = ModelId;
	Face->Rebind();
	return true;
}
