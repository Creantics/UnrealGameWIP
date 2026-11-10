#include "FTPartyVan.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/SpringArmComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "FTExploreCharacter.h"
#include "FTPartyFollower.h"
#include "ForgottenThird.h"

#include <string>

namespace
{
constexpr float KmhToCms = 100000.f / 3600.f;

// -VanCourse, in the course's frame (cm): the straight along +X, the left turn at 150 m, then along -Y.
const FVector CourseOrigin(0.0, 300000.0, 20000.0);
constexpr float CornerX = 15000.f;
constexpr float LegTwo = 5000.f;
constexpr float KerbFrom = 5000.f, KerbTo = 9000.f, KerbInner = 150.f, KerbWidth = 30.f, KerbHeight = 12.f;

// The point `S` cm along the course's centre line (the kerb stretch shifted toward the kerb).
FVector2D CoursePoint(float S)
{
	if (S <= CornerX)
	{
		const float Shift = (S > KerbFrom - 1000.f && S < KerbTo) ? 110.f : 0.f;
		return FVector2D(S, Shift);
	}
	return FVector2D(CornerX, -(S - CornerX));
}

// How far along the course the point P lies (the nearer leg's projection).
float CourseDistance(const FVector2D& P)
{
	const float A = FMath::Clamp(P.X, 0.f, CornerX);
	const float DA = FVector2D::Distance(P, FVector2D(A, 0.f));
	const float B = FMath::Clamp(-P.Y, 0.f, LegTwo);
	const float DB = FVector2D::Distance(P, FVector2D(CornerX, -B));
	return DA <= DB ? A : CornerX + B;
}

UStaticMesh* LoadMesh(const FSoftObjectPath& Path, const TCHAR* Fallback)
{
	UStaticMesh* M = Cast<UStaticMesh>(Path.TryLoad());
	return M ? M : LoadObject<UStaticMesh>(nullptr, Fallback);
}
} // namespace

AFTPartyVan::AFTPartyVan()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetCollisionProfileName(TEXT("Vehicle"));
	Box->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	RootComponent = Box;

	Tilt = CreateDefaultSubobject<USceneComponent>(TEXT("Tilt"));
	Tilt->SetupAttachment(Box);
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Tilt);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	static const TCHAR* Names[4] = {TEXT("WheelFL"), TEXT("WheelFR"), TEXT("WheelRL"), TEXT("WheelRR")};
	for (int I = 0; I < 4; ++I)
	{
		Wheels[I] = CreateDefaultSubobject<UStaticMeshComponent>(Names[I]);
		Wheels[I]->SetupAttachment(Box);
		Wheels[I]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	Boom = CreateDefaultSubobject<USpringArmComponent>(TEXT("Boom"));
	Boom->SetupAttachment(Box);
	Boom->TargetArmLength = 750.f;
	Boom->SocketOffset = FVector(0.0, 0.0, 160.0);
	Boom->SetRelativeRotation(FRotator(-12.0, 0.0, 0.0));
	Boom->bUsePawnControlRotation = false;
	Boom->bEnableCameraRotationLag = true;
	Boom->CameraRotationLagSpeed = 4.f;
	Boom->bDoCollisionTest = true;
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Boom, USpringArmComponent::SocketName);
}

