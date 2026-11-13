#include "FTWaterSubsystem.h"

#include "FTExploreDirector.h"
#include "FTSceneLook.h"
#include "ForgottenThird.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
// The eye counts as in the water from this far over the surface (m): the near plane's lower edge and a frame's
// travel; the material cuts at the line itself.
constexpr float kWaterViewMargin = 0.5f;
// The underwater view's Clarity by the body's kind: the share of the look's absorption and scattering it keeps.
constexpr float kCanalClarity = 0.3f;
constexpr float kLakeClarity = 0.2f;
constexpr float kSeaClarity = 0.12f;
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
	const int32 Count = FMath::Min(Points.Num(), static_cast<int32>(ftcore::kWaterMaxPoints));
	Body.pointCount = static_cast<std::uint8_t>(Count);
	for (int32 I = 0; I < Count; ++I)
	{
		Body.points[static_cast<std::size_t>(I)] = {static_cast<float>(Points[I].X), static_cast<float>(Points[I].Y)};
	}
	return Body;
}

bool UFTWaterSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UFTWaterSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	Water.clear();
	for (TActorIterator<AFTExploreDirector> It(&InWorld); It; ++It)
	{
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

void UFTWaterSubsystem::Tick(float DeltaTime)
{
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
	// A canal's water is as murky as its surface looks; open water is seen further through.
	const ftcore::WaterKind ViewKind = Cam.body >= 0 ? Water.body(static_cast<std::size_t>(Cam.body)).kind : ftcore::WaterKind::Canal;
	const float Clarity = ViewKind == ftcore::WaterKind::Sea ? kSeaClarity : ViewKind == ftcore::WaterKind::Lake ? kLakeClarity : kCanalClarity;
	SetView(Cam.under, Cam.surface * 100.f, Clarity);
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

void UFTWaterSubsystem::SetView(bool bWet, float SurfaceCm, float Clarity)
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
	}
	if (!Volume)
	{
		return;
	}
	if (bWet && Underwater)
	{
		Underwater->SetScalarParameterValue(TEXT("WaterZ"), SurfaceCm);
		Underwater->SetScalarParameterValue(TEXT("Clarity"), Clarity);
	}
	if (static_cast<bool>(Volume->bEnabled) != bWet)
	{
		Volume->bEnabled = bWet;
	}
}
