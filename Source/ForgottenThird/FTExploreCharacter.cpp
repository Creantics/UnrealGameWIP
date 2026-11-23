#include "FTExploreCharacter.h"
#include "FTPartyVan.h"
#include "FTRainCoverSubsystem.h"
#include "FTRainSubsystem.h"
#include "FTSwimUnits.h"
#include "FTWaterSubsystem.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/WaterSurfaceFeet.h"

#include "Animation/AnimSequence.h"
#include "AnimationRuntime.h"
#include "Camera/CameraActor.h"
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
#include "Components/SkinnedMeshComponent.h"
#include "PhysicsEngine/BodySetup.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "TimerManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "Engine/GameViewportClient.h"
#include "UnrealClient.h"
#include "RHI.h"
#include "RenderCore.h"
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
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "UObject/ConstructorHelpers.h"

#include "FTExploreDirector.h"
#include "FTExploreHUD.h"
#include "FTCutsceneDirector.h"
#include "FTFieldDummy.h"
#include "FTGameInstance.h"
#include "FTInputBindings.h"
#include "FTPauseSubsystem.h"
#include "FTPlayerController.h"
#include "FTMeshFacing.h"
#include "FTNewUi.h"
#include "FTPartyFollower.h"
#include "Engine/StreamableManager.h"
#include "FTSceneLook.h"
#include "Components/DecalComponent.h"
#include "Misc/PackageName.h"
#include "ForgottenThird.h"
#include "FTRim.h"
#include "FTGcPacingSubsystem.h"
#include "FTText.h"

#include "ftcore/BattleDemo.h"
#include "ftcore/LeadWet.h"
#include "ftcore/WaterSpotCheck.h"

namespace
{
TAutoConsoleVariable<float> CVarLeadWet(TEXT("ft.LeadWet"), -1.f,
	TEXT("The leads' wet switch: below 0 (the default) follows the rain (UFTRainSubsystem), 0..1 forces that wetness (cloth soak, wet grooms, wet skin; 0 holds them dry)."));

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

	Rim = CreateDefaultSubobject<UFTRimComponent>(TEXT("Rim"));
	Rim->SetupAttachment(GetCapsuleComponent());
	Rim->SetRelativeLocation(FVector(0.0, 0.0, 60.0)); // chest to head on a standing lead

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
	// -PartyExitFast (item 2 of 11-18; off until its proof passes): the partners climb out 1 s behind the lead by its ladder
	// or the flight it walked out by, on Core's climb path (ftcore::waterFollowFastExit).
	if (FParse::Param(FCommandLine::Get(), TEXT("PartyExitFast")))
	{
		ftcore::WaterFollowTuning Fast = WaterParty.tuning();
		ftcore::waterFollowFastExit(Fast);
		// -PartyExitInOrder (with -PartyExitFast): the 11-19 rules, the partners climbing strictly in index order; the
		// off switch for exitReadyFirst (one at its place in the queue climbs ahead of one still swimming to its own).
		const bool bInOrder = FParse::Param(FCommandLine::Get(), TEXT("PartyExitInOrder"));
		Fast.exitReadyFirst = !bInOrder;
		WaterParty = ftcore::WaterFollowParty(Fast);
		UE_LOG(LogForgottenThird, Display, TEXT("Water: -PartyExitFast: the partners climb out 1 s behind the lead, by its way out, on the climb path (%s)."),
			bInOrder ? TEXT("-PartyExitInOrder: strictly in index order") : TEXT("one ready at its place climbs first"));
	}
	// -SwimSwitchKeep (item 3 of 11-18; off until its proof passes): a lead switch (the party menu) while swimming keeps the
	// party in the water: the new lead at the surface swim, the partners respawned at their surface slots
	// (ftcore::waterSwitchStart); a switch while climbing out or being rescued is refused.
	bSwimSwitchKeep = FParse::Param(FCommandLine::Get(), TEXT("SwimSwitchKeep"));
	if (bSwimSwitchKeep)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Water: -SwimSwitchKeep: a lead switch while swimming keeps the party in the water (refused while climbing out or rescued)."));
	}
#if !UE_BUILD_SHIPPING // capture and check switches: not in a shipped game
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
			// -LeadGoDive: swimming in a dive area the lead dives once, swims the route under water along it, and
			// rises when the route ends.
			bLeadGoDive = FParse::Param(FCommandLine::Get(), TEXT("LeadGoDive"));
			FParse::Value(FCommandLine::Get(), TEXT("LeadGoQuit="), LeadGoQuit);
		}
		FParse::Value(FCommandLine::Get(), TEXT("LeadGoPace="), LeadGoPace);
		LeadGoPace = FMath::Clamp(LeadGoPace, 0.1f, 1.f);
		// -LeadGoRoute=<x,y>@<s>[;<x,y>@<s>...] (plan frame of -WorldEyeSide): at each game time, rising, the lead walks at
		// that point and stands there until the next leg. A bad leg is an Error and exit 2.
		FString Route;
		if (FParse::Value(FCommandLine::Get(), TEXT("LeadGoRoute="), Route, false))
		{
			double RouteSide = 1000.0;
			FParse::Value(FCommandLine::Get(), TEXT("WorldEyeSide="), RouteSide);
			TArray<FString> Legs;
			Route.ParseIntoArray(Legs, TEXT(";"));
			bool bOk = Legs.Num() > 0 && Legs.Num() <= UE_ARRAY_COUNT(LeadRouteTo);
			for (int32 L = 0; bOk && L < Legs.Num(); ++L)
			{
				FString Point, When, PX, PY;
				bOk = Legs[L].Split(TEXT("@"), &Point, &When) && Point.Split(TEXT(","), &PX, &PY) && PX.IsNumeric() && PY.IsNumeric() && When.IsNumeric();
				if (bOk)
				{
					const ftcore::MassingVec To = ftcore::massingEnginePoint(RouteSide, FCString::Atod(*PX), FCString::Atod(*PY), 0.0);
					LeadRouteTo[L] = FVector(To.x * 100.0, To.y * 100.0, 0.0);
					LeadRouteAt[L] = FCString::Atof(*When);
					bOk = LeadRouteAt[L] >= 0.f && (L == 0 || LeadRouteAt[L] > LeadRouteAt[L - 1]);
				}
			}
			if (bOk)
			{
				LeadRouteCount = Legs.Num();
				UE_LOG(LogForgottenThird, Display, TEXT("LeadGo: route of %d leg(s), the first at %.1f s."), LeadRouteCount, LeadRouteAt[0]);
			}
			else
			{
				UE_LOG(LogForgottenThird, Error, TEXT("LeadGo: '%s' is not <x,y>@<s>[;<x,y>@<s>...] with rising times (at most %d legs)."), *Route,
					static_cast<int32>(UE_ARRAY_COUNT(LeadRouteTo)));
				FTRequestExit(2, TEXT("LeadGoRoute"));
			}
		}
	}
#endif
	AFTPartyVan::SpawnAt(GetWorld()); // -PartyVanAt only; one van however many leads begin
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
	Dodge.setTuning(UFTLocomotionAnimInstance::GetMoveTuning().dodge);
	Ledge.setTuning(UFTLocomotionAnimInstance::GetMoveTuning().ledge);
	DodgeNow = Dodge.fresh();
	// The portrait and look-dev captures (-ExplorePortrait and every -ExplorePortrait*/-ExploreLookDev*/-ExploreFixedExposure/
	// -ExploreGreyGround flag under it) are capture tools: compiled out of shipping.
#if UE_BUILD_SHIPPING
	const bool bPortrait = false;
	const bool bShowAnyLook = false;
#else
	const bool bPortrait = FParse::Param(FCommandLine::Get(), TEXT("ExplorePortrait"));
	// -ExploreShowLook: a capture may wear a look that is not playable (a party member only) on this character, so a
	// still of that member never needs the arena data changed. Review only.
	const bool bShowAnyLook = FParse::Param(FCommandLine::Get(), TEXT("ExploreShowLook"));
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
		// A party member that is not playable (its kit's data flag) is never the lead: the first playable look starts instead.
		if (!bShowAnyLook && !IsPlayableLead(PartyLooks[LeadIndex]))
		{
			const int32 Playable = PartyLooks.IndexOfByPredicate([this](const FFTLeadLook& Member)
			{
				return IsPlayableLead(Member);
			});
			UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: %s is a party member only (not playable) and can't be the lead; starting as %s."),
				*PartyLooks[LeadIndex].Name, Playable >= 0 ? *PartyLooks[Playable].Name : TEXT("that look anyway (no look is playable)"));
			LeadIndex = Playable >= 0 ? Playable : LeadIndex;
		}
		WearLook(PartyLooks[LeadIndex]);
	}
	// The whole party's assets, loading in the background from now on, so a lead switch finds them in memory.
	if (!PartyLooks.IsEmpty() && !FParse::Param(FCommandLine::Get(), TEXT("NoPartyHold")))
	{
		PartyHold = FTLook::HoldLooks(PartyLooks, TEXT("Party hold"));
	}
	FTLook::LogLevelStart(PartyHold.IsValid() ? TEXT("lead BeginPlay, party hold requested") : TEXT("lead BeginPlay, no party hold"));
	FindPartyRows(); // the party screen's rows, again whenever the lead changes (SwitchLeadTo)
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
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("ExplorePortraitCard")))
		{
			AddPortraitCard();
		}
#endif
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
#if !UE_BUILD_SHIPPING // review checks only, never in a shipped game
		// Exposure-range check: -ExploreNight dims the sun to moonlight and the sky to a night sky (the gate lamps stay
		// lit); -ExploreExposureRange=<min>,<max> meters auto-exposure within that EV100 range instead of the map's.
		if (FParse::Param(FCommandLine::Get(), TEXT("ExploreNight")))
		{
			Nightfall();
		}
		// Wetness proof for the lead materials: -ExploreWet=<Wetness>,<line cm above the feet>,<WetSilt> sets the garment
		// wet inputs (Wetness, WetLineZ, WetSilt) on every slot that has them, once a second so the garments ApplyLook
		// makes later (and a look switch) take them too. The world drives these in play; this only shows a fixed state.
		FString Wet;
		if (FParse::Value(FCommandLine::Get(), TEXT("ExploreWet="), Wet, false))
		{
			TArray<FString> Parts;
			Wet.ParseIntoArray(Parts, TEXT(","));
			WetReview = FVector(Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 1.f, Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 130.f,
				Parts.Num() > 2 ? FCString::Atof(*Parts[2]) : 0.f);
			GetWorldTimerManager().SetTimer(WetTimer, this, &AFTExploreCharacter::SoakForReview, 1.f, true, 0.5f);
		}
		// Wet state across a piece's mesh swap: -ExploreSoakSwap=<wetness>,<word>@<off s>[+<on s>] holds the lead at that
		// wetness through the game's own path (ShowWetness; the water's drive is off), takes off every piece whose asset name
		// holds the word at <off s> and puts it back at <on s> (FTLook::SetPieceWorn, so an under layer swaps its cut).
		// SOAK_SWAP lines give each slot's material and Wetness value at each stage; stills before and after show it.
		FString SoakSwap;
		if (FParse::Value(FCommandLine::Get(), TEXT("ExploreSoakSwap="), SoakSwap, false))
		{
			FString Head, Times, Off, On;
			if (SoakSwap.Split(TEXT("@"), &Head, &Times) && Head.Split(TEXT(","), &Off, &SoakSwapWord))
			{
				SoakSwapWet = FMath::Clamp(FCString::Atof(*Off), 0.f, 1.f);
				if (!Times.Split(TEXT("+"), &Off, &On))
				{
					Off = Times;
				}
				SoakSwapOff = FCString::Atof(*Off);
				SoakSwapOn = On.IsEmpty() ? -1.f : FCString::Atof(*On);
				SoakSwapStart = FPlatformTime::Seconds();
				GetWorldTimerManager().SetTimer(SoakSwapTimer, this, &AFTExploreCharacter::StepSoakSwap, 0.25f, true, 0.25f);
				UE_LOG(LogForgottenThird, Display, TEXT("SOAK_SWAP wetness %.2f, pieces holding '%s' off at %.1f s, on at %.1f s."),
					SoakSwapWet, *SoakSwapWord, SoakSwapOff, SoakSwapOn);
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("SOAK_SWAP: '%s' is not <wetness>,<word>@<off s>[+<on s>]."), *SoakSwap);
			}
		}
		// The lead-switch wet swap: -ExploreSwitchReview=<soak 0..1>@<switch s, game time> gives the lead that soak at the
		// start through the water's own store (UFTWaterSubsystem::SetLeadWetness, the line at the body's height, no silt;
		// it dries from there by the water's law, so each still logs what is left), shows the party (a portrait hides it;
		// frame wide enough for it) and at <switch s> switches the lead as the key does (HandleSwitchLead). The map needs
		// water bodies (the lead's look reads the store only then): without them the switch is an Error and exit 2. The
		// soak is each member's (AFTExploreDirector::OnLeadChanged): the old lead walks behind still wet, the new one takes
		// its own. A refused switch is an Error and exit 2. Portrait shots carry their index, and each logs every soak.
		// Up to four switch times, rising (<soak>@<s>,<s>,...): each one switches again. The rain soak is the party's (one
		// value; the lines print it and each follower's weather input).
		FString SwitchReview;
		if (FParse::Value(FCommandLine::Get(), TEXT("ExploreSwitchReview="), SwitchReview, false))
		{
			FString Soak, At;
			bool bTimes = SwitchReview.Split(TEXT("@"), &Soak, &At) && !Soak.IsEmpty() && !At.IsEmpty() && Soak.IsNumeric();
			if (bTimes)
			{
				TArray<FString> Times;
				At.ParseIntoArray(Times, TEXT(","));
				bTimes = Times.Num() > 0 && Times.Num() <= UE_ARRAY_COUNT(SwitchReviewTimes);
				for (int32 T = 0; bTimes && T < Times.Num(); ++T)
				{
					SwitchReviewTimes[T] = FCString::Atof(*Times[T]);
					bTimes = Times[T].IsNumeric() && SwitchReviewTimes[T] > 0.f && (T == 0 || SwitchReviewTimes[T] > SwitchReviewTimes[T - 1]);
				}
				SwitchReviewCount = bTimes ? Times.Num() : 0;
			}
			if (bTimes && FCString::Atof(*Soak) >= 0.f && FCString::Atof(*Soak) <= 1.f)
			{
				ftcore::WaterWetness Soaked;
				Soaked.wetness = FMath::Clamp(FCString::Atof(*Soak), 0.f, 1.f);
				Soaked.line = Soaked.wetness > 0.f ? 0.02f * GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 0.f;
				UFTWaterSubsystem* Water = GetWorld() ? GetWorld()->GetSubsystem<UFTWaterSubsystem>() : nullptr;
				if (Water)
				{
					Water->SetLeadWetness(Soaked);
					SwitchReviewAt = SwitchReviewTimes[0];
					bPartyHidden = false;
					GetWorldTimerManager().SetTimer(SwitchReviewTimer, this, &AFTExploreCharacter::SwitchForReview, SwitchReviewAt, false);
					UE_LOG(LogForgottenThird, Display, TEXT("SWITCH_REVIEW the lead starts soaked to %.2f (line %.2f m); the party shows; %d switch(es), the first at %.1f s."),
						Soaked.wetness, Soaked.line, SwitchReviewCount, SwitchReviewAt);
				}
				else
				{
					UE_LOG(LogForgottenThird, Error, TEXT("SWITCH_REVIEW: no water subsystem holds the lead's soak; no proof from this run."));
					FTRequestExit(2, TEXT("ExploreSwitchReview"));
				}
			}
			else
			{
				UE_LOG(LogForgottenThird, Error, TEXT("SWITCH_REVIEW: '%s' is not <soak>@<switch s>[,<switch s>...] (the soak 0 to 1, up to four rising times above 0)."), *SwitchReview);
				FTRequestExit(2, TEXT("ExploreSwitchReview"));
			}
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
#endif
		// -ExplorePortraitShot=<path prefix>: the renderer saves one 1400x1100 shot (HUD hidden) per yaw in
		// -ExplorePortraitYaws=180,90,0 (default: the one yaw above) as <prefix>_<yaw>.png, then the game quits.
		if (FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitShot="), ShotPrefix) && !ShotPrefix.IsEmpty())
		{
#if !UE_BUILD_SHIPPING
			bPoseDump = FParse::Param(FCommandLine::Get(), TEXT("ExplorePortraitPoseDump"));
#endif
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
			if (!FaceClip && FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitBodyClip="), ClipPath)
				&& FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitBodyTimes="), TimeList, false))
			{
				BodyClip = LoadObject<UAnimSequence>(nullptr, *ClipPath);
				TArray<FString> Parts;
				TimeList.ParseIntoArray(Parts, TEXT(","));
				for (const FString& Part : Parts)
				{
					BodyTimes.Add(FCString::Atof(*Part));
				}
				if (!BodyClip || BodyTimes.IsEmpty())
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: body clip %s not loaded or no times; body poses off."), *ClipPath);
					BodyClip = nullptr;
					BodyTimes.Reset();
				}
				else
				{
					// One shot per time; the yaw list repeats round them.
					const TArray<float> Yaws = ShotYaws;
					ShotYaws.Reset();
					for (int32 Index = 0; Index < BodyTimes.Num(); ++Index)
					{
						ShotYaws.Add(Yaws[Index % Yaws.Num()]);
					}
					bPortraitWalk = false;
					bPortraitJump = false;
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
#if !UE_BUILD_SHIPPING
	// -StillCam=x,y,z,yaw,pitch,fov (-WorldStartAt's units, ftcore/StillCam.h): a still of a place, not of the lead. The
	// view holds that fixed point from the first tenth of a second (so streaming and lighting settle on it), and the lead
	// and the party are hidden in game (hidden, they cast no shadow). The portrait writer (-ExplorePortrait
	// -ExplorePortraitShot=) saves as usual; its yaws turn the boom, not this camera. Review only.
	FString StillCamText;
	if (FParse::Value(FCommandLine::Get(), TEXT("StillCam="), StillCamText, false))
	{
		StillCamView = ftcore::parseStillCam(FT::ToUtf8(StillCamText));
		if (StillCamView.on)
		{
			bStillCamLead = FParse::Param(FCommandLine::Get(), TEXT("StillCamLead"));
			bPartyHidden = true;
			SetActorHiddenInGame(!bStillCamLead);
			GetWorldTimerManager().SetTimer(StillCamTimer, this, &AFTExploreCharacter::HoldStillCam, 0.25f, true, 0.1f);
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("StillCam: -StillCam=%s isn't x,y,z,yaw,pitch,fov (pitch -89..89, fov 10..150); the view is the usual one."),
				*StillCamText);
		}
	}
#endif
}

#if !UE_BUILD_SHIPPING
void AFTExploreCharacter::HoldStillCam()
{
	// Held every quarter second: anything that shows the lead again (a look switch) or moves the camera is undone.
	AFTExploreDirector* D = GetDirector();
	if (!D || !Camera)
	{
		return;
	}
	const FVector At = D->PlanToWorldCm(StillCamView.x, StillCamView.y, StillCamView.z);
	const FRotator ViewRot(StillCamView.pitchDeg, StillCamView.yawDeg, 0.0);
	Camera->bUsePawnControlRotation = false;
	Camera->SetUsingAbsoluteLocation(true);
	Camera->SetUsingAbsoluteRotation(true);
	Camera->SetWorldLocationAndRotation(At, ViewRot);
	Camera->SetFieldOfView(static_cast<float>(StillCamView.fovDeg));
	SetActorHiddenInGame(!bStillCamLead);
	TArray<AActor*> Held;
	GetAttachedActors(Held, true, true);
	for (AActor* Attached : Held)
	{
		Attached->SetActorHiddenInGame(!bStillCamLead);
	}
	// Every hold: a partner spawned after the first is hidden too (quietly; SetPartyHidden logs, so once below).
	for (AFTPartyFollower* Follower : D->GetFollowers())
	{
		if (Follower && !Follower->IsHidden())
		{
			Follower->SetActorHiddenInGame(true);
		}
	}
	if (!bStillCamHeld)
	{
		bStillCamHeld = true;
		D->SetPartyHidden(true);
		UE_LOG(LogForgottenThird, Display, TEXT("StillCam: view held at (%.0f, %.0f, %.0f) cm, yaw %.1f, pitch %.1f, fov %.1f; %s, %d attached; party hidden."),
			At.X, At.Y, At.Z, StillCamView.yawDeg, StillCamView.pitchDeg, StillCamView.fovDeg,
			bStillCamLead ? TEXT("lead in sight (-StillCamLead)") : TEXT("lead hidden"), Held.Num());
	}
}
#endif

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
		else if (BodyClip)
		{
			HoldBodyAt(BodyTimes[ShotIndex]);
		}
		bShotAimed = true;
		return;
	}
	// A moving light (or a face clip, a lead switch, or a -StillCamLead sequence) makes shots at the same yaw differ, so they carry their index.
#if !UE_BUILD_SHIPPING
	const bool bIndexed = LookDevSpin != 0.f || bStillCamLead || SwitchReviewAt >= 0.f;
#else
	const bool bIndexed = LookDevSpin != 0.f || bStillCamLead;
#endif
	const FString File = FaceClip ? FString::Printf(TEXT("%s_%d_f%d.png"), *ShotPrefix, FMath::RoundToInt(Yaw), ShotIndex)
		: BodyClip                ? FString::Printf(TEXT("%s_%d_b%d.png"), *ShotPrefix, FMath::RoundToInt(Yaw), ShotIndex)
		: bIndexed                ? FString::Printf(TEXT("%s_%d_s%02d.png"), *ShotPrefix, FMath::RoundToInt(Yaw), ShotIndex)
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
#if !UE_BUILD_SHIPPING
	if (bPoseDump)
	{
		DumpPortraitPose(File, Yaw);
	}
	if (SwitchReviewAt >= 0.f)
	{
		LogSwitchReview(*FPaths::GetCleanFilename(File));
	}
#endif
	bShotAimed = false;
	++ShotIndex;
}

#if !UE_BUILD_SHIPPING
void AFTExploreCharacter::DumpPortraitPose(const FString& ShotFile, float Yaw) const
{
	// Written on the shot's frame, so the pose is the one the shot draws. Review only: a path inside the project is
	// refused, so a dump (it has the lead's body in it) can't land in the repository.
	const FString PoseFile = FPaths::ChangeExtension(ShotFile, TEXT("pose.json"));
	FString Full = FPaths::ConvertRelativePathToFull(PoseFile);
	FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(Project);
	if (FPaths::IsUnderDirectory(Full, Project))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: pose dump %s is inside the project; not written."), *Full);
		return;
	}
	auto Xf = [](const FTransform& T)
	{
		const FVector P = T.GetLocation();
		const FQuat Q = T.GetRotation();
		const FVector S = T.GetScale3D();
		return FString::Printf(TEXT("[%.4f, %.4f, %.4f, %.7f, %.7f, %.7f, %.7f, %.5f, %.5f, %.5f]"), P.X, P.Y, P.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z);
	};
	USkeletalMeshComponent* Body = GetMesh();
	FString Json = FString::Printf(TEXT("{\n\"shot\": \"%s\",\n\"yaw\": %.1f,\n\"frame_counter\": %llu,\n\"world_seconds\": %.4f,\n\"transform_layout\": \"tx ty tz (cm) qx qy qz qw sx sy sz, component space unless named world\",\n"),
		*ShotFile.Replace(TEXT("\\"), TEXT("/")), Yaw, static_cast<uint64>(GFrameCounter), GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0);
	// The view the shot is drawn from (the camera manager's, after the boom).
	const APlayerController* Player = Cast<APlayerController>(GetController());
	if (Player && Player->PlayerCameraManager)
	{
		const FMinimalViewInfo& View = Player->PlayerCameraManager->GetCameraCacheView();
		Json += FString::Printf(TEXT("\"view\": {\"world\": %s, \"fov\": %.4f, \"aspect\": %.5f, \"shot_size\": [1400, 1100]},\n"),
			*Xf(FTransform(View.Rotation, View.Location)), View.FOV, View.AspectRatio);
	}
	// What poses the body: a held clip (-ExplorePortraitBodyClip) or the locomotion's layers.
	const int32 Held = ShotIndex;
	if (BodyClip && BodyTimes.IsValidIndex(Held))
	{
		const double Rate = BodyClip->GetSamplingFrameRate().AsDecimal();
		const float Time = FMath::Clamp(BodyTimes[Held], 0.f, BodyClip->GetPlayLength());
		Json += FString::Printf(TEXT("\"pose_source\": {\"mode\": \"held\", \"asset\": \"%s\", \"time\": %.4f, \"frame\": %.2f, \"fps\": %.2f},\n"),
			*BodyClip->GetName(), Time, Time * Rate, Rate);
	}
	else if (const UFTLocomotionAnimInstance* Loco = Body ? Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance()) : nullptr)
	{
		Json += FString::Printf(TEXT("\"pose_source\": {\"mode\": \"locomotion\", \"loco\": %s},\n"), *Loco->PoseLayersJson());
	}
	else
	{
		Json += TEXT("\"pose_source\": {\"mode\": \"unknown\"},\n");
	}
	// Every skinned component on the lead, each bone as that component draws it: a leader-posed piece reads its leader's
	// bone of the same name (GetBoneTransform follows the leader map), so a piece's correctives can be set against the
	// body's. "in_leader" 0 marks a bone the leader lacks; "ref" is the bone's reference pose in component space.
	TInlineComponentArray<USkinnedMeshComponent*> Skinned(this);
	Json += TEXT("\"components\": [\n");
	int32 Written = 0;
	for (const USkinnedMeshComponent* Comp : Skinned)
	{
		const USkinnedAsset* Asset = Comp ? Comp->GetSkinnedAsset() : nullptr;
		if (!Asset)
		{
			continue;
		}
		const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
		const USkinnedMeshComponent* Leader = Comp->LeaderPoseComponent.Get();
		const FTransform ToWorld = Comp->GetComponentTransform();
		Json += FString::Printf(TEXT("%s{\"name\": \"%s\", \"asset\": \"%s\", \"visible\": %d, \"leader\": \"%s\", \"world\": %s, \"bones\": [\n"),
			Written ? TEXT(",\n") : TEXT(""), *Comp->GetName(), *Asset->GetName(), Comp->IsVisible() && !Comp->bHiddenInGame ? 1 : 0,
			Leader ? *Leader->GetName() : TEXT(""), *Xf(ToWorld));
		for (int32 B = 0; B < Ref.GetNum(); ++B)
		{
			const FName BoneName = Ref.GetBoneName(B);
			const int32 InLeader = !Leader ? 1 : Leader->GetBoneIndex(BoneName) != INDEX_NONE ? 1 : 0;
			const FTransform Drawn = Comp->GetBoneTransform(B).GetRelativeTransform(ToWorld);
			Json += FString::Printf(TEXT("%s{\"n\": \"%s\", \"p\": %d, \"in_leader\": %d, \"cs\": %s, \"ref\": %s}"), B ? TEXT(",\n") : TEXT(""),
				*BoneName.ToString(), Ref.GetParentIndex(B), InLeader, *Xf(Drawn), *Xf(FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, B)));
		}
		Json += TEXT("]}");
		++Written;
	}
	Json += TEXT("\n]\n}\n");
	if (FFileHelper::SaveStringToFile(Json, *Full))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: pose dump %s (%d skinned components)."), *Full, Written);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: pose dump %s not written."), *Full);
	}
}
#endif

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

void AFTExploreCharacter::HoldBodyAt(float Time)
{
	// The body leaves its anim blueprint and holds the clip's frame; the leader-posed pieces (garments, shoes, face)
	// copy its bones, so the outfit is seen in that pose. The clip's root is not applied, so the capsule stays put.
	USkeletalMeshComponent* Body = GetMesh();
	if (!Body || !BodyClip)
	{
		return;
	}
	Body->PlayAnimation(BodyClip, false);
	Body->SetPosition(FMath::Clamp(Time, 0.f, BodyClip->GetPlayLength()), false);
	Body->Stop();
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: body held on %s at %.2f s."), *BodyClip->GetName(), Time);
}

