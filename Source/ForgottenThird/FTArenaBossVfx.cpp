#include "FTArenaBossVfx.h"

#include "Engine/World.h"
#include "FTText.h"
#include "FTVfxComponent.h"
#include "ForgottenThird.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

#include "ftcore/Json.h"

#include <string>
#include <string_view>

namespace
{
constexpr int32 SlotCount[FFTArenaBossVfx::EffectCount] = {FFTArenaBossVfx::MaxTellMarks, 2, 3, 2, 1};
static_assert(SlotCount[0] + SlotCount[1] + SlotCount[2] + SlotCount[3] + SlotCount[4] == FFTArenaBossVfx::MaxSlots,
	"Slots[12] in FTArenaBossVfx.h must hold every effect's slots");
static_assert(FFTArenaBossVfx::MaxSlots == 12, "Slots[12] in FTArenaBossVfx.h must follow MaxSlots");

// The json's key for each effect, and its default system.
constexpr const char* kEffectKeys[FFTArenaBossVfx::EffectCount] = {"tell", "impact", "chips", "crit", "glow"};
constexpr const TCHAR* kEffectDefaults[FFTArenaBossVfx::EffectCount] = {
	TEXT("stone_tell"), TEXT("stone_impact"), TEXT("stone_chips"), TEXT("crit_burst"), TEXT("weak_glow")};

float ReadFloat(const ftcore::JsonValue& V, const char* Key, float Fallback)
{
	return static_cast<float>(V[Key].number(static_cast<double>(Fallback)));
}
} // namespace

int32 FFTArenaBossVfx::FirstSlot(EEffect Effect)
{
	int32 First = 0;
	for (int32 E = 0; E < static_cast<int32>(Effect); ++E)
	{
		First += SlotCount[E];
	}
	return First;
}

void FFTArenaBossVfx::ReadTuning(const FString& File)
{
	Tuning = FTuning();
	for (int32 E = 0; E < EffectCount; ++E)
	{
		Tuning.Looks[E].Effect = FName(kEffectDefaults[E]);
	}
	TArray<uint8> Bytes;
	const FString Path = FPaths::ProjectDir() / File;
	if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena VFX: couldn't read %s; the effects use their defaults."), *File);
		return;
	}
	ftcore::JsonDocument Doc;
	std::string Error;
	if (!Doc.parse(FT::BytesAsText(Bytes), &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena VFX: %s refused (%s); the effects use their defaults."), *File, *FT::ToFString(Error));
		return;
	}
	const ftcore::JsonValue Root = Doc.root();
	const ftcore::JsonValue Effects = Root["effects"];
	for (int32 E = 0; E < EffectCount; ++E)
	{
		const ftcore::JsonValue V = Effects[kEffectKeys[E]];
		FLook& Look = Tuning.Looks[E];
		const std::string_view Name = V["effect"].string();
		if (!Name.empty())
		{
			Look.Effect = FName(*FT::ToFString(Name));
		}
		Look.SpeedScale = ReadFloat(V, "speedScale", Look.SpeedScale);
		Look.ShapeScale = ReadFloat(V, "shapeScale", Look.ShapeScale);
	}
	Tuning.Lift = ReadFloat(Root, "lift", Tuning.Lift);
	Tuning.CalledAhead = ReadFloat(Root, "calledAhead", Tuning.CalledAhead);
	Tuning.CalledSide = ReadFloat(Root, "calledSide", Tuning.CalledSide);
	Tuning.ReachShare = FMath::Clamp(ReadFloat(Root, "reachShare", Tuning.ReachShare), 0.f, 1.f);
	Tuning.LaneMarks = FMath::Clamp(static_cast<int32>(Root["laneMarks"].integer(Tuning.LaneMarks)), 1, MaxTellMarks);
	Tuning.AreaScale = FMath::Max(ReadFloat(Root, "areaScale", Tuning.AreaScale), 0.f);
	Tuning.Surface = FMath::Max(ReadFloat(Root, "surface", Tuning.Surface), 0.f);
	Tuning.WeakDepth = FMath::Max(ReadFloat(Root, "weakDepth", Tuning.WeakDepth), 0.f);
	Tuning.FlashUp = FMath::Clamp(ReadFloat(Root, "flashUp", Tuning.FlashUp), -2.f, 2.f);
	Tuning.FlashToCamera = FMath::Clamp(ReadFloat(Root, "flashToCamera", Tuning.FlashToCamera), 0.f, 3.f);
}

