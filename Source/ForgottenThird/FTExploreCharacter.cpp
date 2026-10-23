#include "FTExploreCharacter.h"

#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "UObject/ConstructorHelpers.h"

#include "FTExploreDirector.h"
#include "FTMeshFacing.h"
#include "ForgottenThird.h"

namespace
{
// Walk clip below this ground speed (cm/s) is idle.
constexpr float kWalkThreshold = 25.f;
}

AFTExploreCharacter::AFTExploreCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	GetCapsuleComponent()->InitCapsuleSize(34.f, 88.f);

	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->bOrientRotationToMovement = true;
	Move->RotationRate = FRotator(0.0, 540.0, 0.0);
	Move->MaxWalkSpeed = WalkSpeed;
	Move->JumpZVelocity = 520.f;
	Move->AirControl = 0.35f;
	Move->BrakingDecelerationWalking = 2000.f;

	Boom = CreateDefaultSubobject<USpringArmComponent>(TEXT("Boom"));
	Boom->SetupAttachment(RootComponent);
	Boom->TargetArmLength = 460.f;
	Boom->SocketOffset = FVector(0.0, 0.0, 70.0);
	Boom->bUsePawnControlRotation = true;
	Boom->bEnableCameraLag = true;
	Boom->CameraLagSpeed = 12.f;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Boom, USpringArmComponent::SocketName);
	Camera->SetRelativeRotation(FRotator(-8.0, 0.0, 0.0));
	Camera->bUsePawnControlRotation = false;
	Camera->SetFieldOfView(70.f);

	static ConstructorHelpers::FObjectFinder<USkeletalMesh> Devin(TEXT("/Game/Characters/Devin/devin/SkeletalMeshes/devin.devin"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> Idle(TEXT("/Game/Characters/Devin/devin/SkeletalMeshes/devinidle.devinidle"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> Walk(TEXT("/Game/Characters/Devin/devin/SkeletalMeshes/devinwalk.devinwalk"));
	// The mesh's origin is at its feet; the capsule's is at its middle.
	GetMesh()->SetRelativeLocation(FVector(0.0, 0.0, -88.0));
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (Devin.Object)
	{
		GetMesh()->SetSkeletalMesh(Devin.Object);
	}
	IdleClip = Idle.Object;
	WalkClip = Walk.Object;
}

void AFTExploreCharacter::BeginPlay()
{
	Super::BeginPlay();
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
	ApplyLookOverrides();
	if (USkeletalMesh* Body = GetMesh()->GetSkeletalMeshAsset())
	{
		// Turn the body to face the capsule's forward (+X).
		GetMesh()->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Body), 0.0));
	}
	GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	UpdateClip();
	if (FParse::Param(FCommandLine::Get(), TEXT("ExplorePortrait")))
	{
		// For looking at the face: the camera sits in front of the pawn at head height and stays there.
		Boom->bUsePawnControlRotation = false;
		Boom->bEnableCameraLag = false;
		Boom->SetRelativeRotation(FRotator(0.0, 180.0, 0.0));
		Boom->TargetArmLength = 110.f;
		Boom->SocketOffset = FVector(0.0, 0.0, 78.0);
		Camera->SetRelativeRotation(FRotator(-4.0, 0.0, 0.0));
	}
}

void AFTExploreCharacter::ApplyLookOverrides()
{
	if (!BodyMeshPath.IsNull())
	{
		if (USkeletalMesh* Body = Cast<USkeletalMesh>(BodyMeshPath.TryLoad()))
		{
			GetMesh()->SetSkeletalMesh(Body);
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: no skeletal mesh at %s; keeping the default body."), *BodyMeshPath.ToString());
		}
	}
	auto LoadClip = [](const FSoftObjectPath& Path, TObjectPtr<UAnimSequence>& Clip)
	{
		if (Path.IsNull())
		{
			return;
		}
		if (UAnimSequence* Loaded = Cast<UAnimSequence>(Path.TryLoad()))
		{
			Clip = Loaded;
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: no anim sequence at %s."), *Path.ToString());
		}
	};
	LoadClip(IdleClipPath, IdleClip);
	LoadClip(WalkClipPath, WalkClip);
	int32 Cloth = 0;
	for (const FSoftObjectPath& Path : PiecePaths)
	{
		USkeletalMesh* PieceMesh = Cast<USkeletalMesh>(Path.TryLoad());
		if (!PieceMesh)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: no skeletal mesh at %s; piece skipped."), *Path.ToString());
			continue;
		}
		USkeletalMeshComponent* Piece = NewObject<USkeletalMeshComponent>(this);
		Piece->SetupAttachment(GetMesh());
		Piece->SetSkeletalMesh(PieceMesh);
		Piece->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Piece->bCollideWithEnvironment = true; // cloth pieces (FTBuildCloth) also push off the level, not just the body
		Piece->RegisterComponent();
		Cloth += PieceMesh->GetMeshClothingAssets().Num() > 0;
		Piece->SetLeaderPoseComponent(GetMesh());
		Pieces.Add(Piece);
	}
	const USkeletalMesh* Body = GetMesh()->GetSkeletalMeshAsset();
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: body %s, %d pieces, %d with cloth."), Body ? *Body->GetName() : TEXT("none"),
		Pieces.Num(), Cloth);
}