#if !UE_BUILD_SHIPPING
void AFTExploreCharacter::AddPortraitCard()
{
	// Dev only: -ExplorePortraitCard holds a neutral 18% grey card (M_WorldSurface at 0.18 albedo, 0.9 rough, no mottle or
	// weather, as -WorldEyeCard in FTWorldEyeShotSubsystem.cpp; Weather 0 zeroes the weather node's wet term, Wet = Wetness
	// x Amount, so the look's rain wetness doesn't darken it) on the camera, 14 x 10 cm, facing it, at -ExplorePortraitCardAt=<forward,right,up> cm
	// from the camera (default 45,-17,-10: the lower left of a head shot), so every yaw's still has a known neutral under
	// the same lights as the lead. It casts nothing and has no collision. Review only.
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Rendering/M_WorldSurface.M_WorldSurface"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Cube || !Base || !Camera)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTExploreCharacter: portrait card not laid (mesh %d, material %d, camera %d)."), Cube != nullptr,
			Base != nullptr, Camera != nullptr);
		return;
	}
	FVector At(45.0, -17.0, -10.0);
	FString AtText;
	if (FParse::Value(FCommandLine::Get(), TEXT("ExplorePortraitCardAt="), AtText, false))
	{
		TArray<FString> Parts;
		AtText.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 3)
		{
			At = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
		}
	}
	UStaticMeshComponent* Card = NewObject<UStaticMeshComponent>(this, TEXT("PortraitCard"));
	Card->SetMobility(EComponentMobility::Movable);
	Card->SetStaticMesh(Cube);
	Card->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Card->SetCastShadow(false);
	Card->SetupAttachment(Camera);
	Card->RegisterComponent();
	// The engine cube is 1 m, centred: 2 mm thick along the view, 14 cm wide, 10 cm tall.
	Card->SetRelativeLocation(At);
	Card->SetRelativeScale3D(FVector(0.002, 0.14, 0.10));
	UMaterialInstanceDynamic* Grey = UMaterialInstanceDynamic::Create(Base, Card);
	Grey->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.18f, 0.18f, 0.18f));
	Grey->SetScalarParameterValue(TEXT("Roughness"), 0.9f);
	Grey->SetScalarParameterValue(TEXT("Metallic"), 0.f);
	Grey->SetScalarParameterValue(TEXT("Variation"), 0.f);
	Grey->SetScalarParameterValue(TEXT("Weather"), 0.f);
	Grey->SetScalarParameterValue(TEXT("Street"), 0.f);
	Card->SetMaterial(0, Grey);
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: portrait card laid at %.0f, %.0f, %.0f cm from the camera (0.18 grey)."), At.X, At.Y, At.Z);
}
#endif

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

#if !UE_BUILD_SHIPPING
void AFTExploreCharacter::LogSoakSwap(const TCHAR* Stage)
{
	static const FHashedMaterialParameterInfo WetnessInfo(TEXT("Wetness"));
	for (USkeletalMeshComponent* Piece : Pieces)
	{
		if (!Piece || !Piece->GetSkeletalMeshAsset())
		{
			continue;
		}
		for (int32 Slot = 0; Slot < Piece->GetNumMaterials(); ++Slot)
		{
			UMaterialInterface* Material = Piece->GetMaterial(Slot);
			float Value = -1.f;
			const bool bHas = Material && Material->GetScalarParameterValue(WetnessInfo, Value);
			UE_LOG(LogForgottenThird, Display, TEXT("SOAK_SWAP %s: %s%s slot %d %s Wetness %s (serial %u, %d gathered)."), Stage,
				*Piece->GetSkeletalMeshAsset()->GetName(), Piece->GetVisibleFlag() ? TEXT("") : TEXT(" (off)"), Slot,
				Material ? *Material->GetName() : TEXT("none"), bHas ? *FString::Printf(TEXT("%.2f"), Value) : TEXT("n/a"),
				FTLook::WornMeshSerial(), WetMaterials.Num());
		}
	}
}

void AFTExploreCharacter::StepSoakSwap()
{
	ShowWetness(ftcore::WaterWetness{SoakSwapWet, 3.f, 0.f});
	const double Now = FPlatformTime::Seconds() - SoakSwapStart;
	const bool bOff = SoakSwapStage == 0 && Now >= SoakSwapOff;
	const bool bOn = SoakSwapStage == 1 && SoakSwapOn >= 0.f && Now >= SoakSwapOn;
	if (SoakSwapStage == 0 && Now >= SoakSwapOff - 1.0 && !bSoakSwapBefore)
	{
		bSoakSwapBefore = true;
		LogSoakSwap(TEXT("before"));
	}
	if (!bOff && !bOn)
	{
		return;
	}
	int32 Changed = 0;
	for (USkeletalMeshComponent* Piece : Pieces)
	{
		if (Piece && Piece->GetSkeletalMeshAsset() && Piece->GetSkeletalMeshAsset()->GetName().Contains(SoakSwapWord))
		{
			Changed += FTLook::SetPieceWorn(WornLook, WornFronts, *GetMesh(), Pieces, *Piece, bOn, TEXT("FTExploreCharacter"));
		}
	}
	// The carried materials as the swap left them, before ShowWetness gathers them again.
	LogSoakSwap(bOn ? TEXT("swapped on") : TEXT("swapped off"));
	++SoakSwapStage;
	ShowWetness(ftcore::WaterWetness{SoakSwapWet, 3.f, 0.f});
	UE_LOG(LogForgottenThird, Display, TEXT("SOAK_SWAP %s at %.1f s: %d under piece(s) swapped."), bOn ? TEXT("on") : TEXT("off"), Now, Changed);
	LogSoakSwap(bOn ? TEXT("after on") : TEXT("after off"));
}

void AFTExploreCharacter::LogSwitchReview(const TCHAR* Stage)
{
	// Every member's soak as the water and the followers hold it, so a still can be read against its numbers.
	const UWorld* World = GetWorld();
	const UFTWaterSubsystem* Water = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	const ftcore::WaterWetness Lead = Water ? Water->LeadWetness() : ftcore::WaterWetness{};
	FString Line = FString::Printf(TEXT("SWITCH_REVIEW %s at %.2f s: lead %d %s soak %.2f (line %.2f m), weather %.2f (the party's rain soak %.3f, exposure %.2f)"), Stage,
		World ? World->GetTimeSeconds() : 0.0, LeadIndex, PartyLooks.IsValidIndex(LeadIndex) ? *PartyLooks[LeadIndex].Name : TEXT("?"), Lead.wetness, Lead.line,
		LeadWeather(), LeadRain.soak, LeadRain.exposure);
	if (AFTExploreDirector* D = GetDirector())
	{
		const TArray<TObjectPtr<AFTPartyFollower>>& Party = D->GetFollowers();
		const ftcore::leadwet::FollowerRoster& Roster = D->GetFollowerRoster();
		for (int32 I = 0; I < Party.Num(); ++I)
		{
			const AFTPartyFollower* Follower = Party[I].Get();
			const int32 Member = Roster.memberOf(I);
			const FString Who = PartyLooks.IsValidIndex(Member) ? PartyLooks[Member].Name : FString(TEXT("?"));
			Line += Follower ? FString::Printf(TEXT("; follower %d member %d %s soak %.2f (line %.2f m) weather %.2f%s"), I, Member, *Who, Follower->GetWetness().wetness,
								   Follower->GetWetness().line, Follower->GetWeather(), Follower->IsHidden() ? TEXT(" hidden") : TEXT(""))
							 : FString::Printf(TEXT("; follower %d none"), I);
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s."), *Line);
}

void AFTExploreCharacter::SwitchForReview()
{
	// The key's path; a switch it refuses is said, never shot as if it had happened. Without water bodies the lead's look
	// ignores the water's store (StirWater draws it dry) while the followers show theirs, so that run proves nothing.
	const UWorld* World = GetWorld();
	const UFTWaterSubsystem* Water = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	if (!Water || !Water->HasBodies())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("SWITCH_REVIEW: this map has no water bodies, so the lead's soak is not drawn; no proof from this run."));
		FTRequestExit(2, TEXT("ExploreSwitchReview"));
		return;
	}
	const int32 Was = LeadIndex;
	LogSwitchReview(TEXT("before the switch"));
	HandleSwitchLead();
	if (LeadIndex == Was)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("SWITCH_REVIEW: the switch was refused (van %d, party screen %d, traversing %d, %d looks); no proof from this run."),
			Van.IsValid() ? 1 : 0, PartyScreen.open ? 1 : 0, bTraversing ? 1 : 0, PartyLooks.Num());
		FTRequestExit(2, TEXT("ExploreSwitchReview"));
		return;
	}
	// The followers are cleared here and spawn again on the next tick: this line has the new lead's soak only; the
	// stills after it log the party.
	LogSwitchReview(TEXT("after the switch, the party not yet back"));
	if (++SwitchReviewDone < SwitchReviewCount && World)
	{
		const float Next = SwitchReviewTimes[SwitchReviewDone];
		GetWorldTimerManager().SetTimer(SwitchReviewTimer, this, &AFTExploreCharacter::SwitchForReview, FMath::Max(Next - static_cast<float>(World->GetTimeSeconds()), 0.01f), false);
		UE_LOG(LogForgottenThird, Display, TEXT("SWITCH_REVIEW switch %d of %d done; the next at %.1f s."), SwitchReviewDone, SwitchReviewCount, Next);
	}
}

void AFTExploreCharacter::SoakForReview()
{
	const float FeetZ = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	int32 Slots = 0;
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(this);
	for (USkeletalMeshComponent* Skin : Meshes)
	{
		for (int32 Slot = 0; Slot < Skin->GetNumMaterials(); ++Slot)
		{
			UMaterialInterface* Material = Skin->GetMaterial(Slot);
			float Unused = 0.f;
			if (!Material || !Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Wetness")), Unused))
			{
				continue;
			}
			UMaterialInstanceDynamic* Dynamic = Skin->CreateAndSetMaterialInstanceDynamic(Slot);
			Dynamic->SetScalarParameterValue(TEXT("Wetness"), WetReview.X);
			Dynamic->SetScalarParameterValue(TEXT("WetLineZ"), FeetZ + WetReview.Y);
			Dynamic->SetScalarParameterValue(TEXT("WetSilt"), WetReview.Z);
			++Slots;
		}
	}
	if (Slots != WetSlots)
	{
		WetSlots = Slots;
		UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: wet review on %d garment slots (wetness %.2f, line %.0f cm above the feet, silt %.2f)."),
			Slots, WetReview.X, WetReview.Y, WetReview.Z);
	}
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
#endif

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
	// -ExploreLookDevGrey[=<albedo>]: a set where a dark garment reads against its surround. A grey wall (albedo 0.35
	// unless given) stands behind the pawn on the far side from the camera, the key sits 45 degrees off the view and
	// follows the camera round (so a back or elbow crop is key-lit too), and the fill is stronger, about 1.5 stops
	// under the key. Without the flag the set is as before.
	float GreyAlbedo = 0.f; // also in the set line logged below, shipped or not
#if UE_BUILD_SHIPPING
	const bool bGrey = false; // a review set: never in a shipped game
#else
	const bool bGrey = FParse::Param(FCommandLine::Get(), TEXT("ExploreLookDevGrey")) || FParse::Value(FCommandLine::Get(), TEXT("ExploreLookDevGrey="), GreyAlbedo);
	if (bGrey)
	{
		if (!FParse::Value(FCommandLine::Get(), TEXT("ExploreLookDevGrey="), GreyAlbedo) || GreyAlbedo <= 0.f)
		{
			GreyAlbedo = 0.35f;
		}
		UStaticMesh* Box = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		if (Box && Base)
		{
			UStaticMeshComponent* Wall = NewObject<UStaticMeshComponent>(this);
			Wall->SetStaticMesh(Box);
			Wall->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Wall->SetupAttachment(Boom); // boom space: the camera is at -X, so +X is behind the pawn as it sees it
			Wall->SetUsingAbsoluteScale(true);
			Wall->SetWorldScale3D(FVector(0.05, 14.0, 10.0));
			Wall->SetRelativeLocation(FVector(380.0, 0.0, 150.0));
			Wall->RegisterComponent();
			UMaterialInstanceDynamic* Grey = UMaterialInstanceDynamic::Create(Base, this);
			Grey->SetVectorParameterValue(TEXT("Color"), FLinearColor(GreyAlbedo, GreyAlbedo, GreyAlbedo));
			Wall->SetMaterial(0, Grey);
		}
	}
#endif
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
	const FLightSpec GreySpecs[] = {
		{Boom, FVector(-220.0, -220.0, 190.0), 40.f, 90.f, FLinearColor(1.f, 0.97f, 0.92f)}, // key, 45 degrees off the view
		{Boom, FVector(-260.0, 240.0, 60.0), 14.f, 240.f, FLinearColor(0.95f, 0.97f, 1.f)},  // fill
	};
	const FVector Target(0.0, 0.0, 20.0);
	const TConstArrayView<FLightSpec> Chosen = bGrey ? TConstArrayView<FLightSpec>(GreySpecs) : TConstArrayView<FLightSpec>(Specs);
	for (const FLightSpec& Spec : Chosen)
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
	UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: look-dev set on (%d actors hidden, key spin %.1f deg/s, grey wall %.2f)."), Hidden, LookDevSpin, GreyAlbedo);
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
	WornLook = Look;
	// New pieces: the wet instances are gathered again the next time the lead is wet.
	WetMaterials.Reset();
	WetSkinDry.Reset();
	bWetMaterialsMade = false;
	WetHairMaterials.Reset();
	WetHairDry.Reset();
	bWetHairMade = false;
	SkinWetEased = 0.f;
	// A switch lands the skin at the member's own soak; the first look, at the start, eases in from dry.
	bSkinWetSnap = ftcore::leadwet::skinSnapOnLook(bLookWorn);
	bLookWorn = true;
#if !UE_BUILD_SHIPPING
	++LooksWorn;
	SkinLogStage = 0;
#endif
	const int32 Cloth = FTLook::AddPieces(*this, *BodyComponent, Loaded.Pieces, Pieces, TEXT("FTExploreCharacter"));
	FTLook::AddGrooms(*this, *BodyComponent, Pieces, Look.Grooms, GroomComponents, TEXT("FTExploreCharacter"));
	// The grooms come on dry; the wet switch swaps them on its next tick if the weather is wet.
	bGroomWet = false;
	WetGroomHold.Reset();
	FTLook::PreloadWetGrooms(Look.Grooms, WetGroomHold, TEXT("FTExploreCharacter"));
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
	// The runner's lookup, no fallback to the first kit: a look with no kit in arena.json is not playable.
	const ftcore::ArenaFighter* Kit = WeaponTable.findFighter(FT::ToUtf8(Look.Name.ToLower()));
	return Kit && ftcore::arenaPlayable(*Kit);
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
		if (bNewUiParty)
		{
			TickPartyScreen(nullptr, DeltaSeconds); // shuts the screen with no pick
		}
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
	// Field only (the pad's stick click is Intercept in a fight). This body takes no input in a fight: the arena host
	// possesses the fighter figure, so here is always outside one.
	constexpr bool bInFight = false;
	const bool bCrouchKey = ftcore::inputActionLive(InputAction::Crouch, bInFight) && FTInputBindings::IsDown(*PC, InputAction::Crouch);
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
	// Under the new UI the party screen takes the map button's hold (and Party's own key); its short press is still the
	// map.
	if (bNewUiParty)
	{
		TickPartyScreen(PC, DeltaSeconds);
		return;
	}
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

void AFTExploreCharacter::TickPartyScreen(const APlayerController* PC, float DeltaSeconds)
{
	using ftcore::InputAction;
	ftcore::UiPartyInput In;
	In.live = PC != nullptr && !FTInputBindings::LostFocus();
	In.explore = true;
	// The rows' truth, the same the screen draws (IsPartnerHere): with no other playable look the partner's row reads
	// NOT HERE and a flick skips it, so a release never picks it. The rows are kept current (FindPartyRows at begin
	// play and after every lead change), so this frame's step sees them before it opens.
	In.partnerHere = IsPartnerHere();
	In.lead = LeadIndex;
	if (In.live)
	{
		In.partyDown = FTInputBindings::IsDown(*PC, InputAction::Party);
		In.mapDown = FTInputBindings::IsDown(*PC, InputAction::Map);
		In.toggle = FTInputBindings::PartyToggles(GetWorld());
		In.stickY = PC->GetInputAnalogKeyState(EKeys::Gamepad_RightY);
		if (FMath::Abs(In.stickY) < 0.1f)
		{
			In.stickY = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY); // either stick works the rows
		}
		float MouseDX = 0.f;
		PC->GetInputMouseDelta(MouseDX, In.mouseY);
		// The screen's own keys: the move keys' forward and back (W and S by default), the arrows and the D-pad move
		// the cursor; Enter, a left click or pad A picks; pad B or Esc (the pause key's OnBack, claimed while it
		// stands) shuts it with no pick.
		const FKey& Fwd = FTInputBindings::Key(InputAction::MoveForward, ftcore::InputDevice::Keys);
		const FKey& Back = FTInputBindings::Key(InputAction::MoveBack, ftcore::InputDevice::Keys);
		In.upDown = (Fwd.IsValid() && PC->IsInputKeyDown(Fwd)) || PC->IsInputKeyDown(EKeys::Up) ||
			PC->IsInputKeyDown(EKeys::Gamepad_DPad_Up);
		In.downDown = (Back.IsValid() && PC->IsInputKeyDown(Back)) || PC->IsInputKeyDown(EKeys::Down) ||
			PC->IsInputKeyDown(EKeys::Gamepad_DPad_Down);
		In.confirmDown = PC->IsInputKeyDown(EKeys::Enter) || PC->IsInputKeyDown(EKeys::LeftMouseButton) ||
			PC->IsInputKeyDown(EKeys::Gamepad_FaceButton_Bottom);
		In.cancelDown = bPartyBack || PC->IsInputKeyDown(EKeys::Gamepad_FaceButton_Right);
		In.tapStays = true; // the keys above and the controls line are explore's
		if (AFTPlayerController* FTPC = Cast<AFTPlayerController>(Controller); FTPC && BackBoundTo.Get() != FTPC)
		{
			FTPC->OnBack.AddUniqueDynamic(this, &AFTExploreCharacter::HandlePartyBack);
			BackBoundTo = FTPC;
		}
	}
	bPartyBack = false;
	const ftcore::UiPartyOut Out = ftcore::uiPartyScreenStep(PartyScreen, In, DeltaSeconds);
	// The confirm key still held after the screen shuts is the screen's, not a light (TickAim reads this).
	bPartyKeysHeld = PartyScreen.open || (bPartyKeysHeld && In.confirmDown);
	if (Out.opened)
	{
		FindPartyRows();
	}
	if (Out.map)
	{
		HandleToggleMap();
	}
	if (!Out.picked())
	{
		return;
	}
	switch (Out.pick)
	{
	case ftcore::UiPartyRow::Partner:
		// Guarded as well: only a partner that is here (a valid other look) is ever sent.
		if (IsPartnerHere())
		{
			SwitchLeadTo(PartnerIndex);
		}
		break;
	case ftcore::UiPartyRow::Follow:
		HandleToggleParty();
		break;
	case ftcore::UiPartyRow::Third:
	case ftcore::UiPartyRow::Lead:
		break;
	}
}

void AFTExploreCharacter::HandlePartyBack()
{
	if (!PartyScreen.open)
	{
		return; // Esc opens the pause menu as ever
	}
	bPartyBack = true;
	if (AFTPlayerController* FTPC = Cast<AFTPlayerController>(Controller))
	{
		FTPC->ClaimBack();
	}
}

