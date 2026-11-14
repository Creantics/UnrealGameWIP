#include "FTWorldWaveSubsystem.h"

#include "FTClockTower.h"
#include "FTExploreCharacter.h"
#include "FTSceneLook.h"
#include "FTWorldWave.h"
#include "ForgottenThird.h"

#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/GameViewportClient.h"
#include "Engine/HitResult.h"
#include "Engine/LocalPlayer.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "PhysicsEngine/BodySetup.h"
#include "SceneView.h"
#include "UnrealClient.h"
#include "WorldCollision.h"

namespace
{
// What the proof dressing takes from the first world.
enum class EPick : uint8
{
	None,
	Tree, // a /Game/Props/TREE- mesh (not the pit's grate)
	Lamp, // the TQ-04 street lamp
	Lens, // a lamp's night lens (the night lights' instanced mesh tagged FTScene::NightLampHeadTag)
	Car,  // the parked kei car
};
constexpr int32 kPickCount = 5;
const TCHAR* const kPickNames[kPickCount] = {TEXT("none"), TEXT("street trees"), TEXT("lamp posts"), TEXT("lamp lenses"), TEXT("kei cars")};

EPick PickOf(const UStaticMesh& Mesh)
{
	const FString Path = Mesh.GetPathName();
	if (Path.StartsWith(TEXT("/Game/Props/TREE-")) && !Path.Contains(TEXT("grate")))
	{
		return EPick::Tree;
	}
	if (Path.StartsWith(TEXT("/Game/Props/TQ-04/")))
	{
		return EPick::Lamp;
	}
	if (Path.StartsWith(TEXT("/Game/Vehicles/KeiCar/SM_KeiCar")))
	{
		return EPick::Car;
	}
	return EPick::None;
}

// The second world's own things: real meshes, bare trees and standing boards in turn, set in a ring round the point.
struct FArrival
{
	const TCHAR* Path;
	bool bTree;
	double RingM; // metres out from the point (capped at 0.9 of the dressing's radius)
};
constexpr FArrival kArrivals[] = {
	{TEXT("/Game/Props/TREE-broadleaf-a/TREE-broadleaf-a.TREE-broadleaf-a"), true, 9.0},
	{TEXT("/Game/Props/BKIT/SIGN_VERT_L/StaticMeshes/SIGN_VERT_L.SIGN_VERT_L"), false, 15.0},
	{TEXT("/Game/Props/TREE-park-a/TREE-park-a.TREE-park-a"), true, 21.0},
	{TEXT("/Game/Props/BKIT/SIGN_VERT_M/StaticMeshes/SIGN_VERT_M.SIGN_VERT_M"), false, 12.0},
	{TEXT("/Game/Props/TREE-broadleaf-a/TREE-broadleaf-a.TREE-broadleaf-a"), true, 18.0},
	{TEXT("/Game/Props/BKIT/SIGN_VERT_L/StaticMeshes/SIGN_VERT_L.SIGN_VERT_L"), false, 24.0},
	{TEXT("/Game/Props/TREE-park-a/TREE-park-a.TREE-park-a"), true, 10.0},
	{TEXT("/Game/Props/BKIT/SIGN_VERT_M/StaticMeshes/SIGN_VERT_M.SIGN_VERT_M"), false, 16.0},
};
constexpr int32 kArrivalCount = static_cast<int32>(UE_ARRAY_COUNT(kArrivals));

// The masters with the cut wired in (FTImportPlant -Masters, FTImportVehicle, and the prop twins under the wave folder).
const TCHAR* const kCutMasters[] = {
	TEXT("/Game/Rendering/Foliage/M_FTFoliage"),
	TEXT("/Game/Rendering/Foliage/M_FTBark"),
	TEXT("/Game/Vehicles/KeiCar/Materials/M_KeiCar"),
	TEXT("/Game/Vehicles/KeiCar/Materials/M_KeiCar_ClearCoat"),
	TEXT("/Game/Vehicles/KeiCar/Materials/M_KeiCar_Glass"),
};

FString BasePackageOf(UMaterialInterface* Material)
{
	UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
	return Base ? Base->GetPackage()->GetName() : FString();
}

bool HasCut(UMaterialInterface* Material)
{
	const FString Package = BasePackageOf(Material);
	if (Package.StartsWith(FString(FTWorldWave::Folder) + TEXT("/")))
	{
		return true;
	}
	for (const TCHAR* Master : kCutMasters)
	{
		if (Package == Master)
		{
			return true;
		}
	}
	return false;
}

// "a,b,c,..." after Key on the command line as Count numbers, or false.
bool ParseList(const TCHAR* Key, double* Out, int32 Count)
{
	FString Text;
	if (!FParse::Value(FCommandLine::Get(), Key, Text, false))
	{
		return false;
	}
	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT(","));
	if (Parts.Num() != Count)
	{
		return false;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (!Parts[Index].IsNumeric())
		{
			return false;
		}
		Out[Index] = FCString::Atod(*Parts[Index]);
	}
	return true;
}

ftcore::WaveVec3 ToMetres(const FVector& Cm)
{
	ftcore::WaveVec3 Metres;
	Metres.x = static_cast<float>(Cm.X / 100.0);
	Metres.y = static_cast<float>(Cm.Y / 100.0);
	Metres.z = static_cast<float>(Cm.Z / 100.0);
	return Metres;
}

// A world point through the view: its viewport UV, +1 in front of the camera or -1 behind, and its view depth (cm).
struct FScreenPoint
{
	float U = 0.5f;
	float V = 0.5f;
	float Side = 1.f;
	float Depth = 0.f;
};

