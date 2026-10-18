#pragma once

// An arrow combo (DDR-style steps) judged against the music: ftcore::ComboSequence timed by a
// UFTConductorComponent. All judging rules live in Core; this feeds it presses and the song clock
// and turns its results into events.
//
// Use:
//  1. Put a UFTConductorComponent on the same actor (or set Conductor) and drive its clock.
//  2. Start a chart: StartCombo(Notes) with note times in song seconds, or GenerateCombo(...) for a
//     deterministic chart from a 0-10 difficulty rating (on the conductor's beat grid if asked).
//  3. Call Press(Arrow) from input. The press is judged at GetInputSongTime(), so calibration
//     latency is already removed.
//  4. Listen to OnNoteHit / OnNoteMiss / OnStray / OnScoreChanged / OnComboFinished. Notes whose
//     window passes unanswered are judged Miss by TickComponent.
//
// Battles judge their own combos (UFTBattleComponent); this component is for combos outside a battle.
// The Blueprint types for arrows, grades and notes are declared here and shared with the battle.

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "ftcore/ComboSequence.h"

#include <vector>

#include "FTComboComponent.generated.h"

class UFTConductorComponent;

// Same order as ftcore::Arrow (dance-pad lanes).
UENUM(BlueprintType)
enum class EFTArrow : uint8
{
	Left,
	Down,
	Up,
	Right,
};

// Same order as ftcore::Grade.
UENUM(BlueprintType)
enum class EFTGrade : uint8
{
	Pending,
	Perfect,
	Great,
	Good,
	Miss,
};

USTRUCT(BlueprintType)
struct FFTComboNote
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	EFTArrow Arrow = EFTArrow::Left;
	// Song seconds. Judgement uses this.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	double Time = 0.0;
	// Song beat for charts on a beat grid, else -1.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	double Beat = -1.0;
	// Pending until judged (read-only in practice; ignored by StartCombo).
	UPROPERTY(BlueprintReadOnly, Category = "Combo")
	EFTGrade Grade = EFTGrade::Pending;
};

// Judgement half-widths in seconds around each note. Must satisfy Perfect <= Great <= Good.
USTRUCT(BlueprintType)
struct FFTJudgeWindows
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	double Perfect = 0.045;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	double Great = 0.090;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	double Good = 0.135;
};

// How much each grade adds to the score (Miss adds nothing).
USTRUCT(BlueprintType)
struct FFTGradeWeights
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	float Perfect = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	float Great = 0.8f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	float Good = 0.5f;
};

USTRUCT(BlueprintType)
struct FFTNoteResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Combo")
	int32 Index = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Combo")
	EFTGrade Grade = EFTGrade::Pending;
	// Press time - note time in seconds (negative = early); 0 for notes that expired.
	UPROPERTY(BlueprintReadOnly, Category = "Combo")
	double Error = 0.0;
	// Judged Miss because a different arrow was pressed in its window.
	UPROPERTY(BlueprintReadOnly, Category = "Combo")
	bool bWrongArrow = false;
};

namespace FT
{
inline ftcore::Arrow ToCore(EFTArrow A) { return static_cast<ftcore::Arrow>(A); }
inline EFTArrow FromCore(ftcore::Arrow A) { return static_cast<EFTArrow>(A); }
inline ftcore::Grade ToCore(EFTGrade G) { return static_cast<ftcore::Grade>(G); }
inline EFTGrade FromCore(ftcore::Grade G) { return static_cast<EFTGrade>(G); }
ftcore::JudgeWindows ToCore(const FFTJudgeWindows& W);
ftcore::GradeWeights ToCore(const FFTGradeWeights& W);
FFTComboNote FromCore(const ftcore::ComboNote& N, ftcore::Grade G);
FFTNoteResult FromCore(const ftcore::NoteResult& R);
// Windows that ftcore accepts (Perfect <= Great <= Good, none negative).
ftcore::JudgeWindows SafeWindows(const ftcore::JudgeWindows& W);
} // namespace FT

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTComboNoteEvent, const FFTNoteResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFTComboScoreEvent, float, Score, float, Accuracy, int32, Combo);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFTComboFinishedEvent, float, Accuracy, bool, bFullCombo);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFTComboStrayEvent);

