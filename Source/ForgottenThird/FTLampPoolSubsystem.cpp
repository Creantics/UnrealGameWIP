#include "FTLampPoolSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SpotLightComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTNightLightData.h"
#include "FTNightLook.h"
#include "FTWorldClockSubsystem.h"
#include "ForgottenThird.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "ftcore/HeroCorner.h"
#include "ftcore/NightLook.h"

// The leads' rim collection (the leads lane's FTRimCollection.h); until main has that header, its path and parameter here.
#if __has_include("FTRimCollection.h")
#include "FTRimCollection.h"
#define FT_HAS_RIM_COLLECTION 1
#else
#define FT_HAS_RIM_COLLECTION 0
#endif

namespace
{
#if FT_HAS_RIM_COLLECTION
const TCHAR* const RimCollectionPath = FTRim::Collection;
const TCHAR* const RimColourName = FTRim::Colour;
FLinearColor RimDefaultColour() { return FTRim::DefaultColour; }
#else
const TCHAR* const RimCollectionPath = TEXT("/Game/Characters/Shared/Materials/MPC_FTRim");
const TCHAR* const RimColourName = TEXT("RimColour");
FLinearColor RimDefaultColour() { return FLinearColor(0.82f, 0.88f, 1.f); }
#endif
// A lamp nearer the lead than this counts as this near (cm): a lamp brushing the head doesn't take the rim outright.
constexpr float kRimMinDistCm = 100.f;
// The grid's cell for the pickers, in cm: about the reach, so a pick reads a few cells a side.
constexpr float kPickerCellCm = 2000.f;
// The pool lights' pointing: lamps straight down; shops out of their face at the data's pitch.
const FRotator LampFacing(-90.0, 0.0, 0.0);
} // namespace

bool UFTLampPoolSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) &&
		Super::ShouldCreateSubsystem(Outer);
}

void UFTLampPoolSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	Gather(InWorld);
	if (!Data.IsValid())
	{
		return;
	}
	const TCHAR* Cmd = FTLookSwitches();
	FParse::Value(Cmd, TEXT("LampPoolLamps="), WantLamps);
	FParse::Value(Cmd, TEXT("LampPoolShops="), WantShops);
	float ReachMetres = kReachMetres;
	if (FParse::Value(Cmd, TEXT("LampPoolRadius="), ReachMetres))
	{
		ReachCm = FMath::Max(ReachMetres, 0.f) * 100.f;
	}
	ShadowLamps = FMath::Clamp(ConfigShadowLamps, 0, kMaxLamps);
	FString Shadows;
	if (FParse::Value(Cmd, TEXT("LampShadows="), Shadows))
	{
		ShadowLamps = Shadows.Equals(TEXT("all"), ESearchCase::IgnoreCase) ? kMaxLamps : FMath::Clamp(FCString::Atoi(*Shadows), 0, kMaxLamps);
	}
	else if (FParse::Param(Cmd, TEXT("LampShadows")))
	{
		ShadowLamps = kMaxLamps;
	}
	float ShopShadowM = ShopShadowMetres;
	FParse::Value(Cmd, TEXT("ShopShadows="), ShopShadowM);
	ShopShadowCm = FMath::Max(ShopShadowM, 0.f) * 100.f;
	if (ShopShadowCm > 0.f)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("LampPool: shadows on the shop lights within %.0f m of the camera."), ShopShadowCm / 100.f);
	}
	if (ShadowLamps > 0 || ShopShadowCm > 0.f)
	{
		// Shadowed lamps and shop lights go through MegaLights. Project-setting priority: a -dpcvars or console value still wins.
		if (bMegaLightsForShadows)
		{
			if (IConsoleVariable* MegaLights = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights.EnableForProject")))
			{
				MegaLights->Set(1, ECVF_SetByProjectSetting);
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("LampPool: shadows on the %d nearest lamps (MegaLights %s)."), ShadowLamps,
			bMegaLightsForShadows ? TEXT("asked") : TEXT("left as is"));
	}
	WantLamps = FMath::Clamp(WantLamps, 0, kMaxLamps);
	WantShops = FMath::Clamp(WantShops, 0, kMaxShops);
	bHeroFirst = FParse::Param(Cmd, TEXT("LampPoolHeroFirst"));
	bHeroLog = bHeroFirst || FParse::Param(Cmd, TEXT("LampPoolHeroLog"));
	if (bHeroFirst)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("LampPool: the hero corner's lights within reach are held first (-LampPoolHeroFirst)."));
	}
	bActive = true;
	// The first tick looks at the clock straight away.
	SincePick = kPickSeconds;
}

