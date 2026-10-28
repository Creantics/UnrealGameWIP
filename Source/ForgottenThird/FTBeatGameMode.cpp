#include "FTBeatGameMode.h"

#include "Camera/CameraActor.h"
#include "EngineUtils.h"
#include "FTAudioSubsystem.h"
#include "FTBeatHUD.h"
#include "FTConductorComponent.h"
#include "FTGameInstance.h"
#include "FTPauseSubsystem.h"
#include "FTSettingsSubsystem.h"
#include "FTStepHost.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include <string>
#include <vector>

namespace
{
// -AlphaFast, as the other screens read it.
bool BeatAlphaFast()
{
	return FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
}

// Global time dilation of an autoplay fight under -AlphaFast: the rhythm battle's.
constexpr float kBeatAlphaFastDilation = 8.f;

// A music clock that hasn't reported its first position after this many wall seconds gives way to the wall
// clock (a cue that never plays would otherwise hold the fight back for ever).
constexpr double kBeatClockWait = 2.0;

// The four directions in ftcore::Arrow order (Left, Down, Up, Right), each with the keys AFTPlayerController
// maps for the battle lanes and the quick-time event screen reads: arrow keys, WASD, the pad d-pad and the
// pad's face buttons (X, A, Y, B).
constexpr int32 kBeatDirections = 4;
constexpr int32 kBeatKeysPerDirection = 4;

const FKey& BeatLaneKey(int32 Direction, int32 Which)
{
	static const FKey Keys[kBeatDirections][kBeatKeysPerDirection] = {
		{EKeys::Left, EKeys::A, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_FaceButton_Left},
		{EKeys::Down, EKeys::S, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_FaceButton_Bottom},
		{EKeys::Up, EKeys::W, EKeys::Gamepad_DPad_Up, EKeys::Gamepad_FaceButton_Top},
		{EKeys::Right, EKeys::D, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_FaceButton_Right},
	};
	return Keys[Direction][Which];
}

// After a defeat: Enter, Space or the pad's A retries; Q, Backspace or the pad's B gives up.
bool BeatRetryPressed(const APlayerController& PC)
{
	return PC.WasInputKeyJustPressed(EKeys::Enter) || PC.WasInputKeyJustPressed(EKeys::SpaceBar)
		|| PC.WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Bottom);
}

bool BeatQuitPressed(const APlayerController& PC)
{
	return PC.WasInputKeyJustPressed(EKeys::Q) || PC.WasInputKeyJustPressed(EKeys::BackSpace)
		|| PC.WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Right);
}

// The seed of a story step's fight: FNV-1a over the scene id, the beat and the step, so a step always plays
// the same fight and a retry restarts that fight.
uint32 BeatSeedFor(const FString& Scene, int32 Beat, int32 Step)
{
	uint32 Hash = 2166136261u;
	const std::string Utf8 = FT::ToUtf8(Scene);
	for (const char C : Utf8)
	{
		Hash = (Hash ^ static_cast<uint32>(static_cast<uint8>(C))) * 16777619u;
	}
	for (int32 Shift = 0; Shift < 32; Shift += 8)
	{
		Hash = (Hash ^ ((static_cast<uint32>(Beat) >> Shift) & 0xFFu)) * 16777619u;
		Hash = (Hash ^ ((static_cast<uint32>(Step) >> Shift) & 0xFFu)) * 16777619u;
	}
	return Hash;
}

// The room: the camera stands at the near edge of the floor and the stand-ins make an arc around it.
// Centimetres, x forward, z up.
constexpr double kBeatViewerX = -300.0;
constexpr double kBeatViewerZ = 170.0;
constexpr double kBeatViewerPitch = -4.0;
constexpr double kBeatArcRadius = 560.0;
constexpr double kBeatArcStepDegrees = 24.0;
// A stand-in's body is 150 cm tall and stands on the floor, so its centre is at half that. Its anchor, where
// the HUD hangs the markers, is this far above the centre (clear of the head).
constexpr double kBeatBodyHeight = 150.0;
constexpr double kBeatAnchorLift = 150.0;

