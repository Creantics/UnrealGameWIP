#include "FTExploreCharacter.h"
#include "FTPartyVan.h"
#include "FTWaterSubsystem.h"
#include "ftcore/MassingPlan.h"

#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
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
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "Engine/GameViewportClient.h"
#include "UnrealClient.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/ConstructorHelpers.h"

#include "FTExploreDirector.h"
#include "FTExploreHUD.h"
#include "FTCutsceneDirector.h"
#include "FTFieldDummy.h"
#include "FTGameInstance.h"
#include "FTInputBindings.h"
#include "FTPauseSubsystem.h"
#include "FTMeshFacing.h"
#include "FTPartyFollower.h"
#include "FTSceneLook.h"
#include "Components/DecalComponent.h"
#include "Misc/PackageName.h"
#include "ForgottenThird.h"
#include "FTText.h"

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
// An attack turns the lead to the target it aims at this fast (s, exponential).
constexpr float kAttackTurnTime = 0.04f;
// The follow camera's shoulder offset and field of view off aim (the aim stance lerps to Data/Battle/aim.json's).
const FVector kShoulder(0.0, 70.0, 60.0);
constexpr float kFov = 70.f;
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
	Boom->SocketOffset = kShoulder;
	Boom->bUsePawnControlRotation = true;
	Boom->bEnableCameraLag = true;
	Boom->CameraLagSpeed = 12.f;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Boom, USpringArmComponent::SocketName);
	Camera->SetRelativeRotation(FRotator(-10.0, 0.0, 0.0));
	Camera->bUsePawnControlRotation = false;
	Camera->SetFieldOfView(kFov);

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
	{
		// -LeadGo=<x,y> (plan frame of -WorldEyeSide, default 1000 m) [-LeadGoAfter=<s>] [-LeadGoJump] [-LeadGoQuit=<s>].
		FString Go;
		FString GoX, GoY;
		if (FParse::Value(FCommandLine::Get(), TEXT("LeadGo="), Go, false) && Go.Split(TEXT(","), &GoX, &GoY))
		{
			double GoSide = 1000.0;
			FParse::Value(FCommandLine::Get(), TEXT("WorldEyeSide="), GoSide);
			const ftcore::MassingVec To = ftcore::massingEnginePoint(GoSide, FCString::Atod(*GoX), FCString::Atod(*GoY), 0.0);
			LeadGoTo = FVector(To.x * 100.0, To.y * 100.0, 0.0);
			LeadGoAfter = 3.f;
			FParse::Value(FCommandLine::Get(), TEXT("LeadGoAfter="), LeadGoAfter);
			LeadGoAfter = FMath::Max(LeadGoAfter, 0.f);
			bLeadGoJump = FParse::Param(FCommandLine::Get(), TEXT("LeadGoJump"));
			FParse::Value(FCommandLine::Get(), TEXT("LeadGoQuit="), LeadGoQuit);
		}
		FParse::Value(FCommandLine::Get(), TEXT("LeadGoPace="), LeadGoPace);
		LeadGoPace = FMath::Clamp(LeadGoPace, 0.1f, 1.f);
	}
	AFTPartyVan::SpawnAt(GetWorld()); // -PartyVanAt only; one van however many leads begin
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
	Dodge.setTuning(UFTLocomotionAnimInstance::GetMoveTuning().dodge);
	Ledge.setTuning(UFTLocomotionAnimInstance::GetMoveTuning().ledge);
	DodgeNow = Dodge.fresh();
	// The portrait and look-dev captures (-ExplorePortrait and every -ExplorePortrait*/-ExploreLookDev*/-ExploreFixedExposure/
	// -ExploreGreyGround flag under it) are capture tools: compiled out of shipping.
#if UE_BUILD_SHIPPING
	const bool bPortrait = false;
#else
	const bool bPortrait = FParse::Param(FCommandLine::Get(), TEXT("ExplorePortrait"));
#endif
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
		// A party slot without attacks is never the lead: the first look that has them starts instead.
		if (!IsPlayableLead(PartyLooks[LeadIndex]))
		{
			const int32 Playable = PartyLooks.IndexOfByPredicate([this](const FFTLeadLook& Member)
			{
				return IsPlayableLead(Member);
			});
			UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: %s has no attacks and can't be the lead; starting as %s."),
				*PartyLooks[LeadIndex].Name, Playable >= 0 ? *PartyLooks[Playable].Name : TEXT("him anyway (no look has attacks)"));
			LeadIndex = Playable >= 0 ? Playable : LeadIndex;
		}
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
	// -ExploreWalk: the lead walks the portrait's slow circle outside a portrait too (moving attacks in captures).
	bPortraitWalk = FParse::Param(FCommandLine::Get(), TEXT("ExploreWalk"));
	// -ExploreWedge drops the lead into a V trench too steep to stand in; -ExploreWedgeShot=<path prefix> films it
	// in the trench and after the step back (<prefix>_in.png, <prefix>_out.png), then quits.
	bWedgePending = FParse::Param(FCommandLine::Get(), TEXT("ExploreWedge"));
	FParse::Value(FCommandLine::Get(), TEXT("ExploreWedgeShot="), WedgeShotPrefix);
	// -ExploreDummy stands a field dummy ahead once the lead is on the floor; -ExploreAttackEvery=<s> presses light.
	bDummyPending = FParse::Param(FCommandLine::Get(), TEXT("ExploreDummy"));
	FParse::Value(FCommandLine::Get(), TEXT("ExploreAttackEvery="), AttackEvery);
	// -ExploreAim=<s>: aim is held from s seconds of play on, as right mouse held would.
	FParse::Value(FCommandLine::Get(), TEXT("ExploreAim="), AimFrom);
	// -ExploreWeaponShot=<path prefix>: once -ExploreWeaponShotDelay=<s> (default 12) has passed, a 1600x900 shot (HUD
	// hidden) as each attack goes active, <prefix>_<n>_<clip>.png, -ExploreWeaponShots=<n> of them (default 3), then quit.
	if (FParse::Value(FCommandLine::Get(), TEXT("ExploreWeaponShot="), WeaponShotPrefix) && !WeaponShotPrefix.IsEmpty())
	{
		FParse::Value(FCommandLine::Get(), TEXT("ExploreWeaponShots="), WeaponShotsLeft);
		FParse::Value(FCommandLine::Get(), TEXT("ExploreWeaponShotDelay="), WeaponShotDelay);
		// -ExploreWeaponStrip=<n> [-ExploreWeaponStripStep=<s>, default 0.1]: one walking light attack filmed n times,
		// s seconds apart from its start (the proof the feet keep walking), speed and upper share logged each frame.
		if (FParse::Value(FCommandLine::Get(), TEXT("ExploreWeaponStrip="), StripShots) && StripShots > 0)
		{
			FParse::Value(FCommandLine::Get(), TEXT("ExploreWeaponStripStep="), StripStep);
			WeaponShotsLeft = StripShots;
		}
	}
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
		bPortraitWalk = bPortraitWalk || FParse::Param(FCommandLine::Get(), TEXT("ExplorePortraitWalk"));
		// -ExplorePortraitMoveScale=<0..1>: the circle's input scale (0.5 walks, 1 runs); it implies the walk circle.
		// -ExplorePortraitJump: on the circle, the body jumps every 1.5 s, so shots catch the jump clips.
		if (FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitMoveScale="), PortraitMoveScale))
		{
			PortraitMoveScale = FMath::Clamp(PortraitMoveScale, 0.f, 1.f);
			bPortraitWalk = true;
		}
		bPortraitJump = FParse::Param(FCommandLine::Get(), TEXT("ExplorePortraitJump"));
		bPortraitWalk = bPortraitWalk || bPortraitJump;
		if (FParse::Param(FCommandLine::Get(), TEXT("ExploreLookDev")))
		{
			EnterLookDev();
		}
		else if (FParse::Param(FCommandLine::Get(), TEXT("ExplorePortraitLights")))
		{
			AddPortraitLights();
		}
		// Map-light diagnosis (the look-dev set has both built in): -ExploreGreyGround paints the map's green ground,
		// path and leaves a neutral 0.18 grey; -ExploreFixedExposure turns auto-exposure off (EV100 0, shifted by
		// -ExploreExposureBias=<stops>).
		if (!FParse::Param(FCommandLine::Get(), TEXT("ExploreLookDev")))
		{
			if (FParse::Param(FCommandLine::Get(), TEXT("ExploreGreyGround")))
			{
				GreyGround();
			}
			if (FParse::Param(FCommandLine::Get(), TEXT("ExploreFixedExposure")))
			{
				float Bias = 0.f;
				FParse::Value(FCommandLine::Get(), TEXT("ExploreExposureBias="), Bias);
				FixExposure(Bias);
			}
		}
		// Exposure-range check: -ExploreNight dims the sun to moonlight and the sky to a night sky (the gate lamps stay
		// lit); -ExploreExposureRange=<min>,<max> meters auto-exposure within that EV100 range instead of the map's.
		if (FParse::Param(FCommandLine::Get(), TEXT("ExploreNight")))
		{
			Nightfall();
		}
		FString Range;
		if (FParse::Value(FCommandLine::Get(), TEXT("ExploreExposureRange="), Range, false))
		{
			FString Lo, Hi;
			if (Range.Split(TEXT(","), &Lo, &Hi) && Camera)
			{
				FPostProcessSettings& Post = Camera->PostProcessSettings;
				Post.bOverride_AutoExposureMinBrightness = true;
				Post.AutoExposureMinBrightness = FCString::Atof(*Lo);
				Post.bOverride_AutoExposureMaxBrightness = true;
				Post.AutoExposureMaxBrightness = FCString::Atof(*Hi);
				Camera->PostProcessBlendWeight = 1.f;
				UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: auto-exposure range %.2f..%.2f EV100."),
					Post.AutoExposureMinBrightness, Post.AutoExposureMaxBrightness);
			}
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
	// A moving light (or a face clip) makes shots at the same yaw differ, so they carry their index.
	const FString File = FaceClip ? FString::Printf(TEXT("%s_%d_f%d.png"), *ShotPrefix, FMath::RoundToInt(Yaw), ShotIndex)
		: LookDevSpin != 0.f      ? FString::Printf(TEXT("%s_%d_s%02d.png"), *ShotPrefix, FMath::RoundToInt(Yaw), ShotIndex)
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

void AFTExploreCharacter::FixExposure(float Bias)
{
	if (!Camera)
	{
		return;
	}
	FPostProcessSettings& Post = Camera->PostProcessSettings;
	Post.bOverride_AutoExposureMethod = true;
	Post.AutoExposureMethod = AEM_Manual;
	Post.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Post.AutoExposureApplyPhysicalCameraExposure = false;
	Post.bOverride_AutoExposureBias = true;
	Post.AutoExposureBias = Bias;
	Camera->PostProcessBlendWeight = 1.f;
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: fixed exposure, bias %.2f."), Bias);
}

void AFTExploreCharacter::Nightfall()
{
	int32 Dimmed = 0;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		TInlineComponentArray<UDirectionalLightComponent*> Suns(*It);
		for (UDirectionalLightComponent* Sun : Suns)
		{
			Sun->SetIntensity(Sun->Intensity * 0.002f); // about 0.2 lux of moonlight from a 100 lux sun
			Sun->SetLightColor(FLinearColor(0.6f, 0.7f, 1.f));
			++Dimmed;
		}
		TInlineComponentArray<USkyLightComponent*> Skies(*It);
		for (USkyLightComponent* Sky : Skies)
		{
			Sky->SetIntensity(Sky->Intensity * 0.01f);
			++Dimmed;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: night (%d sun and sky lights dimmed)."), Dimmed);
}

void AFTExploreCharacter::GreyGround()
{
	// The Explore map's surfaces are tints of one material (MI_Explore_<name>); the green and earth ones go grey.
	int32 Painted = 0;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		TInlineComponentArray<UMeshComponent*> Meshes(*It);
		for (UMeshComponent* Surface : Meshes)
		{
			for (int32 Slot = 0; Slot < Surface->GetNumMaterials(); ++Slot)
			{
				UMaterialInterface* Material = Surface->GetMaterial(Slot);
				const FString Name = Material ? Material->GetName() : FString();
				if (Name == TEXT("MI_Explore_Ground") || Name == TEXT("MI_Explore_Path") || Name == TEXT("MI_Explore_Leaves"))
				{
					UMaterialInstanceDynamic* Grey = UMaterialInstanceDynamic::Create(Material, this);
					Grey->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.18f, 0.18f, 0.18f));
					Surface->SetMaterial(Slot, Grey);
					++Painted;
				}
			}
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: grey ground on %d slots."), Painted);
}

void AFTExploreCharacter::EnterLookDev()
{
	// A neutral look-development set, so a material is judged without the map's coloured bounce: every other actor
	// and every light, sky and fog in the level is hidden (the floor still collides), leaving a black surround; a
	// mid-grey (0.18) disc goes under the feet; one warm-neutral key high on the camera's left and one soft dim fill
	// on its right light the pawn. The key hangs off a pivot at the pawn, which -ExploreLookDevSpin=<deg/s> turns.
	int32 Hidden = 0;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == this || Actor->IsA<AController>() || Actor->IsA<AHUD>() || Actor->IsA<APlayerCameraManager>())
		{
			continue;
		}
		Actor->SetActorHiddenInGame(true);
		TInlineComponentArray<USceneComponent*> Components(Actor);
		for (USceneComponent* Component : Components)
		{
			Component->SetVisibility(false, false);
			if (ULightComponent* Light = Cast<ULightComponent>(Component))
			{
				Light->SetIntensity(0.f);
			}
		}
		++Hidden;
	}
	if (UStaticMesh* Disc = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")))
	{
		UStaticMeshComponent* Ground = NewObject<UStaticMeshComponent>(this);
		Ground->SetStaticMesh(Disc);
		Ground->SetUsingAbsoluteRotation(true);
		Ground->SetUsingAbsoluteScale(true);
		Ground->SetWorldScale3D(FVector(8.0, 8.0, 0.01));
		Ground->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ground->SetupAttachment(GetRootComponent());
		Ground->RegisterComponent();
		Ground->SetWorldLocation(FeetLocation() + FVector(0.0, 0.0, 0.2));
		if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
		{
			UMaterialInstanceDynamic* Grey = UMaterialInstanceDynamic::Create(Base, this);
			Grey->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.18f, 0.18f, 0.18f));
			Ground->SetMaterial(0, Grey);
		}
	}
	FParse::Value(FCommandLine::Get(), TEXT("ExploreLookDevSpin="), LookDevSpin);
	LookDevPivot = NewObject<USceneComponent>(this);
	LookDevPivot->SetupAttachment(GetRootComponent());
	LookDevPivot->SetUsingAbsoluteRotation(true);
	LookDevPivot->RegisterComponent();
	// Pivot space at yaw 0: the light sits at -X, so it starts on the side the default (yaw 180) camera looks from.
	struct FLightSpec
	{
		USceneComponent* Parent;
		FVector Location;
		float Candelas;
		float Width;
		FLinearColor Color;
	};
	const FLightSpec Specs[] = {
		{LookDevPivot, FVector(-250.0, -170.0, 190.0), 40.f, 90.f, FLinearColor(1.f, 0.97f, 0.92f)}, // key
		{Boom, FVector(-260.0, 240.0, 30.0), 6.f, 240.f, FLinearColor(0.95f, 0.97f, 1.f)},        // fill
	};
	const FVector Target(0.0, 0.0, 20.0);
	for (const FLightSpec& Spec : Specs)
	{
		URectLightComponent* Light = NewObject<URectLightComponent>(this);
		Light->SetupAttachment(Spec.Parent);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetRelativeLocation(Spec.Location);
		Light->SetRelativeRotation((Target - Spec.Location).Rotation());
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetIntensity(Spec.Candelas);
		Light->SetSourceWidth(Spec.Width);
		Light->SetSourceHeight(Spec.Width);
		Light->SetLightColor(Spec.Color);
		Light->SetAttenuationRadius(1500.f);
		Light->RegisterComponent();
		PortraitLights.Add(Light);
	}
	// The default camera looks from yaw 180; turn the key round to its side.
	LookDevPivot->SetWorldRotation(FRotator(0.0, 180.0, 0.0));
	// Fixed exposure: auto-exposure in a black surround brightens everything, so albedo could not be judged between
	// shots. Manual exposure at EV100 0: the 40 cd key at about 3.3 m lights a 0.18 grey to about 0.18 on screen.
	// -ExploreLookDevBias=<stops> shifts it.
	float Bias = 0.f;
	FParse::Value(FCommandLine::Get(), TEXT("ExploreLookDevBias="), Bias);
	FixExposure(Bias);
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: look-dev set on (%d actors hidden, key spin %.1f deg/s)."), Hidden, LookDevSpin);
}

