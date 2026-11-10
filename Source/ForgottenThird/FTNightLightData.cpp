#include "FTNightLightData.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/CollisionProfile.h"

#include "FTSceneLook.h"

UFTNightLightData::UFTNightLightData()
	: LampCandela(FTNightLight::LampCandela)
	, LampKelvin(FTNightLight::LampKelvin)
	, LampReach(FTNightLight::LampReach)
	, LampOuter(FTNightLight::LampOuter)
	, LampInner(FTNightLight::LampInner)
	, LampSource(FTNightLight::LampSource)
	, ShopCandela(FTNightLight::ShopCandela)
	, ShopKelvin(FTNightLight::ShopKelvin)
	, ShopReach(FTNightLight::ShopReach)
	, ShopOuter(FTNightLight::ShopOuter)
	, ShopInner(FTNightLight::ShopInner)
	, ShopSource(FTNightLight::ShopSource)
	, ShopPitch(FTNightLight::ShopPitch)
{
	PrimaryComponentTick.bCanEverTick = false;
}

AFTNightLights::AFTNightLights()
{
	PrimaryActorTick.bCanEverTick = false;
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Static);
	SetRootComponent(Root);

	Data = CreateDefaultSubobject<UFTNightLightData>(TEXT("Data"));

	LampHeads = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("LampHeads"));
	LampHeads->SetupAttachment(Root);
	LampHeads->SetMobility(EComponentMobility::Static);
	LampHeads->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	LampHeads->SetGenerateOverlapEvents(false);
	LampHeads->SetCanEverAffectNavigation(false);
	LampHeads->SetCastShadow(false);
	LampHeads->ComponentTags.Add(FTScene::NightLampHeadTag);
}