void AFTExploreCharacter::FindPartyRows()
{
	PartnerIndex = -1;
	ThirdIndex = -1;
	for (int32 I = 0; I < PartyLooks.Num(); ++I)
	{
		if (I == LeadIndex)
		{
			continue;
		}
		if (IsPlayableLead(PartyLooks[I]))
		{
			PartnerIndex = PartnerIndex < 0 ? I : PartnerIndex;
		}
		else
		{
			ThirdIndex = ThirdIndex < 0 ? I : ThirdIndex;
		}
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
	else if ((bSwimming || bSwimRescue) && !SwimFlagsNow.attack)
	{
		Body = ftcore::FieldBody::Seated; // swimming (ftcore::Swim's flags): no weapon
	}
	else if (GetCharacterMovement()->IsFalling())
	{
		Body = ftcore::FieldBody::Falling;
	}
	// The swim stows the weapons while its flags say so, and hands them back when it ends.
	const bool bStow = (bSwimming || bSwimRescue) && !SwimFlagsNow.weaponsOut;
	if (bStow != bSwimHolstered)
	{
		bSwimHolstered = bStow;
		Weapons.SetHolstered(bStow);
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
	// The mouse buttons count only on foot, with the game taking input: not in the van, a menu, a pause or a scene, nor
	// under the party screen (its click picks; still held after it shuts, it is no light).
	// Aim and Light from the binding table (right and left mouse, LT and RT by default): Aim held or toggled, Light a
	// tap for the light and held for the heavy.
	const APlayerController* PC = Cast<APlayerController>(Controller);
	const bool bInput = TakesInput() && !bPartyKeysHeld;
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
		UFTGcPacingSubsystem::HiddenMoment(this, TEXT("unstick fade"));
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
	LookAction = MakeAction(TEXT("IA_ExploreLook"), true);
	JumpAction = MakeAction(TEXT("IA_ExploreJump"), false);
	RollAction = MakeAction(TEXT("IA_ExploreRoll"), false);
	InteractAction = MakeAction(TEXT("IA_ExploreInteract"), false);
	SwitchLeadAction = MakeAction(TEXT("IA_ExploreSwitchLead"), false);
	PartyAction = MakeAction(TEXT("IA_ExploreParty"), false);
	MapBoundKeys();
	// Run, Crouch, Aim, Light and the map button's party hold are polled (TickHeldKeys, TickAim): held or toggled.
}

void AFTExploreCharacter::MapBoundKeys()
{
	// From scratch each time: the actions stay (their BindActions with them), the keys are mapped again. UnmapAll and
	// MapKey ask Enhanced Input to rebuild the mappings of every player using the context.
	Context->UnmapAll();
	KeyGeneration = FTInputBindings::Generation();
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
	Context->MapKey(LookAction, EKeys::Mouse2D);
	Context->MapKey(LookAction, EKeys::Gamepad_Right2D);
	Both(JumpAction, ftcore::InputAction::Jump);
	Both(RollAction, ftcore::InputAction::Roll);
	Both(InteractAction, ftcore::InputAction::Interact);

	// Under the new UI the pad switches leads on the party screen only, so SwitchLead keeps its keyboard key alone.
	bNewUiParty = FT::NewUiEnabled();
	if (bNewUiParty)
	{
		const FKey& SwitchKey = FTInputBindings::Key(ftcore::InputAction::SwitchLead, ftcore::InputDevice::Keys);
		if (SwitchKey.IsValid())
		{
			Context->MapKey(SwitchLeadAction, SwitchKey);
		}
	}
	else
	{
		Both(SwitchLeadAction, ftcore::InputAction::SwitchLead);
	}

	Both(PartyAction, ftcore::InputAction::Party);
	// Run, Crouch, Aim, Light and the map button's party hold are polled (TickHeldKeys, TickAim): held or toggled; under
	// the new UI so is Party (the party screen, TickPartyScreen).
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
	if (!bNewUiParty)
	{
		Input->BindAction(PartyAction, ETriggerEvent::Started, this, &AFTExploreCharacter::HandleToggleParty);
	}
}

void AFTExploreCharacter::HandleMove(const FInputActionValue& Value)
{
	if (AFTPartyVan* V = Van.Get())
	{
		V->SetDriveAxis(Value.Get<FVector2D>());
		return;
	}
	const FVector2D Axis = Value.Get<FVector2D>();
	if (!Controller || PartyScreen.open) // the party screen takes the move keys and the stick
	{
		return;
	}
	// Under water the stick goes to the free swim (TickSwimDeep), which moves along the camera's look in 3D.
	if (bSwimUnder || bSwimRescue)
	{
		SwimMoveAxis = Axis;
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
	if (bNoLook || PartyScreen.open)
	{
		return; // the party screen holds the camera: the stick and the mouse move its cursor
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
	if (PartyScreen.open)
	{
		return;
	}
	if (AFTPartyVan* V = Van.Get())
	{
		V->SetHandbrake(true);
		return;
	}
	// Crouched, jump stands the body up; standing in front of a low wall or a ledge, it goes over or up; otherwise it jumps.
	// Held in the air, it catches a ledge within reach (Tick).
	bJumpHeld = true;
	// Under water the held jump is the free swim's (its press rises to the surface); nothing else takes it.
	if (bSwimUnder || bSwimRescue)
	{
		return;
	}
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
	if (bSwimming && WaterPose.lockActions)
	{
		return;
	}
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
	if (PartyScreen.open) // pad B: the party screen's cancel
	{
		return;
	}
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

bool AFTExploreCharacter::ProbeObstacle(const ftcore::TraversalSet& Set, bool bAir, ftcore::TraversalObstacle& Out, FVector& In, bool* OutBlocker) const
{
	const UWorld* World = GetWorld();
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	const float Half = Capsule->GetScaledCapsuleHalfHeight();
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(FTTraversal), false, this);
	return World && FTTraversalProbe::Probe(*World, Query, FeetLocation(), GetActorForwardVector().GetSafeNormal2D(),
		Capsule->GetScaledCapsuleRadius(), Half, Set.tuning(), bAir, Out, In, OutBlocker);
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
	if (PartyScreen.open)
	{
		return;
	}
	// In the van, interact held is the way out; on foot near the party van, it is the way in.
	if (AFTPartyVan* V = Van.Get())
	{
		V->SetExitHeld(true);
		return;
	}
	// Swimming, interact only where the swim allows it (its flags); never while a rescue carries the lead.
	if (bSwimRescue || (bSwimming && !SwimFlagsNow.interact))
	{
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
	if (Van.IsValid() || PartyScreen.open)
	{
		return; // on the party screen its cursor picks
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
	// The next look the player may play; a party member that is not playable is passed over, with its reason logged.
	int32 Next = LeadIndex;
	for (int32 Step = 1; Step < PartyLooks.Num(); ++Step)
	{
		const int32 Try = (LeadIndex + Step) % PartyLooks.Num();
		if (IsPlayableLead(PartyLooks[Try]))
		{
			Next = Try;
			break;
		}
		UE_LOG(LogForgottenThird, Log, TEXT("FTExploreCharacter: lead switch passes over %s: a party member only (not playable)."), *PartyLooks[Try].Name);
	}
	if (Next == LeadIndex)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTExploreCharacter: no other playable lead in PartyLooks."));
		return;
	}
	SwitchLeadTo(Next);
}

void AFTExploreCharacter::SwitchLeadTo(int32 Index)
{
	if (Van.IsValid() || bTraversing || !PartyLooks.IsValidIndex(Index) || Index == LeadIndex)
	{
		return;
	}
	// -SwimSwitchKeep: in the water the new lead keeps the swim (the body, its movement and its swim state are the same
	// actor's; only the look changes) and the new partners start at their surface slots; climbing out or rescued, no switch.
	bool bSwitchInWater = false;
	if (bSwimSwitchKeep)
	{
		const ftcore::WaterSwitchStart Start = ftcore::waterSwitchStart(bSwimming, bSwimUnder, bWaterClimbing, bSwimRescue);
		if (Start == ftcore::WaterSwitchStart::Refused)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Water: no lead switch while %s."), bSwimRescue ? TEXT("being rescued") : TEXT("climbing out of the water"));
			return;
		}
		bSwitchInWater = Start == ftcore::WaterSwitchStart::InWater;
	}
	UnCrouch();
	// A swim pose saved for the old look's mesh is its own: put that rest back before the new look is worn, so the new
	// look's rest is saved afresh on the next posed step (never the old look's offset and turn on the new mesh).
	if (bSwimMeshSaved)
	{
		if (USkeletalMeshComponent* Body = GetMesh())
		{
			Body->SetRelativeLocationAndRotation(SwimMeshLoc, SwimMeshRot);
		}
		bSwimMeshSaved = false;
		SwimPitch = 0.f;
	}
	const int32 OldLead = LeadIndex;
	LeadIndex = Index;
	FindPartyRows(); // the old lead takes the partner's row
	const FFTLeadLook& Look = PartyLooks[LeadIndex];
	WearLook(Look);
	if (UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
	{
		GI->ExploreLead = Look.Name;
	}
	if (bSwitchInWater)
	{
		bWaterStartNext = true;
		UE_LOG(LogForgottenThird, Display, TEXT("Water: lead switch to %s while %s: the lead stays in the water, the partners start at their surface slots."),
			*Look.Name, bSwimUnder ? TEXT("under water") : TEXT("swimming"));
	}
	if (AFTExploreDirector* D = GetDirector())
	{
		D->OnLeadChanged(Look.Name, OldLead, LeadIndex);
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
	UWorld* World = GetWorld();
	if (!Director.IsValid() && World && World->GetRealTimeSeconds() - DirectorMissAt >= 1.0)
	{
		for (TActorIterator<AFTExploreDirector> It(World); It; ++It)
		{
			Director = *It;
			break;
		}
		if (!Director.IsValid())
		{
			DirectorMissAt = World->GetRealTimeSeconds();
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
	// The bindings were read again (a changed file, a rebinding): the context's keys follow. Per frame an int compare.
	if (Context && FTInputBindings::Generation() != KeyGeneration)
	{
		MapBoundKeys();
	}
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
	if (bWaterStartSnap)
	{
		// The first frame after a start in the water: the lead's feet on its swim's target as it is now. The put-in's frame
		// is long (the party respawns in it), so this frame steps the water's time and the wave under the lead moves; the
		// surface hold would only ease after it, while the partners, placed in their own tick, are on the new target.
		bWaterStartSnap = false;
		const UFTWaterSubsystem* StartWater = GetWorld() ? GetWorld()->GetSubsystem<UFTWaterSubsystem>() : nullptr;
		UCharacterMovementComponent* StartMove = GetCharacterMovement();
		double StartFeet = 0.0;
		if (StartWater && StartMove && bSwimming && !bSwimUnder && !bWaterClimbing && SurfaceSwimFeet(*StartWater, GetActorLocation(), WaterPose.sink, StartFeet))
		{
			const double StartHalf = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const FVector StartAt = GetActorLocation();
			SetActorLocation(FVector(StartAt.X, StartAt.Y, StartFeet + StartHalf), false, nullptr, ETeleportType::TeleportPhysics);
			StartMove->Velocity.Z = 0.0;
			UE_LOG(LogForgottenThird, Display, TEXT("Water: the first frame after a start in the water: the lead's feet set on the swim's target %.1f cm (they were %.1f; the water's time %.2f s on)."),
				StartFeet, StartAt.Z - StartHalf, StartWater->WaterTime() - WaterStartSnapTime);
		}
	}
#if !UE_BUILD_SHIPPING
	TickSwimProof(DeltaSeconds);
#endif
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
	// Under water (and through a rescue) TickSwimDeep turns the body to its swim.
	// Swimming at the surface WaterLoco holds the facing (the surface hold).
	const UFTLocomotionAnimInstance* Steered = bTraversing || Strike.attacking() || Aim.aiming() || bSwimming || bSwimUnder || bSwimRescue ? nullptr : LocoAnim;
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
	if (LeadGoTry.holding())
	{
		HandleJumpRelease();
	}
	LeadGoTry.reset();
	LeadGoRan = 0.f;
	LeadGoNext.Reset();
	UE_LOG(LogForgottenThird, Display, TEXT("LeadGo: walking from (%.0f, %.0f, %.0f) at (%.0f, %.0f) cm%s."), GetActorLocation().X, GetActorLocation().Y,
		GetActorLocation().Z, To.X, To.Y, bJumpWhenHeld ? TEXT(", jumping when held up") : TEXT(""));
}

void AFTExploreCharacter::LeadGoTryEvents(float Distance)
{
#if UE_BUILD_SHIPPING
	(void)Distance;
#else
	if (LeadGoTry.released())
	{
		HandleJumpRelease();
	}
	if (LeadGoTry.verdict() != ftcore::HeldUpJumpVerdict::None)
	{
		const FVector At = GetActorLocation();
		UE_LOG(LogForgottenThird, Display, TEXT("LeadGo: jump %d, cleared %s (%.0f cm nearer the point, at (%.0f, %.0f, %.0f) cm, water %s; %d not cleared so far)."),
			LeadGoTry.tries(), LeadGoTry.verdict() == ftcore::HeldUpJumpVerdict::Cleared ? TEXT("yes") : TEXT("no"), LeadGoTry.progress(Distance), At.X, At.Y, At.Z,
			ANSI_TO_TCHAR(ftcore::waterModeName(WaterNow.mode)), LeadGoTry.failed());
	}
#endif
}

void AFTExploreCharacter::LogLeadGoHold()
{
#if !UE_BUILD_SHIPPING
	UWorld* World = GetWorld();
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!World || !Capsule || !Move)
	{
		return;
	}
	const FVector Feet = FeetLocation();
	const FVector Ahead = GetActorForwardVector().GetSafeNormal2D();
	const double Reach = Capsule->GetScaledCapsuleRadius() + 80.0;
	// What blocks the capsule: 6 cm balls swept ahead every 10 cm from 10 cm over the feet to 2 m (a row per height,
	// 'x' where one hit). The lowest unbroken run of hits from the bottom is the wall or railing in the way; the highest
	// hit is told too (a post or a lamp can be it). A look down 3 cm past each one's hit finds its top.
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(FTLeadGoHold), false, this);
	const FCollisionResponseParams Responses(Capsule->GetCollisionResponseToChannels());
	const ECollisionChannel Channel = Capsule->GetCollisionObjectType();
	const FCollisionShape Ball = FCollisionShape::MakeSphere(6.f);
	constexpr int32 KRows = 20;
	TCHAR Rows[KRows + 1] = {};
	double RunRow = -1.0;
	FVector RunHit = FVector::ZeroVector;
	bool bRunOver = false;
	double HighRow = -1.0;
	FVector HighHit = FVector::ZeroVector;
	FHitResult Hit;
	for (int32 Row = 0; Row < KRows; ++Row)
	{
		const double Up = 10.0 * (Row + 1);
		const FVector From = Feet + FVector(0.0, 0.0, Up);
		const bool bHit = World->SweepSingleByChannel(Hit, From, From + Ahead * Reach, FQuat::Identity, Channel, Ball, Query, Responses) && !Hit.bStartPenetrating;
		Rows[Row] = bHit ? TEXT('x') : TEXT('.');
		if (bHit)
		{
			HighRow = Up;
			HighHit = Hit.ImpactPoint;
			if (!bRunOver)
			{
				RunRow = Up;
				RunHit = Hit.ImpactPoint;
			}
		}
		else if (RunRow > 0.0)
		{
			bRunOver = true;
		}
	}
	// A row's top over the feet: the look down past its hit, else the row's ball top.
	auto TopOf = [&](double RowUp, const FVector& RowHit)
	{
		const FVector Over = RowHit + Ahead * 3.0;
		FHitResult Down;
		if (World->LineTraceSingleByChannel(Down, FVector(Over.X, Over.Y, Feet.Z + RowUp + 20.0), FVector(Over.X, Over.Y, Feet.Z - 50.0), Channel, Query, Responses)
			&& !Down.bStartPenetrating)
		{
			return Down.ImpactPoint.Z - Feet.Z;
		}
		return RowUp + 6.0;
	};
	const double Top = RunRow > 0.0 ? TopOf(RunRow, RunHit) : -1.0;
	const double HighTop = HighRow > 0.0 ? TopOf(HighRow, HighHit) : -1.0;
	const double TopAhead = RunRow > 0.0 ? FVector::DotProduct(RunHit - Feet, Ahead) : 0.0;
	// The lead's own jump, from its movement's values: the rise of the feet (ftcore::jumpRise).
	const float Gravity = -Move->GetGravityZ();
	const float Rise = ftcore::jumpRise(Move->JumpZVelocity, Gravity, GetJumpMaxHoldTime());
	// And whether the traversal probe sees anything to vault (TryTraverse's own look).
	const TCHAR* Probe = TEXT("no anim");
	const TCHAR* AirProbe = TEXT("no anim");
	ftcore::TraversalObstacle Obstacle;
	FVector In;
	float ProbeHeight = 0.f;
	float ProbeAhead = 0.f;
	if (const UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance()))
	{
		Probe = TEXT("nothing found");
		bool bBlocker = false;
		if (ProbeObstacle(Anim->GetTraversal(), false, Obstacle, In, &bBlocker))
		{
			Probe = TEXT("found");
			ProbeHeight = Obstacle.height;
			ProbeAhead = Obstacle.distance;
		}
		else if (bBlocker)
		{
			Probe = TEXT("turned down at a frontage blocker");
		}
		// The catch from a jump's height (the air probe), for a top out of the ground's reach.
		AirProbe = TEXT("nothing found");
		bool bAirBlocker = false;
		ftcore::TraversalObstacle AirObstacle;
		FVector AirIn;
		if (ProbeObstacle(Anim->GetTraversal(), true, AirObstacle, AirIn, &bAirBlocker))
		{
			AirProbe = TEXT("found");
		}
		else if (bAirBlocker)
		{
			AirProbe = TEXT("turned down at a frontage blocker");
		}
	}
	const FVector At = GetActorLocation();
	UE_LOG(LogForgottenThird, Display,
		TEXT("LeadGo: held up at (%.0f, %.0f, %.0f) cm; jump %d. In the way: lowest run tops at %.0f cm over the feet, %.0f cm ahead; highest hit tops at %.0f cm (rows 10..200 cm: %s); jump rise %.0f cm (JumpZ %.0f, gravity %.0f, hold %.2f s): %s the lowest run, %s the highest; traversal probe %s (%.0f cm top, %.0f cm ahead); air catch %s."),
		At.X, At.Y, At.Z, LeadGoTry.tries(), Top, TopAhead, HighTop, Rows, Rise, Move->JumpZVelocity, Gravity, GetJumpMaxHoldTime(),
		Top < 0.0 ? TEXT("nothing found for") : ftcore::jumpClears(Rise, static_cast<float>(Top)) ? TEXT("CLEARS") : TEXT("does NOT clear"),
		HighTop < 0.0 ? TEXT("nothing found for") : ftcore::jumpClears(Rise, static_cast<float>(HighTop)) ? TEXT("CLEARS") : TEXT("does NOT clear"), Probe, ProbeHeight, ProbeAhead, AirProbe);
#endif
}

void AFTExploreCharacter::TraceLeadGoJump(float DeltaSeconds, bool bPressed)
{
#if UE_BUILD_SHIPPING
	(void)DeltaSeconds;
	(void)bPressed;
#else
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	if (bPressed)
	{
		if (bLeadGoTrace)
		{
			LogLeadGoTrace(TEXT("the next press"));
		}
		bLeadGoTrace = true;
		LeadGoTraceTime = 0.f;
		LeadGoTraceFeet0 = FeetLocation().Z;
		LeadGoTraceTraversals0 = TraversalsBegun;
		LeadGoTraceTry = LeadGoTry.tries();
		LeadGoTraceTravAt = -1.f;
		bLeadGoTraceCatch = false;
		LeadGoTraceClip = -1;
		LeadGoTraceLedge = 0;
		LeadGoTraceModes = 0;
		LeadGoTracePeak = 0.0;
		LeadGoTracePeakAt = 0.f;
		LeadGoTraceSamples = 0;
		LeadGoTraceStoodAt = -1.f;
		LeadGoTraceStoodRise = 0.0;
		LeadGoTraceStoodOn.Reset();
		LeadGoTraceVerdictAt = -1.f;
		return;
	}
	if (!bLeadGoTrace)
	{
		return;
	}
	LeadGoTraceTime += DeltaSeconds;
	const double Rise = FeetLocation().Z - LeadGoTraceFeet0;
	if (Rise > LeadGoTracePeak)
	{
		LeadGoTracePeak = Rise;
		LeadGoTracePeakAt = LeadGoTraceTime;
	}
	while (LeadGoTraceSamples < static_cast<int32>(UE_ARRAY_COUNT(LeadGoTraceRise)) && LeadGoTraceTime >= 0.1f * static_cast<float>(LeadGoTraceSamples + 1))
	{
		LeadGoTraceRise[LeadGoTraceSamples++] = static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Rise), -32000, 32000));
	}
	if (TraversalsBegun != LeadGoTraceTraversals0 && LeadGoTraceTravAt < 0.f)
	{
		LeadGoTraceTravAt = LeadGoTraceTime;
		bLeadGoTraceCatch = LastTraversal.bCatch;
		LeadGoTraceClip = TravPlan.clip;
	}
	LeadGoTraceLedge |= static_cast<uint8>(1u << static_cast<uint32>(LedgeNow.phase));
	if (Move)
	{
		LeadGoTraceModes |= 1u << static_cast<uint32>(Move->MovementMode.GetValue());
		if (LeadGoTraceStoodAt < 0.f && Move->IsMovingOnGround() && !bTraversing && Rise >= 40.0)
		{
			LeadGoTraceStoodAt = LeadGoTraceTime;
			LeadGoTraceStoodRise = Rise;
			const FHitResult& Floor = Move->CurrentFloor.HitResult;
			LeadGoTraceStoodOn = FString::Printf(TEXT("%s/%s, floor normal z %.2f%s"), Floor.GetActor() ? *Floor.GetActor()->GetName() : TEXT("-"),
				Floor.GetComponent() ? *Floor.GetComponent()->GetName() : TEXT("-"), Floor.ImpactNormal.Z, Move->CurrentFloor.bLineTrace ? TEXT(", a perch (line trace)") : TEXT(""));
		}
	}
	if (bSwimming)
	{
		LogLeadGoTrace(TEXT("in the water"));
	}
	else if (LeadGoTraceTime >= 3.f)
	{
		LogLeadGoTrace(TEXT("3 s"));
	}
#endif
}

void AFTExploreCharacter::LogLeadGoTrace(const TCHAR* Why)
{
#if UE_BUILD_SHIPPING
	(void)Why;
#else
	bLeadGoTrace = false;
	FString Rises;
	for (int32 I = 0; I < LeadGoTraceSamples; ++I)
	{
		Rises += FString::Printf(TEXT("%s%d"), I > 0 ? TEXT(" ") : TEXT(""), LeadGoTraceRise[I]);
	}
	FString LedgeSeen;
	for (uint32 P = 1; P < 4; ++P)
	{
		if (LeadGoTraceLedge & (1u << P))
		{
			LedgeSeen += LedgeSeen.IsEmpty() ? TEXT("") : TEXT("+");
			LedgeSeen += P == 1 ? TEXT("hanging") : P == 2 ? TEXT("climbing") : TEXT("dropping");
		}
	}
	FString Modes;
	for (uint32 M = 0; M < 32; ++M)
	{
		if (LeadGoTraceModes & (1u << M))
		{
			Modes += FString::Printf(TEXT("%s%d"), Modes.IsEmpty() ? TEXT("") : TEXT("+"), M);
		}
	}
	const FString Trav = LeadGoTraceTravAt < 0.f ? FString(TEXT("none"))
		: FString::Printf(TEXT("%s, clip %d, at %.2f s"), bLeadGoTraceCatch ? TEXT("a catch in the air") : TEXT("a vault from the ground"), LeadGoTraceClip, LeadGoTraceTravAt);
	const FString Stood = LeadGoTraceStoodAt < 0.f ? FString(TEXT("never 40 cm or more up"))
		: FString::Printf(TEXT("at %.2f s, %.0f cm up, on %s"), LeadGoTraceStoodAt, LeadGoTraceStoodRise, *LeadGoTraceStoodOn);
	UE_LOG(LogForgottenThird, Display,
		TEXT("LeadGo: jump %d traced to %s (%.2f s; judged at %.2f s): traversal %s; ledge %s; movement modes %s; peak rise %.0f cm at %.2f s; stood %s; rise each 0.1 s (cm): %s."),
		LeadGoTraceTry, Why, LeadGoTraceTime, LeadGoTraceVerdictAt, *Trav, LedgeSeen.IsEmpty() ? TEXT("none") : *LedgeSeen, *Modes, LeadGoTracePeak, LeadGoTracePeakAt, *Stood, *Rises);
#endif
}

void AFTExploreCharacter::TickWater(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	UCharacterMovementComponent* Move = GetCharacterMovement();
#if !UE_BUILD_SHIPPING // the scripted walk (captures and checks): not in a shipped game
	if (LeadGoAfter >= 0.f && (LeadGoAfter -= DeltaSeconds) < 0.f)
	{
		BeginLeadGo(LeadGoTo, bLeadGoJump);
	}
	if (LeadRouteNext < LeadRouteCount && World && World->GetTimeSeconds() >= LeadRouteAt[LeadRouteNext])
	{
		UE_LOG(LogForgottenThird, Display, TEXT("LeadGo: route leg %d of %d at %.2f s."), LeadRouteNext + 1, LeadRouteCount, World->GetTimeSeconds());
		BeginLeadGo(LeadRouteTo[LeadRouteNext++], false);
	}
	if (!bLeadCoverSetLogged && World && World->GetTimeSeconds() >= 5.0)
	{
		const UFTRainSubsystem* Rain = World->GetSubsystem<UFTRainSubsystem>();
		bLeadCoverSetLogged = true;
		if (Rain && Rain->IsOn())
		{
			LogLeadCoverSet();
		}
	}
	if (bLeadGo)
	{
		LeadGoRan += DeltaSeconds;
		// A route: the next point once this one is reached (a held-up jump under way cleared it).
		const double Reached = FVector::Dist2D(LeadGoTo, GetActorLocation());
		if (LeadGoNext.Num() > 0 && Reached <= 55.0 && !bWaterClimbing)
		{
			LeadGoTo = LeadGoNext[0];
			LeadGoNext.RemoveAt(0, EAllowShrinking::No);
			LeadGoTry.pointReached();
			LeadGoTryEvents(static_cast<float>(Reached));
			UE_LOG(LogForgottenThird, Display, TEXT("LeadGo: on to (%.0f, %.0f) cm from (%.0f, %.0f, %.0f), water %s."), LeadGoTo.X, LeadGoTo.Y, GetActorLocation().X,
				GetActorLocation().Y, GetActorLocation().Z, ANSI_TO_TCHAR(ftcore::waterModeName(WaterNow.mode)));
		}
		const FVector Way = (LeadGoTo - GetActorLocation()).GetSafeNormal2D();
		const double Dist = FVector::Dist2D(LeadGoTo, GetActorLocation());
		// Under water the free swim takes the route itself (TickSwimDeep).
		const bool bPush = Dist > 30.0 && !bWaterClimbing && !bSwimUnder && !bSwimRescue;
		if (bPush)
		{
			AddMovementInput(Way, LeadGoPace);
		}
		// Held up for a second: jump, held 0.25 s with the push kept on (TryTraverse vaults where it finds a way over),
		// released on a later tick, and the try judged once the body is down again or in the water (ftcore::HeldUpJump).
		if (bLeadGoJump)
		{
			const bool bBusy = Move->IsFalling() || bTraversing || bWaterClimbing || LedgeNow.phase != ftcore::LedgePhase::None;
			const bool bInWater = bSwimming || WaterNow.mode == ftcore::WaterMode::Swim;
			LeadGoTry.tick(DeltaSeconds, bPush, static_cast<float>(GetVelocity().Size2D()), static_cast<float>(Dist), bBusy, bInWater);
			if (LeadGoTry.pressed())
			{
				SetActorRotation(FRotator(0.0, Way.Rotation().Yaw, 0.0));
				LogLeadGoHold();
				TraceLeadGoJump(0.f, true); // before the jump: the press's feet, the traversals so far
				HandleJump();
			}
			else
			{
				TraceLeadGoJump(DeltaSeconds, false);
			}
			if (bLeadGoTrace && LeadGoTraceVerdictAt < 0.f && LeadGoTry.verdict() != ftcore::HeldUpJumpVerdict::None)
			{
				LeadGoTraceVerdictAt = LeadGoTraceTime;
			}
			LeadGoTryEvents(static_cast<float>(Dist));
		}
		if (LeadGoQuit > 0.f && LeadGoRan >= LeadGoQuit)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("LeadGo: ends at (%.0f, %.0f, %.0f) cm, feet %.0f, water %s."), GetActorLocation().X, GetActorLocation().Y,
				GetActorLocation().Z, GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), ANSI_TO_TCHAR(ftcore::waterModeName(WaterNow.mode)));
			LeadGoQuit = 0.f;
			FTRequestExit(0, TEXT("LeadGo"));
		}
	}
#endif
	// Taken out of play (a fight's figure stands in and the body is hidden, a scene holds the lead, no controller): the
	// water waits. Nothing steps the swim, its breath, a climb out or a rescue (each picks up where it stopped), and
	// nothing forces the flying mode on a parked body; a body that was swimming stops where it is (flying braking is 0
	// under water, so a kept velocity would carry it through the whole scene).
	// The rain soaks the party in a played scene too, and with no controller; a hidden body (a fight's figure stands in)
	// waits. Shown here while parked; StirWater shows it in play.
	if (!IsHidden())
	{
		StepLeadRain(DeltaSeconds);
	}
	if (!Controller || IsHidden() || ScenePlaying())
	{
		bWaterPartyParked = true; // the party's plan skips its step too; its next one is told (Core's resumed)
		if (!IsHidden())
		{
			ShowLeadWetness(DeltaSeconds);
		}
		if (bSwimming || bWaterClimbing || bSwimRescue)
		{
			Move->Velocity = FVector::ZeroVector;
		}
		return;
	}
	// The entry reads the fall as it was before this tick set the flying mode; every path below steps WaterLoco once
	// (the surface hold steps it before it moves the body, so it reads this tick's pose).
	bWaterTopFalling = Move->IsFalling();
	WaterTopVelocity = Move->Velocity;
	ON_SCOPE_EXIT
	{
		StepWaterLoco(DeltaSeconds);
	};
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
			// Out on the top: WaterLoco reads dry ground this tick (the water state is found again on the next), not the
			// swim the climb began in.
			WaterNow = ftcore::WaterState{};
			Move->SetMovementMode(MOVE_Walking);
			UE_LOG(LogForgottenThird, Display, TEXT("Water: climbed out onto (%.0f, %.0f, %.0f) cm."), At.X, At.Y, At.Z);
		}
		StirWater(DeltaSeconds);
		return;
	}
	// Out of breath and vitality under water: carried to the way out, a placed move.
	if (bSwimRescue)
	{
		TickSwimRescue(DeltaSeconds);
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
	// Set down out of the water column while dived (a fight or a scene ended on dry ground, or on a pier inside a body's
	// footprint): the swim ends this frame, its state and pose reset below, its vitality kept. Out of the column: inside a
	// body with the feet at or over its surface (the depth test), or outside every body and every dive area.
	const bool bFeetDry = WaterNow.body >= 0 && WaterNow.immersion <= 0.f;
	if (bSwimUnder && !bOut &&
		(bFeetDry || (WaterNow.body < 0 && System->Bodies().dive(FTSwimUnits::ToPlan(GetActorLocation())).body < 0)))
	{
		bSwimUnder = false;
	}
	// Under water the lead swims on whatever the column under it (ftcore::Swim never ejects it).
	const bool bSwimNow = (WaterNow.mode == ftcore::WaterMode::Swim || (bSwimUnder && !bOut)) && !bTraversing;
	if (bSwimNow && !bSwimming)
	{
		BeginSurfaceSwim(*Move);
		const UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance());
		if (!bSwimSaid && (!Anim || Anim->NumWaterClips() == 0))
		{
			bSwimSaid = true;
			UE_LOG(LogForgottenThird, Display, TEXT("Water: stand-in clips: no swim clip is loaded for this body, so the swim plays the locomotion's own idle (treading) and "
				"walk (the stroke) held upright at the surface, and the wade plays the walk slowed; the climb out is a placed move with no clip."));
		}
	}
	else if (!bSwimNow && bSwimming)
	{
		bSwimming = false;
		bSwimUnder = false;
		Move->MaxFlySpeed = SwimSavedFly;
		Move->BrakingDecelerationFlying = SwimSavedBrake;
		if (Move->MovementMode == MOVE_Flying)
		{
			Move->SetMovementMode(MOVE_Falling);
		}
		PoseSwim(0.f, 1.f); // upright at once (a full step of the ease)
		// Taken out from under water (a scene, the van): at the surface again, its vitality kept.
		if (SwimBody.state() != ftcore::SwimState::Surface && SwimBody.state() != ftcore::SwimState::Rescued)
		{
			const float Vitality = SwimBody.vitality();
			SwimBody.reset();
			SwimBody.setVitality(Vitality);
		}
	}
	// At a ladder, pushing at its wall: up it (the jump at any other wall is the fallback). Not from under water.
	if (!bSwimUnder && WaterNow.mode != ftcore::WaterMode::Dry && TryLadderClimb(false))
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
		// Dived (or diving, or rising): the free swim moves the body. At the surface it only watches for a dive.
		if (System && TickSwimDeep(*System, DeltaSeconds))
		{
			StirWater(DeltaSeconds);
			return;
		}
		// Held at the surface: the feet eased to the swimmer's draft, never thrown up out of the water.
		const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		// WaterLoco's step for this tick: its planar velocity and facing move the body, its sink takes an entry under the
		// draft and back (the movement's own braking off: the velocity is WaterLoco's).
		StepWaterLoco(DeltaSeconds);
		const ftcore::WaterLocoTuning& Loco = WaterLocoBody.tuning();
		Move->MaxFlySpeed = FMath::Max(ftcore::kWaterSwimSpeed, Loco.swimSprintSpeed) * 100.f;
		Move->BrakingDecelerationFlying = 0.f;
		Move->Velocity.X = WaterPose.velocity.x * 100.0;
		Move->Velocity.Y = WaterPose.velocity.y * 100.0;
		SetActorRotation(FRotator(0.0, WaterPose.yaw, 0.0));
		// ... nor pressed into a bed that rises under it (a slip, a canal's slope): there the feet skim the bed, so the
		// body comes up the slope with its stroke until the water is shallow enough to stand in.
		const double Bed = (WaterNow.surface - WaterNow.column) * 100.0;
		const double Target = FMath::Max(ftcore::waterSwimFeetZ(WaterNow.surface, System->WaterTime()) * 100.0 + WaterPose.sink * 100.0, Bed + 8.0) + Half;
		// A splash's sink goes down at the fall's pace (a 1.5 m sink starts near 16 m/s); the hold's own ease is gentler.
		Move->Velocity.Z = FMath::Clamp((Target - GetActorLocation().Z) * 5.0, WaterPose.sink < 0.f ? -1600.0 : -250.0, 120.0);
		const FVector Push(WaterNow.flow.x * 100.0 * DeltaSeconds, WaterNow.flow.y * 100.0 * DeltaSeconds, 0.0);
		if (!Push.IsNearlyZero())
		{
			AddActorWorldOffset(Push, true);
		}
	}
	StirWater(DeltaSeconds);
}

void AFTExploreCharacter::LoadWaterLoco(UFTLocomotionAnimInstance* Anim)
{
	if (!bWaterLocoTried)
	{
		bWaterLocoTried = true;
		WaterLocoBody.reset(SharedWaterLocoTuning());
		const ftcore::WaterLocoTuning& T = WaterLocoBody.tuning();
		UE_LOG(LogForgottenThird, Display, TEXT("WaterLoco: %s tuning: tread %.1f m/s, swim %.1f m/s (sprint %.1f)."),
			WaterLocoBody.tuningOk() ? TEXT("the") : TEXT("refused, the built-in"), T.treadSpeed, T.swimSpeed, T.swimSprintSpeed);
	}
	// The swim clips: the lead body's own (/Game/Characters/<Name>/Swim/A_<clip>), looked up again for a new lead or a
	// new anim instance (SetupLocomotion makes one per look). A clip that does not fit the body is left out.
	if (!Anim || (WaterClipsFor == LeadIndex && WaterClipsAnim.Get() == Anim))
	{
		return;
	}
	WaterClipsFor = LeadIndex;
	WaterClipsAnim = Anim;
	const FString Name = PartyLooks.IsValidIndex(LeadIndex) && !PartyLooks[LeadIndex].Name.IsEmpty() ? PartyLooks[LeadIndex].Name : FString(TEXT("Devin"));
	LoadWaterClips(*Anim, GetMesh()->GetSkeletalMeshAsset(), Name, TEXT("WaterLoco"));
	bWaterUnderClips = Anim->HasWaterClip(ftcore::WaterClip::UnderIdle) && Anim->HasWaterClip(ftcore::WaterClip::UnderSlow);
}

