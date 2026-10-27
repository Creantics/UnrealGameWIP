#pragma once

// Extra pieces on a character's body (clothes, hair, a MetaHuman's face): skeletal meshes that follow
// the body's pose (leader pose) and face shapes. Gate captures: -LocoNoPieces leaves every piece off,
// -LocoNoPiece=hair+collar those whose asset name holds any of the words, to find which one breaks up as
// the body turns. The collar is left off unless -LocoCollar: it stands off the shoulders as a flat plate
// behind the head once the body moves.

#include "CoreMinimal.h"
#include "UObject/ObjectPtr.h"
#include "UObject/SoftObjectPath.h"

class AActor;
class USkeletalMeshComponent;

namespace FTMeshPieces
{
// Attaches a component for each piece at Paths to Body, owned by Owner, adding them to Out; missing
// meshes are skipped with a warning tagged Who. Returns how many of them carry cloth.
FORGOTTENTHIRD_API int32 Attach(AActor& Owner, USkeletalMeshComponent& Body, const TArray<FSoftObjectPath>& Paths,
	TArray<TObjectPtr<USkeletalMeshComponent>>& Out, const TCHAR* Who);
}
