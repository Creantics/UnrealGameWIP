#include "FTArenaFighter.h"

#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/SpringArmComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

#include "FTBeatFighterAnimInstance.h"
#include "FTLocomotionAnimInstance.h"
#include "FTMeshFacing.h"
#include "ForgottenThird.h"

#include "ftcore/ArenaFight.h"

namespace
{
const TCHAR* const kArenaHumanMesh = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_human.test_human");
const TCHAR* const kArenaHumanIdle = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_humanidle.test_humanidle");
const TCHAR* const kArenaHumanWalk = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_humanwalk.test_humanwalk");

// The test human is about this tall (cm), and its walk clip covers about this much ground a second (m/s).
constexpr float kArenaHumanHeight = 178.f;
constexpr float kArenaHumanWalkSpeed = 1.4f;
// Above this ground speed (m/s) the human walks.
constexpr float kArenaWalkFrom = 0.6f;
// A lead's body is about this tall when its mesh says nothing sensible (cm), and never taller or shorter than these.
constexpr float kArenaLeadHeight = 175.f;
constexpr float kArenaLeadMinHeight = 140.f;
constexpr float kArenaLeadMaxHeight = 210.f;

const FLinearColor kArenaBlade(0.75f, 0.78f, 0.82f);
const FLinearColor kArenaShield(0.32f, 0.36f, 0.42f);
const FLinearColor kArenaShieldParry(1.f, 0.85f, 0.3f);
const FLinearColor kFighterTellBlock(1.f, 0.55f, 0.1f);
const FLinearColor kFighterTellDodge(1.f, 0.12f, 0.08f);
const FLinearColor kFighterTellFlash(1.f, 1.f, 0.85f);

// The arm at rest: the blade held low and a little to the right of straight ahead.
const FRotator kArenaArmRest(-60.0, 20.0, 0.0);

float ArenaEase(float T)
{
	T = FMath::Clamp(T, 0.f, 1.f);
	return T * T * (3.f - 2.f * T);
}

FRotator FighterLerp(const FRotator& A, const FRotator& B, float T)
{
	return FRotator(FMath::Lerp(A.Pitch, B.Pitch, static_cast<double>(T)), FMath::Lerp(A.Yaw, B.Yaw, static_cast<double>(T)),
		FMath::Lerp(A.Roll, B.Roll, static_cast<double>(T)));
}

bool ArenaSameColor(const FLinearColor& A, const FLinearColor& B)
{
	return FMath::Abs(A.R - B.R) < 0.004f && FMath::Abs(A.G - B.G) < 0.004f && FMath::Abs(A.B - B.B) < 0.004f;
}

// Where the arm points through each part of a swing: wound (end of startup) and through (end of active).
void ArenaSwingKeys(EFTArenaSwing Swing, FRotator& OutWound, FRotator& OutThrough)
{
	switch (Swing)
	{
	case EFTArenaSwing::Across:
		OutWound = FRotator(-5.0, 115.0, 0.0);
		OutThrough = FRotator(-15.0, -85.0, 0.0);
		break;
	case EFTArenaSwing::Backhand:
		OutWound = FRotator(-5.0, -85.0, 0.0);
		OutThrough = FRotator(-15.0, 115.0, 0.0);
		break;
	case EFTArenaSwing::Overhead:
		OutWound = FRotator(125.0, 0.0, 0.0);
		OutThrough = FRotator(-55.0, 0.0, 0.0);
		break;
	case EFTArenaSwing::Thrust:
	default:
		OutWound = FRotator(-8.0, 0.0, 0.0);
		OutThrough = FRotator(-12.0, 0.0, 0.0);
		break;
	}
}

UStaticMeshComponent* ArenaPart(AFTArenaFighter* Owner, const TCHAR* Name, USceneComponent* Parent, UStaticMesh* Mesh)
{
	UStaticMeshComponent* Part = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
	Part->SetupAttachment(Parent);
	Part->SetStaticMesh(Mesh);
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetGenerateOverlapEvents(false);
	Part->SetCanEverAffectNavigation(false);
	return Part;
}
} // namespace

