#include "FTSpecialCine.h"

#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Info.h"
#include "NiagaraComponent.h"

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
	Actors.Reset();
	Effects.Reset();
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A || A == Performer || (Performer && A->IsAttachedTo(Performer)))
		{
			continue;
		}
		if (A->IsA<AController>() || A->IsA<APlayerCameraManager>() || A->IsA<AHUD>() || A->IsA<AInfo>())
		{
			continue;
		}
		Actors.Add({A, A->CustomTimeDilation});
		A->CustomTimeDilation = Factor;
		TInlineComponentArray<UNiagaraComponent*> Niagaras(A);
		for (UNiagaraComponent* N : Niagaras)
		{
			if (N)
			{
				Effects.Add({N, N->GetCustomTimeDilation()});
				N->SetCustomTimeDilation(N->GetCustomTimeDilation() * Factor);
			}
		}
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
			N->SetCustomTimeDilation(W.Dilation);
		}
	}
	Actors.Reset();
	Effects.Reset();
}
