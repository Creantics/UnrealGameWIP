#pragma once

// Loads the story (UFTStorySubsystem::LoadStory) from StoryDir when the game instance starts. A
// missing or broken story is logged and the game runs on without one. Also loads the big worlds'
// table (Data/Field/worlds.json), which says which world map and district a scene's field steps use.

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"

#include "ftcore/Dialogue.h"
#include "ftcore/Explore.h"
#include "ftcore/Guard.h"
#include "ftcore/StoryChain.h"
#include "ftcore/WorldLayout.h"

#include "FTGameInstance.generated.h"

// What the exploration demo hands to its fight and gets back: set by AFTExploreDirector before it
// opens the battle map, read by AFTBattleDemoActor (where to return, whether it won) and by the
// director when the field opens again.
struct FFTExploreCarry
{
	bool bValid = false;
	FString ReturnMap;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	ftcore::ExploreState State;
	// The fight: an encounter spot's id, or a field guard's id (the other is -1).
	int32 Encounter = -1;
	int32 Guard = -1;
	ftcore::GuardSave Guards; // which guards were defeated before this fight
	bool bWon = false;
	double Seconds = 0.0; // time already spent in the field, for the completion log
};

// What the story hands to a gameplay screen and gets back: set by AFTStoryDirector before it opens
// the battle, field or stub map for each step of a gameplay beat. The screen sets bDone (and bWon)
// and opens ReturnMap; the director there opens the beat's next step, or resumes the story after the
// last one if the runner is still on the same beat. A screen that gives up leaves bDone false.
struct FFTStoryCarry
{
	bool bValid = false;
	FString ReturnMap;
	FString Scene; // scene id and beat index of the gameplay beat
	int32 Beat = -1;
	FString Kind; // gameplay kind as written (CMB, ENG, PLAY); the beat's, not the step's
	ftcore::GameplayScreen Screen = ftcore::GameplayScreen::Box;
	// The step of the beat's ftcore::GameplayPlan this screen plays (0-based) and the plan's step
	// count. The story resumes only after the last step; a single-screen beat is step 0 of 1.
	int32 Step = 0;
	int32 StepCount = 1;
	FString StepLabel; // the step's qualifiers ("duel", "loop, intercut"); empty when none
	// The step's token as notation ("CMB", "CHC"); the stub screen shows it.
	FString StepToken;
	// A field step in one of the big worlds (Data/Field/worlds.json): the world's id, the district
	// whose plaza holds the goal, and the style for the scene. Empty World: the Explore map.
	FString World;
	int32 District = -1;
	FString Style;
	// Set when the last field step was in another world: the step opens the overworld first, the
	// player walks from this world's gate to World's, and entering it opens World's map.
	FString FromWorld;
	bool bAutoPlay = false; // the screen plays itself (a headless hand-off run)
	bool bDone = false;
	bool bWon = false;
};

UCLASS(Config = Game)
class FORGOTTENTHIRD_API UFTGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	// story.json and scenes/<id>.json. Relative paths are relative to the project folder.
	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Story")
	FString StoryDir = TEXT("Data/Story");

	virtual void Init() override;

	// -AlphaFast: the alpha run's gameplay screens apply time dilation to their autopilot.
	bool IsAlphaFast() const { return bAlphaFast; }

	// Loads StoryDir again (after editing the story files).
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool ReloadStory();

	// Data/Field/worlds.json, loaded at Init (empty when missing or refused, so every field step
	// plays on the Explore map).
	const ftcore::WorldTable& GetWorlds() const { return Worlds; }

	FFTExploreCarry ExploreCarry;
	FFTStoryCarry StoryCarry;

	// The lowered plan of the gameplay beat in progress and the step it is on; the director on the
	// story map reads it when a screen comes back.
	ftcore::StoryChain Chain;
	// The tallies of an -AlphaAutoPlay run, kept across map changes.
	ftcore::AlphaCounters Alpha;
	// The beat a screen gave up on (alpha runs skip it instead of retrying it forever).
	FString FailedScene;
	int32 FailedBeat = -1;
	// The world of the last field step the story opened (empty before the first): a field step in
	// another world walks the overworld from this one.
	FString LastWorld;
	// The explore pawn's lead (a PartyLooks name) and party toggle as the player last set them, kept
	// across map changes (a fight and back). Empty and unset until then, so the pawn's config picks.
	FString ExploreLead;
	bool bExplorePartySet = false;
	bool bExplorePartyHidden = false;

private:
	void LoadWorlds();

	bool bAlphaFast = false;
	ftcore::WorldTable Worlds;
};
