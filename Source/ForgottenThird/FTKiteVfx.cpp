#include "FTKiteVfx.h"

#include "Components/SkeletalMeshComponent.h"
#include "FTArenaFighter.h"
#include "FTKiteHost.h"
#include "FTText.h"
#include "FTVfxComponent.h"
#include "ForgottenThird.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"

#include "ftcore/Json.h"

#include <string_view>

namespace
{
// The json's key for each effect, and its default system.
constexpr const char* kEffectKeys[] = {"tell", "open", "crit", "hit"};
constexpr const TCHAR* kEffectDefaults[] = {TEXT("kite_swoop_tell"), TEXT("kite_seam_open"), TEXT("kite_seam_crit"), TEXT("kite_hit")};

float ReadFloat(const ftcore::JsonValue& V, const char* Key, float Fallback)
{
	return static_cast<float>(V[Key].number(static_cast<double>(Fallback)));
}
} // namespace

void FFTKiteVfx::ReadTuning()
{
	static_assert(UE_ARRAY_COUNT(kEffectKeys) == EffectCount && UE_ARRAY_COUNT(kEffectDefaults) == EffectCount, "one key and default per effect");
	for (int32 E = 0; E < EffectCount; ++E)
	{
		Looks[E] = FLook();
		Looks[E].Effect = FName(kEffectDefaults[E]);
	}
	TArray<uint8> Bytes;
	const FString Path = FPaths::ProjectDir() / DataPath;
	if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx: couldn't read %s; the effects use their defaults."), DataPath);
		return;
	}
	ftcore::JsonDocument Doc;
	std::string Error;
	if (!Doc.parse(FT::BytesAsText(Bytes), &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx: %s refused (%s); the effects use their defaults."), DataPath, *FT::ToFString(Error));
		return;
	}
	const ftcore::JsonValue Root = Doc.root();
	const ftcore::JsonValue Effects = Root["effects"];
	for (int32 E = 0; E < EffectCount; ++E)
	{
		const ftcore::JsonValue V = Effects[kEffectKeys[E]];
		FLook& Look = Looks[E];
		const std::string_view Name = V["effect"].string();
		if (!Name.empty())
		{
			Look.Effect = FName(*FT::ToFString(Name));
		}
		Look.SpeedScale = ReadFloat(V, "speedScale", Look.SpeedScale);
		Look.ShapeScale = ReadFloat(V, "shapeScale", Look.ShapeScale);
	}
	const std::string_view Bone = Root["socket"].string();
	if (!Bone.empty())
	{
		Socket = FName(*FT::ToFString(Bone));
	}
	const std::string_view Move = Root["tellMove"].string();
	if (!Move.empty())
	{
		TellMove.assign(Move.data(), Move.size());
	}
	Lift = FMath::Max(ReadFloat(Root, "lift", Lift), 0.f);
}

void FFTKiteVfx::Spawn(AFTArenaFighter* Figure)
{
	if (bSpawned || !Figure)
	{
		return;
	}
	// The OFF half of an OFF/ON capture: the fight runs the same, nothing plays.
	if (FParse::Param(FCommandLine::Get(), TEXT("ArenaNoKiteFx")))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite fx off (-ArenaNoKiteFx)."));
		return;
	}
	ReadTuning();
	bSpawned = true;
	// The glow and the burst ride the seam; the ring and the puff are put where they play.
	Fx[Tell] = Make(Figure, Looks[Tell], NAME_None);
	Fx[Open] = Make(Figure, Looks[Open], Socket);
	Fx[Crit] = Make(Figure, Looks[Crit], Socket);
	Fx[Hit] = Make(Figure, Looks[Hit], NAME_None);
	for (const EEffect E : {Tell, Hit})
	{
		if (Fx[E].IsValid())
		{
			Fx[E]->SetUsingAbsoluteLocation(true);
		}
	}
	// The seam bone scales as the smile opens (face_down): the glow and the burst keep their own size (ShapeScale).
	for (const EEffect E : {Open, Crit})
	{
		if (Fx[E].IsValid())
		{
			Fx[E]->SetUsingAbsoluteScale(true);
		}
	}
	bTelling = bWasOpen = false;
	TellUntil = 0.0;
	CritsSeen = 0;
}

