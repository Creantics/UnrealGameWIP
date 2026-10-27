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
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/PlatformMisc.h"
#include "TimerManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "UObject/ConstructorHelpers.h"

#include "FTExploreDirector.h"
#include "FTMeshFacing.h"
#include "FTMeshPieces.h"
#include "ForgottenThird.h"

namespace
{
// -LocoPulse: how long (s) each rest lasts, long enough for a stop clip to settle.
// The camera comes back round behind a moving lead once the look input has rested this long (s), closing
// kRecentreRate of the gap a second and never faster than kRecentreMax (deg/s), so it trails into turns.
constexpr float kRecentreDelay = 1.f;
constexpr float kRecentreRate = 3.f;
constexpr float kRecentreMax = 120.f;
constexpr float kRecentreSpeed = 50.f; // cm/s: slower counts as standing
constexpr float kSideHold = 1.f;       // -LocoSideView holds the side camera this long (s) after a turn
constexpr float kArm = 400.f;          // the camera's distance behind the body
constexpr float kSideArm = 450.f;      // and, filming a follower's turn, behind the point between it and the lead (plus their gap)
// Traversal probes (cm): the forward traces' heights above the feet and their reach (past the run clips'
// lead), the down trace's start above the feet, the step along the top, how far a top may stray from level
// and still be the same top, the gap past a thin top where the far floor is looked for, and how squarely
// (the cosine) a face must face the body.
constexpr float kProbeHeights[] = {45.f, 95.f};
constexpr float kProbeReach = 560.f;
constexpr float kProbeTop = 360.f;
constexpr float kProbeStep = 5.f;
constexpr float kTopTolerance = 15.f;
constexpr float kBackGap = 50.f;
constexpr float kProbeFacing = 0.8f;
} // namespace

