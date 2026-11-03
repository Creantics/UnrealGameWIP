#pragma once

// Builds the look lab (/Game/Maps/Look_Lab) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildLookLabWorld -NullRHI -unattended -nopause -nosplash
//
// Run FTBuildLookMaterials first. The map is a wet-stone yard under the shared look (FTScene::SpawnLook) holding one of
// each surface a world's look changes: a basin of water 2 m deep with a ramp that rises to its surface (the shallows),
// a glass pane before a glowing strip, a glass ball, a chrome slab and a chrome ball beside a practical lamp (tagged
// FTScene::PracticalTag) under a roofed alcove, so its light hangs in the fog. Three cameras, tagged
// AFTLookLabGameMode::ViewTag(1..3), frame the yard, the water and glass, and the chrome and lamp. AFTLookLabGameMode
// runs it. It overwrites the map. See docs/PHOTOREAL_LOOK.md.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildLookLabWorldCommandlet.generated.h"

UCLASS()
class UFTBuildLookLabWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildLookLabWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