void AFTPartyVan::BeginPlay()
{
	Super::BeginPlay();
	LoadData();
	BodyHalfZ = (BodyBox.Z - Clearance) * 0.5f;
	Box->SetBoxExtent(FVector(BodyBox.X * 0.5f, BodyBox.Y * 0.5f, BodyHalfZ));
	Tilt->SetRelativeLocation(FVector(0.0, 0.0, -(Clearance + BodyHalfZ)));

	UStaticMesh* BodyAsset = Cast<UStaticMesh>(BodyMesh.TryLoad());
	if (BodyAsset)
	{
		Body->SetStaticMesh(BodyAsset);
	}
	else
	{
		Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Body->SetRelativeScale3D(FVector(BodyBox.X, BodyBox.Y, BodyBox.Z - Clearance) / 100.0);
		Body->SetRelativeLocation(FVector(0.0, 0.0, Clearance + BodyHalfZ));
		UE_LOG(LogForgottenThird, Display, TEXT("FTPartyVan: no %s yet, a box stands in."), *BodyMesh.ToString());
	}
	const bool bWheelAsset = WheelMesh.TryLoad() != nullptr;
	UStaticMesh* WheelAsset = LoadMesh(WheelMesh, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	for (int I = 0; I < 4; ++I)
	{
		Wheels[I]->SetStaticMesh(WheelAsset);
		if (!bWheelAsset)
		{
			Wheels[I]->SetRelativeScale3D(FVector(WheelRadius * 2.f, 18.f, WheelRadius * 2.f) / 100.0);
		}
	}

	const FString Cmd = FCommandLine::Get();
	bCourse = FParse::Param(*Cmd, TEXT("VanCourse"));
	bScript = bCourse && FParse::Param(*Cmd, TEXT("VanDrive"));
	FParse::Value(*Cmd, TEXT("VanShots="), ShotPrefix);

	const FVector At = GetActorLocation();
	State = Drive.parked(At.X, At.Y, FMath::DegreesToRadians(GetActorRotation().Yaw));
	Start = FVector2D(At.X, At.Y);
	StartHeading = State.heading;
	PlaceOnGround(true);
	SetActorLocationAndRotation(FVector(State.x, State.y, GroundZ + Clearance + BodyHalfZ), FRotator(0.0, GetActorRotation().Yaw, 0.0));
}

void AFTPartyVan::LoadData()
{
	FString Text;
	std::string Error;
	const FString DriveFile = FPaths::ProjectDir() / TEXT("Data/Vehicles/microvan.json");
	ftcore::VehicleTuning T;
	if (FFileHelper::LoadFileToString(Text, *DriveFile) && ftcore::importVehicleTuning(TCHAR_TO_UTF8(*Text), T, &Error))
	{
		Drive.setTuning(T);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTPartyVan: %s not read (%s); Core defaults."), *DriveFile, UTF8_TO_TCHAR(Error.c_str()));
	}
	const FString RulesFile = FPaths::ProjectDir() / TEXT("Data/Vehicles/vehicle_rules.json");
	Error.clear();
	if (!FFileHelper::LoadFileToString(Text, *RulesFile) || !ftcore::importVehicleRules(TCHAR_TO_UTF8(*Text), Rules, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTPartyVan: %s not read (%s); Core defaults."), *RulesFile, UTF8_TO_TCHAR(Error.c_str()));
	}
}

bool AFTPartyVan::TryEnter(AFTExploreCharacter* Lead)
{
	if (!Lead || Driver.IsValid())
	{
		return false;
	}
	// The lead's distance to the body box, in the van's frame.
	const FVector L = GetActorTransform().InverseTransformPosition(Lead->GetActorLocation());
	const FVector2D Out(FMath::Max(0.0, FMath::Abs(L.X) - BodyBox.X * 0.5), FMath::Max(0.0, FMath::Abs(L.Y) - BodyBox.Y * 0.5));
	float Followers[2] = {0.f, 0.f};
	int Count = 0;
	for (TActorIterator<AFTPartyFollower> It(GetWorld()); It && Count < 2; ++It)
	{
		if (!It->IsHidden())
		{
			Followers[Count++] = FVector::Dist2D(It->GetActorLocation(), GetActorLocation());
		}
	}
	if (!ftcore::tryEnter(Rules.seats, Seats, static_cast<float>(Out.Size()), State.speed(), true, Followers, Count))
	{
		return false;
	}
	Driver = Lead;
	Lead->BoardVan(this);
	LastPhase = Seats.phase;
	UE_LOG(LogForgottenThird, Display, TEXT("FTPartyVan: boarding (%d aboard in %.2f s)."), Seats.count, Seats.length);
	return true;
}

bool AFTPartyVan::SideClear(float Sign) const
{
	const FVector At = GetActorTransform().TransformPosition(FVector(30.0, Sign * (BodyBox.Y * 0.5 + 70.0), 0.0));
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FTVanSide), false, this);
	if (const AFTExploreCharacter* Lead = Driver.Get())
	{
		Params.AddIgnoredActor(Lead);
	}
	return !GetWorld()->OverlapAnyTestByChannel(At, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(40.f, 85.f), Params);
}