AFTExploreCharacter::AFTExploreCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	GetCapsuleComponent()->InitCapsuleSize(34.f, 88.f);

	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;
	UCharacterMovementComponent* Move = GetCharacterMovement();
	// Locomotion steers the body (Tick): its facing eases toward the way it moves, and a sharp turn
	// plays a turn clip whose root motion turns it.
	Move->bOrientRotationToMovement = false;
	Move->MaxWalkSpeed = WalkSpeed;
	Move->MaxWalkSpeedCrouched = CrouchSpeed;
	// Crouched, the capsule is 120 cm tall (ACharacter keeps the feet on the ground).
	Move->GetNavAgentPropertiesRef().bCanCrouch = true;
	Move->SetCrouchedHalfHeight(60.f);
	Move->JumpZVelocity = 520.f;
	Move->AirControl = 0.35f;
	// Braking at a steady deceleration, with no friction term, so the distance to a stop is v^2 / 2b
	// and the stop clip can play to it.
	// Speeding up at half the default, the run start plays near its own pace (about x1.3) rather than
	// racing to keep up with the capsule.
	Move->MaxAcceleration = 1024.f;
	Move->bUseSeparateBrakingFriction = true;
	Move->BrakingFriction = 0.f;
	Move->BrakingDecelerationWalking = 1000.f;

	Boom = CreateDefaultSubobject<USpringArmComponent>(TEXT("Boom"));
	Boom->SetupAttachment(RootComponent);
	// Low and over the right shoulder, the lead filling about 45% of the frame's height (at 16:9), so
	// the ground moving past reads as speed and the run reads whole.
	Boom->TargetArmLength = kArm;
	Boom->SocketOffset = FVector(0.0, 70.0, 60.0);
	Boom->bUsePawnControlRotation = true;
	Boom->bEnableCameraLag = true;
	Boom->CameraLagSpeed = 12.f;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Boom, USpringArmComponent::SocketName);
	Camera->SetRelativeRotation(FRotator(-10.0, 0.0, 0.0));
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
		UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: body %s facing %.0f."), *Body->GetName(), GetMesh()->GetRelativeRotation().Yaw);
	}
	SetupLocomotion();
	// Locomotion test switches: -LocoRun holds the run speed, -LocoJump=<s> jumps every s seconds,
	// -LocoPulse=<s> drops the movement input for -LocoRest=<s> seconds (default 2) after every s seconds
	// of it, for starts and stops; with -LocoFace=<deg> the body turns on the spot by that much in each
	// rest (a rest needs about 4 s to fit the stop and the turn). -LocoSideView films each turn from the side;
	// -LocoNoLook ignores look input.
	if (FParse::Param(FCommandLine::Get(), TEXT("LocoRun")))
	{
		GetCharacterMovement()->MaxWalkSpeed = RunSpeed;
	}
	FParse::Value(FCommandLine::Get(), TEXT("LocoJump="), JumpEvery);
	// -LocoCrouch holds a crouch; -LocoCrouchEvery=<s> gets down or up every s seconds.
	if (FParse::Param(FCommandLine::Get(), TEXT("LocoCrouch")))
	{
		Crouch();
	}
	FParse::Value(FCommandLine::Get(), TEXT("LocoCrouchEvery="), CrouchEvery);
	FParse::Value(FCommandLine::Get(), TEXT("LocoPulse="), PulseEvery);
	FParse::Value(FCommandLine::Get(), TEXT("LocoRest="), PulseRest);
	FParse::Value(FCommandLine::Get(), TEXT("LocoFace="), FaceBy);
	bSideView = FParse::Param(FCommandLine::Get(), TEXT("LocoSideView"));
	bSideAlways = FParse::Value(FCommandLine::Get(), TEXT("LocoCamYaw="), SideAlwaysYaw)
		|| FParse::Param(FCommandLine::Get(), TEXT("LocoSideAlways"));
	FParse::Value(FCommandLine::Get(), TEXT("LocoCamPitch="), SideAlwaysPitch);
	bNoLook = FParse::Param(FCommandLine::Get(), TEXT("LocoNoLook"));
	bCoursePending = FParse::Param(FCommandLine::Get(), TEXT("LocoVaultWall"));
	if (FParse::Param(FCommandLine::Get(), TEXT("ExplorePortrait")))
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
		// -ExplorePortraitShot=<path prefix>: the renderer saves one 1400x1100 shot (HUD hidden) per yaw in
		// -ExplorePortraitYaws=180,90,0 (default: the one yaw above) as <prefix>_<yaw>.png, then the game quits.
		if (FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitShot="), ShotPrefix) && !ShotPrefix.IsEmpty())
		{
			FString YawList;
			if (FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitYaws="), YawList, false))
			{
				TArray<FString> Parts;
				YawList.ParseIntoArray(Parts, TEXT(","));
				for (const FString& Part : Parts)
				{
					ShotYaws.Add(FCString::Atof(*Part));
				}
			}
			if (ShotYaws.IsEmpty())
			{
				ShotYaws.Add(Yaw);
			}
			// Settle time first (streaming, shaders, exposure), then a step every few seconds: aim, then shoot.
			float Delay = 8.f;
			FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitShotDelay="), Delay);
			GetWorldTimerManager().SetTimer(ShotTimer, this, &AFTExploreCharacter::StepPortraitShots, 2.5f, true, Delay);
		}
	}
}

