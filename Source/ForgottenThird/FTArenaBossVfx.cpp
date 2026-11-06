#include "FTArenaBossVfx.h"

#include "Components/RectLightComponent.h"
#include "Engine/Scene.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
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

#include <string>
#include <string_view>

namespace
{
constexpr int32 SlotCount[FFTArenaBossVfx::EffectCount] = {FFTArenaBossVfx::MaxTellMarks, 2, 3, 2, 1, 2, 1};
static_assert(SlotCount[0] + SlotCount[1] + SlotCount[2] + SlotCount[3] + SlotCount[4] + SlotCount[5] + SlotCount[6] ==
		FFTArenaBossVfx::MaxSlots,
	"Slots[15] in FTArenaBossVfx.h must hold every effect's slots");
static_assert(FFTArenaBossVfx::MaxSlots == 15, "Slots[15] in FTArenaBossVfx.h must follow MaxSlots");

// The json's key for each effect, and its default system.
constexpr const char* kEffectKeys[FFTArenaBossVfx::EffectCount] = {"tell", "impact", "chips", "crit", "glow", "chipsHeavy", "tellArea"};
constexpr const TCHAR* kEffectDefaults[FFTArenaBossVfx::EffectCount] = {TEXT("stone_tell"), TEXT("stone_impact"), TEXT("stone_chips"),
	TEXT("crit_burst"), TEXT("weak_glow"), TEXT("stone_chips_heavy"), TEXT("stone_tell_area")};

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
	const ftcore::JsonValue Light = Root["tellLight"];
	Tuning.LightUp = FMath::Clamp(ReadFloat(Light, "up", Tuning.LightUp), 0.05f, 3.f);
	Tuning.LightAcross = FMath::Clamp(ReadFloat(Light, "across", Tuning.LightAcross), 0.05f, 2.f);
	Tuning.LightCandelaPerMetre = FMath::Clamp(ReadFloat(Light, "candelaPerMetre", Tuning.LightCandelaPerMetre), 0.f, 1000.f);
	Tuning.LightKelvin = FMath::Clamp(ReadFloat(Light, "kelvin", Tuning.LightKelvin), 1700.f, 12000.f);
	Tuning.LightPast = FMath::Clamp(ReadFloat(Light, "past", Tuning.LightPast), 0.f, 5.f);
	Tuning.LightBarnDeg = FMath::Clamp(ReadFloat(Light, "barnDoorDeg", Tuning.LightBarnDeg), 0.f, 88.f);
	Tuning.LightBarnLength = FMath::Clamp(ReadFloat(Light, "barnDoorLength", Tuning.LightBarnLength), 0.01f, 1.f);
	// The ramps stay short: in over 2-3 frames, out over 1-2.
	Tuning.LightRampIn = FMath::Clamp(static_cast<int32>(Light["rampIn"].integer(Tuning.LightRampIn)), 2, 3);
	Tuning.LightRampOut = FMath::Clamp(static_cast<int32>(Light["rampOut"].integer(Tuning.LightRampOut)), 1, 2);
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
	SpawnLight(World);
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
	if (LightHost)
	{
		LightHost->Destroy();
	}
	LightHost = nullptr;
	TellLight = nullptr;
	LightLevel = LightGoal = LightFull = 0.f;
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
	LightOff(true, TEXT("everything stopped"));
	bTells = bGlowing = false;
}

void FFTArenaBossVfx::SpawnLight(UWorld* World)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Host = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Host)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena VFX: the tell light's actor didn't spawn; the marked move's light is off."));
		return;
	}
	URectLightComponent* Light = NewObject<URectLightComponent>(Host, TEXT("TellLight"));
	Light->SetMobility(EComponentMobility::Movable);
	Host->SetRootComponent(Light);
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetUseTemperature(true);
	Light->SetTemperature(Tuning.LightKelvin);
	Light->SetLightColor(FLinearColor::White);
	Light->SetCastShadows(false);
	Light->SetSourceWidth(100.f);
	Light->SetSourceHeight(Tuning.LightAcross * 100.f);
	// Barn doors keep the floor's pool a bar (a rect light alone spreads over its whole hemisphere).
	Light->SetBarnDoorAngle(FMath::Min(Tuning.LightBarnDeg, GetRectLightBarnDoorMaxAngle()));
	Light->SetBarnDoorLength(Tuning.LightBarnLength * 100.f);
	Light->SetAttenuationRadius(100.f);
	Light->SetIntensity(0.f);
	Light->SetVisibility(false);
	Light->RegisterComponent();
	Host->AddInstanceComponent(Light);
	LightHost = Host;
	TellLight = Light;
	LightLevel = LightGoal = LightFull = 0.f;
	LightStepIn = 1.f / static_cast<float>(Tuning.LightRampIn);
	LightStepOut = 1.f / static_cast<float>(Tuning.LightRampOut);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena VFX: tell light ready: %.0f K, %.2f m up, %.2f m across, %.1f cd/m, %.2f m past the lane, barn doors %.0f deg %.2f m, in %d frames, out %d (no shadows)."),
		Tuning.LightKelvin, Tuning.LightUp, Tuning.LightAcross, Tuning.LightCandelaPerMetre, Tuning.LightPast, Tuning.LightBarnDeg, Tuning.LightBarnLength,
		Tuning.LightRampIn, Tuning.LightRampOut);
}

