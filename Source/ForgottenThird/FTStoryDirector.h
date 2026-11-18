#pragma once

// Plays the story on screen: holds the ftcore::DialoguePresenter over the story subsystem's runner,
// takes the player's story keys and caches what AFTStoryHUD draws. AFTGameMode spawns one when it
// starts the story. The presenter decides what a press means; the director applies its commands
// through UFTStorySubsystem, so the subsystem's events fire as usual.
//
// Keys (AFTPlayerController's story keys; its own story routing is turned off while a director
// runs): Space / left mouse / pad bottom or Enter finish the text, then move on or continue past a
// gameplay beat; 1-4 / pad d-pad pick an option once the prompt is out.
//
// Gameplay hand-off: continuing past a gameplay beat lowers it with ftcore::GameplayPlan and plays its
// steps in order. Each step fills UFTGameInstance::StoryCarry and opens the step's map, both picked by
// FTStep::Prepare (FTStepHost.h): BattleDemo for a battle (CMB; or its world's combat system's screen),
// Explore for the field (ENG, PLAY; a scene placed in one of the big worlds by Data/Field/worlds.json
// opens that world's map instead, when it has been built), Choice (CHC, HE), Qte, Cutscene (CG),
// Banter (BNT), Mini (MINI) and Stub for the stub screen (unknown tokens). A screen sets carry.bDone,
// then the beat's next step may start in place on its map (FTStep::ContinueHere); otherwise it opens
// carry.ReturnMap and the director spawned there opens the next step, or resumes the story after the
// last one. A screen that comes back not done is logged and the beat waits in the box. Paced autoplay
// hands off too (the screens play themselves); fast autoplay skips the hand-off unless
// -StoryAutoPlayHandoff.
//
// Moves, scenes and wall-clock time in the FINISHED line count the whole story run (UFTStorySubsystem:
// from StartStory or a loaded save), across the hand-off map changes.
//
// Command line: -StoryAutoPlay plays by itself (each text typed out, then a short pause);
// -StoryAutoPlayFast shows each text whole and makes many moves a frame; -StoryQuitOnFinish exits
// when the story ends (a headless run); -StoryScene=<id> starts there (read by AFTGameMode);
// -StoryAutoPlayHandoff turns on autoplay that hands off even when fast (the screens play themselves);
// -StoryQuitOnReturn exits once the story resumes after a whole beat (all its steps).
//
// Reading aids (interactive play only; autoplay never binds them, so headless runs are unchanged):
// B or mouse wheel up opens the backlog (ftcore::DialogueBacklog on the story subsystem), Escape, pad
// Start or B closes it; up / down and the wheel scroll it. Escape and pad Start otherwise open the
// pause menu (UFTPauseSubsystem; its Settings row opens the settings overlay). While the backlog, the
// pause menu or the settings is open the story stands still (UFTStorySubsystem::SetPaused, no typing)
// and presses are ignored. Holding Ctrl, S or the pad's right trigger skips (ftcore::
// SkipController): each line shows at once and moves on every SkipInterval seconds, stopping at
// choices and gameplay beats, and, with the "seen lines only" setting, at lines not seen before.
// Auto-advance (a setting) moves on a fully shown line after a delay. Text speed, skip mode and
// auto-advance come from the settings; the director polls their revision.
//
// Voice: a voiced line (UFTAudioSubsystem::OnVoiceLine) plays its lip-sync track (Data/Voice/<LineId>.nface)
// on every UFTFaceComponent in this map whose ModelId is the speaker's model in the local cast file,
// Data/Voice/speakers.json (ftcore::SpeakerCast; gitignored, since the keys are script labels), or, when
// the map has none, on its stand-in faces (UFTFaceComponent::bStandIn; ftcore::pickFaceRoute). The track
// stops when the voice does. The run's tally goes to the log as "Story: FACES ..." when a map ends. Auto-advance and paced autoplay wait for the voice to finish before their
// pause starts (auto-advance then waits only its short voice tail); a press still moves on at once and
// cuts the voice. Fast autoplay and skip never wait. With no cast file, no faces or no voice, nothing
// changes.
//
// Alpha mode: -AlphaAutoPlay is fast text autoplay that hands off on every step (carry.bAutoPlay set),
// counting scenes, screens, STUCK and warnings in the game instance across maps. When the story ends
// it logs "Alpha: FINISHED ..." and, with -StoryQuitOnFinish, exits with code 0 only if there was no
// STUCK and no warning (1 otherwise). -AlphaFast is read by the screens (UFTGameInstance::IsAlphaFast).
// Logs carry scene ids and counts only, never script text.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ftcore/AssetManifest.h"
#include "ftcore/Backlog.h"
#include "ftcore/Dialogue.h"
#include "ftcore/Skip.h"
#include "ftcore/StoryCast.h"