const ftcore::WaterLocoTuning& AFTExploreCharacter::SharedWaterLocoTuning()
{
	static bool bRead = false;
	static ftcore::WaterLocoTuning Tuning;
	if (!bRead)
	{
		bRead = true;
		TArray<uint8> Bytes;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Swim/water_loco.json");
		std::string Error;
		std::vector<std::string> Notes;
		if (IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File)
			&& ftcore::importWaterLocoTuning(FT::BytesAsText(Bytes), Tuning, &Error, &Notes))
		{
			for (const std::string& Note : Notes)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("WaterLoco: %s"), *FT::ToFString(Note));
			}
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("WaterLoco: no usable Data/Swim/water_loco.json; the built-in tuning.\n%s"), *FT::ToFString(Error));
			Tuning = ftcore::WaterLocoTuning{};
		}
	}
	return Tuning;
}

int32 AFTExploreCharacter::LoadWaterClips(UFTLocomotionAnimInstance& Anim, const USkeletalMesh* BodyAsset, const FString& Name, const TCHAR* LogName)
{
	FString From = Name;
	int32 Found = 0;
	for (int32 Pass = 0; Pass < 2 && Found == 0; ++Pass)
	{
		// The body's own clips; with none, Devin's (one MetaHuman body skeleton: FitsBody still checks each).
		if (Pass == 1)
		{
			if (Name == TEXT("Devin"))
			{
				break;
			}
			From = TEXT("Devin");
		}
		for (int32 Index = 1; Index < ftcore::kWaterClipCount; ++Index)
		{
			const ftcore::WaterClip Which = static_cast<ftcore::WaterClip>(Index);
			const FString Asset = FString::Printf(TEXT("A_%s"), ANSI_TO_TCHAR(ftcore::waterClipName(Which)));
			const FString Package = FString::Printf(TEXT("/Game/Characters/%s/Swim/%s"), *From, *Asset);
			UAnimSequence* Clip = nullptr;
			if (BodyAsset && FPackageName::DoesPackageExist(Package))
			{
				Clip = LoadObject<UAnimSequence>(nullptr, *(Package + TEXT(".") + Asset), nullptr, LOAD_NoWarn | LOAD_Quiet);
				if (Clip && !UFTLocomotionAnimInstance::FitsBody(*Clip, *BodyAsset, *Name))
				{
					Clip = nullptr;
				}
			}
			Anim.SetWaterClip(Which, Clip);
			Found += Clip ? 1 : 0;
		}
	}
	const bool bUnder = Anim.HasWaterClip(ftcore::WaterClip::UnderIdle) && Anim.HasWaterClip(ftcore::WaterClip::UnderSlow);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %d of %d swim clips for %s in /Game/Characters/%s/Swim%s."), LogName, Found, ftcore::kWaterClipCount - 1, *Name, *From,
		Found == 0 ? TEXT(" (none: the stand-in plays)") : bUnder ? TEXT("") : TEXT(" (no prone under-water clips: the body is pitched)"));
	return Found;
}

const ftcore::WaterFollowOrder* AFTExploreCharacter::GetWaterOrder(int32 Slot) const
{
	if (bNoFollowerSwim || Slot < 0 || Slot >= NumWaterFollowers)
	{
		return nullptr;
	}
	return &WaterParty.order(Slot);
}

void AFTExploreCharacter::BeginSurfaceSwim(UCharacterMovementComponent& Move)
{
	if (bSwimming)
	{
		return;
	}
	bSwimming = true;
	if (bIsCrouched)
	{
		UnCrouch();
	}
	SwimSavedFly = Move.MaxFlySpeed;
	SwimSavedBrake = Move.BrakingDecelerationFlying;
	Move.MaxFlySpeed = ftcore::kWaterSwimSpeed * 100.f;
	Move.BrakingDecelerationFlying = 350.f;
	Move.SetMovementMode(MOVE_Flying);
}

bool AFTExploreCharacter::SurfaceSwimFeet(const UFTWaterSubsystem& System, const FVector& At, float Sink, double& OutFeet, float* OutColumn,
	const AActor* AlsoIgnore) const
{
	using namespace FTSwimUnits;
	float Surface = 0.f;
	if (!System.Bodies().surfaceHeight(ToPlan(At), System.WaterTime(), Surface))
	{
		return false;
	}
	// The bed under the place, from just under the surface (a body at the surface is not the bed).
	const UWorld* World = GetWorld();
	const FVector Top(At.X, At.Y, ToCm(Surface) - 5.0);
	FHitResult Bed;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(FTSurfaceSwimFeet), false, this);
	if (AlsoIgnore)
	{
		Query.AddIgnoredActor(AlsoIgnore);
	}
	const bool bBed = World && World->LineTraceSingleByChannel(Bed, Top, Top - FVector(0.0, 0.0, 3000.0), ECC_Visibility, Query);
	const float BedZ = bBed ? ToMetres(Bed.ImpactPoint.Z) : Surface - 30.f;
	if (OutColumn)
	{
		*OutColumn = Surface - BedZ;
	}
	OutFeet = ToCm(ftcore::waterSurfaceFeetZ(ftcore::waterSwimFeetZ(Surface, System.WaterTime()), Sink, BedZ));
	return true;
}

bool AFTExploreCharacter::StartInWater(const FVector& At, double Yaw)
{
	UWorld* World = GetWorld();
	const UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!System || !System->HasBodies() || !Move || Van.IsValid() || bTraversing || bWaterClimbing || bSwimRescue || bSwimUnder)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Water: a start in the water at (%.0f, %.0f) cm refused (no water on the map, the van, a traversal, a climb out, a rescue or a dive)."),
			At.X, At.Y);
		return false;
	}
	double Feet = 0.0;
	float Column = 0.f;
	if (!SurfaceSwimFeet(*System, At, 0.f, Feet, &Column) || Column < ftcore::kWaterSwimDepth)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Water: a start in the water at (%.0f, %.0f) cm refused: the column there is %.2f m (a swim needs over %.2f m)."),
			At.X, At.Y, Column, ftcore::kWaterSwimDepth);
		return false;
	}
	if (bIsCrouched)
	{
		UnCrouch();
	}
	// Still at the surface, at the swim's own height: WaterLoco reads no fall into the water (no splash, no sink).
	Move->StopMovementImmediately();
	WaterTopVelocity = FVector::ZeroVector;
	bWaterTopFalling = false;
	const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	SetActorLocationAndRotation(FVector(At.X, At.Y, Feet + Half), FRotator(0.0, Yaw, 0.0), false, nullptr, ETeleportType::TeleportPhysics);
	BeginSurfaceSwim(*Move);
	bWaterStartNext = true;
	static const bool bNoStartSnap = FParse::Param(FCommandLine::Get(), TEXT("SwimStartNoSnap"));
	bWaterStartSnap = !bNoStartSnap;
	WaterStartSnapTime = System->WaterTime();
	UE_LOG(LogForgottenThird, Display, TEXT("Water: a start in the water at (%.0f, %.0f) cm, feet %.1f cm over a %.1f m column: the lead at the surface, the partners at their surface slots."),
		At.X, At.Y, Feet, Column);
	if (AFTExploreDirector* D = GetDirector())
	{
		D->RespawnFollowers(*this);
	}
	return true;
}

void AFTExploreCharacter::OnFollowersSpawned()
{
	// The party's clips, held from the first setup on: a switch's respawn then finds them loaded and their measures kept.
	static const bool bNoHold = FParse::Param(FCommandLine::Get(), TEXT("NoPartyHold"));
	if (!bNoHold)
	{
		const int32 HeldBefore = PartyClipHold.Num();
		if (const UFTLocomotionAnimInstance* LeadAnim = GetMesh() ? Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance()) : nullptr)
		{
			LeadAnim->AppendClips(PartyClipHold);
		}
		if (const AFTExploreDirector* HoldDirector = GetDirector())
		{
			for (const TObjectPtr<AFTPartyFollower>& Member : HoldDirector->GetFollowers())
			{
				const USkeletalMeshComponent* MemberBody = Member ? Member->GetBody() : nullptr;
				if (const UFTLocomotionAnimInstance* MemberAnim = MemberBody ? Cast<UFTLocomotionAnimInstance>(MemberBody->GetAnimInstance()) : nullptr)
				{
					MemberAnim->AppendClips(PartyClipHold);
				}
			}
		}
		if (PartyClipHold.Num() > HeldBefore)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Party hold: %d clips held (%d new)."), PartyClipHold.Num(), PartyClipHold.Num() - HeldBefore);
		}
	}
	if (!bLevelStartPartyLogged)
	{
		bLevelStartPartyLogged = true;
		FTLook::LogLevelStart(bNoHold ? TEXT("party set up, no clip hold") : TEXT("party set up, clips held"));
	}
	// A start in the water (a lead switch while swimming, StartInWater): the new partners' plan starts in the water before
	// the director's first placement, so that placement puts them at their surface slots (the rejoin), not on land.
	if (bWaterStartNext)
	{
		StepWaterParty(0.f);
	}
}

void AFTExploreCharacter::StepWaterParty(float DeltaSeconds)
{
	using namespace FTSwimUnits;
	if (!bNoFollowerSwimRead)
	{
		bNoFollowerSwimRead = true;
		bNoFollowerSwim = FParse::Param(FCommandLine::Get(), TEXT("LocoNoFollowerSwim"));
	}
	UWorld* World = GetWorld();
	const UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	if (bNoFollowerSwim || !System || !System->HasBodies())
	{
		NumWaterFollowers = 0;
		bWaterStartNext = false;
		return;
	}
	// Who follows: the director's followers of this lead, in its order (a member keeps its slot and its side). A new
	// set (a lead switch respawns them, a member joins or leaves) starts the plan again: every member ashore.
	AFTPartyFollower* Found[ftcore::kWaterFollowMax] = {};
	int32 Count = 0;
	if (AFTExploreDirector* Dir = GetDirector())
	{
		for (AFTPartyFollower* Follower : Dir->GetFollowers())
		{
			if (Follower && Follower->GetWaterLead() == this && Count < ftcore::kWaterFollowMax)
			{
				Found[Count++] = Follower;
			}
		}
	}
	// A start in the water is only for a lead that still swims (or is under).
	if (!bSwimming && !bSwimUnder)
	{
		bWaterStartNext = false;
	}
	bool bSame = Count == NumWaterFollowers;
	for (int32 Slot = 0; bSame && Slot < Count; ++Slot)
	{
		bSame = WaterFollowers[Slot].Get() == Found[Slot];
	}
	if (!bSame)
	{
		// A start in the water (a lead switch while swimming, StartInWater): the new set starts Placed and is put at its
		// surface slots (the rejoin) by the placement that follows, not ashore.
		const bool bStartIn = bWaterStartNext && Count > 0;
		for (int32 Slot = 0; Slot < ftcore::kWaterFollowMax; ++Slot)
		{
			WaterFollowers[Slot] = Slot < Count ? Found[Slot] : nullptr;
			if (Slot < Count)
			{
				Found[Slot]->SetWaterSlot(Slot);
				if (bStartIn)
				{
					Found[Slot]->BeginInWater();
				}
			}
		}
		if (bStartIn)
		{
			WaterParty.startInWater();
			UE_LOG(LogForgottenThird, Display, TEXT("WaterFollow: %d partners start in the water with the lead (a lead switch while swimming, or a start in the sea)."), Count);
		}
		else
		{
			WaterParty.reset();
			UE_LOG(LogForgottenThird, Display, TEXT("WaterFollow: %d partners follow the lead into the water (the plan starts again)."), Count);
		}
		NumWaterFollowers = Count;
	}
	if (Count > 0)
	{
		bWaterStartNext = false;
	}
	if (NumWaterFollowers == 0)
	{
		return;
	}
	ftcore::WaterFollowMember Members[ftcore::kWaterFollowMax];
	for (int32 Slot = 0; Slot < NumWaterFollowers; ++Slot)
	{
		if (const AFTPartyFollower* Follower = WaterFollowers[Slot].Get())
		{
			Follower->FillWaterMember(Members[Slot]);
		}
	}
	ftcore::WaterFollowLead Lead;
	const ftcore::WaterLocoState State = WaterPose.state;
	Lead.swimming = State != ftcore::WaterLocoState::Dry && State != ftcore::WaterLocoState::Wade;
	// A new swim: the exit an earlier climb out used is not this one's.
	if (Lead.swimming && !bWaterPartyLeadSwam)
	{
		WaterLeadExit = -1;
	}
	bWaterPartyLeadSwam = Lead.swimming;
	Lead.swim = SwimBody.state();
	const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	Lead.position = RefFromActor(GetActorLocation(), Half);
	const FVector Vel = GetVelocity();
	Lead.velocity = {static_cast<float>(Vel.X * 0.01), static_cast<float>(Vel.Y * 0.01)};
	Lead.yaw = static_cast<float>(GetActorRotation().Yaw);
	Lead.sprint = bRunOn;
	Lead.exiting = bWaterClimbing;
	Lead.exitIndex = WaterLeadExit;
	Lead.out = !Lead.swimming && !bWaterClimbing;
	Lead.camera = Camera ? ToSwim(Camera->GetComponentLocation()) : Lead.position;
	// Back from a park (a scene, a hidden or uncontrolled lead skips the plan's step): Core places swimmers if it is dry.
	Lead.resumed = bWaterPartyParked;
	bWaterPartyParked = false;
	const ftcore::WaterBodies& Bodies = System->Bodies();
	const std::size_t Exits = Bodies.exitCount();
	WaterParty.step(Lead, Members, NumWaterFollowers, Exits > 0 ? &Bodies.exit(0) : nullptr, Exits, DeltaSeconds, &Bodies);
	for (int32 Slot = 0; Slot < NumWaterFollowers; ++Slot)
	{
		const ftcore::WaterFollowOrder& Order = WaterParty.order(Slot);
		if (Order.changed)
		{
			const AFTPartyFollower* Follower = WaterFollowers[Slot].Get();
			UE_LOG(LogForgottenThird, Display, TEXT("WaterFollow: %s to phase %d (entry %d, exit %d) at (%.0f, %.0f, %.0f) cm."),
				Follower ? *Follower->GetName() : TEXT("(gone)"), static_cast<int32>(Order.phase), static_cast<int32>(Order.entry.kind), Order.exitIndex,
				Follower ? Follower->GetActorLocation().X : 0.0, Follower ? Follower->GetActorLocation().Y : 0.0, Follower ? Follower->GetActorLocation().Z : 0.0);
		}
	}
}

void AFTExploreCharacter::StepWaterLoco(float DeltaSeconds)
{
	using namespace FTSwimUnits;
	if (WaterLocoFrame == GFrameCounter)
	{
		return;
	}
	WaterLocoFrame = GFrameCounter;
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(GetMesh()->GetAnimInstance());
	LoadWaterLoco(Anim);
	ftcore::WaterLocoInput In;
	In.water = WaterNow;
	In.swim = SwimBody.state();
	// Swim's dived and surfaced are its step's: only on the frame TickSwimDeep stepped it.
	const ftcore::SwimStep& Last = SwimBody.last();
	const bool bSwimFresh = SwimSteppedFrame == GFrameCounter;
	In.dived = bSwimFresh && Last.dived;
	In.surfaced = bSwimFresh && Last.surfaced;
	In.breathShare = Last.breathShare;
	In.swimVelocity = Last.velocity;
	In.move = {static_cast<float>(WaterMoveWanted.X), static_cast<float>(WaterMoveWanted.Y)};
	WaterMoveWanted = FVector2D::ZeroVector;
	In.sprint = bRunOn;
	In.yaw = static_cast<float>(GetActorRotation().Yaw);
	In.velocity = ToSwim(WaterTopVelocity);
	In.falling = bWaterTopFalling;
	if (bWaterClimbing)
	{
		const float T = FMath::Clamp(WaterClimbTime, 0.f, 1.f);
		if (bWaterClimbLadder)
		{
			// Up the rungs, then over the top once TickWater's move across begins (half way).
			In.exitMove = T < 0.5f ? ftcore::WaterExitMove::Ladder : ftcore::WaterExitMove::LadderTop;
			In.ladderClimbed = ToMetres(GetActorLocation().Z - WaterClimbFrom.Z);
			In.exitProgress = T < 0.5f ? 0.f : (T - 0.5f) * 2.f;
		}
		else
		{
			In.exitMove = ftcore::WaterExitMove::Mantle;
			In.exitProgress = T;
		}
		// The hands reach for the top's edge, a little in from the wall's face toward the place the body ends.
		const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector Across = (WaterClimbTo - WaterClimbFrom).GetSafeNormal2D();
		In.exitHand = ToSwim(FVector(WaterClimbFrom.X, WaterClimbFrom.Y, WaterClimbTo.Z - Half) + Across * 35.0);
	}
	WaterPose = WaterLocoBody.step(In, DeltaSeconds);
	const ftcore::WaterLocoPose& Pose = WaterPose;
	if (Anim)
	{
		// WaterLoco's yaw is the engine's (its plan frame is the engine's, in metres), so its lean has the yaw rate's
		// sign, as the locomotion's lean has.
		Anim->SetWater(Pose.waterWeight, Pose.layers, Pose.layerCount, Pose.lean);
	}
	if (Pose.changed)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("WaterLoco: %s to %s at (%.0f, %.0f, %.0f) cm, %.1f m/s, sink %.2f m, cues 0x%x."),
			ANSI_TO_TCHAR(ftcore::waterLocoStateName(Pose.from)), ANSI_TO_TCHAR(ftcore::waterLocoStateName(Pose.state)), GetActorLocation().X,
			GetActorLocation().Y, GetActorLocation().Z, Pose.speed, Pose.sink, Pose.cue);
	}
	if (!Pose.inputOk)
	{
		UE_LOG(LogForgottenThird, Verbose, TEXT("WaterLoco: an input value was replaced at (%.0f, %.0f, %.0f) cm."), GetActorLocation().X, GetActorLocation().Y,
			GetActorLocation().Z);
	}
	StepWaterParty(DeltaSeconds);
}

void AFTExploreCharacter::LoadSwimTuning()
{
	if (bSwimTuningTried)
	{
		return;
	}
	bSwimTuningTried = true;
	TArray<uint8> Bytes;
	const FString File = FPaths::ProjectDir() / TEXT("Data/Swim/swim.json");
	ftcore::SwimTuning Tuning;
	std::string Error;
	std::vector<std::string> Notes;
	if (IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File)
		&& ftcore::importSwimTuning(FT::BytesAsText(Bytes), Tuning, &Error, &Notes))
	{
		SwimBody.reset(FTSwimUnits::TuningForRef(Tuning));
		for (const std::string& Note : Notes)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Swim: %s"), *FT::ToFString(Note));
		}
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Swim: no usable Data/Swim/swim.json; the built-in tuning.\n%s"), *FT::ToFString(Error));
		SwimBody.reset(FTSwimUnits::TuningForRef(ftcore::SwimTuning{}));
	}
	const ftcore::SwimTuning& T = SwimBody.tuning();
	UE_LOG(LogForgottenThird, Display, TEXT("Swim: %s tuning: %.1f m/s (sprint %.1f), breath %.0f s (warning at %.0f s), dive %.1f m, edge push %.1f m/s over %.0f m."),
		SwimBody.tuningOk() ? TEXT("the") : TEXT("refused, the built-in"), T.swimSpeed, T.sprintSpeed, T.breathSeconds, T.warningSeconds,
		T.diveDepth, T.edgePushSpeed, T.edgeRampMetres);
}

bool AFTExploreCharacter::TickSwimDeep(const UFTWaterSubsystem& System, float DeltaSeconds)
{
	using namespace FTSwimUnits;
	LoadSwimTuning();
	UWorld* World = GetWorld();
	UCharacterMovementComponent* Move = GetCharacterMovement();
	const FVector At = GetActorLocation();
	const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const ftcore::WaterBodies& Water = System.Bodies();
	const ftcore::Vec2 P = ToPlan(At);
	// Only a playable lead dives (the Third never does); found again when the lead changes.
	if (SwimLeadChecked != LeadIndex)
	{
		SwimLeadChecked = LeadIndex;
		bSwimLeadDives = PartyLooks.IsValidIndex(LeadIndex) && IsPlayableLead(PartyLooks[LeadIndex]);
	}
	const ftcore::WaterDive Dive = Water.dive(P);
	const int BodyIndex = Dive.body >= 0 ? Dive.body : WaterNow.body;
	if (BodyIndex < 0)
	{
		bSwimUnder = false;
		return false;
	}

	// The water at the lead (m): the surface over it, the bed under it (the ground a trace down finds, else the body's
	// own bed), the dive area and its outer edge.
	ftcore::SwimEnv Env;
	Env.position = RefFromActor(At, Half);
	Env.surfaceZ = Water.surfaceOf(static_cast<std::size_t>(BodyIndex), P, System.WaterTime());
	float BedZ = Water.body(static_cast<std::size_t>(BodyIndex)).bedAt(P);
	if (World)
	{
		FHitResult Bed;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(FTSwimBed), false, this);
		if (World->LineTraceSingleByChannel(Bed, At, At - FVector(0.0, 0.0, Half + 1500.0), ECC_Visibility, Query))
		{
			BedZ = ToMetres(Bed.ImpactPoint.Z);
		}
	}
	Env.bedZ = FMath::Min(BedZ, Env.surfaceZ - 0.01f);
	Env.column = Env.surfaceZ - Env.bedZ;
	Env.deepAllowed = Dive.allowed && bSwimLeadDives;
	Env.airPocket = false; // the strip's basin has none
	Env.edgeDistance = Dive.edgeDistance;
	Env.edgeInward = {Dive.edgeInward.x, Dive.edgeInward.y, 0.f};

	// What the player asks: the camera's look, the stick (under water HandleMove keeps it here), sprint, and the held
	// crouch and jump (Swim takes their presses).
	ftcore::SwimInput In;
	const FRotator View = Controller ? Controller->GetControlRotation() : GetActorRotation();
	In.look = DirectionToSwim(View.Vector());
	In.forward = static_cast<float>(SwimMoveAxis.Y);
	In.right = static_cast<float>(SwimMoveAxis.X);
	In.sprint = bRunOn;
	// The crouch key's real state, and only while the player steers: TickHeldKeys latches bCrouchKeyWas true while input
	// is taken (a menu, a scene, a fight), which Swim would read as a dive press.
	In.crouch = TakesInput() && bCrouchKeyWas;
	In.jump = bJumpHeld;
	// An entry's splash and recovery, a climb out or a rescue (WaterLoco's lock): no dive and no rise.
	if (WaterPose.lockActions)
	{
		In.crouch = false;
		In.jump = false;
	}
	SwimMoveAxis = FVector2D::ZeroVector;
#if !UE_BUILD_SHIPPING // -LeadGoDive: the scripted walk dives once, swims its route under water and rises at its end
	if (bLeadGo && bLeadGoDive)
	{
		const ftcore::SwimState Now = SwimBody.state();
		// The presses: down every other frame until taken (Swim's buttons are edge-triggered, and the held key's state
		// is not known here).
		const bool bPressFrame = (GFrameCounter & 1) != 0;
		if (!bLeadGoDived && Now == ftcore::SwimState::Surface && SwimBody.last().canDive)
		{
			In.crouch = bPressFrame;
		}
		else if (Now == ftcore::SwimState::Under || Now == ftcore::SwimState::Diving)
		{
			// Along the route, held about 2.5 m under the surface (or over the bed where it is shallower).
			const double Flat = FVector::Dist2D(LeadGoTo, At);
			const double CruiseZ = ToCm(FMath::Max(Env.surfaceZ - 2.5f, Env.bedZ + 1.f)) - (ToCm(ftcore::kWaterSwimDraft) - Half);
			const FVector Way = FVector(LeadGoTo.X - At.X, LeadGoTo.Y - At.Y, (CruiseZ - At.Z) * 2.0).GetSafeNormal();
			if (!Way.IsNearlyZero())
			{
				In.look = DirectionToSwim(Way);
			}
			In.forward = Flat > 30.0 ? LeadGoPace : 0.f;
			In.right = 0.f;
			if (!bLeadGoRose && LeadGoNext.Num() == 0 && Flat <= 55.0 && Now == ftcore::SwimState::Under)
			{
				In.jump = bPressFrame;
			}
		}
	}
#endif

	const bool bWasUnder = bSwimUnder;
	// A hitch: Swim clamps its step to kSwimMaxStep and drops the rest, so the movement below carries only that step too.
	const ftcore::SwimStep& S = SwimBody.step(In, Env, FMath::Min(DeltaSeconds, ftcore::kSwimMaxStep));
	SwimSteppedFrame = GFrameCounter;
#if !UE_BUILD_SHIPPING
	bLeadGoDived = bLeadGoDived || S.dived;
	bLeadGoRose = bLeadGoRose || S.state == ftcore::SwimState::Surfacing;
#endif
	SwimFlagsNow = S.flags;
	bSwimUnder = S.state == ftcore::SwimState::Diving || S.state == ftcore::SwimState::Under || S.state == ftcore::SwimState::Surfacing;
	if (S.dived)
	{
		bSwimWarned = false;
		UE_LOG(LogForgottenThird, Display, TEXT("Swim: dived at (%.0f, %.0f, %.0f) cm, column %.1f m, %.0f m inside the outer edge, breath %.0f s."), At.X, At.Y,
			At.Z, Env.column, FMath::Min(Env.edgeDistance, 9999.f), S.breath);
	}
	if (S.diveRefused)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Swim: no dive at (%.0f, %.0f) cm: %s."), At.X, At.Y,
			!bSwimLeadDives ? TEXT("this lead does not dive") : !Dive.allowed ? TEXT("not in a dive area") : TEXT("the water is too shallow"));
	}
	if (S.warning && !bSwimWarned)
	{
		bSwimWarned = true;
		UE_LOG(LogForgottenThird, Display, TEXT("Swim: breath warning, %.1f s left at (%.0f, %.0f, %.0f) cm."), S.breath, At.X, At.Y, At.Z);
	}
	if (S.surfaced)
	{
		bSwimWarned = false;
		UE_LOG(LogForgottenThird, Display, TEXT("Swim: surfaced at (%.0f, %.0f) cm, breath %.0f%%, vitality %.0f%%."), At.X, At.Y, S.breathShare * 100.f,
			S.vitality * 100.f);
	}
	if (!S.envOk || !S.inputOk)
	{
		UE_LOG(LogForgottenThird, Verbose, TEXT("Swim: a value was replaced (env %s, input %s) at (%.0f, %.0f, %.0f) cm."), S.envOk ? TEXT("ok") : TEXT("bad"),
			S.inputOk ? TEXT("ok") : TEXT("bad"), At.X, At.Y, At.Z);
	}
	if (S.rescue)
	{
		bSwimUnder = false;
		BeginSwimRescue(System);
		return true;
	}

	if (!bSwimUnder)
	{
		// At the surface (or back at it): the surface hold's speed and braking again, the body upright.
		if (bWasUnder)
		{
			Move->MaxFlySpeed = ftcore::kWaterSwimSpeed * 100.f;
			Move->BrakingDecelerationFlying = 350.f;
		}
		PoseSwim(0.f, DeltaSeconds);
		return false;
	}

	// Under water: the movement carries the body at the swim's velocity (its sweep stops it at a wall or the bed), with
	// room for a sprint plus the edge's push and no braking of its own.
	const ftcore::SwimTuning& T = SwimBody.tuning();
	Move->MaxFlySpeed = (T.sprintSpeed + T.edgePushSpeed) * 100.f;
	Move->BrakingDecelerationFlying = 0.f;
	const FVector Velocity = PositionValid(S) ? VelocityToEngine(S.velocity) : FVector::ZeroVector;
	// The movement integrates the whole frame: over a hitch the velocity is scaled so the frame moves only Swim's step
	// (its surface, bed and edge clamps hold).
	Move->Velocity = DeltaSeconds > S.dt && DeltaSeconds > 0.f ? Velocity * (S.dt / DeltaSeconds) : Velocity;
	// The body turns to its way across, and pitches along it: head first along the travel, upright (a slight lean) in
	// a glide or a rise with no stroke.
	const double Across = Velocity.Size2D();
	if (Across > 20.0 && DeltaSeconds > 0.f)
	{
		const FRotator Was = GetActorRotation();
		const float Yaw = FMath::FixedTurn(static_cast<float>(Was.Yaw), static_cast<float>(Velocity.Rotation().Yaw), 270.f * DeltaSeconds);
		SetActorRotation(FRotator(0.0, Yaw, 0.0));
	}
	const bool bStroke = Velocity.Size() > 30.0 && (In.forward != 0.f || In.right != 0.f || S.state != ftcore::SwimState::Under);
	const float Elevation = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Velocity.Z, FMath::Max(Across, 1.0))));
	// The swim clips lie prone and pitch the body themselves (UnderSlow, PitchUp, PitchDown): the mesh stays upright.
	// Without them the upright stand-in is laid along the travel.
	PoseSwim(bWaterUnderClips ? 0.f : bStroke ? FMath::Clamp(-(90.f - Elevation), -170.f, 0.f) : -15.f, DeltaSeconds);
	return true;
}