FScreenPoint Project(const FMatrix& ViewProjection, const FVector& Point)
{
	const FVector4 Clip = ViewProjection.TransformFVector4(FVector4(Point, 1.0));
	const double W = Clip.W;
	const double Safe = W >= 0.0 ? FMath::Max(W, 1.0) : FMath::Min(W, -1.0);
	FScreenPoint Out;
	Out.U = static_cast<float>(0.5 + 0.5 * Clip.X / Safe);
	Out.V = static_cast<float>(0.5 - 0.5 * Clip.Y / Safe);
	Out.Side = W > 0.0 ? 1.f : -1.f;
	Out.Depth = static_cast<float>(W);
	return Out;
}

// The ground under a plan point: a world-static floor facing up within 3 m of the point's height, with nothing standing
// at walking height over the footprint (HalfFoot, cm). False on a roof, a wall, a slope or a cluttered spot.
bool FindGround(const UWorld& World, const FVector& Point, const FVector& HalfFoot, FVector& OutGround)
{
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(FTWorldWaveGround), false);
	FCollisionObjectQueryParams Floor;
	Floor.AddObjectTypesToQuery(ECC_WorldStatic);
	FHitResult Hit;
	if (!World.LineTraceSingleByObjectType(Hit, Point + FVector(0.0, 0.0, 3000.0), Point - FVector(0.0, 0.0, 5000.0), Floor, Query))
	{
		return false;
	}
	if (Hit.ImpactNormal.Z < 0.8 || FMath::Abs(Hit.ImpactPoint.Z - Point.Z) > 300.0)
	{
		return false;
	}
	FCollisionObjectQueryParams Standing;
	Standing.AddObjectTypesToQuery(ECC_WorldStatic);
	Standing.AddObjectTypesToQuery(ECC_WorldDynamic);
	const double HalfHeight = 110.0;
	const FVector Clear = Hit.ImpactPoint + FVector(0.0, 0.0, 30.0 + HalfHeight);
	if (World.OverlapAnyTestByObjectType(Clear, FQuat::Identity, Standing, FCollisionShape::MakeBox(FVector(HalfFoot.X, HalfFoot.Y, HalfHeight)), Query))
	{
		return false;
	}
	OutGround = Hit.ImpactPoint;
	return true;
}

// ft.WorldWave <x> <y> <z> [reverse] | off
void WorldWaveCommand(const TArray<FString>& Args, UWorld* World)
{
	UFTWorldWaveSubsystem* Wave = World ? World->GetSubsystem<UFTWorldWaveSubsystem>() : nullptr;
	if (!Wave)
	{
		return;
	}
	if (Args.Num() == 1 && Args[0].Equals(TEXT("off"), ESearchCase::IgnoreCase))
	{
		Wave->StopWave();
		return;
	}
	const bool bReverse = Args.Num() == 4 && Args[3].Equals(TEXT("reverse"), ESearchCase::IgnoreCase);
	if ((Args.Num() != 3 && !bReverse) || !Args[0].IsNumeric() || !Args[1].IsNumeric() || !Args[2].IsNumeric())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("ft.WorldWave <x> <y> <z> [reverse]: a wave from that world point (cm) starting now; ft.WorldWave off ends it."));
		return;
	}
	Wave->StartWave(FVector(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), FCString::Atod(*Args[2])), Wave->GetClock(), bReverse);
}

FAutoConsoleCommandWithWorldAndArgs GWorldWaveCommand(TEXT("ft.WorldWave"),
	TEXT("Travel between worlds: ft.WorldWave <x> <y> <z> [reverse] starts a wave from that world point (cm) now; ft.WorldWave off ends it."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WorldWaveCommand));

// ft.WorldWaveDress <x> <y> <z> <r>
void WorldWaveDressCommand(const TArray<FString>& Args, UWorld* World)
{
	UFTWorldWaveSubsystem* Wave = World ? World->GetSubsystem<UFTWorldWaveSubsystem>() : nullptr;
	if (!Wave)
	{
		return;
	}
	if (Args.Num() != 4 || !Args[0].IsNumeric() || !Args[1].IsNumeric() || !Args[2].IsNumeric() || !Args[3].IsNumeric())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("ft.WorldWaveDress <x> <y> <z> <r>: the world wave's proof dressing round that world point (cm), within r cm."));
		return;
	}
	Wave->Dress(FVector(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), FCString::Atod(*Args[2])), FCString::Atod(*Args[3]));
}

FAutoConsoleCommandWithWorldAndArgs GWorldWaveDressCommand(TEXT("ft.WorldWaveDress"),
	TEXT("The world wave's proof dressing: ft.WorldWaveDress <x> <y> <z> <r> tags the street trees, lamps and parked cars within r cm "
		 "of the world point (cm) as the first world's, and sets the second world's bare trees and boards round it."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WorldWaveDressCommand));
} // namespace

bool UFTWorldWaveSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) && Super::ShouldCreateSubsystem(Outer);
}

