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
#include "FTGameInstance.h"
#include "FTMeshFacing.h"
#include "ForgottenThird.h"

#include "ftcore/BattleDemo.h"

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
// In the air the stick asks for a catch pointing at least this squarely (the cosine) into the face. -LocoCatch jumps at a
// wall between these distances ahead (cm).
constexpr float kCatchStick = 0.5f;
// -LocoCatch hangs this long at each catch before it climbs (s).
constexpr float kCourseHang = 0.5f;
constexpr float kCourseJumpFrom = 80.f;
constexpr float kCourseJumpTo = 120.f;
// A roll press the body can't act on yet is kept this long (s), and a roll or a slide turns the body to its way (a catch
// square into the face) with this time constant (s). -LocoRollTurn keeps the stick turned this long (s) after a roll
// hands back.
constexpr float kRollBuffer = 0.2f;
constexpr float kRollTurnTime = 0.05f;
constexpr float kRollTurnHold = 1.f;
// A wall a slide meets this squarely or more (the cosine between its way and into the wall) stops it; one met at more
// of a glance turns it along the wall.
constexpr float kSlideHeadOn = 0.7f;
// The get-ups in ftcore::SlideOut's order, for the log.
const TCHAR* const SlideOutNames[] = {TEXT("a stand"), TEXT("a crouch"), TEXT("a walk"), TEXT("a run"), TEXT("a sprint"), TEXT("a crouch walk")};
static_assert(UE_ARRAY_COUNT(SlideOutNames) == ftcore::kSlideOuts, "one name per get-up");
} // namespace

AFTExploreCharacter::AFTExploreCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	GetCapsuleComponent()->InitCapsuleSize(CapsuleRadius, CapsuleHalfHeight);

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
	// Up as high as the jump clips' roots go (94 cm, sqrt(2 * 980 * 94)), so the capsule comes down when they do.
	Move->JumpZVelocity = 428.f;
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
	// The mesh's origin is at its feet; the capsule's is at its middle, and the capsule hovers over the floor.
	GetMesh()->SetRelativeLocation(FVector(0.0, 0.0, -(CapsuleHalfHeight + FloorHover)));
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (Devin.Object)
	{
		GetMesh()->SetSkeletalMesh(Devin.Object);
	}
}

void AFTExploreCharacter::BeginPlay()
{
	Super::BeginPlay();
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
	Dodge.setTuning(UFTLocomotionAnimInstance::GetMoveTuning().dodge);
	Ledge.setTuning(UFTLocomotionAnimInstance::GetMoveTuning().ledge);
	DodgeNow = Dodge.fresh();
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
		Own.ClipFolder = ClipFolder;
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
	// -LocoCrouch holds a crouch; -LocoCrouchEvery=<s> gets down or up every s seconds (sprinting, as with -LocoRun,
	// down is a slide).
	if (FParse::Param(FCommandLine::Get(), TEXT("LocoCrouch")))
	{
		Crouch();
	}
	FParse::Value(FCommandLine::Get(), TEXT("LocoCrouchEvery="), CrouchEvery);
	// -LocoRollEvery=<s> rolls every s seconds; -LocoRollTurn=<deg> turns the stick that far off each roll's way, from
	// its start until kRollTurnHold after it hands back, for turning out of it.
	FParse::Value(FCommandLine::Get(), TEXT("LocoRollEvery="), RollEvery);
	FParse::Value(FCommandLine::Get(), TEXT("LocoRollTurn="), RollTurn);
	FParse::Value(FCommandLine::Get(), TEXT("LocoPulse="), PulseEvery);
	FParse::Value(FCommandLine::Get(), TEXT("LocoRest="), PulseRest);
	FParse::Value(FCommandLine::Get(), TEXT("LocoFace="), FaceBy);
	bSideView = FParse::Param(FCommandLine::Get(), TEXT("LocoSideView"));
	bSideAlways = FParse::Value(FCommandLine::Get(), TEXT("LocoCamYaw="), SideAlwaysYaw)
		|| FParse::Param(FCommandLine::Get(), TEXT("LocoSideAlways"));
	FParse::Value(FCommandLine::Get(), TEXT("LocoCamPitch="), SideAlwaysPitch);
	bNoLook = FParse::Param(FCommandLine::Get(), TEXT("LocoNoLook"));
	bCatchCourse = FParse::Param(FCommandLine::Get(), TEXT("LocoCatch"));
	bCoursePending = bCatchCourse || FParse::Param(FCommandLine::Get(), TEXT("LocoVaultWall"));
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
		// -ExplorePortraitWalk: the body walks a slow circle (about 5.7 m across) so shots catch it mid-stride.
		bPortraitWalk = FParse::Param(FCommandLine::Get(), TEXT("ExplorePortraitWalk"));
		// -ExplorePortraitMoveScale=<0..1>: the circle's input scale (0.5 walks, 1 runs); it implies the walk circle.
		// -ExplorePortraitJump: on the circle, the body jumps every 1.5 s, so shots catch the jump clips.
		if (FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitMoveScale="), PortraitMoveScale))
		{
			PortraitMoveScale = FMath::Clamp(PortraitMoveScale, 0.f, 1.f);
			bPortraitWalk = true;
		}
		bPortraitJump = FParse::Param(FCommandLine::Get(), TEXT("ExplorePortraitJump"));
		bPortraitWalk = bPortraitWalk || bPortraitJump;
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
			FString ClipPath;
			FString TimeList;
			if (FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitFaceClip="), ClipPath)
				&& FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitFaceTimes="), TimeList, false))
			{
				FaceClip = LoadObject<UAnimSequence>(nullptr, *ClipPath);
				TArray<FString> Parts;
				TimeList.ParseIntoArray(Parts, TEXT(","));
				for (const FString& Part : Parts)
				{
					FaceTimes.Add(FCString::Atof(*Part));
				}
				if (!FaceClip || FaceTimes.IsEmpty())
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: face clip %s not loaded or no times; face shots off."), *ClipPath);
					FaceClip = nullptr;
					FaceTimes.Reset();
				}
				else
				{
					// One shot per time; the yaw list repeats round them.
					const TArray<float> Yaws = ShotYaws;
					ShotYaws.Reset();
					for (int32 Index = 0; Index < FaceTimes.Num(); ++Index)
					{
						ShotYaws.Add(Yaws[Index % Yaws.Num()]);
					}
				}
			}
			// Settle time first (streaming, shaders, exposure), then a step every few seconds: aim, then shoot.
			float Delay = 8.f;
			FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitShotDelay="), Delay);
			FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitShotEvery="), ShotEvery);
			ShotEvery = FMath::Max(ShotEvery, 0.1f);
			GetWorldTimerManager().SetTimer(ShotTimer, this, &AFTExploreCharacter::StepPortraitShots, ShotEvery, true, Delay);
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
		if (FaceClip)
		{
			HoldFaceAt(FaceTimes[ShotIndex]);
		}
		bShotAimed = true;
		return;
	}
	const FString File = FaceClip
		? FString::Printf(TEXT("%s_%d_f%d.png"), *ShotPrefix, FMath::RoundToInt(Yaw), ShotIndex)
		: FString::Printf(TEXT("%s_%d.png"), *ShotPrefix, FMath::RoundToInt(Yaw));
	// A high-res shot renders its own view at a fixed size, so a window the desktop resizes can't upset the copy.
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1400x1100 filename=\"%s\""), *File), *GLog);
	}
	const FFindFloorResult& Floor = GetCharacterMovement()->CurrentFloor;
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: portrait shot %s (floor dist %.3f cm, walkable %d, speed %.0f)."),
		*File, Floor.FloorDist, Floor.IsWalkableFloor() ? 1 : 0, GetVelocity().Size2D());
	bShotAimed = false;
	++ShotIndex;
}