void AFTExploreCharacter::PoseSwim(float TargetPitch, float DeltaSeconds)
{
	USkeletalMeshComponent* Body = GetMesh();
	if (!Body)
	{
		return;
	}
	if (!bSwimMeshSaved)
	{
		if (FMath::IsNearlyZero(TargetPitch))
		{
			return; // upright and not posed: nothing to do
		}
		SwimMeshLoc = Body->GetRelativeLocation();
		SwimMeshRot = Body->GetRelativeRotation();
		bSwimMeshSaved = true;
		SwimPitch = 0.f;
	}
	// The dive's and the surfacing's 0.4 s blend: an ease of rate 5.
	SwimPitch = FMath::FInterpTo(SwimPitch, TargetPitch, DeltaSeconds, 5.f);
	if (FMath::IsNearlyZero(TargetPitch) && FMath::Abs(SwimPitch) < 0.5f)
	{
		Body->SetRelativeLocationAndRotation(SwimMeshLoc, SwimMeshRot);
		bSwimMeshSaved = false;
		SwimPitch = 0.f;
		return;
	}
	// About the capsule's centre (the actor's origin), on the actor's own right axis: the mesh's rest offset and its
	// facing turned together.
	const FQuat Pitch = FRotator(SwimPitch, 0.0, 0.0).Quaternion();
	Body->SetRelativeLocationAndRotation(Pitch.RotateVector(SwimMeshLoc), Pitch * SwimMeshRot.Quaternion());
}

void AFTExploreCharacter::TickSwimProof(float DeltaSeconds)
{
#if UE_BUILD_SHIPPING
	(void)DeltaSeconds;
#else
	if (SwimProofStage == -1)
	{
		if (!FParse::Value(FCommandLine::Get(), TEXT("SwimProof="), SwimProofKind) || SwimProofKind.IsEmpty())
		{
			SwimProofStage = -2; // off for good
			return;
		}
		FParse::Value(FCommandLine::Get(), TEXT("SwimProofShots="), SwimProofShots);
		SwimProofStage = 0;
		FTLook::LogLevelStart(TEXT("proof's first tick"));
	}
	if (SwimProofStage < 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	const UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	SwimProofTime += DeltaSeconds;
	if (SwimProofStage == 0)
	{
		if (SwimProofTime < 3.f || !System || !System->HasBodies())
		{
			if (SwimProofTime > 20.f)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("SwimProof: no water bodies on this map; quitting."));
				FTRequestExit(1, TEXT("SwimProof"));
				SwimProofStage = -2;
			}
			return;
		}
		// The nearest swimmable point (rings 4 m apart, 24 bearings), then back toward the start to its dry edge.
		const ftcore::WaterBodies& Bodies = System->Bodies();
		const FVector Start = GetActorLocation();
		auto Depth = [&Bodies](const FVector& P) { return Bodies.depth(ftcore::Vec2{static_cast<float>(P.X / 100.0), static_cast<float>(P.Y / 100.0)}); };
		// Each candidate is checked before it is taken (11-18 ruling, item 2), and ftcore::judgeWaterSpot decides:
		// - the ground under its dry edge;
		// - a look up from there (a roof or a ceiling within 4 m: the edge is indoors or covered; both the body's channel
		//   and Visibility);
		// - the lead's capsule swept from the dry edge to over the "in" point at walking height lifted by the step height
		//   (a wall, a glass front, a railing);
		// - and swept from the "in" point to the far point at swimming height (a pier, a pile, a wall in the water).
		// The watchdog's spot must be clear all the way. The land run keeps a railing on the way in (its held-up jumps are
		// what it measures, item 4) but takes no roof and nothing in the water. Should no spot pass, the watchdog falls back
		// to the nearest one blocked only on the way in, said loudly (its held-up jumps and the harness placement handle it).
		const FCollisionQueryParams Params(SCENE_QUERY_STAT(SwimProof), false, this);
		const UCapsuleComponent* BodyCapsule = GetCapsuleComponent();
		const double HalfCm = BodyCapsule->GetScaledCapsuleHalfHeight();
		const FCollisionShape BodyShape = FCollisionShape::MakeCapsule(BodyCapsule->GetScaledCapsuleRadius(), static_cast<float>(HalfCm));
		const ECollisionChannel BodyChannel = BodyCapsule->GetCollisionObjectType();
		const FCollisionResponseParams BodyResponses(BodyCapsule->GetCollisionResponseToChannels());
		FCollisionQueryParams SpotQuery(SCENE_QUERY_STAT(SwimProofSpot), false, this);
		for (int32 I = 0; I < NumWaterFollowers && I < ftcore::kWaterFollowMax; ++I)
		{
			if (const AFTPartyFollower* Partner = WaterFollowers[I].Get())
			{
				SpotQuery.AddIgnoredActor(Partner); // the party gathering round the start is not the way
			}
		}
		const double StepUp = GetCharacterMovement()->MaxStepHeight;
		ftcore::WaterSpotRules SpotRules;
		SpotRules.rejectDryToIn = SwimProofKind != TEXT("land");
		ftcore::WaterSpotRules KeepWayIn = SpotRules;
		KeepWayIn.rejectDryToIn = false;
		ftcore::WaterSpotTally Tally;
		int32 Logged = 0;
		bool bFallback = false;
		FVector FallDry = FVector::ZeroVector, FallWet = FVector::ZeroVector, FallFar = FVector::ZeroVector;
		FString FallWhat;
		auto HitName = [](const FHitResult& H)
		{
			return FString::Printf(TEXT("%s/%s"), H.GetActor() ? *H.GetActor()->GetName() : TEXT("-"), H.GetComponent() ? *H.GetComponent()->GetName() : TEXT("-"));
		};
		// Checks the spot (dry edge Dry at the start's height, "in" point P, far point Far): fills the dry edge's actor
		// height in Dry, and what blocked it (by leg) in Why.
		auto CheckSpot = [&](FVector& Dry, const FVector& P, const FVector& Far, FString& Why)
		{
			ftcore::WaterSpotHit Hits[4];
			int32 Count = 0;
			FString UpWhat, InWhat, OutWhat;
			FHitResult Ground;
			const bool bGround = World->LineTraceSingleByChannel(Ground, Dry + FVector(0.0, 0.0, 400.0), Dry - FVector(0.0, 0.0, 3000.0), ECC_Visibility, Params);
			if (bGround)
			{
				const double GroundZ = Ground.ImpactPoint.Z;
				Dry.Z = GroundZ + HalfCm + 2.0;
				// Up from 0.5 m over the ground to 4.5 m (a hit at 4 m or under is a roof).
				const FVector UpFrom(Dry.X, Dry.Y, GroundZ + 50.0);
				const FVector UpTo(Dry.X, Dry.Y, GroundZ + 450.0);
				FHitResult Up;
				if (World->LineTraceSingleByChannel(Up, UpFrom, UpTo, BodyChannel, SpotQuery, BodyResponses) && Up.bBlockingHit)
				{
					Hits[Count++] = {ftcore::WaterSpotLeg::Up, static_cast<float>((Up.ImpactPoint.Z - GroundZ) / 100.0), true};
					UpWhat = HitName(Up);
				}
				if (World->LineTraceSingleByChannel(Up, UpFrom, UpTo, ECC_Visibility, SpotQuery) && Up.bBlockingHit)
				{
					Hits[Count++] = {ftcore::WaterSpotLeg::Up, static_cast<float>((Up.ImpactPoint.Z - GroundZ) / 100.0), true};
					UpWhat = UpWhat.IsEmpty() ? HitName(Up) : UpWhat;
				}
				// The way in: the standing capsule lifted by the step height (a kerb is stepped over, a railing is not).
				const double WalkZ = Dry.Z + StepUp;
				FHitResult In;
				if (World->SweepSingleByChannel(In, FVector(Dry.X, Dry.Y, WalkZ), FVector(P.X, P.Y, WalkZ), FQuat::Identity, BodyChannel, BodyShape, SpotQuery, BodyResponses)
					&& In.bBlockingHit)
				{
					Hits[Count++] = {ftcore::WaterSpotLeg::DryToIn, In.bStartPenetrating ? 0.f : static_cast<float>(In.Distance / 100.0), true};
					InWhat = HitName(In);
				}
			}
			// Out in the water: the capsule at the height the swim (and the harness placement) puts it.
			if (FVector::Dist2D(P, Far) > 1.0)
			{
				const ftcore::Vec2 InAt = FTSwimUnits::ToPlan(P);
				float Level = 0.f;
				if (System->Bodies().surfaceHeight(InAt, System->WaterTime(), Level))
				{
					const double SwimZ = FTSwimUnits::ActorFromRef({InAt.x, InAt.y, Level}, HalfCm).Z;
					FHitResult Out;
					if (World->SweepSingleByChannel(Out, FVector(P.X, P.Y, SwimZ), FVector(Far.X, Far.Y, SwimZ), FQuat::Identity, BodyChannel, BodyShape, SpotQuery, BodyResponses)
						&& Out.bBlockingHit)
					{
						Hits[Count++] = {ftcore::WaterSpotLeg::InToFar, Out.bStartPenetrating ? 0.f : static_cast<float>(Out.Distance / 100.0), true};
						OutWhat = HitName(Out);
					}
				}
			}
			const ftcore::WaterSpotVerdict V = ftcore::judgeWaterSpot(Hits, static_cast<size_t>(Count), bGround, SpotRules);
			Why = V.reason == ftcore::WaterSpotReason::Roofed ? UpWhat
				: V.reason == ftcore::WaterSpotReason::DryToInBlocked || (V.accepted && V.dryToInBlocked) ? InWhat
				: V.reason == ftcore::WaterSpotReason::InToFarBlocked ? OutWhat : FString();
			if (!V.accepted && V.reason == ftcore::WaterSpotReason::DryToInBlocked
				&& ftcore::judgeWaterSpot(Hits, static_cast<size_t>(Count), bGround, KeepWayIn).accepted && !bFallback)
			{
				bFallback = true;
				FallDry = Dry;
				FallWet = P;
				FallFar = Far;
				FallWhat = FString::Printf(TEXT("%s at %.2f m"), *InWhat, V.dryToInAt);
			}
			return V;
		};
		bool bFound = false;
		for (int32 Ring = 1; Ring <= 100 && !bFound; ++Ring)
		{
			for (int32 Bear = 0; Bear < 24 && !bFound; ++Bear)
			{
				const double A = Bear * (2.0 * UE_DOUBLE_PI / 24.0);
				const FVector Dir(FMath::Cos(A), FMath::Sin(A), 0.0);
				const FVector P = Start + Dir * (Ring * 400.0);
				if (Depth(P) < ftcore::kWaterSwimDepth + 0.8f)
				{
					continue;
				}
				FVector Q = P;
				for (int32 Back = 0; Back < 200 && Depth(Q) > 0.f; ++Back)
				{
					Q -= Dir * 50.0;
				}
				if (Depth(Q) > 0.f)
				{
					continue;
				}
				FVector Far = P;
				for (int32 Out = 1; Out <= 30 && Depth(P + Dir * (Out * 100.0)) >= ftcore::kWaterSwimDepth + 0.5f; ++Out)
				{
					Far = P + Dir * (Out * 100.0);
				}
				// The watchdog's run needs the lead well past stuckFar (12 m) from a partner treading by the entry.
				if ((SwimProofKind == TEXT("watchdog") || SwimProofKind == TEXT("boom") || SwimProofKind == TEXT("exit") || SwimProofKind == TEXT("switch")
					|| SwimProofKind == TEXT("start") || SwimProofKind == TEXT("cost")) && FVector::Dist2D(P, Far) < 1600.0)
				{
					continue;
				}
				// Down from a little over the start's height (a quay or a street, not a roof).
				FVector Dry(Q.X - Dir.X * 300.0, Q.Y - Dir.Y * 300.0, Start.Z);
				FString Why;
				const ftcore::WaterSpotVerdict V = CheckSpot(Dry, P, Far, Why);
				Tally.add(V);
				if (!V.accepted)
				{
					if (Logged++ < 16)
					{
						UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: spot rejected, %s%s%s: dry (%.0f, %.0f, %.0f), in (%.0f, %.0f), far (%.0f, %.0f)."), *SwimProofKind,
							ANSI_TO_TCHAR(ftcore::waterSpotReasonName(V.reason)), V.at >= 0.f ? *FString::Printf(TEXT(" at %.2f m"), V.at) : TEXT(""),
							Why.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Why), Dry.X, Dry.Y, Dry.Z, P.X, P.Y, Far.X, Far.Y);
					}
					continue;
				}
				if (V.dryToInBlocked)
				{
					UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: spot kept with a blocker on the way in at %.2f m (%s): the held-up jumps meet it."), *SwimProofKind,
						V.dryToInAt, *Why);
				}
				SwimProofDry = Dry;
				SwimProofWet = P;
				SwimProofFar = Far;
				bFound = true;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: water spots checked %d: ok %d, no ground %d, roofed %d, blocked on the way in %d, blocked in the water %d."),
			*SwimProofKind, Tally.total(), Tally.of(ftcore::WaterSpotReason::Ok), Tally.of(ftcore::WaterSpotReason::NoGround), Tally.of(ftcore::WaterSpotReason::Roofed),
			Tally.of(ftcore::WaterSpotReason::DryToInBlocked), Tally.of(ftcore::WaterSpotReason::InToFarBlocked));
		if (!bFound && bFallback)
		{
			SwimProofDry = FallDry;
			SwimProofWet = FallWet;
			SwimProofFar = FallFar;
			bFound = true;
			bSwimProofSpotFallback = true;
			UE_LOG(LogForgottenThird, Warning, TEXT("SWIMPROOF: WATER SPOT FALLBACK: no spot clear all the way; taking the nearest blocked only on the way in (%s)."), *FallWhat);
		}
		if (!bFound)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("SwimProof: no swimmable water within 400 m (16 m of open water for the watchdog) that passes the spot check; quitting."));
			FTRequestExit(1, TEXT("SwimProof"));
			SwimProofStage = -2;
			return;
		}
		const FVector2D Way(SwimProofWet.X - SwimProofDry.X, SwimProofWet.Y - SwimProofDry.Y);
		SetActorLocationAndRotation(SwimProofDry, FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(Way.Y, Way.X)), 0.0), false, nullptr, ETeleportType::TeleportPhysics);
		if (Controller)
		{
			Controller->SetControlRotation(FRotator(-12.0, GetActorRotation().Yaw, 0.0));
		}
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: dry edge (%.0f, %.0f, %.0f), in at (%.0f, %.0f), far (%.0f, %.0f) cm, %.0f m out."), *SwimProofKind,
			SwimProofDry.X, SwimProofDry.Y, SwimProofDry.Z, SwimProofWet.X, SwimProofWet.Y, SwimProofFar.X, SwimProofFar.Y, FVector::Dist2D(SwimProofWet, SwimProofFar) / 100.0);
		SwimProofStage = 1;
		SwimProofTime = 0.f;
		return;
	}
	if (SwimProofStage == 10)
	{
		// -SwimProof=cost: only its windows from here (as little harness work inside them as can be).
		TickSwimProofCost(DeltaSeconds);
		return;
	}
	// The party gathers at the edge: at least 6 s, and slot 0 within 10 m of the lead (else it may stay ashore with no
	// edge, and neither path can start); 30 s at most, then on with a warning.
	const AFTPartyFollower* Slot0Body = WaterFollowers[0].Get();
	const bool bSlot0Near = Slot0Body && FVector::Dist2D(Slot0Body->GetActorLocation(), GetActorLocation()) <= 1000.0;
	if (SwimProofStage == 1 && SwimProofTime >= 6.f && (bSlot0Near || SwimProofTime >= 30.f))
	{
		if (!bSlot0Near)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("SwimProof %s: slot 0 %s after 30 s; going on."), *SwimProofKind,
				Slot0Body ? *FString::Printf(TEXT("is %.0f m away"), FVector::Dist2D(Slot0Body->GetActorLocation(), GetActorLocation()) / 100.0) : TEXT("has no member"));
		}
		if (SwimProofKind == TEXT("cost"))
		{
			// Item 4 of 11-18, a measurement only. One fixed camera on the perpendicular bisector of the land spot (the dry
			// edge, where the party gathered) and the swim spot (the far point): both at the same distance and angle, the
			// camera over the water where it can be, 5 m above the edge, looking at their midpoint. Far enough back (25 m)
			// that the director's lens hide (a follower 20% nearer the camera than the lead) keeps all three drawn. The party
			// idle on land, then treading at the far point (StartInWater); each window's frame, game-thread, render-thread
			// and GPU ms (TickSwimProofCost).
			const FVector2D CostSpan(SwimProofFar.X - SwimProofDry.X, SwimProofFar.Y - SwimProofDry.Y);
			const FVector2D CostAlong = CostSpan.GetSafeNormal();
			const FVector2D CostSide(-CostAlong.Y, CostAlong.X);
			const FVector CostMid((SwimProofDry.X + SwimProofFar.X) * 0.5, (SwimProofDry.Y + SwimProofFar.Y) * 0.5, SwimProofDry.Z);
			const double CostBack = FMath::Max(2500.0, CostSpan.Size() * 0.9);
			const FVector CostA(CostMid.X + CostSide.X * CostBack, CostMid.Y + CostSide.Y * CostBack, SwimProofDry.Z + 500.0);
			const FVector CostB(CostMid.X - CostSide.X * CostBack, CostMid.Y - CostSide.Y * CostBack, SwimProofDry.Z + 500.0);
			float CostSurface = 0.f;
			const bool bCostA = System && System->Bodies().surfaceHeight(FTSwimUnits::ToPlan(CostA), System->WaterTime(), CostSurface);
			const bool bCostB = System && System->Bodies().surfaceHeight(FTSwimUnits::ToPlan(CostB), System->WaterTime(), CostSurface);
			const FVector CostFrom = (bCostA || !bCostB) ? CostA : CostB;
			FActorSpawnParameters CostParams;
			CostParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ACameraActor* CostCam = World ? World->SpawnActor<ACameraActor>(CostFrom, (CostMid - CostFrom).Rotation(), CostParams) : nullptr;
			APlayerController* CostPc = Cast<APlayerController>(Controller);
			if (!CostCam || !CostPc)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("SwimProof cost: no fixed camera (spawn %d, player controller %d); no figures from this run."),
					CostCam ? 1 : 0, CostPc ? 1 : 0);
				FTRequestExit(2, TEXT("SwimProof"));
				SwimProofStage = -2;
				return;
			}
			CostPc->SetViewTarget(CostCam);
			SwimProofCostCam = CostCam;
			for (int32 I = 0; I < 2; ++I)
			{
				SwimProofCostStats[I] = MakeUnique<ftcore::FrameStats>(static_cast<std::size_t>(kSwimProofCostCap)); // once, before any window
			}
			SwimProofStage = 10;
			SwimProofCostStage = 0;
			SwimProofCostTime = 0.f;
			bSwimProofCostShot = false;
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof cost: fixed camera at (%.0f, %.0f, %.0f) cm (%s side, %s), %.0f m from the land spot (%.0f, %.0f) and the swim spot (%.0f, %.0f); land window first."),
				CostFrom.X, CostFrom.Y, CostFrom.Z, CostFrom == CostA ? TEXT("A") : TEXT("B"), (CostFrom == CostA ? bCostA : bCostB) ? TEXT("over water") : TEXT("over land"),
				FVector::Dist2D(CostFrom, SwimProofDry) / 100.0, SwimProofDry.X, SwimProofDry.Y, SwimProofFar.X, SwimProofFar.Y);
			return;
		}
		if (SwimProofKind == TEXT("start"))
		{
			// Item 5 of 11-18: no walk in. The lead is put in the sea at the far point (a scene or hold that starts in the
			// water) and its partners respawned at their surface slots; judged from the next frame (TickSwimProofSwitch).
			for (int32 I = 0; I < 2; ++I)
			{
				SwimProofSwitchOld[I] = WaterFollowers[I];
			}
			const FVector2D StartWay(SwimProofWet.X - SwimProofDry.X, SwimProofWet.Y - SwimProofDry.Y);
			const double StartYaw = FMath::RadiansToDegrees(FMath::Atan2(StartWay.Y, StartWay.X));
			SwimProofSwitchClock = FPlatformTime::Seconds();
			if (!StartInWater(SwimProofFar, StartYaw))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("SwimProof start: the start in the water at the far point was refused; no proof from this run."));
				FTRequestExit(2, TEXT("SwimProof"));
				SwimProofStage = -2;
				return;
			}
			if (Controller)
			{
				Controller->SetControlRotation(FRotator(-12.0, StartYaw, 0.0));
			}
			SwimProofStage = 9;
			SwimProofSwitchStage = 2;
			SwimProofTime = 0.f;
			SwimProofSwitchAt = 0.f;
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof start: the lead put in the sea at (%.0f, %.0f) cm with its partners; judged for 3 s from the next frame."),
				SwimProofFar.X, SwimProofFar.Y);
			return;
		}
		// The scripted walk (LeadGo) takes the route: in, then out to the far point, where it treads. A held-up jump counts
		// as cleared only when it ends in the water or at the "in" point (no progress counts: a catch or a hop onto a lip
		// that leaves the lead dry is not cleared).
		ftcore::HeldUpJumpRules OnlyWater;
		OnlyWater.minProgress = 1.0e6f;
		LeadGoTry.setRules(OnlyWater);
		BeginLeadGo(FVector(SwimProofWet.X, SwimProofWet.Y, 0.0), true); // jumps a quay's railing
		LeadGoNext.Add(FVector(SwimProofFar.X, SwimProofFar.Y, 0.0));
		SwimProofStage = 2;
		SwimProofTime = 0.f;
	}
	if (SwimProofStage == 9)
	{
		// -SwimProof=start: only its judge from here.
		if (System)
		{
			TickSwimProofSwitch(*System, DeltaSeconds);
		}
		return;
	}
	if (SwimProofStage >= 2)
	{
		// Three held-up jumps that didn't clear (a quay's railing), or three tries and still not in at 45 s: the harness puts
		// the lead in the water at the "in" point, at the surface, and the walk goes on from there. It waits for the body to
		// be down (not in the air, a traversal, a hang or a climb), then checks it swims within 3 s (once more if not).
		// Said loudly, and the run's lines say "lead placed" from then on, so the run can't be read as a walk-in (the 11-18
		// ruling's option 3).
		const bool bLeadBusy = GetCharacterMovement()->IsFalling() || bTraversing || bWaterClimbing || LedgeNow.phase != ftcore::LedgePhase::None;
		if (SwimProofStage == 2 && !bSwimProofLeadPlaced && SwimProofPlaceDue < 0.f && (LeadGoTry.failed() >= 3 || (LeadGoTry.tries() >= 3 && SwimProofTime >= 45.f)))
		{
			SwimProofPlaceDue = SwimProofTime;
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: lead placement due at %.1f s (%d jumps, %d not cleared; %s)."), *SwimProofKind, SwimProofTime,
				LeadGoTry.tries(), LeadGoTry.failed(), LeadGoTry.failed() >= 3 ? TEXT("3 not cleared") : TEXT("backstop: 3 tries and not in at 45 s"));
		}
		const bool bPlaceAgain = bSwimProofLeadPlaced && !bSwimProofLeadSwims && SwimProofPlaceCount < 2 && SwimProofTime >= SwimProofPlacedAt + 3.f;
		if (bSwimProofLeadPlaced && !bSwimProofLeadSwims)
		{
			if (bSwimming)
			{
				bSwimProofLeadSwims = true;
				UE_LOG(LogForgottenThird, Display, TEXT("SWIMPROOF: placed lead swimming %.1f s after the placement."), SwimProofTime - SwimProofPlacedAt);
			}
			else if (SwimProofPlaceCount >= 2 && SwimProofTime >= SwimProofPlacedAt + 3.f && SwimProofPlacedAt >= 0.f)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("SWIMPROOF: placed lead NOT swimming 3 s after the second placement (water %s); no proof from this run."),
					ANSI_TO_TCHAR(ftcore::waterModeName(WaterNow.mode)));
				SwimProofPlacedAt = -1.f; // said once
			}
		}
		if (SwimProofPlaceDue >= 0.f && System && ((SwimProofStage == 2 && !bSwimProofLeadPlaced && !bLeadBusy) || bPlaceAgain))
		{
			if (LeadGoTry.holding())
			{
				HandleJumpRelease();
			}
			const double HalfCm = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const ftcore::Vec2 InAt = FTSwimUnits::ToPlan(SwimProofWet);
			float Level = WaterNow.surface;
			System->Bodies().surfaceHeight(InAt, System->WaterTime(), Level);
			const FVector Put(SwimProofWet.X, SwimProofWet.Y, FTSwimUnits::ActorFromRef({InAt.x, InAt.y, Level}, HalfCm).Z);
			GetCharacterMovement()->StopMovementImmediately();
			SetActorLocation(Put, false, nullptr, ETeleportType::TeleportPhysics);
			bSwimProofLeadPlaced = true;
			SwimProofPlacedAt = SwimProofTime;
			++SwimProofPlaceCount;
			UE_LOG(LogForgottenThird, Warning, TEXT("SWIMPROOF: LEAD PLACED BY HARNESS after %d jumps at (%.0f, %.0f, %.0f) (%d of them not cleared; placement %d, %.1f s after it was due; %s run at %.1f s)."),
				LeadGoTry.tries(), Put.X, Put.Y, Put.Z, LeadGoTry.failed(), SwimProofPlaceCount, SwimProofTime - SwimProofPlaceDue, *SwimProofKind, SwimProofTime);
		}
		else if (SwimProofStage == 2 && SwimProofPlaceDue >= 0.f && !bSwimProofLeadPlaced && SwimProofTime >= SwimProofPlaceDue + 10.f && SwimProofPlacedAt > -2.f)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("SWIMPROOF: LEAD NOT PLACED: still busy 10 s after it was due (falling %d, traversing %d, climbing out %d, ledge phase %d); no proof from this run."),
				GetCharacterMovement()->IsFalling() ? 1 : 0, bTraversing ? 1 : 0, bWaterClimbing ? 1 : 0, static_cast<int32>(LedgeNow.phase));
			SwimProofPlacedAt = -2.f; // said once
		}
		const FVector To = SwimProofStage == 2 ? SwimProofWet : SwimProofFar;
		if (SwimProofStage < 4 && FVector::Dist2D(To, GetActorLocation()) <= 100.0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: stage %d reached at %.1f s (swimming %d)."), *SwimProofKind, SwimProofStage, SwimProofTime, bSwimming ? 1 : 0);
			++SwimProofStage;
			if (SwimProofStage == 4)
			{
				SwimProofHoldAt = SwimProofTime;
			}
		}
		// Slot 0's phase changes, with the counts it had the step before (which path placed it: two given-up entries, or
		// the watchdog).
		if (const ftcore::WaterFollowOrder* Slot0 = GetWaterOrder(0))
		{
			const int32 Now = static_cast<int32>(Slot0->phase);
			if (Now != SwimProofSlotPhase)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: slot 0 phase %d -> %d at %.1f s (rejoin %d; before it: stuck %.1f s, failed entries %d)."), *SwimProofKind,
					SwimProofSlotPhase, Now, SwimProofTime, Slot0->rejoin ? 1 : 0, SwimProofSlotStuck, SwimProofSlotFails);
				// Each kind passes only on its own path into Placed (WaterFollow.h): land from ToEntry after one given-up walk,
				// failed entries 1 the step before (Core's ++ to 2 is that step); watchdog from Surface, stuck ~20 s, no failed
				// entry. A teleport or the other path is no proof, whatever else follows.
				constexpr int32 KAshore = static_cast<int32>(ftcore::WaterFollowPhase::Ashore);
				constexpr int32 KToEntry = static_cast<int32>(ftcore::WaterFollowPhase::ToEntry);
				constexpr int32 KSurface = static_cast<int32>(ftcore::WaterFollowPhase::Surface);
				constexpr int32 KPlaced = static_cast<int32>(ftcore::WaterFollowPhase::Placed);
				if (SwimProofSlotPhase == KToEntry && Now == KAshore)
				{
					++SwimProofGaveUp;
				}
				if (Now == KPlaced && SwimProofPath == 0)
				{
					const bool bLand = SwimProofKind == TEXT("land") && SwimProofSlotPhase == KToEntry && SwimProofGaveUp >= 1 && SwimProofSlotFails == 1;
					const bool bWatch = SwimProofKind == TEXT("watchdog") && SwimProofSlotPhase == KSurface && SwimProofSlotStuck >= 19.f && SwimProofSlotFails == 0;
					SwimProofPath = bLand || bWatch ? 1 : -1;
					UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: slot 0 placed by %s."), *SwimProofKind,
						SwimProofPath > 0 ? TEXT("its own path (PATH OK)") : TEXT("another path (PATH WRONG: no proof)"));
				}
				if (SwimProofSlotPhase == KPlaced && Now == KSurface && SwimProofPath > 0 && SwimProofRejoinAt >= 0.f && !bSwimProofPass)
				{
					bSwimProofPass = true;
					UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: PASS LINE: own path, rejoined, then Surface at %.1f s (%s)."), *SwimProofKind, SwimProofTime,
						bSwimProofLeadPlaced ? TEXT("lead placed by harness") : TEXT("lead walked in"));
				}
				SwimProofSlotPhase = Now;
			}
			if (Slot0->phase == ftcore::WaterFollowPhase::Placed && Slot0->rejoin && SwimProofRejoinAt < 0.f)
			{
				SwimProofRejoinAt = SwimProofTime;
			}
			SwimProofSlotStuck = WaterParty.stuckFor(0);
			SwimProofSlotFails = WaterParty.entryFailsFor(0);
		}
		// 3 s after the rejoin (or at 60 s without one) the camera turns once round the lead over 12 s, its boom through
		// the swimming partners (item 5).
		const bool bBoomRun = SwimProofKind == TEXT("boom");
		const bool bExitRun = SwimProofKind == TEXT("exit");
		if (bExitRun && System)
		{
			TickSwimProofExit(*System, DeltaSeconds);
			if (SwimProofStage < 0)
			{
				return;
			}
		}
		const bool bSwitchRun = SwimProofKind == TEXT("switch");
		if (bSwitchRun && System)
		{
			TickSwimProofSwitch(*System, DeltaSeconds);
			if (SwimProofStage < 0)
			{
				return;
			}
		}
		// The boom run waits for its partners to tread (only a treading partner can be held), 25 s at the most.
		bool bPartnersTread = NumWaterFollowers > 0;
		for (int32 I = 0; I < 2 && I < NumWaterFollowers && I < ftcore::kWaterFollowMax; ++I)
		{
			const AFTPartyFollower* Partner = WaterFollowers[I].Get();
			bPartnersTread = bPartnersTread && Partner && Partner->GetWaterPhase() == ftcore::WaterFollowPhase::Surface;
		}
		const bool bBoomDue = bBoomRun && SwimProofHoldAt >= 0.f && SwimProofTime >= SwimProofHoldAt + 3.f
			&& (bPartnersTread || SwimProofTime >= SwimProofHoldAt + 25.f);
		const bool bOtherDue = !bBoomRun && !bExitRun && !bSwitchRun && ((SwimProofRejoinAt >= 0.f && SwimProofTime >= SwimProofRejoinAt + 3.f) || SwimProofTime >= 60.f);
		// The boom line at the camera's height (the 11-18 rerun): 1 m in front of the camera toward the arm's root, along
		// the line the probe sweeps, from the arm's unfixed camera spot (so the probe's own pull-in does not move it).
		auto BoomLineSpot = [this]()
		{
			const FVector Eye = Boom ? Boom->GetUnfixedCameraPosition() : Camera->GetComponentLocation();
			const FVector Root = Boom ? Boom->GetComponentLocation() + Boom->TargetOffset : GetActorLocation();
			const FVector2D Along = FVector2D(Root.X - Eye.X, Root.Y - Eye.Y).GetSafeNormal();
			return FVector(Eye.X + Along.X * 100.0, Eye.Y + Along.Y * 100.0, Eye.Z);
		};
		if (SwimProofSweep < 0.f && (bOtherDue || bBoomDue))
		{
			SwimProofSweep = 0.f;
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: camera sweep starts at %.1f s."), *SwimProofKind, SwimProofTime);
			if (bBoomRun && Camera)
			{
				// Item 5, the 11-18 rerun: slot 0 ON the boom line at the camera's height, its middle 1 m in front of the camera,
				// kept there each tick while the camera turns, so the boom points through its body all the way round; slot 1 on
				// the sweep circle 270 deg ahead (the yaw turns +, the boom with it about its root), so the camera passes over
				// it. The sweep waits 0.3 s after slot 0 is put there.
				const FVector Eye = Camera->GetComponentLocation();
				const FVector Pivot = Boom ? Boom->GetComponentLocation() : GetActorLocation();
				const FVector Spot1 = Pivot + FRotator(0.0, 270.0, 0.0).RotateVector(Eye - Pivot);
				const FVector Line = BoomLineSpot();
				const TCHAR* Names[2] = {TEXT("on the boom line 1 m in front of the camera at its height"), TEXT("on the sweep circle 270 deg ahead")};
				for (int32 I = 0; I < 2 && I < NumWaterFollowers && I < ftcore::kWaterFollowMax; ++I)
				{
					AFTPartyFollower* Partner = WaterFollowers[I].Get();
					const bool bHeld = Partner && (I == 0 ? Partner->HoldForProofAt(Line) : Partner->HoldForProof(FVector2D(Spot1.X, Spot1.Y)));
					bSwimProofBoomLine = bSwimProofBoomLine || (I == 0 && bHeld);
					SwimProofBoomHeld += bHeld ? 1 : 0;
					SwimProofBoomWhere += FString::Printf(TEXT("%sslot %d %s %s"), I > 0 ? TEXT("; ") : TEXT(""), I, bHeld ? TEXT("held") : TEXT("NOT held"), Names[I]);
				}
				SwimProofBoomSettle = 0.3f;
				const AFTPartyFollower* First = WaterFollowers[0].Get();
				UE_LOG(LogForgottenThird, Display, TEXT("SwimProof boom: camera at (%.0f, %.0f, %.0f), %.0f cm from the lead (%.0f cm in plan), arm %.0f cm; boom line spot (%.0f, %.0f, %.0f), slot 0's feet %.0f cm under the camera; probe %s (channel %d); %s."),
					Eye.X, Eye.Y, Eye.Z, FVector::Dist(Eye, GetActorLocation()), FVector::Dist2D(Eye, GetActorLocation()), Boom ? Boom->TargetArmLength : -1.f,
					Line.X, Line.Y, Line.Z, First ? Eye.Z - First->GetActorLocation().Z : -1.0,
					Boom && Boom->bDoCollisionTest ? TEXT("on") : TEXT("off"), Boom ? static_cast<int32>(Boom->ProbeChannel.GetValue()) : -1, *SwimProofBoomWhere);
			}
		}
		if (SwimProofBoomSettle > 0.f)
		{
			SwimProofBoomSettle -= DeltaSeconds;
		}
		else if (SwimProofSweep >= 0.f && SwimProofSweep < 360.f && Controller)
		{
			const float Turn = FMath::Min(30.f * DeltaSeconds, 360.f - SwimProofSweep);
			SwimProofSweep += Turn;
			FRotator View = Controller->GetControlRotation();
			View.Yaw += Turn;
			Controller->SetControlRotation(View);
		}
		// Item 5, logged only (no fix): while the sweep turns, the camera's distance to the nearest bone of the nearest
		// partner's body (the followers' meshes have no collision to measure to; the skin is ~10-20 cm out from the bones),
		// and whether any of that partner's meshes (body or worn pieces) is drawn. Under 50 cm and drawn is the clip.
		// Slot 0 kept on the boom line each tick (the rerun), the settle included.
		if (bBoomRun && bSwimProofBoomLine && !bSwimProofBoomLost && SwimProofSweep >= 0.f && SwimProofSweep < 360.f && Camera)
		{
			AFTPartyFollower* First = NumWaterFollowers > 0 ? WaterFollowers[0].Get() : nullptr;
			if (!First || !First->HoldForProofAt(BoomLineSpot()))
			{
				bSwimProofBoomLost = true;
				UE_LOG(LogForgottenThird, Warning, TEXT("SwimProof boom: slot 0's hold on the boom line lost at %.0f deg (%.1f s)."), SwimProofSweep, SwimProofTime);
			}
		}
		const bool bSweeping = SwimProofSweep >= 0.f && SwimProofSweep < 360.f && SwimProofBoomSettle <= 0.f;
		float CamNear = -1.f;
		float Slot0Near = -1.f;
		int32 CamSlot = -1;
		bool bCamDrawn = false;
		if (bSweeping && Camera)
		{
			const FVector Eye = Camera->GetComponentLocation();
			for (int32 I = 0; I < NumWaterFollowers && I < ftcore::kWaterFollowMax; ++I)
			{
				const AFTPartyFollower* Partner = WaterFollowers[I].Get();
				const USkeletalMeshComponent* PartnerBody = Partner ? Partner->GetBody() : nullptr;
				if (!PartnerBody)
				{
					continue;
				}
				const FTransform ToWorld = PartnerBody->GetComponentTransform();
				double Nearest = TNumericLimits<double>::Max();
				for (const FTransform& Bone : PartnerBody->GetComponentSpaceTransforms())
				{
					Nearest = FMath::Min(Nearest, FVector::Dist(Eye, ToWorld.TransformPosition(Bone.GetLocation())));
				}
				if (Nearest == TNumericLimits<double>::Max())
				{
					continue;
				}
				const float D = static_cast<float>(Nearest);
				Slot0Near = I == 0 ? D : Slot0Near;
				if (CamSlot < 0 || D < CamNear)
				{
					CamNear = D;
					CamSlot = I;
					bool bAnyDrawn = false;
					Partner->ForEachComponent<USkeletalMeshComponent>(false, [&bAnyDrawn](const USkeletalMeshComponent* Piece)
						{ bAnyDrawn = bAnyDrawn || (Piece->IsVisible() && Piece->WasRecentlyRendered(0.1f)); });
					bCamDrawn = !Partner->IsHidden() && bAnyDrawn;
				}
			}
			if (Boom && Boom->IsCollisionFixApplied())
			{
				++SwimProofBoomPulled;
			}
			if (bBoomRun && bSwimProofBoomLine)
			{
				const AFTPartyFollower* First = NumWaterFollowers > 0 ? WaterFollowers[0].Get() : nullptr;
				const bool bHid = First && First->IsHidden();
				SwimProofBoomHidden += bHid ? 1 : 0;
				if (Slot0Near >= 0.f && Slot0Near < 50.f)
				{
					++SwimProofBoomNear;
					SwimProofBoomNearHidden += bHid ? 1 : 0;
				}
			}
			if (CamSlot >= 0)
			{
				++SwimProofSweepTicks;
				SwimProofSweepNearest = SwimProofSweepNearest < 0.f ? CamNear : FMath::Min(SwimProofSweepNearest, CamNear);
				if (CamNear < 50.f && bCamDrawn)
				{
					++SwimProofClipTicks;
				}
			}
		}
		if (!SwimProofShots.IsEmpty() && SwimProofTime >= SwimProofShotAt)
		{
			SwimProofShotAt = SwimProofTime + 0.5f;
			const int32 Shot = SwimProofShotCount++;
			FScreenshotRequest::RequestScreenshot(FPaths::Combine(SwimProofShots, FString::Printf(TEXT("swimproof_%s_%03d.png"), *SwimProofKind, Shot)), false, false);
			if (bSweeping && CamSlot >= 0)
			{
				const bool bClip = CamNear < 50.f && bCamDrawn;
				UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: sweep still %03d at %.0f deg: camera %.0f cm from slot %d's nearest bone, mesh %s%s."), *SwimProofKind, Shot,
					SwimProofSweep, CamNear, CamSlot, bCamDrawn ? TEXT("drawn") : TEXT("not drawn"), bClip ? TEXT(": CLIP (candidate; check the still)") : TEXT(""));
				if (bClip && SwimProofClipShotCount < static_cast<int32>(UE_ARRAY_COUNT(SwimProofClipShots)))
				{
					SwimProofClipShots[SwimProofClipShotCount++] = Shot;
				}
			}
		}
		const float End = SwimProofSweep >= 360.f ? 0.f : 110.f; // after the sweep (at 80 s at the soonest, but the boom run's), or at 110 s
		if (!bExitRun && !bSwitchRun && ((SwimProofSweep >= 360.f && (SwimProofTime >= 80.f || bBoomRun)) || (End > 0.f && SwimProofTime >= End)))
		{
			for (int32 I = 0; I < NumWaterFollowers && I < ftcore::kWaterFollowMax; ++I)
			{
				if (AFTPartyFollower* Partner = WaterFollowers[I].Get())
				{
					Partner->ReleaseProofHold();
				}
			}
			// The boom run's verdict: it tests the clip only if the camera came within 50 cm of a held partner's bones.
			const bool bBoomMeasured = SwimProofBoomHeld > 0 && SwimProofSweepTicks > 0;
			const bool bBoomExercised = bBoomMeasured && SwimProofSweepNearest >= 0.f && SwimProofSweepNearest < 50.f;
			// The rerun's pass (ruled 11-18): slot 0 kept on the boom line through the whole sweep, the probe pulled the camera
			// in or slot 0's mesh hid, and no CLIP still.
			const bool bBoomGuarded = SwimProofBoomPulled > 0 || SwimProofBoomHidden > 0;
			const bool bBoomOk = bBoomMeasured && bSwimProofBoomLine && !bSwimProofBoomLost && bBoomGuarded && SwimProofClipShotCount == 0;
			if (bBoomRun)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("SwimProof boom: done at %.1f s, %d stills, %s; %d sweep ticks, nearest %.0f cm (%s), %d ticks under 50 cm with the mesh drawn, %d ticks the probe pulled the camera in; slot 0 hidden %d of %d ticks, within 50 cm of its bones %d ticks (hidden in %d); CLIP stills %d: %s."),
					SwimProofTime, SwimProofShotCount, SwimProofBoomWhere.IsEmpty() ? TEXT("no partner placed") : *SwimProofBoomWhere, SwimProofSweepTicks, SwimProofSweepNearest,
					bBoomExercised ? TEXT("within 50 cm") : TEXT("never within 50 cm"), SwimProofClipTicks, SwimProofBoomPulled, SwimProofBoomHidden, SwimProofSweepTicks,
					SwimProofBoomNear, SwimProofBoomNearHidden, SwimProofClipShotCount,
					!bBoomMeasured ? TEXT("NO MEASURE") : !bSwimProofBoomLine ? TEXT("NO MEASURE (slot 0 not on the boom line)") : bSwimProofBoomLost ? TEXT("NO PASS (slot 0's hold lost)")
					: SwimProofClipShotCount > 0 ? TEXT("NO PASS: CLIP (check the stills)") : !bBoomGuarded ? TEXT("NO PASS (neither pulled in nor hidden)")
					: SwimProofBoomHidden > 0 && SwimProofBoomPulled > 0 ? TEXT("PASS on the hide (slot 0 hidden; the pull-ins are not shown to be slot 0's)")
					: SwimProofBoomHidden > 0 ? TEXT("PASS on the hide (slot 0 hidden)")
					: TEXT("PASS on the pull-in only (not shown to be slot 0's: rv_boom pulled in with nothing on the line)"));
			}
			FString ClipList;
			for (int32 I = 0; I < SwimProofClipShotCount; ++I)
			{
				ClipList += FString::Printf(TEXT("%s%03d"), I > 0 ? TEXT(", ") : TEXT(""), SwimProofClipShots[I]);
			}
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: item 5 sweep: %d ticks measured, nearest %.0f cm, %d ticks under 50 cm with the mesh drawn; CLIP stills: %s."),
				*SwimProofKind, SwimProofSweepTicks, SwimProofSweepNearest, SwimProofClipTicks, ClipList.IsEmpty() ? TEXT("none") : *ClipList);
			const TCHAR* LeadHow = bSwimProofLeadPlaced ? TEXT("lead placed by harness") : SwimProofStage >= 3 ? TEXT("lead walked in") : TEXT("lead never in the water");
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: done at %.1f s, %d stills, %s%s, %d held-up jumps (%d not cleared), rejoin %s, path %d, %s."), *SwimProofKind,
				SwimProofTime, SwimProofShotCount, LeadHow, bSwimProofSpotFallback ? TEXT(" (water spot fallback)") : TEXT(""), LeadGoTry.tries(), LeadGoTry.failed(),
				SwimProofRejoinAt >= 0.f ? TEXT("seen") : TEXT("NOT seen"), SwimProofPath, bSwimProofPass ? TEXT("PASS") : TEXT("NO PASS"));
			FTLook::LogLevelStart(TEXT("proof's end"));
			FTRequestExit((bBoomRun ? bBoomOk : bSwimProofPass) ? 0 : 2, TEXT("SwimProof"));
			SwimProofStage = -2;
		}
	}
