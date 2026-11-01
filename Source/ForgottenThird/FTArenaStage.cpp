#include "FTArenaStage.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTSceneLook.h"
#include "ForgottenThird.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

namespace
{
// The old demo's lamp: 6000 candela reaching 18 m. A layout light's intensity is a multiple of it.
constexpr float kLampCandela = 6000.f;

UMaterialInterface* LoadQuiet(const TCHAR* Path)
{
	return LoadObject<UMaterialInterface>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

bool IsFlat(ftcore::StageGroup Group)
{
	using ftcore::StageGroup;
	return Group == StageGroup::Floor || Group == StageGroup::Mark || Group == StageGroup::Debris || Group == StageGroup::Pool
		|| Group == StageGroup::Crack;
}
} // namespace

int32 FTArenaStage::Build(UWorld* World, const ftcore::ArenaStageLayout& Layout)
{
	if (!World)
	{
		return 0;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// The scene's light, air and camera finish, then the fog's colour (the look has no field for it).
	FTScene::SpawnLook(World, Spawn, Layout.look, FRotator(Layout.sunPitch, Layout.sunYaw, 0.f));
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		It->GetComponent()->SetFogInscatteringColor(FLinearColor(Layout.fogR, Layout.fogG, Layout.fogB));
	}

	// One dynamic instance per role: the world surface master, or the glow master for a role that lights up.
	UMaterialInterface* Surface = LoadQuiet(TEXT("/Game/Rendering/M_WorldSurface.M_WorldSurface"));
	if (!Surface)
	{
		Surface = LoadQuiet(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}
	UMaterialInterface* Glow = LoadQuiet(*(FString(FTArenaStage::GlowMaterial) + TEXT(".") + FPackageName::GetShortName(FTArenaStage::GlowMaterial)));
	UMaterialInstanceDynamic* Paint[ftcore::kStageRoleCount] = {};
	for (size_t Role = 0; Role < ftcore::kStageRoleCount; ++Role)
	{
		const ftcore::StageRoleLook& Look = Layout.roles[Role];
		const FLinearColor Color(Look.r, Look.g, Look.b);
		UMaterialInterface* Base = (Look.glow > 0.f && Glow) ? Glow : Surface;
		if (!Base)
		{
			continue;
		}
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Base, World);
		Instance->SetVectorParameterValue(TEXT("Color"), Color);
		if (Base == Glow)
		{
			Instance->SetScalarParameterValue(TEXT("Strength"), Look.glow);
		}
		else
		{
			Instance->SetScalarParameterValue(TEXT("Roughness"), Look.roughness);
			Instance->SetScalarParameterValue(TEXT("Metallic"), Look.metallic);
		}
		Paint[Role] = Instance;
	}

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (!Cube || !Cylinder)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena stage: a /Engine/BasicShapes mesh is missing; the stage has no props."));
		return 0;
	}

	int32 Made = 0;
	for (const ftcore::StagePiece& P : Layout.pieces)
	{
		const double Height = static_cast<double>(P.sz);
		const FVector At(static_cast<double>(P.at.x) * 100.0, static_cast<double>(P.at.y) * 100.0, (static_cast<double>(P.z) + Height * 0.5) * 100.0);
		AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(At, FRotator(-static_cast<double>(P.pitch), static_cast<double>(P.yaw), 0.0), Spawn);
		if (!Prop)
		{
			continue;
		}
		UStaticMeshComponent* Mesh = Prop->GetStaticMeshComponent();
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetStaticMesh(P.shape == ftcore::StageShape::Cylinder ? Cylinder : Cube);
		if (UMaterialInstanceDynamic* Instance = Paint[static_cast<size_t>(P.role)])
		{
			Mesh->SetMaterial(0, Instance);
		}
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->SetCastShadow(!IsFlat(P.group));
		// Both basic shapes are 1 m across (and the cylinder 1 m tall), so the scale is the size in metres.
		Prop->SetActorScale3D(FVector(static_cast<double>(P.sx), static_cast<double>(P.sy), Height));
		++Made;
	}

	for (const ftcore::StageLight& L : Layout.lights)
	{
		APointLight* Lamp = World->SpawnActor<APointLight>(FVector(static_cast<double>(L.at.x) * 100.0, static_cast<double>(L.at.y) * 100.0, static_cast<double>(L.z) * 100.0), FRotator::ZeroRotator, Spawn);
		if (!Lamp)
		{
			continue;
		}
		Lamp->PointLightComponent->SetMobility(EComponentMobility::Movable);
		Lamp->PointLightComponent->SetIntensity(L.intensity * kLampCandela);
		Lamp->PointLightComponent->SetLightColor(FLinearColor(L.r, L.g, L.b));
		Lamp->PointLightComponent->SetAttenuationRadius(L.radius * 100.f);
		Lamp->PointLightComponent->SetCastShadows(false);
		++Made;
	}
	return Made;
}
