#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFTHairCards, Log, All);

namespace FTHairCards
{
// True once the hair card generator's Python controller exists. The generator's startup script makes it on the
// editor's first ticks, after any pip install it waits for.
bool IsGeneratorReady();

// Builds and saves every groom in the spec (a path relative to the project folder, or absolute), or only the one
// with id Only when that isn't empty. Returns how many grooms failed, or -1 when the spec is refused.
int32 Run(const FString& SpecPath, const FString& Only);
} // namespace FTHairCards