#endif
}

void AFTExploreCharacter::TickSwimProofSwitch(const UFTWaterSubsystem& System, float DeltaSeconds)
{
#if UE_BUILD_SHIPPING
	(void)System;
	(void)DeltaSeconds;
#else
	const bool bStart = SwimProofKind == TEXT("start");
	const TCHAR* Kind = bStart ? TEXT("start") : TEXT("switch");
	auto Finish = [this](int32 Code)
	{
		FTLook::LogLevelStart(TEXT("proof's end"));
		FTRequestExit(Code, TEXT("SwimProof"));
		SwimProofStage = -2;
	};
	// This frame: its real time (the switch's own cost lands in the frame after it), the camera's step about the lead, and
	// the lead's feet against its surface swim's target (the hold's own rule, bob and sink included).
	const double Clock = FPlatformTime::Seconds();
	const float Frame = SwimProofSwitchClock > 0.0 ? static_cast<float>(Clock - SwimProofSwitchClock) : DeltaSeconds;
	SwimProofSwitchClock = Clock;
	const FVector Me = GetActorLocation();
	const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	double LeadWant = 0.0;
	const bool bLeadWant = SurfaceSwimFeet(System, Me, WaterPose.sink, LeadWant);
	const float LeadOff = bLeadWant ? static_cast<float>(FMath::Abs(Me.Z - Half - LeadWant)) : -1.f;
	const bool bLeadSwims = bSwimming && GetCharacterMovement()->MovementMode == MOVE_Flying;
	const FVector CamOff = Camera ? Camera->GetComponentLocation() - Me : FVector::ZeroVector;
	const float CamStep = bSwimProofSwitchCamHas ? static_cast<float>((CamOff - SwimProofSwitchCamOff).Size()) : 0.f;
	SwimProofSwitchCamOff = CamOff;
	bSwimProofSwitchCamHas = Camera != nullptr;
	constexpr int32 Partners = 2;

	if (!bStart && SwimProofSwitchStage == 0)
	{
		// 3 s after the lead holds at the far point, once both partners tread (25 s at the most).
		if (SwimProofHoldAt < 0.f)
		{
			if (SwimProofTime >= 120.f)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("SwimProof switch: the lead never held at the far point in 120 s; no proof from this run."));
				Finish(2);
			}
			return;
		}
		bool bTread = NumWaterFollowers >= Partners;
		for (int32 I = 0; I < Partners && I < NumWaterFollowers; ++I)
		{
			const AFTPartyFollower* Partner = WaterFollowers[I].Get();
			bTread = bTread && Partner && Partner->GetWaterPhase() == ftcore::WaterFollowPhase::Surface;
		}
		if (SwimProofTime < SwimProofHoldAt + 3.f || (!bTread && SwimProofTime < SwimProofHoldAt + 25.f))
		{
			return;
		}
		if (!bTread)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("SwimProof switch: the partners are not both treading 25 s after the hold; switching all the same."));
		}
		SwimProofSwitchStage = 1;
		SwimProofSwitchAt = SwimProofTime;
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch: 1 s baseline from %.1f s (-SwimSwitchKeep %s)."), SwimProofTime, bSwimSwitchKeep ? TEXT("on") : TEXT("OFF: the old switch"));
		return;
	}
	if (!bStart && SwimProofSwitchStage == 1)
	{
		SwimProofSwitchBaseFrame = FMath::Max(SwimProofSwitchBaseFrame, Frame);
		SwimProofSwitchBaseCam = FMath::Max(SwimProofSwitchBaseCam, CamStep);
		SwimProofSwitchBaseLead = FMath::Max(SwimProofSwitchBaseLead, LeadOff);
		if (SwimProofTime < SwimProofSwitchAt + 1.f)
		{
			return;
		}
		// The switch as the party menu makes it (its confirm on the partner's row).
		if (!IsPartnerHere())
		{
			UE_LOG(LogForgottenThird, Error, TEXT("SwimProof switch: no partner to switch to; no proof from this run."));
			Finish(1);
			return;
		}
		const int32 Was = LeadIndex;
		SwimProofSwitchOldLook = PartyLooks.IsValidIndex(Was) ? PartyLooks[Was].Name : FString();
		for (int32 I = 0; I < Partners; ++I)
		{
			SwimProofSwitchOld[I] = WaterFollowers[I];
		}
		const double Before = FPlatformTime::Seconds();
		SwimProofSwitchPress = Before;
		SwitchLeadTo(PartnerIndex);
		const double CallMs = (FPlatformTime::Seconds() - Before) * 1000.0;
		if (LeadIndex == Was)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("SwimProof switch: the switch was refused (swimming %d, under %d, climbing out %d, rescued %d): FAIL."),
				bSwimming ? 1 : 0, bSwimUnder ? 1 : 0, bWaterClimbing ? 1 : 0, bSwimRescue ? 1 : 0);
			Finish(2);
			return;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch: %s -> %s at %.1f s (the call %.1f ms); baseline: worst frame %.1f ms, camera step %.1f cm, lead feet off its target %.1f cm."),
			*SwimProofSwitchOldLook, *PartyLooks[LeadIndex].Name, SwimProofTime, CallMs, SwimProofSwitchBaseFrame * 1000.f, SwimProofSwitchBaseCam, SwimProofSwitchBaseLead);
		if (!bSwimSwitchKeep)
		{
			// The no-flag rerun: from this frame the stick pushes the new lead 5 m on out (away from the dry edge).
			FVector Way = SwimProofFar - SwimProofDry;
			Way.Z = 0.0;
			Way = Way.IsNearlyZero() ? GetActorForwardVector().GetSafeNormal2D() : Way.GetSafeNormal();
			SwimProofSwitchFrom = GetActorLocation();
			SwimProofSwitchWay = Way;
			BeginLeadGo(SwimProofSwitchFrom + Way * 500.0, false);
			SwimProofSwitchShot0 = SwimProofShotCount;
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch: no flag: the stick pushes the new lead 5 m on out (LeadGo, pace %.2f); stills every 0.5 s from %03d."),
				LeadGoPace, SwimProofSwitchShot0);
		}
		SwimProofSwitchStage = 2;
		SwimProofSwitchAt = SwimProofTime;
		return;
	}
	if (SwimProofSwitchStage != 2)
	{
		return;
	}
	// Judged for 3 s from the frame after the switch (or the start).
	const float Since = SwimProofTime - SwimProofSwitchAt;
	const bool bFirstFrame = !bSwimProofSwitchJudged;
	bSwimProofSwitchJudged = true;
	if (Since <= 0.5f)
	{
		SwimProofSwitchWorstFrame = FMath::Max(SwimProofSwitchWorstFrame, Frame);
		SwimProofSwitchSumFrame += Frame;
		++SwimProofSwitchFrames;
	}
	if (!bFirstFrame && CamStep > FMath::Max(2.f * SwimProofSwitchBaseCam, 5.f))
	{
		SwimProofSwitchCamLast = Since;
	}
	SwimProofSwitchCamWorst = FMath::Max(SwimProofSwitchCamWorst, CamStep);
	if (!bLeadSwims)
	{
		++SwimProofSwitchLeadDry;
	}
	if (LeadOff >= 0.f)
	{
		if (SwimProofSwitchLeadFirst < 0.f)
		{
			SwimProofSwitchLeadFirst = LeadOff;
		}
		SwimProofSwitchLeadWorst = FMath::Max(SwimProofSwitchLeadWorst, LeadOff);
	}
	if (!bStart && SwimProofSwitchOpenWall < 0.f && Controller && !PartyScreen.open && !Controller->IsMoveInputIgnored())
	{
		// The first frame the new lead's move input is open, on the wall clock from the press.
		SwimProofSwitchOpenWall = static_cast<float>(Clock - SwimProofSwitchPress);
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch: the move input open %.2f s after the press (wall clock; +%.2f s game time)."), SwimProofSwitchOpenWall, Since);
	}
	const bool bLogNow = bFirstFrame || (SwimProofSwitchLog -= DeltaSeconds) <= 0.f;
	if (bLogNow)
	{
		SwimProofSwitchLog = 0.5f;
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: +%.2f s lead feet %.1f cm, target %.1f (off %.1f), swimming %d, frame %.1f ms, camera step %.1f cm."), Kind, Since,
			Me.Z - Half, LeadWant, LeadOff, bLeadSwims ? 1 : 0, Frame * 1000.f, CamStep);
	}
	for (int32 I = 0; I < Partners; ++I)
	{
		AFTPartyFollower* Partner = WaterFollowers[I].Get();
		if (!Partner || WaterFollowers[I] == SwimProofSwitchOld[I])
		{
			continue; // not the new party's member yet
		}
		const bool bNew = SwimProofSwitchSeen[I] < 0.f;
		if (bNew)
		{
			SwimProofSwitchSeen[I] = Since;
			SwimProofSwitchName[I] = Partner->GetWaterName();
		}
		const FVector At = Partner->GetActorLocation();
		double Want = 0.0;
		if (SurfaceSwimFeet(System, At, 0.f, Want, nullptr, Partner))
		{
			const float Off = static_cast<float>(FMath::Abs(At.Z - Want));
			if (SwimProofSwitchFirst[I] < 0.f)
			{
				SwimProofSwitchFirst[I] = Off;
			}
			SwimProofSwitchWorst[I] = FMath::Max(SwimProofSwitchWorst[I], Off);
			if (At.Z < Want - 30.0)
			{
				++SwimProofSwitchFell[I];
			}
			if (bNew || bLogNow)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: +%.2f s slot %d %s%s feet %.1f cm, target %.1f (off %.1f), phase %d."), Kind, Since, I, *SwimProofSwitchName[I],
					bNew ? TEXT(" first seen:") : TEXT(""), At.Z, Want, Off, GetWaterOrder(I) ? static_cast<int32>(GetWaterOrder(I)->phase) : -1);
			}
		}
		if (const ftcore::WaterFollowOrder* Order = GetWaterOrder(I))
		{
			const ftcore::WaterFollowPhase P = Order->phase;
			if (P == ftcore::WaterFollowPhase::Ashore || P == ftcore::WaterFollowPhase::ToEntry || P == ftcore::WaterFollowPhase::Entering)
			{
				++SwimProofSwitchLand[I];
			}
		}
	}
	if (bStart && bFirstFrame)
	{
		// Frame 1 of the start: the lead swimming within 3 cm of its target, both partners there and within 3 cm of theirs.
		bSwimProofStartFrame1 = bLeadSwims && LeadOff >= 0.f && LeadOff <= 3.f;
		for (int32 I = 0; I < Partners; ++I)
		{
			bSwimProofStartFrame1 = bSwimProofStartFrame1 && SwimProofSwitchFirst[I] >= 0.f && SwimProofSwitchFirst[I] <= 3.f;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof start: frame 1: lead off %.1f cm (swimming %d), slot 0 %s, slot 1 %s: %s."), LeadOff, bLeadSwims ? 1 : 0,
			SwimProofSwitchFirst[0] >= 0.f ? *FString::Printf(TEXT("off %.1f cm"), SwimProofSwitchFirst[0]) : TEXT("NOT THERE"),
			SwimProofSwitchFirst[1] >= 0.f ? *FString::Printf(TEXT("off %.1f cm"), SwimProofSwitchFirst[1]) : TEXT("NOT THERE"),
			bSwimProofStartFrame1 ? TEXT("OK") : TEXT("FAIL"));
	}
	if (!bStart && !bSwimSwitchKeep)
	{
		// The switch without the flag (the 11-18 rerun). The new lead answers the stick: 20 cm along the push from where it
		// switched, its move input open (no party screen, not ignored).
		using namespace FTSwimUnits;
		const bool bInputOpen = Controller && !PartyScreen.open && !Controller->IsMoveInputIgnored();
		const double Along = FVector::DotProduct(Me - SwimProofSwitchFrom, SwimProofSwitchWay);
		if (SwimProofSwitchControl < 0.f && bInputOpen && Along >= 20.0)
		{
			SwimProofSwitchControl = Since;
			SwimProofSwitchControlWall = static_cast<float>(Clock - SwimProofSwitchPress);
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch: the new lead answers the stick %.2f s after the press (wall clock; +%.2f s game time; %.0f cm on, swimming %d)."),
				SwimProofSwitchControlWall, Since, Along, bLeadSwims ? 1 : 0);
		}
		constexpr int32 KPlaced = static_cast<int32>(ftcore::WaterFollowPhase::Placed);
		for (int32 I = 0; I < Partners; ++I)
		{
			const AFTPartyFollower* Partner = WaterFollowers[I].Get();
			const bool bNewMember = Partner && WaterFollowers[I] != SwimProofSwitchOld[I];
			const ftcore::WaterFollowOrder* Order = bNewMember ? GetWaterOrder(I) : nullptr;
			const int32 PhaseNow = Order ? static_cast<int32>(Order->phase) : -1;
			SwimProofSwitchWatchWorst[I] = FMath::Max(SwimProofSwitchWatchWorst[I], WaterParty.stuckFor(I));
			if (PhaseNow != SwimProofSwitchPhase[I])
			{
				UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch: +%.2f s slot %d phase %d -> %d (the watchdog at %.1f s)%s."), Since, I, SwimProofSwitchPhase[I], PhaseNow,
					WaterParty.stuckFor(I), PhaseNow == KPlaced ? TEXT(": PLACED") : TEXT(""));
				SwimProofSwitchPlaced[I] += PhaseNow == KPlaced ? 1 : 0;
				SwimProofSwitchPhase[I] = PhaseNow;
			}
			bool bAtSlot = false;
			if (bNewMember)
			{
				// Under the bed: the topmost ground under it, traced down from just under the water's surface there (as
				// SurfaceSwimFeet does: the surface itself is not the bed), or from 3 m over its feet where it is dry; its feet
				// more than 10 cm under that is a fall through.
				const FVector At = Partner->GetActorLocation();
				float Surface = 0.f;
				const bool bWet = System.Bodies().surfaceHeight(ToPlan(At), System.WaterTime(), Surface);
				const double Top = bWet ? FMath::Max(ToCm(Surface) - 5.0, At.Z) : At.Z + 300.0;
				FCollisionQueryParams Query(SCENE_QUERY_STAT(SwimProofBed), false, this);
				Query.AddIgnoredActor(Partner);
				FHitResult Bed;
				const bool bBedHit = GetWorld()->LineTraceSingleByChannel(Bed, FVector(At.X, At.Y, Top), FVector(At.X, At.Y, At.Z - 1000.0), ECC_Visibility, Query);
				if (SwimProofSwitchBedTicks[I]++ == 0)
				{
					UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch: +%.2f s slot %d's bed trace from %.0f cm: %s."), Since, I, Top,
						bBedHit ? *FString::Printf(TEXT("ground at %.0f cm (%s), feet at %.0f cm"), Bed.ImpactPoint.Z, Bed.GetActor() ? *Bed.GetActor()->GetName() : TEXT("?"), At.Z)
								: TEXT("NO BED FOUND (an under-the-bed count of 0 means nothing here)"));
				}
				SwimProofSwitchBedHit[I] += bBedHit ? 1 : 0;
				if (bBedHit)
				{
					const float Under = static_cast<float>(Bed.ImpactPoint.Z - At.Z);
					if (Under > 10.f)
					{
						if (SwimProofSwitchUnder[I]++ == 0)
						{
							UE_LOG(LogForgottenThird, Warning, TEXT("SwimProof switch: +%.2f s slot %d UNDER THE BED: feet at %.0f cm, the ground at %.0f cm (%s)."), Since, I, At.Z,
								Bed.ImpactPoint.Z, Bed.GetActor() ? *Bed.GetActor()->GetName() : TEXT("?"));
						}
						SwimProofSwitchUnderWorst[I] = FMath::Max(SwimProofSwitchUnderWorst[I], Under);
					}
				}
				bAtSlot = Order && Order->phase == ftcore::WaterFollowPhase::Surface
					&& FVector::Dist2D(At, FVector(ToCm(Order->target.x), ToCm(Order->target.y), 0.0)) <= 300.0;
			}
			if (bAtSlot)
			{
				if (SwimProofSwitchSettled[I] < 0.f)
				{
					SwimProofSwitchSettled[I] = Since;
					UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch: +%.2f s slot %d first at its surface slot."), Since, I);
				}
				SwimProofSwitchOff[I] = 0.f;
			}
			else
			{
				SwimProofSwitchOff[I] += DeltaSeconds;
				SwimProofSwitchStuckWorst[I] = FMath::Max(SwimProofSwitchStuckWorst[I], SwimProofSwitchOff[I]);
			}
		}
		// Ends on resolution: both at their slots from 10.5 s (the 20th still after the switch is taken by then), or a
		// stretch off a slot past 25 s (the watchdog's 20 s and its placement have had their time), or 45 s (a stretch that
		// starts after 10.5 s still gets one full watchdog cycle).
		const bool bBothAtSlots = SwimProofSwitchSettled[0] >= 0.f && SwimProofSwitchSettled[1] >= 0.f && SwimProofSwitchOff[0] <= 0.f && SwimProofSwitchOff[1] <= 0.f;
		const bool bStretchOver = SwimProofSwitchStuckWorst[0] > 25.f || SwimProofSwitchStuckWorst[1] > 25.f;
		if (Since < 10.5f || (!bBothAtSlots && !bStretchOver && Since < 45.f))
		{
			return;
		}
		// The verdict (ruled 11-18): the new lead answers within 1 s; no partner under the bed in any frame; none stuck more
		// than 20 s on the water plan's own clock (its watchdog places it at 20 s: a placement is allowed and logged), and
		// both at their surface slots at the end. The longest stretch off the slot is logged, not judged.
		constexpr float KStuckLine = 20.25f; // the watchdog's 20 s and the step that places it
		// Control on the wall clock from the press (ruled 11-19): game time steps are clamped on a long frame.
		const bool bControlOk = SwimProofSwitchControlWall >= 0.f && SwimProofSwitchControlWall <= 1.f;
		bool bPartnersOk = true;
		for (int32 I = 0; I < Partners; ++I)
		{
			const bool bAtEnd = SwimProofSwitchSettled[I] >= 0.f && SwimProofSwitchOff[I] <= 0.f;
			const bool bOk = SwimProofSwitchSeen[I] >= 0.f && SwimProofSwitchUnder[I] == 0 && SwimProofSwitchWatchWorst[I] <= KStuckLine && bAtEnd;
			bPartnersOk = bPartnersOk && bOk;
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch: slot %d %s: seen at +%.2f s, first at its surface slot at +%.2f s, at it at the end %d, longest off it %.1f s, the watchdog's longest %.1f s, placements %d, frames under the bed %d (deepest %.1f cm; the bed found in %d of %d frames): %s."),
				I, SwimProofSwitchName[I].IsEmpty() ? TEXT("(none)") : *SwimProofSwitchName[I], SwimProofSwitchSeen[I], SwimProofSwitchSettled[I], bAtEnd ? 1 : 0, SwimProofSwitchStuckWorst[I],
				SwimProofSwitchWatchWorst[I], SwimProofSwitchPlaced[I], SwimProofSwitchUnder[I], SwimProofSwitchUnderWorst[I], SwimProofSwitchBedHit[I], SwimProofSwitchBedTicks[I],
				bOk ? TEXT("OK") : TEXT("FAIL"));
		}
		const bool bPass = bControlOk && bPartnersOk;
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch (no flag): the new lead answers the stick %.2f s after the press on the wall clock (+%.2f s game time) %s; move input open at %.2f s; worst frame %.1f ms in the first 0.5 s; partners %s; lead dry frames %d, feet worst %.1f cm off; stills %03d-%03d every 0.5 s; judged %.1f s (%s): %s."),
			SwimProofSwitchControlWall, SwimProofSwitchControl, bControlOk ? TEXT("OK") : TEXT("FAIL"), SwimProofSwitchOpenWall, SwimProofSwitchWorstFrame * 1000.f,
			bPartnersOk ? TEXT("OK") : TEXT("FAIL"), SwimProofSwitchLeadDry, SwimProofSwitchLeadWorst, SwimProofSwitchShot0, SwimProofShotCount - 1, Since, bBothAtSlots ? TEXT("both at their slots") : bStretchOver ? TEXT("a stretch off a slot over 25 s") : TEXT("the 45 s cap"),
			bPass ? TEXT("PASS") : TEXT("FAIL"));
		Finish(bPass ? 0 : 2);
		return;
	}
	if (Since < 3.f)
	{
		return;
	}
	// The verdict.
	const float CamSettled = SwimProofSwitchCamLast < 0.f ? 0.f : SwimProofSwitchCamLast;
	const bool bHitchOk = SwimProofSwitchWorstFrame < 0.3f;
	const bool bCameraOk = CamSettled <= 0.3f;
	const bool bLeadOk = SwimProofSwitchLeadDry == 0 && (SwimProofSwitchLeadWorst <= 3.f || SwimProofSwitchLeadWorst <= SwimProofSwitchBaseLead + 0.5f);
	bool bPartnersOk = true;
	bool bOldThere = false;
	for (int32 I = 0; I < Partners; ++I)
	{
		const bool bOk = SwimProofSwitchSeen[I] >= 0.f && SwimProofSwitchSeen[I] <= 1.f && SwimProofSwitchFirst[I] >= 0.f && SwimProofSwitchFirst[I] <= 3.f
			&& SwimProofSwitchWorst[I] <= 3.f && SwimProofSwitchLand[I] == 0 && SwimProofSwitchFell[I] == 0;
		bPartnersOk = bPartnersOk && bOk;
		bOldThere = bOldThere || (!SwimProofSwitchOldLook.IsEmpty() && SwimProofSwitchName[I] == SwimProofSwitchOldLook);
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: slot %d %s: seen at +%.2f s, first off %.1f cm, worst %.1f cm, land-phase frames %d, frames over 30 cm under %d: %s."), Kind, I,
			SwimProofSwitchName[I].IsEmpty() ? TEXT("(none)") : *SwimProofSwitchName[I], SwimProofSwitchSeen[I], SwimProofSwitchFirst[I], SwimProofSwitchWorst[I], SwimProofSwitchLand[I],
			SwimProofSwitchFell[I], bOk ? TEXT("OK") : TEXT("FAIL"));
	}
	const float MeanMs = SwimProofSwitchFrames > 0 ? SwimProofSwitchSumFrame * 1000.f / static_cast<float>(SwimProofSwitchFrames) : 0.f;
	bool bPass = false;
	if (bStart)
	{
		bPass = bSwimProofStartFrame1 && bLeadOk && bPartnersOk;
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof start: frame 1 %s; lead first off %.1f cm, worst %.1f, dry frames %d; first 0.5 s worst frame %.1f ms (mean %.1f): %s."),
			bSwimProofStartFrame1 ? TEXT("OK") : TEXT("FAIL"), SwimProofSwitchLeadFirst, SwimProofSwitchLeadWorst, SwimProofSwitchLeadDry, SwimProofSwitchWorstFrame * 1000.f, MeanMs,
			bPass ? TEXT("PASS") : TEXT("FAIL"));
	}
	else
	{
		const bool bOpenOk = SwimProofSwitchOpenWall >= 0.f && SwimProofSwitchOpenWall <= 1.f;
		bPass = bHitchOk && bOpenOk && bCameraOk && bLeadOk && bPartnersOk && bOldThere;
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof switch: the move input open %.2f s after the press on the wall clock %s."), SwimProofSwitchOpenWall,
			bOpenOk ? TEXT("OK") : TEXT("FAIL"));
		UE_LOG(LogForgottenThird, Display,
			TEXT("SwimProof switch: worst frame %.1f ms in the first 0.5 s (mean %.1f, baseline %.1f) %s; camera settled at +%.2f s (worst step %.1f cm, baseline %.1f) %s; lead first off %.1f cm, worst %.1f (baseline %.1f), dry frames %d %s; old lead %s %s; -SwimSwitchKeep %s: %s."),
			SwimProofSwitchWorstFrame * 1000.f, MeanMs, SwimProofSwitchBaseFrame * 1000.f, bHitchOk ? TEXT("OK") : TEXT("FAIL"), CamSettled, SwimProofSwitchCamWorst, SwimProofSwitchBaseCam,
			bCameraOk ? TEXT("OK") : TEXT("FAIL"), SwimProofSwitchLeadFirst, SwimProofSwitchLeadWorst, SwimProofSwitchBaseLead, SwimProofSwitchLeadDry, bLeadOk ? TEXT("OK") : TEXT("FAIL"),
			*SwimProofSwitchOldLook, bOldThere ? TEXT("among the partners") : TEXT("NOT among the partners"), bSwimSwitchKeep ? TEXT("on") : TEXT("off"), bPass ? TEXT("PASS") : TEXT("FAIL"));
	}
	Finish(bPass ? 0 : 2);