#if !UE_BUILD_SHIPPING
namespace
{
// -LookFrontProbe=<closed>+<open> (look-dev, headless): the look worn with one garment pair (in place of its own) made of two existing
// meshes, so the runtime open/closed switch (FTLook::SetFront) can be exercised before a real open mesh exists. The
// closed half is added to the pieces unless either half is listed. False without the flag or when it doesn't parse.
bool FrontProbeLook(const FFTLeadLook& Look, FFTLeadLook& Out)
{
	FString Arg;
	if (!FParse::Value(FCommandLine::Get(), TEXT("LookFrontProbe="), Arg))
	{
		return false;
	}
	FString Closed, Open;
	if (!Arg.Split(TEXT("+"), &Closed, &Open) || Closed.IsEmpty() || Open.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: -LookFrontProbe=%s isn't <closed>+<open>; ignored."), *Arg);
		return false;
	}
	Out = Look;
	FFTGarmentFronts Pair;
	Pair.Closed = FSoftObjectPath(Closed);
	Pair.Open = FSoftObjectPath(Open);
	Pair.bOpen = false;
	Out.Fronts = {Pair};  // alone: a configured pair naming the same mesh would match first
	if (!Out.PiecePaths.Contains(Pair.Closed) && !Out.PiecePaths.Contains(Pair.Open))
	{
		Out.PiecePaths.Add(Pair.Closed);
	}
	return true;
}

// One line per piece: its mesh, whether the body leads its pose, and what each slot shows.
void LogPieces(const TCHAR* Step, const USkeletalMeshComponent& Body, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces)
{
	for (const USkeletalMeshComponent* Piece : Pieces)
	{
		if (!Piece)
		{
			continue;
		}
		FString Slots;
		for (int32 I = 0; I < Piece->GetNumMaterials(); ++I)
		{
			const UMaterialInterface* Material = Piece->GetMaterial(I);
			Slots += FString::Printf(TEXT(" [%d]%s"), I, Material ? *Material->GetName() : TEXT("none"));
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FRONT_PROBE %s: %s leader=%s slots:%s"), Step,
			Piece->GetSkeletalMeshAsset() ? *Piece->GetSkeletalMeshAsset()->GetName() : TEXT("none"),
			Piece->LeaderPoseComponent.Get() == &Body ? TEXT("body") : TEXT("OTHER"), *Slots);
	}
}
}
#endif

void AFTExploreCharacter::WearLook(const FFTLeadLook& InLook)
{
#if !UE_BUILD_SHIPPING
	FFTLeadLook ProbeLook;
	const bool bFrontProbe = FrontProbeLook(InLook, ProbeLook);
	const FFTLeadLook& Look = bFrontProbe ? ProbeLook : InLook;
#else
	const FFTLeadLook& Look = InLook;
#endif
	// A new look is a swap: the safe points the last lead walked stay with it.
	Unstick.reset();
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
	WornFronts = Loaded.Fronts;
	const int32 Cloth = FTLook::AddPieces(*this, *BodyComponent, Loaded.Pieces, Pieces, TEXT("FTExploreCharacter"));
	FTLook::AddGrooms(*this, *BodyComponent, Pieces, Look.Grooms, GroomComponents, TEXT("FTExploreCharacter"));
	FTLook::SwapMaterials(*BodyComponent, Pieces, Look.MaterialSwaps, TEXT("FTExploreCharacter"));
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: %s: body %s facing %.0f, %d pieces (%d with cloth), %d grooms, clips from %s."),
		*Look.Name, Body ? *Body->GetName() : TEXT("none"), BodyComponent->GetRelativeRotation().Yaw, Pieces.Num(), Cloth,
		GroomComponents.Num(), *Loaded.ClipFolder);
#if !UE_BUILD_SHIPPING
	if (bFrontProbe)
	{
		LogPieces(TEXT("setup"), *BodyComponent, Pieces);
		const int32 Opened = FTLook::SetFront(Look, WornFronts, *BodyComponent, Pieces, FString(), true, TEXT("FTExploreCharacter"));
		LogPieces(TEXT("open"), *BodyComponent, Pieces);
		const int32 Closed = FTLook::SetFront(Look, WornFronts, *BodyComponent, Pieces, FString(), false, TEXT("FTExploreCharacter"));
		LogPieces(TEXT("closed"), *BodyComponent, Pieces);
		UE_LOG(LogForgottenThird, Display, TEXT("FRONT_PROBE done: %d piece(s) opened, %d closed."), Opened, Closed);
	}
#endif
	// A body that didn't load keeps the clips worn with it.
	if (Loaded.Body)
	{
		SetupLocomotion(Loaded);
	}
	BeginWeapon(Look);
}

void AFTExploreCharacter::LoadWeaponTable()
{
	if (!bWeaponTableTried)
	{
		bWeaponTableTried = true;
		TArray<uint8> Bytes;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/arena.json");
		std::string Error;
		std::vector<std::string> Warnings;
		bWeaponTable = IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File)
			&& ftcore::importArenaTable(FT::BytesAsText(Bytes), WeaponTable, &Error, &Warnings);
		if (!bWeaponTable)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Field: no usable Data/Battle/arena.json; the lead fights with the built-in kit.\n%s"),
				*FT::ToFString(Error));
			WeaponTable = ftcore::ArenaTable();
		}
	}
}

bool AFTExploreCharacter::IsPlayableLead(const FFTLeadLook& Look)
{
	LoadWeaponTable();
	return ftcore::arenaPlayable(WeaponTable.fighter(FT::ToUtf8(Look.Name.ToLower())));
}