void AFTExploreCharacter::HoldFaceAt(float Time)
{
	// A leader-posed piece copies the body's bones and never runs its own animation, so the MetaHuman face rig
	// (its post-process blueprint) would stay neutral. The face piece leaves the leader pose and holds the clip.
	for (USkeletalMeshComponent* Piece : Pieces)
	{
		const USkeletalMesh* PieceMesh = Piece ? Piece->GetSkeletalMeshAsset() : nullptr;
		if (!PieceMesh || !PieceMesh->GetName().Contains(TEXT("FaceMesh")))
		{
			continue;
		}
		Piece->SetLeaderPoseComponent(nullptr);
		Piece->PlayAnimation(FaceClip, false);
		Piece->SetPosition(FMath::Clamp(Time, 0.f, FaceClip->GetPlayLength()), false);
		Piece->Stop();
		UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: face %s held on %s at %.2f s."), *PieceMesh->GetName(),
			*FaceClip->GetName(), Time);
		return;
	}
	UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: no FaceMesh piece to hold the face clip on."));
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
	const USkeletalMesh* Body = BodyComponent->GetSkeletalMeshAsset();
	if (Body)
	{
		// Turn the body to face the capsule's forward (+X).
		BodyComponent->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Body), 0.0));
	}
	const int32 Cloth = FTLook::AddPieces(*this, *BodyComponent, Look.PiecePaths, Pieces, TEXT("FTExploreCharacter"));
	FTLook::AddGrooms(*this, *BodyComponent, Pieces, Look.Grooms, GroomComponents, TEXT("FTExploreCharacter"));
	FTLook::SwapMaterials(*BodyComponent, Pieces, Look.MaterialSwaps, TEXT("FTExploreCharacter"));
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: %s: body %s facing %.0f, %d pieces (%d with cloth), %d grooms, clips from %s."),
		*Look.Name, Body ? *Body->GetName() : TEXT("none"), BodyComponent->GetRelativeRotation().Yaw, Pieces.Num(), Cloth,
		GroomComponents.Num(), *Loaded.ClipFolder);
	// A body that didn't load keeps the clips worn with it.
	if (Loaded.Body)
	{
		SetupLocomotion(Loaded);
	}
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

	RollAction = MakeAction(TEXT("IA_ExploreRoll"), false);
	Context->MapKey(RollAction, EKeys::LeftAlt);
	Context->MapKey(RollAction, EKeys::Gamepad_FaceButton_Right);

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
	Input->BindAction(JumpAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleJump);
	Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &AFTExploreCharacter::HandleJumpRelease);
	Input->BindAction(RollAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleRoll);
	Input->BindAction(RunAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleRunStart);
	Input->BindAction(RunAction, ETriggerEvent::Completed, this, &AFTExploreCharacter::HandleRunStop);
	Input->BindAction(CrouchAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleCrouch);
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
	// Held in the air, it catches a ledge within reach (Tick).
	bJumpHeld = true;
	// Mid-slide, the press is kept a moment for TickSlide, which cancels the slide into a jump once Core allows it.
	if (SlideNow.sliding())
	{
		SlideJumpAsked = kRollBuffer;
		return;
	}
	// Hanging from a ledge, it climbs up (Tick, through ftcore::LedgeHang).
	if (LedgeNow.phase == ftcore::LedgePhase::Hanging)
	{
		LedgeJumpAsked = kRollBuffer;
		return;
	}
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

void AFTExploreCharacter::HandleJumpRelease()
{
	bJumpHeld = false;
	StopJumping();
}

void AFTExploreCharacter::HandleRoll()
{
	// A press the body can't act on yet (in the air, mid-traversal) is kept for a moment: Tick tries it again. One in
	// the air is kept for the landing's press window, and the touchdown it lasts to rolls out of the landing.
	const bool bAir = GetCharacterMovement()->IsFalling();
	RollAsked = TryRoll(false, true) ? 0.f : bAir ? UFTLocomotionAnimInstance::GetMoveTuning().landing.pressWindow : kRollBuffer;
	bRollAskedAir = bAir && RollAsked > 0.f;
}

bool AFTExploreCharacter::ProbeObstacle(const ftcore::TraversalSet& Set, bool bAir, ftcore::TraversalObstacle& Out, FVector& In) const
{
	const UWorld* World = GetWorld();
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	const float Half = Capsule->GetScaledCapsuleHalfHeight();
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(FTTraversal), false, this);
	return World && FTTraversalProbe::Probe(*World, Query, FeetLocation(), GetActorForwardVector().GetSafeNormal2D(),
		Capsule->GetScaledCapsuleRadius(), Half, Set.tuning(), bAir, Out, In);
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
	FVector In;
	if (!ProbeObstacle(Set, false, Obstacle, In))
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
		FeetLocation());
	// Marked for the followers, to go the same way where they meet it.
	LastTraversal.Face = TravFrame.GetLocation() + In * Obstacle.distance;
	LastTraversal.Way = In;
	LastTraversal.bCatch = false;
	++TraversalsBegun;
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