void AFTExploreCharacter::WatchFrom(USceneComponent* Root)
{
	Watched = Root->GetOwner();
	Boom->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
	Boom->SetRelativeLocation(FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
}

void AFTExploreCharacter::StepPortraitShots()
{
	if (ShotIndex >= ShotYaws.Num())
	{
		GetWorldTimerManager().ClearTimer(ShotTimer);
		UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: %d portrait shots taken; quitting."), ShotYaws.Num());
		FPlatformMisc::RequestExit(false, TEXT("ExplorePortraitShot"));
		return;
	}
	const float Yaw = ShotYaws[ShotIndex];
	if (!bShotAimed)
	{
		// Aim one step ahead of the shot, so temporal AA and lighting settle at the new angle.
		Boom->SetRelativeRotation(FRotator(0.0, Yaw, 0.0));
		// The HUD's canvas would land in the shot; the look is what is being checked.
		const APlayerController* Player = Cast<APlayerController>(GetController());
		if (AHUD* Hud = Player ? Player->GetHUD() : nullptr)
		{
			Hud->bShowHUD = false;
		}
		bShotAimed = true;
		return;
	}
	const FString File = FString::Printf(TEXT("%s_%d.png"), *ShotPrefix, FMath::RoundToInt(Yaw));
	// A high-res shot renders its own view at a fixed size, so a window the desktop resizes can't upset the copy.
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1400x1100 filename=\"%s\""), *File), *GLog);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: portrait shot %s."), *File);
	bShotAimed = false;
	++ShotIndex;
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
	LoadClip(RunClipPath, RunClip);
	LoadClip(SprintClipPath, SprintClip);
	LoadClip(JumpClipPath, JumpClip);
	LoadClip(FallClipPath, FallClip);
	LoadClip(LandClipPath, LandClip);
	LoadClip(JumpMovingClipPath, JumpMovingClip);
	LoadClip(LandMovingClipPath, LandMovingClip);
	const int32 Cloth = FTMeshPieces::Attach(*this, *GetMesh(), PiecePaths, Pieces, TEXT("FTExploreCharacter"));
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

	CrouchAction = MakeAction(TEXT("IA_ExploreCrouch"), false);
	Context->MapKey(CrouchAction, EKeys::C);
	Context->MapKey(CrouchAction, EKeys::LeftControl);
	Context->MapKey(CrouchAction, EKeys::Gamepad_RightThumbstick);

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
	Input->BindAction(JumpAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleJump);
	Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
	Input->BindAction(RunAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleRunStart);
	Input->BindAction(RunAction, ETriggerEvent::Completed, this, &AFTExploreCharacter::HandleRunStop);
	Input->BindAction(CrouchAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleCrouch);
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
	if (bNoLook)
	{
		return;
	}
	if (!Axis.IsNearlyZero())
	{
		LookRest = 0.f;
	}
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(-Axis.Y);
}

void AFTExploreCharacter::HandleRunStart()
{
	// Sprinting stands the body up.
	UnCrouch();
	GetCharacterMovement()->MaxWalkSpeed = RunSpeed;
}

void AFTExploreCharacter::HandleJump()
{
	// Crouched, jump stands the body up; standing in front of a low wall or a ledge, it goes over or up; otherwise it jumps.
	if (bTraversing)
	{
		return;
	}
	if (GetCharacterMovement()->bWantsToCrouch)
	{
		UnCrouch();
		return;
	}
	if (TryTraverse(true))
	{
		return;
	}
	Jump();
}

