#include "FTWaterSubsystem.h"

#include "FTExploreDirector.h"
#include "FTSceneLook.h"
#include "FTVfxComponent.h"
#include "ForgottenThird.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

#include "ftcore/Vfx.h"

namespace
{
// The eye counts as in the water from this far over the surface (m): the near plane's lower edge and a frame's
// travel; the material cuts at the line itself.
constexpr float kWaterViewMargin = 0.5f;
// The lead's half width where the surface meets it (cm): the surfaces draw the churn and the contact line around it.
constexpr double kLeadWaterlineCm = 24.0;
// The underwater view by the body's kind (linear colours): the water's own colour near the surface and far down, and
// what a metre of it takes from each of red, green and blue. A canal is green and closes in within some fifteen metres; open
// water is the blue its surface shows from above and is seen far through.
struct FWaterView
{
	FLinearColor Tint;
	FLinearColor Deep;
	FLinearColor Extinct;
};
const FWaterView kCanalView{FLinearColor(0.24f, 0.38f, 0.27f), FLinearColor(0.035f, 0.11f, 0.11f), FLinearColor(0.30f, 0.13f, 0.17f)};
const FWaterView kLakeView{FLinearColor(0.17f, 0.35f, 0.33f), FLinearColor(0.025f, 0.10f, 0.14f), FLinearColor(0.25f, 0.10f, 0.11f)};
const FWaterView kSeaView{FLinearColor(0.18f, 0.50f, 0.68f), FLinearColor(0.022f, 0.11f, 0.27f), FLinearColor(0.20f, 0.06f, 0.05f)};
} // namespace

FFTWaterBodyRow FFTWaterBodyRow::From(const ftcore::WaterBody& Body)
{
	FFTWaterBodyRow Row;
	Row.Kind = static_cast<uint8>(Body.kind);
	Row.Shape = static_cast<uint8>(Body.shape);
	Row.bWaves = Body.waves;
	Row.Level = Body.level;
	Row.Bed = Body.bed;
	Row.Flow = FVector2D(Body.flow.x, Body.flow.y);
	Row.Centre = FVector2D(Body.centre.x, Body.centre.y);
	Row.Half = FVector2D(Body.half.x, Body.half.y);
	Row.CosYaw = Body.cosYaw;
	Row.SinYaw = Body.sinYaw;
	Row.BuoyLine = Body.buoyLine;
	Row.ShelfBed = Body.shelfBed;
	Row.Shelf = Body.shelf;
	Row.Batter = Body.batter;
	for (std::size_t I = 0; I < Body.pointCount; ++I)
	{
		Row.Points.Add(FVector2D(Body.points[I].x, Body.points[I].y));
	}
	return Row;
}

ftcore::WaterBody FFTWaterBodyRow::ToBody() const
{
	ftcore::WaterBody Body;
	Body.kind = static_cast<ftcore::WaterKind>(FMath::Min<uint8>(Kind, static_cast<uint8>(ftcore::WaterKind::Sea)));
	Body.shape = static_cast<ftcore::WaterShape>(FMath::Min<uint8>(Shape, static_cast<uint8>(ftcore::WaterShape::HalfPlane)));
	Body.waves = bWaves;
	Body.level = Level;
	Body.bed = Bed;
	Body.flow = {static_cast<float>(Flow.X), static_cast<float>(Flow.Y)};
	Body.centre = {static_cast<float>(Centre.X), static_cast<float>(Centre.Y)};
	Body.half = {static_cast<float>(Half.X), static_cast<float>(Half.Y)};
	Body.cosYaw = CosYaw;
	Body.sinYaw = SinYaw;
	Body.buoyLine = BuoyLine;
	Body.shelfBed = ShelfBed;
	Body.shelf = Shelf;
	Body.batter = Batter;
	const int32 Count = FMath::Min(Points.Num(), static_cast<int32>(ftcore::kWaterMaxPoints));
	Body.pointCount = static_cast<std::uint8_t>(Count);
	for (int32 I = 0; I < Count; ++I)
	{
		Body.points[static_cast<std::size_t>(I)] = {static_cast<float>(Points[I].X), static_cast<float>(Points[I].Y)};
	}
	return Body;
}