bool AFTExploreCharacter::TryCatch(bool bLog)
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance());
	if (bTraversing || !Anim || !Move->IsFalling() || bIsCrouched || !Ledge.canCatch(LedgeNow))
	{
		return false;
	}
	const ftcore::TraversalSet& Set = Anim->GetTraversal();
	ftcore::TraversalObstacle Obstacle;
	FVector In;
	if (!ProbeObstacle(Set, true, Obstacle, In))
	{
		return false;
	}
	// Asked for by the stick pointing into the face, or by the jump held while the body is not going away from it.
	const FVector Stick = Move->GetCurrentAcceleration();
	const FVector V = GetVelocity();
	const float VIn = static_cast<float>(FVector::DotProduct(V, In));
	const bool bInto = Stick.SizeSquared2D() > 1.0 && FVector::DotProduct(Stick.GetSafeNormal2D(), In) > kCatchStick;
	if (!bInto && (!bJumpHeld || VIn < 0.f))
	{
		return false;
	}
	// Core's x is into the face, y its left (left of In in UE's ground plane is (In.Y, -In.X)), z up; the speed it goes on
	// at is the one the stick asks for.
	const ftcore::TravPoint Velocity{VIn, static_cast<float>(V.X * In.Y - V.Y * In.X), static_cast<float>(V.Z)};
	const float Gravity = -Move->GetGravityZ();
	const float Speed = Move->MaxWalkSpeed * FMath::Min(static_cast<float>(Stick.Size2D()) / FMath::Max(Move->GetMaxAcceleration(), 1.f), 1.f);
	const ftcore::TraversalPlan Plan = Set.planCatch(Obstacle, Velocity, Gravity, Speed);
	if (!Plan.ok())
	{
		// Said once a flight for each outcome: none fits, or one is still out of reach.
		const int32 Said = Plan.tooFar ? 1 : 0;
		if (bLog && Said != CatchMissSaid)
		{
			CatchMissSaid = Said;
			UE_LOG(LogForgottenThird, Display, TEXT("Catch: none for a %.0f cm top %.0f cm ahead, %.0f deep, at (%.0f, %.0f, %.0f) cm/s%s."),
				Obstacle.height, Obstacle.distance, Obstacle.depth, Velocity.x, Velocity.y, Velocity.z, Plan.tooFar ? TEXT(": still out of reach") : TEXT(""));
		}
		return false;
	}
	bTraversing = true;
	TravPlan = Plan;
	TravTime = Plan.start;
	TravFrame = FTransform(FRotator(0.0, In.Rotation().Yaw, 0.0),
		FeetLocation());
	// Marked for the followers, to catch the same ledge where they meet it in the air.
	LastTraversal.Face = TravFrame.GetLocation() + In * Obstacle.distance;
	LastTraversal.Way = In;
	LastTraversal.bCatch = true;
	++TraversalsBegun;
	Move->SetMovementMode(MOVE_Custom);
	Anim->SetTraversal(Plan.clip, TravTime);
	const ftcore::TraversalClipInfo& Clip = Set.clip(static_cast<std::size_t>(Plan.clip));
	const float Lead = Clip.contact - Plan.start;
	const float Flight = Velocity.z * Lead - 0.5f * Gravity * Lead * Lead;
	UE_LOG(LogForgottenThird, Display,
		TEXT("Catch: clip %d (edge %.0f up) from %.2f s, %.2f s before the catch, for a %.0f cm top %.0f cm ahead, %.0f deep, at (%.0f, %.0f, %.0f) cm/s; the catch at (%.0f, %.0f), its flight's miss (%.0f, %.0f)."),
		Plan.clip, Clip.ledgeHeight, Plan.start, Lead, Obstacle.height, Obstacle.distance, Obstacle.depth, Velocity.x, Velocity.y, Velocity.z,
		Plan.atContact.x, Plan.atContact.z, Plan.atContact.x - Velocity.x * Lead, Plan.atContact.z - Flight);
	return true;
}

ftcore::LedgeInput AFTExploreCharacter::MakeLedgeInput() const
{
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	ftcore::LedgeInput In;
	In.jumpHeld = bJumpHeld;
	In.jumpPressed = LedgeJumpAsked > 0.f;
	In.dropPressed = LedgeDropAsked > 0.f;
	const FVector Stick = Move->GetCurrentAcceleration();
	In.stick = FMath::Min(static_cast<float>(Stick.Size2D()) / FMath::Max(Move->GetMaxAcceleration(), 1.f), 1.f);
	In.stickOff = In.stick > 0.f ? FMath::FindDeltaAngleDegrees(static_cast<float>(TravFrame.Rotator().Yaw),
		static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Stick.Y, Stick.X)))) : 0.f;
	return In;
}

