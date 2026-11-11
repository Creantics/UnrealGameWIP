#include "FTMaterialCheck.h"

#include "ForgottenThird.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

// Named, not anonymous: the module builds in unity files.
namespace FTMaterialCheck::Impl
{
// Why a slot's material fails: empty when it holds.
FString Fault(const UMaterialInterface* Material, const UMaterial* Default)
{
	if (!Material)
	{
		return TEXT("null");
	}
	const FString Path = Material->GetPathName();
	// The default itself, by object or by name (an engine build may hand out a copy), or an instance resolving to it.
	if (Material == Default || Path.Contains(TEXT("WorldGridMaterial")) || Path.EndsWith(TEXT(".DefaultMaterial")))
	{
		return FString::Printf(TEXT("default (%s)"), *Path);
	}
	const UMaterial* const Root = Material->GetMaterial();
	if (!Root)
	{
		return FString::Printf(TEXT("%s has no base material"), *Path);
	}
	if (Root == Default || Root->GetName() == TEXT("WorldGridMaterial") || Root->GetName() == TEXT("DefaultMaterial"))
	{
		return FString::Printf(TEXT("%s resolves to the default (%s)"), *Path, *Root->GetPathName());
	}
	return FString();
}
} // namespace FTMaterialCheck::Impl

int32 FTMaterialCheck::Count(const UWorld* World)
{
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("MaterialCheck: no world to check."));
		return 1;
	}
	const UMaterial* const Default = UMaterial::GetDefaultMaterial(MD_Surface);
	int32 Components = 0, Slots = 0, Skipped = 0, Bad = 0, BadSlots = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* const Actor = *It;
		TInlineComponentArray<UStaticMeshComponent*> Meshes;
		Actor->GetComponents(Meshes);
		for (const UStaticMeshComponent* Component : Meshes)
		{
			const UStaticMesh* const Mesh = Component ? Component->GetStaticMesh() : nullptr;
			const UInstancedStaticMeshComponent* const Instanced = Cast<UInstancedStaticMeshComponent>(Component);
			if (!Mesh || Actor->IsHidden() || Component->bHiddenInGame || !Component->GetVisibleFlag() ||
				(Instanced && Instanced->GetInstanceCount() == 0))
			{
				++Skipped;
				continue;
			}
			++Components;
			FString Faults;
			const int32 SlotCount = Component->GetNumMaterials();
			for (int32 Slot = 0; Slot < SlotCount; ++Slot)
			{
				++Slots;
				const FString Why = FTMaterialCheck::Impl::Fault(Component->GetMaterial(Slot), Default);
				if (!Why.IsEmpty())
				{
					++BadSlots;
					Faults += FString::Printf(TEXT("%sslot %d %s"), Faults.IsEmpty() ? TEXT("") : TEXT(", "), Slot, *Why);
				}
			}
			if (SlotCount == 0)
			{
				++BadSlots;
				Faults = TEXT("no material slot");
			}
			if (Faults.IsEmpty())
			{
				continue;
			}
			if (Bad < kMaxLines)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("MaterialCheck: %s.%s mesh %s%s: %s"), *Actor->GetActorNameOrLabel(), *Component->GetName(),
					*Mesh->GetPathName(), Instanced ? *FString::Printf(TEXT(" (%d instances)"), Instanced->GetInstanceCount()) : TEXT(""), *Faults);
			}
			++Bad;
		}
	}
	if (Bad > 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("MaterialCheck: FAILED: %d of %d drawn mesh components have a null or default material (%d of %d slots; "
			"%d listed); %d hidden or empty components left out."), Bad, Components, BadSlots, Slots, FMath::Min(Bad, kMaxLines), Skipped);
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("MaterialCheck: OK: %d drawn mesh components, %d material slots, none null or default; %d hidden or "
			"empty components left out."), Components, Slots, Skipped);
	}
	return Bad;
}

bool FTMaterialCheck::Run(const UWorld* World)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("MaterialCheck")))
	{
		return true;
	}
	return Count(World) == 0;
}