void UFTWorldWaveSubsystem::Initialize(FSubsystemCollectionBase& InCollection)
{
	Super::Initialize(InCollection);
	static_assert(UE_ARRAY_COUNT(FTWorldWave::VectorNames) == UE_ARRAY_COUNT(VectorParams), "one name for each vector parameter");
	static_assert(UE_ARRAY_COUNT(FTWorldWave::ScalarNames) == UE_ARRAY_COUNT(ScalarParams), "one name for each scalar parameter");
	int32 Next = 0;
	for (const TCHAR* Name : FTWorldWave::VectorNames)
	{
		VectorParams[Next++] = FName(Name);
	}
	Next = 0;
	for (const TCHAR* Name : FTWorldWave::ScalarNames)
	{
		ScalarParams[Next++] = FName(Name);
	}
	MpcLook = FTWorldWave::Load();
	if (!MpcLook)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("WorldWave: no %s; the wave runs with no look (build it with FTBuildLookMaterials)."), FTWorldWave::CollectionPath);
	}
	else if (!MpcLook->GetScalarParameterByName(ScalarParams[0]))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("WorldWave: %s has no wave parameters yet (run FTBuildWaveMaterials, then FTBuildLookMaterials)."), FTWorldWave::CollectionPath);
	}
	Pawns.Reserve(16);
	Fogs.Reserve(8);
	PostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &UFTWorldWaveSubsystem::PostActorTick);
}

void UFTWorldWaveSubsystem::Deinitialize()
{
	FWorldDelegates::OnWorldPostActorTick.Remove(PostTickHandle);
	PostTickHandle.Reset();
	// The world is going: nothing to put back but the state.
	bWave = false;
	bBootPending = false;
	Tracked.Empty();
	Pawns.Empty();
	Fogs.Empty();
	Super::Deinitialize();
}

void UFTWorldWaveSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	double Values[4] = {0.0, 0.0, 0.0, 0.0};
	if (ParseList(TEXT("WorldWaveDress="), Values, 4))
	{
		bBootDress = true;
		BootDressAt = FVector(Values[0], Values[1], Values[2]);
		BootDressRadius = Values[3];
	}
	if (ParseList(TEXT("WorldWaveAt="), Values, 4))
	{
		bBootWave = true;
		BootWaveAt = FVector(Values[0], Values[1], Values[2]);
		BootWaveTime = Values[3];
		bBootReverse = FParse::Param(FCommandLine::Get(), TEXT("WorldWaveReverse"));
	}
	bBootPending = bBootDress || bBootWave;
	if (bBootPending)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("WorldWave: on the first frame%s%s."),
			bBootDress ? *FString::Printf(TEXT(", the dressing round %s within %.0f cm"), *BootDressAt.ToString(), BootDressRadius) : TEXT(""),
			bBootWave ? *FString::Printf(TEXT(", a %s wave from %s at %.3f s on the wave clock"), bBootReverse ? TEXT("reverse") : TEXT("forward"), *BootWaveAt.ToString(), BootWaveTime) : TEXT(""));
	}
}

void UFTWorldWaveSubsystem::SetClock(double Seconds)
{
	bHoldClock = true;
	HeldClock = Seconds;
}

void UFTWorldWaveSubsystem::ReleaseClock()
{
	bHoldClock = false;
}

double UFTWorldWaveSubsystem::GetClock() const
{
	if (bHoldClock)
	{
		return HeldClock;
	}
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.0;
}

void UFTWorldWaveSubsystem::PostActorTick(UWorld* InWorld, ELevelTick, float)
{
	if (!InWorld || InWorld != GetWorld() || InWorld->bIsTearingDown)
	{
		return;
	}
	if (bBootPending)
	{
		bBootPending = false;
		if (bBootDress)
		{
			Dress(BootDressAt, BootDressRadius);
		}
		if (bBootWave)
		{
			StartWave(BootWaveAt, BootWaveTime, bBootReverse);
		}
	}
	if (bWave)
	{
		Step(*InWorld);
	}
}

void UFTWorldWaveSubsystem::StartWave(const FVector& Origin, double StartTime, bool bReverse)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (bWave)
	{
		StopWave();
	}
	Params = ftcore::WorldWaveParams{};
	Params.origin = ToMetres(Origin);
	Params.startTime = StartTime;
	Params.direction = bReverse ? ftcore::WaveDirection::Reverse : ftcore::WaveDirection::Forward;
	State = ftcore::WorldWaveState{};
	Frame = ftcore::WorldWaveFrame{};
	OriginCm = Origin;
	// The fogs at the map's own density (StopWave above has put back any a last wave thinned).
	Fogs.Reset();
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		if (UExponentialHeightFogComponent* FogComponent = It->GetComponent())
		{
			FFog& Entry = Fogs.AddDefaulted_GetRef();
			Entry.Component = FogComponent;
			Entry.Density = FogComponent->FogDensity;
			Entry.Written = FogComponent->FogDensity;
		}
	}
	GatherPawns(*World);
	// Before the front moves everything is on the side beyond it: the first world for Forward, the second for Reverse.
	for (FTracked& Item : Tracked)
	{
		ResetThing(Item, !bReverse);
	}
	EnsureVolume(*World);
	if (Volume)
	{
		Volume->bEnabled = true;
	}
	bWave = true;
	UE_LOG(LogForgottenThird, Display, TEXT("WorldWave: a %s wave from %s (cm) at %.3f s on the wave clock (now %.3f); %d things tracked, %d fogs, %d leads."),
		bReverse ? TEXT("reverse") : TEXT("forward"), *Origin.ToString(), StartTime, GetClock(), Tracked.Num(), Fogs.Num(), Pawns.Num());
}

void UFTWorldWaveSubsystem::StopWave()
{
	const bool bWas = bWave;
	bWave = false;
	SetExposureHold(false);
	if (Volume)
	{
		Volume->bEnabled = false;
	}
	for (FFog& Fog : Fogs)
	{
		if (UExponentialHeightFogComponent* FogComponent = Fog.Component.Get())
		{
			FogComponent->SetFogDensity(Fog.Density);
		}
	}
	Fogs.Reset();
	for (FTracked& Item : Tracked)
	{
		ResetThing(Item, true);
	}
	if (UWorld* World = GetWorld())
	{
		ClearCollection(*World);
	}
	if (bWas)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("WorldWave: ended; the first world back."));
	}
}

