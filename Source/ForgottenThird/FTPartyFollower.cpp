#include "FTPartyFollower.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"

#include "FTMeshFacing.h"
#include "FTMeshPieces.h"
#include "ForgottenThird.h"

namespace
{
// Slower than this (cm/s) counts as standing still. A speed, not a distance: at a high frame rate a walking
// follower moves well under a centimetre a frame.
constexpr float kStillSpeed = 10.f;
// The time constant (s) of the follower's turn toward the way it moves, as the lead's facing eases (LocoTuning::faceTime).
constexpr float kFaceTime = 0.15f;
// And never faster than this (deg/s): a walking body's turn, not a spin.
constexpr float kMaxFaceRate = 220.f;
// How fast the speed fed to the anim follows each frame's step (1/s), so uneven frames don't jitter the stride.
constexpr float kSpeedResponse = 12.f;
// Asked to face a way, it has once it is this near (degrees): nearer than LocoTuning::faceMin, no turn on the spot starts.
constexpr float kFacedWithin = 40.f;

// An object path from a config or command-line value; a bare package path names its asset after the package.
FSoftObjectPath ObjectPath(FString Path)
{
	Path.TrimStartAndEndInline();
	if (!Path.IsEmpty() && !Path.Contains(TEXT(".")))
	{
		Path += TEXT(".") + FPackageName::GetShortName(Path);
	}
	return FSoftObjectPath(Path);
}
}

AFTPartyFollower::AFTPartyFollower()
{
	PrimaryActorTick.bCanEverTick = false;
	// The body hangs off its own root: as the root, the actor's turn would overwrite the mesh's facing offset.
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetGenerateOverlapEvents(false);
	Body->SetupAttachment(GetRootComponent());
	SetActorEnableCollision(false);
	TraceParams = FCollisionQueryParams(FName(TEXT("FTPartyFollower")), false);
}