FFTWaterExitRow FFTWaterExitRow::From(const ftcore::WaterExit& Exit)
{
	FFTWaterExitRow Row;
	Row.Kind = static_cast<uint8>(Exit.kind);
	Row.Foot = FVector2D(Exit.foot.x, Exit.foot.y);
	Row.Top = FVector2D(Exit.top.x, Exit.top.y);
	Row.TopZ = Exit.topZ;
	Row.Out = FVector2D(Exit.out.x, Exit.out.y);
	return Row;
}

ftcore::WaterExit FFTWaterExitRow::ToExit() const
{
	ftcore::WaterExit Exit;
	Exit.kind = Kind == static_cast<uint8>(ftcore::WaterExitKind::Steps) ? ftcore::WaterExitKind::Steps : ftcore::WaterExitKind::Ladder;
	Exit.foot = {static_cast<float>(Foot.X), static_cast<float>(Foot.Y)};
	Exit.top = {static_cast<float>(Top.X), static_cast<float>(Top.Y)};
	Exit.topZ = TopZ;
	Exit.out = {static_cast<float>(Out.X), static_cast<float>(Out.Y)};
	return Exit;
}

bool UFTWaterSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UFTWaterSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	Water = ftcore::WaterBodies{};
	for (TActorIterator<AFTExploreDirector> It(&InWorld); It; ++It)
	{
		for (const FFTWaterExitRow& Row : It->WaterExits)
		{
			Water.addExit(Row.ToExit());
		}
		for (const FFTWaterBodyRow& Row : It->WaterBodies)
		{
			if (!Water.add(Row.ToBody()))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Water: a body of %s was refused (no footprint, or more than %d)."), *It->GetName(),
					static_cast<int32>(ftcore::kWaterMaxBodies));
			}
		}
	}
	bHasBodies = Water.count() > 0;
	if (bHasBodies)
	{
		int32 Seas = 0;
		for (std::size_t I = 0; I < Water.count(); ++I)
		{
			Seas += Water.body(I).kind == ftcore::WaterKind::Sea ? 1 : 0;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Water: %d bodies (%d sea): wade to %.1f m, deep wade to %.1f m, swim over it."),
			static_cast<int32>(Water.count()), Seas, ftcore::kWaterWadeDepth, ftcore::kWaterSwimDepth);
		int32 Ladders = 0;
		for (std::size_t I = 0; I < Water.exitCount(); ++I)
		{
			Ladders += Water.exit(I).kind == ftcore::WaterExitKind::Ladder ? 1 : 0;
		}
		for (std::size_t I = 0; I < Water.count(); ++I)
		{
			const ftcore::WaterBody& Body = Water.body(I);
			if (Body.batter > 0.f)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Water: body %d (%s): %.2f m at its walls on a %.1f m shelf, %.2f m in the middle."), static_cast<int32>(I),
					ANSI_TO_TCHAR(ftcore::waterKindName(Body.kind)), Body.level - Body.shelfBed, Body.shelf, Body.level - Body.bed);
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Water: %d ways out (%d ladders, %d flights of steps); a jump at any other wall is the fallback."),
			static_cast<int32>(Water.exitCount()), Ladders, static_cast<int32>(Water.exitCount()) - Ladders);
		// What the surfaces read (rings, the lead, the sun under water) and the splash; each is done without if missing.
		const FString Path = FTScene::LookWaterCollection;
		SurfaceParams = LoadObject<UMaterialParameterCollection>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
		SplashSystem = LoadObject<UNiagaraSystem>(nullptr, *UFTVfxComponent::SystemPath(TEXT("water_splash")), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!SurfaceParams)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Water: %s is missing (FTBuildLookMaterials -Water builds it); no rings, wake or light shafts."),
				FTScene::LookWaterCollection);
		}
		if (!SplashSystem)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Water: no water_splash effect (FTBuildVfx -Only=water_splash builds it)."));
		}
	}
}

float UFTWaterSubsystem::WaterTime() const
{
	const UWorld* World = GetWorld();
	return World ? static_cast<float>(World->GetTimeSeconds()) : 0.f;
}

TStatId UFTWaterSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTWaterSubsystem, STATGROUP_Tickables);
}