bool AFTExploreCharacter::TryRoll(bool bLanding, bool bLog)
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance());
	if (bTraversing || !Anim || !Move->IsMovingOnGround())
	{
		return false;
	}
	// An asked roll is a dodge: Core says whether one may start yet (the last one and its cooldown over, a direction
	// if the tuning needs one). A landing's roll is the landing's, free.
	const bool bAskedWay = Move->GetCurrentAcceleration().SizeSquared2D() > 1.0;
	if (!bLanding && !Dodge.canStart(DodgeNow, bAskedWay))
	{
		if (bLog)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Roll: not yet (%s)."), DodgeNow.active() ? TEXT("dodging")
				: DodgeNow.cooldown > 0.f ? TEXT("cooling down") : TEXT("no direction asked"));
		}
		return false;
	}
	const ftcore::TraversalSet& Set = Anim->GetTraversal();
	const float Speed = static_cast<float>(GetVelocity().Size2D());
	ftcore::TraversalPlan Plan = Set.planRoll(Speed, -1, bLanding);
	if (!Plan.ok())
	{
		if (bLog)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Roll: none (no roll clip measured)."));
		}
		return false;
	}
	// Standing up first, while the body still walks, so the capsule grows from the feet (in the custom mode it would
	// grow from its middle and sink the feet). Only when crouched: the movement component's uncrouch resets the mesh's
	// offset whether or not it was. With no room to stand, no roll.
	if (bIsCrouched)
	{
		Move->UnCrouch(false);
		if (bIsCrouched)
		{
			return false;
		}
	}
	UnCrouch();
	// Its way: a pressed roll goes where the stick points, else where the body moves; a landing rolls on the way it
	// was going, else where the stick points; failing both, the way the body faces.
	const FVector Stick = Move->GetCurrentAcceleration();
	const FVector Going = GetVelocity();
	const bool bStick = Stick.SizeSquared2D() > 1.0;
	const bool bGoing = Going.Size2D() > kRecentreSpeed;
	const FVector Way = bLanding ? (bGoing ? Going : bStick ? Stick : GetActorForwardVector())
		: (bStick ? Stick : bGoing ? Going : GetActorForwardVector());
	const double Heading = FMath::RadiansToDegrees(FMath::Atan2(Way.Y, Way.X));
	// The dodge sets the roll's speed by its kind (a sprint's Long roll keeps its momentum, a Short one goes at its own
	// pace, a Tired one slow), and lasts as long as the clip has left.
	if (!bLanding)
	{
		const float Stamina = DodgeNow.stamina;
		Dodge.start(DodgeNow, Speed, bAskedWay, Set.clip(static_cast<std::size_t>(Plan.clip)).length - Plan.start);
		Plan.speed = DodgeNow.speed;
		Plan.entryRate = Set.rate(Plan, Plan.start);
		const ftcore::DodgeTuning& DT = Dodge.tuning();
		UE_LOG(LogForgottenThird, Display, TEXT("Dodge: %s at %.0f cm/s, stamina %.0f -> %.0f, covered %.2f-%.2f s of %.2f."),
			DodgeNow.kind == ftcore::DodgeKind::Long ? TEXT("long") : DodgeNow.kind == ftcore::DodgeKind::Short ? TEXT("short") : TEXT("tired"),
			DodgeNow.speed, Stamina, DodgeNow.stamina, DT.invulnStart,
			DodgeNow.kind == ftcore::DodgeKind::Tired ? DT.invulnStart + (DT.invulnEnd - DT.invulnStart) * DT.tiredInvuln : DT.invulnEnd, DodgeNow.length);
	}
	bTraversing = true;
	TravPlan = Plan;
	TravTime = Plan.start;
	TravFrame = FTransform(FRotator(0.0, Heading, 0.0),
		FeetLocation());
	Move->SetMovementMode(MOVE_Custom);
	Anim->SetTraversal(Plan.clip, TravTime);
	RollTurnLeft = RollTurn != 0.f ? kRollTurnHold : 0.f;
	const ftcore::TraversalClipInfo& Clip = Set.clip(static_cast<std::size_t>(Plan.clip));
	UE_LOG(LogForgottenThird, Display, TEXT("Roll (%s): clip %d, %.2f s, at %.0f cm/s, rolling at %.0f (the clip's %.0f, x%.2f), heading %.0f, facing %.0f."),
		bLanding ? TEXT("landing") : TEXT("asked"), Plan.clip, Clip.length, Speed, Plan.speed, Clip.entrySpeed, Plan.entryRate, Heading,
		GetActorRotation().Yaw);
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
	const ftcore::TraversalClipInfo& Clip = Set.clip(static_cast<std::size_t>(TravPlan.clip));
	const bool bRoll = Clip.move == ftcore::TraversalMove::Roll;
	const bool bCatch = Clip.move == ftcore::TraversalMove::Catch;
	// Let go of a ledge: the body falls from where it hangs, pushed off the face; the clip fades out from the hang.
	if (bCatch && LedgeNow.phase == ftcore::LedgePhase::Dropping)
	{
		bTraversing = false;
		Anim->LetGoTraversal(1.f, false);
		Move->SetMovementMode(MOVE_Falling);
		Move->Velocity = -TravFrame.GetRotation().GetForwardVector() * Ledge.tuning().dropPush;
		UE_LOG(LogForgottenThird, Display, TEXT("Ledge: dropped %.2f s into the catch."), TravClock);
		return;
	}
	const bool bHanging = bCatch && LedgeNow.phase == ftcore::LedgePhase::Hanging;
	// For its log: how long it has gone and its slowest speed, from its first frame.
	if (TravTime <= TravPlan.start)
	{
		TravClock = 0.f;
		TravSlowest = TNumericLimits<float>::Max();
		ResetSoleGap();
	}
	// A hang holds the catch clip at its catch; anything else plays on.
	TravTime = bHanging ? TravTime : Set.advance(TravPlan, TravTime, DeltaSeconds);
	if (bCatch && LedgeNow.phase == ftcore::LedgePhase::None && TravTime >= Clip.contact)
	{
		const ftcore::LedgeInput LIn = MakeLedgeInput();
		if (Ledge.reach(LedgeNow, LIn) == ftcore::LedgePhase::Hanging)
		{
			TravTime = Clip.contact;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Ledge: %s at %.2f s (jump %s, stick %.2f at %.0f deg off the face)."),
			LedgeNow.through ? TEXT("climbing straight up") : TEXT("hanging"), TravClock, LIn.jumpHeld ? TEXT("held") : TEXT("not held"),
			LIn.stick, LIn.stickOff);
	}
	TravClock += DeltaSeconds;
	const float Length = Clip.length;
	const float T = FMath::Min(TravTime, Length);
	const float Rate = Set.rate(TravPlan, T);
	// The plan's root in the body's space as it began: x forward, y left (UE's Y is right), z up.
	auto FeetAt = [&](float At)
	{
		const ftcore::TravPoint R = Set.rootAt(TravPlan, At);
		return TravFrame.TransformPosition(FVector(R.x, -R.y, R.z));
	};
	FVector Feet = FeetAt(T);
	// The capsule rides the floor hover above the planned feet, as CharacterMovement holds it, so the mesh stays on them.
	const FVector HalfUp(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + FloorHover);
	bool bBlocked = false;
	bool bFalls = false;
	if (bRoll)
	{
		// A roll keeps to the real floor under its path, as a walk does. A wall ends it; with no floor to stand on, the
		// body lets go and falls.
		FHitResult Wall, Floor;
		MoveOverFloor(Feet, Wall, Floor);
		bBlocked = Wall.bBlockingHit && !Move->IsWalkable(Wall);
		bFalls = !Floor.bBlockingHit || !Move->IsWalkable(Floor);
		Feet = GetActorLocation() - HalfUp;
		NoteSoleGap();
	}
	// Past the landing the clip's tail (a run on, a stand-up) is swept; the floor hover keeps the capsule clear of the
	// floor, so only a wall ahead stops it, and ends it there.
	else if (T > Clip.land)
	{
		FHitResult Hit;
		SetActorLocation(Feet + HalfUp, true, &Hit);
		bBlocked = Hit.bBlockingHit && Hit.ImpactNormal.Z < 0.7;
		Feet = GetActorLocation() - HalfUp;
		NoteSoleGap();
	}
	else
	{
		SetActorLocation(Feet + HalfUp);
	}
	// The body turns to the way it rolls, or square into the face it catches.
	if (bRoll || bCatch)
	{
		const float Gap = FMath::FindDeltaAngleDegrees(static_cast<float>(GetActorRotation().Yaw), static_cast<float>(TravFrame.Rotator().Yaw));
		AddActorWorldRotation(FRotator(0.0, Gap * (1.f - FMath::Exp(-DeltaSeconds / kRollTurnTime)), 0.0));
	}
	// The movement component's velocity is the path's at the pace the clip plays, for the locomotion now and the walk
	// after (a roll's level: its floor is the real one).
	const float Back = FMath::Max(DeltaSeconds, 1.f / 60.f);
	Move->Velocity = (FeetAt(T) - FeetAt(FMath::Max(TravPlan.start, T - Back))) / Back * Rate;
	Move->Velocity.Z = bRoll ? 0.0 : Move->Velocity.Z;
	if (LedgeNow.phase == ftcore::LedgePhase::Hanging)
	{
		Move->Velocity = FVector::ZeroVector;
	}
	if (T - Back >= TravPlan.start)
	{
		TravSlowest = FMath::Min(TravSlowest, static_cast<float>(Move->Velocity.Size2D()));
	}
	// A body that wants to move hands back to its locomotion just after the landing (a roll's body once it is
	// back up, and then and there if it wants to go well off the roll's way); one left standing plays the clip out.
	const FVector Stick = Move->GetCurrentAcceleration();
	const bool bWantsMove = bVaultCourse || !Stick.IsNearlyZero();
	const float Off = bRoll && !Stick.IsNearlyZero()
		? FMath::FindDeltaAngleDegrees(static_cast<float>(TravFrame.Rotator().Yaw), static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Stick.Y, Stick.X))))
		: 0.f;
	if (T >= Length || bBlocked || bFalls || (bWantsMove && T >= Set.exitTime(TravPlan, Off)))
	{
		if (bBlocked) Move->Velocity = FVector::ZeroVector;
		bTraversing = false;
		// A dodge ends with its roll, however it ends; its cooldown runs from here.
		if (bRoll)
		{
			Dodge.stop(DodgeNow);
		}
		if (bCatch)
		{
			Ledge.finish(LedgeNow);
		}
		// The clip plays on at the pace it had while it fades out, and under a roll's the body may set off on a turn.
		Anim->LetGoTraversal(Rate, bRoll);
		Move->Velocity.Z = 0.0;
		Move->SetMovementMode(bFalls ? MOVE_Falling : MOVE_Walking);
		LogSoleGap(TEXT("Traversal"));
		HandBackZ = GetActorLocation().Z;
		HandBackMeshZ = GetMesh()->GetComponentLocation().Z;
		bHandBackPending = !bFalls;
		const float Speed = static_cast<float>(Move->Velocity.Size2D());
		UE_LOG(LogForgottenThird, Display, TEXT("Traversal: done at %.2f of %.2f s (%s) after %.2f s, %.0f cm/s (%.0f at the slowest), %.0f cm from the start, %+.0f cm up."),
			T, Length, bBlocked ? TEXT("blocked") : bFalls ? TEXT("off an edge") : T >= Length ? TEXT("clip end") : T < Set.exitTime(TravPlan) ? TEXT("turn") : TEXT("exit"),
			TravClock, Speed, FMath::Min(TravSlowest, Speed), FVector::Dist2D(Feet, TravFrame.GetLocation()), Feet.Z - TravFrame.GetLocation().Z);
		return;
	}
	Anim->SetTraversal(TravPlan.clip, T);
}