void AFTExploreCharacter::BeginWeapon(const FFTLeadLook& Look)
{
	LoadWeaponTable();
	const ftcore::ArenaFighter& Kit = WeaponTable.fighter(FT::ToUtf8(Look.Name.ToLower()));
	// The body and its anim instance may be new: the props, effects and clips go on again.
	Weapons.End();
	Weapons.Begin(this, GetMesh(), Look, Kit);
	Strike.begin(Kit);
	AimKindNow = ftcore::aimKindOf(Kit);
	AttackHold.reset();
	bPressLight = bPressHeavy = false;
	bWeaponBodyBusy = false;
	ChestBone = INDEX_NONE;
	if (const USkeletalMeshComponent* Body = GetMesh())
	{
		for (const TCHAR* Bone : {TEXT("spine_05"), TEXT("spine_04"), TEXT("spine_03")})
		{
			ChestBone = Body->GetBoneIndex(FName(Bone));
			if (ChestBone != INDEX_NONE)
			{
				break;
			}
		}
	}
	// -ExploreDummy's post stands where the first light attack reaches it (80% of its range, 1.2 to 3 m).
	const ftcore::ArenaAttack* First = ftcore::arenaPlayerAttack(Kit, 0);
	DummyDistance = FMath::Clamp((First ? First->range : 3.f) * 80.f, 120.f, 300.f);
	UE_LOG(LogForgottenThird, Display, TEXT("Field: weapon for %s (%d light attacks, heavy %.2f s), %s."), *Look.Name, Kit.lightCount,
		Kit.heavy.total(), Weapons.IsOn() ? TEXT("props and clips on") : TEXT("no body to wear them"));
}

bool AFTExploreCharacter::TakesInput()
{
	const APlayerController* PC = Cast<APlayerController>(Controller);
	return PC && !Van.IsValid() && !PC->bShowMouseCursor && !UFTPauseSubsystem::IsBlocking(this) &&
		!UFTPauseSubsystem::IsJustResumed(this) && !ScenePlaying();
}

void AFTExploreCharacter::TickHeldKeys(float DeltaSeconds)
{
	using ftcore::InputAction;
	const APlayerController* PC = Cast<APlayerController>(Controller);
	if (!TakesInput())
	{
		// Taken away (the van, a menu, a pause, a scene): the latches let go. Getting into the van stops the run
		// itself (ResetHeldInput); the van's own speed is not ours to touch.
		RunInput.reset();
		bCrouchKeyWas = true;
		MapHeld = 0.f;
		bMapHoldUsed = true;
		if (bRunOn && !Van.IsValid())
		{
			bRunOn = false;
			HandleRunStop();
		}
		return;
	}
	// Run: its key held or toggled, or on a pad the left stick pushed all the way.
	const float StickX = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
	const float StickY = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY);
	const bool bStickFull = StickX * StickX + StickY * StickY >= 0.95f * 0.95f;
	const bool bRunKey = RunInput.tick(FTInputBindings::IsDown(*PC, InputAction::Run), FTInputBindings::Toggles(GetWorld(), InputAction::Run));
	const bool bRun = bRunKey || bStickFull;
	if (bRun != bRunOn)
	{
		bRunOn = bRun;
		if (bRun)
		{
			HandleRunStart();
		}
		else
		{
			HandleRunStop();
		}
	}
	// Crouch: the press crouches (or slides in a sprint, or lets go of a ledge); held, letting go stands up; toggled,
	// the next press does.
	const bool bCrouchKey = FTInputBindings::IsDown(*PC, InputAction::Crouch);
	if (bCrouchKey && !bCrouchKeyWas)
	{
		HandleCrouch();
	}
	else if (!bCrouchKey && bCrouchKeyWas && !FTInputBindings::Toggles(GetWorld(), InputAction::Crouch)
		&& GetCharacterMovement()->bWantsToCrouch)
	{
		UnCrouch();
	}
	bCrouchKeyWas = bCrouchKey;
	// The map button held shows or hides the party (the pad has no button of its own for it); a short press, on
	// letting go, is the map.
	if (FTInputBindings::IsDown(*PC, InputAction::Map))
	{
		MapHeld += DeltaSeconds;
		if (!bMapHoldUsed && MapHeld >= ftcore::kInputMapHoldForParty)
		{
			bMapHoldUsed = true;
			HandleToggleParty();
		}
	}
	else
	{
		if (MapHeld > 0.f && !bMapHoldUsed)
		{
			HandleToggleMap();
		}
		MapHeld = 0.f;
		bMapHoldUsed = false;
	}
}