void AFTPartyVan::TickSeats(float DeltaSeconds)
{
	if (!Driver.IsValid())
	{
		return;
	}
	const float DriverSign = bRightHandDrive ? 1.f : -1.f;
	ftcore::tickSeats(Rules.seats, Seats, DeltaSeconds, State.speed(), bExitHeld, SideClear(DriverSign), SideClear(-DriverSign));
	if (Seats.phase == ftcore::SeatPhase::Exiting && LastPhase != ftcore::SeatPhase::Exiting)
	{
		ExitSign = Seats.side == ftcore::ExitSide::Passenger ? -DriverSign : DriverSign;
	}
	if (Seats.phase == ftcore::SeatPhase::OnFoot && LastPhase == ftcore::SeatPhase::Exiting)
	{
		FinishExit();
	}
	LastPhase = Seats.phase;
}

void AFTPartyVan::FinishExit()
{
	AFTExploreCharacter* Lead = Driver.Get();
	Driver.Reset();
	bExitHeld = false;
	if (!Lead)
	{
		return;
	}
	const FVector At = GetActorTransform().TransformPosition(FVector(30.0, ExitSign * (BodyBox.Y * 0.5 + 70.0), 0.0));
	Lead->LeaveVan(FVector(At.X, At.Y, GroundZ + 100.0), GetActorRotation().Yaw);
	UE_LOG(LogForgottenThird, Display, TEXT("FTPartyVan: out on the %s side."), ExitSign > 0.f ? TEXT("right") : TEXT("left"));
}

void AFTPartyVan::PlaceOnGround(bool bFirst)
{
	const float C = FMath::Cos(State.heading), S = FMath::Sin(State.heading);
	static const float XS[4] = {1.f, 1.f, -1.f, -1.f};
	static const float YS[4] = {-1.f, 1.f, -1.f, 1.f};
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FTVanWheel), false, this);
	if (const AFTExploreCharacter* Lead = Driver.Get())
	{
		Params.AddIgnoredActor(Lead);
	}
	const float From = bFirst ? GetActorLocation().Z + 500.f : GroundZ + 120.f;
	const float To = bFirst ? From - 10000.f : GroundZ - 300.f;
	for (int I = 0; I < 4; ++I)
	{
		const float LX = XS[I] * HalfWheelbase, LY = YS[I] * HalfTrack;
		const FVector2D P(State.x + LX * C - LY * S, State.y + LX * S + LY * C);
		FHitResult Hit;
		if (GetWorld()->LineTraceSingleByChannel(Hit, FVector(P.X, P.Y, From), FVector(P.X, P.Y, To), ECC_Visibility, Params))
		{
			const float Z = static_cast<float>(Hit.ImpactPoint.Z);
			// A rise under a front wheel going forward is a kerb edge (Core slows the van for it).
			if (!bFirst && I < 2 && WheelHit[I] && State.forward > 0.f)
			{
				const float Rise = Z - WheelGround[I];
				if (Rise > 2.f && Rise <= Drive.tuning().kerbMax + 1.f)
				{
					Drive.mountKerb(State, Rise);
					bKerbTouched = true;
				}
			}
			WheelGround[I] = Z;
			WheelHit[I] = true;
		}
	}
	GroundZ = (WheelGround[0] + WheelGround[1] + WheelGround[2] + WheelGround[3]) * 0.25f;
}