void UFTWorldWaveSubsystem::Step(UWorld& World)
{
	const double Time = GetClock();

	// The eye: the first player's view as the renderer will draw it this frame (the cameras have updated).
	APlayerController* Player = World.GetFirstPlayerController();
	FVector Eye = OriginCm;
	FMatrix ViewProjection = FMatrix::Identity;
	bool bView = false;
	if (Player)
	{
		if (Player->PlayerCameraManager)
		{
			Eye = Player->PlayerCameraManager->GetCameraLocation();
		}
		ULocalPlayer* LocalView = Player->GetLocalPlayer();
		FViewport* Viewport = LocalView && LocalView->ViewportClient ? LocalView->ViewportClient->Viewport : nullptr;
		FSceneViewProjectionData Projection;
		if (Viewport && LocalView->GetProjectionData(Viewport, Projection))
		{
			Eye = Projection.ViewOrigin;
			ViewProjection = Projection.ComputeViewProjectionMatrix();
			bView = true;
		}
	}

	const ftcore::WorldWaveStep Result = ftcore::updateWorldWave(State, Params, Time, ToMetres(Eye));
	Frame = Result.frame;
	static constexpr ftcore::WorldWaveCue kCues[] = {
		ftcore::WorldWaveCue::Prelude, ftcore::WorldWaveCue::FrontPassedListener, ftcore::WorldWaveCue::Settled};
	for (const ftcore::WorldWaveCue Cue : kCues)
	{
		if (!ftcore::hasWorldWaveCue(Result.cues, Cue))
		{
			continue;
		}
		// No sound yet: the cue's line is its place in time.
		UE_LOG(LogForgottenThird, Display, TEXT("WorldWave: %s at %.3f s (radius %.2f m, band %.2f m, camera inside %.2f)."),
			ANSI_TO_TCHAR(ftcore::worldWaveCueName(Cue)), Time - Params.startTime, Frame.radius, Frame.bandWidth, Frame.cameraInside);
		if (Cue == ftcore::WorldWaveCue::Prelude)
		{
			// Leads spawned since the start.
			GatherPawns(World);
		}
	}
	// The eye holds its exposure from the wave's start, so the change reads as the world's.
	if (!bExposureHeld && Time >= Params.startTime)
	{
		SetExposureHold(true);
	}
	else if (bExposureHeld && Time < Params.startTime)
	{
		// A held clock set back before the start (a capture's settle): the eye adapts freely again until it starts.
		SetExposureHold(false);
	}

	if (UMaterialParameterCollectionInstance* Mpc = MpcLook ? World.GetParameterCollectionInstance(MpcLook) : nullptr)
	{
		FScreenPoint Foot;
		FScreenPoint Tip;
		if (bView)
		{
			Foot = Project(ViewProjection, OriginCm);
			Tip = Project(ViewProjection, OriginCm + FVector(0.0, 0.0, FTWorldWave::StreakHeightCm));
		}
		const float Drawable = bView && Foot.Depth > 10.f && Tip.Depth > 10.f ? 1.f : 0.f;
		Mpc->SetVectorParameterValue(VectorParams[0], FLinearColor(static_cast<float>(OriginCm.X), static_cast<float>(OriginCm.Y), static_cast<float>(OriginCm.Z), 0.f));
		Mpc->SetVectorParameterValue(VectorParams[1], FLinearColor(Foot.U, Foot.V, Foot.Side, 1.f));
		Mpc->SetVectorParameterValue(VectorParams[2], FLinearColor(Foot.U, Foot.V, Drawable, Foot.Depth));
		Mpc->SetVectorParameterValue(VectorParams[3], FLinearColor(Tip.U, Tip.V, Drawable, Tip.Depth));
		Mpc->SetScalarParameterValue(ScalarParams[0], Frame.radius * 100.f);
		Mpc->SetScalarParameterValue(ScalarParams[1], Frame.bandWidth * 100.f);
		Mpc->SetScalarParameterValue(ScalarParams[2], Frame.preludeProgress);
		Mpc->SetScalarParameterValue(ScalarParams[3], Frame.direction == ftcore::WaveDirection::Reverse ? 1.f : 0.f);
		Mpc->SetScalarParameterValue(ScalarParams[4], Frame.cameraInside);
		Mpc->SetScalarParameterValue(ScalarParams[5], static_cast<float>(FMath::Max(Time - Params.startTime, 0.0)));
	}

	// The second world has no haze: the fog thins with the camera's share of it (the material whitens the sky alike).
	const float NewShare = Frame.direction == ftcore::WaveDirection::Forward ? Frame.cameraInside : 1.f - Frame.cameraInside;
	for (FFog& Fog : Fogs)
	{
		UExponentialHeightFogComponent* FogComponent = Fog.Component.Get();
		const float Thinned = Fog.Density * (1.f - NewShare);
		if (FogComponent && FMath::Abs(Thinned - Fog.Written) > 1e-6f)
		{
			FogComponent->SetFogDensity(Thinned);
			Fog.Written = Thinned;
		}
	}

	// The tagged things: shown, solid and cut as the front has them.
	const APawn* PlayerPawn = Player ? Player->GetPawn() : nullptr;
	for (FTracked& Item : Tracked)
	{
		UPrimitiveComponent* Component = Item.Component.Get();
		if (!Component)
		{
			continue;
		}
		const bool bPawn = Item.Thing.tag != ftcore::WorldTag::Both && PawnInFootprint(Item, PlayerPawn);
		const ftcore::WorldWaveThingStep Cut = ftcore::updateWorldWaveThing(Item.Thing, Params, Frame, Time, bPawn);
		if (Cut.drawn != Item.bDrawn)
		{
			Component->SetVisibility(Cut.drawn);
			Item.bDrawn = Cut.drawn;
		}
		if (Cut.collisionOn != Item.bSolid)
		{
			Component->SetCollisionEnabled(Cut.collisionOn ? Item.Collision.GetValue() : ECollisionEnabled::NoCollision);
			Item.bSolid = Cut.collisionOn;
		}
		const float Local = Cut.localFront ? FMath::Max(Cut.localRadius * 100.f, 1.f) : 0.f;
		if (Local != Item.Local)
		{
			Component->SetCustomPrimitiveDataFloat(FTWorldWave::kLocalSlot, Local);
			Item.Local = Local;
		}
		if (Cut.held != Item.bHeld)
		{
			Item.bHeld = Cut.held;
			UE_LOG(LogForgottenThird, Display, TEXT("WorldWave: %s %s at %.3f s."), Cut.held ? TEXT("holds") : TEXT("releases"),
				*Component->GetReadableName(), Time - Params.startTime);
		}
	}
}