void UFTLampPoolSubsystem::Gather(UWorld& World)
{
	LampAt.Reset();
	LampGain.Reset();
	ShopAt.Reset();
	ShopYaw.Reset();
	ShopPitch.Reset();
	ShopGain.Reset();
	for (FPool* Pool : {&Lamps, &Shops})
	{
		Pool->Hero.Reset();
		Pool->HeroIdx.Reset();
		Pool->HeroLocal.Reset();
	}
	// Every night-light set (the strip's, a streamed sub-level's) merged by ftcore::lampPoolMerge: one set gives the pool
	// as before. Runs at BeginPlay and once more when the pool is made, never per frame.
	TArray<const UFTNightLightData*> Sets;
	TArray<FString> Names;
	FString Counts;
	for (TActorIterator<AFTNightLights> It(&World); It; ++It)
	{
		UFTNightLightData* Found = It->FindComponentByClass<UFTNightLightData>();
		if (!Found)
		{
			continue;
		}
		if (!Data.IsValid())
		{
			// The first one's cones and sources serve every light.
			NightLights = *It;
			Data = Found;
		}
		const ULevel* Level = It->GetLevel();
		Names.Add(Level ? FPackageName::GetShortName(Level->GetOutermost()->GetName()) : FString(TEXT("?")));
		Counts += FString::Printf(TEXT(" %s %d lamps + %d shops;"), *Names.Last(), Found->LampAt.Num(), FMath::Min(Found->ShopAt.Num(), Found->ShopYaw.Num()));
		Sets.Add(Found);
	}
	const int32 Actors = Sets.Num();
	std::vector<std::vector<ftcore::LampPoolPoint>> LampPts(static_cast<size_t>(Actors)), ShopPts(static_cast<size_t>(Actors));
	std::vector<ftcore::LampPoolSet> LampSets(static_cast<size_t>(Actors)), ShopSets(static_cast<size_t>(Actors));
	for (int32 S = 0; S < Actors; ++S)
	{
		const UFTNightLightData& Set = *Sets[S];
		const size_t Si = static_cast<size_t>(S);
		for (const FVector3f& At : Set.LampAt)
		{
			LampPts[Si].push_back({At.X, At.Y, At.Z});
		}
		const int32 SetShops = FMath::Min(Set.ShopAt.Num(), Set.ShopYaw.Num());
		for (int32 I = 0; I < SetShops; ++I)
		{
			ShopPts[Si].push_back({Set.ShopAt[I].X, Set.ShopAt[I].Y, Set.ShopAt[I].Z});
		}
		LampSets[Si] = {LampPts[Si].data(), LampPts[Si].size(), Set.LampGain.GetData(), static_cast<size_t>(Set.LampGain.Num())};
		ShopSets[Si] = {ShopPts[Si].data(), ShopPts[Si].size(), Set.ShopGain.GetData(), static_cast<size_t>(Set.ShopGain.Num())};
	}
	std::vector<ftcore::LampPoolSlot> Slots;
	// The hero corner's lights, by the level they came from.
	auto MarkHero = [&Names](FPool& Pool, const ftcore::LampPoolSlot& Slot, int32 Index)
	{
		const bool bHero = Names[static_cast<int32>(Slot.set)].EndsWith(TEXT("_HeroCorner"));
		Pool.Hero.Add(bHero);
		if (bHero)
		{
			Pool.HeroIdx.Add(Index);
			Pool.HeroLocal.Add(static_cast<int32>(Slot.index));
		}
	};
	ftcore::lampPoolMerge(LampSets.data(), LampSets.size(), Slots);
	for (const ftcore::LampPoolSlot& Slot : Slots)
	{
		MarkHero(Lamps, Slot, LampAt.Num());
		LampAt.Add(FVector3f(static_cast<float>(Slot.at.x), static_cast<float>(Slot.at.y), static_cast<float>(Slot.at.z)));
		LampGain.Add(Slot.gain);
	}
	// Two sets claiming the same light (within 50 cm) is a corner built twice: logged, never silently doubled.
	auto Clashes = [&](const TCHAR* What)
	{
		size_t A = 0, B = 0;
		const size_t N = ftcore::lampPoolClashes(Slots, 50.0, &A, &B);
		if (N > 0)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("LampPool: %d %s claimed by two night light sets, first %s #%d and %s #%d at (%.0f, %.0f, %.0f)."),
				static_cast<int32>(N), What, *Names[Slots[A].set], static_cast<int32>(Slots[A].index), *Names[Slots[B].set], static_cast<int32>(Slots[B].index),
				Slots[A].at.x, Slots[A].at.y, Slots[A].at.z);
		}
		return static_cast<int32>(N);
	};
	const int32 LampClashes = Actors > 1 ? Clashes(TEXT("lamps")) : 0;
	ftcore::lampPoolMerge(ShopSets.data(), ShopSets.size(), Slots);
	for (const ftcore::LampPoolSlot& Slot : Slots)
	{
		const UFTNightLightData& Set = *Sets[Slot.set];
		const int32 I = static_cast<int32>(Slot.index);
		MarkHero(Shops, Slot, ShopAt.Num());
		ShopAt.Add(FVector3f(static_cast<float>(Slot.at.x), static_cast<float>(Slot.at.y), static_cast<float>(Slot.at.z)));
		ShopYaw.Add(Set.ShopYaw[I]);
		ShopPitch.Add(Set.ShopSlotPitch.IsValidIndex(I) ? Set.ShopSlotPitch[I] : Set.ShopPitch);
		ShopGain.Add(Slot.gain);
	}
	const int32 ShopClashes = Actors > 1 ? Clashes(TEXT("shop lights")) : 0;
	if (Actors != GatheredFrom)
	{
		GatheredFrom = Actors;
		UE_LOG(LogForgottenThird, Display, TEXT("LampPool: merged %d night light sets:%s %d lamps, %d shops in all; %d lamp and %d shop slots claimed twice; hero corner %d lamps, %d shops."),
			Actors, *Counts, LampAt.Num(), ShopAt.Num(), LampClashes, ShopClashes, Lamps.HeroIdx.Num(), Shops.HeroIdx.Num());
	}
}

