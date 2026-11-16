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
//
// Lamp shadows (the night default, trial arm c): every pooled lamp casts, under MegaLights. The default is config, in
// DefaultGame.ini [/Script/ForgottenThird.FTLampPoolSubsystem]: ShadowLamps (a count of the nearest lamps, clamped to
// kMaxLamps; 64 = every pooled lamp, since the pool never holds more than kMaxLamps = 64 and holds WantLamps = 48 by
// default) and bMegaLightsForShadows (sets r.MegaLights.EnableForProject at project-setting priority, so -dpcvars
// still overrides it). For A/B: -LampShadows=<n> casts from the n nearest, -LampShadows=0 none, -LampShadows=all (or a
// bare -LampShadows) every pooled lamp.
//
// A map whose parts stand in several levels (the strip and its always-loaded hero corner sub-level) has one
// AFTNightLights per level: the pool takes every one's lamps and shops into its own lists (each shop with its own pitch
// and gain), the first one's cones and sources for all, gathered again when the pool is made so a level streamed in
// after BeginPlay is counted.
//
// Shop shadows: a pooled shop light within ShopShadowMetres of the camera casts (a lit room's light through its lattice
// or mullions onto the lane), under MegaLights as the lamps; beyond it, none. Config ShopShadowMetres (15 by default;
// 0 none); -ShopShadows=<m> for A/B (-ShopShadows=0 none).
//
// -LampPoolHeroFirst (stills of the hero corner): the corner sub-level's lamps and shop lights within reach are held
// first, so a dense strip never crowds them out of the pool; logged per camera place.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "ftcore/LightPicker.h"

#include "FTLampPoolSubsystem.generated.h"

class AFTNightLights;
class UFTNightLightData;
class USpotLightComponent;

UCLASS(Config = Game)
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
		// The hero corner's (a set from a level named *_HeroCorner, Gather): per data index, and as a list with each one's
		// index in its own set. Merged is -LampPoolHeroFirst's scratch, the pool's size, made with the pool.
		TArray<bool> Hero;
		TArray<int32> HeroIdx;
		TArray<int32> HeroLocal;
		TArray<uint32> Merged;
	};

	// Every AFTNightLights' points into the lists below; logs the counts per actor when they change.
	void Gather(UWorld& World);
	bool IsNightNow() const;
	// The lamps' switch-on now: 1 when forced, else the world clock's (0 with no clock running).
	float LampFraction() const;
	void CreatePool();
	void SetNight(bool bOn);
	void ApplyValues(USpotLightComponent& Light, bool bLamp) const;
	float Candela(bool bLamp) const;
	void Pick();
	void PickPool(FPool& Pool, TArray<TObjectPtr<USpotLightComponent>>& Lights, bool bLamp, const FVector& From);

	TWeakObjectPtr<AFTNightLights> NightLights;
	TWeakObjectPtr<UFTNightLightData> Data;
	// Every level's lights, merged (Gather).
	TArray<FVector3f> LampAt;
	TArray<float> LampGain;
	TArray<FVector3f> ShopAt;
	TArray<float> ShopYaw;
	TArray<float> ShopPitch;
	TArray<float> ShopGain;
	int32 GatheredFrom = 0;

	UPROPERTY(Transient)
	TArray<TObjectPtr<USpotLightComponent>> LampPool;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USpotLightComponent>> ShopPool;

	FPool Lamps;
	FPool Shops;
	int32 WantLamps = 48; // of kMaxLamps: measured about 0.3 ms a night frame under the whole pool
	int32 WantShops = 24;
	float ReachCm = kReachMetres * 100.f;
	float LampCandelaOverride = 0.f, LampReachOverride = 0.f, ShopCandelaOverride = 0.f, ShopReachOverride = 0.f;
	float SincePick = 0.f;
	// Config defaults (DefaultGame.ini); see the header comment.
	UPROPERTY(Config)
	int32 ConfigShadowLamps = 0;
	UPROPERTY(Config)
	bool bMegaLightsForShadows = false;
	int32 ShadowLamps = 0; // in use: 0 none, a count of the nearest, or kMaxLamps for all
	UPROPERTY(Config)
	float ShopShadowMetres = 15.f;
	float ShopShadowCm = 0.f; // in use: shop lights this near the camera cast, 0 none
	bool bActive = false;
	bool bForced = false;
	bool bPoolMade = false;
	bool bNight = false;
	bool bLogged = false;
	// -LampPoolHeroFirst (eyes on the hero corner, default off): the corner's lamps and shop lights within reach take the
	// pool first, then the nearest of the rest; a line names the corner's live slots whenever they or the camera change.
	bool bHeroFirst = false;
	uint32 HeroLogKey = 0;
	FVector HeroLogAt = FVector(1e30);
	void LogHero(const FVector& From);
};