void UFTWorldWaveSubsystem::EnsureVolume(UWorld& World)
{
	if (IsValid(Volume))
	{
		return;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Spawn.ObjectFlags |= RF_Transient;
	APostProcessVolume* Made = World.SpawnActor<APostProcessVolume>(Spawn);
	if (!Made)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("WorldWave: could not spawn the wave's post-process volume."));
		return;
	}
	Made->bUnbound = true;
	Made->BlendWeight = 1.f;
	Made->Priority = 1000.f;
	// The world keeps its volumes sorted by priority as they are added: added again, it goes last (on top).
	if (World.RemovePostProcessVolume(Made))
	{
		World.AddPostProcessVolume(Made);
	}
	const FString InkPath = FString(FTWorldWave::InkMaterialPath) + TEXT(".") + FPackageName::GetShortName(FTWorldWave::InkMaterialPath);
	if (UMaterialInterface* Ink = LoadObject<UMaterialInterface>(nullptr, *InkPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		Made->Settings.WeightedBlendables.Array.Add(FWeightedBlendable(1.f, Ink));
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("WorldWave: no %s (run FTBuildWaveMaterials); the wave runs without its look."), FTWorldWave::InkMaterialPath);
	}
	Made->bEnabled = false;
	Volume = Made;
	SetExposureHold(bExposureHeld);
}

void UFTWorldWaveSubsystem::SetExposureHold(bool bHold)
{
	bExposureHeld = bHold;
	if (!Volume)
	{
		return;
	}
	// Speed 0.001 (under the editor's 0.02 floor, which is only the slider's): the eye stays where it stood.
	FPostProcessSettings& Post = Volume->Settings;
	Post.bOverride_AutoExposureSpeedUp = bHold;
	Post.bOverride_AutoExposureSpeedDown = bHold;
	Post.AutoExposureSpeedUp = 0.001f;
	Post.AutoExposureSpeedDown = 0.001f;
}

void UFTWorldWaveSubsystem::GatherPawns(UWorld& World)
{
	Pawns.Reset();
	for (TActorIterator<AFTExploreCharacter> It(&World); It; ++It)
	{
		Pawns.Add(*It);
	}
}

bool UFTWorldWaveSubsystem::PawnInFootprint(const FTracked& Item, const APawn* PlayerPawn) const
{
	const auto Overlaps = [&Item](const APawn* Pawn)
	{
		if (!Pawn)
		{
			return false;
		}
		float PawnRadius = 0.f;
		float PawnHalfHeight = 0.f;
		Pawn->GetSimpleCollisionCylinder(PawnRadius, PawnHalfHeight);
		const FVector Apart = (Pawn->GetActorLocation() - Item.FootCentre).GetAbs();
		return Apart.X <= Item.FootExtent.X + PawnRadius && Apart.Y <= Item.FootExtent.Y + PawnRadius &&
			   Apart.Z <= Item.FootExtent.Z + PawnHalfHeight;
	};
	if (Overlaps(PlayerPawn))
	{
		return true;
	}
	for (const TWeakObjectPtr<APawn>& Lead : Pawns)
	{
		if (Overlaps(Lead.Get()))
		{
			return true;
		}
	}
	return false;
}

void UFTWorldWaveSubsystem::ResetThing(FTracked& Item, bool bOldSide)
{
	ftcore::WorldWaveThing Fresh;
	Fresh.pivot = Item.Thing.pivot;
	Fresh.radius = Item.Thing.radius;
	Fresh.tag = Item.Thing.tag;
	Item.Thing = Fresh;
	const bool bShow = Fresh.tag == ftcore::WorldTag::Both || ((Fresh.tag == ftcore::WorldTag::OldOnly) == bOldSide);
	Item.bDrawn = bShow;
	Item.bSolid = bShow;
	Item.bHeld = false;
	Item.Local = 0.f;
	if (UPrimitiveComponent* Component = Item.Component.Get())
	{
		Component->SetVisibility(bShow);
		Component->SetCollisionEnabled(bShow ? Item.Collision.GetValue() : ECollisionEnabled::NoCollision);
		Component->SetCustomPrimitiveDataFloat(FTWorldWave::kLocalSlot, 0.f);
	}
}

