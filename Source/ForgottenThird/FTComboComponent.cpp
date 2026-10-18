#include "FTComboComponent.h"

#include "FTConductorComponent.h"
#include "GameFramework/Actor.h"

#include "ftcore/ComboGenerator.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogFTCombo, Log, All);

static_assert(static_cast<int>(EFTArrow::Left) == static_cast<int>(ftcore::Arrow::Left));
static_assert(static_cast<int>(EFTArrow::Down) == static_cast<int>(ftcore::Arrow::Down));
static_assert(static_cast<int>(EFTArrow::Up) == static_cast<int>(ftcore::Arrow::Up));
static_assert(static_cast<int>(EFTArrow::Right) == static_cast<int>(ftcore::Arrow::Right));
static_assert(static_cast<int>(EFTGrade::Pending) == static_cast<int>(ftcore::Grade::Pending));
static_assert(static_cast<int>(EFTGrade::Perfect) == static_cast<int>(ftcore::Grade::Perfect));
static_assert(static_cast<int>(EFTGrade::Great) == static_cast<int>(ftcore::Grade::Great));
static_assert(static_cast<int>(EFTGrade::Good) == static_cast<int>(ftcore::Grade::Good));
static_assert(static_cast<int>(EFTGrade::Miss) == static_cast<int>(ftcore::Grade::Miss));

namespace FT
{
ftcore::JudgeWindows ToCore(const FFTJudgeWindows& W) { return {W.Perfect, W.Great, W.Good}; }
ftcore::GradeWeights ToCore(const FFTGradeWeights& W) { return {W.Perfect, W.Great, W.Good}; }

FFTComboNote FromCore(const ftcore::ComboNote& N, ftcore::Grade G)
{
	FFTComboNote Out;
	Out.Arrow = FromCore(N.arrow);
	Out.Time = N.time;
	Out.Beat = N.beat;
	Out.Grade = FromCore(G);
	return Out;
}

FFTNoteResult FromCore(const ftcore::NoteResult& R)
{
	FFTNoteResult Out;
	Out.Index = static_cast<int32>(R.index);
	Out.Grade = FromCore(R.grade);
	Out.Error = R.error;
	Out.bWrongArrow = R.wrongArrow;
	return Out;
}

ftcore::JudgeWindows SafeWindows(const ftcore::JudgeWindows& W)
{
	ftcore::JudgeWindows Out;
	Out.perfect = std::max(W.perfect, 0.0);
	Out.great = std::max(W.great, Out.perfect);
	Out.good = std::max(W.good, Out.great);
	return Out;
}
} // namespace FT

UFTComboComponent::UFTComboComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false; // on while a combo runs
}

void UFTComboComponent::BeginPlay()
{
	Super::BeginPlay();
	if (!Conductor && GetOwner())
	{
		Conductor = GetOwner()->FindComponentByClass<UFTConductorComponent>();
	}
	if (Conductor)
	{
		// Tick after the conductor has taken this frame's cursor.
		AddTickPrerequisiteComponent(Conductor);
	}
	const size_t Reserve = static_cast<size_t>(FMath::Max(MaxNotes, 1));
	Scratch.reserve(Reserve);
	Results.reserve(Reserve + 2);
	Sequence.reserve(Reserve);
}

bool UFTComboComponent::HasClock() const
{
	if (Conductor && Conductor->IsClockValid())
	{
		return true;
	}
	if (!bWarnedNoClock)
	{
		UE_LOG(LogFTCombo, Warning, TEXT("%s: no running conductor clock; the combo waits for one"), *GetPathName());
		bWarnedNoClock = true;
	}
	return false;
}

void UFTComboComponent::StartCombo(const TArray<FFTComboNote>& Notes)
{
	Scratch.clear();
	for (const FFTComboNote& N : Notes)
	{
		Scratch.push_back({FT::ToCore(N.Arrow), N.Time, N.Beat});
	}
	std::stable_sort(Scratch.begin(), Scratch.end(),
		[](const ftcore::ComboNote& A, const ftcore::ComboNote& B) { return A.time < B.time; });
	Begin(FT::ToCore(Windows));
}

int32 UFTComboComponent::GenerateCombo(float Rating, int32 BaseNotes, double LeadIn, int64 Seed, bool bOnBeatGrid)
{
	if (!HasClock())
	{
		return 0;
	}
	const ftcore::DifficultyTuning Tuning;
	const ftcore::ConductorConfig& Clock = Conductor->GetConductor().config();
	const bool bGrid = bOnBeatGrid && Clock.bpm > 0.0;
	ftcore::ComboParams Params = bGrid ? ftcore::comboParams(BaseNotes, Rating, Tuning, Clock.bpm)
	                                   : ftcore::comboParams(BaseNotes, Rating, Tuning);
	if (bGrid)
	{
		Params.beatOffset = Clock.offsetSeconds;
	}
	const double Start = Conductor->GetSongTime() + FMath::Max(LeadIn, 0.0);
	ftcore::generateComboInto(Scratch, Params, Start, static_cast<uint64>(Seed));
	Begin(ftcore::scaled(FT::ToCore(Windows), Params.windowScale));
	return GetNoteCount();
}

void UFTComboComponent::Begin(const ftcore::JudgeWindows& BaseWindows)
{
	Sequence.reset(Scratch, FT::SafeWindows(BaseWindows), FMath::Max(Lockout, 0.0), FT::ToCore(Weights));
	Results.clear();
	bActive = true;
	SetComponentTickEnabled(true);
	Dispatch(); // an empty chart finishes at once
}

void UFTComboComponent::StopCombo()
{
	bActive = false;
	Results.clear();
	SetComponentTickEnabled(false);
}

void UFTComboComponent::Press(EFTArrow Arrow)
{
	if (!bActive || !HasClock())
	{
		return;
	}
	const int StraysBefore = Sequence.strays();
	Sequence.press(FT::ToCore(Arrow), Conductor->GetInputSongTime(), Results);
	Dispatch();
	if (Sequence.strays() > StraysBefore)
	{
		OnStray.Broadcast();
	}
}

void UFTComboComponent::Dispatch()
{
	if (bDispatching)
	{
		return; // a handler pressed: the loop below picks its results up
	}
	bDispatching = true;
	const bool bAny = !Results.empty();
	// By index: a handler may press (appending) or start/stop a combo (clearing).
	for (size_t I = 0; I < Results.size(); ++I)
	{
		const FFTNoteResult R = FT::FromCore(Results[I]);
		if (R.Grade == EFTGrade::Miss)
		{
			OnNoteMiss.Broadcast(R);
		}
		else
		{
			OnNoteHit.Broadcast(R);
		}
	}
	Results.clear();
	bDispatching = false;

	if (bAny && bActive)
	{
		OnScoreChanged.Broadcast(Sequence.score(), Sequence.accuracy(), Sequence.combo());
	}
	if (bActive && Sequence.complete())
	{
		bActive = false;
		SetComponentTickEnabled(false);
		OnComboFinished.Broadcast(Sequence.accuracy(), Sequence.fullCombo());
	}
}

FFTComboNote UFTComboComponent::GetNote(int32 Index) const
{
	if (Index < 0 || static_cast<size_t>(Index) >= Sequence.size())
	{
		return FFTComboNote();
	}
	const size_t I = static_cast<size_t>(Index);
	return FT::FromCore(Sequence.note(I), Sequence.grade(I));
}

void UFTComboComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bActive || !HasClock())
	{
		return;
	}
	Sequence.update(Conductor->GetSongTime(), Results);
	Dispatch();
}