void AFTExploreCharacter::MoveOverFloor(const FVector& To, FHitResult& Wall, FHitResult& Floor)
{
	const FVector From = GetActorLocation();
	const FVector StepUp(0.0, 0.0, GetCharacterMovement()->MaxStepHeight);
	FHitResult Hit;
	SetActorLocation(From + StepUp, true, &Hit);
	const FVector Lifted(0.0, 0.0, GetActorLocation().Z - From.Z);
	SetActorLocation(FVector(To.X, To.Y, GetActorLocation().Z), true, &Wall);
	const FVector Across = GetActorLocation();
	SetActorLocation(Across - Lifted - StepUp, true, &Floor);
	if (!Floor.bBlockingHit)
	{
		SetActorLocation(Across - Lifted);
	}
	else
	{
		// Off the floor by the hover CharacterMovement keeps, so the soles meet it and nothing pops at the hand-back.
		SetActorLocation(GetActorLocation() + FVector(0.0, 0.0, FloorHover), true);
	}
}

FVector AFTExploreCharacter::FeetLocation() const
{
	return GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + FloorHover);
}

void AFTExploreCharacter::ResetSoleGap()
{
	SoleGapMin = TNumericLimits<float>::Max();
	SoleGapMax = TNumericLimits<float>::Lowest();
}

void AFTExploreCharacter::NoteSoleGap()
{
	const UWorld* World = GetWorld();
	const FVector Feet = FeetLocation();
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(FTSoleGap), false, this);
	FHitResult Hit;
	if (World && World->LineTraceSingleByChannel(Hit, GetActorLocation(), Feet - FVector(0.0, 0.0, 50.0), ECC_Visibility, Query))
	{
		const float Gap = static_cast<float>(Feet.Z - Hit.ImpactPoint.Z);
		SoleGapMin = FMath::Min(SoleGapMin, Gap);
		SoleGapMax = FMath::Max(SoleGapMax, Gap);
	}
}

void AFTExploreCharacter::LogSoleGap(const TCHAR* What) const
{
	if (SoleGapMin <= SoleGapMax)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: soles %+.2f to %+.2f cm over the floor on its grounded frames."), What, SoleGapMin, SoleGapMax);
	}
}