void AFTExploreCharacter::CreateInputObjects()
{
	if (Context)
	{
		return;
	}
	Context = NewObject<UInputMappingContext>(this, TEXT("IMC_Explore"));

	MoveAction = MakeAction(TEXT("IA_ExploreMove"), true);
	// Keys give X = right, Y = forward. W and S swizzle their 1D value into Y.
	auto MapMove = [this](const FKey& Key, bool bSwizzle, bool bNegate)
	{
		FEnhancedActionKeyMapping& Mapping = Context->MapKey(MoveAction, Key);
		if (bSwizzle)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(Context));
		}
		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(Context));
		}
	};
	MapMove(EKeys::W, true, false);
	MapMove(EKeys::S, true, true);
	MapMove(EKeys::D, false, false);
	MapMove(EKeys::A, false, true);
	MapMove(EKeys::Up, true, false);
	MapMove(EKeys::Down, true, true);
	MapMove(EKeys::Right, false, false);
	MapMove(EKeys::Left, false, true);
	MapMove(EKeys::Gamepad_Left2D, false, false);

	LookAction = MakeAction(TEXT("IA_ExploreLook"), true);
	Context->MapKey(LookAction, EKeys::Mouse2D);
	Context->MapKey(LookAction, EKeys::Gamepad_Right2D);

	JumpAction = MakeAction(TEXT("IA_ExploreJump"), false);
	Context->MapKey(JumpAction, EKeys::SpaceBar);
	Context->MapKey(JumpAction, EKeys::Gamepad_FaceButton_Bottom);

	RunAction = MakeAction(TEXT("IA_ExploreRun"), false);
	Context->MapKey(RunAction, EKeys::LeftShift);
	Context->MapKey(RunAction, EKeys::Gamepad_LeftTrigger);

	InteractAction = MakeAction(TEXT("IA_ExploreInteract"), false);
	Context->MapKey(InteractAction, EKeys::E);
	Context->MapKey(InteractAction, EKeys::Enter);
	Context->MapKey(InteractAction, EKeys::Gamepad_FaceButton_Left);
}

UInputAction* AFTExploreCharacter::MakeAction(const TCHAR* Name, bool bAxis2D)
{
	UInputAction* Action = NewObject<UInputAction>(this, Name);
	Action->ValueType = bAxis2D ? EInputActionValueType::Axis2D : EInputActionValueType::Boolean;
	return Action;
}

void AFTExploreCharacter::PawnClientRestart()
{
	Super::PawnClientRestart();
	CreateInputObjects();
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			PC ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()) : nullptr)
	{
		// Above the story context (priority 0), so Space jumps rather than advancing nothing.
		Subsystem->AddMappingContext(Context, 2);
	}
}

void AFTExploreCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	CreateInputObjects();
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		return;
	}
	Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AFTExploreCharacter::HandleMove);
	Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &AFTExploreCharacter::HandleLook);
	Input->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
	Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
	Input->BindAction(RunAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleRunStart);
	Input->BindAction(RunAction, ETriggerEvent::Completed, this, &AFTExploreCharacter::HandleRunStop);
	Input->BindAction(InteractAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleInteract);
}

void AFTExploreCharacter::HandleMove(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (!Controller)
	{
		return;
	}
	const FRotator Yaw(0.0, Controller->GetControlRotation().Yaw, 0.0);
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::X), Axis.Y);
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y), Axis.X);
}

void AFTExploreCharacter::HandleLook(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(-Axis.Y);
}

void AFTExploreCharacter::HandleRunStart()
{
	GetCharacterMovement()->MaxWalkSpeed = RunSpeed;
}

void AFTExploreCharacter::HandleRunStop()
{
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}

void AFTExploreCharacter::HandleInteract()
{
	if (AFTExploreDirector* D = GetDirector())
	{
		D->Interact();
	}
}

AFTExploreDirector* AFTExploreCharacter::GetDirector()
{
	if (!Director.IsValid())
	{
		for (TActorIterator<AFTExploreDirector> It(GetWorld()); It; ++It)
		{
			Director = *It;
			break;
		}
	}
	return Director.Get();
}

FVector2D AFTExploreCharacter::GetFacing2D() const
{
	const FVector F = GetActorForwardVector();
	return FVector2D(F.X, F.Y).GetSafeNormal();
}

void AFTExploreCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateClip();
}

void AFTExploreCharacter::UpdateClip()
{
	USkeletalMeshComponent* Body = GetMesh();
	const float Speed = static_cast<float>(GetVelocity().Size2D());
	const bool bWantWalk = Speed > kWalkThreshold && GetCharacterMovement()->IsMovingOnGround();
	if (!bClipStarted || bWantWalk != bWalking)
	{
		UAnimSequence* Clip = bWantWalk ? WalkClip.Get() : IdleClip.Get();
		if (Clip)
		{
			Body->PlayAnimation(Clip, true);
		}
		bWalking = bWantWalk;
		bClipStarted = true;
	}
	Body->SetPlayRate(bWalking ? FMath::Clamp(Speed / WalkClipSpeed, 0.5f, 3.f) : 1.f);
}