// A stand-in is the body and the actors attached to it (head, visor): they hide together.
void BeatHideStandIn(AActor* Body, bool bHidden)
{
	Body->SetActorHiddenInGame(bHidden);
	TArray<AActor*> Attached;
	Body->GetAttachedActors(Attached, true, true);
	for (AActor* Part : Attached)
	{
		if (Part)
		{
			Part->SetActorHiddenInGame(bHidden);
		}
	}
}
} // namespace

AFTBeatGameMode::AFTBeatGameMode()
{
	HUDClass = AFTBeatHUD::StaticClass();
	bHostsStory = false;
	PrimaryActorTick.bCanEverTick = true;

	// The fight's clock: song time with music, else the pause-aware wall clock (nothing bound).
	Conductor = CreateDefaultSubobject<UFTConductorComponent>(TEXT("Conductor"));
	Conductor->bWallClockWithoutAudio = true;
}

FName AFTBeatGameMode::StandInTag(int32 Index)
{
	return FName(*FString::Printf(TEXT("BeatEnemy%d"), Index));
}

void AFTBeatGameMode::StandInPlace(int32 Index, int32 Count, FVector& OutLocation, FRotator& OutRotation)
{
	// Spread evenly either side of straight ahead, each facing the viewer.
	const double Degrees = (static_cast<double>(Index) - 0.5 * static_cast<double>(Count - 1)) * kBeatArcStepDegrees;
	const double Radians = FMath::DegreesToRadians(Degrees);
	OutLocation = FVector(kBeatViewerX + kBeatArcRadius * FMath::Cos(Radians), kBeatArcRadius * FMath::Sin(Radians), kBeatBodyHeight * 0.5);
	OutRotation = FRotator(0.0, Degrees + 180.0, 0.0);
}

FVector AFTBeatGameMode::ViewerLocation()
{
	return FVector(kBeatViewerX, 0.0, kBeatViewerZ);
}

FRotator AFTBeatGameMode::ViewerRotation()
{
	return FRotator(kBeatViewerPitch, 0.0, 0.0);
}

bool AFTBeatGameMode::GetEnemyAnchor(int32 Index, FVector& OutLocation) const
{
	if (Index < 0 || Index >= MaxStandIns)
	{
		return false;
	}
	const AActor* Actor = StandIns[Index].Get();
	if (!Actor)
	{
		return false;
	}
	OutLocation = Actor->GetActorLocation() + FVector(0.0, 0.0, kBeatAnchorLift);
	return true;
}

void AFTBeatGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the stub, choice, battle, quick-time and exploration modes
	// do: the story must not start here.
	AGameModeBase::StartPlay();

	StartWall = UFTPauseSubsystem::WallSeconds();
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const FFTStoryCarry* Carry = (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone) ? &GI->StoryCarry : nullptr;
	bStandalone = Carry == nullptr;
	bStoryAutoPlay = Carry && Carry->bAutoPlay;
	bAutoPlay = bStoryAutoPlay || FParse::Param(FCommandLine::Get(), TEXT("BeatAutoPlay"));
	bQuitOnFinish = bStandalone && FParse::Param(FCommandLine::Get(), TEXT("BeatQuitOnFinish"));

	// The fight: the step's, else the table's default; the built-in default without a usable table.
	const bool bTable = LoadTable();
	Setup = ftcore::BeatSetup();
	Setup.def = ftcore::defaultBeatFightDef();
	Seed = 1;
	if (bTable)
	{
		Setup = Table.fallback;
	}
	if (Carry)
	{
		if (bTable && Carry->Beat >= 0 && Carry->Step >= 0)
		{
			Setup = Table.find(FT::ToUtf8(Carry->Scene), static_cast<uint32>(Carry->Beat), static_cast<uint32>(Carry->Step));
		}
		Seed = BeatSeedFor(Carry->Scene, Carry->Beat, Carry->Step);
	}

	Assist = ftcore::QteAssist();
	if (const UFTSettingsSubsystem* SettingsSub = GI ? GI->GetSubsystem<UFTSettingsSubsystem>() : nullptr)
	{
		Assist.autoComplete = SettingsSub->Get().qteAutoComplete;
		Assist.holdToggle = SettingsSub->Get().holdToggle;
	}

	// The stand-ins stand where the fight's enemy count puts them.
	FindStandIns();
	SeatStandIns(FMath::Clamp(static_cast<int32>(Setup.def.enemyCount), 1, MaxStandIns));

	// Look through the map's camera rather than the pawn.
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		for (TActorIterator<ACameraActor> It(GetWorld()); It; ++It)
		{
			PC->SetViewTarget(*It);
			break;
		}
	}

	ChooseClock();
	// The fight reads the conductor's clock, so this tick comes after the conductor has taken its frame's.
	AddTickPrerequisiteComponent(Conductor);
}

void AFTBeatGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RestoreDilation();
	Super::EndPlay(EndPlayReason);
}

bool AFTBeatGameMode::LoadTable()
{
	const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/beat.json");
	if (!IFileManager::Get().FileExists(*File))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Beat: no Data/Battle/beat.json; using the built-in fight."));
		return false;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Beat: couldn't read Data/Battle/beat.json; using the built-in fight."));
		return false;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importBeatTable(FT::BytesAsText(Bytes), Table, &Error, &Warnings))
	{
		// The loader's errors name paths and keys only.
		UE_LOG(LogForgottenThird, Warning, TEXT("Beat: Data/Battle/beat.json refused; using the built-in fight:\n%s"), *FT::ToFString(Error));
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Beat: %s"), *FT::ToFString(W));
	}
	return true;
}

void AFTBeatGameMode::ChooseClock()
{
	bClockIsMusic = false;

	// A headless alpha run plays the fight 8x faster. The clock has to follow the dilated game time, or the
	// bars and the autopilot's presses would fall out of step; the music isn't dilated, so it is never bound.
	if (bStoryAutoPlay && BeatAlphaFast())
	{
		Conductor->bWallClockFollowsGameTime = true;
		Conductor->ResetClock(); // its BeginPlay already seeded the clock from platform time
		UGameplayStatics::SetGlobalTimeDilation(this, kBeatAlphaFastDilation);
		bDilated = true;
		UE_LOG(LogForgottenThird, Display, TEXT("Beat: clock: game time (wall clock following it), -AlphaFast, game time x%.0f."), kBeatAlphaFastDilation);
		return;
	}

	if (Setup.music.empty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Beat: clock: wall clock (the fight names no music cue)."));
		return;
	}
	const FString Cue = FT::ToFString(Setup.music);
	const UFTAudioSubsystem* Audio = GetGameInstance() ? GetGameInstance()->GetSubsystem<UFTAudioSubsystem>() : nullptr;
	const ftcore::AudioCue* Playing = Audio ? Audio->GetMusicCue() : nullptr;
	if (!Audio || !Playing || Audio->GetCurrentMusic() != Cue)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Beat: clock: wall clock (music cue %s isn't playing)."), *Cue);
		return;
	}
	if (!(Playing->bpm > 0.f) || !Audio->GetMusicComponent())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Beat: clock: wall clock (music cue %s has no tempo or no sound)."), *Cue);
		return;
	}
	Conductor->BindMusic(Audio->GetMusicComponent(), *Playing, Audio->GetMusicCursor());
	bClockIsMusic = true;
	UE_LOG(LogForgottenThird, Display, TEXT("Beat: clock: song time of music cue %s (%.2f bpm)."), *Cue, Playing->bpm);
	// The fight's beat grid is its own (bpm and offset in beat.json); it has to match the music's.
	if (FMath::Abs(static_cast<double>(Playing->bpm) - Setup.def.bpm) > 0.5
		|| FMath::Abs(static_cast<double>(Playing->offsetSeconds) - Setup.def.offset) > 0.05)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Beat: the fight's grid (%.2f bpm, offset %.3f s) isn't music cue %s's (%.2f bpm, offset %.3f s); the prompts will sit off the music."),
			Setup.def.bpm, Setup.def.offset, *Cue, Playing->bpm, Playing->offsetSeconds);
	}
}

bool AFTBeatGameMode::ClockReady()
{
	if (Conductor->IsClockValid())
	{
		return true;
	}
	if (!bClockIsMusic || UFTPauseSubsystem::WallSeconds() - StartWall < kBeatClockWait)
	{
		return false;
	}
	// A bound music clock is valid from the cursor's first report: it never came.
	Conductor->UnbindAudio();
	Conductor->ResetClock();
	bClockIsMusic = false;
	UE_LOG(LogForgottenThird, Warning, TEXT("Beat: the music clock didn't start within %.0f s; the fight uses the wall clock."), kBeatClockWait);
	return Conductor->IsClockValid();
}