void AFTExploreCharacter::SpawnFieldDummy()
{
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Forward = GetActorForwardVector().GetSafeNormal2D();
	// The post is 180 cm tall, centred on its root: its base on the lead's floor.
	float Distance = DummyDistance;
	FParse::Value(FCommandLine::Get(), TEXT("ExploreDummyAt="), Distance);
	const FVector At = GetActorLocation() + Forward * double(Distance) + FVector(0.0, 0.0, 90.0 - HalfHeight);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const AFTFieldDummy* Dummy = GetWorld()->SpawnActor<AFTFieldDummy>(At, FRotator::ZeroRotator, Params);
	// A forced aim (-ExploreAim) looks at the post, as a player holding aim would: the camera's line on it for every
	// lead, the body turning to that line.
	if (APlayerController* PC = Cast<APlayerController>(Controller); PC && Dummy && AimFrom >= 0.f)
	{
		PC->SetControlRotation(FRotator(-5.0, Forward.Rotation().Yaw, 0.0));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Field: dummy %s at %s."), Dummy ? TEXT("stood") : TEXT("not spawned"), *At.ToCompactString());
}

void AFTExploreCharacter::TickWeapon(float DeltaSeconds)
{
	if (bDummyPending && GetWorld()->GetTimeSeconds() > 1.0 && GetCharacterMovement()->IsMovingOnGround())
	{
		bDummyPending = false;
		SpawnFieldDummy();
	}
	if (AttackEvery > 0.f && (AttackTimer += DeltaSeconds) >= AttackEvery)
	{
		AttackTimer = 0.f;
		// Every fourth press a heavy.
		(++AttackPresses % 4 == 0 ? bPressHeavy : bPressLight) = true;
	}
	// The body's own moves come first: while it vaults, climbs, hangs, rolls or slides, and through the hand-back after
	// (while its pose fades back into locomotion), presses are dropped and an attack under way is cut (its clips, arm aim
	// and effects with it); nothing turns or paints the body for the weapon. Falling alone allows attacks.
	ftcore::FieldBody Body = ftcore::FieldBody::Free;
	if (Van.IsValid())
	{
		Body = ftcore::FieldBody::Seated; // boarding, seated or getting out: no weapon
	}
	else if (LedgeNow.phase != ftcore::LedgePhase::None)
	{
		Body = ftcore::FieldBody::Hanging;
	}
	else if (bTraversing)
	{
		Body = ftcore::FieldBody::Traversing;
	}
	else if (SlideNow.sliding())
	{
		Body = ftcore::FieldBody::Sliding;
	}
	else if (DodgeNow.active())
	{
		Body = ftcore::FieldBody::Rolling;
	}
	else if (GetCharacterMovement()->IsFalling())
	{
		Body = ftcore::FieldBody::Falling;
	}
	Strike.setBody(Body);
	TickAim(DeltaSeconds, Body);
	const bool bBusy = !Strike.canAttack();
	if (bBusy && !bWeaponBodyBusy)
	{
		Weapons.Restart();
	}
	bWeaponBodyBusy = bBusy;
	if (bBusy)
	{
		bPressLight = bPressHeavy = false;
		AttackTimer = 0.f;
	}
	// The dummies in play, in metres on the floor plane, their chests above the lead's feet.
	const FVector Pos = GetActorLocation();
	const double FeetZ = Pos.Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	ftcore::FieldTarget Targets[ftcore::kMaxFieldTargets];
	AFTFieldDummy* Dummies[ftcore::kMaxFieldTargets] = {};
	std::size_t Count = 0;
	const UWorld* World = GetWorld();
	for (const TWeakObjectPtr<AFTFieldDummy>& Weak : AFTFieldDummy::All())
	{
		AFTFieldDummy* Dummy = Weak.Get();
		if (!Dummy || Dummy->GetWorld() != World || Count >= ftcore::kMaxFieldTargets)
		{
			continue;
		}
		const FVector At = Dummy->GetActorLocation();
		Targets[Count] = ftcore::FieldTarget{ftcore::Vec2{float(At.X / 100.0), float(At.Y / 100.0)}, Dummy->Radius / 100.f, true,
			float((Dummy->GetChest().Z - FeetZ) / 100.0), Dummy->GetUniqueID()};
		Dummies[Count++] = Dummy;
	}
	// With nobody in reach the lead aims at its own chest height, read as a press comes in.
	if ((bPressLight || bPressHeavy) && ChestBone != INDEX_NONE)
	{
		Strike.setChestHeight(float((GetMesh()->GetBoneTransform(ChestBone).GetLocation().Z - FeetZ) / 100.0));
	}
	const FVector Face = GetActorForwardVector().GetSafeNormal2D();
	// On the move the attack turns with the body: toward the way the stick pushes, else (aiming) the camera's.
	{
		FVector Want = GetLastMovementInputVector().GetSafeNormal2D();
		if (Want.IsNearlyZero() && Aim.aiming() && Controller)
		{
			Want = FRotator(0.0, Controller->GetControlRotation().Yaw, 0.0).Vector();
		}
		Strike.setSteer(ftcore::Vec2{float(Want.X), float(Want.Y)}, AttackMoveNow().turnRate);
	}
	const ftcore::ArenaOutput& Out = Strike.tick(DeltaSeconds, bPressLight, bPressHeavy,
		ftcore::Vec2{float(Pos.X / 100.0), float(Pos.Y / 100.0)}, ftcore::Vec2{float(Face.X), float(Face.Y)}, Targets, Count);
	bPressLight = bPressHeavy = false;
	for (const ftcore::ArenaEvent& Event : Out.events)
	{
		if (Event.type == ftcore::ArenaEventType::Hit && Event.target < Count && Dummies[Event.target])
		{
			Dummies[Event.target]->TakeHit(Event.amount);
		}
	}
	if (bBusy)
	{
		TickWeaponShots();
		return;
	}
	// While it attacks, the lead turns to Core's facing (to the target it aims at) and aims at the hit point.
	if (Strike.attacking())
	{
		const float Want = FMath::RadiansToDegrees(FMath::Atan2(Out.player.facing.y, Out.player.facing.x));
		const float Gap = FMath::FindDeltaAngleDegrees(static_cast<float>(GetActorRotation().Yaw), Want);
		AddActorWorldRotation(FRotator(0.0, Gap * (1.f - FMath::Exp(-DeltaSeconds / kAttackTurnTime)), 0.0));
	}
	// An aimed gauntlet strike's lunge toward the reticle (m/s), swept so it stops at a wall.
	if (Out.player.velocity.x != 0.f || Out.player.velocity.y != 0.f)
	{
		AddActorWorldOffset(FVector(Out.player.velocity.x * 100.0 * DeltaSeconds, Out.player.velocity.y * 100.0 * DeltaSeconds, 0.0), true);
	}
	const ftcore::FieldAim& StrikeAim = Strike.aim();
	if (Aim.alpha() > 0.f && !Strike.attacking())
	{
		Weapons.SetAim(ReticlePoint, true); // the raised pistol or casting hand follows the reticle between attacks
	}
	else
	{
		Weapons.SetAim(FVector(StrikeAim.at.x * 100.0, StrikeAim.at.y * 100.0, FeetZ + StrikeAim.height * 100.0), StrikeAim.aimed);
	}
	Weapons.SetAimHold(Aim.alpha());
	// Walking, the strike plays from the spine up and the legs keep the walk; standing, on the whole body.
	Weapons.SetActionUpper(ftcore::attackUpperShare(AttackMoveTable, static_cast<float>(GetVelocity().Size2D() / 100.0)));
	const double T = Strike.time();
	Weapons.Feed(Out.events, T);
	Weapons.Track(Out.player, T);
	Weapons.Paint(T);
	TickWeaponShots();
}

void AFTExploreCharacter::TickAim(float DeltaSeconds, ftcore::FieldBody Body)
{
	if (!bAimRulesTried)
	{
		bAimRulesTried = true;
		ftcore::AimRules Rules;
		TArray<uint8> Bytes;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/aim.json");
		std::string Error;
		if (!(IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File)
			&& ftcore::importAimRules(FT::BytesAsText(Bytes), Rules, &Error)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Field: no usable Data/Battle/aim.json; the aim uses its built-in rules.\n%s"), *FT::ToFString(Error));
			Rules = ftcore::AimRules();
		}
		Aim.setRules(Rules);
		ftcore::AttackMoveRules Moves;
		const FString MoveFile = FPaths::ProjectDir() / TEXT("Data/Battle/attack_move.json");
		Bytes.Reset();
		if (!(FFileHelper::LoadFileToArray(Bytes, *MoveFile) && ftcore::importAttackMoveRules(FT::BytesAsText(Bytes), Moves, &Error)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Field: no usable Data/Battle/attack_move.json; attacks use built-in move rules: %s"), *FT::ToFString(Error));
			Moves = ftcore::AttackMoveRules();
		}
		AttackMoveTable = Moves;
	}
	const ftcore::AimRules& Rules = Aim.rules();
	// The mouse buttons count only on foot, with the game taking input: not in the van, a menu, a pause or a scene.
	// Aim and Light from the binding table (right and left mouse, LT and RT by default): Aim held or toggled, Light a
	// tap for the light and held for the heavy.
	const APlayerController* PC = Cast<APlayerController>(Controller);
	const bool bInput = TakesInput();
	const bool bForced = AimFrom >= 0.f && GetWorld()->GetTimeSeconds() >= AimFrom && !Van.IsValid() && !ScenePlaying();
	// No input (paused or just resumed, a scene): the press states clear before they tick, so a held key let go
	// meanwhile reads as no press rather than a light (PressHold::tickIf). Out of focus only the held attack clears;
	// the toggled latches (run, aim, crouch) stay.
	if (!bInput)
	{
		AimInput.reset();
	}
	const bool bHeld = (bInput && AimInput.tick(FTInputBindings::IsDown(*PC, ftcore::InputAction::Aim),
		FTInputBindings::Toggles(GetWorld(), ftcore::InputAction::Aim))) || bForced;
	// A gun kit aiming fires one round per press, at the press, and never a heavy.
	const ftcore::PressHold::Out Press = AttackHold.tickIf(bInput && !FTInputBindings::LostFocus(), DeltaSeconds, bInput && FTInputBindings::IsDown(*PC, ftcore::InputAction::Light),
		Rules.heavyHold, Strike.attacking(), bHeld && Strike.kit().weapon == ftcore::ArenaWeapon::Guns);
	bPressLight |= Press.light;
	bPressHeavy |= Press.heavy;
	Aim.tick(DeltaSeconds, bHeld, Body);
	ApplyAimCamera();
	// The walk speed: whatever the movement handlers last set, scaled while aiming.
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (Move->MaxWalkSpeed != AimSpeedSet)
	{
		AimSpeedBase = Move->MaxWalkSpeed;
	}
	if (!Van.IsValid())
	{
		// An attack under way slows the walk too; a fist's or spell's heavy stops it through its active part (speed 0:
		// the movement brakes the body to a stop at its own braking rate).
		Move->MaxWalkSpeed = AimSpeedBase * Aim.speedScale() * AttackMoveNow().speedScale;
		AimSpeedSet = Move->MaxWalkSpeed;
	}
	// The reticle: the camera's line to the first thing it meets within reticleRange, the lead itself not counted.
	const bool bAiming = Aim.aiming();
	if (Aim.alpha() > 0.f && Camera)
	{
		const FVector From = Camera->GetComponentLocation();
		const FVector To = From + Camera->GetForwardVector() * (double(Rules.reticleRange) * 100.0);
		const FCollisionQueryParams Params(SCENE_QUERY_STAT(FTAimReticle), false, this);
		FHitResult Hit;
		ReticlePoint = GetWorld()->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params) ? FVector(Hit.ImpactPoint) : To;
	}
	const double FeetZ = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	Strike.setReticle(bAiming, ftcore::Vec2{float(ReticlePoint.X / 100.0), float(ReticlePoint.Y / 100.0)},
		float((ReticlePoint.Z - FeetZ) / 100.0), AimKindNow == ftcore::AimKind::Focus ? Rules.lungeScale : 0.f, Rules.lungeStop);
	// The body turns to the camera's aim (an attack under way turns itself, to the reticle).
	if (bAiming && Controller && !Strike.attacking() && DeltaSeconds > 0.f)
	{
		const float Want = static_cast<float>(Controller->GetControlRotation().Yaw);
		const float Gap = FMath::DegreesToRadians(FMath::FindDeltaAngleDegrees(static_cast<float>(GetActorRotation().Yaw), Want));
		AddActorWorldRotation(FRotator(0.0, FMath::RadiansToDegrees(ftcore::aimTurnStep(Rules, Gap, DeltaSeconds)), 0.0));
	}
}

float AFTExploreCharacter::BaseWalkSpeed() const
{
	// The walk speed before the aim's and the attack's scaling (TickAim): what the ledge catch, the slide and the gait read.
	const float Max = GetCharacterMovement()->MaxWalkSpeed;
	return Max == AimSpeedSet ? AimSpeedBase : Max;
}

bool AFTExploreCharacter::ScenePlaying()
{
	// An in-place cutscene or a scene holding the party: the mouse's attack and aim are off too.
	const AFTExploreDirector* D = GetDirector();
	if (!D)
	{
		return false;
	}
	const AFTCutsceneDirector* Cut = D->GetCutsceneDirector();
	return (Cut && Cut->IsPlayingInPlace()) || D->IsPartyHeld();
}

ftcore::AttackMoveOut AFTExploreCharacter::AttackMoveNow() const
{
	const ftcore::ArenaAttack* Attack = Strike.attack();
	// By the move's own kind: a gun kit's shot fires, its kicks take the focus numbers.
	return Attack ? ftcore::attackMove(AttackMoveTable, ftcore::aimKindOfMove(Strike.kit(), Strike.moveNow()), Strike.heavyNow(),
		*Attack, Strike.actionTime()) : ftcore::AttackMoveOut();
}

void AFTExploreCharacter::TickUnstick(float DeltaSeconds)
{
	if (!bUnstickTried)
	{
		bUnstickTried = true;
		ftcore::UnstickRules Rules;
		TArray<uint8> Bytes;
		std::string Error;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Movement/unstick.json");
		if (!(FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importUnstickRules(FT::BytesAsText(Bytes), Rules, &Error)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Field: no usable Data/Movement/unstick.json; the unstick uses built-in rules: %s"), *FT::ToFString(Error));
			Rules = ftcore::UnstickRules();
		}
		Unstick.setRules(Rules);
	}
	UCharacterMovementComponent* Move = GetCharacterMovement();
	// Only on foot, walking or in the air: the van, a traversal, a hang or a swim carry the body their own way.
	if (DeltaSeconds <= 0.f)
	{
		return; // a paused frame: nothing moved
	}
	if (Van.IsValid() || bTraversing || !(Move->IsMovingOnGround() || Move->IsFalling()))
	{
		Unstick.reset();
		return;
	}
	FFindFloorResult Floor = Move->CurrentFloor;
	if (Move->IsFalling())
	{
		Move->FindFloor(GetActorLocation(), Floor, false);
	}
	// The movement's own walkable test wins (a stair's edge can show a steep normal on walkable ground).
	const bool bOnGround = Floor.bBlockingHit && Floor.FloorDist < 10.f;
	const FVector N = bOnGround ? FVector(Floor.HitResult.ImpactNormal) : FVector::UpVector;
	const double HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector At = GetActorLocation();
	const bool bRescue = Unstick.tick(DeltaSeconds, ftcore::Vec2{float(At.X / 100.0), float(At.Y / 100.0)}, float((At.Z - HalfHeight) / 100.0),
		bOnGround, ftcore::Vec2{float(N.X), float(N.Y)}, float(N.Z), Move->IsFalling(), bOnGround ? (Floor.bWalkableFloor ? 1 : 0) : -1);
	if (bRescue)
	{
		const ftcore::UnstickPoint& To = Unstick.rescueTo();
		SetActorLocation(FVector(To.at.x * 100.0, To.at.y * 100.0, To.z * 100.0 + HalfHeight + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		Move->StopMovementImmediately();
		if (WedgeDropAt >= 0.0 && WedgeRescueAt < 0.0)
		{
			WedgeRescueAt = GetWorld()->GetTimeSeconds();
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Field: the lead was stuck off walkable ground; stepped back to (%.0f, %.0f, %.0f)."),
			To.at.x * 100.0, To.at.y * 100.0, To.z * 100.0);
		if (APlayerController* PC = Cast<APlayerController>(Controller); PC && PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->StartCameraFade(1.f, 0.f, Unstick.rules().fadeTime, FLinearColor::Black, false, false);
		}
		return;
	}
	// Too steep to stand on: down the slope at slideSpeed at least (jump stays open, HandleJump).
	if (Unstick.sliding() && Move->IsFalling())
	{
		const ftcore::Vec2 V = Unstick.slideVelocity(ftcore::Vec2{float(Move->Velocity.X / 100.0), float(Move->Velocity.Y / 100.0)}, DeltaSeconds);
		Move->Velocity.X = V.x * 100.0;
		Move->Velocity.Y = V.y * 100.0;
	}
}

void AFTExploreCharacter::ApplyAimCamera()
{
	// The look and side cameras of the capture flags own the boom; the aim leaves them be.
	if (!Boom || !Camera || !Boom->bUsePawnControlRotation || bSideAlways || bSideView)
	{
		return;
	}
	const float A = Aim.alpha();
	if (A <= 0.f && !bAimCamera)
	{
		return;
	}
	bAimCamera = A > 0.f;
	const ftcore::AimRules& Rules = Aim.rules();
	Boom->TargetArmLength = Aim.blend(kArm, kArm * Rules.armScale);
	Boom->SocketOffset = FMath::Lerp(kShoulder, FVector(0.0, Rules.shoulderRight, Rules.shoulderUp), double(A));
	Camera->SetFieldOfView(Aim.blend(kFov, Rules.fov));
}

void AFTExploreCharacter::TickWeaponShots()
{
	if (WeaponShotPrefix.IsEmpty())
	{
		return;
	}
	const int32 Active = Weapons.GetActiveCount();
	const bool bNew = Active > WeaponShotSeen;
	WeaponShotSeen = Active;
	if (WeaponShotsLeft <= 0)
	{
		if ((WeaponQuitIn -= GetWorld()->GetDeltaSeconds()) <= 0.f)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Field: weapon shots taken; quitting."));
			WeaponShotPrefix.Reset();
			FPlatformMisc::RequestExit(false, TEXT("ExploreWeaponShot"));
		}
		return;
	}
	const float Speed = static_cast<float>(GetVelocity().Size2D() / 100.0);
	if (StripShots > 0)
	{
		// The strip: the first light attack begun while walking, filmed from its start every StripStep seconds.
		if (StripAt < 0.0 && bNew && GetWorld()->GetTimeSeconds() >= WeaponShotDelay && !Strike.heavyNow() && Speed > 0.5f)
		{
			StripAt = Strike.time();
			WeaponShotDue = StripAt;
		}
		if (StripAt >= 0.0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Field: strip frame t=%.3f speed=%.2f m/s upper=%.2f attacking=%d"),
				Strike.time() - StripAt, Speed, ftcore::attackUpperShare(AttackMoveTable, Speed), Strike.attacking() ? 1 : 0);
		}
	}
	// Each new attack is filmed at its impact, or (every second one) a shot's or cast's mid-flight.
	else if (bNew && GetWorld()->GetTimeSeconds() >= WeaponShotDelay)
	{
		WeaponShotDue = Weapons.GetViewTime(WeaponShotIndex % 2 == 0);
	}
	if (WeaponShotDue < 0.0 || Strike.time() + 1e-6 < WeaponShotDue)
	{
		return;
	}
	WeaponShotDue = StripShots > 0 ? WeaponShotDue + StripStep : -1.0;
	const APlayerController* Player = Cast<APlayerController>(GetController());
	if (AFTExploreHUD* Hud = Player ? Cast<AFTExploreHUD>(Player->GetHUD()) : nullptr)
	{
		Hud->bReticleOnly = true;
	}
	++WeaponShotIndex;
	const FString File = FString::Printf(TEXT("%s_%d_%s.png"), *WeaponShotPrefix, WeaponShotIndex, Weapons.GetLastStrikeName());
	// The viewport's own frame with the UI drawn (HighResShot leaves the HUD out, and with it the aim reticle); the
	// size is the window's (-ResX/-ResY).
	FScreenshotRequest::RequestScreenshot(File, true, false);
	UE_LOG(LogForgottenThird, Display, TEXT("Field: weapon shot %s at %.2f s, speed %.2f m/s, upper %.2f."), *File, Strike.time(), Speed,
		ftcore::attackUpperShare(AttackMoveTable, Speed));
	if (--WeaponShotsLeft <= 0)
	{
		WeaponQuitIn = 2.f; // the last shot is written before the exit
	}
}

void AFTExploreCharacter::EndPlay(const EEndPlayReason::Type Reason)
{
	Weapons.Reset();
	Super::EndPlay(Reason);
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
	// Every key from the binding table (FTInputBindings); the left stick and the look axes are analog and fixed.
	for (const ftcore::InputDevice Device : {ftcore::InputDevice::Keys, ftcore::InputDevice::Pad})
	{
		const auto Move = [&MapMove, Device](ftcore::InputAction Bound, bool bSwizzle, bool bNegate)
		{
			const FKey& Key = FTInputBindings::Key(Bound, Device);
			if (Key.IsValid())
			{
				MapMove(Key, bSwizzle, bNegate);
			}
		};
		Move(ftcore::InputAction::MoveForward, true, false);
		Move(ftcore::InputAction::MoveBack, true, true);
		Move(ftcore::InputAction::MoveRight, false, false);
		Move(ftcore::InputAction::MoveLeft, false, true);
	}
	MapMove(EKeys::Gamepad_Left2D, false, false);
	const auto Both = [this](UInputAction* Action, ftcore::InputAction Bound)
	{
		for (const ftcore::InputDevice Device : {ftcore::InputDevice::Keys, ftcore::InputDevice::Pad})
		{
			const FKey& Key = FTInputBindings::Key(Bound, Device);
			if (Key.IsValid())
			{
				Context->MapKey(Action, Key);
			}
		}
	};

	LookAction = MakeAction(TEXT("IA_ExploreLook"), true);
	Context->MapKey(LookAction, EKeys::Mouse2D);
	Context->MapKey(LookAction, EKeys::Gamepad_Right2D);

	JumpAction = MakeAction(TEXT("IA_ExploreJump"), false);
	Both(JumpAction, ftcore::InputAction::Jump);

	RollAction = MakeAction(TEXT("IA_ExploreRoll"), false);
	Both(RollAction, ftcore::InputAction::Roll);

	InteractAction = MakeAction(TEXT("IA_ExploreInteract"), false);
	Both(InteractAction, ftcore::InputAction::Interact);

	SwitchLeadAction = MakeAction(TEXT("IA_ExploreSwitchLead"), false);
	Both(SwitchLeadAction, ftcore::InputAction::SwitchLead);

	PartyAction = MakeAction(TEXT("IA_ExploreParty"), false);
	Both(PartyAction, ftcore::InputAction::Party);
	// Run, Crouch, Aim, Light and the map button's party hold are polled (TickHeldKeys, TickAim): held or toggled.
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
	Input->BindAction(InteractAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleInteract);
	Input->BindAction(InteractAction, ETriggerEvent::Completed, this, &AFTExploreCharacter::HandleInteractRelease);
	Input->BindAction(SwitchLeadAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleSwitchLead);
	Input->BindAction(PartyAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleToggleParty);
}

void AFTExploreCharacter::HandleMove(const FInputActionValue& Value)
{
	if (AFTPartyVan* V = Van.Get())
	{
		V->SetDriveAxis(Value.Get<FVector2D>());
		return;
	}
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
	if (AFTPartyVan* V = Van.Get())
	{
		V->AddLook(Value.Get<FVector2D>());
		return;
	}
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
	if (Van.IsValid())
	{
		return;
	}
	// Sprinting stands the body up.
	UnCrouch();
	GetCharacterMovement()->MaxWalkSpeed = RunSpeed;
}

void AFTExploreCharacter::HandleJump()
{
	if (AFTPartyVan* V = Van.Get())
	{
		V->SetHandbrake(true);
		return;
	}
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
	if (!bSwimming && TryTraverse(true))
	{
		return;
	}
	// In the water, at a ladder: up it. At any other wall: out onto it (the fallback). A swimmer doesn't jump otherwise.
	if (TryLadderClimb(true) || TryWaterClimb() || bSwimming)
	{
		return;
	}
	// Sliding down a too-steep bank the body is in the air to the movement, where Jump won't go: kick off it.
	if (Unstick.sliding() && GetCharacterMovement()->IsFalling())
	{
		const FVector Off(Unstick.slideDir().x, Unstick.slideDir().y, 0.0);
		LaunchCharacter(Off * 200.0 + FVector(0.0, 0.0, GetCharacterMovement()->JumpZVelocity), true, true);
		return;
	}
	Jump();
}

void AFTExploreCharacter::HandleJumpRelease()
{
	if (AFTPartyVan* V = Van.Get())
	{
		V->SetHandbrake(false);
		return;
	}
	bJumpHeld = false;
	StopJumping();
}

void AFTExploreCharacter::HandleRoll()
{
	// Not in a van, nor swimming or climbing out of the water.
	if (Van.IsValid() || bSwimming || bWaterClimbing)
	{
		return;
	}
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
	const float Speed = BaseWalkSpeed() * FMath::Min(static_cast<float>(Stick.Size2D()) / FMath::Max(Move->GetMaxAcceleration(), 1.f), 1.f);
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
	if (bTraversing || !Anim || !Move->IsMovingOnGround() || bIsCrouched || BaseWalkSpeed() < RunSpeed)
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
	In.wantSpeed = bStick ? BaseWalkSpeed() * Move->GetAnalogInputModifier() : 0.f;
	In.turn = bStick ? FMath::FindDeltaAngleDegrees(SlideHeading, static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Stick.Y, Stick.X)))) : 0.f;
	In.sprint = BaseWalkSpeed() >= RunSpeed;
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