AFTArenaFighter::AFTArenaFighter()
{
	PrimaryActorTick.bCanEverTick = false;
	// Core owns every facing: the controller's rotation only aims the camera.
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;
	AutoPossessAI = EAutoPossessAI::Disabled;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	ShapeMaterial = MaterialFinder.Object;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	Root->SetMobility(EComponentMobility::Movable);
	Visual = CreateDefaultSubobject<USceneComponent>(TEXT("Visual"));
	Visual->SetupAttachment(Root);

	Body = ArenaPart(this, TEXT("Body"), Visual, CylinderFinder.Object);
	Head = ArenaPart(this, TEXT("Head"), Visual, SphereFinder.Object);
	Visor = ArenaPart(this, TEXT("Visor"), Visual, CubeFinder.Object);
	Arm = CreateDefaultSubobject<USceneComponent>(TEXT("Arm"));
	Arm->SetupAttachment(Visual);
	Blade = ArenaPart(this, TEXT("Blade"), Arm, CubeFinder.Object);
	Shield = ArenaPart(this, TEXT("Shield"), Visual, CubeFinder.Object);
	for (int32 I = 0; I < TellSegments; ++I)
	{
		Tell[I] = ArenaPart(this, *FString::Printf(TEXT("Tell%d"), I), Root, CubeFinder.Object);
		Tell[I]->SetCastShadow(false);
	}

	Skin = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Skin"));
	Skin->SetupAttachment(Visual);
	Skin->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Skin->SetGenerateOverlapEvents(false);
	// The mesh faces +Y; the figure faces +X.
	Skin->SetRelativeRotation(FRotator(0.0, -90.0, 0.0));
	Skin->SetVisibility(false);

	// The camera: over the right shoulder, aimed by the controller's rotation (the game mode steers it).
	Boom = CreateDefaultSubobject<USpringArmComponent>(TEXT("Boom"));
	Boom->SetupAttachment(Root);
	Boom->SetRelativeLocation(FVector(0.0, 0.0, 150.0));
	Boom->TargetArmLength = 640.f;
	Boom->SocketOffset = FVector(0.0, 120.0, 70.0);
	Boom->bUsePawnControlRotation = true;
	// The camera may leave the zone's walls but not pass through the scene: the arm probes the world and is pulled in.
	Boom->bDoCollisionTest = true;
	Boom->ProbeSize = 12.f;
	Boom->ProbeChannel = ECC_Camera;
	Boom->bEnableCameraLag = true;
	Boom->CameraLagSpeed = 9.f;
	Boom->bEnableCameraRotationLag = true;
	Boom->CameraRotationLagSpeed = 10.f;
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Boom, USpringArmComponent::SocketName);
	Camera->SetFieldOfView(70.f);
}

void AFTArenaFighter::SetCameraRig(float ArmMeters, float PivotMeters, float SideMeters, float UpMeters, float FovDegrees)
{
	Boom->TargetArmLength = ArmMeters * 100.f;
	Boom->SetRelativeLocation(FVector(0.0, 0.0, PivotMeters * 100.f));
	Boom->SocketOffset = FVector(0.0, SideMeters * 100.f, UpMeters * 100.f);
	Camera->SetFieldOfView(FovDegrees);
}

