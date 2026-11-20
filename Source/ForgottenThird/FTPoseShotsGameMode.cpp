#include "FTPoseShotsGameMode.h"

#include "Animation/AnimSequence.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "ContentStreaming.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "FTExploreCharacter.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "ShaderCompiler.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"
#include "ftcore/AnimImport.h"

#include <string>
#include <vector>

#if UE_BUILD_SHIPPING
// A tool for development builds: in Shipping the class stays declared (UHT reflects it in every configuration) but none
// of its code is compiled. Picked by ?game=, it is the plain game mode it derives from: no -Pose flag is read, nothing is
// spawned or shot.
AFTPoseShotsGameMode::AFTPoseShotsGameMode() = default;

void AFTPoseShotsGameMode::StartPlay()
{
	Super::StartPlay();
}

void AFTPoseShotsGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
}
#else
AFTPoseShotsGameMode::AFTPoseShotsGameMode()
{
	HUDClass = nullptr;
	DefaultPawnClass = nullptr;
	bHostsStory = false;
	PrimaryActorTick.bCanEverTick = true;
}

namespace FTPoseShots
{
constexpr double kStep = 1.0 / 48.0;
// Ticks before the first still (exposure, Lumen, streaming) and before each still after a pose or camera change.
constexpr int32 kFirstSettle = 240;
constexpr int32 kSettle = 96;
// Where the character stands in the stub room (floor top at z = 0, the wall at x = 850).
const FVector kSpot(300.0, 0.0, 0.0);
// An unskinned bone farther than this from the skinned bones' box is left out of the frame (FrameBox).
constexpr double kNearSkin = 40.0;
// The framed angle has to move by more than this before Tick aims again: a window dragged to a new size would otherwise
// aim, and wait to settle, on every tick.
constexpr double kReaimDeg = 0.5;

// The box that frames a mesh as it stands now: its skinned bones (LOD 0's sections) and any other bone within kNearSkin
// of them. An unskinned helper left at its rest pose (an IK target, an attach point a metre off the body) would pull the
// frame off the body; a head-top end bone stays in. Every bone when the mesh has no render data.
FBox FrameBox(const USkeletalMeshComponent& C)
{
	const int32 Num = C.GetNumBones();
	TBitArray<> Skinned(false, Num);
	const USkeletalMesh* Mesh = C.GetSkeletalMeshAsset();
	const FSkeletalMeshRenderData* Render = Mesh ? Mesh->GetResourceForRendering() : nullptr;
	if (Render && Render->LODRenderData.Num() > 0)
	{
		for (const FSkelMeshRenderSection& Section : Render->LODRenderData[0].RenderSections)
		{
			for (const FBoneIndexType B : Section.BoneMap)
			{
				if (B < Num)
				{
					Skinned[B] = true;
				}
			}
		}
	}
	FBox All(ForceInit);
	FBox Skin(ForceInit);
	for (int32 B = 0; B < Num; ++B)
	{
		const FVector P = C.GetBoneLocation(C.GetBoneName(B));
		All += P;
		if (Skinned[B])
		{
			Skin += P;
		}
	}
	if (!Skin.IsValid)
	{
		return All;
	}
	const FBox Near = Skin.ExpandBy(kNearSkin);
	FBox Box = Skin;
	for (int32 B = 0; B < Num; ++B)
	{
		const FVector P = C.GetBoneLocation(C.GetBoneName(B));
		if (!Skinned[B] && Near.IsInsideOrOn(P))
		{
			Box += P;
		}
	}
	return Box;
}

// The game viewport's size in pixels; 0x0 before the window has one (as at StartPlay).
FIntPoint WindowSize(const UWorld* World)
{
	const UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
	return Client && Client->Viewport ? Client->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
}

// Half of the narrower field of view in radians. The camera's FovDeg (-PoseFov, 30 by default) spans the viewport's width,
// and a still keeps the narrower angle: a 1200x1200 still from a 1280x720 window showed about 17 degrees, not 30, and cut
// the head and feet off.
double HalfAngle(const UWorld* World, double FovDeg)
{
	double Half = FMath::DegreesToRadians(FovDeg * 0.5);
	const FIntPoint V = WindowSize(World);
	if (V.X > 0 && V.Y > 0 && V.Y < V.X)
	{
		Half = FMath::Atan(FMath::Tan(Half) * V.Y / V.X);
	}
	return Half;
}

// A look by name among the explore character's PartyLooks, then its CastLooks (read from its config defaults).
const FFTLeadLook* FindLook(const FString& Name)
{
	const UObject* Defaults = AFTExploreCharacter::StaticClass()->GetDefaultObject();
	for (const TCHAR* Field : {TEXT("PartyLooks"), TEXT("CastLooks")})
	{
		const FArrayProperty* Prop = FindFProperty<FArrayProperty>(AFTExploreCharacter::StaticClass(), Field);
		if (!Prop)
		{
			continue;
		}
		const TArray<FFTLeadLook>* Looks = Prop->ContainerPtrToValuePtr<TArray<FFTLeadLook>>(Defaults);
		for (const FFTLeadLook& Look : *Looks)
		{
			if (Look.Name.Equals(Name, ESearchCase::IgnoreCase))
			{
				return &Look;
			}
		}
	}
	return nullptr;
}

void Freeze(USkeletalMeshComponent& Component, UAnimSequence* Anim)
{
	Component.SetAnimationMode(EAnimationMode::AnimationSingleNode);
	Component.PlayAnimation(Anim, false);
	Component.SetPlayRate(0.f);
}
} // namespace FTPoseShots

