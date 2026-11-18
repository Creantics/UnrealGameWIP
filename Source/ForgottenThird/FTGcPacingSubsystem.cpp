#include "FTGcPacingSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/UObjectGlobals.h"

#include "ForgottenThird.h"

void UFTGcPacingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Pacing.enabled = FParse::Param(FCommandLine::Get(), TEXT("GcHidden"));
	Pacing.collected(FPlatformTime::Seconds()); // the start's load has just collected
	PostGcHandle = FCoreUObjectDelegates::GetPostGarbageCollect().AddUObject(this, &UFTGcPacingSubsystem::OnPostGarbageCollect);
}

void UFTGcPacingSubsystem::Deinitialize()
{
	FCoreUObjectDelegates::GetPostGarbageCollect().Remove(PostGcHandle);
	Super::Deinitialize();
}

void UFTGcPacingSubsystem::OnPostGarbageCollect()
{
	Pacing.collected(FPlatformTime::Seconds());
}

void UFTGcPacingSubsystem::HiddenMoment(const UObject* WorldContext, const TCHAR* Why)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UFTGcPacingSubsystem* Self = GameInstance ? GameInstance->GetSubsystem<UFTGcPacingSubsystem>() : nullptr;
	if (!Self || !Self->Pacing.enabled || !GEngine)
	{
		return;
	}
	const double Since = FPlatformTime::Seconds() - Self->Pacing.last();
	if (Self->Pacing.hiddenMoment(FPlatformTime::Seconds()))
	{
		GEngine->ForceGarbageCollection(true);
		UE_LOG(LogForgottenThird, Display, TEXT("GC: hidden moment (%s), %.0f s after the last collection: a full collection asked."), Why, Since);
	}
}
