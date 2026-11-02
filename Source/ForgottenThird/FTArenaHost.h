#pragma once

// The arena step's host on the explore maps (FTStepHost.h, EFTStepHost::Arena): a story fight that plays the arena system
// begins where the lead stands, on the scene's world map, and the story walks on from there when it ends.
//
// The fight is the arena demo's (UFTArenaRunner): the lead's own fighter kit against the enemies of the district's zone in
// Data/Battle/arenas.json (one whose world and district are the step's), else the default zone's line-up in a zone
// anchored on the district's plaza; the zone is fitted to the world's real layout, so its walls are the buildings around
// it. The explore character steps out for the fight: hidden, its collision and movement off, and the controller takes
// the lead's fighter figure, which wears the same look. The followers are held and stand at the ring's edge behind the
// lead, and the arena HUD replaces the explore HUD. At the end the explore character comes back where the figure
// stands, the party is let go, the step is marked done (won or lost) and, as every host does, the beat's next step
// starts in place or the story map opens (FTStep::ContinueHere).
//
// The explore director spawns one on every map it hosts steps on and starts it when the map was opened for an arena
// step. -ArenaHere starts one without a story step (a standalone fight in the lead's district, for headless checks);
// -ArenaAutoPlay, -ArenaIdle and -ArenaQuitOnFinish work as on the arena demo.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaZone.h"

#include <string>

#include "FTArenaHost.generated.h"

class AFTExploreCharacter;
class AFTExploreDirector;
class AHUD;
class UFTArenaRunner;

UCLASS()
class FORGOTTENTHIRD_API AFTArenaHost : public AActor
{
	GENERATED_BODY()

public:
	AFTArenaHost();

	// Registers this map's arena host with UFTStepHostSubsystem. Explore: the director that holds the party.
	void HostSteps(AFTExploreDirector* Explore);
	// Begins the carried arena step (or, with no step, a standalone fight) around the lead.
	void StartStepHere();

	// The running fight, for AFTArenaHUD; null between fights.
	const UFTArenaRunner* GetRunner() const;
	bool IsFighting() const { return bFighting; }

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	bool LoadTables();
	// The zone and the line-up for the step: false when nothing can be fought.
	bool BuildFight(const AFTExploreCharacter& Body, int32 District, const FString& World, ftcore::ArenaDef& Def,
		ftcore::ArenaZoneFit& Fit, std::string& ZoneId, FString& KitId) const;
	void StepOut(AFTExploreCharacter& Body);
	void ParkFollowers(const ftcore::ArenaZoneFit& Fit);
	// Gives the controller the explore character back, at the figure's last spot, and the explore HUD.
	void EndInPlace();
	void Finish();

	UPROPERTY(Transient)
	TObjectPtr<UFTArenaRunner> Runner;
	TWeakObjectPtr<AFTExploreDirector> ExploreDirector;
	TWeakObjectPtr<AFTExploreCharacter> Lead;
	UPROPERTY(Transient)
	TSubclassOf<AHUD> ExploreHUD;

	ftcore::ArenaTable Table;
	ftcore::ArenaZoneTable Zones;
	bool bTablesTried = false;
	bool bTablesLoaded = false;

	bool bRegistered = false;
	bool bFighting = false;
	bool bFinished = false;
	bool bHasStep = false;
};