void UFTWorldWaveSubsystem::Track(UPrimitiveComponent& Component, ftcore::WorldTag Tag, const UStaticMesh& Mesh, const FTransform& WorldTransform)
{
	Component.SetCustomPrimitiveDataFloat(FTWorldWave::kTagSlot, static_cast<float>(static_cast<uint8>(Tag)));
	Component.SetCustomPrimitiveDataFloat(FTWorldWave::kLocalSlot, 0.f);
	FTracked& Item = Tracked.AddDefaulted_GetRef();
	Item.Component = &Component;
	const FBoxSphereBounds Bounds = Mesh.GetBounds().TransformBy(WorldTransform);
	Item.Thing.pivot = ToMetres(Bounds.Origin);
	Item.Thing.radius = static_cast<float>(Bounds.SphereRadius / 100.0);
	Item.Thing.tag = Tag;
	// The footprint a pawn must be clear of: the collision's box (a tree's trunk, not its crown), else the bounds'.
	FBox Foot = Bounds.GetBox();
	if (const UBodySetup* Body = Mesh.GetBodySetup(); Body && Body->AggGeom.GetElementCount() > 0)
	{
		Foot = Body->AggGeom.CalcAABB(WorldTransform);
	}
	Item.FootCentre = Foot.GetCenter();
	Item.FootExtent = Foot.GetExtent();
	Item.Collision = Component.GetCollisionEnabled();
	ResetThing(Item, true);
}

UMaterialInterface* UFTWorldWaveSubsystem::CutMaterialFor(UMaterialInterface* Material)
{
	if (!Material)
	{
		return nullptr;
	}
	if (const TObjectPtr<UMaterialInterface>* Found = CutFor.Find(Material))
	{
		return Found->Get();
	}
	UMaterialInterface* Chosen = Material;
	const FString Package = BasePackageOf(Material);
	for (const TCHAR* Master : FTWorldWave::PropMasters)
	{
		if (Package != Master)
		{
			continue;
		}
		const FString TwinName = FPackageName::GetShortName(Master) + FTWorldWave::TwinSuffix;
		const FString TwinPath = FString(FTWorldWave::Folder) + TEXT("/") + TwinName + TEXT(".") + TwinName;
		if (UMaterialInterface* Twin = LoadObject<UMaterialInterface>(nullptr, *TwinPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Twin, this);
			Instance->CopyMaterialUniformParameters(Material);
			Instances.Add(Instance);
			Chosen = Instance;
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("WorldWave: no %s (run FTBuildWaveMaterials); %s keeps its uncut master."), *TwinPath, *Material->GetName());
		}
		break;
	}
	CutFor.Add(Material, Chosen);
	return Chosen;
}

void UFTWorldWaveSubsystem::ClearCollection(UWorld& World)
{
	UMaterialParameterCollectionInstance* Mpc = MpcLook ? World.GetParameterCollectionInstance(MpcLook) : nullptr;
	if (!Mpc)
	{
		return;
	}
	for (const FName& Name : VectorParams)
	{
		Mpc->SetVectorParameterValue(Name, FLinearColor(0.f, 0.f, 0.f, 0.f));
	}
	for (const FName& Name : ScalarParams)
	{
		Mpc->SetScalarParameterValue(Name, 0.f);
	}
}