void AFTPoseShotsGameMode::Fail(const FString& Why)
{
	UE_LOG(LogForgottenThird, Error, TEXT("FTPoseShots: %s"), *Why);
	bActive = false;
	FTRequestExit(1, TEXT("FTPoseShots"));
}

void AFTPoseShotsGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose: no story here.
	AGameModeBase::StartPlay();
	if (!FParse::Value(FCommandLine::Get(), TEXT("PoseShots="), OutDir) || OutDir.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTPoseShots: no -PoseShots=<dir>; nothing to do."));
		return;
	}
	if (!Setup())
	{
		return;
	}
	FApp::SetUseFixedTimeStep(true);
	FApp::SetFixedDeltaTime(FTPoseShots::kStep);
	bActive = true;
	Pose(Times[0]);
	Aim(0);
	Wait = FTPoseShots::kFirstSettle;
	UE_LOG(LogForgottenThird, Display, TEXT("FTPoseShots: %d times x %d views to %s."), Times.Num(), Views.Num(), *OutDir);
}

bool AFTPoseShotsGameMode::Setup()
{
	namespace AI = ftcore::animimport;
	const TCHAR* Cmd = FCommandLine::Get();
	OutDir = FPaths::ConvertRelativePathToFull(OutDir);
	IFileManager::Get().MakeDirectory(*OutDir, true);
	FParse::Value(Cmd, TEXT("PoseSize="), Size);
	FParse::Value(Cmd, TEXT("PoseFov="), Fov);
	FString StaticPath;
	const bool bStatic = FParse::Value(Cmd, TEXT("PoseStatic="), StaticPath);

	FString Text;
	std::string Error;
	std::vector<double> ParsedTimes;
	if (bStatic)
	{
		// A static mesh has no clip: one still per view.
		Times.Add(0.0);
	}
	else if (!FParse::Value(Cmd, TEXT("PoseTimes="), Text, false) || !AI::ParseTimes(TCHAR_TO_UTF8(*Text), ParsedTimes, &Error))
	{
		Fail(FString::Printf(TEXT("-PoseTimes=%s: %s"), *Text, *FT::ToFString(Error.empty() ? std::string("missing") : Error)));
		return false;
	}
	for (double T : ParsedTimes)
	{
		Times.Add(T);
	}
	Text = TEXT("front,tq");
	FParse::Value(Cmd, TEXT("PoseViews="), Text, false);
	std::vector<AI::ShotView> ParsedViews;
	if (!AI::ParseViews(TCHAR_TO_UTF8(*Text), ParsedViews, &Error))
	{
		Fail(FString::Printf(TEXT("-PoseViews=%s: %s"), *Text, *FT::ToFString(Error)));
		return false;
	}
	for (const AI::ShotView& V : ParsedViews)
	{
		Views.Add(FView{FT::ToFString(V.name), V.yawDeg, V.pitchDeg});
	}
	if (FParse::Value(Cmd, TEXT("PoseNear="), Text, false))
	{
		TArray<FString> Pairs;
		Text.ParseIntoArray(Pairs, TEXT("+"));
		for (const FString& Pair : Pairs)
		{
			FString A, B;
			if (!Pair.Split(TEXT(":"), &A, &B))
			{
				Fail(FString::Printf(TEXT("-PoseNear: '%s' is not bodybone:propbone."), *Pair));
				return false;
			}
			Near.Emplace(FName(*A), FName(*B));
		}
	}

	double Yaw = 90.0;
	FParse::Value(Cmd, TEXT("PoseYaw="), Yaw);
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Subject = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FRotator(0.0, Yaw, 0.0), FTPoseShots::kSpot), Spawn);
	USceneComponent* Root = NewObject<USceneComponent>(Subject, TEXT("Root"));
	Subject->SetRootComponent(Root);
	Root->RegisterComponent();
	Subject->SetActorTransform(FTransform(FRotator(0.0, Yaw, 0.0), FTPoseShots::kSpot));
	if (bStatic)
	{
		return SetupStatic(StaticPath);
	}

	FString ClipPath;
	if (!FParse::Value(Cmd, TEXT("PoseClip="), ClipPath) || !(Clip = LoadObject<UAnimSequence>(nullptr, *ClipPath)))
	{
		Fail(FString::Printf(TEXT("-PoseClip=%s doesn't load."), *ClipPath));
		return false;
	}
	StillBase = Clip->GetName();
	Body = NewObject<USkeletalMeshComponent>(Subject, TEXT("Body"));
	Body->SetupAttachment(Root);
	Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	FString LookName, MeshPath;
	TArray<FSoftObjectPath> PiecePaths;
	const FFTLeadLook* Look = nullptr;
	if (FParse::Value(Cmd, TEXT("PoseLook="), LookName))
	{
		Look = FTPoseShots::FindLook(LookName);
		if (!Look)
		{
			Fail(FString::Printf(TEXT("no look named %s in the explore character's PartyLooks or CastLooks."), *LookName));
			return false;
		}
		const FFTLoadedLook Loaded = FTLook::LoadLook(*Look, TEXT("FTPoseShots"));
		if (!Loaded.Body)
		{
			Fail(FString::Printf(TEXT("look %s has no body that loads."), *LookName));
			return false;
		}
		Body->SetSkeletalMesh(Loaded.Body);
		Fronts = Loaded.Fronts;
		PiecePaths = Loaded.Pieces;
	}
	else if (FParse::Value(Cmd, TEXT("PoseMesh="), MeshPath))
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
		if (!Mesh)
		{
			Fail(FString::Printf(TEXT("-PoseMesh=%s doesn't load."), *MeshPath));
			return false;
		}
		Body->SetSkeletalMesh(Mesh);
	}
	else
	{
		Fail(TEXT("pass -PoseLook=<name> or -PoseMesh=<skeletal mesh>."));
		return false;
	}
	Body->RegisterComponent();
	if (Body->GetSkeletalMeshAsset()->GetSkeleton() != Clip->GetSkeleton())
	{
		Fail(FString::Printf(TEXT("%s is on %s, the body on %s."), *Clip->GetPathName(), *GetPathNameSafe(Clip->GetSkeleton()),
			*GetPathNameSafe(Body->GetSkeletalMeshAsset()->GetSkeleton())));
		return false;
	}
	if (FParse::Value(Cmd, TEXT("PosePieces="), Text, false))
	{
		TArray<FString> Extra;
		Text.ParseIntoArray(Extra, TEXT("+"));
		for (const FString& Path : Extra)
		{
			PiecePaths.Add(FSoftObjectPath(Path));
		}
	}
	FTLook::AddPieces(*Subject, *Body, PiecePaths, Pieces, TEXT("FTPoseShots"));
	if (Look)
	{
		FTLook::AddGrooms(*Subject, *Body, Pieces, Look->Grooms, Grooms, TEXT("FTPoseShots"));
		FTLook::SwapMaterials(*Body, Pieces, Look->MaterialSwaps, TEXT("FTPoseShots"));
	}
	FTPoseShots::Freeze(*Body, Clip);

	FString PropText, PropDesc = TEXT("none");
	if (FParse::Value(Cmd, TEXT("PoseProp="), PropText, false))
	{
		TArray<FString> Parts;
		PropText.ParseIntoArray(Parts, TEXT(","));
		USkeletalMesh* PropMesh = Parts.Num() >= 2 ? LoadObject<USkeletalMesh>(nullptr, *Parts[0]) : nullptr;
		PropClip = Parts.Num() >= 2 ? LoadObject<UAnimSequence>(nullptr, *Parts[1]) : nullptr;
		if (!PropMesh || !PropClip)
		{
			Fail(FString::Printf(TEXT("-PoseProp=%s: needs <skeletal mesh>,<anim>[,<socket>] that load."), *PropText));
			return false;
		}
		Prop = NewObject<USkeletalMeshComponent>(Subject, TEXT("Prop"));
		if (Parts.Num() >= 3)
		{
			Prop->SetupAttachment(Body, FName(*Parts[2]));
		}
		else
		{
			Prop->SetupAttachment(Root);
		}
		Prop->SetSkeletalMesh(PropMesh);
		Prop->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		Prop->RegisterComponent();
		FTPoseShots::Freeze(*Prop, PropClip);
		PropDesc = PropMesh->GetPathName() + TEXT(",") + PropClip->GetPathName() + (Parts.Num() >= 3 ? TEXT(",") + Parts[2] : FString());
	}

	SpawnCamera();
	Describe = FString::Printf(TEXT("mesh=%s look=%s clip=%s prop=%s pieces=%d grooms=%d"),
		*GetPathNameSafe(Body->GetSkeletalMeshAsset()), Look ? *Look->Name : TEXT("none"), *Clip->GetPathName(), *PropDesc, Pieces.Num(),
		Grooms.Num());
	UE_LOG(LogForgottenThird, Display, TEXT("FTPoseShots: %s, clip %.3f s."), *Describe, Clip->GetPlayLength());
	const FIntPoint Window = FTPoseShots::WindowSize(GetWorld());
	UE_LOG(LogForgottenThird, Display, TEXT("FTPoseShots: window %dx%d, framed to %.1f degrees (camera %.0f across), stills %s."), Window.X,
		Window.Y, FMath::RadiansToDegrees(FTPoseShots::HalfAngle(GetWorld(), Fov)) * 2.0, Fov, *Size);
	return true;
}