bool AFTPartyFollower::Setup(const FString& Name, const AActor* Lead)
{
	const FString Lower = Name.ToLower();
	const FString Dir = FString::Printf(TEXT("/Game/Characters/%s/%s/SkeletalMeshes/"), *Name, *Lower);
	// Config overrides under [FTPartyFollower.<Name>], then -Follower<Name><Key>= on the command line.
	const FString Section = TEXT("FTPartyFollower.") + Name;
	auto Override = [&](const TCHAR* Key, FString& Value)
	{
		GConfig->GetString(*Section, Key, Value, GGameIni);
		FParse::Value(FCommandLine::Get(), *FString::Printf(TEXT("Follower%s%s="), *Name, Key), Value);
	};
	FString BodyPath = Dir + Lower;
	FString ClipFolder = FString::Printf(TEXT("/Game/Characters/%s/Rig/Retargeted"), *Name);
	FString PieceArg;
	TArray<FString> PieceList;
	Override(TEXT("BodyMeshPath"), BodyPath);
	Override(TEXT("ClipFolder"), ClipFolder);
	GConfig->GetArray(*Section, TEXT("PiecePaths"), PieceList, GGameIni);
	if (FParse::Value(FCommandLine::Get(), *FString::Printf(TEXT("Follower%sPiecePaths="), *Name), PieceArg, false))
	{
		PieceArg.ParseIntoArray(PieceList, TEXT("+"));
	}
	const FSoftObjectPath BodyAsset = ObjectPath(BodyPath);
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(BodyAsset.TryLoad());
	TraceParams.AddIgnoredActor(this);
	if (Lead)
	{
		TraceParams.AddIgnoredActor(Lead);
	}
	if (!FParse::Param(FCommandLine::Get(), TEXT("LocoNoPhaseTrim")))
	{
		LeadCharacter = Cast<ACharacter>(Lead);
	}
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("PartyFollower %s: no skeletal mesh at %s."), *Name, *BodyAsset.ToString());
		SetActorHiddenInGame(true);
		return false;
	}
	Body->SetSkeletalMesh(Mesh);
	// Turn the body to face the actor's forward (+X).
	Body->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Mesh), 0.0));
	Body->SetAnimInstanceClass(UFTLocomotionAnimInstance::StaticClass());
	TArray<FSoftObjectPath> PiecePaths;
	for (const FString& Piece : PieceList)
	{
		PiecePaths.Add(ObjectPath(Piece));
	}
	const FString Who = TEXT("PartyFollower ") + Name;
	const int32 Cloth = FTMeshPieces::Attach(*this, *Body, PiecePaths, Pieces, *Who);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: body %s facing %.0f, %d pieces (%d with cloth), clips from %s."), *Who, *Mesh->GetName(),
		Body->GetRelativeRotation().Yaw, Pieces.Num(), Cloth, *ClipFolder);
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());
	if (!Anim)
	{
		return true;
	}
	// Retargeted clips first (the mocap sample's, then the template's: FTBuildRig names them after the
	// mesh), then the mesh's own idle and walk.
	const FString Rig = ClipFolder.EndsWith(TEXT("/")) ? ClipFolder : ClipFolder + TEXT("/");
	const FString Target = Mesh->GetName();
	auto Load = [&](std::initializer_list<const TCHAR*> Retargeted, const TCHAR* Own) -> UAnimSequence*
	{
		for (const TCHAR* Source : Retargeted)
		{
			const FString Asset = FString::Printf(TEXT("%s_to_%s"), Source, *Target);
			if (UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *(Rig + Asset + TEXT(".") + Asset), nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				return Clip;
			}
		}
		return Own ? LoadObject<UAnimSequence>(nullptr, *(Dir + Lower + Own + TEXT(".") + Lower + Own), nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
	};
	Anim->SetClip(ftcore::LocoClip::Idle, Load({TEXT("M_Neutral_Stand_Idle_Loop"), TEXT("MM_Idle")}, TEXT("idle")));
	Anim->SetClip(ftcore::LocoClip::Walk, Load({TEXT("M_Neutral_Walk_Loop_F"), TEXT("MF_Unarmed_Walk_Fwd")}, TEXT("walk")));
	Anim->SetClip(ftcore::LocoClip::Run, Load({TEXT("M_Neutral_Run_Loop_F"), TEXT("MF_Unarmed_Jog_Fwd")}, nullptr));
	Anim->SetClip(ftcore::LocoClip::Sprint, Load({TEXT("M_Neutral_Sprint_Loop_F")}, nullptr));
	// The lead's jumps, replayed on the trail.
	Anim->SetClip(ftcore::LocoClip::JumpStart, Load({TEXT("M_Neutral_Jump_F_Start_Stand_Lfoot"), TEXT("MM_Jump")}, nullptr));
	Anim->SetClip(ftcore::LocoClip::Fall, Load({TEXT("M_Neutral_Jump_Loop_Fall"), TEXT("MM_Fall_Loop")}, nullptr));
	Anim->SetClip(ftcore::LocoClip::Land, Load({TEXT("M_Neutral_Jump_F_Land_Stand_Light_Lfoot")}, nullptr));
	Anim->SetClip(ftcore::LocoClip::JumpMoving, Load({TEXT("M_Neutral_Jump_F_Start_Run_Lfoot")}, nullptr));
	Anim->SetClip(ftcore::LocoClip::LandMoving, Load({TEXT("M_Neutral_Jump_F_Land_Run_Light_Lfoot")}, nullptr));
	Anim->SetMovesFromGait(ftcore::LocoClip::Walk);
	Anim->SetMovesFromGait(ftcore::LocoClip::Run);
	Anim->SetMovesFromGait(ftcore::LocoClip::Sprint);
	Anim->SetTurnsFromRun();
	Anim->SetAirPoseFromRun();
	return true;
}