void UFTWaterSubsystem::ApplyTune()
{
#if UE_BUILD_SHIPPING
	// A tuning switch for stills: not in a shipping build.
	return;
#else
	// -WaterTune=<Name>=<value>,...: each name is tried on the water collection, then on the underwater view.
	FString Tune;
	if (!FParse::Value(FCommandLine::Get(), TEXT("WaterTune="), Tune, false) || Tune.IsEmpty())
	{
		return;
	}
	UWorld* World = GetWorld();
	UMaterialParameterCollectionInstance* Params = World && SurfaceParams ? World->GetParameterCollectionInstance(SurfaceParams) : nullptr;
	TArray<FString> Pairs;
	Tune.ParseIntoArray(Pairs, TEXT(","));
	for (const FString& Pair : Pairs)
	{
		FString Name, Value;
		if (!Pair.Split(TEXT("="), &Name, &Value) || !Value.IsNumeric())
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Water: -WaterTune: \"%s\" is not <Name>=<number>."), *Pair);
			continue;
		}
		const float Number = FCString::Atof(*Value);
		const bool bSurface = Params && Params->SetScalarParameterValue(FName(*Name), Number);
		if (Underwater)
		{
			Underwater->SetScalarParameterValue(FName(*Name), Number);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Water: tune %s = %g (%s)."), *Name, Number,
			bSurface ? TEXT("the water collection") : Underwater ? TEXT("the underwater view") : TEXT("kept for the underwater view"));
	}
#endif
}

void UFTWaterSubsystem::Tick(float DeltaTime)
{
	PushSurface(DeltaTime);
	UWorld* World = GetWorld();
	const APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
	if (!Player || !Player->PlayerCameraManager)
	{
		return;
	}
	const FVector Eye = Player->PlayerCameraManager->GetCameraLocation();
	const ftcore::Vec2 At{static_cast<float>(Eye.X * 0.01), static_cast<float>(Eye.Y * 0.01)};
	const float EyeZ = static_cast<float>(Eye.Z * 0.01);
	const ftcore::WaterCamera Cam = Water.camera(At, EyeZ, WaterTime(), kWaterViewMargin);
	SetView(Cam.under, Cam.surface * 100.f, Cam.body);
	const bool bNowUnder = Cam.under && Cam.depth > 0.f;
	Depth = bNowUnder ? Cam.depth : 0.f;
	if (bNowUnder != bUnder)
	{
		bUnder = bNowUnder;
		const FName Kind(Cam.body >= 0 ? ANSI_TO_TCHAR(ftcore::waterKindName(Water.body(static_cast<std::size_t>(Cam.body)).kind)) : TEXT("none"));
		UE_LOG(LogForgottenThird, Display, TEXT("Water: the view %s (%s, %.2f m under)."), bUnder ? TEXT("went under the surface") : TEXT("came out of the water"),
			*Kind.ToString(), Depth);
		OnCameraWater.Broadcast(bUnder, Depth, Kind);
	}
}

void UFTWaterSubsystem::SetView(bool bWet, float SurfaceCm, int32 Body)
{
	if (bWet && !Volume && !bMaterialMissing)
	{
		UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, FTScene::LookUnderwaterMaterial);
		UWorld* World = GetWorld();
		if (!Parent || !World)
		{
			bMaterialMissing = true;
			UE_LOG(LogForgottenThird, Warning, TEXT("Water: %s is missing (FTBuildLookMaterials -Underwater builds it); no underwater view."),
				FTScene::LookUnderwaterMaterial);
			return;
		}
		Underwater = UMaterialInstanceDynamic::Create(Parent, this);
		Volume = World->SpawnActor<APostProcessVolume>();
		if (Volume)
		{
			Volume->bUnbound = true;
			Volume->Priority = 100.f;
			Volume->BlendWeight = 1.f;
			Volume->bEnabled = false;
			Volume->Settings.WeightedBlendables.Array.Add(FWeightedBlendable(1.f, Underwater.Get()));
		}
		ApplyTune();
	}
	if (!Volume)
	{
		return;
	}
	if (bWet && Underwater)
	{
		// The water's own colour by its kind: a canal is green and soon closes in; open water is blue and seen far through.
		const ftcore::WaterKind Kind = Body >= 0 ? Water.body(static_cast<std::size_t>(Body)).kind : ftcore::WaterKind::Canal;
		const FWaterView& View = Kind == ftcore::WaterKind::Sea ? kSeaView : Kind == ftcore::WaterKind::Canal ? kCanalView : kLakeView;
		Underwater->SetScalarParameterValue(TEXT("WaterZ"), SurfaceCm);
		Underwater->SetScalarParameterValue(TEXT("BedZ"), Body >= 0 ? Water.body(static_cast<std::size_t>(Body)).bed * 100.f : SurfaceCm - 300.f);
		Underwater->SetVectorParameterValue(TEXT("Tint"), View.Tint);
		Underwater->SetVectorParameterValue(TEXT("DeepTint"), View.Deep);
		Underwater->SetVectorParameterValue(TEXT("Extinct"), View.Extinct);
	}
	if (static_cast<bool>(Volume->bEnabled) != bWet)
	{
		Volume->bEnabled = bWet;
	}
}