void AFTArenaFighter::Setup(bool bPlayer, float BodyRadius, const FLinearColor& InBaseColor, float HeightMeters, const FFTLeadLook* Look)
{
	BaseColor = InBaseColor;
	Radius = FMath::Clamp(BodyRadius, 0.2f, 6.f) * 100.f;
	// A wider body stands taller: 175 cm at Core's default 0.4 m, up to 1.6x that, unless the data gives a height.
	Height = HeightMeters > 0.f ? FMath::Clamp(HeightMeters, 0.5f, 30.f) * 100.f : FMath::Clamp(Radius / 40.f, 0.8f, 1.6f) * 175.f;

	SkinKind = ESkin::Shapes;
	bool bLead = false;
	if (bPlayer)
	{
		bLead = Look && WearLook(*Look);
		if (Look && !bLead)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTArenaFighter: the look %s has no body; using the test human."), *Look->Name);
		}
		// Quietly: a checkout without the test human plays with the body of shapes.
		HumanMesh = LoadObject<USkeletalMesh>(nullptr, kArenaHumanMesh, nullptr, LOAD_NoWarn | LOAD_Quiet);
		HumanIdle = LoadObject<UAnimSequence>(nullptr, kArenaHumanIdle, nullptr, LOAD_NoWarn | LOAD_Quiet);
		HumanWalk = LoadObject<UAnimSequence>(nullptr, kArenaHumanWalk, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!bLead && HumanMesh && HumanIdle && HumanWalk)
		{
			SkinKind = ESkin::TestHuman;
		}
	}
	else
	{
		// An enemy is never looked through.
		Camera->Deactivate();
	}

	if (ShapeMaterial)
	{
		BodyMaterial = UMaterialInstanceDynamic::Create(ShapeMaterial, this);
		BladeMaterial = UMaterialInstanceDynamic::Create(ShapeMaterial, this);
		ShieldMaterial = UMaterialInstanceDynamic::Create(ShapeMaterial, this);
		TellMaterial = UMaterialInstanceDynamic::Create(ShapeMaterial, this);
		Body->SetMaterial(0, BodyMaterial);
		Head->SetMaterial(0, BodyMaterial);
		Blade->SetMaterial(0, BladeMaterial);
		Shield->SetMaterial(0, ShieldMaterial);
		for (int32 I = 0; I < TellSegments; ++I)
		{
			Tell[I]->SetMaterial(0, TellMaterial);
		}
		BladeMaterial->SetVectorParameterValue(TEXT("Color"), kArenaBlade);
		UMaterialInstanceDynamic* VisorMaterial = UMaterialInstanceDynamic::Create(ShapeMaterial, this);
		VisorMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.03f, 0.03f, 0.04f));
		Visor->SetMaterial(0, VisorMaterial);
	}

	// The body of shapes: a cylinder to the shoulders, a head, a visor on the front of it (which way it faces).
	const float BodyTall = Height * 0.8f;
	const float HeadSize = Height * 0.2f;
	Body->SetRelativeLocation(FVector(0.0, 0.0, BodyTall * 0.5f));
	Body->SetRelativeScale3D(FVector(Radius * 2.f / 100.f, Radius * 2.f / 100.f, BodyTall / 100.f));
	Head->SetRelativeLocation(FVector(0.0, 0.0, BodyTall + HeadSize * 0.45f));
	Head->SetRelativeScale3D(FVector(HeadSize / 100.f));
	Visor->SetRelativeLocation(FVector(HeadSize * 0.4f, 0.0, BodyTall + HeadSize * 0.5f));
	Visor->SetRelativeScale3D(FVector(0.08f, HeadSize * 0.7f / 100.f, HeadSize * 0.22f / 100.f));
	if (SkinKind == ESkin::TestHuman)
	{
		Height = kArenaHumanHeight;
		Body->SetVisibility(false);
		Head->SetVisibility(false);
		Visor->SetVisibility(false);
		Skin->SetSkeletalMesh(HumanMesh);
		Skin->SetVisibility(true);
		Skin->PlayAnimation(HumanIdle, true);
		bWalking = false;
	}
	else if (bLead)
	{
		// WearLook set the body, the pieces and the clips; the height comes from its mesh.
		const USkeletalMesh* Mesh = Skin->GetSkeletalMeshAsset();
		Height = Mesh ? FMath::Clamp(static_cast<float>(Mesh->GetBounds().BoxExtent.Z) * 2.f, kArenaLeadMinHeight, kArenaLeadMaxHeight)
					  : kArenaLeadHeight;
		Body->SetVisibility(false);
		Head->SetVisibility(false);
		Visor->SetVisibility(false);
		Skin->SetVisibility(true);
	}

	// The weapon: from the right shoulder, a blade a little over half the body's height.
	const float BladeLength = bPlayer ? 115.f : Height * 0.6f;
	Arm->SetRelativeLocation(FVector(0.0, Radius * 0.95f, Height * 0.68f));
	Arm->SetRelativeRotation(kArenaArmRest);
	Blade->SetRelativeLocation(FVector(BladeLength * 0.5f, 0.0, 0.0));
	Blade->SetRelativeScale3D(FVector(BladeLength / 100.f, bPlayer ? 0.05f : 0.09f, bPlayer ? 0.12f : 0.18f));
	// The shield: a plate in front of the chest, shown while guarding.
	Shield->SetRelativeLocation(FVector(Radius + 14.f, 0.0, Height * 0.55f));
	Shield->SetRelativeScale3D(FVector(0.07f, 0.7f, 0.85f));
	Shield->SetVisibility(false);
	bShieldShown = false;
	SetTellVisible(false);
}