void AFTExploreCharacter::SpawnWedge()
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UWorld* World = GetWorld();
	if (!Cube || !World)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Wedge: no cube mesh; nothing built."));
		return;
	}
	const FVector Ahead = GetActorForwardVector().GetSafeNormal2D();
	const FVector Across = FVector::CrossProduct(FVector::UpVector, Ahead);
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	const FVector Bottom = Feet + Ahead * 400.0;
	// Two slabs 4 m up the slope, 3 m along the trench, meeting at the bottom at 60 degrees from level.
	const double Slope = FMath::DegreesToRadians(60.0);
	for (const double Side : {1.0, -1.0})
	{
		const FVector Up = (Across * Side * FMath::Cos(Slope) + FVector::UpVector * FMath::Sin(Slope)).GetSafeNormal();
		const FVector Centre = Bottom + Up * 200.0;
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Slab = World->SpawnActor<AStaticMeshActor>(Centre, FRotationMatrix::MakeFromXY(Ahead, Up).Rotator(), Params);
		if (!Slab)
		{
			continue;
		}
		Slab->SetMobility(EComponentMobility::Movable);
		Slab->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Slab->SetActorScale3D(FVector(3.0, 4.0, 0.2));
	}
	SetActorLocation(Bottom + FVector(0.0, 0.0, 250.0), false, nullptr, ETeleportType::TeleportPhysics);
	GetCharacterMovement()->StopMovementImmediately();
	WedgeDropAt = World->GetTimeSeconds();
	UE_LOG(LogForgottenThird, Display, TEXT("Wedge: a 60-degree V trench 4 m ahead; the lead dropped in at %.2f s."), WedgeDropAt);
}

void AFTExploreCharacter::TickWedge()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	if (bWedgePending && Now > 3.0 && GetCharacterMovement()->IsMovingOnGround())
	{
		bWedgePending = false;
		SpawnWedge();
		return;
	}
	if (WedgeShotPrefix.IsEmpty() || WedgeDropAt < 0.0)
	{
		return;
	}
	const auto Shot = [&](const TCHAR* Tag)
	{
		const APlayerController* Player = Cast<APlayerController>(GetController());
		if (AHUD* Hud = Player ? Player->GetHUD() : nullptr)
		{
			Hud->bShowHUD = false;
		}
		const FString File = FString::Printf(TEXT("%s_%s.png"), *WedgeShotPrefix, Tag);
		if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
		{
			Viewport->Exec(World, *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *File), *GLog);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Wedge: shot %s at %.2f s."), *File, Now);
		++WedgeShotsTaken;
	};
	if (WedgeShotsTaken == 0 && Now > WedgeDropAt + 1.0)
	{
		Shot(TEXT("in"));
	}
	else if (WedgeShotsTaken == 1 && WedgeRescueAt >= 0.0 && Now > WedgeRescueAt + 1.0)
	{
		Shot(TEXT("out"));
	}
	else if ((WedgeShotsTaken == 2 && Now > WedgeRescueAt + 3.0) || Now > WedgeDropAt + 15.0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Wedge: done (rescued %d); quitting."), WedgeRescueAt >= 0.0 ? 1 : 0);
		WedgeShotPrefix.Reset();
		FPlatformMisc::RequestExit(false, TEXT("ExploreWedge"));
	}
}