bool AFTExploreCharacter::ProbeObstacle(const ftcore::TraversalSet& Set, ftcore::TraversalObstacle& Out) const
{
	const UWorld* World = GetWorld();
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	const FVector Fwd = GetActorForwardVector().GetSafeNormal2D();
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Capsule->GetScaledCapsuleHalfHeight());
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(FTTraversal), false, this);
	const FCollisionObjectQueryParams Static(ECC_WorldStatic);
	auto Trace = [World, &Query, &Static](const FVector& From, const FVector& To, FHitResult& Hit)
	{
		return World->LineTraceSingleByObjectType(Hit, From, To, Static, Query) && !Hit.bStartPenetrating;
	};
	auto Level = [](const FHitResult& Hit) { return Hit.ImpactNormal.Z > 0.7; };

	// The near face: the nearer hit of the forward traces on a wall facing the body.
	FHitResult Wall;
	bool bWall = false;
	for (const float Height : kProbeHeights)
	{
		const FVector From = Feet + FVector(0.0, 0.0, Height);
		FHitResult Hit;
		if (Trace(From, From + Fwd * kProbeReach, Hit) && FMath::Abs(Hit.ImpactNormal.Z) < 0.3
			&& FVector::DotProduct(-Hit.ImpactNormal.GetSafeNormal2D(), Fwd) > kProbeFacing && (!bWall || Hit.Distance < Wall.Distance))
		{
			Wall = Hit;
			bWall = true;
		}
	}
	if (!bWall)
	{
		return false;
	}
	const float Distance = static_cast<float>(FVector::DotProduct(Wall.ImpactPoint - Feet, Fwd));
	// The top: down just past the face, from above the highest clip (a start inside something is too high).
	const FVector Over = Feet + Fwd * (Distance + kProbeStep);
	FHitResult Top;
	if (!Trace(Over + FVector(0.0, 0.0, kProbeTop), Over, Top) || !Level(Top))
	{
		return false;
	}
	const double TopZ = Top.ImpactPoint.Z;
	// Its depth: on along the top while it stays level, as far as standing on it needs.
	const ftcore::TraversalTuning& Tuning = Set.tuning();
	float Depth = 0.f;
	for (float D = 2.f * kProbeStep; D <= Tuning.standDepth + kProbeStep; D += kProbeStep)
	{
		const FVector At = Feet + Fwd * (Distance + D);
		FHitResult Hit;
		if (!Trace(FVector(At.X, At.Y, TopZ + kTopTolerance), FVector(At.X, At.Y, TopZ - kTopTolerance), Hit))
		{
			break;
		}
		Depth = D;
	}
	Out = ftcore::TraversalObstacle();
	Out.distance = Distance;
	Out.height = static_cast<float>(TopZ - Feet.Z);
	Out.depth = Depth;
	if (Depth < Tuning.standDepth)
	{
		// Thin: the floor beyond, no lower than the deepest drop.
		const FVector Beyond = Feet + Fwd * (Distance + Depth + kBackGap);
		FHitResult Floor;
		if (Trace(FVector(Beyond.X, Beyond.Y, TopZ + kTopTolerance), FVector(Beyond.X, Beyond.Y, Feet.Z - Tuning.maxDrop - 20.0), Floor) && Level(Floor))
		{
			Out.backFloor = true;
			Out.backHeight = static_cast<float>(Floor.ImpactPoint.Z - Feet.Z);
		}
	}
	else
	{
		// Deep: room on the top for the capsule.
		const float Half = Capsule->GetScaledCapsuleHalfHeight();
		const float Radius = Capsule->GetScaledCapsuleRadius();
		const FVector Stand = Feet + Fwd * (Distance + Radius + kProbeStep);
		if (World->OverlapBlockingTestByChannel(FVector(Stand.X, Stand.Y, TopZ + Half + 2.0), FQuat::Identity, ECC_Pawn,
				FCollisionShape::MakeCapsule(Radius, Half), Query))
		{
			return false;
		}
	}
	return true;
}

bool AFTExploreCharacter::TryTraverse(bool bLog)
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance());
	if (bTraversing || !Anim || !Move->IsMovingOnGround() || bIsCrouched)
	{
		return false;
	}
	const ftcore::TraversalSet& Set = Anim->GetTraversal();
	ftcore::TraversalObstacle Obstacle;
	if (!ProbeObstacle(Set, Obstacle))
	{
		return false;
	}
	const float Speed = static_cast<float>(GetVelocity().Size2D());
	const ftcore::TraversalPlan Plan = Set.plan(Obstacle, Speed, -1);
	if (!Plan.ok())
	{
		if (bLog)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Traversal: none for a %.0f cm top %.0f cm ahead, %.0f deep (floor beyond %d at %.0f), at %.0f cm/s%s."),
				Obstacle.height, Obstacle.distance, Obstacle.depth, Obstacle.backFloor ? 1 : 0, Obstacle.backHeight, Speed,
				Plan.tooFar ? TEXT(": still out of reach") : TEXT(""));
		}
		return false;
	}
	bTraversing = true;
	TravPlan = Plan;
	TravTime = Plan.start;
	TravFrame = FTransform(FRotator(0.0, GetActorRotation().Yaw, 0.0),
		GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
	// Nothing moves the capsule but TickTraversal (a custom mode with no physics of its own).
	Move->SetMovementMode(MOVE_Custom);
	Anim->SetTraversal(Plan.clip, TravTime);
	const ftcore::TraversalClipInfo& Clip = Set.clip(static_cast<std::size_t>(Plan.clip));
	UE_LOG(LogForgottenThird, Display,
		TEXT("Traversal: clip %d (%s, edge %.0f up) from %.2f s for a %.0f cm top %.0f cm ahead, %.0f deep (floor beyond %d at %.0f), at %.0f cm/s; offsets contact (%.0f, %.0f) leave (%.0f, %.0f) land (%.0f, %.0f)."),
		Plan.clip, Clip.move == ftcore::TraversalMove::Up ? TEXT("up") : TEXT("over"), Clip.ledgeHeight, Plan.start, Obstacle.height,
		Obstacle.distance, Obstacle.depth, Obstacle.backFloor ? 1 : 0, Obstacle.backHeight, Speed, Plan.atContact.x, Plan.atContact.z,
		Plan.atLeave.x, Plan.atLeave.z, Plan.atLand.x, Plan.atLand.z);
	return true;
}

