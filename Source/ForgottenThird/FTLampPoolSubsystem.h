#pragma once

// The street lamps' and shop spill lights' night pool, for maps whose massing build wrote the lights as data
// (AFTNightLights with a UFTNightLightData) instead of one light actor each.
//
// By day no lamp or shop light is registered with the renderer at all: the pool's components are unregistered. Once the
// world clock's NightAmount passes 0.5 (UFTWorldClockSubsystem), or the world eye's night forces it (ForceNight), the
// pool is made once on the AFTNightLights actor (kMaxLamps unshadowed movable spots for the lamps, kMaxShops for the
// shops, at the night spec's values below) and registered; every kPickSeconds the lamps and shops nearest the player's camera
// within kReachMetres (ftcore::LightPicker) take the pool's lights. A light whose lamp stays picked keeps its slot, so
// only the lamps that come into or drop out of reach move; slots with nothing to light are hidden. Every other lamp
// shows only its emissive head (the AFTNightLights' head instances, whose material glows by the window collection's
// Night on its own).
//
// Every pool light takes the night spec's values (FTNightLook.h: candela, reach and colour, the same for the running
// clock and a night still; the data keeps only the cones and sources), times its lamp's crowding share
// (ftcore::nightLampScales, made with the pickers): where lamps or shop lights stand close the share falls, so a
// dense lane doesn't burn out while a lone lamp keeps its pool.
//
// -LampPoolLamps=<n> and -LampPoolShops=<n> shrink the pool (never past kMaxLamps, kMaxShops); -LampPoolRadius=<m>
// sets the reach.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "ftcore/LightPicker.h"

#include "FTLampPoolSubsystem.generated.h"

class AFTNightLights;
class UFTNightLightData;
class USpotLightComponent;

UCLASS()
class FORGOTTENTHIRD_API UFTLampPoolSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	static constexpr int32 kMaxLamps = 64;
	static constexpr int32 kMaxShops = 32;
	static constexpr float kReachMetres = 60.f;
	static constexpr float kPickSeconds = 0.2f;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bActive; }

	// True when the map carries its lights as data (an AFTNightLights with its UFTNightLightData).
	bool HasNightLights() const { return bActive; }
	// Night from now on whatever the clock says, for stills: the pool goes live at once, picked from the camera. A
	// value at or under 0 keeps the data's. False (and nothing done) on a map without AFTNightLights.
	bool ForceNight(float LampCandela, float LampReachCm, float ShopCandela, float ShopReachCm);
	int32 GetLampCount() const;
	int32 GetShopCount() const;
	int32 GetLampPoolSize() const { return LampPool.Num(); }
	int32 GetShopPoolSize() const { return ShopPool.Num(); }

private:
	struct FPool
	{
		TArray<int32> Slot;       // the data index each light stands at, -1 when free
		TArray<uint32> Picked;    // the latest pick, nearest first
		TArray<bool> Taken;       // per picked entry: already held by a slot
		TArray<float> Scale;      // per data index: its crowding share of the candela
		ftcore::LightPicker Picker;
	};

	bool IsNightNow() const;
	void CreatePool();
	void SetNight(bool bOn);
	void ApplyValues(USpotLightComponent& Light, bool bLamp) const;
	float Candela(bool bLamp) const;
	void Pick();
	void PickPool(FPool& Pool, TArray<TObjectPtr<USpotLightComponent>>& Lights, bool bLamp, const FVector& From);

	TWeakObjectPtr<AFTNightLights> NightLights;
	TWeakObjectPtr<UFTNightLightData> Data;

	UPROPERTY(Transient)
	TArray<TObjectPtr<USpotLightComponent>> LampPool;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USpotLightComponent>> ShopPool;

	FPool Lamps;
	FPool Shops;
	int32 WantLamps = kMaxLamps;
	int32 WantShops = kMaxShops;
	float ReachCm = kReachMetres * 100.f;
	float LampCandelaOverride = 0.f, LampReachOverride = 0.f, ShopCandelaOverride = 0.f, ShopReachOverride = 0.f;
	float SincePick = 0.f;
	bool bActive = false;
	bool bForced = false;
	bool bPoolMade = false;
	bool bNight = false;
	bool bLogged = false;
};
