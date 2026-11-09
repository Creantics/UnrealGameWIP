#include "FTPlaceSkeletalSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "FTMeshFacing.h"
#include "ForgottenThird.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ftcore/MassingPlan.h"

namespace
{
// How long after begin play the ground is looked for, in seconds: long enough for a streamed cell to come in.
constexpr float kPlaceSnapSeconds = 8.f;
// The line down starts this far above the plan point and ends this far below it, in centimetres.
constexpr double kPlaceProbeUp = 200.0;
constexpr double kPlaceProbeDown = 500.0;

// "-Key=x,y,z" in the plan frame (metres) as an engine point in centimetres, or false.
bool ReadPlanPoint(const TCHAR* Key, double Side, FVector& Out, FVector& Plan)
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
	Plan = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
	const ftcore::MassingVec M = ftcore::massingEnginePoint(Side, Plan.X, Plan.Y, Plan.Z);
	Out = FVector(M.x * 100.0, M.y * 100.0, M.z * 100.0);
	return true;
}

// "/Game/A/b" names the object b in /Game/A/b; "/Game/A/b.b" is already an object path.
FString PlaceObjectPath(const FString& Path)
{
	return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPaths::GetBaseFilename(Path);
}

void PlaceFail(const TCHAR* Why, const FString& What)
{
	UE_LOG(LogForgottenThird, Error, TEXT("PlaceSkeletal: %s %s"), Why, *What);
	FTRequestExit(1, TEXT("PlaceSkeletal"));
}
} // namespace

bool UFTPlaceSkeletalSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	FString Skin;
	return World && World->WorldType == EWorldType::Game && FParse::Value(FCommandLine::Get(), TEXT("PlaceSkin="), Skin) &&
		!Skin.IsEmpty() && Super::ShouldCreateSubsystem(Outer);
}

void UFTPlaceSkeletalSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	FString MeshPath, AnimPath;
	FParse::Value(FCommandLine::Get(), TEXT("PlaceSkin="), MeshPath);
	FParse::Value(FCommandLine::Get(), TEXT("PlaceAnim="), AnimPath);
	double Side = 1000.0;
	FParse::Value(FCommandLine::Get(), TEXT("PlaceSide="), Side);
	FVector Plan, FacePlan;
	FVector Face = FVector::ZeroVector;
	if (Side <= 0.0 || !ReadPlanPoint(TEXT("PlaceAt="), Side, Base, Plan))
	{
		PlaceFail(TEXT("-PlaceAt=<x,y,z> (plan frame, metres) and a -PlaceSide above 0 are needed;"), TEXT("nothing placed."));
		return;
	}
	const bool bFace = ReadPlanPoint(TEXT("PlaceFace="), Side, Face, FacePlan);

	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *PlaceObjectPath(MeshPath));
	if (!Mesh)
	{
		PlaceFail(TEXT("couldn't load the mesh"), MeshPath);
		return;
	}
	UAnimSequence* Clip = nullptr;
	if (!AnimPath.IsEmpty())
	{
		Clip = LoadObject<UAnimSequence>(nullptr, *PlaceObjectPath(AnimPath));
		if (!Clip)
		{
			PlaceFail(TEXT("couldn't load the clip"), AnimPath);
			return;
		}
		// A clip on another skeleton plays nothing and says nothing.
		if (Clip->GetSkeleton() != Mesh->GetSkeleton())
		{
			PlaceFail(TEXT("the clip's skeleton isn't the mesh's:"), Clip->GetPathName());
			return;
		}
	}

	FVector Toward = Face - Base;
	Toward.Z = 0.0;
	const double FaceYaw = bFace && !Toward.IsNearlyZero() ? Toward.Rotation().Yaw : 0.0;
	const double MeshYaw = FTMeshFacing::FacingYaw(*Mesh);
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Placed = InWorld.SpawnActor<ASkeletalMeshActor>(Base, FRotator(0.0, FaceYaw + MeshYaw, 0.0), Spawn);
	if (!Placed)
	{
		PlaceFail(TEXT("couldn't spawn the actor for"), Mesh->GetPathName());
		return;
	}
	Placed->SetActorEnableCollision(false);
	USkeletalMeshComponent* Skin = Placed->GetSkeletalMeshComponent();
	Skin->SetMobility(EComponentMobility::Movable);
	Skin->SetSkeletalMesh(Mesh);
	// Posed every frame whether or not the renderer thinks it is seen: the still is the point.
	Skin->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	if (Clip)
	{
		Skin->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		Skin->PlayAnimation(Clip, true);
	}
	GroundQuery = FCollisionQueryParams(SCENE_QUERY_STAT(FTPlaceSkeletalGround), false, Placed.Get());
	Searched = 0.f;
	bSnapping = true;
	UE_LOG(LogForgottenThird, Display,
		TEXT("PlaceSkeletal: %s at (%.2f, %.2f, %.2f) in the plan frame (engine %s), yaw %.1f (facing %.1f%s, mesh %.1f), clip %s%s."),
		*Mesh->GetPathName(), Plan.X, Plan.Y, Plan.Z, *Base.ToString(), FaceYaw + MeshYaw, FaceYaw,
		bFace ? *FString::Printf(TEXT(" toward (%.2f, %.2f, %.2f)"), FacePlan.X, FacePlan.Y, FacePlan.Z) : TEXT(", no -PlaceFace"),
		MeshYaw, Clip ? *Clip->GetPathName() : TEXT("none (reference pose)"),
		Clip ? *FString::Printf(TEXT(" looping, %.2f s"), Clip->GetPlayLength()) : TEXT(""));
}

void UFTPlaceSkeletalSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!bSnapping || !World || !Placed)
	{
		bSnapping = false;
		return;
	}
	FHitResult Hit;
	const FVector Start = Base + FVector(0.0, 0.0, kPlaceProbeUp);
	const FVector End = Base - FVector(0.0, 0.0, kPlaceProbeDown);
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, GroundQuery))
	{
		Placed->SetActorLocation(Hit.ImpactPoint);
		UE_LOG(LogForgottenThird, Display, TEXT("PlaceSkeletal: ground found after %.1f s at engine z %.1f cm (%+.1f cm from the plan z), on %s."),
			Searched, Hit.ImpactPoint.Z, Hit.ImpactPoint.Z - Base.Z, Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("no actor"));
		bSnapping = false;
		return;
	}
	Searched += DeltaTime;
	if (Searched >= kPlaceSnapSeconds)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("PlaceSkeletal: no ground from %.0f cm above to %.0f cm below the plan point in %.1f s; it stays at the plan z."),
			kPlaceProbeUp, kPlaceProbeDown, Searched);
		bSnapping = false;
	}
}

TStatId UFTPlaceSkeletalSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTPlaceSkeletalSubsystem, STATGROUP_Tickables);
}