void AFTBeatGameMode::FindStandIns()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// (Not named Tags: that is AActor's own member.)
	FName StandInTags[MaxStandIns];
	for (int32 I = 0; I < MaxStandIns; ++I)
	{
		StandInTags[I] = StandInTag(I);
		StandIns[I].Reset();
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		for (int32 I = 0; I < MaxStandIns; ++I)
		{
			if (It->ActorHasTag(StandInTags[I]))
			{
				StandIns[I] = *It;
				break;
			}
		}
	}
}

void AFTBeatGameMode::SeatStandIns(int32 Count)
{
	int32 Seated = 0;
	for (int32 I = 0; I < MaxStandIns; ++I)
	{
		AActor* Actor = StandIns[I].Get();
		if (!Actor)
		{
			continue;
		}
		const bool bShow = I < Count;
		bStandInShown[I] = bShow;
		BeatHideStandIn(Actor, !bShow);
		if (bShow)
		{
			FVector Location;
			FRotator Rotation;
			StandInPlace(I, Count, Location, Rotation);
			Actor->SetActorLocationAndRotation(Location, Rotation);
			++Seated;
		}
	}
	if (Seated < Count)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Beat: the map has %d enemy stand-ins for a fight of %d; the HUD places the others on the screen (FTBuildBeatWorld builds them)."),
			Seated, Count);
	}
}

void AFTBeatGameMode::RefreshStandIns()
{
	const ftcore::BeatFight& Fight = Screen.fight();
	for (int32 I = 0; I < MaxStandIns; ++I)
	{
		AActor* Actor = StandIns[I].Get();
		if (!Actor)
		{
			continue;
		}
		// An enemy that is down leaves the stage; a retry brings it back.
		const size_t Enemy = static_cast<size_t>(I);
		const bool bShow = Enemy < Fight.enemyCount() && Fight.enemyAlive(Enemy);
		if (bStandInShown[I] != bShow)
		{
			bStandInShown[I] = bShow;
			BeatHideStandIn(Actor, !bShow);
		}
	}
}

void AFTBeatGameMode::PollKeys()
{
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	// The frame after the pause menu closed, a key still held (pad A that picked Resume is also "down" here)
	// is not a press of the screen's own: it counts once it is let go and pressed again.
	const bool bResumed = UFTPauseSubsystem::IsJustResumed(this);
	// The time of this frame's key events on the fight's clock, latency compensated.
	const double EventTime = Conductor->GetInputSongTime();
	for (int32 D = 0; D < kBeatDirections; ++D)
	{
		bool bPressed = false;
		bool bReleased = false;
		bool bNowDown = false;
		for (int32 K = 0; K < kBeatKeysPerDirection; ++K)
		{
			const FKey& Key = BeatLaneKey(D, K);
			bPressed |= PC->WasInputKeyJustPressed(Key);
			bReleased |= PC->WasInputKeyJustReleased(Key);
			bNowDown |= PC->IsInputKeyDown(Key);
		}
		// The screen ignores them outside the fight; the key states stay current all the same.
		const ftcore::Arrow Direction = static_cast<ftcore::Arrow>(D);
		if (!bResumed && (bPressed || (bNowDown && !bDown[D])))
		{
			Screen.press(Direction, EventTime);
		}
		if (bReleased || (!bNowDown && bDown[D]))
		{
			Screen.release(Direction, EventTime);
		}
		bDown[D] = bNowDown;
	}
	if (Screen.askingRetry() && !bResumed)
	{
		if (BeatRetryPressed(*PC))
		{
			Screen.choose(ftcore::BeatChoice::Retry);
		}
		else if (BeatQuitPressed(*PC))
		{
			Screen.choose(ftcore::BeatChoice::Quit);
		}
	}
}

