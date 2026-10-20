#pragma once

// Game mode of the cutscene screen (GameplayScreen::Cutscene: a CG step of a story gameplay beat).
// It looks up the step's shot list in Data/Cutscene/cutscenes.json by the carry's scene, beat and step
// (the "default" list when the step isn't listed), plays it through the map's AFTCutsceneDirector
// and shows AFTCutsceneHUD (letterbox, the step's title, a skip hint). When the last shot ends, or
// on Space / Enter / pad A (after a short grace), it marks UFTGameInstance::StoryCarry done and opens
// ReturnMap, as AFTStubGameMode does. With StoryCarry.bAutoPlay it skips itself after one second of
// real time. Logs ids and counts only, never text:
//   Cutscene: done <scene> beat <b> step <s> shots <n> skipped <0|1>
// Opened without a valid carry it plays the default list as a preview and stays on the map.

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/Cutscene.h"

#include "FTCutsceneGameMode.generated.h"

class AFTCutsceneDirector;

UCLASS()
class FORGOTTENTHIRD_API AFTCutsceneGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	AFTCutsceneGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// True when the map was opened with a story step to play.
	bool HasStep() const { return bHasStep; }
	const AFTCutsceneDirector* GetDirector() const { return Director; }
	// Text for the HUD, built once in StartPlay.
	const FString& GetTitleText() const { return TitleText; }
	const FString& GetStepText() const { return StepText; }
	// "Shot i of n" for the director's current shot (empty when there is none).
	const FString& GetShotText() const;

private:
	// The step's shot list from Data/Cutscene/cutscenes.json, or a one-shot list when the file can't
	// be read (logged).
	void LoadPlan(const FString& Scene, int32 Beat, int32 Step, ftcore::CutscenePlan& Out) const;
	void Finish();
	void OpenReturnMap();

	UPROPERTY()
	TObjectPtr<AFTCutsceneDirector> Director;

	bool bHasStep = false;
	bool bReturning = false;
	FString TitleText;
	FString StepText;
	TArray<FString> ShotTexts;
};