void AFTExploreCharacter::HandleCrouch()
{
	// Not in a van, nor swimming or climbing out of the water.
	if (Van.IsValid() || bSwimming || bWaterClimbing)
	{
		return;
	}
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
	if (Van.IsValid())
	{
		return;
	}
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}

void AFTExploreCharacter::ResetHeldInput()
{
	HandleRunStop();
	bRunOn = false;
	RunInput.reset();
	AimInput.reset();
	RollAsked = 0.f;
	bRollAskedAir = false;
	SlideJumpAsked = 0.f;
	LedgeJumpAsked = LedgeDropAsked = 0.f;
	bJumpHeld = false;
	bPressLight = bPressHeavy = false; // the attack buffer too
	AttackTimer = 0.f;
	UnCrouch();
}

void AFTExploreCharacter::HandleInteract()
{
	// In the van, interact held is the way out; on foot near the party van, it is the way in.
	if (AFTPartyVan* V = Van.Get())
	{
		V->SetExitHeld(true);
		return;
	}
	for (TActorIterator<AFTPartyVan> It(GetWorld()); It; ++It)
	{
		if (It->TryEnter(this))
		{
			return;
		}
	}
	if (AFTExploreDirector* D = GetDirector())
	{
		D->Interact();
	}
}

void AFTExploreCharacter::HandleSwitchLead()
{
	if (Van.IsValid())
	{
		return;
	}
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
	// The next look the player may play; a party slot without attacks is passed over.
	int32 Next = LeadIndex;
	for (int32 Step = 1; Step < PartyLooks.Num(); ++Step)
	{
		const int32 Try = (LeadIndex + Step) % PartyLooks.Num();
		if (IsPlayableLead(PartyLooks[Try]))
		{
			Next = Try;
			break;
		}
	}
	if (Next == LeadIndex)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: no other playable lead in PartyLooks."));
		return;
	}
	UnCrouch();
	LeadIndex = Next;
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

void AFTExploreCharacter::HandleToggleMap()
{
	const APlayerController* Player = Cast<APlayerController>(GetController());
	if (AFTExploreHUD* Hud = Player ? Cast<AFTExploreHUD>(Player->GetHUD()) : nullptr)
	{
		Hud->ToggleMapOverlay();
	}
}

void AFTExploreCharacter::HandleToggleParty()
{
	if (Van.IsValid())
	{
		return;
	}
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

void AFTExploreCharacter::HandleInteractRelease()
{
	if (AFTPartyVan* V = Van.Get())
	{
		V->SetExitHeld(false);
	}
}

bool AFTExploreCharacter::CanBoardVan() const
{
	// Not from a ledge (hanging, climbing or dropping), a vault, a slide or the air.
	return !Van.IsValid() && !bTraversing && LedgeNow.phase == ftcore::LedgePhase::None && !SlideNow.sliding()
		&& GetCharacterMovement()->IsMovingOnGround();
}

void AFTExploreCharacter::BoardVan(AFTPartyVan* InVan)
{
	ResetHeldInput(); // before Van is set, or the run and jump resets are routed to the van and lost
	Aim.reset();
	AttackHold.reset();
	ApplyAimCamera();
	Van = InVan;
	GetCharacterMovement()->StopMovementImmediately();
	GetCharacterMovement()->DisableMovement();
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	AttachToActor(InVan, FAttachmentTransformRules::KeepWorldTransform);
	bPartyHiddenBeforeVan = bPartyHidden;
	if (AFTExploreDirector* D = GetDirector())
	{
		D->SetPartyHidden(true);
	}
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		PC->SetViewTargetWithBlend(InVan, 0.6f);
	}
}

void AFTExploreCharacter::LeaveVan(const FVector& At, double Yaw)
{
	Van.Reset();
	ResetHeldInput(); // out at a walk, whatever was held inside
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	SetActorLocationAndRotation(At, FRotator(0.0, Yaw, 0.0), false, nullptr, ETeleportType::TeleportPhysics);
	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	if (AFTExploreDirector* D = GetDirector())
	{
		D->SetPartyHidden(bPartyHiddenBeforeVan);
	}
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		PC->SetControlRotation(FRotator(-10.0, Yaw, 0.0));
		PC->SetViewTargetWithBlend(this, 0.6f);
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
	TickHeldKeys(DeltaSeconds);
	// A frame after a traversal or slide hands back: how far CharacterMovement moved the capsule (a pop if not ~0).
	if (bHandBackPending)
	{
		bHandBackPending = false;
		const FFindFloorResult& Floor = GetCharacterMovement()->CurrentFloor;
		UE_LOG(LogForgottenThird, Display, TEXT("Hand-back: on CharacterMovement's first frame the mesh moved %+.2f cm, the capsule %+.2f (crouched %d; floor dist %.2f cm, walkable %d)."),
			GetMesh()->GetComponentLocation().Z - HandBackMeshZ, GetActorLocation().Z - HandBackZ, bIsCrouched ? 1 : 0, Floor.FloorDist, Floor.IsWalkableFloor() ? 1 : 0);
	}
	if (LookDevPivot && LookDevSpin != 0.f)
	{
		LookDevPivot->AddWorldRotation(FRotator(0.0, LookDevSpin * DeltaSeconds, 0.0));
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
	// An attack's turn is TickWeapon's.
	// While aiming, TickAim turns the body to the camera's aim.
	const UFTLocomotionAnimInstance* Steered = bTraversing || Strike.attacking() || Aim.aiming() ? nullptr : LocoAnim;
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
	TickWater(DeltaSeconds);
	TickWetPrints(DeltaSeconds);
	TickWeapon(DeltaSeconds);
	TickUnstick(DeltaSeconds);
	TickWedge();
}

void AFTExploreCharacter::BeginLeadGo(const FVector& To, bool bJumpWhenHeld)
{
	bLeadGo = true;
	bLeadGoJump = bJumpWhenHeld;
	LeadGoTo = To;
	LeadGoAfter = -1.f;
	LeadGoStuck = 0.f;
	LeadGoRan = 0.f;
	LeadGoNext.Reset();
	UE_LOG(LogForgottenThird, Display, TEXT("LeadGo: walking from (%.0f, %.0f, %.0f) at (%.0f, %.0f) cm%s."), GetActorLocation().X, GetActorLocation().Y,
		GetActorLocation().Z, To.X, To.Y, bJumpWhenHeld ? TEXT(", jumping when held up") : TEXT(""));
}

void AFTExploreCharacter::TickWater(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	UCharacterMovementComponent* Move = GetCharacterMovement();
	// The scripted walk.
	if (LeadGoAfter >= 0.f && (LeadGoAfter -= DeltaSeconds) < 0.f)
	{
		BeginLeadGo(LeadGoTo, bLeadGoJump);
	}
	if (bLeadGo)
	{
		LeadGoRan += DeltaSeconds;
		// A route: the next point once this one is reached.
		if (LeadGoNext.Num() > 0 && FVector::Dist2D(LeadGoTo, GetActorLocation()) <= 55.0 && !bWaterClimbing)
		{
			LeadGoTo = LeadGoNext[0];
			LeadGoNext.RemoveAt(0, EAllowShrinking::No);
			LeadGoStuck = 0.f;
			UE_LOG(LogForgottenThird, Display, TEXT("LeadGo: on to (%.0f, %.0f) cm from (%.0f, %.0f, %.0f), water %s."), LeadGoTo.X, LeadGoTo.Y, GetActorLocation().X,
				GetActorLocation().Y, GetActorLocation().Z, ANSI_TO_TCHAR(ftcore::waterModeName(WaterNow.mode)));
		}
		const FVector Way = (LeadGoTo - GetActorLocation()).GetSafeNormal2D();
		if (FVector::Dist2D(LeadGoTo, GetActorLocation()) > 30.0 && !bWaterClimbing)
		{
			AddMovementInput(Way, LeadGoPace);
			LeadGoStuck = GetVelocity().Size2D() < 15.0 ? LeadGoStuck + DeltaSeconds : 0.f;
			if (bLeadGoJump && LeadGoStuck > 1.f)
			{
				LeadGoStuck = 0.f;
				SetActorRotation(FRotator(0.0, Way.Rotation().Yaw, 0.0));
				UE_LOG(LogForgottenThird, Display, TEXT("LeadGo: held up at (%.0f, %.0f, %.0f) cm; jump."), GetActorLocation().X, GetActorLocation().Y,
					GetActorLocation().Z);
				HandleJump();
				HandleJumpRelease();
			}
		}
		if (LeadGoQuit > 0.f && LeadGoRan >= LeadGoQuit)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("LeadGo: ends at (%.0f, %.0f, %.0f) cm, feet %.0f, water %s."), GetActorLocation().X, GetActorLocation().Y,
				GetActorLocation().Z, GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), ANSI_TO_TCHAR(ftcore::waterModeName(WaterNow.mode)));
			LeadGoQuit = 0.f;
			FTRequestExit(0, TEXT("LeadGo"));
		}
	}
	// A climb out of the water: up the wall's face, then onto its top.
	if (bWaterClimbing)
	{
		WaterClimbTime += DeltaSeconds / FMath::Max(WaterClimbSeconds, 0.3f);
		const float T = FMath::Min(WaterClimbTime, 1.f);
		const float Up = FMath::SmoothStep(0.f, 0.65f, T);
		const float Over = FMath::SmoothStep(0.5f, 1.f, T);
		FVector At = FMath::Lerp(WaterClimbFrom, WaterClimbTo, Over);
		At.Z = FMath::Lerp(WaterClimbFrom.Z, WaterClimbTo.Z, Up);
		SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
		if (T >= 1.f)
		{
			bWaterClimbing = false;
			Move->SetMovementMode(MOVE_Walking);
			UE_LOG(LogForgottenThird, Display, TEXT("Water: climbed out onto (%.0f, %.0f, %.0f) cm."), At.X, At.Y, At.Z);
		}
		StirWater(DeltaSeconds);
		return;
	}
	const UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	const ftcore::WaterMode Was = WaterNow.mode;
	const bool bOut = !System || !System->HasBodies() || Van.IsValid();
	if (bOut)
	{
		WaterNow = ftcore::WaterState{};
	}
	else
	{
		const FVector At = GetActorLocation();
		const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		ftcore::WaterCapsule Body;
		Body.p = {static_cast<float>(At.X * 0.01), static_cast<float>(At.Y * 0.01)};
		Body.feetZ = static_cast<float>((At.Z - Half) * 0.01);
		Body.height = 2.f * Half * 0.01f;
		Body.swimming = bSwimming;
		if (System->Bodies().bodyAt(Body.p) >= 0)
		{
			if (Move->IsMovingOnGround())
			{
				Body.groundZ = Body.feetZ;
				Body.hasGround = true;
			}
			else
			{
				// Off the ground (falling in, swimming): the bed under it, as far down as matters.
				FHitResult Bed;
				FCollisionQueryParams Query(SCENE_QUERY_STAT(FTWaterBed), false, this);
				const bool bBed = World->LineTraceSingleByChannel(Bed, At, At - FVector(0.0, 0.0, Half + 400.0), ECC_Visibility, Query);
				Body.groundZ = bBed ? static_cast<float>(Bed.ImpactPoint.Z * 0.01) : Body.feetZ - 4.f;
				Body.hasGround = true;
			}
		}
		WaterNow = ftcore::waterState(System->Bodies(), Body, System->WaterTime());
	}
	if (WaterNow.mode != Was)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Water: the lead is %s (column %.2f m, %.0f%% under, walk x%.2f) at (%.0f, %.0f, %.0f) cm."),
			WaterNow.mode == ftcore::WaterMode::Dry ? TEXT("dry") : WaterNow.mode == ftcore::WaterMode::Wade ? TEXT("wading")
			: WaterNow.mode == ftcore::WaterMode::DeepWade ? TEXT("wading deep") : TEXT("swimming"),
			WaterNow.column, WaterNow.submerged * 100.f, WaterNow.speedFactor, GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z);
	}
	const bool bSwimNow = WaterNow.mode == ftcore::WaterMode::Swim && !bTraversing;
	if (bSwimNow && !bSwimming)
	{
		bSwimming = true;
		if (bIsCrouched)
		{
			UnCrouch();
		}
		SwimSavedFly = Move->MaxFlySpeed;
		SwimSavedBrake = Move->BrakingDecelerationFlying;
		Move->MaxFlySpeed = ftcore::kWaterSwimSpeed * 100.f;
		Move->BrakingDecelerationFlying = 350.f;
		Move->SetMovementMode(MOVE_Flying);
		if (!bSwimSaid)
		{
			bSwimSaid = true;
			UE_LOG(LogForgottenThird, Display, TEXT("Water: stand-in clips: no swim, tread or wade clip exists, so the swim plays the locomotion's own idle (treading) and "
				"walk (the stroke) held upright at the surface, and the wade plays the walk slowed; the climb out is a placed move with no clip."));
		}
	}
	else if (!bSwimNow && bSwimming)
	{
		bSwimming = false;
		Move->MaxFlySpeed = SwimSavedFly;
		Move->BrakingDecelerationFlying = SwimSavedBrake;
		if (Move->MovementMode == MOVE_Flying)
		{
			Move->SetMovementMode(MOVE_Falling);
		}
	}
	// At a ladder, pushing at its wall: up it (the jump at any other wall is the fallback).
	if (WaterNow.mode != ftcore::WaterMode::Dry && TryLadderClimb(false))
	{
		StirWater(DeltaSeconds);
		return;
	}
	if (bSwimming)
	{
		if (Move->MovementMode != MOVE_Flying)
		{
			Move->SetMovementMode(MOVE_Flying);
		}
		// Held at the surface: the feet eased to the swimmer's draft, never thrown up out of the water.
		const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		// ... nor pressed into a bed that rises under it (a slip, a canal's slope): there the feet skim the bed, so the
		// body comes up the slope with its stroke until the water is shallow enough to stand in.
		const double Bed = (WaterNow.surface - WaterNow.column) * 100.0;
		const double Target = FMath::Max(ftcore::waterSwimFeetZ(WaterNow.surface, System->WaterTime()) * 100.0, Bed + 8.0) + Half;
		Move->Velocity.Z = FMath::Clamp((Target - GetActorLocation().Z) * 5.0, -250.0, 120.0);
		const FVector Push(WaterNow.flow.x * 100.0 * DeltaSeconds, WaterNow.flow.y * 100.0 * DeltaSeconds, 0.0);
		if (!Push.IsNearlyZero())
		{
			AddActorWorldOffset(Push, true);
		}
	}
	StirWater(DeltaSeconds);
}

