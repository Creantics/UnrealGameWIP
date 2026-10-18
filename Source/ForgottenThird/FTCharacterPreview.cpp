#include "FTCharacterPreview.h"

#include "Components/SkeletalMeshComponent.h"
#include "FTFaceAnimInstance.h"
#include "FTFaceComponent.h"
#include "ForgottenThird.h"

AFTCharacterPreview::AFTCharacterPreview()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Root);
	// Moves the jaw and tongue bones from the face component; no Anim Blueprint needed.
	Body->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	Body->SetAnimInstanceClass(UFTFaceAnimInstance::StaticClass());

	Face = CreateDefaultSubobject<UFTFaceComponent>(TEXT("Face"));
	Face->FaceMeshComponentName = TEXT("Body");

	CycleExpressions = {
		TEXT("neutral"), TEXT("warm"), TEXT("amused"), TEXT("wry"), TEXT("proud"),
		TEXT("hopeful"), TEXT("sad"), TEXT("grief"), TEXT("afraid"), TEXT("anxious"),
		TEXT("angry"), TEXT("contempt"), TEXT("stunned"), TEXT("cold"),
	};
}

void AFTCharacterPreview::BeginPlay()
{
	Super::BeginPlay();

	if (!Body->GetSkeletalMeshAsset())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: no mesh on Body; nothing to preview."), *GetName());
	}
	Current = 0;
	Held = 0.f;
	if (bAutoCycle && CycleExpressions.Num() > 0)
	{
		ShowExpression(0);
	}
}

void AFTCharacterPreview::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bAutoCycle || CycleExpressions.Num() == 0)
	{
		return;
	}
	Held += DeltaSeconds;
	if (Held >= CycleSeconds)
	{
		Held = 0.f;
		ShowExpression((Current + 1) % CycleExpressions.Num());
	}
}

void AFTCharacterPreview::ShowExpression(int32 Index)
{
	Current = Index;
	if (!Face->SetExpression(CycleExpressions[Index], Intensity, 0.4f))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: unknown expression '%s'."), *GetName(),
			*CycleExpressions[Index].ToString());
	}
}
