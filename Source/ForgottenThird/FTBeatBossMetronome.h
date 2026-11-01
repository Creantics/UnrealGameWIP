#pragma once

// The beat boss demo's stand-in music (docs/BEAT_BOSS_DEMO.md): a procedural sound that synthesises short
// voices on the audio thread when the game thread triggers them. No sound asset and no licensed audio: the
// beat is a click (accented on a bar line), a count is its own blip, and each move's impact sounds its stem
// (a low thump, a noise hit or a chime), so the track's moves are heard where the beat line draws them.
//
//   UFTBeatBossMetronome* Wave = NewObject<UFTBeatBossMetronome>(Owner);
//   UGameplayStatics::SpawnSound2D(Owner, Wave, ...);           // plays for as long as it lives
//   Wave->Trigger(UFTBeatBossMetronome::EVoice::Accent);         // game thread, any time
//
// Trigger is lock-free and allocates nothing (a fixed ring of 64 triggers, read on the audio thread; more
// than 64 between two buffers would overwrite the oldest, which a frame's few never come near). The audio thread mixes up to 8 voices at once into the
// buffer the engine hands it, whose size it settles on once. Timing is the frame's: a voice starts on the
// next audio buffer after its trigger, a few tens of milliseconds, which is fine for a demo's beat.

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"

#include <atomic>

#include "FTBeatBossMetronome.generated.h"

UCLASS()
class FORGOTTENTHIRD_API UFTBeatBossMetronome : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	enum class EVoice : uint8
	{
		Click,  // a beat
		Accent, // a bar line
		Count,  // a count's numeral ("click" counts)
		Voice,  // a count's numeral ("voice" counts: lower and longer)
		Bass,   // stem 0: a low thump
		Snare,  // stem 1: a noise hit
		Chime,  // stem 2: a bell
		Good,   // an answer landed
		Miss,   // a miss
	};

	UFTBeatBossMetronome(const FObjectInitializer& ObjectInitializer);

	// Game thread. Starts Voice on the next buffer.
	void Trigger(EVoice Voice);

	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;
	virtual Audio::EAudioMixerStreamDataFormat::Type GetGeneratedPCMDataFormat() const override
	{
		return Audio::EAudioMixerStreamDataFormat::Int16;
	}

private:
	static constexpr uint32 RingSize = 64;
	static constexpr int32 MaxVoices = 8;
	static constexpr int32 Rate = 48000;

	struct FVoiceState
	{
		int32 Kind = -1; // EVoice, -1: free
		int32 Age = 0;   // samples since it started
		uint32 Noise = 22222u;
	};

	float Sample(FVoiceState& V) const;

	uint8 Ring[RingSize] = {};
	std::atomic<uint32> Written{0}; // game thread writes
	uint32 Taken = 0;               // audio thread reads
	FVoiceState Voices[MaxVoices];
};
