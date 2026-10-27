#include "FTExploreCharacter.h"

#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/RectLightComponent.h"
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
#include "FTGameInstance.h"
#include "FTMeshFacing.h"
#include "ForgottenThird.h"

#include "ftcore/BattleDemo.h"

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
	const bool bPortrait = FParse::Param(FCommandLine::Get(), TEXT("ExplorePortrait"));
	// The lead and the party toggle: the player's last choice this session, else the command line, else config. A
	// portrait shows the lead alone.
	FString Lead = StartLead;
	FParse::Value(FCommandLine::Get(), TEXT("ExploreLead="), Lead);
	bPartyHidden = bStartSolo || bPortrait || FParse::Param(FCommandLine::Get(), TEXT("ExploreSolo"));
	if (const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
	{
		Lead = GI->ExploreLead.IsEmpty() ? Lead : GI->ExploreLead;
		bPartyHidden = GI->bExplorePartySet ? GI->bExplorePartyHidden : bPartyHidden;
	}
	if (PartyLooks.IsEmpty())
	{
		FFTLeadLook Own;
		Own.Name = UTF8_TO_TCHAR(ftcore::demo::kPartyNames[0]);
		Own.BodyMeshPath = BodyMeshPath;
		Own.IdleClipPath = IdleClipPath;
		Own.WalkClipPath = WalkClipPath;
		Own.PiecePaths = PiecePaths;
		Own.Grooms = Grooms;
		WearLook(Own);
	}
	else
	{
		LeadIndex = FMath::Max(0, PartyLooks.IndexOfByPredicate([&Lead](const FFTLeadLook& Member)
		{
			return Member.Name.Equals(Lead, ESearchCase::IgnoreCase);
		}));
		WearLook(PartyLooks[LeadIndex]);
	}
	if (bPortrait)
	{
		// For checking a look: the camera holds still round the pawn, by default in front of the face. Yaw 180 is
		// the front, 135 a three-quarter, 90 a side and 0 the back; a longer arm and a lower height frame the body.
		float Yaw = 180.f, Arm = 110.f, Height = 78.f, Pitch = -4.f;
		FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitYaw="), Yaw);
		FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitArm="), Arm);
		FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitHeight="), Height);
		FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitPitch="), Pitch);
		Boom->bUsePawnControlRotation = false;
		Boom->bEnableCameraLag = false;
		Boom->bDoCollisionTest = false;
		Boom->SetRelativeRotation(FRotator(0.0, Yaw, 0.0));
		Boom->TargetArmLength = Arm;
		Boom->SocketOffset = FVector(0.0, 0.0, Height);
		Camera->SetRelativeRotation(FRotator(Pitch, 0.0, 0.0));
		if (FParse::Param(FCommandLine::Get(), TEXT("ExplorePortraitLights")))
		{
			AddPortraitLights();
		}
	}
}

