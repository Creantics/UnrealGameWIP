#include "FTSpecialCine.h"

#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/AudioComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Info.h"
#include "NiagaraComponent.h"
#include "PhysicsEngine/BodyInstance.h"

UFTSpecialCineModifier::UFTSpecialCineModifier()
{
	Priority = 255; // after every other modifier: the shot is what is drawn
}

bool UFTSpecialCineModifier::ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
	In = InOutPOV;
	if (bOn)
	{
		const float W = FMath::Clamp(1.f - Shot.PlayWeight, 0.f, 1.f); // the shot's share
		InOutPOV.Location += Shot.PlayShiftCm;
		if (W > 0.f)
		{
			InOutPOV.Location = FMath::Lerp(InOutPOV.Location, Shot.Location + Shot.ShakeCm, static_cast<double>(W));
			const FQuat Blended = FQuat::Slerp(InOutPOV.Rotation.Quaternion(), Shot.Rotation.Quaternion(), static_cast<double>(W));
			InOutPOV.Rotation = Blended.Rotator();
			InOutPOV.Rotation.Roll = 0.0;
			InOutPOV.FOV = FMath::Lerp(InOutPOV.FOV, Shot.FovDegrees, W);
		}
		if (Shot.bCut && CameraOwner)
		{
			CameraOwner->SetGameCameraCutThisFrame();
			Shot.bCut = false; // once
		}
	}
	Out = InOutPOV;
	// The base applies the post process this modifier gives (ModifyPostProcess) as a cached blend.
	Super::ModifyCamera(DeltaTime, InOutPOV);
	return false;
}

void UFTSpecialCineModifier::ModifyPostProcess(float DeltaTime, float& PostProcessBlendWeight, FPostProcessSettings& PostProcessSettings)
{
	Super::ModifyPostProcess(DeltaTime, PostProcessBlendWeight, PostProcessSettings);
	if (!bOn)
	{
		return;
	}
	const float W = FMath::Clamp(1.f - Shot.PlayWeight, 0.f, 1.f);
	if (W <= 0.f)
	{
		return;
	}
	PostProcessBlendWeight = W;
	PostProcessSettings.bOverride_DepthOfFieldFocalDistance = true;
	PostProcessSettings.DepthOfFieldFocalDistance = FMath::Max(10.f, Shot.FocusCm);
	PostProcessSettings.bOverride_DepthOfFieldFstop = true;
	PostProcessSettings.DepthOfFieldFstop = Shot.FStop;
	PostProcessSettings.bOverride_DepthOfFieldSensorWidth = true;
	PostProcessSettings.DepthOfFieldSensorWidth = 36.f;
}

void FFTSpecialFreeze::Begin(UWorld* World, const AActor* Performer, float Factor)
{
	if (bOn || !World)
	{
		return;
	}
	bOn = true;
	HeldAt = Factor;
	HeldWorld = World;
	HeldPerformer = Performer;
	Spawned = 0;
	Actors.Reset();
	Effects.Reset();
	Sounds.Reset();
	BodiesHeld.Reset();
	Cloths.Reset();
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		Hold(*It);
	}
	SpawnHandle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateRaw(this, &FFTSpecialFreeze::OnSpawned));
}

void FFTSpecialFreeze::OnSpawned(AActor* A)
{
	const AActor* Performer = HeldPerformer.Get();
	if (!bOn || !A || (Performer && (A->GetOwner() == Performer || A->GetInstigator() == Performer)))
	{
		return; // the performer's own (a strike's effect, a thrown piece) play at its speed
	}
	const int32 Before = Actors.Num();
	Hold(A);
	Spawned += Actors.Num() > Before ? 1 : 0;
}