bool AFTExploreCharacter::TrySlide()
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance());
	if (bTraversing || !Anim || !Move->IsMovingOnGround() || bIsCrouched || Move->MaxWalkSpeed < RunSpeed)
	{
		return false;
	}
	const ftcore::SlideSet& Set = Anim->GetSlide();
	const float Speed = static_cast<float>(GetVelocity().Size2D());
	const ftcore::SlideState Start = Set.start(Speed, -1);
	if (!Start.sliding())
	{
		return false;
	}
	// It slides on the way it was going.
	bTraversing = true;
	SlideNow = Start;
	SlideTime = 0.f;
	ResetSoleGap();
	SlideHeading = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(GetVelocity().Y, GetVelocity().X)));
	SlideFloor = Move->CurrentFloor.IsWalkableFloor() ? Move->CurrentFloor.HitResult.ImpactNormal : FVector::UpVector;
	bSlideBlocked = false;
	TravFrame = FTransform(FRotator(0.0, SlideHeading, 0.0),
		FeetLocation());
	// Nothing moves the capsule but TickSlide. The crouch is on (letting it go gets the body up), but the custom mode
	// keeps the capsule standing.
	Move->SetMovementMode(MOVE_Custom);
	Crouch();
	Anim->SetTraversal(UFTLocomotionAnimInstance::FirstSlideClip + SlideNow.clip, SlideNow.clipTime, SlideNow.loop, false);
	const ftcore::SlideIntoInfo& Into = Set.into(static_cast<std::size_t>(SlideNow.into));
	UE_LOG(LogForgottenThird, Display, TEXT("Slide: in with clip %d from %.2f s (take-off foot %d, its %.0f cm/s, x%.2f) at %.0f cm/s, heading %.0f, facing %.0f."),
		SlideNow.clip, SlideNow.clipTime, Into.takeoffFoot, Into.entrySpeed, SlideNow.entryRate, Speed, SlideHeading, GetActorRotation().Yaw);
	return true;
}