void AFTPartyVan::MoveBody(float DeltaSeconds)
{
	const bool bCanDrive = Seats.canDrive();
	if (!bScript)
	{
		if (!bAxisFresh)
		{
			DriveAxis = FVector2D::ZeroVector;
		}
		bAxisFresh = false;
	}
	ftcore::VehicleInput In;
	if (bCanDrive)
	{
		In.throttle = FMath::Max(0.f, static_cast<float>(DriveAxis.Y));
		In.brake = FMath::Max(0.f, static_cast<float>(-DriveAxis.Y));
		In.steer = FMath::Clamp(static_cast<float>(DriveAxis.X), -1.f, 1.f);
	}
	In.handbrake = bHandbrake || !bCanDrive;
	ftcore::holdForSeats(Seats, In);
	// No pedestrian sensing yet: none ahead (-1), so the road's limit stands alone.
	ftcore::applySpeedLimits(Rules.pedestrian, In, LimitKmh * KmhToCms, -1.f, 0.f);
	Drive.step(State, In, DeltaSeconds);
	ftcore::addDistance(Rules.wear, Wear, State.speed() * DeltaSeconds);

	PlaceOnGround(false);
	FHitResult Hit;
	SetActorLocationAndRotation(FVector(State.x, State.y, GroundZ + Clearance + BodyHalfZ),
		FRotator(0.0, FMath::RadiansToDegrees(State.heading), 0.0), true, &Hit);
	if (Hit.bBlockingHit && FMath::Abs(Hit.ImpactNormal.Z) < 0.7)
	{
		const FVector At = GetActorLocation();
		State.x = static_cast<float>(At.X);
		State.y = static_cast<float>(At.Y);
		const float Into = Drive.collide(State, static_cast<float>(Hit.ImpactNormal.X), static_cast<float>(Hit.ImpactNormal.Y));
		const FVector Local = GetActorTransform().InverseTransformPosition(Hit.ImpactPoint);
		ftcore::addImpact(Rules.wear, Wear, Into, static_cast<float>(Local.X), static_cast<float>(Local.Y));
	}

	// The body tilts with the ground under the wheels; the wheels sit on their own ground, spin and steer.
	const float Pitch = FMath::RadiansToDegrees(FMath::Atan2((WheelGround[0] + WheelGround[1] - WheelGround[2] - WheelGround[3]) * 0.5f, 2.f * HalfWheelbase));
	const float Roll = FMath::RadiansToDegrees(FMath::Atan2((WheelGround[0] + WheelGround[2] - WheelGround[1] - WheelGround[3]) * 0.5f, 2.f * HalfTrack));
	Tilt->SetRelativeRotation(FRotator(Pitch, 0.0, Roll));
	Spin = FMath::Fmod(Spin + FMath::RadiansToDegrees(State.forward / WheelRadius) * DeltaSeconds, 360.f);
	const float Steer = FMath::RadiansToDegrees(State.steer);
	const float BoxZ = GroundZ + Clearance + BodyHalfZ;
	static const float XS[4] = {1.f, 1.f, -1.f, -1.f};
	static const float YS[4] = {-1.f, 1.f, -1.f, 1.f};
	for (int I = 0; I < 4; ++I)
	{
		const bool bRight = YS[I] > 0.f; // the wheel's outer face is its -Y side: the right ones turn round
		const float Yaw = (I < 2 ? Steer : 0.f) + (bRight ? 180.f : 0.f);
		Wheels[I]->SetRelativeLocationAndRotation(FVector(XS[I] * HalfWheelbase, YS[I] * HalfTrack, WheelGround[I] + WheelRadius - BoxZ),
			FRotator(bRight ? Spin : -Spin, Yaw, 0.0));
	}
}