#endif
}

bool AFTExploreCharacter::SwimProofCostParty(bool bWantWater, FString& OutWhy) const
{
	// Who is where, read once at a window's start and end (never per frame): on land nobody swims; in the sea the lead
	// swims at the surface and both partners keep their surface slots. All three drawn in both.
	const bool bLeadIn = bSwimming || bSwimUnder;
	int32 Swimmers = bLeadIn ? 1 : 0;
	int32 Shown = IsHidden() ? 0 : 1;
	int32 Partners = 0;
	for (int32 I = 0; I < 2 && I < NumWaterFollowers; ++I)
	{
		const AFTPartyFollower* Partner = WaterFollowers[I].Get();
		if (!Partner)
		{
			continue;
		}
		++Partners;
		Shown += Partner->IsHidden() ? 0 : 1;
		Swimmers += Partner->GetWaterPhase() == ftcore::WaterFollowPhase::Surface ? 1 : 0;
	}
	OutWhy = FString::Printf(TEXT("%d of 3 swimming (lead %s), %d of 3 drawn, %d partners"), Swimmers,
		bSwimUnder ? TEXT("under") : bSwimming ? TEXT("at the surface") : TEXT("dry"), Shown, Partners);
	return Partners == 2 && Shown == 3 && (bWantWater ? (Swimmers == 3 && bSwimming && !bSwimUnder) : Swimmers == 0);
}

void AFTExploreCharacter::TickSwimProofCost(float DeltaSeconds)
{
#if UE_BUILD_SHIPPING
	(void)DeltaSeconds;
#else
	auto Finish = [this](int32 Code)
	{
		FTLook::LogLevelStart(TEXT("proof's end"));
		FTRequestExit(Code, TEXT("SwimProof"));
		SwimProofStage = -2;
	};
	// A window's frames: the same reads as -FrameStats (UFTFrameStatsSubsystem), into storage made before the first.
	if (SwimProofCostStage == 1 || SwimProofCostStage == 3)
	{
		ftcore::FrameStats* Stats = SwimProofCostStats[SwimProofCostStage / 2].Get();
		const double FrameMs = FApp::GetDeltaTime() * 1000.0;
		if (Stats && FrameMs < 2000.0)
		{
			ftcore::FrameSample S;
			S.frameMs = static_cast<float>(FrameMs);
			S.gameMs = static_cast<float>(FPlatformTime::ToMilliseconds(GGameThreadTime));
			S.renderMs = static_cast<float>(FPlatformTime::ToMilliseconds(GRenderThreadTime));
			S.gpuMs = static_cast<float>(FPlatformTime::ToMilliseconds64(RHIGetGPUFrameCycles(0)));
			S.drawCalls = static_cast<float>(GNumDrawCallsRHI[0]);
			S.primitives = static_cast<float>(GNumPrimitivesDrawnRHI[0]);
			Stats->add(S);
		}
	}
	SwimProofCostTime += DeltaSeconds;
	const bool bWater = SwimProofCostStage >= 2;
	const TCHAR* Window = bWater ? TEXT("water") : TEXT("land");
	// One still per window, 2 s before its frames start (the still's read-back lands outside the window).
	if ((SwimProofCostStage == 0 || SwimProofCostStage == 2) && !bSwimProofCostShot && SwimProofCostTime >= 2.f)
	{
		bSwimProofCostShot = true;
		if (!SwimProofShots.IsEmpty())
		{
			FScreenshotRequest::RequestScreenshot(FPaths::Combine(SwimProofShots, FString::Printf(TEXT("swimproof_cost_%s.png"), Window)), false, false);
		}
	}
	// Settle 4 s on land, 5 s after the start in the sea (the partners respawn at their surface slots); 10 s windows.
	const float Settle = bWater ? 5.f : 4.f;
	if ((SwimProofCostStage == 0 || SwimProofCostStage == 2) && SwimProofCostTime >= Settle)
	{
		FString Why;
		if (!SwimProofCostParty(bWater, Why))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("SwimProof cost: the %s window cannot start: %s; no figures from this run."), Window, *Why);
			Finish(2);
			return;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof cost: %s window starts (%s)."), Window, *Why);
		++SwimProofCostStage;
		SwimProofCostTime = 0.f;
		return;
	}
	if ((SwimProofCostStage == 1 || SwimProofCostStage == 3) && SwimProofCostTime >= 10.f)
	{
		FString Why;
		const bool bStill = SwimProofCostParty(bWater, Why);
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof cost: %s window ends (%s)."), Window, *Why);
		if (!bStill)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("SwimProof cost: the party left its %s state during the window; no figures from this run."), Window);
			Finish(2);
			return;
		}
		if (SwimProofCostStage == 1)
		{
			const FVector2D Way(SwimProofWet.X - SwimProofDry.X, SwimProofWet.Y - SwimProofDry.Y);
			if (!StartInWater(SwimProofFar, FMath::RadiansToDegrees(FMath::Atan2(Way.Y, Way.X))))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("SwimProof cost: the start in the water at the far point was refused; no water window."));
				Finish(2);
				return;
			}
			SwimProofCostStage = 2;
			SwimProofCostTime = 0.f;
			bSwimProofCostShot = false;
			return;
		}
		// Both windows: each one's medians and p95, then the difference, with the settings the frames were drawn at.
		ftcore::FrameSummary Sum[2];
		for (int32 I = 0; I < 2; ++I)
		{
			ftcore::FrameStats& Stats = *SwimProofCostStats[I];
			Sum[I] = Stats.summary();
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof cost %s: %d frames%s; frame %.2f ms median (p95 %.2f); game %.2f (p95 %.2f); render %.2f (p95 %.2f); GPU %.2f (p95 %.2f); draw calls %.0f (p95 %.0f); primitives %.0f; over 33 ms %d."),
				I == 0 ? TEXT("land") : TEXT("water"), static_cast<int32>(Sum[I].count), Stats.size() >= static_cast<std::size_t>(kSwimProofCostCap) ? TEXT(" (storage full)") : TEXT(""),
				Sum[I].frame.median, Sum[I].frame.p95, Sum[I].game.median, Sum[I].game.p95, Sum[I].render.median, Sum[I].render.p95,
				Sum[I].gpu.median, Sum[I].gpu.p95, Sum[I].drawCalls.median, Sum[I].drawCalls.p95, Sum[I].primitives.median, static_cast<int32>(Sum[I].over33));
		}
		auto Cvar = [](const TCHAR* Name) -> float
		{
			IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
			return Var ? Var->GetFloat() : -1.f;
		};
		FIntPoint View(0, 0);
		if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
		{
			View = GEngine->GameViewport->Viewport->GetSizeXY();
		}
		const bool bNoGpu = Sum[0].gpu.median <= 0.f || Sum[1].gpu.median <= 0.f;
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof cost: three swimmers minus land (medians): frame %+.2f ms, game %+.2f, render %+.2f, GPU %+.2f%s; viewport %dx%d, r.ScreenPercentage %.0f, sg.ResolutionQuality %.0f, sg.ViewDistance %.0f, sg.Shadow %.0f, sg.GI %.0f, sg.Reflection %.0f, sg.PostProcess %.0f, sg.Effects %.0f, sg.Shading %.0f."),
			Sum[1].frame.median - Sum[0].frame.median, Sum[1].game.median - Sum[0].game.median, Sum[1].render.median - Sum[0].render.median,
			Sum[1].gpu.median - Sum[0].gpu.median, bNoGpu ? TEXT(" (NO GPU TIMES: not valid)") : TEXT(""), View.X, View.Y, Cvar(TEXT("r.ScreenPercentage")),
			Cvar(TEXT("sg.ResolutionQuality")), Cvar(TEXT("sg.ViewDistanceQuality")), Cvar(TEXT("sg.ShadowQuality")), Cvar(TEXT("sg.GlobalIlluminationQuality")),
			Cvar(TEXT("sg.ReflectionQuality")), Cvar(TEXT("sg.PostProcessQuality")), Cvar(TEXT("sg.EffectsQuality")), Cvar(TEXT("sg.ShadingQuality")));
		Finish(bNoGpu ? 2 : 0);
	}
#endif
}

void AFTExploreCharacter::TickSwimProofExit(const UFTWaterSubsystem& System, float DeltaSeconds)
{
#if UE_BUILD_SHIPPING
	(void)System;
	(void)DeltaSeconds;
#else
	const ftcore::WaterBodies& Bodies = System.Bodies();
	const int32 Partners = FMath::Min(FMath::Min(NumWaterFollowers, ftcore::kWaterFollowMax), 2);
	const bool bFast = GetWaterFollowTuning().climbPath;
	auto Finish = [this](int32 Code)
	{
		for (int32 I = 0; I < NumWaterFollowers && I < ftcore::kWaterFollowMax; ++I)
		{
			if (AFTPartyFollower* Partner = WaterFollowers[I].Get())
			{
				Partner->ReleaseProofHold();
			}
		}
		FTLook::LogLevelStart(TEXT("proof's end"));
		FTRequestExit(Code, TEXT("SwimProof"));
		SwimProofStage = -2;
	};
	if (SwimProofExitStage == 0)
	{
		// 3 s after the lead holds at the far point, once both partners tread (25 s at the most): the nearest ladder in the
		// lead's own water, approached from 2.5 m out, then pushed at (TryLadderClimb takes it within reach).
		bool bTread = Partners > 0;
		for (int32 I = 0; I < Partners; ++I)
		{
			const AFTPartyFollower* Partner = WaterFollowers[I].Get();
			bTread = bTread && Partner && Partner->GetWaterPhase() == ftcore::WaterFollowPhase::Surface;
		}
		if (SwimProofHoldAt < 0.f || SwimProofTime < SwimProofHoldAt + 3.f || (!bTread && SwimProofTime < SwimProofHoldAt + 25.f))
		{
			return;
		}
		const FVector At = GetActorLocation();
		const int32 Here = Bodies.bodyAt(FTSwimUnits::ToPlan(At));
		double Best = TNumericLimits<double>::Max();
		for (std::size_t E = 0; E < Bodies.exitCount(); ++E)
		{
			const ftcore::WaterExit& Ex = Bodies.exit(E);
			if (Ex.kind != ftcore::WaterExitKind::Ladder || Bodies.bodyAt(Ex.foot) != Here)
			{
				continue;
			}
			const double D = FVector2D::Distance(FVector2D(FTSwimUnits::ToCm(Ex.foot.x), FTSwimUnits::ToCm(Ex.foot.y)), FVector2D(At.X, At.Y));
			if (D < Best)
			{
				Best = D;
				SwimProofExitLadder = static_cast<int32>(E);
			}
		}
		if (SwimProofExitLadder < 0)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("SwimProof exit: no ladder in the lead's water (body %d, %d exits on the map); NO PASS."), Here, static_cast<int32>(Bodies.exitCount()));
			Finish(2);
			return;
		}
		const ftcore::WaterExit& L = Bodies.exit(static_cast<std::size_t>(SwimProofExitLadder));
		const FVector Foot(FTSwimUnits::ToCm(L.foot.x), FTSwimUnits::ToCm(L.foot.y), 0.0);
		const FVector Out(L.out.x, L.out.y, 0.0);
		BeginLeadGo(Foot - Out * 250.0, false);
		LeadGoNext.Add(Foot + Out * 100.0);
		SwimProofExitStage = 1;
		SwimProofExitAt = SwimProofTime;
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof exit: to ladder %d, foot (%.0f, %.0f) cm, top (%.0f, %.0f, %.0f) cm, %.1f m away; partners %s; fast exit %s."),
			SwimProofExitLadder, Foot.X, Foot.Y, FTSwimUnits::ToCm(L.top.x), FTSwimUnits::ToCm(L.top.y), FTSwimUnits::ToCm(L.topZ), Best / 100.0,
			bTread ? TEXT("treading") : TEXT("NOT all treading after 25 s"), bFast ? TEXT("ON (-PartyExitFast)") : TEXT("off (the default)"));
		return;
	}
	if (SwimProofExitStage == 1)
	{
		if (bWaterClimbing)
		{
			SwimProofExitStage = 2;
			SwimProofLeadClimbAt = SwimProofTime;
			bLeadGo = false; // the climb carries it; once out it stands
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof exit: the lead climbs %s %d at %.1f s (%.1f s after the route began), %.1f s up."),
				bWaterClimbLadder ? TEXT("ladder") : TEXT("a wall (not the ladder), exit"), WaterLeadExit, SwimProofTime, SwimProofTime - SwimProofExitAt, WaterClimbSeconds);
		}
		else if (SwimProofTime >= SwimProofExitAt + 90.f)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("SwimProof exit: the lead never climbed out 90 s after the route began (at (%.0f, %.0f, %.0f), swimming %d); NO PASS."),
				GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z, bSwimming ? 1 : 0);
			Finish(2);
		}
		return;
	}
	if (SwimProofExitStage == 2 && !bWaterClimbing)
	{
		if (bSwimming)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("SwimProof exit: the lead's climb ended with it swimming again; NO PASS."));
			Finish(2);
			return;
		}
		SwimProofExitStage = 3;
		SwimProofLeadOutAt = SwimProofTime;
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof exit: the lead out at %.2f s, feet at (%.0f, %.0f, %.0f) cm."), SwimProofTime, GetActorLocation().X,
			GetActorLocation().Y, GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	}
	// From the lead's climb on: each partner's phases, its feet on the exit's top, and the most it moves between two frames.
	const float FromOut = SwimProofLeadOutAt >= 0.f ? SwimProofTime - SwimProofLeadOutAt : -1.f;
	for (int32 I = 0; I < Partners; ++I)
	{
		const AFTPartyFollower* Partner = WaterFollowers[I].Get();
		if (!Partner)
		{
			continue;
		}
		const FVector Now = Partner->GetActorLocation();
		const int32 Phase = static_cast<int32>(Partner->GetWaterPhase());
		if (bSwimProofExitHasLast[I])
		{
			const float Step = static_cast<float>(FVector::Dist(Now, SwimProofExitLast[I]));
			if (Step > SwimProofExitWorst[I])
			{
				SwimProofExitWorst[I] = Step;
				SwimProofExitWorstDt[I] = DeltaSeconds;
				SwimProofExitWorstPhase[I] = Phase;
			}
		}
		SwimProofExitLast[I] = Now;
		bSwimProofExitHasLast[I] = true;
		if (Phase != SwimProofExitPhase[I])
		{
			const ftcore::WaterFollowOrder* Order = GetWaterOrder(I);
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof exit: slot %d phase %d -> %d at %.2f s (%s), exit %d, feet (%.0f, %.0f, %.0f) cm."), I, SwimProofExitPhase[I], Phase,
				SwimProofTime, FromOut >= 0.f ? *FString::Printf(TEXT("%.2f s after the lead out"), FromOut) : TEXT("the lead not out yet"), Order ? Order->exitIndex : -9, Now.X, Now.Y, Now.Z);
			if (Phase == static_cast<int32>(ftcore::WaterFollowPhase::Climbing) && SwimProofExitClimbAt[I] < 0.f)
			{
				SwimProofExitClimbAt[I] = SwimProofTime;
				SwimProofExitIndex[I] = Order ? Order->exitIndex : -1;
			}
			if (Phase == static_cast<int32>(ftcore::WaterFollowPhase::Ashore) && SwimProofExitPhase[I] == static_cast<int32>(ftcore::WaterFollowPhase::Climbing)
				&& SwimProofExitAshoreAt[I] < 0.f)
			{
				SwimProofExitAshoreAt[I] = SwimProofTime;
			}
			SwimProofExitPhase[I] = Phase;
		}
		const int32 Ex = SwimProofExitIndex[I];
		if (SwimProofExitClimbAt[I] >= 0.f && SwimProofExitDryAt[I] < 0.f && Ex >= 0 && static_cast<std::size_t>(Ex) < Bodies.exitCount()
			&& Now.Z >= FTSwimUnits::ToCm(Bodies.exit(static_cast<std::size_t>(Ex)).topZ) - 5.0)
		{
			SwimProofExitDryAt[I] = SwimProofTime;
		}
	}
	if (SwimProofExitStage < 3)
	{
		return;
	}
	bool bAllOut = Partners > 0;
	float LastOut = 0.f;
	for (int32 I = 0; I < Partners; ++I)
	{
		bAllOut = bAllOut && SwimProofExitAshoreAt[I] >= 0.f;
		LastOut = FMath::Max(LastOut, SwimProofExitAshoreAt[I]);
	}
	if (!((bAllOut && SwimProofTime >= LastOut + 2.f) || FromOut >= 20.f))
	{
		return;
	}
	// The verdict: strict (every partner Climbing -> Ashore within 4 s of the lead out), dry feet (its feet at the exit's top
	// within 4 s), and no step over 20 cm between two frames.
	bool bStrict = Partners > 0, bDry = Partners > 0, bSteps = true;
	FString Lines;
	for (int32 I = 0; I < Partners; ++I)
	{
		const float Ashore = SwimProofExitAshoreAt[I] >= 0.f ? SwimProofExitAshoreAt[I] - SwimProofLeadOutAt : -1.f;
		const float Dry = SwimProofExitDryAt[I] >= 0.f ? SwimProofExitDryAt[I] - SwimProofLeadOutAt : Ashore;
		const float Climb = SwimProofExitClimbAt[I] >= 0.f ? SwimProofExitClimbAt[I] - SwimProofLeadOutAt : -99.f;
		bStrict = bStrict && Ashore >= 0.f && Ashore <= 4.f;
		bDry = bDry && Dry >= -10.f && Dry <= 4.f && SwimProofExitAshoreAt[I] >= 0.f;
		bSteps = bSteps && SwimProofExitWorst[I] <= 20.f;
		Lines += FString::Printf(TEXT("%sslot %d by exit %d: climb from %+.2f s, feet at the top %s, ashore %s, worst step %.1f cm (dt %.3f s, phase %d)"), I > 0 ? TEXT("; ") : TEXT(""), I,
			SwimProofExitIndex[I], Climb, SwimProofExitDryAt[I] >= 0.f ? *FString::Printf(TEXT("%+.2f s"), Dry) : TEXT("n/a"),
			Ashore >= 0.f ? *FString::Printf(TEXT("%.2f s"), Ashore) : TEXT("NOT OUT in 20 s"), SwimProofExitWorst[I], SwimProofExitWorstDt[I], SwimProofExitWorstPhase[I]);
	}
	const TCHAR* Verdict = !bSteps ? TEXT("FAIL (a step over 20 cm)") : bStrict ? TEXT("PASS") : bDry ? TEXT("PASS ON DRY FEET ONLY (Ashore later than 4 s)") : TEXT("FAIL (later than 4 s)");
	UE_LOG(LogForgottenThird, Display, TEXT("SwimProof exit: done at %.1f s, fast exit %s, lead climbed at %.2f s and out at %.2f s; %s; %s."), SwimProofTime, bFast ? TEXT("ON") : TEXT("off"),
		SwimProofLeadClimbAt, SwimProofLeadOutAt, *Lines, Verdict);
	Finish(bStrict && bSteps ? 0 : 2);