bool AFTPoseShotsGameMode::SetupStatic(const FString& MeshPath)
{
	const TCHAR* Cmd = FCommandLine::Get();
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		Fail(FString::Printf(TEXT("-PoseStatic=%s doesn't load."), *MeshPath));
		return false;
	}
	FParse::Value(Cmd, TEXT("PoseDist="), FixedDist);
	FString Text;
	if (FParse::Value(Cmd, TEXT("PoseAt="), Text, false))
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() != 3 || !Parts[0].IsNumeric() || !Parts[1].IsNumeric() || !Parts[2].IsNumeric())
		{
			Fail(FString::Printf(TEXT("-PoseAt=%s: needs x,y,z in cm."), *Text));
			return false;
		}
		AimAt = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
		bAimAt = true;
	}
	int32 Lod = -1;
	FParse::Value(Cmd, TEXT("PoseLod="), Lod);
	if (Lod >= Mesh->GetNumLODs())
	{
		Fail(FString::Printf(TEXT("-PoseLod=%d: %s has %d LODs."), Lod, *Mesh->GetPathName(), Mesh->GetNumLODs()));
		return false;
	}
	Still = NewObject<UStaticMeshComponent>(Subject, TEXT("Still"));
	Still->SetupAttachment(Subject->GetRootComponent());
	Still->SetStaticMesh(Mesh);
	if (Lod >= 0)
	{
		// ForcedLodModel counts from 1 (0 = the engine picks).
		Still->SetForcedLodModel(Lod + 1);
	}
	Still->RegisterComponent();
	StillBase = Mesh->GetName();
	SpawnCamera();
	Describe = FString::Printf(TEXT("static=%s lods=%d lod=%s nanite=%d bounds=%s cm dist=%s at=%s fov=%.0f"), *Mesh->GetPathName(),
		Mesh->GetNumLODs(), Lod >= 0 ? *FString::FromInt(Lod) : TEXT("auto"), Mesh->HasValidNaniteData() ? 1 : 0,
		*Still->Bounds.GetBox().GetSize().ToString(), FixedDist > 0.0 ? *FString::Printf(TEXT("%.0f"), FixedDist) : TEXT("framed"),
		bAimAt ? *AimAt.ToString() : TEXT("centre"), Fov);
	UE_LOG(LogForgottenThird, Display, TEXT("FTPoseShots: %s."), *Describe);
	return true;
}