#include "FTStoryDirector.generated.h"

class AFTCastMember;
class AFTPlayerController;
class UFTAudioSubsystem;
class UFTFaceComponent;
class UFTSettingsSubsystem;
class UFTStorySubsystem;

UCLASS()
class FORGOTTENTHIRD_API AFTStoryDirector : public AActor
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxOptions = 4;

	AFTStoryDirector();

	// Characters per second the text types out at (0: whole at once). Autoplay only: interactive play
	// uses the text speed setting.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	float RevealRate = 45.f;
	// Seconds between lines while skip is held.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	float SkipInterval = 0.08f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	bool bAutoPlay = false;
	// Seconds autoplay waits once a text is out.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	float AutoPlayPause = 0.8f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Story")
	bool bQuitOnFinish = false;
	// The pause before a gameplay step's map opens (the maps: FTStep::Prepare).
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

	// ---- reading aids, for the HUD ----
	bool IsBacklogOpen() const { return bBacklogOpen; }
	// How many entries back from the newest the backlog is scrolled (0: at the newest).
	int32 GetBacklogScroll() const { return BacklogScroll; }
	// Null before the story is loaded.
	const ftcore::DialogueBacklog* GetBacklog() const;
	const ftcore::StoryDatabase* GetStoryDatabase() const;
	bool IsSkipping() const { return Skip.skipping(); }
	bool IsAutoAdvanceOn() const { return !bAutoPlay && AutoAdvance.enabled(); }
	// Dialogue text size multiplier from the settings (1 in autoplay).
	float GetSubtitleScale() const { return SubtitleScale; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UFUNCTION()
	void HandlePress();
	UFUNCTION()
	void HandleChoose(int32 Option);
	UFUNCTION()
	void HandleBacklogKey();
	UFUNCTION()
	void HandleWheel(int32 Direction);
	UFUNCTION()
	void HandleSkip(bool bHeld);
	UFUNCTION()
	void HandleCancel();
	// Esc / pad Start (AFTPlayerController::OnBack): closes the backlog and claims the press.
	UFUNCTION()
	void HandleBack();
	UFUNCTION()
	void HandleMenuStep(int32 Step);
	UFUNCTION()
	void HandleVoiceLine(const FString& LineId, float Seconds);
	UFUNCTION()
	void HandleVoiceStopped();

	// Reads Data/Voice/speakers.json into Speakers (checked against Data/Assets/models.json when it loads) and the
	// local Data/Story/cast_marks.json into CastMarks.
	void LoadCast();
	// At a scene's start, in a map with a stand-in face: puts the scene's cast speakers on screen, each a lead look
	// (the explore character's PartyLooks) whose ModelId is the speaker's model, on its cast mark (else the default
	// arc) around the stand-in. One actor per model, made once and reused. The stand-in hides when every cast
	// speaker of the scene has its own; otherwise it stays on the anchor and the cast stands around it. A checkout
	// without looks for the cast puts no one on screen and changes nothing.
	void PlaceSceneCast();
	// Destroys the cast pool and shows the stand-in again (the director ends, the story finishes, a step takes
	// over the screen): no cast is left standing and a later director starts from none.
	void ClearSceneCast(const TCHAR* Why);
	// The frame-cost buckets below, by what the cast is doing this frame.
	void NoteCastFrame();
	// The path of the next -StoryShot still of LineId; empty when the name is unsafe or 99 are saved.
	FString StoryShotFile(const FString& LineId) const;
	// The current line's voice is still playing.
	bool IsVoiceBusy() const;

	// A reading overlay (backlog or settings) or the pause menu is up, or just closed this frame.
	bool InputBlocked() const;
	// Interactive play, not handing off, and no overlay up.
	bool CanOpenOverlay() const;
	void OpenBacklog();
	void CloseBacklog();
	void ScrollBacklog(int32 Delta);
	// Takes the text speed, skip mode and auto-advance from the settings when their revision moved.
	void ApplySettings();
	// One interactive frame: skip, then auto-advance.
	void TickReading(float DeltaSeconds);
	bool IsSkipKeyDown() const;
	void NoteShown();

	// Applies a presenter command through the subsystem. False when there was none or it was refused.
	bool Apply(const ftcore::DialogueCommand& Command);
	// After the runner may have moved: picks up a new beat and scene and refreshes the cache.
	void Refresh();
	void CacheBeat();
	void TickAutoPlay(float DeltaSeconds);
	void ReportFinished();
	// Back from a gameplay screen: counts its step (FTStep::FinishCarried), then opens the beat's next
	// step, or resumes the story if it is still on the handed-off beat.
	void ResumeFromCarry();
	// Lowers the runner's gameplay beat and starts its first step. False when the beat has no plan
	// (it is resumed as a plain skip) or a screen already gave up on it in an alpha run.
	bool BeginChain();
	// Banner, log and the timer that opens the current step's map.
	void StartStep();
	void OpenStep();
	void Quit(int32 ExitCode);

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

	bool bAlpha = false; // -AlphaAutoPlay
	bool bHandoffAuto = false; // -StoryAutoPlayHandoff
	bool bQuitOnReturn = false;
	// -StoryShot=<name>: autoplay (not fast) saves the screen with the HUD for each of the run's first
	// -StoryShotMax=N lines (default 4, 0..64; 0 quits at once), once the line is revealed and 1 s in,
	// then quits. The stills go to the screenshot folder as <name>_<line id>.png, never over an
	// existing one (ftcore::storyShotNameOk, clampStoryShotMax).
	FString ShotPrefix;
	int32 ShotsLeft = 0;
	bool bShotNoHud = false; // -StoryShotNoHud: the stills leave the HUD out (clean plates for UI mock-ups)
	float ShotQuitIn = -1.f;
	bool bShotThisBeat = false;
	// -StoryShotSpeaking=<model id> (non-shipping, with -StoryShot): instead of one still per line, three stills
	// across the first fully voiced line that model speaks on its own face: mouth closed, open, closed again.
	FString ShotSpeakingModel;
	int32 ShotSpeakingPhase = 0;
	float ShotSpeakingAt = 0.f;
	int32 ShotSpeakingBeat = -1;
	void TickSpeakingShots();
	float BeatAge = 0.f;          // seconds since the beat began (autoplay)
	FString VoiceLineId;          // the line whose voice last started, and its length
	float VoiceSeconds = 0.f;
	bool bPrevRouteInput = false;
	bool bHandingOff = false;
	FString Banner;
	double BannerStart = -1.0;

	// Reading aids (interactive play).
	ftcore::SkipController Skip;
	ftcore::AutoAdvancer AutoAdvance;
	TWeakObjectPtr<UFTSettingsSubsystem> SettingsSub;
	uint32 AppliedSettings = MAX_uint32;
	float SubtitleScale = 1.f;
	int32 BacklogScroll = 0;
	bool bBacklogOpen = false;
	bool bBacklogBeganMenu = false; // OpenBacklog turned the controller's menu input on
	bool bSkipHeld = false;
	bool bStoryPaused = false;

	// Voice and faces.
	TWeakObjectPtr<UFTAudioSubsystem> Audio;
	ftcore::SpeakerCast Speakers;
	TArray<TWeakObjectPtr<UFTFaceComponent>> SpeakingFaces;
	bool bLoggedNoFace = false;
	ftcore::CastMarks CastMarks;
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<AFTCastMember>> CastActors; // by model id; null: the look didn't load
	TWeakObjectPtr<AActor> CastAnchor;
	// Non-shipping diagnostics: -FaceDiag logs each cast face's peaks when the cast is cleared; -CastCost times
	// frames by cast state (below) and logs them at EndPlay. Both off by default and compiled out of shipping.
	bool bCastDiag = false;
	bool bCastCost = false;
	// The face curve mapping asset, loaded with the map (not at a face's first bind mid-scene).
	UPROPERTY(Transient)
	TObjectPtr<UObject> CurveMapPoseHold;
	// Frame cost (wall ms per frame) by cast state: none in the pool, pool all hidden, some on screen idle, one
	// speaking. Logged at EndPlay.
	double LastFrameAt = -1.0;
	double CastFrameMs[4] = {};
	int32 CastFrames[4] = {};
};