int32 UFTLampPoolSubsystem::GetLampCount() const
{
	return LampAt.Num();
}

int32 UFTLampPoolSubsystem::GetShopCount() const
{
	return ShopAt.Num();
}

bool UFTLampPoolSubsystem::IsNightNow() const
{
	if (bForced)
	{
		return true;
	}
	const UWorld* World = GetWorld();
	const UFTWorldClockSubsystem* Clock = World ? World->GetSubsystem<UFTWorldClockSubsystem>() : nullptr;
	// Live from the first lamp's switch-on (the clock's lamp fraction, on the sun's elevation); PickPool shows each lamp
	// at its own hashed point of it.
	return Clock && Clock->IsRunning() && Clock->GetLampFraction() > 0.f;
}

float UFTLampPoolSubsystem::LampFraction() const
{
	if (bForced)
	{
		return 1.f;
	}
	const UWorld* World = GetWorld();
	const UFTWorldClockSubsystem* Clock = World ? World->GetSubsystem<UFTWorldClockSubsystem>() : nullptr;
	return Clock && Clock->IsRunning() ? Clock->GetLampFraction() : 0.f;
}

float UFTLampPoolSubsystem::Candela(bool bLamp) const
{
	const ftcore::NightLook& Look = FTNight::Look();
	return bLamp ? (LampCandelaOverride > 0.f ? LampCandelaOverride : Look.lampCandela)
				 : (ShopCandelaOverride > 0.f ? ShopCandelaOverride : Look.shopCandela);
}