void AFTPartyVan::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	DeltaSeconds = FMath::Min(DeltaSeconds, 0.1f);
	if (bCourse)
	{
		TickScript(DeltaSeconds);
	}
	TickSeats(DeltaSeconds);
	MoveBody(DeltaSeconds);
	if (bScript)
	{
		TickChecks();
	}
	// The chase camera swings back behind the van a moment after the look input stops.
	LookRest += DeltaSeconds;
	if (LookRest > 1.5f)
	{
		LookYaw = FMath::FInterpTo(LookYaw, 0.f, DeltaSeconds, 2.f);
	}
	if (ShotPending.IsEmpty())
	{
		Boom->SetRelativeRotation(FRotator(-12.0, LookYaw, 0.0));
	}
}

void AFTPartyVan::TickScript(float DeltaSeconds)
{
	ScriptTime += DeltaSeconds;
	AFTExploreCharacter* Lead = Cast<AFTExploreCharacter>(UGameplayStatics::GetPlayerPawn(this, 0));
	if (ScriptStage == 0 && ScriptTime > 1.f && Lead)
	{
		// The lead stands at the driver's door.
		const FVector At = GetActorTransform().TransformPosition(FVector(60.0, (bRightHandDrive ? 1.0 : -1.0) * (BodyBox.Y * 0.5 + 60.0), 0.0));
		Lead->SetActorLocation(FVector(At.X, At.Y, GroundZ + 100.0), false, nullptr, ETeleportType::TeleportPhysics);
		ScriptStage = bScript ? 1 : 9;
	}
	if (!bScript)
	{
		return;
	}
	if (ScriptStage == 1 && ScriptTime > 2.f)
	{
		if (TryEnter(Lead))
		{
			ScriptStage = 2;
		}
	}
	if (ScriptStage == 2 && Seats.canDrive())
	{
		ScriptStage = 3;
		UE_LOG(LogForgottenThird, Display, TEXT("VANCHECK seated after %.2f s"), ScriptTime);
	}
	if (ScriptStage < 3)
	{
		return;
	}

	// Pure pursuit along the course line, in the course's frame (the van started at its origin facing +X).
	const float C0 = FMath::Cos(StartHeading), S0 = FMath::Sin(StartHeading);
	const FVector2D Rel(State.x - Start.X, State.y - Start.Y);
	const FVector2D P(Rel.X * C0 + Rel.Y * S0, -Rel.X * S0 + Rel.Y * C0);
	const float Along = CourseDistance(P);
	Travelled = Along;
	LimitKmh = Along > 10000.f ? 80.f : 60.f;
	const float Speed = State.forward;
	const FVector2D Target = CoursePoint(Along + 600.f + 0.3f * FMath::Max(Speed, 0.f));
	const float H = State.heading - StartHeading;
	const float Alpha = FMath::FindDeltaAngleRadians(H, FMath::Atan2(static_cast<float>(Target.Y - P.Y), static_cast<float>(Target.X - P.X)));
	const float Ld = FMath::Max(100.f, static_cast<float>(FVector2D::Distance(Target, P)));
	const float Delta = FMath::Atan(2.f * Drive.tuning().wheelbase * FMath::Sin(Alpha) / Ld);
	const float Steer = FMath::Clamp(Delta / FMath::DegreesToRadians(Drive.tuning().maxSteer), -1.f, 1.f);

	float Want = LimitKmh * KmhToCms; // flat out to the limit
	if (Along > 12000.f && Along < CornerX + 1500.f)
	{
		Want = 25.f * KmhToCms; // slow for the corner
	}
	else if (Along >= CornerX + 1500.f)
	{
		Want = 40.f * KmhToCms;
	}
	if (Along > 19000.f)
	{
		Want = 0.f;
	}
	float Pedal = FMath::Clamp((Want - Speed) / 300.f, -1.f, 1.f);
	if (Want == 0.f && Speed < 30.f)
	{
		Pedal = 0.f;
	}
	DriveAxis = FVector2D(Steer, Pedal);
	bHandbrake = Want == 0.f && Speed < 30.f;
	Peak = FMath::Max(Peak, Speed);

	// Views: the kerb side-on, behind on the straight at speed, top-down in the turn.
	if (!ShotPrefix.IsEmpty())
	{
		if (Shots == 0 && Along > KerbFrom + 1200.f) { Shot(TEXT("side"), -90.f, -5.f, 700.f); }
		else if (Shots == 1 && Along > 11000.f) { Shot(TEXT("behind"), 0.f, -10.f, 750.f); }
		else if (Shots == 2 && Along > CornerX - 200.f) { Shot(TEXT("top"), 0.f, -89.f, 2500.f); }
	}
	if (!ShotPending.IsEmpty() && ScriptTime - LastShotAt > 0.15f && !bShotTaken)
	{
		const FString File = FString::Printf(TEXT("%s_%s.png"), *ShotPrefix, *ShotPending);
		if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
		{
			Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *File), *GLog);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("VANCHECK view %s at %.1f m"), *File, Along / 100.f);
		bShotTaken = true;
	}
	if (!ShotPending.IsEmpty() && ScriptTime - LastShotAt > 0.5f)
	{
		ShotPending.Reset();
		bShotTaken = false;
		Boom->TargetArmLength = 750.f;
		Boom->bEnableCameraRotationLag = true;
	}

	// Stopped at the end: out, then the verdict.
	if (ScriptStage == 3 && Along > 19000.f && State.speed() < 5.f)
	{
		bExitHeld = true;
		ScriptStage = 4;
	}
	if (ScriptStage == 4 && Seats.phase == ftcore::SeatPhase::OnFoot)
	{
		ScriptStage = 5;
		EndAt = ScriptTime + 1.f;
	}
	if (ScriptStage == 5 && ScriptTime > EndAt)
	{
		if (!bKerbTouched)
		{
			++Fails;
			UE_LOG(LogForgottenThird, Warning, TEXT("VANCHECK FAIL the kerb was never touched"));
		}
		UE_LOG(LogForgottenThird, Display, TEXT("VANCHECK %s: %.1f m driven, peak %.1f km/h, kerb %d, %d fails, %d views, wear F%d R%d L%d R%d"),
			Fails == 0 ? TEXT("PASS") : TEXT("FAIL"), Travelled / 100.f, Peak / KmhToCms, bKerbTouched ? 1 : 0, Fails, Shots,
			ftcore::wearStage(Wear, ftcore::WearZone::Front), ftcore::wearStage(Wear, ftcore::WearZone::Rear),
			ftcore::wearStage(Wear, ftcore::WearZone::Left), ftcore::wearStage(Wear, ftcore::WearZone::Right));
		ScriptStage = 6;
		FPlatformMisc::RequestExitWithStatus(false, Fails == 0 ? 0 : 1, TEXT("Van"));
	}
	// A run that never finishes fails.
	if (ScriptStage < 5 && ScriptTime > 90.f)
	{
		++Fails;
		UE_LOG(LogForgottenThird, Warning, TEXT("VANCHECK FAIL timed out at stage %d, %.1f m"), ScriptStage, Along / 100.f);
		ScriptStage = 5;
		EndAt = ScriptTime;
	}
}