void AFTExploreCharacter::StirWater(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	if (!System || !System->HasBodies())
	{
		return;
	}
	// As the surfaces see it now: during the climb out the state is the one it began with, the feet rising out of it.
	const FVector At = GetActorLocation();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	ftcore::WaterState State = WaterNow;
	if (State.mode != ftcore::WaterMode::Dry)
	{
		State.immersion = FMath::Max(State.surface - static_cast<float>((At.Z - Half) * 0.01), 0.f);
		if (State.immersion <= 0.f)
		{
			State.mode = ftcore::WaterMode::Dry;
		}
	}
	System->LeadWater(*this, State, At, GetVelocity(), 2.f * Half, DeltaSeconds);
	ShowWetness(System->LeadWetness());
}

namespace
{
// No wet print on the party: every primitive of `Actor` and of what hangs on it (clothes, weapons) takes no decals.
void NoPrintsOn(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	TInlineComponentArray<UPrimitiveComponent*> Parts(Actor);
	for (UPrimitiveComponent* Part : Parts)
	{
		if (Part && Part->bReceivesDecals)
		{
			Part->SetReceivesDecals(false);
		}
	}
	Actor->ForEachAttachedActors([](AActor* Attached)
	{
		NoPrintsOn(Attached);
		return true;
	});
}
} // namespace

void AFTExploreCharacter::TickWetPrints(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	const UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	if (!System || !System->HasBodies())
	{
		return;
	}
	const float Wetness = System->LeadWetness().wetness;
	// The pool, once, the first time the lead is wet (in the water, out of sight of any print).
	if (WetPrintDecals.Num() == 0 && Wetness > 0.f)
	{
		const FString Path = FString(FTScene::LookWetPrintMaterial) + TEXT(".") + FPackageName::GetShortName(FTScene::LookWetPrintMaterial);
		UMaterialInterface* PrintMaterial = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!PrintMaterial)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Wet prints: %s is missing (FTBuildLookMaterials -Prints); none show."), FTScene::LookWetPrintMaterial);
		}
		WetPrintDecals.Reserve(static_cast<int32>(ftcore::kWetPrintCap));
		for (std::size_t I = 0; I < ftcore::kWetPrintCap; ++I)
		{
			UDecalComponent* Decal = NewObject<UDecalComponent>(this);
			Decal->SetUsingAbsoluteLocation(true);
			Decal->SetUsingAbsoluteRotation(true);
			Decal->SetUsingAbsoluteScale(true);
			// Down 5 cm into the ground and up 5 out of it; the print's whole side across.
			Decal->DecalSize = FVector(5.0, 0.5 * FTScene::WetPrintSpan, 0.5 * FTScene::WetPrintSpan);
			Decal->FadeScreenSize = 0.f;
			Decal->SetDecalMaterial(PrintMaterial);
			Decal->SetVisibility(false);
			Decal->RegisterComponent();
			WetPrintDecals.Add(Decal);
		}
		NoPrintsOn(this);
		for (TActorIterator<AFTPartyFollower> It(World); It; ++It)
		{
			NoPrintsOn(*It);
		}
		WetPrintLook = 2.f;
	}
	// On foot on dry ground: walking or running, not falling, swimming, wading, climbing or vaulting.
	const UFTLocomotionAnimInstance* Anim = GetMesh() ? Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance()) : nullptr;
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	const bool bInWater = bSwimming || bWaterClimbing || WaterNow.mode != ftcore::WaterMode::Dry;
	bool bOnFoot = Anim && Move && Move->IsMovingOnGround() && !bInWater && !bTraversing;
	if (bOnFoot)
	{
		const ftcore::Locomotion& Loco = Anim->GetLocomotion();
		bOnFoot = Loco.state() == ftcore::LocoState::Ground && GetVelocity().Size2D() >= Loco.tuning().moveStop;
	}
	const float Phase = Anim ? Anim->LeadPhaseAtFrameStart() : -1.f;
	// The body dries at the water's rate (UFTWaterSubsystem steps it at dryScale 1); its prints age at the same.
	constexpr float DryScale = 1.f;
	const int Laid = WetPrints.step(Phase, bOnFoot, Wetness, DeltaSeconds, DryScale);
	if (Laid >= 0 && Laid < WetPrintDecals.Num())
	{
		PlaceWetPrint(Laid, DryScale);
	}
	// Now and then: the faded decals hidden; a follower come in since takes no prints.
	if ((WetPrintLook -= DeltaSeconds) <= 0.f && WetPrintDecals.Num() > 0)
	{
		WetPrintLook = 2.f;
		for (int32 I = 0; I < WetPrintDecals.Num(); ++I)
		{
			UDecalComponent* Decal = WetPrintDecals[I];
			if (Decal && Decal->IsVisible() && !(WetPrints.strength(static_cast<std::size_t>(I)) > 0.f))
			{
				Decal->SetVisibility(false);
			}
		}
		NoPrintsOn(this);
		for (TActorIterator<AFTPartyFollower> It(World); It; ++It)
		{
			NoPrintsOn(*It);
		}
	}
	// The low-rate log: from leaving the water, every 2 s while the lead is wet or prints show, and once when both end.
	if (bInWater)
	{
		WetPrintOut = 0.f;
		WetPrintLogAt = 0.f;
		bWetPrintLoggedDry = false;
	}
	else if (WetPrintOut >= 0.f)
	{
		WetPrintOut += DeltaSeconds;
		if (!bWetPrintLoggedDry && WetPrintOut >= WetPrintLogAt)
		{
			WetPrintLogAt += 2.f;
			const int32 Live = static_cast<int32>(WetPrints.live());
			int32 Shown = 0;
			for (const UDecalComponent* Decal : WetPrintDecals)
			{
				Shown += Decal && Decal->IsVisible() ? 1 : 0;
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Wet prints: %d live (%d shown), %u laid, wetness %.2f, %.0f s out of the water, frame %llu."), Live, Shown,
				WetPrints.laid(), Wetness, WetPrintOut, static_cast<unsigned long long>(GFrameCounter));
			bWetPrintLoggedDry = Live == 0 && !(Wetness > 0.f);
		}
	}
}