void UFTLampPoolSubsystem::ApplyValues(USpotLightComponent& Light, bool bLamp) const
{
	const UFTNightLightData& D = *Data.Get();
	const ftcore::NightLook& Look = FTNight::Look();
	const float Reach = bLamp ? (LampReachOverride > 0.f ? LampReachOverride : Look.lampReachM * 100.f)
							  : (ShopReachOverride > 0.f ? ShopReachOverride : Look.shopReachM * 100.f);
	Light.SetIntensityUnits(ELightUnits::Candelas);
	Light.SetIntensity(Candela(bLamp));
	Light.SetLightColor(FLinearColor::White, false);
	Light.SetUseTemperature(true);
	Light.SetTemperature(bLamp ? Look.lampKelvin : Look.shopKelvin);
	Light.SetAttenuationRadius(Reach);
	// The lamps' cone and lens from the night spec (wider than the built data's), the shops' from the data.
	Light.SetOuterConeAngle(bLamp ? FMath::Min(Look.lampOuterDeg, 80.f) : D.ShopOuter);
	Light.SetInnerConeAngle(bLamp ? FMath::Min(Look.lampInnerDeg, Look.lampOuterDeg) : D.ShopInner);
	Light.SetSourceRadius(bLamp ? Look.lampSourceCm : D.ShopSource);
	Light.SetSpecularScale(1.f);
	Light.SetCastShadows(false);
	// Shop lights stand inside the bays: no fog lit there (11-14 haze ruling). Street lamps keep the engine's default.
	if (!bLamp)
	{
		Light.SetVolumetricScatteringIntensity(0.f);
	}
}

void UFTLampPoolSubsystem::CreatePool()
{
	if (bPoolMade || !NightLights.IsValid() || !Data.IsValid())
	{
		return;
	}
	// Again now: a level streamed in after BeginPlay (an always-loaded sub-level) brings its own lights.
	if (UWorld* World = GetWorld())
	{
		Gather(*World);
	}
	AFTNightLights* Owner = NightLights.Get();
	if (!Owner)
	{
		return;
	}
	bPoolMade = true;
	const int32 LampCount = GetLampCount();
	const int32 ShopCount = GetShopCount();
	TArray<float> Xy;
	Xy.Reserve(FMath::Max(LampCount, ShopCount) * 2);
	for (int32 I = 0; I < LampCount; ++I)
	{
		Xy.Add(LampAt[I].X);
		Xy.Add(LampAt[I].Y);
	}
	Lamps.Picker.build(Xy.GetData(), static_cast<size_t>(LampCount), kPickerCellCm);
	Lamps.Scale.SetNumUninitialized(LampCount);
	ftcore::nightLampScales(FTNight::Look(), Xy.GetData(), static_cast<size_t>(LampCount), 100.f, Lamps.Scale.GetData());
	for (int32 I = 0; I < LampCount; ++I)
	{
		Lamps.Scale[I] *= LampGain[I];
	}
	Xy.Reset();
	for (int32 I = 0; I < ShopCount; ++I)
	{
		Xy.Add(ShopAt[I].X);
		Xy.Add(ShopAt[I].Y);
	}
	Shops.Picker.build(Xy.GetData(), static_cast<size_t>(ShopCount), kPickerCellCm);
	Shops.Scale.SetNumUninitialized(ShopCount);
	ftcore::nightLampScales(FTNight::Look(), Xy.GetData(), static_cast<size_t>(ShopCount), 100.f, Shops.Scale.GetData());
	for (int32 I = 0; I < ShopCount; ++I)
	{
		Shops.Scale[I] *= ShopGain[I];
	}

	auto Make = [this, Owner](FPool& Pool, TArray<TObjectPtr<USpotLightComponent>>& Lights, int32 Size, bool bLamp)
	{
		Lights.Reserve(Size);
		Pool.Slot.Init(INDEX_NONE, Size);
		Pool.Picked.SetNumZeroed(Size);
		Pool.Taken.Init(false, Size);
		Pool.Merged.SetNumZeroed(Size);
		for (int32 I = 0; I < Size; ++I)
		{
			USpotLightComponent* Light = NewObject<USpotLightComponent>(Owner, NAME_None, RF_Transient);
			Light->SetMobility(EComponentMobility::Movable);
			ApplyValues(*Light, bLamp);
			Light->SetVisibility(false);
			Light->SetWorldLocationAndRotation(Owner->GetActorLocation(), LampFacing);
			Lights.Add(Light);
		}
	};
	Make(Lamps, LampPool, FMath::Min(WantLamps, LampCount), true);
	Make(Shops, ShopPool, FMath::Min(WantShops, ShopCount), false);
}

