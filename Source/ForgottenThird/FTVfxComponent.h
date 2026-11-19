#pragma once

// Plays one of the code-built particle effects (/Game/VFX/NS_<name>, made by FTBuildVfx from
// Data/Vfx/<name>.json) and fills its user parameters from ftcore::VfxUserParameters. See docs/VFX_UE.md.
//
// The Niagara component is created once, on the first Play. Parameters are pushed when the effect starts
// and when SetParameters is called, never per frame; replaying a one-shot effect runs on a timer. Nothing
// here allocates per frame.

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"

#include "FTVfxComponent.generated.h"

class UNiagaraComponent;
class UNiagaraSystem;

UCLASS(ClassGroup = (ForgottenThird), meta = (BlueprintSpawnableComponent))
class FORGOTTENTHIRD_API UFTVfxComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UFTVfxComponent();

	// The effect's name, as its Data/Vfx file is called ("heal_ring").
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VFX")
	FName Effect;

	// The built system. When empty, Play loads /Game/VFX/NS_<Effect>; set it to keep the asset in a cook.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VFX")
	TObjectPtr<UNiagaraSystem> System;

	// User.SpeedScale: multiplies every start velocity.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VFX", meta = (ClampMin = "0", ClampMax = "10"))
	float SpeedScale = 1.f;

	// User.ShapeScale: multiplies the spawn shape, on all three axes.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VFX", meta = (ClampMin = "0", ClampMax = "10"))
	float ShapeScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VFX")
	bool bPlayOnBeginPlay = true;

	// Restarts the effect every this many seconds (0: play once). For one-shot effects in a showcase.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VFX", meta = (ClampMin = "0"))
	float ReplayInterval = 0.f;

	// Starts (or restarts) the effect. False when the system can't be found.
	UFUNCTION(BlueprintCallable, Category = "VFX")
	bool Play();

	// Loads the system and makes the Niagara component with its system instance (once the system is ready to run), not
	// started: the effect's pipeline states begin compiling here, so a first Play later draws from its first frame (the
	// engine draws nothing of an effect whose pipeline states are still compiling). False when the system can't be found.
	bool Prepare();

	UFUNCTION(BlueprintCallable, Category = "VFX")
	void Stop();

	// Stops the effect and removes the particles already out (a wind-up cut short leaves nothing behind).
	void Clear();

	// Sets and pushes the user parameters (clamped by ftcore::sanitizeVfxUserParameters).
	UFUNCTION(BlueprintCallable, Category = "VFX")
	void SetParameters(float InSpeedScale, float InShapeScale);

	// The effect's own clock rate (Niagara custom time dilation; 1 by default, kept on later plays): a one-shot whose
	// lifetimes are built for one flight runs over another (a cast's bolt over its startup). Clamped to 0.05..20.
	void SetTimeDilation(float InDilation);

	UNiagaraComponent* GetNiagara() const { return Niagara; }

	// "/Game/VFX/NS_<Effect>.NS_<Effect>".
	static FString SystemPath(FName InEffect);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	void PushParameters();
	void Replay();

	float TimeDilation = 1.f;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> Niagara;

	// Loads the system and makes the Niagara component (not started). False when the system can't be found.
	bool MakeNiagara();

	FTimerHandle ReplayTimer;
};

// An actor that only holds a UFTVfxComponent, for placing effects in a map.
UCLASS()
class FORGOTTENTHIRD_API AFTVfxActor : public AActor
{
	GENERATED_BODY()

public:
	AFTVfxActor();

	UFTVfxComponent* GetVfx() const { return Vfx; }

private:
	UPROPERTY(VisibleAnywhere, Category = "VFX")
	TObjectPtr<UFTVfxComponent> Vfx;
};