void AFTExploreCharacter::PlaceWetPrint(int32 Index, float DryScale)
{
	UDecalComponent* Decal = WetPrintDecals[Index];
	if (!Decal)
	{
		return;
	}
	const ftcore::WetPrint& Print = WetPrints.print(static_cast<std::size_t>(Index));
	const bool bLeft = Print.foot == ftcore::WetFoot::Left;
	// The foot: between its ankle and its ball, else a hand's width to the body's side.
	const USkeletalMeshComponent* Body = GetMesh();
	const FName Ankle = bLeft ? FName(TEXT("foot_l")) : FName(TEXT("foot_r"));
	const FName Ball = bLeft ? FName(TEXT("ball_l")) : FName(TEXT("ball_r"));
	const FVector At = GetActorLocation();
	const float FeetZ = static_cast<float>(At.Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	FVector Foot = At + GetActorRightVector() * (bLeft ? -11.0 : 11.0);
	if (Body && Body->GetBoneIndex(Ankle) != INDEX_NONE && Body->GetBoneIndex(Ball) != INDEX_NONE)
	{
		Foot = 0.5 * (Body->GetSocketLocation(Ankle) + Body->GetSocketLocation(Ball));
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FTWetPrint), false, this);
	FHitResult Hit;
	const bool bGround = GetWorld()->LineTraceSingleByChannel(Hit, FVector(Foot.X, Foot.Y, FeetZ + 30.0), FVector(Foot.X, Foot.Y, FeetZ - 60.0), ECC_Visibility, Params);
	// Dry walkable ground only: never on water, a slope or inside something.
	if (!bGround || Hit.bStartPenetrating || Hit.ImpactNormal.Z < 0.7)
	{
		Decal->SetVisibility(false);
		return;
	}
	const UFTWaterSubsystem* System = GetWorld()->GetSubsystem<UFTWaterSubsystem>();
	const ftcore::Vec2 Plan{static_cast<float>(Hit.ImpactPoint.X * 0.01), static_cast<float>(Hit.ImpactPoint.Y * 0.01)};
	if (System && System->HasBodies() && System->Bodies().overWater(Plan, static_cast<float>(Hit.ImpactPoint.Z * 0.01), 0.02f))
	{
		Decal->SetVisibility(false);
		return;
	}
	// Down the decal's X; its u (the material's along) is the body's heading, its v the body's right.
	Decal->SetWorldLocationAndRotation(Hit.ImpactPoint, FRotator(-90.0, GetActorRotation().Yaw, 0.0));
	Decal->SetDecalColor(FLinearColor(bLeft ? 0.f : 1.f, Print.drops, FMath::Frac(static_cast<float>(Print.serial) * 0.618034f), Print.stamp));
	Decal->SetVisibility(true);
	// The engine's fade last, from this frame (a colour or visibility change re-makes the decal's proxy, which starts
	// its fade from when it is made): 1 to 0 over the print's life, so the stamp x fade is the Core's strength. The
	// fade's own end would destroy the component; the pool keeps it.
	Decal->SetFadeOut(0.f, ftcore::WetPrints::life(Print.stamp) / DryScale, false);
	Decal->SetLifeSpan(0.f);
}

void AFTExploreCharacter::ShowWetness(const ftcore::WaterWetness& Wet)
{
	if (!(Wet.wetness > 0.f) && !(WetShown > 0.f) && !(Wet.silt > 0.f) && !(WetSiltShown > 0.f))
	{
		return;
	}
	if (!bWetMaterialsMade)
	{
		// Once: a dynamic instance of every material on the lead's skeletal meshes (body, head, clothes).
		bWetMaterialsMade = true;
		TInlineComponentArray<USkeletalMeshComponent*> Meshes(this);
		for (USkeletalMeshComponent* Part : Meshes)
		{
			const int32 Count = Part ? Part->GetNumMaterials() : 0;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!Part->GetMaterial(Index))
				{
					continue;
				}
				if (UMaterialInstanceDynamic* Dynamic = Part->CreateAndSetMaterialInstanceDynamic(Index))
				{
					Dynamic->SetScalarParameterValue(TEXT("WetLineSoft"), 8.f);
					WetMaterials.Add(Dynamic);
				}
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Water: wetness goes to %d dynamic materials on %d skeletal meshes (Wetness, WetLineZ, WetLineSoft, WetSilt)."),
			WetMaterials.Num(), Meshes.Num());
	}
	// The line is kept over the feet, so it travels with the body; it is written when it has moved a centimetre.
	const float LineZ = static_cast<float>(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) + Wet.line * 100.f;
	const bool bWet = FMath::Abs(Wet.wetness - WetShown) > 0.004f || (Wet.wetness <= 0.f && WetShown > 0.f);
	const bool bLine = FMath::Abs(LineZ - WetLineShown) > 1.f;
	const bool bSilt = Wet.silt != WetSiltShown;
	if (!bWet && !bLine && !bSilt)
	{
		return;
	}
	static const FName WetnessName(TEXT("Wetness"));
	static const FName LineName(TEXT("WetLineZ"));
	static const FName SiltName(TEXT("WetSilt"));
	for (UMaterialInstanceDynamic* Dynamic : WetMaterials)
	{
		if (!Dynamic)
		{
			continue;
		}
		if (bWet)
		{
			Dynamic->SetScalarParameterValue(WetnessName, Wet.wetness);
		}
		if (bLine)
		{
			Dynamic->SetScalarParameterValue(LineName, LineZ);
		}
		if (bSilt)
		{
			Dynamic->SetScalarParameterValue(SiltName, Wet.silt);
		}
	}
	WetShown = bWet ? Wet.wetness : WetShown;
	WetLineShown = bLine ? LineZ : WetLineShown;
	WetSiltShown = Wet.silt;
}

bool AFTExploreCharacter::TryLadderClimb(bool bPressed)
{
	UWorld* World = GetWorld();
	const UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	if (!System || bWaterClimbing || WaterNow.mode == ftcore::WaterMode::Dry)
	{
		return false;
	}
	const FVector At = GetActorLocation();
	const ftcore::WaterBodies& Bodies = System->Bodies();
	// Within reach of a ladder's foot (a little further for a pressed jump).
	const int32 Index = Bodies.nearestExit(ftcore::Vec2{static_cast<float>(At.X * 0.01), static_cast<float>(At.Y * 0.01)},
		ftcore::kWaterLadderReach * (bPressed ? 1.5f : 1.f));
	if (Index < 0 || Bodies.exit(static_cast<std::size_t>(Index)).kind != ftcore::WaterExitKind::Ladder)
	{
		return false;
	}
	const ftcore::WaterExit& Exit = Bodies.exit(static_cast<std::size_t>(Index));
	const FVector Out(Exit.out.x, Exit.out.y, 0.0);
	// Pushing at the wall (the stick or a scripted walk), or facing it when jump is pressed.
	const FVector Push = GetCharacterMovement()->GetLastInputVector().GetSafeNormal2D();
	const bool bPushing = FVector::DotProduct(Push, Out) > 0.5;
	const bool bToward = FVector::DotProduct(GetActorForwardVector().GetSafeNormal2D(), Out) > 0.3;
	if (!(bPushing || (bPressed && bToward)))
	{
		return false;
	}
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const double Feet = At.Z - Half;
	bWaterClimbing = true;
	WaterClimbTime = 0.f;
	WaterClimbFrom = At;
	WaterClimbTo = FVector(Exit.top.x * 100.0, Exit.top.y * 100.0, Exit.topZ * 100.0 + Half + 2.0);
	// A rung at a time: about a second and a half for a quay wall.
	WaterClimbSeconds = FMath::Clamp(static_cast<float>((WaterClimbTo.Z - Half - Feet) * 0.01) * 0.5f, 0.9f, 2.4f);
	if (bSwimming)
	{
		bSwimming = false;
		GetCharacterMovement()->MaxFlySpeed = SwimSavedFly;
		GetCharacterMovement()->BrakingDecelerationFlying = SwimSavedBrake;
	}
	SetActorRotation(FRotator(0.0, Out.Rotation().Yaw, 0.0));
	GetCharacterMovement()->StopMovementImmediately();
	GetCharacterMovement()->SetMovementMode(MOVE_None);
	UE_LOG(LogForgottenThird, Display, TEXT("Water: climbing the ladder at (%.0f, %.0f) cm, %.2f m up in %.1f s."), Exit.foot.x * 100.f, Exit.foot.y * 100.f,
		(WaterClimbTo.Z - Half - Feet) * 0.01, WaterClimbSeconds);
	return true;
}

bool AFTExploreCharacter::TryWaterClimb()
{
	// Facing a wall within arm's reach whose top is dry, walkable and no more than a reach over the feet: up and onto it.
	constexpr double kReach = 90.0;    // cm ahead
	constexpr double kTopMost = 330.0; // cm over the feet
	UWorld* World = GetWorld();
	if (!World || bWaterClimbing || WaterNow.mode == ftcore::WaterMode::Dry)
	{
		return false;
	}
	const FVector At = GetActorLocation();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const double Feet = At.Z - Half;
	const FVector Ahead = GetActorForwardVector().GetSafeNormal2D();
	FCollisionQueryParams Query(SCENE_QUERY_STAT(FTWaterClimb), false, this);
	FHitResult Wall;
	// The wall is looked for just over the surface and at the body's middle (a wall that ends under the surface is a step).
	const FVector Chest(At.X, At.Y, FMath::Max(At.Z, WaterNow.surface * 100.0 + 10.0));
	if (!World->LineTraceSingleByChannel(Wall, Chest, Chest + Ahead * kReach, ECC_Visibility, Query) || FMath::Abs(Wall.ImpactNormal.Z) > 0.5)
	{
		return false;
	}
	// Onto the top just past the wall's face, or, where something stands there (a railing along the edge), past it.
	FVector Onto = FVector::ZeroVector;
	FHitResult Top;
	bool bFound = false;
	for (const double Past : {Radius + 12.0, Radius + 55.0, Radius + 95.0})
	{
		Onto = Wall.ImpactPoint + Ahead * Past;
		if (!World->LineTraceSingleByChannel(Top, FVector(Onto.X, Onto.Y, Feet + kTopMost), FVector(Onto.X, Onto.Y, Feet + 20.0), ECC_Visibility, Query)
			|| Top.bStartPenetrating || Top.ImpactNormal.Z < 0.7 || Top.ImpactPoint.Z <= WaterNow.surface * 100.0)
		{
			continue;
		}
		const FVector Stand(Onto.X, Onto.Y, Top.ImpactPoint.Z + Half + 2.0);
		if (!World->OverlapBlockingTestByChannel(Stand, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius, Half), Query))
		{
			bFound = true;
			break;
		}
	}
	if (!bFound)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Water: no way out up this wall at (%.0f, %.0f) cm (no clear dry top within %.1f m of the feet)."),
			Wall.ImpactPoint.X, Wall.ImpactPoint.Y, kTopMost * 0.01);
		return false;
	}
	bWaterClimbing = true;
	WaterClimbTime = 0.f;
	WaterClimbSeconds = 0.9f;
	WaterClimbFrom = At;
	WaterClimbTo = FVector(Onto.X, Onto.Y, Top.ImpactPoint.Z + Half + 2.0);
	if (bSwimming)
	{
		bSwimming = false;
		GetCharacterMovement()->MaxFlySpeed = SwimSavedFly;
		GetCharacterMovement()->BrakingDecelerationFlying = SwimSavedBrake;
	}
	GetCharacterMovement()->StopMovementImmediately();
	GetCharacterMovement()->SetMovementMode(MOVE_None);
	UE_LOG(LogForgottenThird, Display, TEXT("Water: climbing out (the fallback: no ladder or steps here), %.2f m up the wall at (%.0f, %.0f) cm."), (Top.ImpactPoint.Z - Feet) * 0.01, Onto.X, Onto.Y);
	return true;
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
	// Wading: the walk is slowed by the water's depth (the movement caps its speed at the input's share).
	if (!bSwimming && (WaterNow.mode == ftcore::WaterMode::Wade || WaterNow.mode == ftcore::WaterMode::DeepWade))
	{
		ScaleValue *= WaterNow.speedFactor;
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