int32 UFTWorldWaveSubsystem::Dress(const FVector& Origin, double Radius)
{
	UWorld* World = GetWorld();
	if (!World || Radius <= 0.0)
	{
		return Tracked.Num();
	}
	const double RadiusSq = Radius * Radius;
	const FName LensTag(FTScene::NightLampHeadTag);

	// What lies within the radius: instances of a cell's instanced meshes, and fallback actors' meshes.
	struct FSplit
	{
		UInstancedStaticMeshComponent* Source = nullptr;
		EPick Pick = EPick::None;
		TArray<int32> Indices;
	};
	struct FPlain
	{
		UStaticMeshComponent* Component = nullptr;
		EPick Pick = EPick::None;
	};
	TArray<FSplit> Splits;
	TArray<FPlain> Plains;
	TMap<const UStaticMesh*, EPick> Picks;
	int32 Towers = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->IsA<AFTClockTower>())
		{
			++Towers;
			continue;
		}
		TInlineComponentArray<UStaticMeshComponent*> Meshes(Actor);
		for (UStaticMeshComponent* MeshComponent : Meshes)
		{
			UStaticMesh* Mesh = MeshComponent ? MeshComponent->GetStaticMesh() : nullptr;
			if (!Mesh || !MeshComponent->IsRegistered() || !MeshComponent->IsVisible())
			{
				continue;
			}
			// Tagged already (a split piece, an arrival, a fallback dressed before).
			const TArray<float>& Cpd = MeshComponent->GetCustomPrimitiveData().Data;
			if (Cpd.IsValidIndex(FTWorldWave::kTagSlot) && Cpd[FTWorldWave::kTagSlot] != 0.f)
			{
				continue;
			}
			EPick Pick = EPick::None;
			if (MeshComponent->ComponentHasTag(LensTag))
			{
				Pick = EPick::Lens;
			}
			else if (const EPick* Cached = Picks.Find(Mesh))
			{
				Pick = *Cached;
			}
			else
			{
				Pick = Picks.Add(Mesh, PickOf(*Mesh));
			}
			if (Pick == EPick::None)
			{
				continue;
			}
			if (UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(MeshComponent))
			{
				FSplit* Split = nullptr;
				for (int32 Index = 0; Index < Ism->GetInstanceCount(); ++Index)
				{
					FTransform Placed;
					if (Ism->GetInstanceTransform(Index, Placed, true) && FVector::DistSquared(Placed.GetLocation(), Origin) <= RadiusSq)
					{
						if (!Split)
						{
							Split = &Splits.AddDefaulted_GetRef();
							Split->Source = Ism;
							Split->Pick = Pick;
						}
						Split->Indices.Add(Index);
					}
				}
			}
			else if (FVector::DistSquared(MeshComponent->GetComponentLocation(), Origin) <= RadiusSq)
			{
				FPlain& Plain = Plains.AddDefaulted_GetRef();
				Plain.Component = MeshComponent;
				Plain.Pick = Pick;
			}
		}
	}

	int32 Count = 0;
	for (const FSplit& Split : Splits)
	{
		Count += Split.Indices.Num();
	}
	Tracked.Reserve(Tracked.Num() + Count + Plains.Num() + kArrivalCount);

	int32 PickCounts[kPickCount] = {};
	TMap<FString, int32> PickedMeshes;
	TSet<UMaterialInterface*> Uncut;
	const auto NoteSlots = [&Uncut](const UStaticMeshComponent& Dressed)
	{
		for (int32 Slot = 0; Slot < Dressed.GetNumMaterials(); ++Slot)
		{
			UMaterialInterface* Material = Dressed.GetMaterial(Slot);
			if (Material && !HasCut(Material))
			{
				Uncut.Add(Material);
			}
		}
	};

	// One instance of a cell's instanced mesh becomes a one-instance instanced mesh of its own on the same holder, as the
	// tag is per primitive.
	for (const FSplit& Split : Splits)
	{
		UInstancedStaticMeshComponent* Source = Split.Source;
		AActor* Holder = Source->GetOwner();
		UStaticMesh* Mesh = Source->GetStaticMesh();
		if (!Holder || !Mesh)
		{
			continue;
		}
		USceneComponent* Root = Holder->GetRootComponent();
		const FTransform RootTransform = Root ? Root->GetComponentTransform() : FTransform::Identity;
		const int32 NumCustom = Source->NumCustomDataFloats;
		for (const int32 Index : Split.Indices)
		{
			FTransform Placed;
			Source->GetInstanceTransform(Index, Placed, true);
			UInstancedStaticMeshComponent* Piece = NewObject<UInstancedStaticMeshComponent>(
				Holder, MakeUniqueObjectName(Holder, UInstancedStaticMeshComponent::StaticClass(), FName(TEXT("WaveOld"))), RF_Transient);
			Piece->CreationMethod = EComponentCreationMethod::Instance;
			Piece->SetMobility(Source->Mobility);
			Piece->SetStaticMesh(Mesh);
			for (int32 Slot = 0; Slot < Source->GetNumMaterials(); ++Slot)
			{
				UMaterialInterface* Want = CutMaterialFor(Source->GetMaterial(Slot));
				if (Want != Piece->GetMaterial(Slot))
				{
					Piece->SetMaterial(Slot, Want);
				}
			}
			Piece->SetCollisionProfileName(Source->GetCollisionProfileName(), false);
			Piece->SetCollisionObjectType(Source->GetCollisionObjectType());
			Piece->SetCollisionResponseToChannels(Source->GetCollisionResponseToChannels());
			Piece->SetCollisionEnabled(Source->GetCollisionEnabled());
			Piece->SetGenerateOverlapEvents(false);
			Piece->SetCanEverAffectNavigation(false);
			Piece->CastShadow = Source->CastShadow;
			Piece->bCastDynamicShadow = Source->bCastDynamicShadow;
			Piece->bCastStaticShadow = Source->bCastStaticShadow;
			Piece->bVisibleInRayTracing = Source->bVisibleInRayTracing;
			Piece->bAffectDistanceFieldLighting = Source->bAffectDistanceFieldLighting;
			Piece->bAffectDynamicIndirectLighting = Source->bAffectDynamicIndirectLighting;
			Piece->bReceivesDecals = Source->bReceivesDecals;
			Piece->InstanceStartCullDistance = Source->InstanceStartCullDistance;
			Piece->InstanceEndCullDistance = Source->InstanceEndCullDistance;
			Piece->InstancingRandomSeed = Source->InstancingRandomSeed;
			Piece->ComponentTags = Source->ComponentTags;
			Piece->SetNumCustomDataFloats(NumCustom);
			// The instance goes in before the piece registers, relative to the holder's root it attaches to.
			Piece->AddInstance(Placed.GetRelativeTransform(RootTransform), false);
			if (NumCustom > 0 && Source->PerInstanceSMCustomData.Num() >= (Index + 1) * NumCustom)
			{
				Piece->SetCustomData(0, TArrayView<const float>(Source->PerInstanceSMCustomData.GetData() + Index * NumCustom, NumCustom), false);
			}
			if (Root)
			{
				Piece->SetupAttachment(Root);
			}
			Holder->AddInstanceComponent(Piece);
			Piece->RegisterComponent();
			Track(*Piece, ftcore::WorldTag::OldOnly, *Mesh, Placed);
			NoteSlots(*Piece);
			++PickCounts[static_cast<int32>(Split.Pick)];
			++PickedMeshes.FindOrAdd(Mesh->GetPathName());
		}
		TArray<int32> Removed = Split.Indices;
		Removed.Sort(TGreater<int32>());
		Source->RemoveInstances(Removed, true);
		if (UHierarchicalInstancedStaticMeshComponent* Tree = Cast<UHierarchicalInstancedStaticMeshComponent>(Source))
		{
			Tree->BuildTreeIfOutdated(false, true);
		}
	}

	// A fallback actor's mesh is tagged where it stands.
	for (const FPlain& Plain : Plains)
	{
		UStaticMeshComponent* Dressed = Plain.Component;
		for (int32 Slot = 0; Slot < Dressed->GetNumMaterials(); ++Slot)
		{
			UMaterialInterface* Was = Dressed->GetMaterial(Slot);
			UMaterialInterface* Want = CutMaterialFor(Was);
			if (Want != Was)
			{
				Dressed->SetMaterial(Slot, Want);
			}
		}
		Track(*Dressed, ftcore::WorldTag::OldOnly, *Dressed->GetStaticMesh(), Dressed->GetComponentTransform());
		NoteSlots(*Dressed);
		++PickCounts[static_cast<int32>(Plain.Pick)];
		++PickedMeshes.FindOrAdd(Dressed->GetStaticMesh()->GetPathName());
	}

	// The second world's own things, set down round the point and facing it. They start hidden and not solid.
	int32 Arrived = 0;
	for (int32 Slot = 0; Slot < kArrivalCount; ++Slot)
	{
		const FArrival& Arrival = kArrivals[Slot];
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, Arrival.Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Mesh)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("WorldWave: dressing: no %s; that arrival is left out."), Arrival.Path);
			continue;
		}
		const FBoxSphereBounds Local = Mesh->GetBounds();
		const double Half = FMath::Max(FMath::Max(Local.BoxExtent.X, Local.BoxExtent.Y) * 0.5, 20.0);
		const FVector HalfFoot(Half, Half, 0.0);
		const double Angle = FMath::DegreesToRadians(22.5 + 45.0 * Slot);
		const FVector Out(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);
		const double Ring = FMath::Min(Arrival.RingM * 100.0, Radius * 0.9);
		FVector Ground = FVector::ZeroVector;
		bool bFound = false;
		const double Tries[] = {Ring, Ring - 300.0, Ring + 300.0};
		for (const double Try : Tries)
		{
			if (Try >= 100.0 && Try <= Radius && FindGround(*World, Origin + Out * Try, HalfFoot, Ground))
			{
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("WorldWave: dressing: no clear ground for %s at %.1f m, %.1f degrees; left out."),
				*Mesh->GetName(), Ring / 100.0, 22.5 + 45.0 * Slot);
			continue;
		}
		const FVector ToPoint(Origin.X - Ground.X, Origin.Y - Ground.Y, 0.0);
		const FRotator Facing = FRotationMatrix::MakeFromX(ToPoint.GetSafeNormal()).Rotator();
		const FTransform Placed(Facing, FVector(Ground.X, Ground.Y, Ground.Z - (Local.Origin.Z - Local.BoxExtent.Z)));
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Spawn.ObjectFlags |= RF_Transient;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Placed, Spawn);
		UStaticMeshComponent* Dressed = Actor ? Actor->GetStaticMeshComponent() : nullptr;
		if (!Dressed)
		{
			continue;
		}
		Dressed->SetMobility(EComponentMobility::Movable);
		Dressed->SetStaticMesh(Mesh);
		Dressed->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Dressed->SetGenerateOverlapEvents(false);
		Dressed->SetCanEverAffectNavigation(false);
		for (int32 Material = 0; Material < Dressed->GetNumMaterials(); ++Material)
		{
			UMaterialInterface* Was = Dressed->GetMaterial(Material);
			if (!Was)
			{
				continue;
			}
			if (Arrival.bTree)
			{
				// Bare: the leaves and the far billboard gone whatever the season.
				const FString Name = Was->GetName();
				if (Name.EndsWith(TEXT("_Leaf")) || Name.EndsWith(TEXT("_Billboard")))
				{
					UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Was, this);
					Instance->SetScalarParameterValue(TEXT("Bare"), 1.f);
					Instances.Add(Instance);
					Dressed->SetMaterial(Material, Instance);
				}
			}
			else
			{
				UMaterialInterface* Want = CutMaterialFor(Was);
				if (Want != Was)
				{
					Dressed->SetMaterial(Material, Want);
				}
			}
		}
		Track(*Dressed, ftcore::WorldTag::NewOnly, *Mesh, Actor->GetActorTransform());
		NoteSlots(*Dressed);
		++Arrived;
		UE_LOG(LogForgottenThird, Display, TEXT("WorldWave: dressing: arrival %s at %s (%.1f m out)."), *Mesh->GetPathName(),
			*Placed.GetLocation().ToString(), FVector::Dist2D(Placed.GetLocation(), Origin) / 100.0);
	}

	// The picks.
	UE_LOG(LogForgottenThird, Display, TEXT("WorldWave: dressing round %s within %.0f cm: first world's only %d %s, %d %s, %d %s, %d %s; "
		"second world's only %d arrivals; the ground, the buildings and the clock tower (%d) left to both."),
		*Origin.ToString(), Radius,
		PickCounts[1], kPickNames[1], PickCounts[2], kPickNames[2], PickCounts[3], kPickNames[3], PickCounts[4], kPickNames[4],
		Arrived, Towers);
	for (const TPair<FString, int32>& Picked : PickedMeshes)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("WorldWave: dressing: %d x %s."), Picked.Value, *Picked.Key);
	}
	for (UMaterialInterface* Material : Uncut)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("WorldWave: dressing: %s has no cut in its master (%s): its thing goes by visibility alone."),
			*Material->GetPathName(), *BasePackageOf(Material));
	}
	return Tracked.Num();
}