void AFTExploreCharacter::TickTraversal(float DeltaSeconds)
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance());
	if (!Anim)
	{
		bTraversing = false;
		Move->SetMovementMode(MOVE_Walking);
		return;
	}
	const ftcore::TraversalSet& Set = Anim->GetTraversal();
	TravTime += DeltaSeconds;
	const float Length = Set.clip(static_cast<std::size_t>(TravPlan.clip)).length;
	const float T = FMath::Min(TravTime, Length);
	// The plan's root in the body's space as it began: x forward, y left (UE's Y is right), z up.
	auto FeetAt = [&](float At)
	{
		const ftcore::TravPoint R = Set.rootAt(TravPlan, At);
		return TravFrame.TransformPosition(FVector(R.x, -R.y, R.z));
	};
	const FVector Feet = FeetAt(T);
	SetActorLocation(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
	// The movement component's velocity is the path's, for the locomotion now and the walk after.
	const float Back = FMath::Max(DeltaSeconds, 1.f / 60.f);
	Move->Velocity = (Feet - FeetAt(FMath::Max(TravPlan.start, T - Back))) / Back;
	if (T >= Length)
	{
		bTraversing = false;
		Anim->SetTraversal(-1, 0.f);
		Move->Velocity.Z = 0.0;
		Move->SetMovementMode(MOVE_Walking);
		UE_LOG(LogForgottenThird, Display, TEXT("Traversal: done at %.0f cm/s, %.0f cm from the start, %+.0f cm up."), Move->Velocity.Size2D(),
			FVector::Dist2D(Feet, TravFrame.GetLocation()), Feet.Z - TravFrame.GetLocation().Z);
		return;
	}
	Anim->SetTraversal(TravPlan.clip, T);
}

void AFTExploreCharacter::SpawnTraversalCourse()
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UWorld* World = GetWorld();
	if (!Cube || !World)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Traversal course: no cube mesh; nothing built."));
		return;
	}
	CourseDir = GetActorForwardVector().GetSafeNormal2D();
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	// Ahead, each on the floor found under it: a thin wall to go over, a low block to climb onto (and run off the
	// far end), and a high block to climb.
	struct FBlock
	{
		float At, Depth, Height;
	};
	const FBlock Blocks[] = {{700.f, 25.f, 100.f}, {1500.f, 300.f, 100.f}, {2600.f, 300.f, 240.f}};
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(FTTraversalCourse), false, this);
	for (const FBlock& B : Blocks)
	{
		const FVector Centre = Feet + CourseDir * (B.At + 0.5f * B.Depth);
		FHitResult Floor;
		double Z = Feet.Z;
		if (World->LineTraceSingleByObjectType(Floor, Centre + FVector(0.0, 0.0, 500.0), Centre - FVector(0.0, 0.0, 500.0),
				FCollisionObjectQueryParams(ECC_WorldStatic), Query))
		{
			Z = Floor.ImpactPoint.Z;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Block = World->SpawnActor<AStaticMeshActor>(FVector(Centre.X, Centre.Y, Z + 0.5 * B.Height),
			FRotator(0.0, CourseDir.Rotation().Yaw, 0.0), Params);
		if (!Block)
		{
			continue;
		}
		Block->SetMobility(EComponentMobility::Movable);
		Block->GetStaticMeshComponent()->SetStaticMesh(Cube);
		// The cube is 100 cm a side: x along the course, y across it.
		Block->SetActorScale3D(FVector(B.Depth / 100.0, 3.0, B.Height / 100.0));
		UE_LOG(LogForgottenThird, Display, TEXT("Traversal course: a %.0f cm block %.0f deep, %.0f cm ahead (floor %+.0f)."), B.Height, B.Depth,
			B.At, Z - Feet.Z);
	}
	GetCharacterMovement()->MaxWalkSpeed = RunSpeed;
	bVaultCourse = true;
}

