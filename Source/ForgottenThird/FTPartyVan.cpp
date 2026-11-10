#include "FTPartyVan.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/SpringArmComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "FTCrowdSubsystem.h"
#include "FTExploreCharacter.h"
#include "FTPartyFollower.h"
#include "FTWorldClockSubsystem.h"
#include "ForgottenThird.h"

#include <string>

namespace
{
constexpr float KmhToCms = 100000.f / 3600.f;

// The lamps on the body, in its mesh frame (cm, from the side and rear renders): head lamps 25 by 14 at the front
// corners, tail lamps 10 wide and 28 tall at the back. Glow is the lens's emissive scale when lit.
const FVector LensAt[4] = {FVector(174.0, -51.0, 77.0), FVector(174.0, 51.0, 77.0), FVector(-153.0, -66.0, 83.0), FVector(-153.0, 66.0, 83.0)};
const FVector LensSize[4] = {FVector(4.0, 25.0, 14.0), FVector(4.0, 25.0, 14.0), FVector(4.0, 10.0, 28.0), FVector(4.0, 10.0, 28.0)};
constexpr float HeadGlow = 25.f, TailGlowOn = 4.f, BrakeGlow = 12.f;
constexpr float HeadCandela = 1500.f, HeadReach = 3000.f; // a low beam's spread; street lamps are 400 cd
// The tail light is a spot under the rear bumper aimed back and down, so it lights a small pool on the road behind
// and never the van's own rear.
constexpr float TailCandela = 3.f, BrakeCandela = 8.f, TailReach = 250.f;
const FVector TailGlowAt(-175.0, 0.0, 40.0);
const FRotator TailGlowAim(-60.0, 180.0, 0.0);
const FLinearColor GlassColor(0.015f, 0.018f, 0.022f);
const TCHAR* const GlassMeshPath = TEXT("/Game/Vehicles/PartyVanGlass/SM_PartyVanGlass/StaticMeshes/SM_PartyVanGlass.SM_PartyVanGlass");
constexpr float LookEvery = 0.5f;
const TCHAR* const BodyLookPath = TEXT("/Game/Vehicles/PartyVan/M_VanBody.M_VanBody");
const TCHAR* const LensLookPath = TEXT("/Game/Vehicles/PartyVan/M_VanLamp.M_VanLamp");

// -VanCourse, in the course's frame (cm): the straight along +X, the left turn at 150 m, then along -Y.
const FVector CourseOrigin(0.0, 300000.0, 20000.0);
constexpr float CornerX = 15000.f;
constexpr float LegTwo = 5000.f;
constexpr float KerbFrom = 5000.f, KerbTo = 9000.f, KerbInner = 150.f, KerbWidth = 30.f, KerbHeight = 12.f;

// The point `S` cm along the course's centre line (the kerb stretch shifted toward the kerb).
FVector2D CoursePoint(float S, bool bStraight)
{
	if (bStraight)
	{
		return FVector2D(S, 0.f);
	}
	if (S <= CornerX)
	{
		const float Shift = (S > KerbFrom - 1000.f && S < KerbTo) ? 110.f : 0.f;
		return FVector2D(S, Shift);
	}
	return FVector2D(CornerX, -(S - CornerX));
}

// How far along the course the point P lies (the nearer leg's projection).
float CourseDistance(const FVector2D& P, bool bStraight)
{
	if (bStraight)
	{
		return FMath::Max(0.f, static_cast<float>(P.X));
	}
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

	static const TCHAR* LensNames[4] = {TEXT("HeadLensL"), TEXT("HeadLensR"), TEXT("TailLensL"), TEXT("TailLensR")};
	for (int I = 0; I < 4; ++I)
	{
		Lens[I] = CreateDefaultSubobject<UStaticMeshComponent>(LensNames[I]);
		Lens[I]->SetupAttachment(Tilt);
		Lens[I]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Lens[I]->SetCastShadow(false);
	}
	// Unshadowed lights, hidden by day: two beams ahead and a red glow behind. Their reach is short and they stay off
	// until the clock's night band, so the pooled street lights' budget holds.
	static const TCHAR* BeamNames[2] = {TEXT("HeadBeamL"), TEXT("HeadBeamR")};
	for (int I = 0; I < 2; ++I)
	{
		HeadBeam[I] = CreateDefaultSubobject<USpotLightComponent>(BeamNames[I]);
		HeadBeam[I]->SetupAttachment(Tilt);
		HeadBeam[I]->SetCastShadows(false);
		HeadBeam[I]->SetIntensityUnits(ELightUnits::Candelas);
		HeadBeam[I]->SetIntensity(HeadCandela);
		HeadBeam[I]->SetAttenuationRadius(HeadReach);
		HeadBeam[I]->SetOuterConeAngle(32.f);
		HeadBeam[I]->SetInnerConeAngle(14.f);
		HeadBeam[I]->SetLightColor(FLinearColor(1.f, 0.93f, 0.82f));
		HeadBeam[I]->SetVisibility(false);
		HeadBeam[I]->SetMobility(EComponentMobility::Movable);
	}
	TailGlow = CreateDefaultSubobject<USpotLightComponent>(TEXT("TailGlow"));
	TailGlow->SetupAttachment(Tilt);
	TailGlow->SetCastShadows(false);
	TailGlow->SetIntensityUnits(ELightUnits::Candelas);
	TailGlow->SetIntensity(TailCandela);
	TailGlow->SetAttenuationRadius(TailReach);
	TailGlow->SetLightColor(FLinearColor(1.f, 0.05f, 0.02f));
	TailGlow->SetOuterConeAngle(50.f);
	TailGlow->SetInnerConeAngle(20.f);
	TailGlow->SetVisibility(false);
	TailGlow->SetMobility(EComponentMobility::Movable);
	Glass = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Glass"));
	Glass->SetupAttachment(Tilt);
	Glass->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Glass->SetCastShadow(false);
	Glass->SetVisibility(false);
}

void AFTPartyVan::SetupLook(bool bBodyAsset)
{
	// The body's painted slot takes M_VanBody (glass and wear); the arch liners keep theirs. Without the material, or
	// on the stand-in box, the body stays as it was.
	UMaterialInterface* BodyMaster = bBodyAsset ? LoadObject<UMaterialInterface>(nullptr, BodyLookPath) : nullptr;
	if (BodyMaster)
	{
		for (int32 Slot = 0; Slot < Body->GetNumMaterials(); ++Slot)
		{
			const UMaterialInterface* Was = Body->GetMaterial(Slot);
			if (Was && !Was->GetName().Contains(TEXT("ArchLiner")))
			{
				if (!BodyLook)
				{
					BodyLook = UMaterialInstanceDynamic::Create(BodyMaster, this);
				}
				Body->SetMaterial(Slot, BodyLook);
			}
		}
	}
	UMaterialInterface* LensMaster = bBodyAsset ? LoadObject<UMaterialInterface>(nullptr, LensLookPath) : nullptr;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (LensMaster)
	{
		const FLinearColor Colors[2] = {FLinearColor(1.f, 0.95f, 0.85f), FLinearColor(0.75f, 0.02f, 0.01f)};
		for (int I = 0; I < 2; ++I)
		{
			LensLook[I] = UMaterialInstanceDynamic::Create(LensMaster, this);
			LensLook[I]->SetVectorParameterValue(TEXT("Color"), Colors[I]);
			LensLook[I]->SetScalarParameterValue(TEXT("Glow"), 0.f);
		}
	}
	for (int I = 0; I < 4; ++I)
	{
		Lens[I]->SetStaticMesh(Cube);
		Lens[I]->SetRelativeLocation(LensAt[I]);
		Lens[I]->SetRelativeScale3D(LensSize[I] / 100.0);
		if (LensLook[I / 2])
		{
			Lens[I]->SetMaterial(0, LensLook[I / 2]);
		}
		Lens[I]->SetVisibility(LensLook[I / 2] != nullptr);
	}
	for (int I = 0; I < 2; ++I)
	{
		HeadBeam[I]->SetRelativeLocationAndRotation(LensAt[I] + FVector(8.0, 0.0, 0.0), FRotator(-4.0, 0.0, 0.0));
	}
	TailGlow->SetRelativeLocationAndRotation(TailGlowAt, TailGlowAim);
	// The cab door panes, dark glass in the body's mesh frame; left out without the mesh or the lamp material.
	UStaticMesh* Panes = bBodyAsset ? LoadObject<UStaticMesh>(nullptr, GlassMeshPath) : nullptr;
	if (Panes && LensMaster)
	{
		GlassLook = UMaterialInstanceDynamic::Create(LensMaster, this);
		GlassLook->SetVectorParameterValue(TEXT("Color"), GlassColor);
		GlassLook->SetScalarParameterValue(TEXT("Glow"), 0.f);
		Glass->SetStaticMesh(Panes);
		Glass->SetRelativeTransform(Body->GetRelativeTransform());
		for (int32 Slot = 0; Slot < Glass->GetNumMaterials(); ++Slot)
		{
			Glass->SetMaterial(Slot, GlassLook);
		}
	}
	Glass->SetVisibility(Panes && GlassLook);

	// -VanWear=front,rear,left,right,dirt (0 to 4 each) starts the van that worn, for a look at the wear.
	FString WearText;
	if (FParse::Value(FCommandLine::Get(), TEXT("VanWear="), WearText, false))
	{
		TArray<FString> Parts;
		WearText.ParseIntoArray(Parts, TEXT(","));
		for (int32 I = 0; I < FMath::Min(Parts.Num(), 5); ++I)
		{
			const float V = FMath::Clamp(FCString::Atof(*Parts[I]), 0.f, 4.f);
			if (I < ftcore::kWearZones)
			{
				Wear.zone[I] = V;
			}
			else
			{
				Wear.dirt = V;
			}
		}
	}
	ShowWear();
	LookAge = LookEvery; // the first tick reads the clock
}

void AFTPartyVan::ShowWear()
{
	if (!BodyLook)
	{
		return;
	}
	static const FName Names[5] = {TEXT("WearFront"), TEXT("WearRear"), TEXT("WearLeft"), TEXT("WearRight"), TEXT("Dirt")};
	const int32 Now[5] = {ftcore::wearStage(Wear, ftcore::WearZone::Front), ftcore::wearStage(Wear, ftcore::WearZone::Rear),
		ftcore::wearStage(Wear, ftcore::WearZone::Left), ftcore::wearStage(Wear, ftcore::WearZone::Right), ftcore::dirtStage(Wear)};
	for (int I = 0; I < 5; ++I)
	{
		if (Now[I] != WearShown[I])
		{
			WearShown[I] = Now[I];
			BodyLook->SetScalarParameterValue(Names[I], static_cast<float>(Now[I]) / static_cast<float>(ftcore::kWearMaxStage));
		}
	}
}

void AFTPartyVan::TickLamps(float DeltaSeconds)
{
	LookAge += DeltaSeconds;
	if (LookAge >= LookEvery)
	{
		LookAge = 0.f;
		const UFTWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UFTWorldClockSubsystem>();
		bNight = Clock && Clock->IsRunning() && ftcore::nightBand(Clock->GetNightAmount(), bNight);
		ShowWear();
	}
	// A driver in the seat: settled, or still in it while getting out (Entering is still on foot for the lamps).
	const bool bDriver = Seats.phase == ftcore::SeatPhase::Seated || Seats.phase == ftcore::SeatPhase::Exiting;
	const ftcore::LampState L = ftcore::lampState(bNight, LampBrake, bLampHandbrake, bDriver);
	if (bLampsSet && L.head == Lamps.head && L.tail == Lamps.tail && L.brake == Lamps.brake)
	{
		return;
	}
	bLampsSet = true;
	Lamps = L;
	for (int I = 0; I < 2; ++I)
	{
		HeadBeam[I]->SetVisibility(L.head);
	}
	TailGlow->SetVisibility(L.tail || L.brake);
	TailGlow->SetIntensity(L.brake ? BrakeCandela : TailCandela);
	if (LensLook[0])
	{
		LensLook[0]->SetScalarParameterValue(TEXT("Glow"), L.head ? HeadGlow : 0.f);
	}
	if (LensLook[1])
	{
		LensLook[1]->SetScalarParameterValue(TEXT("Glow"), L.brake ? BrakeGlow : (L.tail ? TailGlowOn : 0.f));
	}
}

void AFTPartyVan::BeginPlay()
{
	Super::BeginPlay();
	PedCache.Reserve(64);
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
	SetupLook(BodyAsset != nullptr);
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
	FString At3;
	bMapRoute = !bCourse && FParse::Value(*Cmd, TEXT("PartyVanAt="), At3, false);
	bScript = (bCourse || bMapRoute) && FParse::Param(*Cmd, TEXT("VanDrive"));
	bMapRoute = bMapRoute && bScript;
	float RouteM = 80.f;
	if (FParse::Value(*Cmd, TEXT("VanRoute="), RouteM))
	{
		RouteLen = FMath::Clamp(RouteM, 10.f, 2000.f) * 100.f;
	}
	FParse::Value(*Cmd, TEXT("VanShots="), ShotPrefix);

	const FVector At = GetActorLocation();
	State = Drive.parked(At.X, At.Y, FMath::DegreesToRadians(GetActorRotation().Yaw));
	Start = FVector2D(At.X, At.Y);
	StartHeading = State.heading;
	PlaceOnGround(true);
	SetActorLocationAndRotation(FVector(State.x, State.y, GroundZ + Clearance + BodyHalfZ), FRotator(0.0, GetActorRotation().Yaw, 0.0));

	FString LoopText;
	if (bMapRoute && FParse::Value(FCommandLine::Get(), TEXT("VanLoop="), LoopText, false))
	{
		TArray<FString> Pts;
		LoopText.ParseIntoArray(Pts, TEXT(","));
		const float C0 = FMath::Cos(StartHeading), S0 = FMath::Sin(StartHeading);
		Loop[0] = FVector2D::ZeroVector;
		int32 N = 1;
		for (const FString& T : Pts)
		{
			FString A, B;
			if (N > MaxLoop || !T.Split(TEXT(":"), &A, &B))
			{
				continue;
			}
			const FVector2D Rel(FCString::Atod(*A) - Start.X, FCString::Atod(*B) - Start.Y);
			Loop[N++] = FVector2D(Rel.X * C0 + Rel.Y * S0, -Rel.X * S0 + Rel.Y * C0);
		}
		Loop[N++] = FVector2D::ZeroVector; // and back to the start
		LoopCum[0] = 0.f;
		for (int32 I = 1; I < N; ++I)
		{
			LoopCum[I] = LoopCum[I - 1] + static_cast<float>(FVector2D::Distance(Loop[I - 1], Loop[I]));
		}
		LoopN = N >= 3 ? N : 0;
		if (LoopN > 0)
		{
			RouteLen = LoopCum[LoopN - 1];
			UE_LOG(LogForgottenThird, Display, TEXT("FTPartyVan: loop of %d points, %.0f m."), LoopN - 2, RouteLen / 100.f);
		}
	}
}

FVector2D AFTPartyVan::RoutePoint(float S) const
{
	S = FMath::Clamp(S, 0.f, LoopCum[LoopN - 1]);
	for (int32 I = 1; I < LoopN; ++I)
	{
		if (S <= LoopCum[I] || I == LoopN - 1)
		{
			const float Len = FMath::Max(LoopCum[I] - LoopCum[I - 1], 1.f);
			return FMath::Lerp(Loop[I - 1], Loop[I], FMath::Clamp((S - LoopCum[I - 1]) / Len, 0.f, 1.f));
		}
	}
	return Loop[0];
}

float AFTPartyVan::RouteDistance(const FVector2D& P, float Near) const
{
	// The nearest projection among the legs near the distance already driven (the loop meets itself at the start).
	float Best = Near, BestD = 1e30f;
	for (int32 I = 1; I < LoopN; ++I)
	{
		if (LoopCum[I] < Near - 2000.f || LoopCum[I - 1] > Near + 4000.f)
		{
			continue;
		}
		const FVector2D A = Loop[I - 1], D = Loop[I] - Loop[I - 1];
		const float Len2 = FMath::Max(static_cast<float>(D.SizeSquared()), 1.f);
		const float T = FMath::Clamp(static_cast<float>(FVector2D::DotProduct(P - A, D)) / Len2, 0.f, 1.f);
		const float Dist = static_cast<float>(FVector2D::Distance(P, A + D * T));
		if (Dist < BestD)
		{
			BestD = Dist;
			Best = LoopCum[I - 1] + T * FMath::Sqrt(Len2);
		}
	}
	return Best;
}

float AFTPartyVan::CornerSpeed(float Along) const
{
	// Down to 14 km/h for each turn of more than 30 degrees, braking at 4 m/s2 into it.
	constexpr float Turn = 14.f * KmhToCms, Brake = 400.f;
	float Want = 1e9f;
	for (int32 I = 1; I < LoopN - 1; ++I)
	{
		const FVector2D In = (Loop[I] - Loop[I - 1]).GetSafeNormal(), Out = (Loop[I + 1] - Loop[I]).GetSafeNormal();
		if (FVector2D::DotProduct(In, Out) > 0.866)
		{
			continue;
		}
		const float D = LoopCum[I] - Along;
		if (D < -500.f || D > 3000.f)
		{
			continue;
		}
		Want = FMath::Min(Want, FMath::Sqrt(Turn * Turn + 2.f * Brake * FMath::Max(D - 150.f, 0.f)));
	}
	return Want;
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
	if (!Lead || Driver.IsValid() || !Lead->CanBoardVan())
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
	ClearHeldInput();
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
	// The doors' sides are only looked at while the way out is held.
	const bool bDriverClear = !bExitHeld || SideClear(DriverSign);
	const bool bPassengerClear = !bExitHeld || SideClear(-DriverSign);
	ftcore::tickSeats(Rules.seats, Seats, DeltaSeconds, State.speed(), bExitHeld, bDriverClear, bPassengerClear);
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
	ClearHeldInput(); // a handbrake or stick held through the exit goes to the foot path on release
	if (!Lead)
	{
		return;
	}
	const FVector At = GetActorTransform().TransformPosition(FVector(30.0, ExitSign * (BodyBox.Y * 0.5 + 70.0), 0.0));
	Lead->LeaveVan(FVector(At.X, At.Y, GroundZ + 100.0), GetActorRotation().Yaw);
	UE_LOG(LogForgottenThird, Display, TEXT("FTPartyVan: out on the %s side."), ExitSign > 0.f ? TEXT("right") : TEXT("left"));
}

void AFTPartyVan::ClearHeldInput()
{
	DriveAxis = FVector2D::ZeroVector;
	bAxisFresh = false;
	bHandbrake = false;
	bExitHeld = false;
}

void AFTPartyVan::SensePedestrian(float& Ahead, float& Side, AActor*& Who, int32& CrowdSlot)
{
	Ahead = ftcore::kNobodyAhead;
	Side = 0.f;
	Who = nullptr;
	CrowdSlot = -1;
	// Looked for out to the braking distance from the top speed, and across the pavements on both sides.
	constexpr float Range = 4000.f, Across = 450.f;
	const FTransform& T = GetActorTransform();
	const float Front = static_cast<float>(BodyBox.X) * 0.5f;
	const float Behind = -Rules.pedestrian.clearance;
	auto Consider = [&](const FVector& P) -> bool
	{
		const FVector L = T.InverseTransformPosition(P);
		const float A = static_cast<float>(L.X) - Front;
		const float S = static_cast<float>(L.Y);
		if (A < Behind || A > Range || FMath::Abs(S) > Across || FMath::Abs(L.Z) > 400.0 || A >= Ahead)
		{
			return false;
		}
		Ahead = A;
		Side = S;
		return true;
	};
	// The world's walkers are gathered a few times a second into the reserved list, not iterated every frame.
	constexpr float PedRefresh = 0.25f;
	if (PedCacheAge >= PedRefresh)
	{
		PedCacheAge = 0.f;
		PedCache.Reset();
		for (TActorIterator<ACharacter> It(GetWorld()); It && PedCache.Num() < 64; ++It)
		{
			PedCache.Add(*It);
		}
		for (TActorIterator<AFTPartyFollower> It(GetWorld()); It && PedCache.Num() < 64; ++It)
		{
			PedCache.Add(*It);
		}
	}
	const AFTExploreCharacter* Lead = Driver.Get();
	for (const TWeakObjectPtr<AActor>& W : PedCache)
	{
		AActor* A = W.Get();
		if (A && A != Lead && !A->IsHidden() && A->GetActorEnableCollision() && Consider(A->GetActorLocation()))
		{
			Who = A;
		}
	}
	if (const UFTCrowdSubsystem* Crowd = UFTCrowdSubsystem::Get(GetWorld()))
	{
		const ftcore::CrowdSim& Sim = Crowd->GetSim();
		const float Z = static_cast<float>(GetActorLocation().Z);
		for (uint32 I = 0; I < Sim.capacity(); ++I)
		{
			const ftcore::CrowdAgentView& V = Sim.agent(I);
			if (V.live && !V.indoors && Consider(FVector(V.pos.x * 100.0, V.pos.y * 100.0, Z)))
			{
				Who = nullptr;
				CrowdSlot = static_cast<int32>(I);
			}
		}
	}
}

void AFTPartyVan::ReactPedestrian(float Ahead, float Side, AActor* Who, int32 CrowdSlot)
{
	if (Ahead >= ftcore::kNobodyAhead || Ahead > Rules.pedestrian.nearDistance)
	{
		LastPedChar.Reset(); // nobody close: the next approach reacts afresh
		LastPedSlot = -1;
		return;
	}
	if ((Who && LastPedChar.Get() == Who) || (CrowdSlot >= 0 && CrowdSlot == LastPedSlot))
	{
		return; // once per approach
	}
	const ftcore::PedOutcome O = ftcore::pedestrianReact(Rules.pedestrian, State.speed(), Ahead, Side);
	if (O.react == ftcore::PedReact::None)
	{
		return;
	}
	LastPedChar = Who;
	LastPedSlot = CrowdSlot;
	const FVector Right = GetActorRightVector();
	const FVector At = GetActorTransform().TransformPosition(FVector(BodyBox.X * 0.5 + Ahead, Side, 0.0));
	if (ACharacter* Walker = Cast<ACharacter>(Who))
	{
		// A character steps aside, or is pushed down the way the van is going (its own fall and get-up are its own).
		const FVector Push = O.react == ftcore::PedReact::Dodge
			? Right * (O.sidestep > 0.f ? 1.0 : -1.0) * Rules.pedestrian.sidestepSpeed
			: GetActorForwardVector() * O.contact + FVector(0.0, 0.0, O.react == ftcore::PedReact::KnockDown ? 250.0 : 80.0);
		Walker->LaunchCharacter(Push, true, false);
	}
	else if (UFTCrowdSubsystem* Crowd = Who ? nullptr : UFTCrowdSubsystem::Get(GetWorld()))
	{
		// The crowd scatters from a van passing close, and a knock-down frightens it like a fall.
		Crowd->RaiseEvent(O.react == ftcore::PedReact::Dodge ? ftcore::CrowdEventKind::PlayerSprint : ftcore::CrowdEventKind::LoudNoise, At);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("VANPED %s %.0f cm ahead, %.0f to the side, at %.1f km/h (contact %.1f)."),
		O.react == ftcore::PedReact::Dodge ? TEXT("dodge") : O.react == ftcore::PedReact::Stumble ? TEXT("stumble") : TEXT("knock-down"),
		Ahead, Side, State.speed() / KmhToCms, O.contact / KmhToCms);
}

void AFTPartyVan::PlaceOnGround(bool bFirst, float Moved)
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
			// A step under a front wheel going forward is a kerb edge (Core slows the van for it). A slope rises with the
			// distance travelled, so only the rise beyond the wheel's last grade counts, whatever the frame rate.
			if (bFirst)
			{
				WheelGrade[I] = 0.f;
			}
			else if (I < 2 && WheelHit[I] && State.forward > 0.f && Moved > 0.5f)
			{
				const float Rise = Z - WheelGround[I];
				const float Step = Rise - WheelGrade[I] * Moved;
				if (Step > FMath::Max(3.f, 0.06f * Moved) && Step <= Drive.tuning().kerbMax + 1.f)
				{
					Drive.mountKerb(State, Step);
					bKerbTouched = true;
				}
				else
				{
					WheelGrade[I] = FMath::Clamp(Rise / Moved, -0.5f, 0.5f);
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
	LampBrake = In.brake;
	bLampHandbrake = In.handbrake;
	// The nearest pedestrian ahead brakes the van down to the zone's cap; they react to it when it is close.
	float PedAhead = ftcore::kNobodyAhead, PedSide = 0.f;
	AActor* PedChar = nullptr;
	int32 PedSlot = -1;
	PedCacheAge += DeltaSeconds;
	SensePedestrian(PedAhead, PedSide, PedChar, PedSlot);
	ftcore::applySpeedLimits(Rules.pedestrian, In, LimitKmh * KmhToCms, PedAhead, PedSide);
	Drive.step(State, In, DeltaSeconds);
	ftcore::addDistance(Rules.wear, Wear, State.speed() * DeltaSeconds);
	ReactPedestrian(PedAhead, PedSide, PedChar, PedSlot);

	PlaceOnGround(false, State.speed() * DeltaSeconds);
	// The flat box rides on the highest wheel's ground, so a slope or a crest under it does not snag it.
	const float Highest = FMath::Max(FMath::Max(WheelGround[0], WheelGround[1]), FMath::Max(WheelGround[2], WheelGround[3]));
	Lift = FMath::Max(Highest - GroundZ, 0.f);
	const FVector Want(State.x, State.y, GroundZ + Lift + Clearance + BodyHalfZ);
	const FRotator WantRot(0.0, FMath::RadiansToDegrees(State.heading), 0.0);
	FHitResult Hit;
	SetActorLocationAndRotation(Want, WantRot, true, &Hit);
	if (Hit.bBlockingHit && Hit.ImpactNormal.Z >= 0.7)
	{
		// Ground it can drive on (a ramp's lip, a terrace edge): step the body up over it and try once more.
		const FVector Up(0.0, 0.0, Drive.tuning().kerbMax + 10.f);
		SetActorLocation(GetActorLocation() + Up, true);
		SetActorLocationAndRotation(Want + Up, WantRot, true, &Hit);
		if (!Hit.bBlockingHit)
		{
			// Over it: back down onto the wheels' height, so body and wheels do not hover on the lip.
			const FVector At = GetActorLocation();
			SetActorLocation(FVector(At.X, At.Y, Want.Z), true);
		}
	}
	if (Hit.bBlockingHit)
	{
		// Stopped short: the model goes where the body is (it never runs on ahead of it).
		const FVector At = GetActorLocation();
		State.x = static_cast<float>(At.X);
		State.y = static_cast<float>(At.Y);
		State.heading = FMath::DegreesToRadians(static_cast<float>(GetActorRotation().Yaw));
		const FVector2D N(Hit.ImpactNormal.X, Hit.ImpactNormal.Y);
		float Into = 0.f;
		if (N.Size() > 0.2)
		{
			Into = Drive.collide(State, static_cast<float>(N.X), static_cast<float>(N.Y));
		}
		else
		{
			// Straight above or below (an overhang, a ledge it cannot step): it stops.
			Into = State.speed();
			State.forward = 0.f;
			State.side = 0.f;
			State.yawRate = 0.f;
		}
		const FVector Local = GetActorTransform().InverseTransformPosition(Hit.ImpactPoint);
		if (bScript && TraceAge > 0.5f)
		{
			TraceAge = 0.f;
			UE_LOG(LogForgottenThird, Display, TEXT("VANTRACE blocked at (%.0f, %.0f, %.0f) by %s normal (%.2f, %.2f, %.2f) local (%.0f, %.0f, %.0f) ground %.0f lift %.0f"),
				At.X, At.Y, At.Z, Hit.GetComponent() ? *Hit.GetComponent()->GetReadableName() : TEXT("?"), Hit.ImpactNormal.X, Hit.ImpactNormal.Y,
				Hit.ImpactNormal.Z, Local.X, Local.Y, Local.Z, GroundZ, Lift);
		}
		ftcore::addImpact(Rules.wear, Wear, Into, static_cast<float>(Local.X), static_cast<float>(Local.Y));
	}
	Tilt->SetRelativeLocation(FVector(0.0, 0.0, -(Clearance + BodyHalfZ + Lift)));

	// The body tilts with the ground under the wheels; the wheels sit on their own ground, spin and steer.
	const float Pitch = FMath::RadiansToDegrees(FMath::Atan2((WheelGround[0] + WheelGround[1] - WheelGround[2] - WheelGround[3]) * 0.5f, 2.f * HalfWheelbase));
	const float Roll = FMath::RadiansToDegrees(FMath::Atan2((WheelGround[0] + WheelGround[2] - WheelGround[1] - WheelGround[3]) * 0.5f, 2.f * HalfTrack));
	Tilt->SetRelativeRotation(FRotator(Pitch, 0.0, Roll));
	Spin = FMath::Fmod(Spin + FMath::RadiansToDegrees(State.forward / WheelRadius) * DeltaSeconds, 360.f);
	const float Steer = FMath::RadiansToDegrees(State.steer);
	const float BoxZ = static_cast<float>(GetActorLocation().Z);
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
	if (bCourse || bMapRoute)
	{
		TraceAge += DeltaSeconds;
		TickScript(DeltaSeconds);
	}
	TickSeats(DeltaSeconds);
	MoveBody(DeltaSeconds);
	TickLamps(DeltaSeconds);
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
	const float Along = LoopN > 0 ? RouteDistance(P, Travelled) : CourseDistance(P, bMapRoute);
	Travelled = Along;
	LimitKmh = bMapRoute ? 60.f : (Along > 10000.f ? 80.f : 60.f);
	const float EndAlong = bMapRoute ? RouteLen : 19000.f;
	const float Speed = State.forward;
	const FVector2D Target = LoopN > 0 ? RoutePoint(Along + 350.f + 0.2f * FMath::Max(Speed, 0.f))
		: CoursePoint(Along + 600.f + 0.3f * FMath::Max(Speed, 0.f), bMapRoute);
	const float H = State.heading - StartHeading;
	const float Alpha = FMath::FindDeltaAngleRadians(H, FMath::Atan2(static_cast<float>(Target.Y - P.Y), static_cast<float>(Target.X - P.X)));
	const float Ld = FMath::Max(100.f, static_cast<float>(FVector2D::Distance(Target, P)));
	const float Delta = FMath::Atan(2.f * Drive.tuning().wheelbase * FMath::Sin(Alpha) / Ld);
	const float Steer = FMath::Clamp(Delta / FMath::DegreesToRadians(Drive.tuning().maxSteer), -1.f, 1.f);

	float Want = LimitKmh * KmhToCms; // flat out to the limit
	if (bMapRoute)
	{
		// Town speed, easing down over the last 20 m to stop at the route's end.
		Want = FMath::Min(40.f * KmhToCms, FMath::Sqrt(FMath::Max(0.f, 2.f * 500.f * (EndAlong - 300.f - Along))));
		if (LoopN > 0)
		{
			Want = FMath::Min(Want, FMath::Min(35.f * KmhToCms, CornerSpeed(Along)));
		}
	}
	else if (Along > 12000.f && Along < CornerX + 1500.f)
	{
		Want = 25.f * KmhToCms; // slow for the corner
	}
	else if (Along >= CornerX + 1500.f)
	{
		Want = 40.f * KmhToCms;
	}
	if (Along > EndAlong - 300.f)
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
	if (!ShotPrefix.IsEmpty() && bMapRoute)
	{
		if (Shots == 0 && Along > EndAlong * 0.35f) { Shot(TEXT("side"), -90.f, -5.f, 700.f); }
		else if (Shots == 1 && Along > EndAlong * 0.55f) { Shot(TEXT("behind"), 0.f, -10.f, 750.f); }
		else if (Shots == 2 && Along > EndAlong * 0.75f) { Shot(TEXT("top"), 0.f, -89.f, 2500.f); }
		// A loop adds its last-but-one corner from above and behind, the van just into its turn.
		else if (Shots == 3 && LoopN > 3 && Along > LoopCum[LoopN - 3] + 300.f) { Shot(TEXT("junction"), 30.f, -35.f, 1400.f); }
	}
	else if (!ShotPrefix.IsEmpty())
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
	if (ScriptStage == 3 && Along > EndAlong - 300.f && State.speed() < 5.f)
	{
		bExitHeld = true;
		ScriptStage = 4;
		// From low behind while the lead gets out: the handbrake holds, so the brake lamps are lit.
		if (!ShotPrefix.IsEmpty() && bMapRoute)
		{
			Shot(TEXT("stop"), 0.f, -4.f, 650.f);
		}
	}
	if (ScriptStage == 4 && Seats.phase == ftcore::SeatPhase::OnFoot)
	{
		ScriptStage = 5;
		EndAt = ScriptTime + 1.5f;
		// Parked with nobody aboard: from low behind and to the side, every lamp dark (at night too).
		if (!ShotPrefix.IsEmpty() && bMapRoute)
		{
			Shot(TEXT("parked"), 35.f, -6.f, 800.f);
		}
	}
	if (ScriptStage == 5 && ScriptTime > EndAt)
	{
		if (!bKerbTouched && !bMapRoute)
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
	if (ScriptStage < 5 && ScriptTime > 90.f + RouteLen / 500.f)
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

AFTPartyVan* AFTPartyVan::SpawnAt(UWorld* World)
{
	FString Value;
	if (!World || !FParse::Value(FCommandLine::Get(), TEXT("PartyVanAt="), Value, false)) // commas are part of it
	{
		return nullptr;
	}
	for (TActorIterator<AFTPartyVan> It(World); It; ++It)
	{
		return nullptr; // one van
	}
	TArray<FString> Parts;
	Value.ParseIntoArray(Parts, TEXT(","));
	if (Parts.Num() < 2)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTPartyVan: -PartyVanAt=%s wants X,Y[,Yaw] in cm and degrees."), *Value);
		return nullptr;
	}
	const double X = FCString::Atod(*Parts[0]);
	const double Y = FCString::Atod(*Parts[1]);
	const double Yaw = Parts.Num() > 2 ? FCString::Atod(*Parts[2]) : 0.0;
	// The ground under it: the highest blocking surface below 100 m up.
	double Z = 0.0;
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, FVector(X, Y, 10000.0), FVector(X, Y, -10000.0), ECC_Visibility))
	{
		Z = Hit.ImpactPoint.Z;
	}
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AFTPartyVan* Van = World->SpawnActor<AFTPartyVan>(FVector(X, Y, Z + 150.0), FRotator(0.0, Yaw, 0.0), P);
	UE_LOG(LogForgottenThird, Display, TEXT("FTPartyVan: spawned at (%.0f, %.0f, %.0f) yaw %.0f"), X, Y, Z, Yaw);
	return Van;
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
		const FVector2D P = CoursePoint(S, false);
		const FVector2D Side = S <= CornerX ? FVector2D(0.0, -600.0) : FVector2D(600.0, 0.0);
		Slab(O + FVector(P.X + Side.X, P.Y + Side.Y, 75.0), FVector(20.0, 20.0, 150.0));
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AFTPartyVan* Van = World->SpawnActor<AFTPartyVan>(O + FVector(0.0, 0.0, 200.0), FRotator::ZeroRotator, Params);
	UE_LOG(LogForgottenThird, Display, TEXT("FTPartyVan: -VanCourse laid at %s."), *O.ToString());
	return Van;
}