#endif
}

void AFTExploreCharacter::BeginSwimRescue(const UFTWaterSubsystem& System)
{
	using namespace FTSwimUnits;
	UWorld* World = GetWorld();
	UCharacterMovementComponent* Move = GetCharacterMovement();
	const FVector At = GetActorLocation();
	const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const ftcore::WaterBodies& Water = System.Bodies();
	const ftcore::Vec2 P = ToPlan(At);
	bSwimRescue = true;
	SwimRescueTime = 0.f;
	SwimRescueFrom = At;
	float Surface = WaterNow.surface;
	Water.surfaceHeight(P, System.WaterTime(), Surface);
	SwimRescueUp = FVector(At.X, At.Y, ActorFromRef({P.x, P.y, Surface}, Half).Z);
	// To the nearest way out within 40 m (on its bank, a step past its top)...
	const TCHAR* Kind = TEXT("the surface");
	SwimRescueTo = SwimRescueUp;
	const int Exit = Water.nearestExit(P, 40.f);
	if (Exit >= 0)
	{
		const ftcore::WaterExit& E = Water.exit(static_cast<std::size_t>(Exit));
		SwimRescueTo = FVector(ToCm(E.top.x + E.out.x * 0.6f), ToCm(E.top.y + E.out.y * 0.6f), ToCm(E.topZ) + Half + 2.0);
		Kind = TEXT("the nearest way out");
	}
	else if (World)
	{
		// ... else the nearest dry ground: rings of 16 looks down at 4, 8, 16, 32 and 64 m, the first ring with walkable
		// ground that is no water's (a one-off search, at the rescue only).
		FCollisionQueryParams Query(SCENE_QUERY_STAT(FTSwimRescue), false, this);
		bool bFound = false;
		for (const double Radius : {400.0, 800.0, 1600.0, 3200.0, 6400.0})
		{
			double Best = TNumericLimits<double>::Max();
			for (int32 I = 0; I < 16; ++I)
			{
				const double A = UE_DOUBLE_TWO_PI * I / 16.0;
				// From 4 m over the surface: a quay or a bank, never a roof.
				const FVector Look(At.X + Radius * FMath::Cos(A), At.Y + Radius * FMath::Sin(A), ToCm(Surface));
				FHitResult Ground;
				if (!World->LineTraceSingleByChannel(Ground, Look + FVector(0.0, 0.0, 400.0), Look - FVector(0.0, 0.0, 500.0), ECC_Visibility, Query)
					|| Ground.ImpactNormal.Z < 0.7 || Ground.bStartPenetrating
					|| Water.overWater(ToPlan(Ground.ImpactPoint), ToMetres(Ground.ImpactPoint.Z)))
				{
					continue;
				}
				const double Gap = FVector::Dist2D(Ground.ImpactPoint, At);
				if (Gap < Best)
				{
					Best = Gap;
					SwimRescueTo = Ground.ImpactPoint + FVector(0.0, 0.0, Half + 2.0);
					bFound = true;
				}
			}
			if (bFound)
			{
				Kind = TEXT("the nearest dry ground");
				break;
			}
		}
	}
	Move->StopMovementImmediately();
	Move->SetMovementMode(MOVE_Flying);
	UE_LOG(LogForgottenThird, Display, TEXT("Swim: out of breath and vitality at (%.0f, %.0f, %.0f) cm; rescued to %s at (%.0f, %.0f, %.0f) cm."), At.X, At.Y, At.Z,
		Kind, SwimRescueTo.X, SwimRescueTo.Y, SwimRescueTo.Z);
}

void AFTExploreCharacter::TickSwimRescue(float DeltaSeconds)
{
	// A placed carry: up to the surface (0.8 s), then over to the way out (1.4 s), rising first so it clears the wall.
	constexpr float Rise = 0.8f;
	constexpr float Carry = 1.4f;
	SwimRescueTime += DeltaSeconds;
	FVector At;
	if (SwimRescueTime < Rise)
	{
		At = FMath::Lerp(SwimRescueFrom, SwimRescueUp, FMath::SmoothStep(0.f, 1.f, SwimRescueTime / Rise));
	}
	else
	{
		const float A = FMath::Min((SwimRescueTime - Rise) / Carry, 1.f);
		At = FMath::Lerp(SwimRescueUp, SwimRescueTo, FMath::SmoothStep(0.3f, 1.f, A));
		At.Z = FMath::Lerp(SwimRescueUp.Z, SwimRescueTo.Z, FMath::SmoothStep(0.f, 0.5f, A));
	}
	SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->Velocity = FVector::ZeroVector;
	PoseSwim(0.f, DeltaSeconds);
	if (SwimRescueTime < Rise + Carry)
	{
		return;
	}
	// There: the swim starts again at the surface with a full breath and the rescue's vitality; on dry ground the lead
	// is out of the water (it lands from just over the ground).
	bSwimRescue = false;
	bSwimWarned = false;
	SwimBody.reset();
	SwimFlagsNow = SwimBody.last().flags;
	PoseSwim(0.f, 1.f);
	const bool bAshore = !SwimRescueTo.Equals(SwimRescueUp, 1.0);
	if (bAshore)
	{
		bSwimming = false;
		Move->MaxFlySpeed = SwimSavedFly;
		Move->BrakingDecelerationFlying = SwimSavedBrake;
		Move->SetMovementMode(MOVE_Falling);
	}
	else
	{
		Move->MaxFlySpeed = ftcore::kWaterSwimSpeed * 100.f;
		Move->BrakingDecelerationFlying = 350.f;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Swim: rescued, %s at (%.0f, %.0f, %.0f) cm, vitality %.0f%%."), bAshore ? TEXT("ashore") : TEXT("at the surface"),
		At.X, At.Y, At.Z, SwimBody.vitality() * 100.f);
}

void AFTExploreCharacter::StirWater(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	if (!System || !System->HasBodies())
	{
		// No water here: the weather alone (dry by default, so this is two early returns).
		ShowLeadWetness(DeltaSeconds);
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
	ShowLeadWetness(DeltaSeconds);
}

void AFTExploreCharacter::ShowLeadWetness(float DeltaSeconds)
{
#if !UE_BUILD_SHIPPING
	if (SoakSwapWet >= 0.f)
	{
		return; // -ExploreSoakSwap holds its own wetness
	}
#endif
	// The water's store only where the map has water (else it may hold a stale soak); held as it is while parked.
	const UWorld* World = GetWorld();
	const UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	const ftcore::WaterWetness Water = System && System->HasBodies() ? System->LeadWetness() : ftcore::WaterWetness{};
	const float Weather = LeadWeather();
	ShowWetness(ftcore::leadwet::cloth(Water, Weather));
	ShowLeadWet(ftcore::leadwet::surface(Weather, Water), DeltaSeconds);
}

float AFTExploreCharacter::LeadWeather() const
{
	// Below 0 follows the party's rain soak (StepLeadRain); 0..1 forces it. The look's wetness is the ground's damp
	// floor, never a lead's.
	return ftcore::leadwet::weather(CVarLeadWet.GetValueOnGameThread(), LeadRain.soak);
}

namespace
{
// What the cover map draws (its show flags leave out skeletal meshes, particles, translucency, decals and landscape):
// a visible static mesh with an opaque material. The lead's trace counts a hit only on these.
bool CoverDrawn(const UPrimitiveComponent* Prim)
{
	if (!Prim || !Prim->IsVisible() || Prim->bHiddenInGame || !Prim->IsA<UStaticMeshComponent>())
	{
		return false;
	}
	for (int32 M = 0; M < Prim->GetNumMaterials(); ++M)
	{
		const UMaterialInterface* Mat = Prim->GetMaterial(M);
		if (Mat && !IsTranslucentBlendMode(Mat->GetBlendMode()))
		{
			return true;
		}
	}
	return false;
}
} // namespace

ftcore::leadwet::CoverAnswer AFTExploreCharacter::TraceLeadCover(const ftcore::RainCoverSpec& Spec) const
{
	// Up from the top of the head as far as the cover map looks down from. Only what the map draws counts (CoverDrawn). A
	// skipped hit is ignored and the trace runs again, at most four times; past that, or with no world or a hit with no
	// component, the answer is Unknown and the last exposure holds (never open by default). FCollisionQueryParams keeps
	// its ignore list inline: no heap allocation here.
	using ftcore::leadwet::CoverAnswer;
	using ftcore::leadwet::CoverTrace;
	const UWorld* World = GetWorld();
	if (!World)
	{
		return CoverAnswer{};
	}
	const FVector At = GetActorLocation();
	const float Head = At.Z + GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector From(At.X, At.Y, Head);
	const FVector To(At.X, At.Y, Head + Spec.aboveM * 100.f);
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FTLeadRainCover), true, this);
	for (int32 Try = 0; Try < 4; ++Try)
	{
		FHitResult Hit;
		if (!World->LineTraceSingleByObjectType(Hit, From, To, Objects, Params))
		{
			return CoverAnswer{CoverTrace::Open, 0.f, static_cast<float>(Head * 0.01)};
		}
		const UPrimitiveComponent* Prim = Hit.GetComponent();
		if (CoverDrawn(Prim))
		{
			return CoverAnswer{CoverTrace::Hit, static_cast<float>(Hit.ImpactPoint.Z * 0.01), static_cast<float>(Head * 0.01)};
		}
		if (!Prim)
		{
			return CoverAnswer{};
		}
		Params.AddIgnoredComponent(Prim);
	}
	return CoverAnswer{};
}

void AFTExploreCharacter::StepLeadRain(float DeltaSeconds)
{
	// Source passes values; Core decides (ftcore::leadwet::leadRainStep: the cadence, the exposure from the answer, the
	// first settle once a trace has answered, the soak step). The trace needs only the cover's spec, map on or off.
	const UWorld* World = GetWorld();
	const UFTRainSubsystem* Rain = World ? World->GetSubsystem<UFTRainSubsystem>() : nullptr;
	const UFTRainCoverSubsystem* Cover = World ? World->GetSubsystem<UFTRainCoverSubsystem>() : nullptr;
	const ftcore::RainState State = Rain ? Rain->GetLeadRain() : ftcore::RainState{};
	const ftcore::RainSpec Spec = Rain ? Rain->GetSpec() : ftcore::RainSpec{};
	const ftcore::RainCoverSpec CoverSpec = Cover ? Cover->GetSpec() : ftcore::RainCoverSpec{};
	const bool bChanged = ftcore::leadwet::leadRainStep(LeadRain, Spec, CoverSpec, State, DeltaSeconds, [&]
	{
#if !UE_BUILD_SHIPPING
		LeadRainTracePrevAt = LeadRainTraceAt;
		LeadRainTraceAt = GetActorLocation();
		LeadRainTraceTime = World ? World->GetTimeSeconds() : 0.0;
#endif
		return TraceLeadCover(CoverSpec);
	});
#if !UE_BUILD_SHIPPING
	// A line on each exposure flip (held to one a second at a roof edge, the count kept), else one every 5 s while wet.
	if (ftcore::leadwet::leadRainLogDue(LeadRainLog, bChanged, State.wetness > 0.f || LeadRain.soak > 0.f, DeltaSeconds))
	{
		const FVector At = GetActorLocation();
		UE_LOG(LogForgottenThird, Display,
			TEXT("LEAD_RAIN t %.2f s exposure %.2f soak %.3f rain %.3f (intensity %.2f) flips %d cover map %s; at (%.0f, %.0f, %.0f) cm; last trace t %.2f s at (%.0f, %.0f), the one before at (%.0f, %.0f)."),
			World ? World->GetTimeSeconds() : 0.0, LeadRain.exposure, LeadRain.soak, State.wetness, State.intensity, LeadRainLog.flips,
			(Cover && Cover->IsActive()) ? TEXT("on") : TEXT("off"), At.X, At.Y, At.Z, LeadRainTraceTime, LeadRainTraceAt.X, LeadRainTraceAt.Y,
			LeadRainTracePrevAt.X, LeadRainTracePrevAt.Y);
	}
#else
	(void)bChanged;
#endif
}

#if !UE_BUILD_SHIPPING
void AFTExploreCharacter::LogLeadCoverSet()
{
	// The cover map draws render geometry; the lead's trace hits collision. This lists what the map would draw that the
	// trace cannot hit (it hits only WorldStatic/WorldDynamic objects with query collision, complex), and drawn meshes
	// that are not static meshes (the trace skips them). Once, in a rainy run; what was loaded is said with it.
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	int32 Actors = 0, Drawn = 0, Hittable = 0, QueryOff = 0, ObjectType = 0, NoBody = 0, NoShapes = 0, SimpleOnly = 0, Other = 0;
	FString First;
	int32 Named = 0;
	auto Note = [&](const UPrimitiveComponent* C, const TCHAR* Why)
	{
		if (Named < 8)
		{
			const FBox B = C->Bounds.GetBox();
			First += FString::Printf(TEXT("%s%s.%s (%s, z %.1f..%.1f m)"), Named ? TEXT("; ") : TEXT(""), *GetNameSafe(C->GetOwner()), *C->GetName(), Why,
				B.Min.Z * 0.01, B.Max.Z * 0.01);
			++Named;
		}
	};
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++Actors;
		TInlineComponentArray<UMeshComponent*> Meshes(*It);
		for (const UMeshComponent* Mesh : Meshes)
		{
			if (Mesh == nullptr || Mesh->IsA<USkinnedMeshComponent>() || !Mesh->IsVisible() || Mesh->bHiddenInGame)
			{
				continue;
			}
			if (!Mesh->IsA<UStaticMeshComponent>())
			{
				++Other;
				Note(Mesh, TEXT("drawn, not a static mesh"));
				continue;
			}
			if (!CoverDrawn(Mesh))
			{
				continue; // translucent: the map leaves it out too
			}
			++Drawn;
			const ECollisionChannel Type = Mesh->GetCollisionObjectType();
			const UBodySetup* Body = Mesh->GetBodySetup();
			if (!Mesh->IsQueryCollisionEnabled())
			{
				++QueryOff;
				Note(Mesh, TEXT("query collision off"));
			}
			else if (Type != ECC_WorldStatic && Type != ECC_WorldDynamic)
			{
				++ObjectType;
				Note(Mesh, TEXT("object type not WorldStatic/WorldDynamic"));
			}
			else if (!Body)
			{
				++NoBody;
				Note(Mesh, TEXT("no body setup"));
			}
			else if (Body->GetCollisionTraceFlag() == CTF_UseSimpleAsComplex ? Body->AggGeom.GetElementCount() == 0
																				 : (Body->TriMeshGeometries.Num() == 0 && Body->AggGeom.GetElementCount() == 0))
			{
				++NoShapes;
				Note(Mesh, TEXT("no collision shapes"));
			}
			else if (Body->GetCollisionTraceFlag() != CTF_UseSimpleAsComplex && Body->TriMeshGeometries.Num() == 0)
			{
				++SimpleOnly;
				Note(Mesh, TEXT("simple shapes only, no triangle mesh"));
			}
			else
			{
				++Hittable;
			}
		}
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("LEAD_COVER_SET at %.1f s, %d actors loaded: %d drawn opaque static meshes, %d the lead's trace can hit, %d it cannot (query collision off %d, object type %d, no body %d, no shapes %d), %d with simple shapes only (a complex trace may miss them); %d other drawn meshes it skips. First: %s."),
		World->GetTimeSeconds(), Actors, Drawn, Hittable, QueryOff + ObjectType + NoBody + NoShapes, QueryOff, ObjectType, NoBody, NoShapes, SimpleOnly, Other,
		First.IsEmpty() ? TEXT("none") : *First);
}
#endif

void AFTExploreCharacter::ShowLeadWet(float Weather, float DeltaSeconds)
{
	// Weather here is the lead's surface wetness (ftcore::leadwet::surface): rain or its own water.
	const bool bGroom = ftcore::leadwet::groomWet(Weather, bGroomWet);
	if (bGroom != bGroomWet)
	{
		bGroomWet = bGroom;
		const int32 Swapped = FTLook::SetGroomsWet(GroomComponents, WornLook.Grooms, bGroomWet, TEXT("FTExploreCharacter"));
		UE_LOG(LogForgottenThird, Display, TEXT("Wet switch: %s grooms on (%d swapped) at weather %.2f."), bGroomWet ? TEXT("wet") : TEXT("dry"),
			Swapped, Weather);
	}
	// The wet-skin amount, eased; it goes to the materials ShowWetness gathered (any skin is wet only when the cloth is,
	// so they are there) and to the hair without a wet groom.
	const float Target = ftcore::leadwet::skin(Weather);
	SkinWetEased = ftcore::leadwet::shownSkin(SkinWetEased, Target, DeltaSeconds, bSkinWetSnap);
#if !UE_BUILD_SHIPPING
	// Proof log (captures only): the shown skin on the first show after each look and about half a second later, so a
	// start in the rain can be read for a snap (the first look eases, a later one lands at the member's soak).
	if (SkinLogStage < 2)
	{
		const UWorld* World = GetWorld();
		const double Now = World ? World->GetTimeSeconds() : 0.0;
		if (SkinLogStage == 0 || Now >= SkinLogAt + 0.5)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("SKIN_LOOK %s: look %d, snap %d, surface %.2f, target %.3f, shown %.3f at %.2f s."),
				SkinLogStage == 0 ? TEXT("first show") : TEXT("0.5 s on"), LooksWorn, bSkinWetSnap ? 1 : 0, Weather, Target, SkinWetEased, Now);
			SkinLogAt = SkinLogStage == 0 ? Now : SkinLogAt;
			++SkinLogStage;
		}
	}
#endif
	bSkinWetSnap = false;
	const float Skin = SkinWetEased;
	// Written when it has moved enough, and once more when it reaches its target (so the last small step lands).
	const bool bChanged = FMath::Abs(Skin - SkinWetShown) > 0.004f || (Skin == Target && Skin != SkinWetShown);
	if (!bWetMaterialsMade || !bChanged)
	{
		return;
	}
	static const FName SkinWetName(TEXT("SkinWet"));
	static const FName MhRoughName(TEXT("Roughness Global Multiply Post-Bake"));
	static const FName MhSpecName(TEXT("Specular Global Multiply Post-Bake"));
	for (int32 Index = 0; Index < WetMaterials.Num(); ++Index)
	{
		UMaterialInstanceDynamic* Dynamic = WetMaterials[Index];
		if (!Dynamic)
		{
			continue;
		}
		Dynamic->SetScalarParameterValue(SkinWetName, Skin);
		const FVector2f Dry = WetSkinDry.IsValidIndex(Index) ? WetSkinDry[Index] : FVector2f(-1.f, -1.f);
		if (Dry.X >= 0.f)
		{
			Dynamic->SetScalarParameterValue(MhRoughName, ftcore::leadwet::mhRoughness(Dry.X, Skin));
		}
		if (Dry.Y >= 0.f)
		{
			Dynamic->SetScalarParameterValue(MhSpecName, ftcore::leadwet::mhSpecular(Dry.Y, Skin));
		}
	}
	ShowHairWet(Skin);
	SkinWetShown = Skin;
}

void AFTExploreCharacter::ShowHairWet(float Wet)
{
	static const FName RoughName(TEXT("Roughness"));
	static const FName HairRoughName(TEXT("HairRoughness"));
	static const FName MelaninName(TEXT("hairMelanin"));
	if (!bWetHairMade)
	{
		// The first time the lead is wet with this look: an instance on each groom without a wet groom, dry values kept.
		bWetHairMade = true;
		WetHairMaterials.Reset();
		WetHairDry.Reset();
		FTLook::MakeHairWetMaterials(GroomComponents, WornLook.Grooms, WetHairMaterials);
		for (UMaterialInstanceDynamic* Dynamic : WetHairMaterials)
		{
			FVector3f Dry(-1.f, -1.f, -1.f);
			float Value = 0.f;
			if (Dynamic && Dynamic->GetScalarParameterValue(FHashedMaterialParameterInfo(RoughName), Value))
			{
				Dry.X = Value;
			}
			if (Dynamic && Dynamic->GetScalarParameterValue(FHashedMaterialParameterInfo(HairRoughName), Value))
			{
				Dry.Y = Value;
			}
			if (Dynamic && Dynamic->GetScalarParameterValue(FHashedMaterialParameterInfo(MelaninName), Value))
			{
				Dry.Z = Value;
			}
			WetHairDry.Add(Dry);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Wet switch: %d hair materials without a wet groom take the wet value."), WetHairMaterials.Num());
	}
	for (int32 Index = 0; Index < WetHairMaterials.Num(); ++Index)
	{
		UMaterialInstanceDynamic* Dynamic = WetHairMaterials[Index];
		const FVector3f& Dry = WetHairDry[Index];
		if (!Dynamic)
		{
			continue;
		}
		if (Dry.X >= 0.f)
		{
			Dynamic->SetScalarParameterValue(RoughName, ftcore::leadwet::hairRoughness(Dry.X, Wet));
		}
		if (Dry.Y >= 0.f)
		{
			Dynamic->SetScalarParameterValue(HairRoughName, ftcore::leadwet::hairRoughness(Dry.Y, Wet));
		}
		if (Dry.Z >= 0.f)
		{
			Dynamic->SetScalarParameterValue(MelaninName, ftcore::leadwet::hairMelanin(Dry.Z, Wet));
		}
	}
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
	if (!bWetMaterialsMade || WetMaterialsSerial != FTLook::WornMeshSerial())
	{
		// A dynamic instance of every material on the lead's skeletal meshes (body, head, clothes): the first time it is
		// wet, and again once a piece's mesh was swapped (its slots are new; FTLook carries an instance across a swap, so
		// this mostly finds the same ones). The shown values are forgotten so every one is written to them all.
		bWetMaterialsMade = true;
		WetMaterialsSerial = FTLook::WornMeshSerial();
		WetMaterials.Reset();
		WetSkinDry.Reset();
		WetShown = -1.f;
		WetLineShown = -1.0e9f;
		WetSiltShown = -1.f;
		SkinWetShown = -1.f;
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
					// The MetaHuman skin's dry multipliers (-1 where the material has none), for the wet switch.
					FVector2f Dry(-1.f, -1.f);
					float Value = 0.f;
					if (Dynamic->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Roughness Global Multiply Post-Bake")), Value))
					{
						Dry.X = Value;
					}
					if (Dynamic->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Specular Global Multiply Post-Bake")), Value))
					{
						Dry.Y = Value;
					}
					WetSkinDry.Add(Dry);
				}
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Water: wetness goes to %d dynamic materials on %d skeletal meshes (Wetness, WetLineZ, WetLineSoft, WetSilt)."),
			WetMaterials.Num(), Meshes.Num());
	}
	// The line is kept over the feet, so it travels with the body; it is written when it has moved a centimetre.
	const float LineZ = static_cast<float>(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) + Wet.line * 100.f;
	const bool bWet = FMath::Abs(Wet.wetness - WetShown) > 0.004f || (Wet.wetness <= 0.f && WetShown != 0.f);
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
	const FVector Push = bSwimming ? FVector(WaterMoveWanted.X, WaterMoveWanted.Y, 0.0).GetSafeNormal2D()
		: GetCharacterMovement()->GetLastInputVector().GetSafeNormal2D();
	const bool bPushing = FVector::DotProduct(Push, Out) > 0.5;
	const bool bToward = FVector::DotProduct(GetActorForwardVector().GetSafeNormal2D(), Out) > 0.3;
	if (!(bPushing || (bPressed && bToward)))
	{
		return false;
	}
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const double Feet = At.Z - Half;
	bWaterClimbing = true;
	bWaterClimbLadder = true;
	WaterLeadExit = Index;
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
	bWaterClimbLadder = false;
	WaterLeadExit = -1;
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
	// Swimming at the surface: the move is WaterLoco's to make (the surface hold sets the velocity from its step), so
	// the ask is kept for it rather than given to the movement.
	if (bSwimming && !bSwimUnder && !bSwimRescue && !bWaterClimbing)
	{
		WaterMoveWanted += FVector2D(WorldDirection.X, WorldDirection.Y) * ScaleValue;
		return;
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
