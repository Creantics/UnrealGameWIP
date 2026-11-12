#pragma once

// Config-driven looks for the explore screen's characters (the pawn and the party following it): a body with its
// locomotion clips, skeletal-mesh pieces worn over it (clothes, a MetaHuman face) and hair grooms bound to one of
// them (brows, lashes). Grooms are made by reflection, so the module has no HairStrands dependency: without that
// plugin they are skipped with one warning.
//
// Gate captures: -LocoNoPieces leaves every piece off, -LocoNoPiece=hair+collar those whose asset name holds any of
// the words, to find which one breaks up as the body turns. Collar pieces draw by default (-LocoNoPiece=collar
// leaves them off).
//
// Open and closed garments (ftcore/GarmentFront.h): a jacket or coat is two fitted meshes, closed and open, and the
// look says which it wears by default. -LookFront=open|closed, or word:state items joined by + (jacket:open+coat:closed),
// picks the state at setup; a scene can do the same (FTLook::SetSceneFronts) or switch a worn piece (FTLook::SetFront).

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

// A material worn in place of another on the body and the pieces: every slot showing From shows To instead (a
// MetaHuman face's FaceHide skin, masked under the garments, so the mesh and its DNA stay as built).
USTRUCT()
struct FFTMaterialSwap
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath From;
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath To;
};

// A garment's two fitted meshes, worn closed or open (the lining shows). PiecePaths lists either one; the look wears
// Open when bOpen, else Closed, unless a scene or -LookFront asks otherwise. An Open that is empty or not made yet
// (no such package) keeps the garment closed, so a look can name an open mesh before it is imported.
USTRUCT()
struct FFTGarmentFronts
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath Closed;
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath Open;
	UPROPERTY(EditAnywhere, Category = "Look")
	bool bOpen = false;
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
	// Shirt-off: BodyMeshPath may be cut away under the covering garments (a BodyHide copy). While a piece named by
	// a cover word is left off, the look wears FullBodyMeshPath, the uncut body, so no hole shows (modular outfits).
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath FullBodyMeshPath;
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FString> CoverWords;
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
	// Garments worn open or closed (FFTGarmentFronts); PiecePaths' entry for one stands for the pair.
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FFTGarmentFronts> Fronts;
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FFTGroomLook> Grooms;
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FFTMaterialSwap> MaterialSwaps;
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

// True when the piece is left off by the command line (-LocoNoPieces, or -LocoNoPiece=a+b naming it).
FORGOTTENTHIRD_API bool PieceLeftOff(const FSoftObjectPath& Piece);

// The look's pieces as worn: each garment pair's half for its state (its default, then the scene's asks, then
// -LookFront's). Without Fronts this is PiecePaths.
FORGOTTENTHIRD_API TArray<FSoftObjectPath> WornPiecePaths(const FFTLeadLook& Look);

// The scene's front asks ("open", "closed" or word:state items joined by +), used by every look set up after this
// call; empty clears them. False (and nothing changed) when the text doesn't parse.
FORGOTTENTHIRD_API bool SetSceneFronts(const FString& Asks);

// Switches the worn pieces of the look's pairs that `Word` names (every pair when empty) to `bOpen`: each such piece
// component gets the other fitted mesh, keeping its leader pose and materials as imported. Returns how many changed.
FORGOTTENTHIRD_API int32 SetFront(const FFTLeadLook& Look, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces,
	const FString& Word, bool bOpen, const TCHAR* Who);

// Adds a component per piece mesh, attached to the body and posed by it (leader pose). Missing meshes are skipped
// with a warning that names `Who`. Returns how many of the added pieces have cloth.
FORGOTTENTHIRD_API int32 AddPieces(AActor& Owner, USkeletalMeshComponent& Body, const TArray<FSoftObjectPath>& Paths,
	TArray<TObjectPtr<USkeletalMeshComponent>>& OutPieces, const TCHAR* Who);

// Adds a groom component per entry, bound to the body or to the piece whose mesh is the binding's target (the body
// when no piece is). Returns how many were added.
FORGOTTENTHIRD_API int32 AddGrooms(AActor& Owner, USkeletalMeshComponent& Body, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces,
	const TArray<FFTGroomLook>& Grooms, TArray<TObjectPtr<USceneComponent>>& OutGrooms, const TCHAR* Who);

// Puts each swap's To in every slot of the body and the pieces that shows its From. A swap that doesn't load or whose
// From no slot shows is warned about, naming `Who`. Returns how many slots changed.
FORGOTTENTHIRD_API int32 SwapMaterials(USkeletalMeshComponent& Body, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces,
	const TArray<FFTMaterialSwap>& Swaps, const TCHAR* Who);
}
