#pragma once

// The per-world photoreal look at run time. The maps are built with the shared default look (FTScene::SpawnLook), so
// once a game world's look actors exist this writes the world's own look into them in place (FTScene::ApplyLook) and
// hands the after-tonemap grade its constants (the FTLook plugin), and fills the look's material parameters (a world's
// wetness, water and glow; FTScene::ApplyLookParameters). Once per run it also sets the render settings looks.json lists
// (console variables, under the ini and the command line). The world comes from Data/Look/looks.json, picked by
// the map's name (Solhaven, Lattice or Unwritten in it) or by -Look=<world>. -LookOff, or r.FT.Look 0 at start, applies
// nothing, so the scene renders exactly as the shared look does. ft.Look <world|off> switches it while playing.
//
// A fight's hot accent is set here too (SetFightAccent): the grade keeps that hue's chroma while the world around it
// is desaturated. See docs/PHOTOREAL_LOOK.md.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "ftcore/LookGrade.h"
#include "ftcore/SceneLook.h"

#include "FTLookSubsystem.generated.h"

class FFTLookGradeExtension;
namespace ftcore
{
struct LookTable;
}

namespace FTLook
{
// Data/Look/looks.json, read once (empty when it is missing or refused; the problems are logged).
FORGOTTENTHIRD_API const ftcore::LookTable& Table();
} // namespace FTLook

UCLASS()
class FORGOTTENTHIRD_API UFTLookSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bWaiting; }

	// Switches to World's look ("solhaven", "lattice", "unwritten"), or back to the shared look with "off". False, with
	// nothing changed, when Data/Look/looks.json has no such world.
	bool SetWorldLook(const FString& World);
	// The world whose look is on, or empty when it's the shared look.
	const FString& GetWorldLook() const { return WorldId; }
	// The look wanted on the scene, and whether it (and its material parameters) has been written or given up on.
	const ftcore::SceneLook& GetLook() const { return Wanted; }
	bool IsSettled() const { return !bWaiting; }

	// The fight's accent: the grade keeps the chroma of hues within WidthDegrees of HueDegrees (OKLab hue) at Chroma
	// times their own. ClearFightAccent turns it off.
	void SetFightAccent(float HueDegrees, float WidthDegrees, float Chroma);
	void ClearFightAccent();

private:
	void PushGrade();
	// A day world-eye still (FTDayShade::StillAmount): the day's shadow floor over the look just written.
	void ApplyStillDayShade();

	TSharedPtr<FFTLookGradeExtension, ESPMode::ThreadSafe> Grade;
	FString WorldId;
	// What the scene's actors carry now, so a switch rewrites only what differs.
	ftcore::SceneLook Applied;
	ftcore::SceneLook Wanted;
	ftcore::FightAccent Accent;
	// Waiting for the level's look actors (the arena spawns them after the world begins).
	bool bWaiting = false;
	float Waited = 0.f;
	// The look's material parameters still to write (the world's collection instance may not be there yet).
	bool bParametersPending = false;
	int32 ParametersWritten = 0;
};