void AFTExploreCharacter::HandleCrouch()
{
	if (GetCharacterMovement()->bWantsToCrouch)
	{
		UnCrouch();
	}
	else
	{
		Crouch();
	}
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
	// -LocoVaultWall builds the course once the body has settled where the map puts it, then runs along it,
	// traversing what it can.
	if (bCoursePending && GetWorld()->GetTimeSeconds() > 1.0 && GetCharacterMovement()->IsMovingOnGround())
	{
		bCoursePending = false;
		SpawnTraversalCourse();
	}
	if (bVaultCourse && !bTraversing)
	{
		AddMovementInput(CourseDir, 1.f);
		TryTraverse(false);
	}
	if (bTraversing)
	{
		TickTraversal(DeltaSeconds);
	}
	// A traversal holds the facing it began with.
	const UFTLocomotionAnimInstance* Steered = bTraversing ? nullptr : Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance());
	if (const UFTLocomotionAnimInstance* Anim = Steered)
	{
		const float Step = Anim->GetLocomotion().yawStep();
		if (Step != 0.f)
		{
			AddActorWorldRotation(FRotator(0.0, Step, 0.0));
		}
	}
	SlideProbe.Sample(*GetMesh(), DeltaSeconds, static_cast<float>(GetVelocity().Size2D()));
	LookRest += DeltaSeconds;
	const AActor* Framed = Watched.IsValid() ? Watched.Get() : this;
	if (bSideAlways && Controller)
	{
		// The framed body is filmed from its left (or -LocoCamYaw's angle) the whole time, turning with it, at the
		// usual distance.
		FRotator Control = Controller->GetControlRotation();
		Control.Yaw = Framed->GetActorRotation().Yaw + SideAlwaysYaw;
		if (SideAlwaysPitch != 0.f)
		{
			Control.Pitch = -SideAlwaysPitch;
		}
		Controller->SetControlRotation(Control);
		if (Framed != this)
		{
			// The follower's root is at its feet, the lead's at its capsule's middle.
			Boom->SetWorldLocation(Framed->GetActorLocation() + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
		}
	}
	else if (bSideView && Controller)
	{
		// A turn is filmed side on from where it began, the camera still until a second after it ends.
		const USkeletalMeshComponent* FramedMesh = Framed->FindComponentByClass<USkeletalMeshComponent>();
		if (const UFTLocomotionAnimInstance* Anim = FramedMesh ? Cast<UFTLocomotionAnimInstance>(FramedMesh->GetAnimInstance()) : nullptr)
		{
			const ftcore::Locomotion& Loco = Anim->GetLocomotion();
			if (Loco.turning() && Loco.turnsBegun() != SideTurns)
			{
				SideTurns = Loco.turnsBegun();
				SideYaw = static_cast<float>(Framed->GetActorRotation().Yaw) + 90.f;
			}
			SideHold = Loco.turning() ? kSideHold : SideHold - DeltaSeconds;
		}
		if (SideHold > 0.f)
		{
			FRotator Control = Controller->GetControlRotation();
			Control.Yaw = SideYaw;
			Controller->SetControlRotation(Control);
		}
		// Watching a follower, the turn is filmed with the lead in frame too: the camera looks at the
		// point between them, pulled back by the gap.
		if (Framed != this)
		{
			const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			if (SideHold > 0.f)
			{
				// The follower's root is at its feet, the lead's at its capsule's middle.
				const FVector Up(0.0, 0.0, HalfHeight);
				Boom->SetWorldLocation((GetActorLocation() - Up + Framed->GetActorLocation()) * 0.5 + Up);
				Boom->TargetArmLength = kSideArm + static_cast<float>(FVector::Dist2D(GetActorLocation(), Framed->GetActorLocation()));
			}
			else
			{
				Boom->SetRelativeLocation(FVector(0.0, 0.0, HalfHeight));
				Boom->TargetArmLength = kArm;
			}
		}
	}
	if (Controller && Boom->bUsePawnControlRotation && LookRest >= kRecentreDelay && GetVelocity().Size2D() > kRecentreSpeed
		&& SideHold <= 0.f && !bSideAlways)
	{
		FRotator Control = Controller->GetControlRotation();
		const float Gap = FMath::FindDeltaAngleDegrees(static_cast<float>(Control.Yaw), static_cast<float>(Framed->GetActorRotation().Yaw));
		Control.Yaw += FMath::Clamp(Gap * kRecentreRate * DeltaSeconds, -kRecentreMax * DeltaSeconds, kRecentreMax * DeltaSeconds);
		Controller->SetControlRotation(Control);
	}
	if (PulseEvery > 0.f && (PulseTimer += DeltaSeconds) > PulseEvery + PulseRest)
	{
		PulseTimer = 0.f;
	}
	if (FaceBy != 0.f && PulseEvery > 0.f && (PulseTimer > PulseEvery) != bFacing)
	{
		bFacing = !bFacing;
		if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance()))
		{
			Anim->SetFace(bFacing, static_cast<float>(GetActorRotation().Yaw) + FaceBy);
		}
	}
	if (JumpEvery > 0.f && (JumpTimer += DeltaSeconds) >= JumpEvery)
	{
		JumpTimer = 0.f;
		Jump();
	}
	if (CrouchEvery > 0.f && (CrouchTimer += DeltaSeconds) >= CrouchEvery)
	{
		CrouchTimer = 0.f;
		HandleCrouch();
	}
}

