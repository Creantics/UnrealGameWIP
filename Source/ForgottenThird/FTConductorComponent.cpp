#include "FTConductorComponent.h"

#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Sound/SoundWave.h"

#include "ftcore/Calibration.h"

#include <cmath>
#include <span>

UFTConductorComponent::UFTConductorComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UFTConductorComponent::BeginPlay()
{
	Super::BeginPlay();
	ApplySettings();
	Taps.Reserve(64);
	ObserveWallClock(); // valid from the first frame
}

void UFTConductorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindAudio();
	Super::EndPlay(EndPlayReason);
}

void UFTConductorComponent::ApplySettings()
{
	Conductor.setBpm(Bpm > 0.0 ? Bpm : 120.0);
	Conductor.setOffsetSeconds(OffsetSeconds);
	Conductor.setLatencyOffset(bCalibrating ? 0.0 : LatencyOffset);
}

void UFTConductorComponent::BindAudio(UAudioComponent* Audio)
{
	UnbindAudio();
	ResetClock();
	BoundAudio = Audio;
	if (BoundAudio)
	{
		BoundAudio->OnAudioPlaybackPercentNative.AddUObject(this, &UFTConductorComponent::HandlePlaybackPercent);
	}
}

void UFTConductorComponent::UnbindAudio()
{
	if (BoundAudio)
	{
		BoundAudio->OnAudioPlaybackPercentNative.RemoveAll(this);
		BoundAudio = nullptr;
	}
}

void UFTConductorComponent::HandlePlaybackPercent(const UAudioComponent* /*Audio*/, const USoundWave* Wave, const float Percent)
{
	if (Wave)
	{
		LatestCursor = static_cast<double>(Percent) * static_cast<double>(Wave->Duration);
		bHaveCursor = true;
	}
}

void UFTConductorComponent::ObserveCursor(double AudioCursorSeconds)
{
	Conductor.observe(AudioCursorSeconds, FPlatformTime::Seconds());
}

void UFTConductorComponent::ResetClock()
{
	Conductor.reset();
	LatestCursor = 0.0;
	bHaveCursor = false;
	WallEpoch = ClockNow();
	bHaveWallEpoch = true;
	ObserveWallClock();
}

double UFTConductorComponent::ClockNow() const
{
	if (bWallClockFollowsGameTime && bWallClockWithoutAudio && !BoundAudio)
	{
		if (const UWorld* World = GetWorld())
		{
			return World->GetTimeSeconds();
		}
	}
	return FPlatformTime::Seconds();
}

void UFTConductorComponent::ObserveWallClock()
{
	if (bWallClockWithoutAudio && !BoundAudio)
	{
		const double Wall = ClockNow();
		if (!bHaveWallEpoch)
		{
			WallEpoch = Wall;
			bHaveWallEpoch = true;
		}
		Conductor.observe(Wall - WallEpoch, Wall);
	}
}

double UFTConductorComponent::GetSongTime()
{
	if (bWallClockFollowsGameTime)
	{
		// Game frames can be long under time dilation: bring the cursor up to this frame first, or the
		// clock would stall at its extrapolation limit.
		ObserveWallClock();
	}
	return Conductor.songTime(ClockNow());
}
double UFTConductorComponent::GetBeat() { return Conductor.beatAt(GetSongTime()); }
double UFTConductorComponent::GetInputSongTime() const { return Conductor.songTimeForEvent(ClockNow()); }
double UFTConductorComponent::GetEventSongTime(double WallSeconds) const { return Conductor.songTimeForEvent(WallSeconds); }

void UFTConductorComponent::BeginCalibration()
{
	Taps.Reset();
	bCalibrating = true;
	SavedLatency = LatencyOffset;
	ApplySettings(); // taps are measured without the current offset
}

void UFTConductorComponent::RecordTap()
{
	if (bCalibrating && Conductor.valid())
	{
		Taps.Add(Conductor.songTimeForEvent(FPlatformTime::Seconds()));
	}
}

FFTCalibrationResult UFTConductorComponent::EndCalibration(bool bApply)
{
	FFTCalibrationResult Out;
	if (!bCalibrating)
	{
		return Out;
	}
	bCalibrating = false;
	LatencyOffset = SavedLatency;
	if (Taps.Num() > 0)
	{
		// The beats around the taps, one either side.
		double First = Taps[0], Last = Taps[0];
		for (const double T : Taps)
		{
			First = FMath::Min(First, T);
			Last = FMath::Max(Last, T);
		}
		const int64 B0 = static_cast<int64>(std::floor(Conductor.beatAt(First))) - 1;
		const int64 B1 = static_cast<int64>(std::ceil(Conductor.beatAt(Last))) + 1;
		TArray<double> Beats;
		Beats.Reserve(static_cast<int32>(B1 - B0 + 1));
		for (int64 B = B0; B <= B1; ++B)
		{
			Beats.Add(Conductor.timeAtBeat(static_cast<double>(B)));
		}
		const ftcore::CalibrationResult R = ftcore::estimateLatency(
			std::span<const double>(Taps.GetData(), static_cast<size_t>(Taps.Num())),
			std::span<const double>(Beats.GetData(), static_cast<size_t>(Beats.Num())));
		Out.Offset = R.offset;
		Out.Spread = R.spread;
		Out.Samples = R.samples;
		Out.bReliable = R.reliable;
		if (bApply && R.reliable)
		{
			LatencyOffset = R.offset;
		}
	}
	ApplySettings();
	return Out;
}

void UFTConductorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (BoundAudio && bHaveCursor)
	{
		Conductor.observe(LatestCursor, FPlatformTime::Seconds());
	}
	else
	{
		ObserveWallClock();
	}
}
