#pragma once

// Config-driven looks for the explore screen's characters (the pawn and the party following it): a body with its
// locomotion clips, skeletal-mesh pieces worn over it (clothes, a MetaHuman face) and hair grooms bound to one of
// them (brows, lashes). Grooms are made by reflection, so the module has no HairStrands dependency: without that
// plugin they are skipped with one warning.
//
// Gate captures: -LocoNoPieces leaves every piece off, -LocoNoPiece=hair+collar those whose asset name holds any of
// the words, to find which one breaks up as the body turns. The collar is left off unless -LocoCollar: it stands off
// the shoulders as a flat plate behind the head once the body moves.

#include "CoreMinimal.h"

#include "FTLocomotionAnimInstance.h"

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

// One party member's look. An empty body path (or one that doesn't load) uses the member's own body under
// /Game/Characters/<Name>/<name>/SkeletalMeshes/, as the Tripo leads were imported. Each locomotion clip is the
// look's own (the idle and walk paths), else the one retargeted onto the body in ClipFolder, else (the idle and
// walk) the member's own; a clip made for another skeleton is never worn.
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
	// Where the clips retargeted onto the body are (FTBuildRig names them <source>_to_<body>); empty is
	// /Game/Characters/<Name>/Rig/Retargeted.
	UPROPERTY(EditAnywhere, Category = "Look")
	FString ClipFolder;
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FSoftObjectPath> PiecePaths;
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FFTGroomLook> Grooms;
};

// A look's body and its base locomotion clips (UFTLocomotionAnimInstance::SetBaseClips' order), loaded; null where
// nothing loaded, and no clips without a body. ClipFolder is where the retargeted clips were looked for.
struct FFTLoadedLook
{
	USkeletalMesh* Body = nullptr;
	UAnimSequence* Clips[UFTLocomotionAnimInstance::NumBaseClips] = {};
	FString ClipFolder;
};

namespace FTLook
{
// Loads a look's body and clips (FFTLeadLook). A set path that doesn't load is warned about, naming `Who`.
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