UCLASS(ClassGroup = (ForgottenThird), meta = (BlueprintSpawnableComponent))
class FORGOTTENTHIRD_API UFTComboComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFTComboComponent();

	// The song clock. None: the owner's UFTConductorComponent, found at BeginPlay.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	TObjectPtr<UFTConductorComponent> Conductor;

	// Base judgement windows (GenerateCombo scales them by difficulty).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	FFTJudgeWindows Windows;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	FFTGradeWeights Weights;
	// Seconds input is ignored after a press with no note in range.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	double Lockout = 0.25;
	// Longest chart StartCombo / GenerateCombo expect; storage is reserved for it at BeginPlay.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo")
	int32 MaxNotes = 32;

	// Starts a chart (times in song seconds; sorted here). Replaces any running combo.
	UFUNCTION(BlueprintCallable, Category = "Combo")
	void StartCombo(const TArray<FFTComboNote>& Notes);
	// Generates and starts a chart for a difficulty Rating (0-10) and a move's BaseNotes. The first
	// note comes LeadIn seconds from now; with bOnBeatGrid every note sits on the conductor's beat
	// grid (the first on the next beat after LeadIn). Same seed, same chart. Returns the note count.
	UFUNCTION(BlueprintCallable, Category = "Combo")
	int32 GenerateCombo(float Rating, int32 BaseNotes = 4, double LeadIn = 1.0, int64 Seed = 1, bool bOnBeatGrid = true);
	UFUNCTION(BlueprintCallable, Category = "Combo")
	void StopCombo();

	// Judges a step pressed now.
	UFUNCTION(BlueprintCallable, Category = "Combo")
	void Press(EFTArrow Arrow);

	UFUNCTION(BlueprintCallable, Category = "Combo")
	void SetConductor(UFTConductorComponent* InConductor) { Conductor = InConductor; }

	// A combo is running (started and not every note judged).
	UFUNCTION(BlueprintPure, Category = "Combo")
	bool IsComboActive() const { return bActive; }
	UFUNCTION(BlueprintPure, Category = "Combo")
	int32 GetNoteCount() const { return static_cast<int32>(Sequence.size()); }
	// Note Index with its grade so far (a default note when out of range).
	UFUNCTION(BlueprintPure, Category = "Combo")
	FFTComboNote GetNote(int32 Index) const;
	// Earliest note not judged yet (== GetNoteCount() when all are).
	UFUNCTION(BlueprintPure, Category = "Combo")
	int32 GetNextNoteIndex() const { return static_cast<int32>(Sequence.nextIndex()); }
	UFUNCTION(BlueprintPure, Category = "Combo")
	int32 GetJudgedCount() const { return static_cast<int32>(Sequence.judged()); }
	UFUNCTION(BlueprintPure, Category = "Combo")
	int32 GetGradeCount(EFTGrade Grade) const { return static_cast<int32>(Sequence.count(FT::ToCore(Grade))); }
	// Sum of the grade weights so far.
	UFUNCTION(BlueprintPure, Category = "Combo")
	float GetScore() const { return Sequence.score(); }
	// Score over the whole chart, 0..1 (unjudged notes count 0).
	UFUNCTION(BlueprintPure, Category = "Combo")
	float GetAccuracy() const { return Sequence.accuracy(); }
	UFUNCTION(BlueprintPure, Category = "Combo")
	int32 GetCombo() const { return Sequence.combo(); }
	UFUNCTION(BlueprintPure, Category = "Combo")
	int32 GetMaxCombo() const { return Sequence.maxCombo(); }
	UFUNCTION(BlueprintPure, Category = "Combo")
	int32 GetStrays() const { return Sequence.strays(); }
	UFUNCTION(BlueprintPure, Category = "Combo")
	bool IsFullCombo() const { return Sequence.fullCombo(); }

	// A note was judged Perfect, Great or Good.
	UPROPERTY(BlueprintAssignable, Category = "Combo")
	FFTComboNoteEvent OnNoteHit;
	// A note was missed: expired, or a wrong arrow in its window (Result.bWrongArrow).
	UPROPERTY(BlueprintAssignable, Category = "Combo")
	FFTComboNoteEvent OnNoteMiss;
	// A press with no note in range; input is locked out for Lockout seconds.
	UPROPERTY(BlueprintAssignable, Category = "Combo")
	FFTComboStrayEvent OnStray;
	// After each batch of judged notes.
	UPROPERTY(BlueprintAssignable, Category = "Combo")
	FFTComboScoreEvent OnScoreChanged;
	// Every note is judged.
	UPROPERTY(BlueprintAssignable, Category = "Combo")
	FFTComboFinishedEvent OnComboFinished;

	// Native access.
	const ftcore::ComboSequence& GetSequence() const { return Sequence; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

private:
	bool HasClock() const;
	void Begin(const ftcore::JudgeWindows& BaseWindows); // Scratch holds the new chart: swap it in and go
	void Dispatch(); // broadcasts Results, then the score and the finish

	ftcore::ComboSequence Sequence;
	std::vector<ftcore::ComboNote> Scratch;  // swapped with the sequence's notes on each start
	std::vector<ftcore::NoteResult> Results; // reused by Press/Tick
	bool bActive = false;
	bool bDispatching = false;
	mutable bool bWarnedNoClock = false;
};