UFTVfxComponent* FFTKiteVfx::Make(AFTArenaFighter* Figure, const FLook& Look, FName Bone)
{
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UFTVfxComponent::SystemPath(Look.Effect), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!System)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx: no %s; that effect is off (run FTBuildVfx -Only=%s)."),
			*UFTVfxComponent::SystemPath(Look.Effect), *Look.Effect.ToString());
		return nullptr;
	}
	UFTVfxComponent* Vfx = NewObject<UFTVfxComponent>(Figure);
	Vfx->Effect = Look.Effect;
	Vfx->System = System;
	Vfx->SpeedScale = Look.SpeedScale;
	Vfx->ShapeScale = Look.ShapeScale;
	Vfx->bPlayOnBeginPlay = false;
	USkeletalMeshComponent* Skin = Figure->GetSkin();
	if (Skin)
	{
		if (!Bone.IsNone() && !Skin->DoesSocketExist(Bone))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx %s: the mesh has no bone or socket %s; it plays at the mesh's origin."),
				*Look.Effect.ToString(), *Bone.ToString());
		}
		Vfx->SetupAttachment(Skin, Bone);
	}
	else
	{
		Vfx->SetupAttachment(Figure->GetRootComponent());
	}
	Vfx->RegisterComponent();
	return Vfx;
}

void FFTKiteVfx::Play(EEffect Effect, double T)
{
	UFTVfxComponent* Vfx = Fx[Effect].Get();
	if (!Vfx)
	{
		return;
	}
	Vfx->Play();
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite fx %s at %.2f"), *Looks[Effect].Effect.ToString(), T);
}

void FFTKiteVfx::StopOne(EEffect Effect)
{
	if (UFTVfxComponent* Vfx = Fx[Effect].Get())
	{
		Vfx->Stop();
	}
}

void FFTKiteVfx::CutOne(EEffect Effect, double T)
{
	if (UFTVfxComponent* Vfx = Fx[Effect].Get())
	{
		Vfx->StopNow();
		if (T >= 0.0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite fx %s cut at %.2f"), *Looks[Effect].Effect.ToString(), T);
		}
	}
}

void FFTKiteVfx::FadeOne(EEffect Effect, double T)
{
	if (UFTVfxComponent* Vfx = Fx[Effect].Get())
	{
		Vfx->StopWithin(GlowFadeSeconds);
		if (T >= 0.0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite fx %s fades at %.2f"), *Looks[Effect].Effect.ToString(), T);
		}
	}
}

void FFTKiteVfx::Feed(const ftcore::ArenaEvents& Events, const FFTKiteHost& Host, const ftcore::ArenaFight& Fight, const FVector& KiteGround,
	const FVector& LeadChest)
{
	if (!bSpawned || !Host.IsActive())
	{
		return;
	}
	const double T = static_cast<double>(Fight.time());
	const size_t Slot = Host.GetSlot();
	const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[Slot];
	for (const ftcore::ArenaEvent& E : Events)
	{
		if (E.type == ftcore::ArenaEventType::AttackStarted && E.source == Slot && E.move < Kind.moveCount)
		{
			const ftcore::ArenaEnemyMove& Move = Kind.moves[E.move];
			if (Move.clip() == std::string_view(TellMove) && Fx[Tell].IsValid())
			{
				Fx[Tell]->SetWorldLocation(FVector(KiteGround.X, KiteGround.Y, KiteGround.Z + Lift * 100.f));
				Play(Tell, T);
				bTelling = true;
				TellUntil = T + static_cast<double>(Move.attack.startup + Move.attack.active);
			}
		}
		else if ((E.type == ftcore::ArenaEventType::Hit || E.type == ftcore::ArenaEventType::CounterStagger
					 || E.type == ftcore::ArenaEventType::ReadCounter)
			&& E.source == ftcore::kArenaPlayer && E.target == Slot)
		{
			// A lead blow (its own, or the partner's paired or Read counter). The blow hook counted a crit for it, or it
			// landed plain: the burst at the seam, or the puff where it struck.
			const int32 Crits = Host.GetCrits();
			const bool bCrit = Crits != CritsSeen;
			CritsSeen = Crits;
			if (bCrit)
			{
				Play(Crit, T);
			}
			else if (Fx[Hit].IsValid())
			{
				FVector Toward = LeadChest - KiteGround;
				Toward.Z = 0.0;
				const double Reach = FMath::Min(static_cast<double>(Kind.bodyRadius) * 100.0, Toward.Size());
				const FVector Edge = KiteGround + Toward.GetSafeNormal() * Reach;
				Fx[Hit]->SetWorldLocation(FVector(Edge.X, Edge.Y, LeadChest.Z));
				Play(Hit, T);
			}
		}
		else if (bTelling
			&& (((E.type == ftcore::ArenaEventType::Staggered || E.type == ftcore::ArenaEventType::Downed) && E.target == Slot)
				|| (E.type == ftcore::ArenaEventType::Parried && E.source == Slot)))
		{
			// The windup is broken off (the parry, a stagger, the kill): its ring goes with it, in this step, not after its
			// particles' lifetimes.
			CutOne(Tell, T);
			bTelling = false;
		}
	}
	if (bTelling && T >= TellUntil)
	{
		StopOne(Tell);
		bTelling = false;
	}
	// The fight is over, or the kite is down: nobody steps the fight again, so the window's own end never reaches the glow
	// (Host.IsOpen is a time, still true then). It ends here, in the step that ended the fight.
	if (Fight.over() || !Fight.enemyAlive(Slot))
	{
		EndOfFight(T);
		return;
	}
	// The window opens on a parry or a stagger and shuts itself: the glow follows it.
	const bool bOpen = Host.IsOpen();
	if (bOpen && !bWasOpen)
	{
		if (bTelling)
		{
			CutOne(Tell, T);
			bTelling = false;
		}
		Play(Open, T);
	}
	else if (!bOpen && bWasOpen)
	{
		StopOne(Open);
	}
	bWasOpen = bOpen;
}

