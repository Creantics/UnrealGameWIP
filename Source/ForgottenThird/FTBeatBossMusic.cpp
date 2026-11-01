#include "FTBeatBossMusic.h"

#include "HAL/PlatformTime.h"

UFTBeatBossMusic::UFTBeatBossMusic(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	SetSampleRate(Rate);
	NumChannels = 1;
	Duration = INDEFINITELY_LOOPING_DURATION;
	bLooping = false;
	SoundGroup = SOUNDGROUP_Default;
	bCanProcessAsync = true;
	Synth.init(Rate);
}

uint64 UFTBeatBossMusic::FrameOf(double Time) const
{
	const double Frame = (Time + Offset) * static_cast<double>(Rate);
	return Frame <= 0.0 ? 0 : static_cast<uint64>(Frame + 0.5);
}

bool UFTBeatBossMusic::Sync(double ClockNow)
{
	const double Wall = FPlatformTime::Seconds();
	bool bResync = false;
	if (!IsRunning())
	{
		// Nothing can be placed before the stream runs; try again next frame.
		bAnchored = false;
		LastClock = ClockNow;
		LastWall = Wall;
		return false;
	}
	if (!bAnchored)
	{
		bResync = true;
	}
	else if (FMath::Abs((Wall - LastWall) - (ClockNow - LastClock)) > 0.05)
	{
		// The clock stood still (or jumped) while the wall clock ran.
		bResync = true;
	}
	LastClock = ClockNow;
	LastWall = Wall;
	if (bResync)
	{
		const double Ahead = static_cast<double>(Rendered.load(std::memory_order_acquire)) / static_cast<double>(Rate) - ClockNow;
		Anchor = Ahead + Margin;
		Offset = Anchor;
		bAnchored = true;
	}
	return bResync;
}

void UFTBeatBossMusic::Push(const FCommand& Command)
{
	const uint32 W = Written.load(std::memory_order_relaxed);
	if (W - Taken.load(std::memory_order_acquire) >= RingSize)
	{
		Dropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	Ring[W % RingSize] = Command;
	Written.store(W + 1, std::memory_order_release);
}

void UFTBeatBossMusic::Post(const ftcore::MusicEvents& Events, double ClockNow)
{
	if (!bAnchored)
	{
		return;
	}
	for (const ftcore::MusicEvent& E : Events)
	{
		FCommand C;
		if (E.kind == ftcore::MusicEventKind::Revoke && E.time < -1e17)
		{
			C.Frame = 0; // everything
		}
		else
		{
			C.Frame = FrameOf(E.time);
			// A sound for a time still ahead that is already behind the stream: the offset was too tight.
			if (E.kind == ftcore::MusicEventKind::Play && E.time > ClockNow + 0.002)
			{
				const uint64 Behind = Rendered.load(std::memory_order_acquire);
				if (C.Frame < Behind && Offset - Anchor < MaxBump)
				{
					Offset += static_cast<double>(Behind - C.Frame) / static_cast<double>(Rate);
					C.Frame = FrameOf(E.time);
				}
			}
		}
		C.Kind = E.kind == ftcore::MusicEventKind::Play ? ECommand::Play
			: E.kind == ftcore::MusicEventKind::Cut     ? ECommand::Cut
														 : ECommand::Revoke;
		C.Sound = static_cast<uint8>(E.sound);
		C.Velocity = E.velocity;
		C.Note = E.note;
		C.Aux = E.aux;
		C.Length = static_cast<uint32>(FMath::Max(0.0, E.length) * static_cast<double>(Rate) + 0.5);
		Push(C);
	}
}

void UFTBeatBossMusic::Trigger(ftcore::MusicSound Sound, uint8 Velocity)
{
	FCommand C;
	C.Kind = ECommand::Play;
	C.Frame = Rendered.load(std::memory_order_acquire); // due already: the next frame the audio thread makes
	C.Sound = static_cast<uint8>(Sound);
	C.Velocity = Velocity;
	Push(C);
}

int32 UFTBeatBossMusic::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	// The buffer keeps its size from one call to the next, so this allocates once.
	OutAudio.SetNumUninitialized(NumSamples * static_cast<int32>(sizeof(int16)), EAllowShrinking::No);
	int16* Out = reinterpret_cast<int16*>(OutAudio.GetData());
	if (Scratch.Num() < NumSamples)
	{
		Scratch.SetNumUninitialized(NumSamples);
	}

	const uint32 Until = Written.load(std::memory_order_acquire);
	uint32 Next = Taken.load(std::memory_order_relaxed);
	for (; Next != Until; ++Next)
	{
		const FCommand& C = Ring[Next % RingSize];
		switch (C.Kind)
		{
		case ECommand::Play:
			Synth.play(C.Frame, static_cast<ftcore::MusicSound>(C.Sound), C.Velocity, C.Note, C.Aux, C.Length);
			break;
		case ECommand::Revoke:
			Synth.revokeFrom(C.Frame);
			break;
		case ECommand::Cut:
			Synth.cut(C.Frame);
			break;
		}
	}
	Taken.store(Next, std::memory_order_release);

	Synth.render(Scratch.GetData(), NumSamples);
	for (int32 S = 0; S < NumSamples; ++S)
	{
		Out[S] = static_cast<int16>(FMath::Clamp(Scratch[S], -1.f, 1.f) * 32000.f);
	}
	Late.store(Synth.late(), std::memory_order_relaxed);
	Rendered.store(Synth.position(), std::memory_order_release);
	return NumSamples;
}
