#pragma once

// The story save slot: Core's JSON text for a StorySave (ftcore::encodeStorySave) in one string.
// Save with UGameplayStatics::SaveGameToSlot as usual; the text format, not the UPROPERTY layout,
// is what survives changes to the story data. The text (format 2) also carries which beats the
// player has seen, for "skip only seen lines", and the party's xp and gold.

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

	// The story state's block (UFTStoryStateSubsystem, ftcore::storyEncode; the block carries its own layout
	// version and checksum). Empty: a save from before it, which loads as a new game's state.
	UPROPERTY(SaveGame)
	TArray<uint8> StoryBlock;

	// Stores the subsystem's current story progress, and the story state's block when its subsystem is
	// there. False (SaveJson and StoryBlock untouched) when the story isn't on a scene.
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool CaptureFrom(const UFTStorySubsystem* Story);

	// Loads SaveJson into the subsystem, then StoryBlock into the story state (a refused or empty block: a
	// new game's state, with a warning for a refused one; it never fails the load). False when the text is
	// empty or invalid (OutError says why; the story and its state are untouched) or the saved scene no
	// longer exists (the story is then stopped, the state untouched).
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool ApplyTo(UFTStorySubsystem* Story, FString& OutError, TArray<FString>& OutSkippedFlags) const;
};