void AFTPartyVan::Shot(const TCHAR* Name, float CamYaw, float CamPitch, float Arm)
{
	ShotPending = Name;
	LastShotAt = ScriptTime;
	bShotTaken = false;
	++Shots;
	Boom->bEnableCameraRotationLag = false;
	Boom->TargetArmLength = Arm;
	Boom->SetRelativeRotation(FRotator(CamPitch, CamYaw, 0.0));
}

void AFTPartyVan::TickChecks()
{
	if (ScriptStage < 3 || ScriptStage > 4)
	{
		return;
	}
	auto Fail = [this](const FString& What)
	{
		if (++Fails <= 10)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("VANCHECK FAIL %s at %.1f m"), *What, Travelled / 100.f);
		}
	};
	const float Limit = LimitKmh * KmhToCms;
	if (State.forward > Limit * 1.02f + 5.f)
	{
		Fail(FString::Printf(TEXT("speed %.1f km/h over the %.0f limit"), State.forward / KmhToCms, LimitKmh));
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FTVanCheck), false, this);
	if (const AFTExploreCharacter* Lead = Driver.Get())
	{
		Params.AddIgnoredActor(Lead);
	}
	const FVector Ext = Box->GetScaledBoxExtent() - FVector(2.0);
	if (GetWorld()->OverlapBlockingTestByChannel(GetActorLocation(), GetActorQuat(), Box->GetCollisionObjectType(), FCollisionShape::MakeBox(Ext), Params))
	{
		Fail(TEXT("the body overlaps the world after a move (tunnelling)"));
	}
	for (int I = 0; I < 4; ++I)
	{
		const FVector Hub = Wheels[I]->GetComponentLocation();
		FHitResult Hit;
		if (GetWorld()->LineTraceSingleByChannel(Hit, Hub + FVector(0.0, 0.0, 50.0), Hub - FVector(0.0, 0.0, 200.0), ECC_Visibility, Params))
		{
			const float Gap = static_cast<float>(Hub.Z - Hit.ImpactPoint.Z) - WheelRadius;
			if (FMath::Abs(Gap) > 2.f)
			{
				Fail(FString::Printf(TEXT("wheel %d is %.1f cm off the ground"), I, Gap));
			}
		}
	}
	if (ScriptTime >= NextLog)
	{
		NextLog = ScriptTime + 1.f;
		UE_LOG(LogForgottenThird, Display, TEXT("VAN %.1f s: %.1f m, %.1f km/h (limit %.0f), steer %.1f deg, kerb %d"),
			ScriptTime, Travelled / 100.f, State.forward / KmhToCms, LimitKmh, FMath::RadiansToDegrees(State.steer), bKerbTouched ? 1 : 0);
	}
}