void AFTPoseShotsGameMode::SpawnCamera()
{
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Camera = GetWorld()->SpawnActor<ACameraActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	Camera->GetCameraComponent()->SetFieldOfView(Fov);
	Camera->GetCameraComponent()->bConstrainAspectRatio = false;
	if (APlayerController* Player = GetWorld()->GetFirstPlayerController())
	{
		Player->SetViewTarget(Camera);
	}
}

void AFTPoseShotsGameMode::Pose(double Seconds)
{
	if (!Body)
	{
		// A static mesh holds no pose.
		return;
	}
	Body->SetPosition(static_cast<float>(Seconds), false);
	Body->TickAnimation(0.f, false);
	Body->RefreshBoneTransforms();
	if (Prop)
	{
		Prop->SetPosition(static_cast<float>(Seconds), false);
		Prop->TickAnimation(0.f, false);
		Prop->RefreshBoneTransforms();
	}
	// The pose jumps: cloth starts again from it rather than being dragged there.
	for (USkeletalMeshComponent* Piece : Pieces)
	{
		if (Piece)
		{
			Piece->ForceClothNextUpdateTeleportAndReset();
		}
	}
	// Scale and axis guard: the framing follows the bones, so a wrong scale or a turned clip would still fill the frame.
	FBox Box(ForceInit);
	for (int32 B = 0; B < Body->GetNumBones(); ++B)
	{
		Box += Body->GetBoneLocation(Body->GetBoneName(B));
	}
	// The bones at the two ends of the box's longest axis: what stretches it.
	const FVector BoxSize = Box.GetSize();
	const int32 Axis = BoxSize.X >= BoxSize.Y && BoxSize.X >= BoxSize.Z ? 0 : (BoxSize.Y >= BoxSize.Z ? 1 : 2);
	FName Low, High;
	double LowAt = TNumericLimits<double>::Max(), HighAt = TNumericLimits<double>::Lowest();
	for (int32 B = 0; B < Body->GetNumBones(); ++B)
	{
		const double At = Body->GetBoneLocation(Body->GetBoneName(B))[Axis];
		if (At < LowAt)
		{
			LowAt = At;
			Low = Body->GetBoneName(B);
		}
		if (At > HighAt)
		{
			HighAt = At;
			High = Body->GetBoneName(B);
		}
	}
	const auto Height = [this](const TCHAR* Bone)
	{
		return Body->GetBoneIndex(Bone) == INDEX_NONE ? -1.0 : Body->GetBoneLocation(Bone).Z - FTPoseShots::kSpot.Z;
	};
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTPoseShots: t=%.3f pelvis z %.1f cm, head z %.1f cm, bone box %s cm (%c from %s to %s), framed box %s cm."), Seconds,
		Height(TEXT("pelvis")), Height(TEXT("head")), *BoxSize.ToString(), TEXT("XYZ")[Axis], *Low.ToString(), *High.ToString(),
		*FTPoseShots::FrameBox(*Body).GetSize().ToString());
	for (const TPair<FName, FName>& Pair : Near)
	{
		if (!Prop || Body->GetBoneIndex(Pair.Key) == INDEX_NONE || Prop->GetBoneIndex(Pair.Value) == INDEX_NONE)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTPoseShots: -PoseNear %s:%s: a bone (or the prop) is missing."), *Pair.Key.ToString(),
				*Pair.Value.ToString());
			continue;
		}
		const FVector A = Body->GetBoneLocation(Pair.Key);
		const FVector B = Prop->GetBoneLocation(Pair.Value);
		UE_LOG(LogForgottenThird, Display, TEXT("FTPoseShots: t=%.3f %s to %s: %.1f cm."), Seconds, *Pair.Key.ToString(),
			*Pair.Value.ToString(), FVector::Dist(A, B));
	}
}