void UFTLampPoolSubsystem::SetNight(bool bOn)
{
	if (bOn)
	{
		CreatePool();
	}
	bNight = bOn;
	auto Switch = [bOn](FPool& Pool, TArray<TObjectPtr<USpotLightComponent>>& Lights)
	{
		for (int32 I = 0; I < Lights.Num(); ++I)
		{
			USpotLightComponent* Light = Lights[I];
			if (!Light)
			{
				continue;
			}
			if (bOn && !Light->IsRegistered())
			{
				Light->RegisterComponent();
			}
			else if (!bOn)
			{
				// Freed by day, so the night starts with every slot to fill and every light hidden.
				Pool.Slot[I] = INDEX_NONE;
				Light->SetVisibility(false);
				if (Light->IsRegistered())
				{
					Light->UnregisterComponent();
				}
			}
		}
	};
	Switch(Lamps, LampPool);
	Switch(Shops, ShopPool);
	if (bOn && !bLogged)
	{
		bLogged = true;
		UE_LOG(LogForgottenThird, Display, TEXT("LampPool: %d lamps, %d shops in data; pool %d+%d live."), GetLampCount(), GetShopCount(),
			LampPool.Num(), ShopPool.Num());
	}
}

void UFTLampPoolSubsystem::PickPool(FPool& Pool, TArray<TObjectPtr<USpotLightComponent>>& Lights, bool bLamp, const FVector& From)
{
	const int32 Size = Lights.Num();
	if (Size == 0)
	{
		return;
	}
	int32 Count = static_cast<int32>(Pool.Picker.pick(static_cast<float>(From.X), static_cast<float>(From.Y), ReachCm,
		static_cast<size_t>(Size), Pool.Picked.GetData()));
	// -LampPoolHeroFirst: the hero corner's lights within reach first, then the picked rest, nearest first (no allocation:
	// Merged is the pool's size).
	if (bHeroFirst && Pool.HeroIdx.Num() > 0 && Pool.Merged.Num() == Size)
	{
		const float Reach2 = ReachCm * ReachCm;
		int32 N = 0;
		for (int32 H = 0; H < Pool.HeroIdx.Num() && N < Size; ++H)
		{
			const int32 Index = Pool.HeroIdx[H];
			const FVector3f& At = bLamp ? LampAt[Index] : ShopAt[Index];
			const float Dx = At.X - static_cast<float>(From.X);
			const float Dy = At.Y - static_cast<float>(From.Y);
			if (Dx * Dx + Dy * Dy <= Reach2)
			{
				Pool.Merged[N++] = static_cast<uint32>(Index);
			}
		}
		for (int32 P = 0; P < Count && N < Size; ++P)
		{
			const int32 Index = static_cast<int32>(Pool.Picked[P]);
			if (!Pool.Hero.IsValidIndex(Index) || !Pool.Hero[Index])
			{
				Pool.Merged[N++] = Pool.Picked[P];
			}
		}
		for (int32 P = 0; P < N; ++P)
		{
			Pool.Picked[P] = Pool.Merged[P];
		}
		Count = N;
	}
	for (int32 P = 0; P < Count; ++P)
	{
		Pool.Taken[P] = false;
	}
	// Slots whose light is still among the picked keep it; the rest are freed.
	for (int32 S = 0; S < Size; ++S)
	{
		const int32 Held = Pool.Slot[S];
		if (Held == INDEX_NONE)
		{
			continue;
		}
		bool bKept = false;
		for (int32 P = 0; P < Count; ++P)
		{
			if (!Pool.Taken[P] && static_cast<int32>(Pool.Picked[P]) == Held)
			{
				Pool.Taken[P] = true;
				bKept = true;
				break;
			}
		}
		if (!bKept)
		{
			Pool.Slot[S] = INDEX_NONE;
		}
	}
	// The newly picked take the free slots, nearest first.
	int32 Free = 0;
	for (int32 P = 0; P < Count; ++P)
	{
		if (Pool.Taken[P])
		{
			continue;
		}
		while (Free < Size && Pool.Slot[Free] != INDEX_NONE)
		{
			++Free;
		}
		if (Free == Size)
		{
			break;
		}
		const int32 Index = static_cast<int32>(Pool.Picked[P]);
		Pool.Slot[Free] = Index;
		if (USpotLightComponent* Light = Lights[Free])
		{
			const FVector At(bLamp ? LampAt[Index] : ShopAt[Index]);
			const FRotator Facing = bLamp ? LampFacing : FRotator(ShopPitch[Index], ShopYaw[Index], 0.f);
			Light->SetWorldLocationAndRotation(At, Facing);
			Light->SetIntensity(Candela(bLamp) * (Pool.Scale.IsValidIndex(Index) ? Pool.Scale[Index] : 1.f));
			// A hero corner slot burns warm (its timber rooms under paper lamps; its lamps 2400-2700 K by the face ruling, the
			// paper's amber kept to the paper), every other at the look's own. Set on every assignment: lights are recycled.
			const ftcore::NightLook& Look = FTNight::Look();
			const bool bHero = Pool.Hero.IsValidIndex(Index) && Pool.Hero[Index];
			const float HeroK = bLamp ? Look.heroLampKelvin : Look.heroShopKelvin;
			Light->SetTemperature(bHero && HeroK > 0.f ? HeroK : (bLamp ? Look.lampKelvin : Look.shopKelvin));
		}
	}
	// Each held lamp shows once the switch-on passes its own hashed point (shops keyed apart from the lamps). Tick runs
	// this every kPickSeconds while the fraction is above 0, camera moving or not, so a lamp comes on within 0.2 s of
	// its point.
	const float Fraction = LampFraction();
	const uint32 Salt = bLamp ? 0u : 0x9E3779B9u;
	for (int32 S = 0; S < Size; ++S)
	{
		if (USpotLightComponent* Light = Lights[S])
		{
			const int32 Held = Pool.Slot[S];
			Light->SetVisibility(Held != INDEX_NONE && ftcore::nightLampOn(Fraction, static_cast<uint32>(Held) ^ Salt));
		}
	}
	// -LampShadows: the lamps among the first ShadowLamps picked (nearest first) cast; the rest don't.
	if (bLamp && ShadowLamps > 0)
	{
		const int32 Near = FMath::Min(ShadowLamps, Count);
		for (int32 S = 0; S < Size; ++S)
		{
			USpotLightComponent* Light = Lights[S];
			const int32 Held = Pool.Slot[S];
			if (!Light)
			{
				continue;
			}
			bool bCast = false;
			for (int32 P = 0; P < Near && Held != INDEX_NONE; ++P)
			{
				bCast = bCast || static_cast<int32>(Pool.Picked[P]) == Held;
			}
			if ((Light->CastShadows != 0) != bCast)
			{
				Light->SetCastShadows(bCast);
			}
		}
	}
	// Shop shadows: the shop lights within ShopShadowCm of the camera cast; the rest don't.
	if (!bLamp && ShopShadowCm > 0.f)
	{
		const float Near2 = ShopShadowCm * ShopShadowCm;
		for (int32 S = 0; S < Size; ++S)
		{
			USpotLightComponent* Light = Lights[S];
			const int32 Held = Pool.Slot[S];
			if (!Light)
			{
				continue;
			}
			const bool bCast = Held != INDEX_NONE && ShopAt.IsValidIndex(Held) && FVector::DistSquared(FVector(ShopAt[Held]), From) <= Near2;
			if ((Light->CastShadows != 0) != bCast)
			{
				Light->SetCastShadows(bCast);
			}
		}
	}
}

