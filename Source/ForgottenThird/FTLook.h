#pragma once

// Config-driven looks for the explore screen's characters (the pawn and the party following it): a body with its
// idle and walk clips, skeletal-mesh pieces worn over it (clothes, a MetaHuman face) and hair grooms bound to one of
// them (brows, lashes). Grooms are made by reflection, so the module has no HairStrands dependency: without that
// plugin they are skipped with one warning.

#include "CoreMinimal.h"

#include "FTLook.generated.h"

class AActor;
class UAnimSequence;
class USceneComponent;
class USkeletalMesh;
class USkeletalMeshComponent;

// A groom and the binding that fits it to a skeletal mesh. The groom goes on whichever of the body and the
// pieces has the binding's target mesh (a MetaHuman build binds brows and lashes to the face).
USTRUCT()
struct FFTGroomLook
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath Groom;
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath Binding;
};

// One party member's look. An empty path (or one that doesn't load) uses the member's own asset under
// /Game/Characters/<Name>/<name>/SkeletalMeshes/, as the Tripo leads were imported.
USTRUCT()
struct FFTLeadLook
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Look")
	FString Name;
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath BodyMeshPath;
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath IdleClipPath;
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath WalkClipPath;
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FSoftObjectPath> PiecePaths;
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FFTGroomLook> Grooms;
};

// A look's body and clips, loaded; null where neither the path nor the member's own asset loaded.
struct FFTLoadedLook
{
	USkeletalMesh* Body = nullptr;
	UAnimSequence* Idle = nullptr;
	UAnimSequence* Walk = nullptr;
};

namespace FTLook
{
// Loads a look's body and clips (<name>, <name>idle and <name>walk when falling back). A set path that doesn't
// load is warned about, naming `Who`.
FORGOTTENTHIRD_API FFTLoadedLook LoadLook(const FFTLeadLook& Look, const TCHAR* Who);

// Adds a component per piece mesh, attached to the body and posed by it (leader pose). Missing meshes are skipped
// with a warning that names `Who`. Returns how many of the added pieces have cloth.
FORGOTTENTHIRD_API int32 AddPieces(AActor& Owner, USkeletalMeshComponent& Body, const TArray<FSoftObjectPath>& Paths,
	TArray<TObjectPtr<USkeletalMeshComponent>>& OutPieces, const TCHAR* Who);

// Adds a groom component per entry, bound to the body or to the piece whose mesh is the binding's target (the body
// when no piece is). Returns how many were added.
FORGOTTENTHIRD_API int32 AddGrooms(AActor& Owner, USkeletalMeshComponent& Body, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces,
	const TArray<FFTGroomLook>& Grooms, TArray<TObjectPtr<USceneComponent>>& OutGrooms, const TCHAR* Who);
}
