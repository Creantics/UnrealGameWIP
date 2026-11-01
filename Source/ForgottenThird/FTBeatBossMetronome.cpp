#include "FTBeatBossMetronome.h"

namespace
{
constexpr float kMetronomeTwoPi = 6.2831853f;

// How long each voice sounds, in seconds (EVoice order).
constexpr float kMetronomeLength[] = {0.03f, 0.05f, 0.09f, 0.16f, 0.32f, 0.2f, 0.45f, 0.07f, 0.24f};
constexpr int32 kMetronomeVoiceKinds = static_cast<int32>(sizeof(kMetronomeLength) / sizeof(kMetronomeLength[0]));
} // namespace

UFTBeatBossMetronome::UFTBeatBossMetronome(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	SetSampleRate(Rate);
	NumChannels = 1;
	Duration = INDEFINITELY_LOOPING_DURATION;
	bLooping = false;
	SoundGroup = SOUNDGROUP_Default;
	bCanProcessAsync = true;
}

void UFTBeatBossMetronome::Trigger(EVoice Voice)
{
	// One writer (the game thread): the slot is filled before the count that publishes it.
	const uint32 W = Written.load(std::memory_order_relaxed);
	Ring[W % RingSize] = static_cast<uint8>(Voice);
	Written.store(W + 1, std::memory_order_release);
}

float UFTBeatBossMetronome::Sample(FVoiceState& V) const
{
	const float T = static_cast<float>(V.Age) / static_cast<float>(Rate);
	switch (static_cast<EVoice>(V.Kind))
	{
	case EVoice::Click:
		return 0.32f * FMath::Exp(-T * 160.f) * FMath::Sin(kMetronomeTwoPi * 1600.f * T);
	case EVoice::Accent:
		return 0.5f * FMath::Exp(-T * 100.f) * FMath::Sin(kMetronomeTwoPi * 2200.f * T);
	case EVoice::Count:
		return 0.45f * FMath::Exp(-T * 45.f) * FMath::Sin(kMetronomeTwoPi * 1000.f * T);
	case EVoice::Voice:
	{
		const float S = FMath::Sin(kMetronomeTwoPi * 440.f * T);
		return 0.35f * FMath::Exp(-T * 18.f) * (0.6f * S + 0.4f * FMath::Sign(S));
	}
	case EVoice::Bass:
	{
		// A thump: the pitch falls from 145 Hz to 55 Hz (the phase is the integral of the falling frequency).
		const float Phase = kMetronomeTwoPi * (55.f * T + 3.f * (1.f - FMath::Exp(-30.f * T)));
		return 0.8f * FMath::Exp(-T * 11.f) * FMath::Sin(Phase);
	}
	case EVoice::Snare:
	{
		V.Noise ^= V.Noise << 13;
		V.Noise ^= V.Noise >> 17;
		V.Noise ^= V.Noise << 5;
		const float Noise = static_cast<float>(V.Noise & 0xFFFFu) / 32767.5f - 1.f;
		return 0.38f * FMath::Exp(-T * 26.f) * Noise + 0.3f * FMath::Exp(-T * 40.f) * FMath::Sin(kMetronomeTwoPi * 190.f * T);
	}
	case EVoice::Chime:
		return 0.22f * FMath::Exp(-T * 8.f) * (FMath::Sin(kMetronomeTwoPi * 1320.f * T) + 0.5f * FMath::Sin(kMetronomeTwoPi * 1980.f * T));
	case EVoice::Good:
		return 0.22f * FMath::Exp(-T * 55.f) * FMath::Sin(kMetronomeTwoPi * 1760.f * T);
	case EVoice::Miss:
	{
		const float Saw = 2.f * FMath::Frac(98.f * T) - 1.f;
		return 0.3f * FMath::Exp(-T * 13.f) * Saw;
	}
	default:
		return 0.f;
	}
}

int32 UFTBeatBossMetronome::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	// The buffer keeps its size from one call to the next, so this allocates once.
	OutAudio.SetNumUninitialized(NumSamples * static_cast<int32>(sizeof(int16)), EAllowShrinking::No);
	int16* Out = reinterpret_cast<int16*>(OutAudio.GetData());

	// New triggers take a free voice, or the oldest one.
	const uint32 Until = Written.load(std::memory_order_acquire);
	for (; Taken != Until; ++Taken)
	{
		const int32 Kind = static_cast<int32>(Ring[Taken % RingSize]);
		if (Kind < 0 || Kind >= kMetronomeVoiceKinds)
		{
			continue;
		}
		int32 Slot = 0;
		for (int32 I = 0; I < MaxVoices; ++I)
		{
			if (Voices[I].Kind < 0)
			{
				Slot = I;
				break;
			}
			if (Voices[I].Age > Voices[Slot].Age)
			{
				Slot = I;
			}
		}
		Voices[Slot].Kind = Kind;
		Voices[Slot].Age = 0;
	}

	for (int32 S = 0; S < NumSamples; ++S)
	{
		float Mix = 0.f;
		for (FVoiceState& V : Voices)
		{
			if (V.Kind < 0)
			{
				continue;
			}
			Mix += Sample(V);
			if (++V.Age >= static_cast<int32>(kMetronomeLength[V.Kind] * static_cast<float>(Rate)))
			{
				V.Kind = -1;
			}
		}
		Out[S] = static_cast<int16>(FMath::Clamp(Mix, -1.f, 1.f) * 30000.f);
	}
	return NumSamples;
}
