#pragma once

// Canvas UI of the stub screen (no UMG): the step's token and label and the beat's notation from
// UFTGameInstance::StoryCarry, and a "continue" prompt. Per-frame drawing uses cached or static
// strings only. Skeleton: the stub-screen stream fills it in.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTStubHUD.generated.h"

UCLASS()
class FORGOTTENTHIRD_API AFTStubHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