void UFTLampPoolSubsystem::Pick()
{
	const UWorld* World = GetWorld();
	const APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
	const APlayerCameraManager* Camera = Player ? Player->PlayerCameraManager.Get() : nullptr;
	if (!Camera || !Data.IsValid())
	{
		return;
	}
	const FVector From = Camera->GetCameraLocation();
	PickPool(Lamps, LampPool, true, From);
	PickPool(Shops, ShopPool, false, From);
	if (bHeroLog)
	{
		LogHero(From);
	}
	const APawn* Pawn = Player->GetPawn();
	PickRim(Pawn ? Pawn->GetActorLocation() : From, FRotationMatrix(Camera->GetCameraRotation()).GetUnitAxis(EAxis::Y));
}

void UFTLampPoolSubsystem::PickRim(const FVector& Lead, const FVector& Right)
{
	constexpr int32 kMax = kMaxLamps + kMaxShops;
	ftcore::RimLamp Lit[kMax];
	FLinearColor Colour[kMax];
	int32 N = 0;
	for (const TArray<TObjectPtr<USpotLightComponent>>* Pool : {&LampPool, &ShopPool})
	{
		for (const USpotLightComponent* Light : *Pool)
		{
			if (!Light || !Light->IsVisible() || Light->Intensity <= 0.f || N == kMax)
			{
				continue;
			}
			const FVector At = Light->GetComponentLocation();
			ftcore::RimLamp& L = Lit[N];
			L.at[0] = static_cast<float>(At.X);
			L.at[1] = static_cast<float>(At.Y);
			L.at[2] = static_cast<float>(At.Z);
			L.candela = Light->Intensity;
			L.on = true;
			FLinearColor C = Light->GetLightColor();
			if (Light->bUseTemperature)
			{
				C *= FLinearColor::MakeFromColorTemperature(Light->Temperature);
			}
			// The hue only, at the rim default's brightness (its largest channel 1).
			const float Peak = FMath::Max3(C.R, C.G, C.B);
			Colour[N] = Peak > 0.f ? FLinearColor(C.R / Peak, C.G / Peak, C.B / Peak, 1.f) : RimDefaultColour();
			++N;
		}
	}
	const float At[3] = {static_cast<float>(Lead.X), static_cast<float>(Lead.Y), static_cast<float>(Lead.Z)};
	const float Side[3] = {static_cast<float>(Right.X), static_cast<float>(Right.Y), static_cast<float>(Right.Z)};
	const ftcore::RimPick Best = ftcore::rimPickLamp(Lit, static_cast<std::size_t>(N), At, Side, kRimMinDistCm);
	const FLinearColor Target = Best.index >= 0 ? Colour[Best.index] : RimDefaultColour();
	if (!Target.Equals(RimTarget, 1e-4f))
	{
		RimTarget = Target;
		bRimSettled = false;
	}
#if FT_HAS_RIM_COLLECTION
	FTRim::SetPreferredSide(Best.index >= 0 ? Best.side : 0.f);
#endif
}