void AFTExploreCharacter::AddMovementInput(FVector WorldDirection, float ScaleValue, bool bForce)
{
	if (PulseEvery > 0.f && PulseTimer > PulseEvery)
	{
		return;
	}
	Super::AddMovementInput(WorldDirection, ScaleValue, bForce);
}

void AFTExploreCharacter::SetupLocomotion()
{
	USkeletalMeshComponent* Body = GetMesh();
	Body->SetAnimInstanceClass(UFTLocomotionAnimInstance::StaticClass());
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());
	if (!Anim)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: no locomotion anim instance; the body stays in its ref pose."));
		return;
	}
	Anim->SetClip(ftcore::LocoClip::Idle, IdleClip);
	Anim->SetClip(ftcore::LocoClip::Walk, WalkClip);
	Anim->SetClip(ftcore::LocoClip::Run, RunClip);
	Anim->SetClip(ftcore::LocoClip::Sprint, SprintClip);
	Anim->SetClip(ftcore::LocoClip::JumpStart, JumpClip);
	Anim->SetClip(ftcore::LocoClip::Fall, FallClip);
	Anim->SetClip(ftcore::LocoClip::Land, LandClip);
	Anim->SetClip(ftcore::LocoClip::JumpMoving, JumpMovingClip);
	Anim->SetClip(ftcore::LocoClip::LandMoving, LandMovingClip);
	Anim->SetMovesFromGait(ftcore::LocoClip::Walk);
	Anim->SetMovesFromGait(ftcore::LocoClip::Run);
	Anim->SetMovesFromGait(ftcore::LocoClip::Sprint);
	Anim->SetTurnsFromRun();
	Anim->SetAirPoseFromRun();
	Anim->SetCrouchFromRun();
	Anim->SetTraversalFromRun();
	// The crouch walk plays at its own pace at the crouched top speed.
	const float Crouched = Anim->GetLocomotion().clip(ftcore::LocoClip::CrouchWalk).groundSpeed;
	if (Crouched > 0.f)
	{
		CrouchSpeed = Crouched;
		GetCharacterMovement()->MaxWalkSpeedCrouched = Crouched;
	}
}