void AFTExploreCharacter::TickSlide(float DeltaSeconds)
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance());
	if (!Anim)
	{
		SlideNow = ftcore::SlideState();
		bTraversing = false;
		Move->SetMovementMode(MOVE_Walking);
		return;
	}
	// What the player asks for, and the floor's rise along the way the body slides (by its normal last met).
	auto WayOf = [](float Yaw)
	{
		const float Rad = FMath::DegreesToRadians(Yaw);
		return FVector(FMath::Cos(Rad), FMath::Sin(Rad), 0.0);
	};
	const FVector Way = WayOf(SlideHeading);
	const double Rise = -(SlideFloor.X * Way.X + SlideFloor.Y * Way.Y) / FMath::Max(SlideFloor.Z, 0.1);
	const FVector Stick = Move->GetCurrentAcceleration();
	const bool bStick = Stick.SizeSquared2D() > 1.0;
	ftcore::SlideInput In;
	In.slope = static_cast<float>(Rise / FMath::Sqrt(1.0 + Rise * Rise));
	In.wantSpeed = bStick ? Move->MaxWalkSpeed * Move->GetAnalogInputModifier() : 0.f;
	In.turn = bStick ? FMath::FindDeltaAngleDegrees(SlideHeading, static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Stick.Y, Stick.X)))) : 0.f;
	In.sprint = Move->MaxWalkSpeed >= RunSpeed;
	In.crouch = Move->bWantsToCrouch;
	In.blocked = bSlideBlocked;
	In.jump = SlideJumpAsked > 0.f;
	SlideJumpAsked = FMath::Max(SlideJumpAsked - DeltaSeconds, 0.f);
	bSlideBlocked = false;
	const ftcore::SlideSet& Set = Anim->GetSlide();
	const ftcore::SlidePhase Was = SlideNow.phase;
	Set.step(SlideNow, In, DeltaSeconds);
	SlideTime += DeltaSeconds;
	const FVector Feet = FeetLocation();
	if (SlideNow.jumped)
	{
		// Cancelled into a jump: the body stands, lets the slide's clip go and takes off along the way it slid at the
		// speed Core hands back (the slide's momentum), the jump's own rise on top.
		const FVector Off = WayOf(SlideHeading) * SlideNow.speed;
		UE_LOG(LogForgottenThird, Display, TEXT("Slide: jumped out at %.2f s (%s) at %.0f cm/s, %.0f cm from the start."), SlideTime,
			Was == ftcore::SlidePhase::Into ? TEXT("dropping in") : Was == ftcore::SlidePhase::Down ? TEXT("down") : TEXT("getting up"),
			SlideNow.speed, FVector::Dist2D(Feet, TravFrame.GetLocation()));
		bTraversing = false;
		bSlideBlocked = false;
		SlideJumpAsked = 0.f;
		SlideNow = ftcore::SlideState();
		UnCrouch();
		Anim->SetTraversal(-1, 0.f);
		Move->SetMovementMode(MOVE_Walking);
		Move->Velocity = Off;
		LaunchCharacter(FVector(Off.X, Off.Y, Move->JumpZVelocity), true, true);
		return;
	}
	if (Was == ftcore::SlidePhase::Into && SlideNow.phase != ftcore::SlidePhase::Into)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Slide: down at %.2f s (clip %d at %.2f), %.0f cm/s, %.0f cm from the start, slope %+.2f."), SlideTime,
			SlideNow.into, SlideNow.phase == ftcore::SlidePhase::Out ? Set.into(static_cast<std::size_t>(SlideNow.into)).down : SlideNow.clipTime, SlideNow.speed,
			FVector::Dist2D(Feet, TravFrame.GetLocation()), In.slope);
	}
	if (Was != ftcore::SlidePhase::Out && (SlideNow.phase == ftcore::SlidePhase::Out || !SlideNow.sliding()))
	{
		// Getting up into a crouch keeps it on, so the capsule crouches once the body walks again; any other stands.
		if (ftcore::crouchedOut(SlideNow.out))
		{
			Crouch();
		}
		else
		{
			UnCrouch();
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Slide: getting up into %s (clip %d) at %.2f s from %.0f cm/s (%s), %.0f cm from the start."),
			SlideOutNames[static_cast<int32>(SlideNow.out)], SlideNow.clip, SlideTime, SlideNow.outFrom,
			!In.crouch ? TEXT("crouch let go") : SlideNow.getUp ? TEXT("a wall") : TEXT("slowed"), FVector::Dist2D(Feet, TravFrame.GetLocation()));
	}

	// It turns as the stick steers and goes on along the real floor at the slide's speed. A wall met square on stops
	// it; met at a glance, it turns along the wall, keeping the speed that goes that way.
	SlideHeading = FMath::UnwindDegrees(SlideHeading + SlideNow.yawStep);
	FHitResult Wall, Floor;
	MoveOverFloor(GetActorLocation() + WayOf(SlideHeading) * (SlideNow.speed * DeltaSeconds), Wall, Floor);
	const bool bFalls = !Floor.bBlockingHit || !Move->IsWalkable(Floor);
	if (!bFalls)
	{
		SlideFloor = Floor.ImpactNormal;
		NoteSoleGap();
	}
	if (Wall.bBlockingHit && !Move->IsWalkable(Wall))
	{
		const FVector Ahead = WayOf(SlideHeading);
		const FVector Normal = Wall.Normal.GetSafeNormal2D();
		const double Square = -FVector::DotProduct(Normal, Ahead);
		if (Square >= kSlideHeadOn)
		{
			bSlideBlocked = true;
		}
		else if (Square > 0.0)
		{
			const FVector Along = (Ahead + Normal * Square).GetSafeNormal2D();
			SlideNow.speed *= static_cast<float>(FVector::DotProduct(Ahead, Along));
			SlideHeading = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Along.Y, Along.X)));
		}
	}
	// The movement component's velocity is the slide's, for the locomotion now and the walk after; the body turns to the
	// way it slides.
	Move->Velocity = WayOf(SlideHeading) * (bSlideBlocked ? 0.f : SlideNow.speed);
	const float Gap = FMath::FindDeltaAngleDegrees(static_cast<float>(GetActorRotation().Yaw), SlideHeading);
	AddActorWorldRotation(FRotator(0.0, Gap * (1.f - FMath::Exp(-DeltaSeconds / kRollTurnTime)), 0.0));

	// It hands back once up, off an edge (falling), or stopped by a wall while getting up.
	const bool bStopped = bSlideBlocked && SlideNow.phase == ftcore::SlidePhase::Out;
	if (!SlideNow.sliding() || bFalls || bStopped)
	{
		if (bFalls)
		{
			UnCrouch();
		}
		const FVector Ended = FeetLocation();
		UE_LOG(LogForgottenThird, Display, TEXT("Slide: done (%s) at %.2f s, %s at %.2f s, %.0f cm/s, %.0f cm from the start, %+.0f cm up."),
			bFalls ? TEXT("off an edge") : bStopped ? TEXT("blocked") : TEXT("up"), SlideTime, SlideOutNames[static_cast<int32>(SlideNow.out)],
			SlideNow.clipTime, Move->Velocity.Size2D(), FVector::Dist2D(Ended, TravFrame.GetLocation()), Ended.Z - TravFrame.GetLocation().Z);
		bTraversing = false;
		bSlideBlocked = false;
		SlideNow = ftcore::SlideState();
		Anim->SetTraversal(-1, 0.f);
		Move->SetMovementMode(bFalls ? MOVE_Falling : MOVE_Walking);
		LogSoleGap(TEXT("Slide"));
		HandBackZ = GetActorLocation().Z;
		HandBackMeshZ = GetMesh()->GetComponentLocation().Z;
		bHandBackPending = !bFalls;
		return;
	}
	Anim->SetTraversal(UFTLocomotionAnimInstance::FirstSlideClip + SlideNow.clip, SlideNow.clipTime, SlideNow.loop, false);
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
	// far end), and a high block to climb; for -LocoCatch, deep blocks to jump at and catch: a low one, one at the edge
	// of reach (caught at the top of the jump, near the most a catch may miss by, and high enough that the drop off its
	// far end ends in a roll), and one caught at the top of the jump within reach, far enough on for that roll.
	struct FBlock
	{
		float At, Depth, Height;
	};
	const FBlock VaultBlocks[] = {{700.f, 25.f, 100.f}, {1500.f, 300.f, 100.f}, {2600.f, 300.f, 240.f}};
	const FBlock CatchBlocks[] = {{700.f, 300.f, 150.f}, {1500.f, 300.f, 300.f}, {3100.f, 300.f, 260.f}};
	const TArrayView<const FBlock> Blocks = bCatchCourse ? MakeArrayView(CatchBlocks) : MakeArrayView(VaultBlocks);
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
	// The vaults are run at in a sprint, the catches in the default run.
	GetCharacterMovement()->MaxWalkSpeed = bCatchCourse ? WalkSpeed : RunSpeed;
	bVaultCourse = true;
}