void UFTLampPoolSubsystem::EaseRim(float DeltaTime)
{
	if (bRimSettled)
	{
		return;
	}
	RimNow = FMath::Lerp(RimNow, RimTarget, ftcore::rimEaseShare(DeltaTime, ftcore::kRimColourEaseSeconds));
	if (RimNow.Equals(RimTarget, 2e-3f))
	{
		RimNow = RimTarget;
		bRimSettled = true;
	}
	WriteRim(RimNow);
}

void UFTLampPoolSubsystem::ResetRim()
{
	RimTarget = RimDefaultColour();
	RimNow = RimDefaultColour();
	bRimSettled = true;
	WriteRim(RimDefaultColour());
#if FT_HAS_RIM_COLLECTION
	FTRim::SetPreferredSide(0.f);
#endif
}

void UFTLampPoolSubsystem::WriteRim(const FLinearColor& Colour)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (!bRimTried)
	{
		bRimTried = true;
		const FString Path = FString(RimCollectionPath) + TEXT(".") + FPackageName::GetShortName(RimCollectionPath);
		RimCollection = LoadObject<UMaterialParameterCollection>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (UMaterialParameterCollectionInstance* Params = RimCollection ? World->GetParameterCollectionInstance(RimCollection) : nullptr)
	{
		Params->SetVectorParameterValue(FName(RimColourName), Colour);
	}
}