void AFTExploreCharacter::AddPortraitLights()
{
	// A studio three-point rig for judging a look: a key above the camera's left, a soft fill low on its right and a
	// rim behind the pawn. The lights hang off the boom, so they keep their places round whichever side the camera
	// shows. Boom space: the camera sits on -X looking down +X at the pawn; +Y is the camera's right.
	struct FLightSpec
	{
		FVector Location;
		float Candelas;
		float Width;
		FLinearColor Color;
	};
	const FLightSpec Specs[] = {
		{FVector(-260.0, -190.0, 170.0), 30.f, 120.f, FLinearColor(1.f, 0.95f, 0.88f)},   // key
		{FVector(-260.0, 230.0, 20.0), 8.f, 200.f, FLinearColor(0.85f, 0.9f, 1.f)},       // fill
		{FVector(240.0, 140.0, 160.0), 50.f, 80.f, FLinearColor(1.f, 1.f, 1.f)},          // rim
	};
	const FVector Target(0.0, 0.0, 20.0);
	for (const FLightSpec& Spec : Specs)
	{
		URectLightComponent* Light = NewObject<URectLightComponent>(this);
		Light->SetupAttachment(Boom);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetRelativeLocation(Spec.Location);
		Light->SetRelativeRotation((Target - Spec.Location).Rotation());
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetIntensity(Spec.Candelas);
		Light->SetSourceWidth(Spec.Width);
		Light->SetSourceHeight(Spec.Width);
		Light->SetLightColor(Spec.Color);
		Light->SetAttenuationRadius(1200.f);
		Light->RegisterComponent();
		PortraitLights.Add(Light);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: portrait lights on (key, fill, rim)."));
}

void AFTExploreCharacter::WearLook(const FFTLeadLook& Look)
{
	// Grooms first: they may be bound to a piece.
	for (USceneComponent* Groom : GroomComponents)
	{
		if (Groom)
		{
			Groom->DestroyComponent();
		}
	}
	GroomComponents.Reset();
	for (USkeletalMeshComponent* Piece : Pieces)
	{
		if (Piece)
		{
			Piece->DestroyComponent();
		}
	}
	Pieces.Reset();

	USkeletalMeshComponent* BodyComponent = GetMesh();
	const FFTLoadedLook Loaded = FTLook::LoadLook(Look, TEXT("FTExploreCharacter"));
	if (Loaded.Body)
	{
		BodyComponent->SetSkeletalMesh(Loaded.Body);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: no body for %s; keeping the one worn."), *Look.Name);
	}
	// A clip that didn't load keeps the one worn (the leads' MetaHuman bodies share a skeleton).
	IdleClip = Loaded.Idle ? Loaded.Idle : IdleClip.Get();
	WalkClip = Loaded.Walk ? Loaded.Walk : WalkClip.Get();
	const USkeletalMesh* Body = BodyComponent->GetSkeletalMeshAsset();
	if (Body)
	{
		// Turn the body to face the capsule's forward (+X).
		BodyComponent->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Body), 0.0));
	}
	const int32 Cloth = FTLook::AddPieces(*this, *BodyComponent, Look.PiecePaths, Pieces, TEXT("FTExploreCharacter"));
	FTLook::AddGrooms(*this, *BodyComponent, Pieces, Look.Grooms, GroomComponents, TEXT("FTExploreCharacter"));
	BodyComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	bClipStarted = false;
	UpdateClip();
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: %s: body %s, %d pieces, %d with cloth, %d grooms."), *Look.Name,
		Body ? *Body->GetName() : TEXT("none"), Pieces.Num(), Cloth, GroomComponents.Num());
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

	SwitchLeadAction = MakeAction(TEXT("IA_ExploreSwitchLead"), false);
	Context->MapKey(SwitchLeadAction, EKeys::Tab);
	Context->MapKey(SwitchLeadAction, EKeys::Gamepad_RightShoulder);

	PartyAction = MakeAction(TEXT("IA_ExploreParty"), false);
	Context->MapKey(PartyAction, EKeys::P);
	Context->MapKey(PartyAction, EKeys::Gamepad_LeftShoulder);
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
	Input->BindAction(SwitchLeadAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleSwitchLead);
	Input->BindAction(PartyAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleToggleParty);
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

void AFTExploreCharacter::HandleSwitchLead()
{
	if (PartyLooks.Num() < 2)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: no lead to switch to (PartyLooks has %d)."), PartyLooks.Num());
		return;
	}
	LeadIndex = (LeadIndex + 1) % PartyLooks.Num();
	const FFTLeadLook& Look = PartyLooks[LeadIndex];
	WearLook(Look);
	if (UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
	{
		GI->ExploreLead = Look.Name;
	}
	if (AFTExploreDirector* D = GetDirector())
	{
		D->OnLeadChanged(Look.Name);
	}
}

void AFTExploreCharacter::HandleToggleParty()
{
	bPartyHidden = !bPartyHidden;
	if (UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
	{
		GI->bExplorePartySet = true;
		GI->bExplorePartyHidden = bPartyHidden;
	}
	if (AFTExploreDirector* D = GetDirector())
	{
		D->SetPartyHidden(bPartyHidden);
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
