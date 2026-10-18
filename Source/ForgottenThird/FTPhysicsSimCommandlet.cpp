#include "FTPhysicsSimCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "Animation/SkeletalMeshActor.h"
#include "AssetCompilingManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTPhysicsComponent.h"
#include "GameFramework/WorldSettings.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "UObject/Package.h"
#endif

#if WITH_EDITOR
namespace
{
const TCHAR* const kSimMap = TEXT("/Game/Maps/Demo");
const TCHAR* const kSimMesh = TEXT("/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head");

// Below this the body has fallen through the floor (whose top is z = 0).
constexpr double kSimFloorTolerance = -5.0;
// At the end every body must be slower than this, or asleep (cm/s).
constexpr double kSimRestSpeed = 5.0;

struct FSimProp
{
	FString Label;
	UPrimitiveComponent* Body = nullptr;
	FVector Start = FVector::ZeroVector;
	double MinZ = TNumericLimits<double>::Max();
	double MaxSpeed = 0.0;
};
}
#endif

UFTPhysicsSimCommandlet::UFTPhysicsSimCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTPhysicsSimCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	float Seconds = 4.f;
	FParse::Value(*Params, TEXT("Seconds="), Seconds);
	Seconds = FMath::Clamp(Seconds, 0.5f, 60.f);
	const bool bRagdoll = !FParse::Param(*Params, TEXT("NoRagdoll"));

	UPackage* Package = LoadPackage(nullptr, kSimMap, LOAD_None);
	UWorld* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPhysicsSim: couldn't load %s; run FTBuildDemoWorld first."), kSimMap);
		return 1;
	}

	// The map as a game world with a physics scene, like play would make it.
	World->WorldType = EWorldType::Game;
	World->AddToRoot();
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	World->InitWorld(UWorld::InitializationValues()
						 .CreatePhysicsScene(true)
						 .ShouldSimulatePhysics(true)
						 .AllowAudioPlayback(false)
						 .RequiresHitProxies(false)
						 .CreateNavigation(false)
						 .CreateAISystem(false)
						 .SetTransactional(false));
	// Meshes compile asynchronously in a commandlet; a mesh without its body setup yet would register without a
	// physics body (the floor included), so finish compiling first and rebuild any body that missed it.
	FAssetCompilingManager::Get().FinishAllCompilation();
	World->UpdateWorldComponents(true, false);
	FAssetCompilingManager::Get().FinishAllCompilation();
	int32 Rebuilt = 0;
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		UStaticMeshComponent* Mesh = It->GetStaticMeshComponent();
		if (Mesh && Mesh->IsRegistered() && !Mesh->GetBodyInstance()->IsValidBodyInstance())
		{
			Mesh->RecreatePhysicsState();
			++Rebuilt;
		}
	}
	World->InitializeActorsForPlay(FURL());
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		const UStaticMeshComponent* Mesh = It->GetStaticMeshComponent();
		if (!Mesh->GetBodyInstance()->IsValidBodyInstance())
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPhysicsSim: %s has no physics body."), *It->GetActorLabel());
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTPhysicsSim: rebuilt %d mesh bodies after compiling."), Rebuilt);

	TArray<FSimProp> Props;
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		const FString Label = It->GetActorLabel();
		if (Label.StartsWith(TEXT("Phys_")))
		{
			FSimProp& Prop = Props.AddDefaulted_GetRef();
			Prop.Label = Label;
			Prop.Body = It->GetStaticMeshComponent();
			Prop.Start = Prop.Body->GetComponentLocation();
		}
	}
	if (Props.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPhysicsSim: no Phys_* props in %s (built with -NoPhysics?)."), kSimMap);
	}

	// The ragdoll: the proof head, standing in its reference pose away from everything else.
	UFTPhysicsComponent* Ragdoll = nullptr;
	if (bRagdoll)
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, kSimMesh);
		const FString AssetPackage = UFTPhysicsComponent::RagdollPackage(TEXT("proof_head"));
		UPhysicsAsset* Asset = LoadObject<UPhysicsAsset>(nullptr, *(AssetPackage + TEXT(".") + FPackageName::GetShortName(AssetPackage)));
		if (!Mesh || !Asset)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPhysicsSim: the proof head or %s is missing; run FTBuildPhysics first."), *AssetPackage);
		}
		else
		{
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>(FVector(-500, -650, 2), FRotator::ZeroRotator, Spawn);
			Actor->GetSkeletalMeshComponent()->SetSkeletalMesh(Mesh);
			Ragdoll = NewObject<UFTPhysicsComponent>(Actor, TEXT("Physics"));
			Ragdoll->RagdollAsset = Asset;
			Ragdoll->Target = Actor->GetSkeletalMeshComponent();
			Ragdoll->RegisterComponent();
		}
	}

	World->GetWorldSettings()->NotifyBeginPlay();
	UE_LOG(LogForgottenThird, Display, TEXT("FTPhysicsSim: %s as a game world, %d props, begun play: %s."), kSimMap, Props.Num(),
		World->HasBegunPlay() ? TEXT("yes") : TEXT("no"));

	int32 Errors = Props.IsEmpty() ? 1 : 0;
	bool bKnockedOut = false;
	if (Ragdoll)
	{
		// A push back and up at the chest, as a heavy hit would give.
		bKnockedOut = Ragdoll->Knockout(FVector(-120.0, 30.0, 40.0), TEXT("spine_03"));
		UE_LOG(LogForgottenThird, Display, TEXT("FTPhysicsSim: knockout with profile %s: %s."), *Ragdoll->Profile.ToString(),
			bKnockedOut ? TEXT("ragdolling") : TEXT("refused"));
		if (!bKnockedOut)
		{
			++Errors;
		}
	}

	// The ball's bounces: count the times it turns from falling to rising, and the highest point after the first.
	UPrimitiveComponent* Ball = nullptr;
	for (const FSimProp& Prop : Props)
	{
		if (Prop.Label == TEXT("Phys_RubberBall"))
		{
			Ball = Prop.Body;
		}
	}
	int32 Bounces = 0;
	double BounceTop = 0.0;
	double LastVz = 0.0;

	const float Step = 1.f / 60.f;
	const int32 Frames = FMath::CeilToInt(Seconds / Step);
	bool bHitStopChecked = false;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		World->Tick(LEVELTICK_All, Step);
		++GFrameCounter;
		for (FSimProp& Prop : Props)
		{
			Prop.MinZ = FMath::Min(Prop.MinZ, Prop.Body->GetComponentLocation().Z);
			Prop.MaxSpeed = FMath::Max(Prop.MaxSpeed, Prop.Body->GetPhysicsLinearVelocity().Size());
		}
		if (Ball)
		{
			const double Vz = Ball->GetPhysicsLinearVelocity().Z;
			if (LastVz < -20.0 && Vz > 20.0)
			{
				++Bounces;
			}
			if (Bounces > 0)
			{
				BounceTop = FMath::Max(BounceTop, Ball->GetComponentLocation().Z);
			}
			LastVz = Vz;
		}
		// Half a second in, a 4-frame hit-stop on the ragdoll: it should report frozen on the next frame.
		if (Ragdoll && bKnockedOut && Frame == 30)
		{
			Ragdoll->HitStop(4);
		}
		if (Ragdoll && bKnockedOut && Frame == 31 && !bHitStopChecked)
		{
			bHitStopChecked = true;
			UE_LOG(LogForgottenThird, Display, TEXT("FTPhysicsSim: hit-stop at frame 31: %s."),
				Ragdoll->IsFrozen() ? TEXT("frozen") : TEXT("not frozen"));
		}
	}

	UE_LOG(LogForgottenThird, Display, TEXT("FTPhysicsSim: after %.2f s (%d frames at 60 Hz):"), Frames * Step, Frames);
	for (const FSimProp& Prop : Props)
	{
		const FVector End = Prop.Body->GetComponentLocation();
		const double Speed = Prop.Body->GetPhysicsLinearVelocity().Size();
		const FBodyInstance* Body = Prop.Body->GetBodyInstance();
		const bool bAwake = Body && Body->IsInstanceAwake();
		UE_LOG(LogForgottenThird, Display,
			TEXT("FTPhysicsSim:   %-16s start (%7.1f %7.1f %6.1f) rest (%7.1f %7.1f %6.1f) moved %6.1f cm, speed %5.1f cm/s (max %6.1f), %s, simulating %s, material %s."),
			*Prop.Label, Prop.Start.X, Prop.Start.Y, Prop.Start.Z, End.X, End.Y, End.Z, FVector::Dist(Prop.Start, End), Speed, Prop.MaxSpeed,
			bAwake ? TEXT("awake") : TEXT("asleep"), Prop.Body->IsSimulatingPhysics() ? TEXT("yes") : TEXT("no"),
			Body && Body->GetSimplePhysicalMaterial() ? *Body->GetSimplePhysicalMaterial()->GetName() : TEXT("none"));
		if (!Prop.Body->IsSimulatingPhysics())
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPhysicsSim: %s isn't simulating."), *Prop.Label);
			++Errors;
		}
		if (Prop.MinZ < kSimFloorTolerance)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPhysicsSim: %s fell through the floor (lowest z %.1f)."), *Prop.Label, Prop.MinZ);
			++Errors;
		}
		if (bAwake && Speed > kSimRestSpeed)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPhysicsSim: %s is still moving at %.1f cm/s."), *Prop.Label, Speed);
			++Errors;
		}
	}
	if (Ball)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTPhysicsSim:   rubber ball: %d bounces, highest after the first %.1f cm (dropped from %.1f)."), Bounces,
			BounceTop, 150.0);
	}

	if (Ragdoll && bKnockedOut)
	{
		const USkeletalMeshComponent* Skel = Cast<USkeletalMeshComponent>(Ragdoll->Target);
		double Lowest = TNumericLimits<double>::Max();
		double Fastest = 0.0;
		int32 Awake = 0;
		for (const FBodyInstance* Body : Skel->Bodies)
		{
			const FVector At = Body->GetUnrealWorldTransform().GetLocation();
			const double Speed = Body->GetUnrealWorldVelocity().Size();
			Lowest = FMath::Min(Lowest, At.Z);
			Fastest = FMath::Max(Fastest, Speed);
			Awake += Body->IsInstanceAwake() ? 1 : 0;
			UE_LOG(LogForgottenThird, Display, TEXT("FTPhysicsSim:   ragdoll %-12s at (%7.1f %7.1f %6.1f), %5.1f cm/s, %.1f kg."),
				*Skel->GetBoneName(Body->InstanceBoneIndex).ToString(), At.X, At.Y, At.Z, Speed, Body->GetBodyMass());
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTPhysicsSim:   ragdoll: %d bodies, %d awake, lowest z %.1f, fastest %.1f cm/s, settle assist fired %d time(s)."),
			Skel->Bodies.Num(), Awake, Lowest, Fastest, Ragdoll->GetSettleCount());
		if (Skel->Bodies.IsEmpty() || Lowest < kSimFloorTolerance - 10.0)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPhysicsSim: the ragdoll has no bodies or fell through the floor."));
			++Errors;
		}
		if (Awake > 0 && Fastest > kSimRestSpeed * 4.0)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPhysicsSim: the ragdoll hasn't settled (%.1f cm/s)."), Fastest);
			++Errors;
		}
	}

	World->EndPlay(EEndPlayReason::Quit);
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	World->RemoveFromRoot();

	UE_LOG(LogForgottenThird, Display, TEXT("FTPhysicsSim: %s."), Errors == 0 ? TEXT("passed") : TEXT("FAILED"));
	return Errors == 0 ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTPhysicsSim needs an editor build."));
	return 1;
#endif
}