void UFTWaterSubsystem::PushSurface(float DeltaTime)
{
	Rings.advance(DeltaTime);
	UWorld* World = GetWorld();
	UMaterialParameterCollectionInstance* Params = World && SurfaceParams ? World->GetParameterCollectionInstance(SurfaceParams) : nullptr;
	if (!Params)
	{
		return;
	}
	if (!bTuned)
	{
		bTuned = true;
		ApplyTune();
	}
	static const FName RingNames[ftcore::kWaterRippleCount] = {TEXT("Ring0"), TEXT("Ring1"), TEXT("Ring2"), TEXT("Ring3"), TEXT("Ring4"), TEXT("Ring5"),
		TEXT("Ring6"), TEXT("Ring7")};
	static const FName LeadName(TEXT("Lead"));
	static const FName WakeName(TEXT("Wake"));
	static const FName SunName(TEXT("Sun"));
	// The rings: place (cm), radius (cm), amplitude. Once more after the last one dies, to clear it.
	const bool bAny = Rings.live() > 0;
	if (bAny || bRingsShown)
	{
		for (std::size_t I = 0; I < ftcore::kWaterRippleCount; ++I)
		{
			const ftcore::WaterRing Ring = Rings.ring(I);
			Params->SetVectorParameterValue(RingNames[I], FLinearColor(Ring.p.x * 100.f, Ring.p.y * 100.f, Ring.radius * 100.f, Ring.amplitude));
		}
		bRingsShown = bAny;
	}
	Params->SetVectorParameterValue(LeadName, FLinearColor(static_cast<float>(LeadAt.X), static_cast<float>(LeadAt.Y), static_cast<float>(LeadAt.Z), 0.f));
	Params->SetVectorParameterValue(WakeName, FLinearColor(static_cast<float>(WakeWay.X), static_cast<float>(WakeWay.Y), WakeSpeed, WakeAmount));
	// The sun as the water bends it (Snell, 1.333): the way to it from under the surface, and how much of it there is.
	if (!Sun.IsValid() && (SunSought -= DeltaTime) <= 0.f)
	{
		SunSought = 2.f;
		for (TActorIterator<ADirectionalLight> It(World); It; ++It)
		{
			Sun = *It;
			break;
		}
	}
	FLinearColor SunUnder(0.f, 0.f, 1.f, 0.f);
	if (const ADirectionalLight* Light = Sun.Get())
	{
		const FVector To = -Light->GetActorForwardVector();
		if (To.Z > 0.02)
		{
			const FVector2D Flat(To.X / 1.333, To.Y / 1.333);
			SunUnder = FLinearColor(static_cast<float>(Flat.X), static_cast<float>(Flat.Y), static_cast<float>(FMath::Sqrt(FMath::Max(1.0 - Flat.SizeSquared(), 0.01))),
				static_cast<float>(FMath::Clamp(To.Z * 5.0, 0.0, 1.0)));
		}
	}
	Params->SetVectorParameterValue(SunName, SunUnder);
}

