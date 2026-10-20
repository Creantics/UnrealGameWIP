#pragma once

// Plays the story on screen: holds the ftcore::DialoguePresenter over the story subsystem's runner,
// takes the player's story keys and caches what AFTStoryHUD draws. AFTGameMode spawns one when it
// starts the story. The presenter decides what a press means; the director applies its commands
// through UFTStorySubsystem, so the subsystem's events fire as usual.
//
// Keys (AFTPlayerController's story keys; its own story routing is turned off while a director
// runs): Space / left mouse / pad bottom or Enter / pad Start finish the text, then move on or
// continue past a gameplay beat; 1-4 / pad d-pad pick an option once the prompt is out.
//
// Gameplay hand-off: continuing past a fight (CMB) opens BattleMap and past an exploration beat (ENG,
// PLAY) opens FieldMap, with an FFTStoryCarry in the game instance. The screen opens this map again
// when it is done; the director spawned there resumes the story after the beat. Other kinds wait in
// the box and continuing skips them. Fast autoplay skips the hand-off unless -StoryAutoPlayHandoff.
//
// Moves, scenes and time in the FINISHED line count from this map visit only.
//
// Command line: -StoryAutoPlay plays by itself (each text typed out, then a short pause);
// -StoryAutoPlayFast shows each text whole and makes many moves a frame; -StoryQuitOnFinish exits
// when the story ends (a headless run); -StoryScene=<id> starts there (read by AFTGameMode);
// -StoryAutoPlayHandoff makes autoplay hand off too (and the screens play themselves);
// -StoryQuitOnReturn exits once the story resumes after a hand-off. Logs carry scene ids and counts
// only, never script text.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ftcore/Dialogue.h"

#include "FTStoryDirector.generated.h"

class AFTPlayerController;
class UFTStorySubsystem;

UCLASS()
class FORGOTTENTHIRD_API AFTStoryDirector : public AActor
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxOptions = 4;

	AFTStoryDirector();

	// Characters per second the text types out at (0: whole at once).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	float RevealRate = 45.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	bool bAutoPlay = false;
	// Seconds autoplay waits once a text is out.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	float AutoPlayPause = 0.8f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	bool bQuitOnFinish = false;
	// Maps the gameplay beats open, and the pause before they do.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	FString BattleMap = TEXT("BattleDemo");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	FString FieldMap = TEXT("Explore");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	float HandoffDelay = 1.2f;

	virtual void Tick(float DeltaSeconds) override;

	// ---- for the HUD (no allocation) ----
	const ftcore::DialoguePresenter& GetPresenter() const { return Presenter; }
	// Bumped whenever the cached strings below change (the HUD re-wraps then).
	uint32 GetBeatSerial() const { return BeatSerial; }
	const FString& GetText() const { return Text; }
	const FString& GetSpeaker() const { return Speaker; }
	// Option lines as shown ("1  ..."); Num() is the option count, at most MaxOptions.
	const TArray<FString>& GetOptions() const { return Options; }
	bool IsOptionSelectable(int32 Option) const { return Options.IsValidIndex(Option) && Selectable[Option]; }
	// Options past MaxOptions can't be picked with the keys (the script has none).
	int32 GetHiddenOptions() const { return HiddenOptions; }
	const FString& GetSceneTitle() const { return SceneTitle; }
	double GetSceneAge() const;
	bool IsFinished() const;
	// A short banner while a gameplay screen opens ("Battle!"); empty otherwise.
	const FString& GetBanner() const { return Banner; }
	double GetBannerAge() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UFUNCTION()
	void HandlePress();
	UFUNCTION()
	void HandleChoose(int32 Option);

	// Applies a presenter command through the subsystem. False when there was none or it was refused.
	bool Apply(const ftcore::DialogueCommand& Command);
	// After the runner may have moved: picks up a new beat and scene and refreshes the cache.
	void Refresh();
	void CacheBeat();
	void TickAutoPlay(float DeltaSeconds);
	void ReportFinished();
	// Back from a gameplay screen: resumes the story if it is still on the handed-off beat.
	void ResumeFromCarry();
	void BeginHandoff(ftcore::GameplayScreen Screen);
	void OpenHandoff();

	UPROPERTY()
	TObjectPtr<UFTStorySubsystem> Story;
	TWeakObjectPtr<AFTPlayerController> Controller;

	ftcore::DialoguePresenter Presenter;
	uint32 BeatSerial = 0;
	uint32 SceneEpoch = 0;
	FString Text;
	FString Speaker;
	TArray<FString> Options;
	bool Selectable[MaxOptions] = {};
	int32 HiddenOptions = 0;
	FString SceneTitle;
	double SceneStart = -1.0;

	bool bFast = false;
	bool bReported = false;
	float WaitTime = 0.f; // autoplay: seconds with nothing to do
	int32 Moves = 0;
	int32 Scenes = 0;
	double StartTime = 0.0;

	bool bHandoffAuto = false; // -StoryAutoPlayHandoff
	bool bQuitOnReturn = false;
	bool bPrevRouteInput = false;
	bool bHandingOff = false;
	ftcore::GameplayScreen HandoffScreen = ftcore::GameplayScreen::Box;
	FString Banner;
	double BannerStart = -1.0;
};
