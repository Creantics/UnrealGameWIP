#pragma once

// The rhythm clock for a song: ftcore::Conductor fed with the audio cursor, which gives a smooth
// song time and latency-compensated times for input events. It also runs latency calibration
// (tap along to a beat, then ftcore::estimateLatency).
//
// The wall clock is FPlatformTime::Seconds() everywhere. Feed the cursor either by binding a
// playing UAudioComponent (BindAudio) or by calling ObserveCursor once per frame. With
// bWallClockWithoutAudio and nothing bound, wall time is the cursor (a silent song clock).

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "ftcore/Conductor.h"

#include "FTConductorComponent.generated.h"

class UAudioComponent;
class USoundWave;

USTRUCT(BlueprintType)
struct FFTCalibrationResult
{
	GENERATED_BODY()

	// Seconds the player taps late (positive) or early. The latency offset to use.
	UPROPERTY(BlueprintReadOnly, Category = "Rhythm")
	double Offset = 0.0;
	// Median absolute deviation of the taps, seconds.
	UPROPERTY(BlueprintReadOnly, Category = "Rhythm")
	double Spread = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "Rhythm")
	int32 Samples = 0;
	// Enough taps, consistent enough to use.
	UPROPERTY(BlueprintReadOnly, Category = "Rhythm")
	bool bReliable = false;
};

UCLASS(ClassGroup = (ForgottenThird), meta = (BlueprintSpawnableComponent))
class FORGOTTENTHIRD_API UFTConductorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFTConductorComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rhythm")
	double Bpm = 120.0;
	// Song time of beat 0.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rhythm")
	double OffsetSeconds = 0.0;
	// How late this player's presses land (from calibration). Subtracted from event times.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rhythm")
	double LatencyOffset = 0.0;
	// With no audio bound, run the song clock on wall time (seconds since BeginPlay or the last
	// ResetClock) instead of standing at 0. For silent rhythm play and tests.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rhythm")
	bool bWallClockWithoutAudio = false;
	// With bWallClockWithoutAudio and no audio bound, the silent song clock follows game time
	// (UWorld::GetTimeSeconds, so global time dilation and pauses count) instead of platform time.
	// For sped-up headless runs: the beat grid, combo timing and an autopilot's presses stay in
	// sync with the dilated game. Set before ResetClock. Event times from other sources (taps from
	// the player controller) are platform-time based and don't belong in this mode.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rhythm")
	bool bWallClockFollowsGameTime = false;

	// Applies Bpm / OffsetSeconds / LatencyOffset after changing them at runtime.
	UFUNCTION(BlueprintCallable, Category = "Rhythm")
	void ApplySettings();

	// Follows Audio's playback position until Unbind or another BindAudio. Resets the clock.
	UFUNCTION(BlueprintCallable, Category = "Rhythm")
	void BindAudio(UAudioComponent* Audio);
	UFUNCTION(BlueprintCallable, Category = "Rhythm")
	void UnbindAudio();
	// Feeds a cursor (seconds) from another audio source. Call once per frame.
	UFUNCTION(BlueprintCallable, Category = "Rhythm")
	void ObserveCursor(double AudioCursorSeconds);
	UFUNCTION(BlueprintCallable, Category = "Rhythm")
	void ResetClock();

	UFUNCTION(BlueprintPure, Category = "Rhythm")
	bool IsClockValid() const { return Conductor.valid(); }
	UFUNCTION(BlueprintPure, Category = "Rhythm")
	bool IsPaused() const { return Conductor.paused(); }

	// Smooth song time now (never decreasing except on seeks).
	UFUNCTION(BlueprintCallable, Category = "Rhythm")
	double GetSongTime();
	// Beat number (fractional) now.
	UFUNCTION(BlueprintCallable, Category = "Rhythm")
	double GetBeat();
	// Latency-compensated song time of an input that is handled now. Use this for judgement.
	UFUNCTION(BlueprintPure, Category = "Rhythm")
	double GetInputSongTime() const;
	// The same, for an event at WallSeconds (FPlatformTime::Seconds()).
	UFUNCTION(BlueprintPure, Category = "Rhythm")
	double GetEventSongTime(double WallSeconds) const;
	UFUNCTION(BlueprintPure, Category = "Rhythm")
	double TimeAtBeat(double Beat) const { return Conductor.timeAtBeat(Beat); }

	// ---- calibration: play a steady beat, call RecordTap on each press, then EndCalibration ----

	UFUNCTION(BlueprintCallable, Category = "Rhythm")
	void BeginCalibration();
	UFUNCTION(BlueprintCallable, Category = "Rhythm")
	void RecordTap();
	// Measures the latency from the taps. bApply: use it as LatencyOffset when reliable.
	UFUNCTION(BlueprintCallable, Category = "Rhythm")
	FFTCalibrationResult EndCalibration(bool bApply = true);
	UFUNCTION(BlueprintPure, Category = "Rhythm")
	bool IsCalibrating() const { return bCalibrating; }

	// Native access.
	ftcore::Conductor& GetConductor() { return Conductor; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void HandlePlaybackPercent(const UAudioComponent* Audio, const USoundWave* Wave, const float Percent);

	ftcore::Conductor Conductor;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> BoundAudio;
	// Feeds wall time as the cursor when bWallClockWithoutAudio and nothing is bound.
	void ObserveWallClock();
	// The clock the silent song follows: game time with bWallClockFollowsGameTime, else platform time.
	double ClockNow() const;

	double LatestCursor = 0.0;
	bool bHaveCursor = false;
	double WallEpoch = 0.0;
	bool bHaveWallEpoch = false;

	bool bCalibrating = false;
	double SavedLatency = 0.0;
	TArray<double> Taps;
};