bool AFTArenaFighter::WearLook(const FFTLeadLook& Look)
{
	const FFTLoadedLook Loaded = FTLook::LoadLook(Look, TEXT("FTArenaFighter"));
	if (!Loaded.Body)
	{
		return false;
	}
	Skin->SetSkeletalMesh(Loaded.Body);
	// Turn the body to face the figure's forward (+X).
	Skin->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Loaded.Body), 0.0));
	Skin->SetAnimInstanceClass(UFTLocomotionAnimInstance::StaticClass());
	const int32 Cloth = FTLook::AddPieces(*this, *Skin, Look.PiecePaths, Pieces, TEXT("FTArenaFighter"));
	FTLook::AddGrooms(*this, *Skin, Pieces, Look.Grooms, Grooms, TEXT("FTArenaFighter"));
	if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Skin->GetAnimInstance()))
	{
		Anim->SetBaseClips(Loaded.Clips, false);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTArenaFighter: no locomotion anim instance; %s stays in the ref pose."), *Look.Name);
	}
	SkinKind = ESkin::Lead;
	UE_LOG(LogForgottenThird, Display, TEXT("FTArenaFighter: %s: body %s facing %.0f, %d pieces (%d with cloth), %d grooms, clips from %s."),
		*Look.Name, *Loaded.Body->GetName(), Skin->GetRelativeRotation().Yaw, Pieces.Num(), Cloth, Grooms.Num(), *Loaded.ClipFolder);
	return true;
}

bool AFTArenaFighter::SetupSkinned(USkeletalMesh* Mesh, float HoverMeters, float Scale, float YawOffset, float HeightMeters)
{
	if (!Mesh)
	{
		return false;
	}
	SkinKind = ESkin::Creature;
	SkinHover = FMath::Max(HoverMeters, 0.f) * 100.f;
	Height = FMath::Max(HeightMeters, 0.5f) * Scale * 100.f + SkinHover;
	// The shapes (the body, head and visor, the weapon arm's blade) are the placeholder: the mesh replaces them.
	Body->SetVisibility(false);
	Head->SetVisibility(false);
	Visor->SetVisibility(false);
	Blade->SetVisibility(false);
	Skin->SetSkeletalMesh(Mesh);
	Skin->SetAnimInstanceClass(UFTBeatFighterAnimInstance::StaticClass());
	Skin->SetRelativeLocation(FVector(0.0, 0.0, SkinHover));
	Skin->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Mesh) + YawOffset, 0.0));
	Skin->SetRelativeScale3D(FVector(Scale));
	Skin->SetVisibility(true);
	return true;
}

void AFTArenaFighter::PlayClip(const UAnimSequence* Clip, float Time, bool bLoop, float DeltaSeconds, bool bHoldRoot)
{
	UFTBeatFighterAnimInstance* Anim = SkinKind == ESkin::Creature ? Cast<UFTBeatFighterAnimInstance>(Skin->GetAnimInstance()) : nullptr;
	if (Anim)
	{
		Anim->SetHoldRoot(bHoldRoot);
		Anim->SetFadeRestart(true); // a second hit while the first one's reaction plays
		Anim->SetPose(Clip, Time, bLoop, DeltaSeconds);
	}
}

void AFTArenaFighter::SetTellVisible(bool bVisible)
{
	if (bTellShown == bVisible)
	{
		return;
	}
	bTellShown = bVisible;
	for (int32 I = 0; I < TellSegments; ++I)
	{
		Tell[I]->SetVisibility(bVisible);
	}
}