void AFTBeatGameMode::AddPopups(const ftcore::BeatEvents& Events)
{
	for (const ftcore::BeatEvent& E : Events)
	{
		switch (E.type)
		{
		case ftcore::BeatEventType::Judged:
		case ftcore::BeatEventType::Late:
		case ftcore::BeatEventType::LockedOut:
		{
			FPopup& P = Popups[PopupTotal % MaxPopups];
			P.Type = E.type;
			P.Grade = E.grade;
			P.Kind = E.kind;
			P.Enemy = static_cast<int32>(E.enemy);
			P.Time = E.time;
			++PopupTotal;
			break;
		}
		case ftcore::BeatEventType::Whiff:
			// Counters are locked out for E.amount milliseconds from the press.
			LockoutSince = E.time;
			LockoutUntil = E.time + static_cast<double>(E.amount) * 0.001;
			break;
		case ftcore::BeatEventType::LeadHit:
			LeadHitTime = E.time;
			break;
		case ftcore::BeatEventType::FinisherStart:
			FinisherTime = E.time;
			FinisherEnemy = static_cast<int32>(E.enemy);
			break;
		case ftcore::BeatEventType::Multiplier:
			MultiplierTime = E.time;
			break;
		default:
			break;
		}
	}
}

void AFTBeatGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bReturning || !Conductor)
	{
		return;
	}
	if (!bBegun)
	{
		// The fight begins once the clock runs, so a hitch while the map loaded can't eat the lead-in.
		if (!ClockReady())
		{
			return;
		}
		Screen.begin(true, Setup, Seed, bAutoPlay, Assist, Conductor->GetSongTime());
		bBegun = true;
	}

	// Presses first: they are stamped with the clock as it reads in this frame, then the screen takes the frame.
	PollKeys();
	Screen.tick(Conductor->GetSongTime());
	AddPopups(Screen.takeFresh());
	RefreshStandIns();
	if (Screen.finished())
	{
		Finish();
	}
}

void AFTBeatGameMode::RestoreDilation()
{
	if (bDilated && GetWorld())
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
		bDilated = false;
	}
}

void AFTBeatGameMode::Finish()
{
	if (bReturning)
	{
		return;
	}
	bReturning = true;
	RestoreDilation();

	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const bool bCarried = !bStandalone && GI && GI->StoryCarry.bValid;
	const bool bWon = Screen.won();
	const FString Scene = bCarried ? GI->StoryCarry.Scene : FString(TEXT("standalone"));
	const int32 BeatIndex = bCarried ? GI->StoryCarry.Beat : 0;
	const int32 StepIndex = bCarried ? GI->StoryCarry.Step : 0;
	const ftcore::BeatStats& Stats = Screen.stats();
	// Ids and counts only; never text.
	UE_LOG(LogForgottenThird, Display, TEXT("Beat: %s at %s beat %d step %d after %.1f s, %d perfect, %d good, %d miss, best chain %d, %d restarts."),
		bWon ? TEXT("VICTORY") : TEXT("DEFEAT"), *Scene, BeatIndex, StepIndex, Screen.playSeconds(), Stats.perfects, Stats.goods, Stats.misses,
		Stats.maxChain, Stats.restarts);
	if (Screen.gaveUp())
	{
		// The autopilot lost the fight and every retry: a counted warning, which fails an alpha run.
		UE_LOG(LogForgottenThird, Warning, TEXT("Beat: autoplay lost %s beat %d step %d and its %d retries; the step ends lost."), *Scene, BeatIndex,
			StepIndex, Screen.autoRetries());
		if (GI)
		{
			++GI->Alpha.warnings;
		}
	}

	if (!bCarried)
	{
		// A fight on its own has nowhere to go back to.
		if (bQuitOnFinish)
		{
			FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(bWon ? 0 : 1), TEXT("Beat"));
		}
		return;
	}
	// The step is done, won or lost; the dispatcher counts it (FTStep::FinishCarried), never this screen.
	FFTStoryCarry& Carry = GI->StoryCarry;
	Carry.bDone = true;
	Carry.bWon = bWon;
	if (FTStep::ContinueHere(this))
	{
		return;
	}
	OpenReturnMap();
}

void AFTBeatGameMode::OpenReturnMap()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid && !GI->StoryCarry.ReturnMap.IsEmpty())
	{
		UGameplayStatics::OpenLevel(this, FName(*GI->StoryCarry.ReturnMap));
	}
}