AFTPartyVan* AFTPartyVan::SpawnCourse(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	auto Slab = [World, Cube](const FVector& Centre, const FVector& Size)
	{
		AStaticMeshActor* A = World->SpawnActor<AStaticMeshActor>(Centre, FRotator::ZeroRotator);
		if (A)
		{
			A->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			A->GetStaticMeshComponent()->SetStaticMesh(Cube);
			A->SetActorScale3D(Size / 100.0);
		}
	};
	const FVector O = CourseOrigin;
	// Ground: the straight (from 20 m behind the start), the second leg; tops at the origin's height.
	Slab(O + FVector(CornerX * 0.5 - 1000.0, 0.0, -50.0), FVector(CornerX + 4000.0, 2000.0, 100.0));
	Slab(O + FVector(CornerX, -LegTwo * 0.5 - 1500.0, -50.0), FVector(2000.0, LegTwo + 3000.0, 100.0));
	// The kerb strip on the right of the straight.
	Slab(O + FVector((KerbFrom + KerbTo) * 0.5, KerbInner + KerbWidth * 0.5, KerbHeight * 0.5), FVector(KerbTo - KerbFrom, KerbWidth, KerbHeight));
	// A post every 25 m, left of the line.
	for (float S = 0.f; S <= CornerX + LegTwo + 1.f; S += 2500.f)
	{
		const FVector2D P = CoursePoint(S);
		const FVector2D Side = S <= CornerX ? FVector2D(0.0, -600.0) : FVector2D(600.0, 0.0);
		Slab(O + FVector(P.X + Side.X, P.Y + Side.Y, 75.0), FVector(20.0, 20.0, 150.0));
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AFTPartyVan* Van = World->SpawnActor<AFTPartyVan>(O + FVector(0.0, 0.0, 200.0), FRotator::ZeroRotator, Params);
	UE_LOG(LogForgottenThird, Display, TEXT("FTPartyVan: -VanCourse laid at %s."), *O.ToString());
	return Van;
}