void UFTWaterSubsystem::LeadWater(AActor& Lead, const ftcore::WaterState& State, const FVector& At, const FVector& Velocity, float Height,
	float DeltaSeconds)
{
	const bool bIn = State.mode != ftcore::WaterMode::Dry && State.immersion > 0.f;
	const ftcore::Vec2 P{static_cast<float>(At.X * 0.01), static_cast<float>(At.Y * 0.01)};
	const ftcore::Vec2 V{static_cast<float>(Velocity.X * 0.01), static_cast<float>(Velocity.Y * 0.01)};
	// The fall's speed is the fastest seen since the lead was last dry: by the tick the water is felt the bed may have stopped it.
	const float FallNow = FMath::Max(static_cast<float>(-Velocity.Z * 0.01), 0.f);
	const float Fall = FMath::Max(LeadFall, FallNow);
	const ftcore::WaterStirEvents Event = Stir.step(Rings, State, P, V, Fall, DeltaSeconds);
	LeadFall = bIn ? 0.f : FallNow;
	const ftcore::WaterKind BodyKind = State.body >= 0 ? Water.body(static_cast<std::size_t>(State.body)).kind : ftcore::WaterKind::Pool;
	Wet.step(bIn ? State.immersion : 0.f, Height * 0.01f, DeltaSeconds, 1.f, ftcore::waterSilt(BodyKind));
	const FName Kind(State.body >= 0 ? ANSI_TO_TCHAR(ftcore::waterKindName(BodyKind)) : TEXT("none"));
	UWorld* World = GetWorld();
	if (Event.entered)
	{
		// The splash: bigger and faster for a harder fall.
		if (SplashSystem && World)
		{
			const FVector Where(At.X, At.Y, State.surface * 100.0 + 2.0);
			if (UNiagaraComponent* Splash = UNiagaraFunctionLibrary::SpawnSystemAtLocation(World, SplashSystem, Where, FRotator::ZeroRotator, FVector::OneVector, true, true,
					ENCPoolMethod::AutoRelease))
			{
				static const FName SpeedScale(UTF8_TO_TCHAR(ftcore::kVfxUserSpeedScale));
				static const FName ShapeScale(UTF8_TO_TCHAR(ftcore::kVfxUserShapeScale));
				Splash->SetVariableFloat(SpeedScale, FMath::Lerp(0.55f, 1.25f, Event.splash));
				Splash->SetVariableVec3(ShapeScale, FVector(FMath::Lerp(0.7f, 1.3f, Event.splash)));
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Water: splash %.2f (fall %.1f m/s) into the %s at (%.0f, %.0f) cm."), Event.splash, Fall, *Kind.ToString(), At.X, At.Y);
		OnLeadSplash.Broadcast(Event.splash, Fall, Kind);
	}
	if (Event.left)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Water: the lead is out, wet to %.2f m over its feet, silt %.2f; dry in about %.0f s."), Wet.line, Wet.silt,
			ftcore::kWaterDryTime);
		OnLeadWaterExit.Broadcast(Wet.line, LeadKind);
	}
	const uint8 Mode = static_cast<uint8>(State.mode);
	if (Event.moving != bLeadMoving || (Event.moving && Mode != LeadMode))
	{
		const FName ModeName(ANSI_TO_TCHAR(ftcore::waterModeName(State.mode)));
		UE_LOG(LogForgottenThird, Display, TEXT("Water: the lead %s in the water (%s, %.2f m/s)."), Event.moving ? TEXT("is moving") : TEXT("stopped"),
			*ModeName.ToString(), Event.speed);
		OnLeadWaterMove.Broadcast(Event.moving, Event.speed, ModeName);
	}
	bLeadMoving = Event.moving;
	LeadMode = Mode;
	LeadSpeed = Event.speed;
	if (bIn)
	{
		LeadKind = Kind;
	}
	// For the surfaces: where the lead stands in the water, and its wake (the way eased, so a turn bends the V over).
	LeadAt = FVector(At.X, At.Y, bIn ? kLeadWaterlineCm : 0.0);
	const float WakeWant = bIn ? FMath::Clamp((Event.speed - ftcore::kWaterWakeSpeed) / 0.7f, 0.f, 1.f) : 0.f;
	WakeAmount = FMath::FInterpTo(WakeAmount, WakeWant, DeltaSeconds, WakeWant > WakeAmount ? 4.f : 2.f);
	if (bIn && Event.speed > ftcore::kWaterWakeSpeed)
	{
		const FVector2D Way = FVector2D(V.x, V.y).GetSafeNormal();
		WakeWay = FMath::Lerp(WakeWay, Way, FMath::Clamp(6.f * DeltaSeconds, 0.f, 1.f)).GetSafeNormal();
		if (WakeWay.IsNearlyZero())
		{
			WakeWay = Way;
		}
		WakeSpeed = FMath::FInterpTo(WakeSpeed, Event.speed * 100.f, DeltaSeconds, 4.f);
	}
}