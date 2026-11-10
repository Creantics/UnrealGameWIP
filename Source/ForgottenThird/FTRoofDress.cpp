#include "FTRoofDress.h"

#include "ForgottenThird.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Parse.h"

namespace FTRoofDress
{
FOptions ParseOptions(const TCHAR* Params)
{
	FOptions Options;
	Options.bScatter = FParse::Param(Params, TEXT("RoofScatter"));
	FParse::Value(Params, TEXT("RoofHut="), Options.HutMesh);
	FParse::Value(Params, TEXT("RoofHutTurn="), Options.HutTurn);
	return Options;
}

void ApplyStyle(const FOptions& Options, ftcore::MassingRoofStyle& Style)
{
	Style.scatter = Options.bScatter;
}

bool FHuts::Begin(const FOptions& Options)
{
	Turn = Options.HutTurn;
	if (Options.HutMesh.IsEmpty())
	{
		return true;
	}
	Mesh = LoadObject<UStaticMesh>(nullptr, *Options.HutMesh);
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -RoofHut: couldn't load '%s'; the huts stay cubes."), *Options.HutMesh);
		return false;
	}
	const FBox Bounds = Mesh->GetBoundingBox();
	MeshMin = ftcore::MassingVec{Bounds.Min.X / 100.0, Bounds.Min.Y / 100.0, Bounds.Min.Z / 100.0};
	MeshMax = ftcore::MassingVec{Bounds.Max.X / 100.0, Bounds.Max.Y / 100.0, Bounds.Max.Z / 100.0};
	return true;
}

bool FHuts::Take(const ftcore::MassingBox& Piece, double Side)
{
	if (!Mesh || Piece.kind != "hut")
	{
		return false;
	}
	// The mesh fills the hut's box, its door to the box's -u side (the roof's middle on a scattered roof).
	ftcore::MassingKitPlace Place;
	if (!ftcore::massingKitPlacement(Piece, Side, MeshMin, MeshMax, true, ftcore::massingRoofHutTurn(Piece) + Turn, 0.0, Place))
	{
		return false;
	}
	Pending.Add(FTransform(FRotator(0.0, Place.yaw, 0.0), FVector(Place.at.x * 100.0, Place.at.y * 100.0, Place.at.z * 100.0),
		FVector(Place.scale.x, Place.scale.y, Place.scale.z)));
	return true;
}

int32 FHuts::Finish(UWorld* World, const FActorSpawnParameters& Spawn)
{
	if (!Mesh || Pending.Num() == 0)
	{
		return 0;
	}
	AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	if (!Holder)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -RoofHut: couldn't spawn the huts' holder."));
		return 0;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("Root"));
	Root->CreationMethod = EComponentCreationMethod::Instance;
	Root->SetMobility(EComponentMobility::Static);
	Holder->SetRootComponent(Root);
	Holder->AddInstanceComponent(Root);
	Root->RegisterComponent();
	Holder->SetActorLabel(TEXT("Roof_Huts"));
	UHierarchicalInstancedStaticMeshComponent* Comp = NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder, TEXT("RoofHutIsm"));
	Comp->CreationMethod = EComponentCreationMethod::Instance;
	Comp->SetMobility(EComponentMobility::Static);
	Comp->SetStaticMesh(Mesh);
	Comp->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	Comp->SetupAttachment(Holder->GetRootComponent());
	Holder->AddInstanceComponent(Comp);
	Comp->RegisterComponent();
	Comp->AddInstances(Pending, false, true);
	const int32 Added = Pending.Num();
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: roofs: %d stair-head huts as instances of %s."), Added, *Mesh->GetPathName());
	Pending.Reset();
	return Added;
}
} // namespace FTRoofDress
