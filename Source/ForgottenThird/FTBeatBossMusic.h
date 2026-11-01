#pragma once

// The beat boss demo's music (docs/BEAT_BOSS_DEMO.md, "The clock and the music"): a procedural sound that plays
// what ftcore::BeatMusicDriver schedules from the fight, through ftcore::BeatMusicSynth (Core/include/ftcore).
// All the music is code: the pattern sequencer decides each bar, the synth makes the sound, and this class
// only carries the driver's events, stamped with the fight's clock, to the audio thread at exact frames.
//
//   UFTBeatBossMusic* Music = NewObject<UFTBeatBossMusic>(Owner);
//   UGameplayStatics::SpawnSound2D(Owner, Music, ...);          // plays for as long as it lives
//   each frame, after driver.update(...):
//     if (Music->Sync(Now)) { /* the mapping was re-anchored (a pause, a hold): restart the driver */ }
//     Music->Post(Events, Now);
//   Music->Trigger(ftcore::MusicSound::Good);                    // a cue at once
//
// Timing. The synth counts its own frames. The game thread maps the fight's clock to them with one fixed
// offset, found when the stream is first seen running and again after a pause (Sync): an event for fight time t
// is queued for frame (t + offset) x rate, so events keep exactly the spacing the fight's clock gave them, whatever
// the frame time or the size of the audio buffers, and the driver's lookahead (0.1 s) keeps them ahead of the
// frames the audio thread has made. The offset leaves a margin (kMargin) over how far the audio thread is ahead
// of the clock, so the music sounds that much after the beat line's clock plus the device's own latency;
// an event for a future time that still arrives behind the stream makes the offset grow by what it was late.
//
// Commands go through a lock-free ring (one writer, the game thread; one reader, the audio thread), which the
// audio thread drains at the start of each buffer. Nothing allocates after construction (the one scratch buffer
// settles on the engine's buffer size on the first call).

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"

#include "ftcore/BeatMusic.h"
#include "ftcore/BeatMusicSynth.h"

#include <atomic>

#include "FTBeatBossMusic.generated.h"

UCLASS()
class FORGOTTENTHIRD_API UFTBeatBossMusic : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	static constexpr int32 Rate = 48000;

	UFTBeatBossMusic(const FObjectInitializer& ObjectInitializer);

	// Game thread. True once the audio thread has produced sound (until then nothing can be placed).
	bool IsRunning() const { return Rendered.load(std::memory_order_acquire) > 0; }
	// Game thread, once a frame before Post: keeps the clock-to-frame mapping. True when it was (re)anchored:
	// the stream had just started, or the fight's clock stood still while the wall clock ran (a pause, a hold).
	// The host then starts the driver over, since what was scheduled ahead belongs to the old mapping.
	bool Sync(double ClockNow);
	// Game thread. Queues the driver's events (times on the fight's clock).
	void Post(const ftcore::MusicEvents& Events, double ClockNow);
	// Game thread. A cue at once (a judged answer).
	void Trigger(ftcore::MusicSound Sound, uint8 Velocity = 127);

	// Sounds the synth had to start late (audio thread's count, for logs).
	uint32 GetLateCount() const { return Late.load(std::memory_order_relaxed); }
	uint32 GetDroppedCount() const { return Dropped.load(std::memory_order_relaxed); }

	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;
	virtual Audio::EAudioMixerStreamDataFormat::Type GetGeneratedPCMDataFormat() const override
	{
		return Audio::EAudioMixerStreamDataFormat::Int16;
	}

private:
	enum class ECommand : uint8
	{
		Play,
		Revoke,
		Cut,
	};
	struct FCommand
	{
		uint64 Frame = 0;
		uint32 Length = 0;
		ECommand Kind = ECommand::Play;
		uint8 Sound = 0;
		uint8 Velocity = 0;
		uint8 Note = 0;
		uint8 Aux = 0;
	};
	static constexpr uint32 RingSize = 1024;
	static constexpr double Margin = 0.04;    // seconds of lead over the audio thread's position when anchoring
	static constexpr double MaxBump = 0.25;   // the offset grows by no more than this on top of its anchor

	uint64 FrameOf(double Time) const;
	void Push(const FCommand& Command);

	// Game thread.
	double Offset = 0.0;
	double Anchor = 0.0;
	double LastClock = 0.0;
	double LastWall = 0.0;
	bool bAnchored = false;
	std::atomic<uint32> Written{0};
	FCommand Ring[RingSize];

	// Audio thread.
	std::atomic<uint32> Taken{0};
	ftcore::BeatMusicSynth Synth;
	TArray<float> Scratch;

	std::atomic<uint64> Rendered{0};
	std::atomic<uint32> Late{0};
	std::atomic<uint32> Dropped{0};
};