void AFTPoseShotsGameMode::Aim(int32 View)
{
	// Frame the bones (body and prop) as they stand now, without far unskinned helpers (FTPoseShots::FrameBox), or a static
	// mesh's bounds.
	FBox Box(ForceInit);
	if (Still)
	{
		Box = Still->Bounds.GetBox();
	}
	else
	{
		for (USkeletalMeshComponent* C : {Body.Get(), Prop.Get()})
		{
			if (C)
			{
				Box += FTPoseShots::FrameBox(*C);
			}
		}
		Box = Box.ExpandBy(FVector(18.0, 18.0, 14.0));
		Box.Min.Z = FMath::Min(Box.Min.Z, FTPoseShots::kSpot.Z - 4.0);
	}
	const FVector Center = bAimAt ? Subject->GetActorTransform().TransformPosition(AimAt) : Box.GetCenter();
	const double Radius = Box.GetExtent().Size();
	AimedHalf = FTPoseShots::HalfAngle(GetWorld(), Fov);
	const double Dist = FixedDist > 0.0 ? FixedDist : Radius / FMath::Tan(AimedHalf) * 0.92;
	// Facing: the body's (or static mesh's) +Y. A positive view yaw goes toward the character's left.
	const USceneComponent* Front = Still ? static_cast<USceneComponent*>(Still.Get()) : static_cast<USceneComponent*>(Body.Get());
	const FVector Facing = Front->GetComponentTransform().TransformVectorNoScale(FVector(0.0, 1.0, 0.0)).GetSafeNormal2D();
	const FVector Dir = Facing.RotateAngleAxis(-Views[View].Yaw, FVector::UpVector);
	const double Pitch = FMath::DegreesToRadians(Views[View].Pitch);
	const FVector Eye = Center + (Dir * FMath::Cos(Pitch) + FVector::UpVector * FMath::Sin(Pitch)) * Dist;
	Camera->SetActorLocationAndRotation(Eye, (Center - Eye).Rotation());
}

void AFTPoseShotsGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bActive)
	{
		return;
	}
	// A material still compiling draws as the engine's default: nothing counts until every shader is in.
	if (GShaderCompilingManager && GShaderCompilingManager->GetNumRemainingJobs() > 0)
	{
		Wait = FMath::Max(Wait, FTPoseShots::kSettle);
		return;
	}
	if (!Pending.IsEmpty())
	{
		if (!IFileManager::Get().FileExists(*Pending) && ++PendingTicks < 6000)
		{
			return;
		}
		if (PendingTicks >= 6000)
		{
			Fail(FString::Printf(TEXT("%s never arrived."), *Pending));
			return;
		}
		Pending.Reset();
		if (++ViewIndex >= Views.Num())
		{
			ViewIndex = 0;
			if (++TimeIndex >= Times.Num())
			{
				UE_LOG(LogForgottenThird, Display, TEXT("FTPoseShots: %d stills saved; quitting."), Times.Num() * Views.Num());
				bActive = false;
				FTRequestExit(0, TEXT("FTPoseShots"));
				return;
			}
			Pose(Times[TimeIndex]);
		}
		Aim(ViewIndex);
		Wait = FTPoseShots::kSettle;
		return;
	}
	if (Wait > 0)
	{
		// The pose holds while the scene settles (the clip doesn't play).
		if (--Wait == 0)
		{
			IStreamingManager::Get().StreamAllResources(10.f);
		}
		return;
	}
	// Setup aims before the window has its size (it reads 0x0 there, so the angle falls back to the full 30 degrees and
	// the first still came out framed too tight): aim again, and settle, whenever the framed angle has moved by more than
	// kReaimDeg since.
	if (const double Half = FTPoseShots::HalfAngle(GetWorld(), Fov);
		FMath::Abs(FMath::RadiansToDegrees(Half - AimedHalf) * 2.0) > FTPoseShots::kReaimDeg)
	{
		const FIntPoint Window = FTPoseShots::WindowSize(GetWorld());
		UE_LOG(LogForgottenThird, Display, TEXT("FTPoseShots: window now %dx%d; aimed again, framed to %.1f degrees (was %.1f)."),
			Window.X, Window.Y, FMath::RadiansToDegrees(Half) * 2.0, FMath::RadiansToDegrees(AimedHalf) * 2.0);
		Aim(ViewIndex);
		Wait = FTPoseShots::kSettle;
		return;
	}
	const double T = Times[TimeIndex];
	const FString File = OutDir / FT::ToFString(ftcore::animimport::StillName(TCHAR_TO_UTF8(*StillBase), T,
		TCHAR_TO_UTF8(*Views[ViewIndex].Name)));
	IFileManager::Get().Delete(*File, false, true, true);
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot %s filename=\"%s\""), *Size, *File), *GLog);
	}
	const double Rate = Clip ? Clip->GetSamplingFrameRate().AsDecimal() : 30.0;
	// frame: the clip's own key, from 0 (a source scene starting at frame 1 adds 1).
	UE_LOG(LogForgottenThird, Display, TEXT("FTPoseShots: still %s t=%.3f frame=%d view=%s yaw=%.0f pitch=%.0f %s"), *File, T,
		ftcore::animimport::FrameAt(T, Rate), *Views[ViewIndex].Name, Views[ViewIndex].Yaw, Views[ViewIndex].Pitch, *Describe);
	Pending = File;
	PendingTicks = 0;
}
#endif // UE_BUILD_SHIPPING
