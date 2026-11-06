#include "FTWorldEyeShotSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "ForgottenThird.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ShaderCompiler.h"

#include "ftcore/MassingPlan.h"

namespace
{
// How long the saved still may take to reach the disk, in ticks.
constexpr int32 kPendingTicks = 6000;

// "x,y,z" as three numbers, or false.
bool ParseTriple(const TCHAR* Key, double& X, double& Y, double& Z)
{
	FString Text;
	if (!FParse::Value(FCommandLine::Get(), Key, Text, false))
	{
		return false;
	}
	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT(","));
	if (Parts.Num() != 3 || !Parts[0].IsNumeric() || !Parts[1].IsNumeric() || !Parts[2].IsNumeric())
	{
		return false;
	}
	X = FCString::Atod(*Parts[0]);
	Y = FCString::Atod(*Parts[1]);
	Z = FCString::Atod(*Parts[2]);
	return true;
}
} // namespace

bool UFTWorldEyeShotSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	FString Shot;
	return World && World->WorldType == EWorldType::Game && FParse::Value(FCommandLine::Get(), TEXT("WorldEyeShot="), Shot) &&
		!Shot.IsEmpty() && Super::ShouldCreateSubsystem(Outer);
}

void UFTWorldEyeShotSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	FParse::Value(FCommandLine::Get(), TEXT("WorldEyeShot="), File);
	double EyeX = 0.0, EyeY = 0.0, EyeZ = 0.0, AtX = 0.0, AtY = 0.0, AtZ = 0.0;
	if (!ParseTriple(TEXT("WorldEye="), EyeX, EyeY, EyeZ) || !ParseTriple(TEXT("WorldEyeAt="), AtX, AtY, AtZ))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: -WorldEye=<x,y,z> and -WorldEyeAt=<x,y,z> are both needed."));
		Finish(1);
		return;
	}
	double Side = 1000.0;
	double Lens = 35.0;
	FParse::Value(FCommandLine::Get(), TEXT("WorldEyeSide="), Side);
	FParse::Value(FCommandLine::Get(), TEXT("WorldEyeLens="), Lens);
	FParse::Value(FCommandLine::Get(), TEXT("WorldEyeDelay="), Delay);
	if (!FParse::Value(FCommandLine::Get(), TEXT("WorldEyeSize="), Size) || Size.IsEmpty())
	{
		Size = TEXT("2560x1440");
	}
	if (Lens <= 0.0 || Side <= 0.0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: -WorldEyeLens and -WorldEyeSide must be above 0."));
		Finish(1);
		return;
	}
	const ftcore::MassingVec EyeM = ftcore::massingEnginePoint(Side, EyeX, EyeY, EyeZ);
	const ftcore::MassingVec AtM = ftcore::massingEnginePoint(Side, AtX, AtY, AtZ);
	const FVector From(EyeM.x * 100.0, EyeM.y * 100.0, EyeM.z * 100.0);
	const FVector To(AtM.x * 100.0, AtM.y * 100.0, AtM.z * 100.0);
	const FRotator Facing = (To - From).Rotation();
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Eye = InWorld.SpawnActor<ACameraActor>(From, Facing, Spawn);
	if (!Eye)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: the eye's camera could not be spawned."));
		Finish(1);
		return;
	}
	// Horizontal field of view for the lens on a 36 mm-wide frame.
	const double Fov = FMath::RadiansToDegrees(2.0 * FMath::Atan(18.0 / Lens));
	UCameraComponent* Camera = Eye->GetCameraComponent();
	Camera->SetFieldOfView(static_cast<float>(Fov));
	Camera->SetConstraintAspectRatio(false);
	// The same name again only proves the new still landed once the old one is gone.
	IFileManager::Get().Delete(*File, false, true, true);
	bActive = true;
	UE_LOG(LogForgottenThird, Display,
		TEXT("WorldEyeShot: eye (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f) in the plan frame, %.1f mm (%.1f degrees across), "
			 "engine eye %s, yaw %.2f, pitch %.2f; %s to %s after %.1f s."),
		EyeX, EyeY, EyeZ, AtX, AtY, AtZ, Lens, Fov, *From.ToString(), Facing.Yaw, Facing.Pitch, *Size, *File, Delay);
}

void UFTWorldEyeShotSubsystem::Tick(float DeltaTime)
{
	if (!bActive)
	{
		return;
	}
	UWorld* World = GetWorld();
	APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
	if (Player && Eye)
	{
		// Anything that sets the view (the lead's own camera on possession) is overruled until the still is saved.
		if (Player->GetViewTarget() != Eye)
		{
			Player->SetViewTarget(Eye);
		}
		// The HUD's canvas would land in the still; the look is what is being judged.
		if (AHUD* Hud = Player->GetHUD())
		{
			Hud->bShowHUD = false;
		}
	}
	if (bShot)
	{
		if (IFileManager::Get().FileExists(*File))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: %s saved; quitting."), *File);
			Finish(0);
		}
		else if (++PendingTicks >= kPendingTicks)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: %s never arrived."), *File);
			Finish(1);
		}
		return;
	}
	// A material still compiling draws as the engine's default: the wait starts over until every shader is in.
	if (GShaderCompilingManager && GShaderCompilingManager->GetNumRemainingJobs() > 0)
	{
		Waited = 0.f;
		return;
	}
	Waited += DeltaTime;
	if (Waited < Delay || !Player)
	{
		return;
	}
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
	if (!Viewport)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("WorldEyeShot: no game viewport to shoot from."));
		Finish(1);
		return;
	}
	Viewport->Exec(World, *FString::Printf(TEXT("HighResShot %s filename=\"%s\""), *Size, *File), *GLog);
	UE_LOG(LogForgottenThird, Display, TEXT("WorldEyeShot: shooting after %.1f s settled."), Waited);
	bShot = true;
}

TStatId UFTWorldEyeShotSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTWorldEyeShotSubsystem, STATGROUP_Tickables);
}

void UFTWorldEyeShotSubsystem::Finish(int32 Code)
{
	bActive = false;
	FTRequestExit(Code, TEXT("WorldEyeShot"));
}
