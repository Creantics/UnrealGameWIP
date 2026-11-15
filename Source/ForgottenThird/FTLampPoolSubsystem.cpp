#include "FTLampPoolSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SpotLightComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTNightLightData.h"
#include "FTNightLook.h"
#include "FTWorldClockSubsystem.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
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
	for (TActorIterator<AFTNightLights> It(&InWorld); It; ++It)
	{
		if (UFTNightLightData* Found = It->FindComponentByClass<UFTNightLightData>())
		{
			NightLights = *It;
			Data = Found;
			break;
		}
	}
	if (!Data.IsValid())
	{
		return;
	}
	const TCHAR* Cmd = FCommandLine::Get();
	FParse::Value(Cmd, TEXT("LampPoolLamps="), WantLamps);
	FParse::Value(Cmd, TEXT("LampPoolShops="), WantShops);
	float ReachMetres = kReachMetres;
	if (FParse::Value(Cmd, TEXT("LampPoolRadius="), ReachMetres))
	{
		ReachCm = FMath::Max(ReachMetres, 0.f) * 100.f;
	}
	WantLamps = FMath::Clamp(WantLamps, 0, kMaxLamps);
	WantShops = FMath::Clamp(WantShops, 0, kMaxShops);
	bActive = true;
	// The first tick looks at the clock straight away.
	SincePick = kPickSeconds;
}

int32 UFTLampPoolSubsystem::GetLampCount() const
{
	const UFTNightLightData* D = Data.Get();
	return D ? D->LampAt.Num() : 0;
}

int32 UFTLampPoolSubsystem::GetShopCount() const
{
	const UFTNightLightData* D = Data.Get();
	return D ? FMath::Min(D->ShopAt.Num(), D->ShopYaw.Num()) : 0;
}

bool UFTLampPoolSubsystem::IsNightNow() const
{
	if (bForced)
	{
		return true;
	}
	const UWorld* World = GetWorld();
	const UFTWorldClockSubsystem* Clock = World ? World->GetSubsystem<UFTWorldClockSubsystem>() : nullptr;
	// The clock's own lamps switch on about NightAmount 0.5 (each at a hashed point between 0.4 and 0.6).
	return Clock && Clock->IsRunning() && Clock->GetNightAmount() > 0.5f;
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
	AFTNightLights* Owner = NightLights.Get();
	const UFTNightLightData* D = Data.Get();
	if (bPoolMade || !Owner || !D)
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
		Xy.Add(D->LampAt[I].X);
		Xy.Add(D->LampAt[I].Y);
	}
	Lamps.Picker.build(Xy.GetData(), static_cast<size_t>(LampCount), kPickerCellCm);
	Lamps.Scale.SetNumUninitialized(LampCount);
	ftcore::nightLampScales(FTNight::Look(), Xy.GetData(), static_cast<size_t>(LampCount), 100.f, Lamps.Scale.GetData());
	Xy.Reset();
	for (int32 I = 0; I < ShopCount; ++I)
	{
		Xy.Add(D->ShopAt[I].X);
		Xy.Add(D->ShopAt[I].Y);
	}
	Shops.Picker.build(Xy.GetData(), static_cast<size_t>(ShopCount), kPickerCellCm);
	Shops.Scale.SetNumUninitialized(ShopCount);
	ftcore::nightLampScales(FTNight::Look(), Xy.GetData(), static_cast<size_t>(ShopCount), 100.f, Shops.Scale.GetData());

	auto Make = [this, Owner](FPool& Pool, TArray<TObjectPtr<USpotLightComponent>>& Lights, int32 Size, bool bLamp)
	{
		Lights.Reserve(Size);
		Pool.Slot.Init(INDEX_NONE, Size);
		Pool.Picked.SetNumZeroed(Size);
		Pool.Taken.Init(false, Size);
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
	const UFTNightLightData& D = *Data.Get();
	const int32 Count = static_cast<int32>(Pool.Picker.pick(static_cast<float>(From.X), static_cast<float>(From.Y), ReachCm,
		static_cast<size_t>(Size), Pool.Picked.GetData()));
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
			const FVector At(bLamp ? D.LampAt[Index] : D.ShopAt[Index]);
			const FRotator Facing = bLamp ? LampFacing : FRotator(D.ShopPitch, D.ShopYaw[Index], 0.f);
			Light->SetWorldLocationAndRotation(At, Facing);
			Light->SetIntensity(Candela(bLamp) * (Pool.Scale.IsValidIndex(Index) ? Pool.Scale[Index] : 1.f));
		}
	}
	for (int32 S = 0; S < Size; ++S)
	{
		if (USpotLightComponent* Light = Lights[S])
		{
			Light->SetVisibility(Pool.Slot[S] != INDEX_NONE);
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
}

void UFTLampPoolSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
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
