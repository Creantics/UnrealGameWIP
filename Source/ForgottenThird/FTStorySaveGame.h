#pragma once

// The story save slot: Core's JSON text for a StorySave (ftcore::encodeStorySave) in one string.
// Save with UGameplayStatics::SaveGameToSlot as usual; the text format, not the UPROPERTY layout,
// is what survives changes to the story data.

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"

#include "FTStorySaveGame.generated.h"

class UFTStorySubsystem;

UCLASS(BlueprintType)
class FORGOTTENTHIRD_API UFTStorySaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	// JSON from ftcore::encodeStorySave (empty: nothing saved yet).
	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Story")
	FString SaveJson;

	// Stores the subsystem's current story progress. False (SaveJson untouched) when the story
	// isn't on a scene.
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool CaptureFrom(const UFTStorySubsystem* Story);

	// Loads SaveJson into the subsystem. False when the text is empty or invalid (OutError says
	// why; the story is untouched) or the saved scene no longer exists (the story is then stopped).
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool ApplyTo(UFTStorySubsystem* Story, FString& OutError, TArray<FString>& OutSkippedFlags) const;
};