void FFTKiteVfx::EndOfFight(double T)
{
	if (!bSpawned)
	{
		return;
	}
	// A no-op once done: Feed's last step and the runner's Finish both come here.
	if (bTelling)
	{
		CutOne(Tell, T);
		bTelling = false;
	}
	if (bWasOpen)
	{
		FadeOne(Open, T);
		bWasOpen = false;
	}
}

void FFTKiteVfx::ShowCue(const ftcore::ShowCueAt& Cue, const FVector& Ground, const FVector& HitAt)
{
	if (!bSpawned)
	{
		return;
	}
	const double T = Cue.at;
	switch (Cue.cue)
	{
	case ftcore::ShowCue::Tell:
		if (Fx[Tell].IsValid())
		{
			Fx[Tell]->SetWorldLocation(FVector(Ground.X, Ground.Y, Ground.Z + Lift * 100.f));
			Play(Tell, T);
			bTelling = true;
		}
		return;
	case ftcore::ShowCue::TellStop:
		if (bTelling)
		{
			if (Cue.place == ftcore::ShowPlace::Ahead)
			{
				CutOne(Tell, T);
			}
			else
			{
				StopOne(Tell);
			}
			bTelling = false;
		}
		return;
	case ftcore::ShowCue::Impact:
		return;
	case ftcore::ShowCue::Chips:
	case ftcore::ShowCue::ChipsHeavy: // the kite has one hit puff, for a light or a heavy blow
		if (Fx[Hit].IsValid())
		{
			Fx[Hit]->SetWorldLocation(HitAt);
			Play(Hit, T);
		}
		return;
	case ftcore::ShowCue::GlowOn:
		Play(Open, T);
		bWasOpen = true;
		return;
	case ftcore::ShowCue::GlowOff:
		// Finish's EndOfFight finds nothing left to do.
		if (bWasOpen)
		{
			FadeOne(Open, T);
			bWasOpen = false;
		}
		return;
	case ftcore::ShowCue::Crit:
		Play(Crit, T);
		return;
	}
}

void FFTKiteVfx::Stop()
{
	for (int32 E = 0; E < EffectCount; ++E)
	{
		CutOne(static_cast<EEffect>(E), -1.0);
	}
	bTelling = bWasOpen = false;
	TellUntil = 0.0;
	// FFTKiteHost::Begin counts crits from 0 again.
	CritsSeen = 0;
}

void FFTKiteVfx::Reset()
{
	Stop();
	for (TWeakObjectPtr<UFTVfxComponent>& Vfx : Fx)
	{
		Vfx.Reset();
	}
	bSpawned = false;
}