void FFTSpecialFreeze::Hold(AActor* A)
{
	const AActor* Performer = HeldPerformer.Get();
	if (!A || A == Performer || (Performer && A->IsAttachedTo(Performer)))
	{
		return;
	}
	if (A->IsA<AController>() || A->IsA<APlayerCameraManager>() || A->IsA<AHUD>() || A->IsA<AInfo>())
	{
		return;
	}
	Actors.Add({A, A->CustomTimeDilation});
	A->CustomTimeDilation = HeldAt;
	TInlineComponentArray<UNiagaraComponent*> Niagaras(A);
	for (UNiagaraComponent* N : Niagaras)
	{
		if (N)
		{
			// Solo: its own tick, which the owner's dilation scales; its own dilation stays as found.
			Effects.Add({N, N->GetForceSolo()});
			N->SetForceSolo(true);
		}
	}
	TInlineComponentArray<UAudioComponent*> Audios(A);
	for (UAudioComponent* S : Audios)
	{
		if (S && S->IsPlaying() && S->GetPlayState() != EAudioComponentPlayState::Paused)
		{
			Sounds.Add(S);
			S->SetPaused(true);
		}
	}
	auto HoldBody = [this](UPrimitiveComponent* C, int32 Index, FBodyInstance* B) {
		if (!B || !B->IsInstanceSimulatingPhysics())
		{
			return;
		}
		BodiesHeld.Add({C, Index, B->GetUnrealWorldVelocity(), B->GetUnrealWorldAngularVelocityInRadians(), static_cast<bool>(B->bEnableGravity)});
		B->SetEnableGravity(false);
		B->SetLinearVelocity(FVector::ZeroVector, false, false);
		B->SetAngularVelocityInRadians(FVector::ZeroVector, false, false);
		B->PutInstanceToSleep();
	};
	TInlineComponentArray<UPrimitiveComponent*> Prims(A);
	for (UPrimitiveComponent* C : Prims)
	{
		if (!C)
		{
			continue;
		}
		if (USkeletalMeshComponent* Skel = Cast<USkeletalMeshComponent>(C))
		{
			for (int32 I = 0; I < Skel->Bodies.Num(); ++I)
			{
				HoldBody(Skel, I, Skel->Bodies[I]);
			}
			if (!Skel->IsClothingSimulationSuspended())
			{
				Cloths.Add(Skel);
				Skel->SuspendClothingSimulation();
			}
			continue;
		}
		HoldBody(C, INDEX_NONE, C->GetBodyInstance());
	}
}

int32 FFTSpecialFreeze::CountChanged() const
{
	int32 Changed = 0;
	for (const FActorWas& W : Actors)
	{
		const AActor* A = W.Actor.Get();
		Changed += A && A->CustomTimeDilation != HeldAt ? 1 : 0;
	}
	return Changed;
}

void FFTSpecialFreeze::End()
{
	if (!bOn)
	{
		return;
	}
	bOn = false;
	if (UWorld* World = HeldWorld.Get())
	{
		World->RemoveOnActorSpawnedHandler(SpawnHandle);
	}
	SpawnHandle.Reset();
	for (const FActorWas& W : Actors)
	{
		if (AActor* A = W.Actor.Get())
		{
			A->CustomTimeDilation = W.Dilation;
		}
	}
	for (const FEffectWas& W : Effects)
	{
		if (UNiagaraComponent* N = W.Effect.Get())
		{
			N->SetForceSolo(W.bSolo);
		}
	}
	for (const TWeakObjectPtr<UAudioComponent>& W : Sounds)
	{
		if (UAudioComponent* S = W.Get())
		{
			S->SetPaused(false);
		}
	}
	for (const FBodyWas& W : BodiesHeld)
	{
		UPrimitiveComponent* C = W.Component.Get();
		USkeletalMeshComponent* Skel = Cast<USkeletalMeshComponent>(C);
		FBodyInstance* B = !C ? nullptr
			: W.Index == INDEX_NONE ? C->GetBodyInstance()
			: Skel && Skel->Bodies.IsValidIndex(W.Index) ? Skel->Bodies[W.Index] : nullptr;
		if (!B || !B->IsInstanceSimulatingPhysics())
		{
			continue;
		}
		B->SetEnableGravity(W.bGravity);
		B->WakeInstance();
		B->SetLinearVelocity(W.Linear, false);
		B->SetAngularVelocityInRadians(W.Angular, false);
	}
	for (const TWeakObjectPtr<USkeletalMeshComponent>& W : Cloths)
	{
		if (USkeletalMeshComponent* Skel = W.Get())
		{
			Skel->ResumeClothingSimulation();
		}
	}
	Actors.Reset();
	Effects.Reset();
	Sounds.Reset();
	BodiesHeld.Reset();
	Cloths.Reset();
	HeldWorld.Reset();
	HeldPerformer.Reset();
}