void AFTPartyFollower::Place(const FVector2D& Ground, float LeadFeetZ, float DeltaSeconds, float WantSpeed, float StopDistance)
{
	const FVector2D Move = bPlaced ? Ground - LastGround : FVector2D::ZeroVector;
	const float Step = DeltaSeconds > 0.f ? static_cast<float>(Move.Size()) / DeltaSeconds : 0.f;
	const bool bWantWalk = Step > kStillSpeed;
	LastGround = Ground;
	bPlaced = true;
	// Before it moves: the pose and the placement are both last frame's.
	SlideProbe.Sample(*Body, DeltaSeconds, Speed);

	double Z = LeadFeetZ;
	if (const UWorld* World = GetWorld())
	{
		FHitResult Hit;
		const FVector From(Ground.X, Ground.Y, LeadFeetZ + 150.0);
		const FVector To(Ground.X, Ground.Y, LeadFeetZ - 300.0);
		if (World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, TraceParams))
		{
			Z = Hit.ImpactPoint.Z;
		}
	}
	// A replayed jump: a hop off the ground below, back down onto it.
	if (bAir && DeltaSeconds > 0.f)
	{
		AirHeight += AirSpeed * DeltaSeconds - 0.5f * AirGravity * DeltaSeconds * DeltaSeconds;
		AirSpeed -= AirGravity * DeltaSeconds;
		if (AirHeight <= 0.f && AirSpeed < 0.f)
		{
			bAir = false;
			AirHeight = AirSpeed = 0.f;
		}
	}
	Z += AirHeight;
	FRotator Rot = GetActorRotation();
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());
	const ftcore::Locomotion* Loco = Anim ? &Anim->GetLocomotion() : nullptr;
	if (Loco && Loco->turning())
	{
		// A replayed turn (running, or on the spot) turns the body by its clip's root, as it turned the lead.
		if (Loco->turnsBegun() != TurnsSeen)
		{
			TurnsSeen = Loco->turnsBegun();
			TurnEntryYaw = static_cast<float>(Rot.Yaw) - Loco->turnYaw();
		}
		Rot.Yaw = TurnEntryYaw + Loco->turnYaw();
	}
	else if (bWantWalk)
	{
		// Ease toward the trail rather than snapping to each frame's step, so a weaving lead doesn't jerk it round,
		// and no faster than a body turns as it walks (setting off well away from its facing, after a stand turn).
		const float Want = FMath::RadiansToDegrees(FMath::Atan2(Move.Y, Move.X));
		const float Ease = DeltaSeconds > 0.f ? 1.f - FMath::Exp(-DeltaSeconds / kFaceTime) : 1.f;
		const float MaxStep = kMaxFaceRate * DeltaSeconds;
		Rot.Yaw += FMath::Clamp(FMath::FindDeltaAngleDegrees(static_cast<float>(Rot.Yaw), Want) * Ease, -MaxStep, MaxStep);
	}
	SetActorLocationAndRotation(FVector(Ground.X, Ground.Y, Z), Rot, false, nullptr, ETeleportType::TeleportPhysics);
	// Facing where it was asked to, or setting off, it no longer turns on the spot.
	if (bFacing && (bWantWalk || FMath::Abs(FMath::FindDeltaAngleDegrees(static_cast<float>(Rot.Yaw), FaceYaw)) < kFacedWithin))
	{
		bFacing = false;
	}

	if (DeltaSeconds > 0.f)
	{
		Speed += ((bWantWalk ? Step : 0.f) - Speed) * (1.f - FMath::Exp(-DeltaSeconds * kSpeedResponse));
	}
	if (Anim)
	{
		Anim->SetGroundSpeed(Speed);
		Anim->SetIntent(WantSpeed, StopDistance);
		Anim->SetAir(bAir, AirSpeed);
		Anim->SetFace(bFacing, FaceYaw);
		// The lead's foot phase while it holds a gait, so the two don't step together.
		float LeadPhase = -1.f;
		const USkeletalMeshComponent* LeadMesh = LeadCharacter.IsValid() ? LeadCharacter->GetMesh() : nullptr;
		if (const UFTLocomotionAnimInstance* LeadAnim = LeadMesh ? Cast<UFTLocomotionAnimInstance>(LeadMesh->GetAnimInstance()) : nullptr)
		{
			const ftcore::Locomotion& LeadLoco = LeadAnim->GetLocomotion();
			if (LeadLoco.state() == ftcore::LocoState::Ground && LeadCharacter->GetVelocity().Size2D() >= LeadLoco.tuning().moveStop)
			{
				LeadPhase = LeadLoco.phase();
			}
		}
		Anim->SetLeadPhase(LeadPhase);
	}
}

void AFTPartyFollower::ReplayJump(float UpSpeed, float Gravity)
{
	if (bAir || UpSpeed <= 0.f || Gravity <= 0.f)
	{
		return;
	}
	bAir = true;
	AirHeight = 0.f;
	AirSpeed = UpSpeed;
	AirGravity = Gravity;
}

void AFTPartyFollower::FaceTo(float Yaw)
{
	bFacing = true;
	FaceYaw = Yaw;
}

void AFTPartyFollower::ReplayTurn(float Delta)
{
	if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance()))
	{
		Anim->ReplayTurn(Delta);
	}
}
