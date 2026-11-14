// ft.LumenProxyDump [name filter]: a debug count of the world's primitive proxies by class, and how many of each the
// Lumen scene can take (FPrimitiveSceneProxy::UpdateVisibleInLumenScene's terms: affects dynamic indirect lighting, has
// a ray-tracing representation, visible in ray tracing, drawn in game), plus up to 12 lines for components whose name or
// owner's name holds the filter. The flags are read from the game thread; they are set when the proxy is made, so the
// read is good enough for a debug log. Not for shipping use.

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "PrimitiveSceneProxy.h"
#include "UObject/UObjectIterator.h"

// A debug tool: compiled out of Shipping (no console command there).
#if !UE_BUILD_SHIPPING

namespace
{
struct FProxyTally
{
	int32 Components = 0;
	int32 Proxies = 0;
	int32 Lumen = 0;
	int32 Indirect = 0;
	int32 RayRep = 0;
	int32 RayVisible = 0;
	int32 Drawn = 0;
	int64 Instances = 0;
};

void DumpLumenProxies(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}
	const FString Filter = Args.Num() > 0 ? Args[0] : FString();
	TMap<FName, FProxyTally> Tallies;
	int32 Lines = 0;
	for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
	{
		UPrimitiveComponent* Component = *It;
		if (!Component || Component->GetWorld() != World || !Component->IsRegistered())
		{
			continue;
		}
		FProxyTally& Tally = Tallies.FindOrAdd(Component->GetClass()->GetFName());
		++Tally.Components;
		const FPrimitiveSceneProxy* Proxy = Component->SceneProxy;
		if (!Proxy)
		{
			continue;
		}
		++Tally.Proxies;
		bool bRayRep = false;
#if RHI_RAYTRACING
		bRayRep = Proxy->HasRayTracingRepresentation();
#endif
		Tally.Lumen += Proxy->IsVisibleInLumenScene() ? 1 : 0;
		Tally.Indirect += Proxy->AffectsDynamicIndirectLighting() ? 1 : 0;
		Tally.RayRep += bRayRep ? 1 : 0;
		Tally.RayVisible += Proxy->IsVisibleInRayTracing() ? 1 : 0;
		Tally.Drawn += Proxy->IsDrawnInGame() ? 1 : 0;
		const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component);
		const int32 Count = Instanced ? Instanced->GetInstanceCount() : 1;
		Tally.Instances += Count;
		const AActor* Owner = Component->GetOwner();
		const FString Name = Owner ? FString::Printf(TEXT("%s.%s"), *Owner->GetName(), *Component->GetName()) : Component->GetName();
		if (!Filter.IsEmpty() && Lines < 12 && Name.Contains(Filter))
		{
			++Lines;
			UE_LOG(LogTemp, Display,
				TEXT("LumenProxyDump: %s (%s) instances %d lumen %d indirect %d rayRep %d rayVisible %d drawn %d group %d"),
				*Name, *Component->GetClass()->GetName(), Count, Proxy->IsVisibleInLumenScene() ? 1 : 0,
				Proxy->AffectsDynamicIndirectLighting() ? 1 : 0, bRayRep ? 1 : 0, Proxy->IsVisibleInRayTracing() ? 1 : 0,
				Proxy->IsDrawnInGame() ? 1 : 0, Proxy->GetRayTracingGroupId());
		}
	}
	for (const TPair<FName, FProxyTally>& Pair : Tallies)
	{
		const FProxyTally& T = Pair.Value;
		UE_LOG(LogTemp, Display,
			TEXT("LumenProxyDump: %s components %d proxies %d instances %lld lumen %d indirect %d rayRep %d rayVisible %d drawn %d"),
			*Pair.Key.ToString(), T.Components, T.Proxies, T.Instances, T.Lumen, T.Indirect, T.RayRep, T.RayVisible, T.Drawn);
	}
}

FAutoConsoleCommandWithWorldAndArgs GLumenProxyDump(TEXT("ft.LumenProxyDump"),
	TEXT("ft.LumenProxyDump [name filter]: per class, how many primitive proxies the Lumen scene can take, and why not."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DumpLumenProxies));
} // namespace

#endif // !UE_BUILD_SHIPPING
