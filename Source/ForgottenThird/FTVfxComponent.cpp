#include "FTVfxComponent.h"

#include "ForgottenThird.h"

#include "Engine/World.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstanceController.h"
#include "TimerManager.h"

#include "ftcore/Vfx.h"

namespace
{
// Resolved once: SetVariable* takes the user parameter's name without the "User." prefix.
const FName& SpeedScaleName()
{
	static const FName Name(UTF8_TO_TCHAR(ftcore::kVfxUserSpeedScale));
	return Name;
}

const FName& ShapeScaleName()
{
	static const FName Name(UTF8_TO_TCHAR(ftcore::kVfxUserShapeScale));
	return Name;
}
} // namespace

UFTVfxComponent::UFTVfxComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

FString UFTVfxComponent::SystemPath(FName InEffect)
{
	const FString Name = TEXT("NS_") + InEffect.ToString();
	return FString::Printf(TEXT("/Game/VFX/%s.%s"), *Name, *Name);
}

void UFTVfxComponent::BeginPlay()
{
	Super::BeginPlay();
	if (bPlayOnBeginPlay)
	{
		Play();
	}
}

void UFTVfxComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReplayTimer);
	}
	Super::EndPlay(Reason);
}

bool UFTVfxComponent::MakeNiagara()
{
	if (!System && !Effect.IsNone())
	{
		System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath(Effect));
	}
	if (!System)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("UFTVfxComponent: no system for effect \"%s\"."), *Effect.ToString());
		return false;
	}
	if (!Niagara)
	{
		Niagara = NewObject<UNiagaraComponent>(GetOwner() ? static_cast<UObject*>(GetOwner()) : this, NAME_None, RF_Transient);
		Niagara->SetAutoActivate(false);
		Niagara->SetupAttachment(this);
		Niagara->SetAsset(System);
		Niagara->RegisterComponent();
	}
	else if (Niagara->GetAsset() != System)
	{
		Niagara->SetAsset(System);
	}
	return true;
}

bool UFTVfxComponent::Prepare()
{
	if (!MakeNiagara())
	{
		return false;
	}
	if (System->IsReadyToRun() && GetWorld())
	{
		Niagara->InitializeSystem(); // its instance made: the pipeline states precache from here (OnInitialize)
	}
	return true;
}

bool UFTVfxComponent::Play()
{
	if (!MakeNiagara())
	{
		return false;
	}
	PushParameters();
	Niagara->Activate(true);

	UWorld* World = GetWorld();
	if (World && ReplayInterval > 0.f && !World->GetTimerManager().IsTimerActive(ReplayTimer))
	{
		World->GetTimerManager().SetTimer(ReplayTimer, this, &UFTVfxComponent::Replay, ReplayInterval, true);
	}
	return true;
}

void UFTVfxComponent::SetEmitterOn(FName InEmitter, bool bOn)
{
	if (!Niagara)
	{
		return;
	}
	Niagara->SetEmitterEnable(InEmitter, bOn); // the override: applied when the effect next starts
	if (const FNiagaraSystemInstanceControllerPtr& Controller = Niagara->GetSystemInstanceController(); Controller.IsValid())
	{
		Controller->SetEmitterEnable(InEmitter, bOn);
	}
}

void UFTVfxComponent::Stop()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReplayTimer);
	}
	if (Niagara)
	{
		Niagara->Deactivate();
	}
}

void UFTVfxComponent::Clear()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReplayTimer);
	}
	if (Niagara)
	{
		Niagara->DeactivateImmediate();
	}
}

void UFTVfxComponent::SetParameters(float InSpeedScale, float InShapeScale)
{
	SpeedScale = InSpeedScale;
	ShapeScale = InShapeScale;
	PushParameters();
}

void UFTVfxComponent::SetTimeDilation(float InDilation)
{
	TimeDilation = FMath::IsFinite(InDilation) ? FMath::Clamp(InDilation, 0.05f, 20.f) : 1.f;
	if (Niagara)
	{
		Niagara->SetCustomTimeDilation(TimeDilation);
	}
}

void UFTVfxComponent::PushParameters()
{
	ftcore::VfxUserParameters P;
	P.speedScale = SpeedScale;
	P.shapeScale = ShapeScale;
	P = ftcore::sanitizeVfxUserParameters(P);
	SpeedScale = P.speedScale;
	ShapeScale = P.shapeScale;
	if (Niagara)
	{
		Niagara->SetVariableFloat(SpeedScaleName(), P.speedScale);
		Niagara->SetVariableVec3(ShapeScaleName(), FVector(P.shapeScale));
		Niagara->SetCustomTimeDilation(TimeDilation);
	}
}

void UFTVfxComponent::Replay()
{
	if (Niagara)
	{
		Niagara->Activate(true);
	}
}

AFTVfxActor::AFTVfxActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Vfx = CreateDefaultSubobject<UFTVfxComponent>(TEXT("Vfx"));
	RootComponent = Vfx;
}
