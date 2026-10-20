#pragma once

// Game mode of the stub screen: the placeholder a gameplay step with no real screen yet opens
// (CHC, BNT, CG, HE, QTE and unknown tokens in ftcore::GameplayPlan; GameplayScreen::Stub). It
// shows the step and continues on a press, then returns to the story through
// UFTGameInstance::StoryCarry. No story on play. Skeleton: the stub-screen stream fills it in.

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "FTStubGameMode.generated.h"

UCLASS()
class FORGOTTENTHIRD_API AFTStubGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	AFTStubGameMode();

	virtual void StartPlay() override;
};
