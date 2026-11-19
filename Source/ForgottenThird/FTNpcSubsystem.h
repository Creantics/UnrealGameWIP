#pragma once

// NPC memory and disposition for one game instance: Core's NpcMemoryWorld (on the heap, about 108 KB) with the tables,
// lines and hook deeds loaded once from Data/Npc. All rules live in Core (ftcore/NpcMemory.h); this finds witnesses,
// turns game events into Core's hooks, follows the world clock, saves, and draws a development overlay.
//
//  - Witnesses: registered AFTNpcActor within WitnessRadius of the deed, in line of sight, nearest eight. Found only
//    when a deed happens, never per tick.
//  - Clock: each tick it compares the world clock's minute with the last one passed to Core and calls npcOnClock only
//    when it moved (hearsay and expectations run there, with the hour's crowd density).
//  - Save: CaptureSaveJson / ApplySaveJson give the text of ftcore::encodeNpcMemorySave; UFTStorySaveGame keeps it
//    beside the story save (with every memory's leads).
//  - Story facts: lines may read them through `fact:<id>=<value>`; SetFactSource hands in the story state's reader and
//    reloads the lines. Until then a line table with a fact clause is refused: the NPCs speak no lines, but memory,
//    stances and terms still run (a bad line table never turns the memory off).
//  - Overlay (development builds): -NpcOverlay, -NpcSliceTest or `ft.NpcOverlay 1` draws the nearest NPC's axes,
//    stances, strongest memories, rumours due and expectations, and the last line picked for it.
//
// UFTNpcWorldSubsystem (below) places the three slice placeholders with -NpcSlice or -NpcSliceTest; -NpcSliceTest also
// runs the slice's scripted day once and logs every step as NPCSLICE lines (the doctest walk, in the game).

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "ftcore/NpcMemory.h"
#include "ftcore/WorldClock.h"

#include "FTNpcSubsystem.generated.h"

class AFTNpcActor;
class APlayerController;
class UCanvas;

UENUM(BlueprintType)
enum class EFTNpcShopAct : uint8
{
	Enter,
	Buy,
	Steal
};

UENUM(BlueprintType)
enum class EFTNpcLineKind : uint8
{
	Greet,
	Bark,
	Talk,
	Remind,
	Foreshadow
};

// A keeper's terms for a lead, as they stand.
USTRUCT(BlueprintType)
struct FFTNpcTerms
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "FT|Npc")
	int32 PricePct = 100;
	UPROPERTY(BlueprintReadOnly, Category = "FT|Npc")
	bool bShopOpen = true;
	UPROPERTY(BlueprintReadOnly, Category = "FT|Npc")
	bool bDoorOpen = true;
};