void FFTArenaBossVfx::Spawn(UWorld* World, const FString& File)
{
	if (bSpawned || !World)
	{
		return;
	}
	ReadTuning(File);
	// The OFF half of an OFF/ON capture: the fight runs the same, nothing plays.
	if (FParse::Param(FCommandLine::Get(), TEXT("ArenaNoBossFx")))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: boss fx off (-ArenaNoBossFx)."));
		return;
	}
	bSpawned = true;
	for (int32 E = 0; E < EffectCount; ++E)
	{
		const FLook& Look = Tuning.Looks[E];
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UFTVfxComponent::SystemPath(Look.Effect), nullptr, LOAD_NoWarn | LOAD_Quiet);
		Next[E] = 0;
		bOn[E] = System != nullptr;
		if (!System)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena VFX: no %s; that effect is off (run FTBuildVfx -Only=%s)."), *UFTVfxComponent::SystemPath(Look.Effect),
				*Look.Effect.ToString());
			continue;
		}
		const int32 First = FirstSlot(static_cast<EEffect>(E));
		for (int32 S = First; S < First + SlotCount[E]; ++S)
		{
			// Deferred, so the component neither plays on BeginPlay nor looks its system up by name.
			AFTVfxActor* Actor = World->SpawnActorDeferred<AFTVfxActor>(AFTVfxActor::StaticClass(), FTransform::Identity, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Actor)
			{
				continue;
			}
			UFTVfxComponent* Vfx = Actor->GetVfx();
			Vfx->bPlayOnBeginPlay = false;
			Vfx->Effect = Look.Effect;
			Vfx->System = System;
			Vfx->SpeedScale = Look.SpeedScale;
			Vfx->ShapeScale = Look.ShapeScale;
			Actor->FinishSpawning(FTransform::Identity);
			Slots[S] = Actor;
		}
	}
	bTells = bGlowing = false;
}

void FFTArenaBossVfx::Destroy()
{
	for (TObjectPtr<AFTVfxActor>& Slot : Slots)
	{
		if (Slot)
		{
			Slot->Destroy();
			Slot = nullptr;
		}
	}
	bSpawned = bTells = bGlowing = false;
}

void FFTArenaBossVfx::StopAll()
{
	for (const TObjectPtr<AFTVfxActor>& Slot : Slots)
	{
		if (Slot)
		{
			Slot->GetVfx()->Stop();
		}
	}
	bTells = bGlowing = false;
}

AFTVfxActor* FFTArenaBossVfx::NextSlot(EEffect Effect)
{
	const int32 E = static_cast<int32>(Effect);
	if (!bSpawned || !bOn[E])
	{
		return nullptr;
	}
	AFTVfxActor* Actor = Slots[FirstSlot(Effect) + Next[E]].Get();
	Next[E] = (Next[E] + 1) % SlotCount[E];
	return Actor;
}

bool FFTArenaBossVfx::Play(EEffect Effect, const FVector& At, float ShapeMul, const TCHAR* Why)
{
	AFTVfxActor* Actor = NextSlot(Effect);
	if (!Actor)
	{
		return false;
	}
	const FLook& Look = Tuning.Looks[static_cast<int32>(Effect)];
	UFTVfxComponent* Vfx = Actor->GetVfx();
	Actor->SetActorLocation(At);
	Vfx->SetParameters(Look.SpeedScale, Look.ShapeScale * ShapeMul);
	Vfx->Play();
	bTells = bTells || Effect == EEffect::Tell;
	if (Why)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena VFX: %s (%s) at %.0f %.0f %.0f"), *Look.Effect.ToString(), Why, At.X, At.Y, At.Z);
	}
	return true;
}

void FFTArenaBossVfx::StopTells()
{
	const int32 First = FirstSlot(EEffect::Tell);
	for (int32 S = First; S < First + SlotCount[static_cast<int32>(EEffect::Tell)]; ++S)
	{
		if (Slots[S])
		{
			Slots[S]->GetVfx()->Stop();
		}
	}
	bTells = false;
}

void FFTArenaBossVfx::SetGlow(bool bOnNow, const FVector& At)
{
	if (bOnNow == bGlowing)
	{
		return;
	}
	if (bOnNow)
	{
		bGlowing = Play(EEffect::Glow, At, 1.f, TEXT("weak point open"));
		return;
	}
	AFTVfxActor* Actor = Slots[FirstSlot(EEffect::Glow)].Get();
	if (Actor)
	{
		// Stop alone lets the live particles finish (0.3 to 1.1 s), so the glow would outlast the weak point by frames.
		UFTVfxComponent* Vfx = Actor->GetVfx();
		Vfx->Stop();
		if (UNiagaraComponent* Niagara = Vfx->GetNiagara())
		{
			Niagara->DeactivateImmediate();
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Arena VFX: %s off (weak point shut)"), *Tuning.Looks[static_cast<int32>(EEffect::Glow)].Effect.ToString());
	}
	bGlowing = false;
}

void FFTArenaBossVfx::MoveGlow(const FVector& At)
{
	AFTVfxActor* Actor = bGlowing ? Slots[FirstSlot(EEffect::Glow)].Get() : nullptr;
	if (Actor)
	{
		Actor->SetActorLocation(At);
	}
}
