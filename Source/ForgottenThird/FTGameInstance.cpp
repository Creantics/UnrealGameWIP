#include "FTGameInstance.h"

#include "ForgottenThird.h"
#include "FTStorySubsystem.h"

void UFTGameInstance::Init()
{
	// Creates the subsystems.
	Super::Init();
	ReloadStory();
}

bool UFTGameInstance::ReloadStory()
{
	UFTStorySubsystem* Story = GetSubsystem<UFTStorySubsystem>();
	if (!Story)
	{
		return false;
	}

	FString Error;
	TArray<FString> Warnings;
	const bool bOk = Story->LoadStory(StoryDir, Error, Warnings);
	for (const FString& Warning : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: %s"), *Warning);
	}
	if (bOk)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Story loaded from %s"), *StoryDir);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story not loaded from %s: %s"), *StoryDir, *Error);
	}
	return bOk;
}