void AFTExploreCharacter::HandleCrouch()
{
	// Hanging from a ledge, crouch lets go (Tick, through ftcore::LedgeHang).
	if (LedgeNow.phase == ftcore::LedgePhase::Hanging)
	{
		LedgeDropAsked = kRollBuffer;
		return;
	}
	// In a sprint, crouch slides (TrySlide); pressed again mid-slide it gets the body up.
	if (!GetCharacterMovement()->bWantsToCrouch && TrySlide())
	{
		return;
	}
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

void AFTExploreCharacter::ResetHeldInput()
{
	HandleRunStop();
	RollAsked = 0.f;
	bRollAskedAir = false;
	SlideJumpAsked = 0.f;
	LedgeJumpAsked = LedgeDropAsked = 0.f;
	bJumpHeld = false;
	UnCrouch();
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
	// Not mid-traversal (the body is carried along its clip); a crouch ends, as the new body starts standing.
	if (bTraversing)
	{
		return;
	}
	UnCrouch();
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
	// A frame after a traversal or slide hands back: how far CharacterMovement moved the capsule (a pop if not ~0).
	if (bHandBackPending)
	{
		bHandBackPending = false;
		const FFindFloorResult& Floor = GetCharacterMovement()->CurrentFloor;
		UE_LOG(LogForgottenThird, Display, TEXT("Hand-back: on CharacterMovement's first frame the mesh moved %+.2f cm, the capsule %+.2f (crouched %d; floor dist %.2f cm, walkable %d)."),
			GetMesh()->GetComponentLocation().Z - HandBackMeshZ, GetActorLocation().Z - HandBackZ, bIsCrouched ? 1 : 0, Floor.FloorDist, Floor.IsWalkableFloor() ? 1 : 0);
	}
	if (bPortraitWalk)
	{
		const double Turn = GetWorld()->GetTimeSeconds() * (2.0 * UE_DOUBLE_PI / 8.0);
		AddMovementInput(FVector(FMath::Cos(Turn), FMath::Sin(Turn), 0.0), PortraitMoveScale);
		const double Now = GetWorld()->GetTimeSeconds();
		if (bPortraitJump && Now - PortraitJumpAt > 1.5 && GetCharacterMovement()->IsMovingOnGround())
		{
			PortraitJumpAt = Now;
			Jump();
		}
		else if (bPortraitJump && Now - PortraitJumpAt > 0.3)
		{
			StopJumping();
		}
	}
	const UFTLocomotionAnimInstance* LocoAnim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance());
	// -LocoVaultWall (or -LocoCatch) builds the course once the body has settled where the map puts it, then runs along
	// it, traversing what it can (or jumping at each wall from about a metre out, the jump held through the flight).
	if (bCoursePending && GetWorld()->GetTimeSeconds() > 1.0 && GetCharacterMovement()->IsMovingOnGround())
	{
		bCoursePending = false;
		SpawnTraversalCourse();
	}
	if (bVaultCourse && !bTraversing)
	{
		AddMovementInput(CourseDir, 1.f);
		if (!bCatchCourse)
		{
			TryTraverse(false);
		}
		else if (GetCharacterMovement()->IsMovingOnGround())
		{
			bJumpHeld = false;
			ftcore::TraversalObstacle Ahead;
			FVector In;
			if (LocoAnim && ProbeObstacle(LocoAnim->GetTraversal(), false, Ahead, In) && Ahead.distance >= kCourseJumpFrom
				&& Ahead.distance <= kCourseJumpTo)
			{
				bJumpHeld = true;
				Jump();
			}
		}
	}
	// In the air, a ledge within reach is caught when asked for (TryCatch).
	if (!bTraversing && GetCharacterMovement()->IsFalling())
	{
		TryCatch(bCatchCourse);
	}
	else
	{
		CatchMissSaid = -1;
	}
	// A roll press still kept is tried again, and a landing the locomotion rolls out of starts one, as does touching
	// down with a press from the air still kept (the locomotion may not have seen the touchdown yet).
	RollAsked = FMath::Max(RollAsked - DeltaSeconds, 0.f);
	bRollAskedAir = bRollAskedAir && RollAsked > 0.f;
	const bool bLandingRoll = LocoAnim && (LocoAnim->GetLocomotion().landRoll() || (bRollAskedAir && GetCharacterMovement()->IsMovingOnGround()));
	if ((RollAsked > 0.f || bLandingRoll) && TryRoll(bLandingRoll, bLandingRoll))
	{
		RollAsked = 0.f;
		bRollAskedAir = false;
	}
	if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance()))
	{
		Anim->SetRollAsked(RollAsked > 0.f);
	}
	if (bTraversing)
	{
		if (SlideNow.sliding())
		{
			TickSlide(DeltaSeconds);
		}
		else
		{
			TickTraversal(DeltaSeconds);
		}
	}
	// A vault or a climb holds the facing it began with; a roll or a slide turns to its way, a catch square into the face
	// (TickTraversal, TickSlide).
	const UFTLocomotionAnimInstance* Steered = bTraversing ? nullptr : LocoAnim;
	if (const UFTLocomotionAnimInstance* Anim = Steered)
	{
		// The step holds over a pose with no time passed, so a frame with none (the actor's time dilated to 0)
		// takes none, rather than the last one again.
		const float Step = Anim->GetLocomotion().yawStep();
		if (Step != 0.f && DeltaSeconds > 0.f)
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
	Dodge.tick(DodgeNow, DeltaSeconds);
	if (bCatchCourse && LedgeNow.phase == ftcore::LedgePhase::Hanging && LedgeNow.time >= kCourseHang)
	{
		LedgeJumpAsked = kRollBuffer;
	}
	{
		const ftcore::LedgePhase Was = LedgeNow.phase;
		Ledge.step(LedgeNow, MakeLedgeInput(), DeltaSeconds);
		LedgeJumpAsked = FMath::Max(LedgeJumpAsked - DeltaSeconds, 0.f);
		LedgeDropAsked = FMath::Max(LedgeDropAsked - DeltaSeconds, 0.f);
		if (Was == ftcore::LedgePhase::Hanging && LedgeNow.phase == ftcore::LedgePhase::Climbing)
		{
			LedgeJumpAsked = 0.f;
			UE_LOG(LogForgottenThird, Display, TEXT("Ledge: climbing up %.2f s into the catch."), TravClock);
		}
	}
	if (RollEvery > 0.f && (RollTimer += DeltaSeconds) >= RollEvery)
	{
		RollTimer = 0.f;
		HandleRoll();
	}
	if (!bTraversing)
	{
		RollTurnLeft = FMath::Max(RollTurnLeft - DeltaSeconds, 0.f);
	}
}

void AFTExploreCharacter::AddMovementInput(FVector WorldDirection, float ScaleValue, bool bForce)
{
	if (PulseEvery > 0.f && PulseTimer > PulseEvery)
	{
		return;
	}
	if (RollTurnLeft > 0.f)
	{
		WorldDirection = WorldDirection.RotateAngleAxis(RollTurn, FVector::UpVector);
	}
	Super::AddMovementInput(WorldDirection, ScaleValue, bForce);
}

void AFTExploreCharacter::SetupLocomotion(const FFTLoadedLook& Loaded)
{
	USkeletalMeshComponent* Body = GetMesh();
	Body->SetAnimInstanceClass(UFTLocomotionAnimInstance::StaticClass());
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());
	if (!Anim)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: no locomotion anim instance; the body stays in its ref pose."));
		return;
	}
	Anim->SetBaseClips(Loaded.Clips, true);
	// The crouch walk plays at its own pace at the crouched top speed.
	const float Crouched = Anim->GetLocomotion().clip(ftcore::LocoClip::CrouchWalk).groundSpeed;
	if (Crouched > 0.f)
	{
		CrouchSpeed = Crouched;
		GetCharacterMovement()->MaxWalkSpeedCrouched = Crouched;
	}
}