FVector AFTArenaFighter::GetAnchor() const
{
	return GetActorLocation() + FVector(0.0, 0.0, Height + 45.0);
}

FVector AFTArenaFighter::GetChest() const
{
	return GetActorLocation() + FVector(0.0, 0.0, Height * 0.6);
}

void AFTArenaFighter::ApplyPose(const FFTArenaPose& Pose)
{
	SetActorLocationAndRotation(Pose.Feet, FRotator(0.0, Pose.Yaw, 0.0));

	// The whole body: lean back, crouch and lean along a roll, or lie flat. A skinned enemy's clips do all that.
	FRotator Lean = FRotator::ZeroRotator;
	FVector Lift = FVector::ZeroVector;
	float Squash = 1.f;
	if (SkinKind == ESkin::Creature)
	{
		// The mesh stands as the clip poses it.
	}
	else if (Pose.bLying)
	{
		Lean.Pitch = 84.0;
		Lift.Z = Radius * 0.7f;
	}
	else
	{
		Lean.Pitch = Pose.LeanBack;
		Lean.Roll = Pose.Wobble;
		if (Pose.bDodging)
		{
			const float Arc = FMath::Sin(PI * FMath::Clamp(Pose.DodgeFraction, 0.f, 1.f));
			Squash = 1.f - 0.42f * Arc;
			Lean.Pitch += -28.0 * Pose.DodgeLocal.X * Arc;
			Lean.Roll += 28.0 * Pose.DodgeLocal.Y * Arc;
		}
	}
	Visual->SetRelativeLocationAndRotation(Lift, Lean);
	Visual->SetRelativeScale3D(FVector(1.0, 1.0, Squash));

	if (SkinKind == ESkin::TestHuman)
	{
		const bool bWalk = Pose.Speed > kArenaWalkFrom && !Pose.bLying;
		if (bWalk != bWalking)
		{
			bWalking = bWalk;
			Skin->PlayAnimation(bWalk ? HumanWalk.Get() : HumanIdle.Get(), true);
		}
		if (bWalking)
		{
			Skin->SetPlayRate(FMath::Clamp(Pose.Speed / kArenaHumanWalkSpeed, 0.6f, 2.4f));
		}
	}
	else if (SkinKind == ESkin::Lead)
	{
		// The locomotion blends idle, walk and run from the ground speed (cm/s); the dodge, the guard and the swings are the
		// figure's own overlay, there being no combat clips for the look yet. An unknown intent plays no starts or stops.
		if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Skin->GetAnimInstance()))
		{
			Anim->SetGroundSpeed(Pose.bLying || Pose.bDodging ? 0.f : Pose.Speed * 100.f);
			Anim->SetIntent(-1.f, -1.f);
		}
	}

	// The arm: rest, or the swing through the attack's parts.
	FRotator ArmRotation = kArenaArmRest;
	FVector ArmReach = FVector::ZeroVector;
	if (Pose.bAttacking)
	{
		FRotator Wound, Through;
		ArenaSwingKeys(Pose.Swing, Wound, Through);
		const float T = ArenaEase(Pose.PartFraction);
		switch (static_cast<ftcore::ArenaPart>(Pose.Part))
		{
		case ftcore::ArenaPart::Startup:
			ArmRotation = FighterLerp(kArenaArmRest, Wound, T);
			if (Pose.Swing == EFTArenaSwing::Thrust)
			{
				ArmReach.X = -30.f * T;
			}
			break;
		case ftcore::ArenaPart::Active:
			ArmRotation = FighterLerp(Wound, Through, T);
			if (Pose.Swing == EFTArenaSwing::Thrust)
			{
				ArmReach.X = FMath::Lerp(-30.f, 45.f, T);
			}
			break;
		case ftcore::ArenaPart::Recovery:
			ArmRotation = FighterLerp(Through, kArenaArmRest, T);
			if (Pose.Swing == EFTArenaSwing::Thrust)
			{
				ArmReach.X = FMath::Lerp(45.f, 0.f, T);
			}
			break;
		default:
			break;
		}
	}
	else if (Pose.bHealing)
	{
		ArmRotation = FRotator(35.0, -40.0, 0.0);
	}
	else if (Pose.bGuard)
	{
		ArmRotation = FRotator(60.0, -10.0, 0.0);
	}
	Arm->SetRelativeLocation(FVector(ArmReach.X, Radius * 0.95f, Height * 0.68f));
	Arm->SetRelativeRotation(ArmRotation);

	const bool bShield = Pose.bGuard && !Pose.bLying;
	if (bShield != bShieldShown)
	{
		bShieldShown = bShield;
		Shield->SetVisibility(bShield);
	}
	if (bShield && ShieldMaterial)
	{
		const FLinearColor ShieldColor = Pose.bParrying ? kArenaShieldParry : kArenaShield;
		if (!ArenaSameColor(ShieldColor, LastShield))
		{
			LastShield = ShieldColor;
			ShieldMaterial->SetVectorParameterValue(TEXT("Color"), ShieldColor);
		}
	}

	if (BodyMaterial && !ArenaSameColor(Pose.Tint, LastTint))
	{
		LastTint = Pose.Tint;
		BodyMaterial->SetVectorParameterValue(TEXT("Color"), Pose.Tint);
	}

	// The floor tell: an arc of short bars at the move's reach, as wide as it sweeps, growing from 40% of the
	// reach to all of it through the startup, then flashing pale as it goes active. A lane (a shot) is two
	// dashed rails instead, its edges, running out from the body and growing the same way.
	const bool bTell = Pose.TellReach > 1.f;
	SetTellVisible(bTell);
	if (bTell && Pose.TellWidth > 1.f)
	{
		const float Reach = Pose.TellReach * (Pose.TellFlash > 0.f ? 1.f : (0.4f + 0.6f * ArenaEase(Pose.TellFraction)));
		const int32 PerRail = TellSegments / 2;
		const float Dash = Reach / static_cast<float>(PerRail);
		for (int32 I = 0; I < TellSegments; ++I)
		{
			const int32 Along = I % PerRail;
			const float Side = (I < PerRail ? 0.5f : -0.5f) * Pose.TellWidth;
			const FVector At(Dash * (static_cast<float>(Along) + 0.5f), Side, 2.0);
			Tell[I]->SetRelativeLocationAndRotation(At, FRotator::ZeroRotator);
			Tell[I]->SetRelativeScale3D(FVector(Dash * 0.7f / 100.f, 0.14f, 0.03f));
		}
	}
	else if (bTell)
	{
		const float Reach = Pose.TellReach * (Pose.TellFlash > 0.f ? 1.f : (0.4f + 0.6f * ArenaEase(Pose.TellFraction)));
		const float Half = FMath::Clamp(Pose.TellHalfAngle, 0.1f, PI);
		const float Step = 2.f * Half / static_cast<float>(TellSegments);
		const float Chord = FMath::Max(10.f, 2.f * Reach * FMath::Sin(Step * 0.5f) + 4.f);
		for (int32 I = 0; I < TellSegments; ++I)
		{
			const float A = -Half + Step * (static_cast<float>(I) + 0.5f);
			const FVector At(Reach * FMath::Cos(A), Reach * FMath::Sin(A), 2.0);
			Tell[I]->SetRelativeLocationAndRotation(At, FRotator(0.0, FMath::RadiansToDegrees(A) + 90.0, 0.0));
			Tell[I]->SetRelativeScale3D(FVector(Chord / 100.f, 0.14f, 0.03f));
		}
	}
	if (bTell)
	{
		if (TellMaterial)
		{
			const FLinearColor Base = Pose.bTellUnblockable ? kFighterTellDodge : kFighterTellBlock;
			const FLinearColor Color = FMath::Lerp(Base, kFighterTellFlash, FMath::Clamp(Pose.TellFlash, 0.f, 1.f));
			if (!ArenaSameColor(Color, LastTell))
			{
				LastTell = Color;
				TellMaterial->SetVectorParameterValue(TEXT("Color"), Color);
			}
		}
	}
}