UCLASS()
class FORGOTTENTHIRD_API UFTNpcSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	UFTNpcSubsystem();
	virtual ~UFTNpcSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bReady && !bHoldClock; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }

	static UFTNpcSubsystem* Get(const UObject* WorldContext);

	// Loads Data/Npc (relative to the project). False, with OutError, when a table is missing or bad; the world is
	// then empty and every hook does nothing.
	bool Load(FString& OutError);
	UFUNCTION(BlueprintPure, Category = "FT|Npc")
	bool IsReady() const { return bReady; }

	// ---- placed NPCs ----
	void RegisterNpc(AFTNpcActor* Npc);
	void UnregisterNpc(AFTNpcActor* Npc);
	int32 NpcIndexOf(const FString& NpcId) const;
	AFTNpcActor* FindNpc(const FString& NpcId) const;

	// ---- game events (Lead: 0..2, anything else is the party) ----
	// Victim: an AFTNpcActor (the tied deed, it learns as the victim) or anything else (a crowd member).
	UFUNCTION(BlueprintCallable, Category = "FT|Npc")
	// Van: the van itself, so the witness traces look past it. Returns the deed's seq (0: nothing recorded).
	int32 OnVanKnockdown(const FVector& At, AActor* Victim, int32 Lead, AActor* Van = nullptr);
	UFUNCTION(BlueprintCallable, Category = "FT|Npc")
	void OnFightEnd(const FVector& At, int32 Lead, bool bWon, bool bSpared);
	// The keeper's terms before the act (Enter only reads them).
	UFUNCTION(BlueprintCallable, Category = "FT|Npc")
	FFTNpcTerms OnShop(AFTNpcActor* Keeper, int32 Lead, EFTNpcShopAct Act);
	UFUNCTION(BlueprintCallable, Category = "FT|Npc")
	void OnHelpedPedestrian(const FVector& At, int32 Lead);
	UFUNCTION(BlueprintCallable, Category = "FT|Npc")
	void OnMeet(AFTNpcActor* Npc, int32 Lead);
	UFUNCTION(BlueprintCallable, Category = "FT|Npc")
	void OnDelivery(AFTNpcActor* Recipient, int32 Lead);
	UFUNCTION(BlueprintCallable, Category = "FT|Npc")
	void OnGift(AFTNpcActor* Giver, int32 Lead, bool bAccepted);
	// Gate: the gate's own place id (1..65535), so each gate is remembered apart.
	UFUNCTION(BlueprintCallable, Category = "FT|Npc")
	void OnGate(AFTNpcActor* Owner, int32 Lead, int32 Gate, bool bMended);

	// The best line for the NPC now, marked said; empty when none qualifies. The id keys the private line text.
	UFUNCTION(BlueprintCallable, Category = "FT|Npc")
	FString PickLine(AFTNpcActor* Npc, int32 Lead, EFTNpcLineKind Kind);
	UFUNCTION(BlueprintPure, Category = "FT|Npc")
	FString GetStance(AFTNpcActor* Npc, int32 Lead) const;
	FFTNpcTerms GetTerms(AFTNpcActor* Npc, int32 Lead) const;

	// ---- story facts ----
	// The reader must outlive its use here (null: none). Reloads the lines with it.
	void SetFactSource(const ftcore::NpcFactSource* Facts);

	// ---- save ----
	FString CaptureSaveJson() const;
	// False (the world untouched) on bad text; unknown ids are dropped and logged.
	bool ApplySaveJson(const FString& Json, FString& OutError);
	// Back to the tables' baselines, nothing remembered (loading a save from before NPC memory, a new game).
	void ResetMemory();

	// ---- for the slice run ----
	// Stops following the world clock (the scripted day sets the memory clock itself).
	void HoldClock(bool bHold) { bHoldClock = bHold; }
	void SetMemoryTime(uint32 Day, double Hours);
	const ftcore::NpcMemoryWorld* GetMemoryWorld() const { return World.Get(); }
	const ftcore::NpcTables& GetTables() const { return Tables; }

	// World witness range (cm).
	UPROPERTY(EditAnywhere, Category = "FT|Npc")
	float WitnessRadius = 3000.f;
	// The district the slice plays in (npc_values.csv district_id).
	UPROPERTY(EditAnywhere, Category = "FT|Npc")
	int32 District = 1;

private:
	ftcore::NpcWhere Where(const FVector& At, int32 Lead, const AActor* Exclude, const AActor* Ignore = nullptr) const;
	static uint8 LeadOf(int32 Lead);
	static uint16 PlaceOf(const FVector& At);
	bool Index(const AFTNpcActor* Npc, uint16& Out) const;
	bool LoadLines(FString& OutError);
	void DrawOverlay(UCanvas* Canvas, APlayerController* PC);

	TUniquePtr<ftcore::NpcMemoryWorld> World;
	ftcore::NpcTables Tables;
	ftcore::NpcLineTable Lines;
	ftcore::NpcGameDeeds Deeds;
	ftcore::TimeCurves Curves;
	std::string LinesCsv;
	const ftcore::NpcFactSource* Facts = nullptr;
	TArray<TWeakObjectPtr<AFTNpcActor>> Placed;
	bool bReady = false;
	bool bHoldClock = false;
	uint32 LastMinute = MAX_uint32;

	// The last pick per NPC, for the overlay.
	TArray<int32> LastPick;
	TArray<int32> LastScore;
	FDelegateHandle OverlayHandle;
};

// Places the slice's placeholder NPCs in a game world and, with -NpcSliceTest, runs the scripted day once.
UCLASS()
class FORGOTTENTHIRD_API UFTNpcWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

private:
	void PlacePlaceholders(UWorld& InWorld);
	void RunSliceTest();
};