void FFTArenaBossVfx::ApplyLight()
{
	TellLight->SetIntensity(LightFull * LightLevel);
	TellLight->SetVisibility(LightLevel > 0.f);
}

bool FFTArenaBossVfx::LightOn(const FVector& From, const FVector& To, const TCHAR* Why)
{
	if (!TellLight)
	{
		return false;
	}
	FVector Along = To - From;
	Along.Z = 0.0;
	const double Length = Along.Size();
	if (Length < 1.0)
	{
		return false;
	}
	const FVector Dir = Along / Length;
	// Emitting along its X (down), its width along its Y (the lane), its height along its Z (across the lane).
	const FVector Mid = (From + To) * 0.5;
	TellLight->SetWorldLocationAndRotation(FVector(Mid.X, Mid.Y, From.Z + static_cast<double>(Tuning.LightUp) * 100.0),
		FRotationMatrix::MakeFromXY(FVector::DownVector, Dir).Rotator());
	TellLight->SetSourceWidth(static_cast<float>(Length));
	TellLight->SetAttenuationRadius(static_cast<float>(Length * 0.5 + static_cast<double>(Tuning.LightPast) * 100.0));
	LightFull = Tuning.LightCandelaPerMetre * static_cast<float>(Length / 100.0);
	LightGoal = 1.f;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena VFX: tell light on (%s): %.2f m along the lane from (%.2f, %.2f) to (%.2f, %.2f) m, %.1f cd at full, in over %d frames."),
		Why ? Why : TEXT("marked"), Length / 100.0, From.X / 100.0, From.Y / 100.0, To.X / 100.0, To.Y / 100.0, LightFull, Tuning.LightRampIn);
	return true;
}

void FFTArenaBossVfx::LightOff(bool bAtOnce, const TCHAR* Why)
{
	if (!TellLight || (LightGoal <= 0.f && LightLevel <= 0.f))
	{
		return;
	}
	const float Was = LightLevel;
	LightGoal = 0.f;
	if (bAtOnce)
	{
		LightLevel = 0.f;
		ApplyLight();
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena VFX: tell light off (%s): %s from level %.2f."), Why ? Why : TEXT("off"),
		bAtOnce ? TEXT("at once") : TEXT("ramped"), Was);
}

void FFTArenaBossVfx::TickLight()
{
	if (!TellLight || LightLevel == LightGoal)
	{
		return;
	}
	LightLevel = LightGoal > LightLevel ? FMath::Min(LightGoal, LightLevel + LightStepIn) : FMath::Max(LightGoal, LightLevel - LightStepOut);
	// Exactly 1 or 0 at the ramp's last step (the steps are 1/n).
	if (FMath::IsNearlyEqual(LightLevel, LightGoal, 1e-4f))
	{
		LightLevel = LightGoal;
	}
	ApplyLight();
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
	bTells = bTells || Effect == EEffect::Tell || Effect == EEffect::TellArea;
	if (Why)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena VFX: %s (%s) at %.0f %.0f %.0f"), *Look.Effect.ToString(), Why, At.X, At.Y, At.Z);
	}
	return true;
}

void FFTArenaBossVfx::StopTells()
{
	for (const EEffect Tell : {EEffect::Tell, EEffect::TellArea})
	{
		const int32 First = FirstSlot(Tell);
		for (int32 S = First; S < First + SlotCount[static_cast<int32>(Tell)]; ++S)
		{
			if (Slots[S])
			{
				Slots[S]->GetVfx()->Stop();
			}
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
		// No new particles; the live ones finish their lives (weak_glow keeps them all at 0.27 s or under), so the glow
		// fades over 6 to 8 frames instead of vanishing on one.
		Actor->GetVfx()->Stop();
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