void UFTLampPoolSubsystem::LogHero(const FVector& From)
{
	// A key over the corner's held slots; the line goes out when it changes or the camera has moved a metre (a new eye),
	// so a still camera logs once.
	auto Live = [](const FPool& Pool, int32 Index)
	{
		for (int32 S = 0; S < Pool.Slot.Num(); ++S)
		{
			if (Pool.Slot[S] == Index)
			{
				return S;
			}
		}
		return static_cast<int32>(INDEX_NONE);
	};
	uint32 Key = 2166136261u;
	for (const FPool* Pool : {&Lamps, &Shops})
	{
		for (const int32 Index : Pool->HeroIdx)
		{
			Key = (Key ^ static_cast<uint32>(Live(*Pool, Index) + 1)) * 16777619u;
		}
	}
	if (Key == HeroLogKey && FVector::DistSquared(From, HeroLogAt) < 100.f * 100.f)
	{
		return;
	}
	HeroLogKey = Key;
	HeroLogAt = From;
	auto List = [&Live](const FPool& Pool, int32& Count)
	{
		FString Out;
		Count = 0;
		for (int32 H = 0; H < Pool.HeroIdx.Num(); ++H)
		{
			const int32 Slot = Live(Pool, Pool.HeroIdx[H]);
			Count += Slot != INDEX_NONE ? 1 : 0;
			Out += Slot != INDEX_NONE ? FString::Printf(TEXT(" %d"), Pool.HeroLocal[H]) : FString::Printf(TEXT(" (%d)"), Pool.HeroLocal[H]);
		}
		return Out;
	};
	int32 LiveShops = 0, LiveLamps = 0;
	const FString ShopList = List(Shops, LiveShops);
	const FString LampList = List(Lamps, LiveLamps);
	UE_LOG(LogForgottenThird, Display, TEXT("LampPool: hero corner at camera (%.0f, %.0f, %.0f): shops %d/%d live:%s; lamps %d/%d live:%s (bracketed: dark)."),
		From.X, From.Y, From.Z, LiveShops, Shops.HeroIdx.Num(), *ShopList, LiveLamps, Lamps.HeroIdx.Num(), *LampList);
}

void UFTLampPoolSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// The rim colour eases every frame; the pick sets its target every kPickSeconds.
	EaseRim(DeltaTime);
	SincePick += DeltaTime;
	if (SincePick < kPickSeconds)
	{
		return;
	}
	SincePick = 0.f;
	const bool bWant = IsNightNow();
	if (bWant != bNight)
	{
		SetNight(bWant);
		if (!bWant)
		{
			ResetRim();
		}
	}
	if (bNight)
	{
		Pick();
	}
}

TStatId UFTLampPoolSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTLampPoolSubsystem, STATGROUP_Tickables);
}

bool UFTLampPoolSubsystem::ForceNight(float LampCandela, float LampReachCm, float ShopCandela, float ShopReachCm)
{
	if (!bActive || !Data.IsValid())
	{
		return false;
	}
	LampCandelaOverride = LampCandela;
	LampReachOverride = LampReachCm;
	ShopCandelaOverride = ShopCandela;
	ShopReachOverride = ShopReachCm;
	bForced = true;
	for (USpotLightComponent* Light : LampPool)
	{
		if (Light)
		{
			ApplyValues(*Light, true);
		}
	}
	for (USpotLightComponent* Light : ShopPool)
	{
		if (Light)
		{
			ApplyValues(*Light, false);
		}
	}
	// Every slot is filled again below, so each light takes its lamp's crowding share at the new values.
	for (int32& Held : Lamps.Slot)
	{
		Held = INDEX_NONE;
	}
	for (int32& Held : Shops.Slot)
	{
		Held = INDEX_NONE;
	}
	if (!bNight)
	{
		SetNight(true);
	}
	Pick();
	return true;
}
