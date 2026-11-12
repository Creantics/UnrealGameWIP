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
// picks the state at setup (not in shipping); a scene can do the same (FTLook::SetSceneFronts) or switch a worn piece (FTLook::SetFront).

#include "CoreMinimal.h"

#include "FTLocomotionAnimInstance.h"

#include "FTLook.generated.h"

class AActor;
class UAnimSequence;
class UMaterialInterface;
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

// A look's garment pairs as set up (FTLook::PrepareFronts): both halves loaded once and held here, so switching never
// loads on the gameplay path; one entry per FFTLeadLook::Fronts entry, null where the mesh isn't made. The actor wearing
// the look keeps this as a UPROPERTY so the meshes stay loaded.
USTRUCT()
struct FFTWornFronts
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<TObjectPtr<USkeletalMesh>> Closed;
	UPROPERTY()
	TArray<TObjectPtr<USkeletalMesh>> Open;
	// The look's MaterialSwaps, resolved once when the look has pairs (one entry per swap that loads, From and To at
	// the same index), so a switch re-applies them without loading.
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> SwapFrom;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> SwapTo;
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
	// The character model the look is (an id from Data/Assets/models.json). The story director puts the look on
	// screen for a scene whose cast speaker is cast as this model (Data/Voice/speakers.json). Empty: never cast.
	UPROPERTY(EditAnywhere, Category = "Look")
	FString ModelId;
};

// A look's body and its base locomotion clips (UFTLocomotionAnimInstance::SetBaseClips' order), loaded; null where
// nothing loaded, and no clips without a body. ClipFolder is where the retargeted clips were looked for.
struct FFTLoadedLook
{
	USkeletalMesh* Body = nullptr;
	UAnimSequence* Clips[UFTLocomotionAnimInstance::NumBaseClips] = {};
	FString ClipFolder;
	// The look's garment pairs, both halves loaded (FTLook::PrepareFronts), and the pieces as worn with them
	// (FTLook::WornPiecePaths). The wearer copies Fronts into a UPROPERTY before the next garbage collection.
	FFTWornFronts Fronts;
	TArray<FSoftObjectPath> Pieces;
};

namespace FTLook
{
// Loads a look's body and clips (FFTLeadLook). A set path that doesn't load is warned about, naming `Who`.
FORGOTTENTHIRD_API FFTLoadedLook LoadLook(const FFTLeadLook& Look, const TCHAR* Who);

// True when the piece is left off by the command line (-LocoNoPieces, or -LocoNoPiece=a+b naming it).
FORGOTTENTHIRD_API bool PieceLeftOff(const FSoftObjectPath& Piece);

// Loads both halves of each of the look's garment pairs, once per look setup. Whether an open mesh exists is decided
// here: its package is looked up (in a cooked build, a package that wasn't cooked doesn't exist, so an open mesh not
// referenced by the cook stays closed) and loaded. Front asks whose word names no pair are warned about.
FORGOTTENTHIRD_API FFTWornFronts PrepareFronts(const FFTLeadLook& Look, const TCHAR* Who);

// The look's pieces as worn: each garment pair's half for its state (its default, then the scene's asks, then
// -LookFront's), the closed half when the open mesh isn't made, whichever half PiecePaths lists. Without Fronts this
// is PiecePaths.
FORGOTTENTHIRD_API TArray<FSoftObjectPath> WornPiecePaths(const FFTLeadLook& Look, const FFTWornFronts& Fronts);

// The scene's front asks ("open", "closed" or word:state items joined by +), used by every look set up after this
// call; empty clears them. False (and nothing changed) when the text doesn't parse.
FORGOTTENTHIRD_API bool SetSceneFronts(const FString& Asks);

// Switches the worn pieces of the look's pairs that `Word` names (any case, in the asset name; every pair when empty)
// to `bOpen`, using the meshes PrepareFronts held. Each such piece component gets the other fitted mesh: its material
// overrides are dropped (the new mesh's own slots show) and the look's swaps (held in Fronts) applied again on it, quietly
// where its slots show none of their Froms; the leader pose map
// is rebuilt on `Body` and the cloth actors are made again. A word naming no pair is warned about. Returns how many
// pieces changed.
FORGOTTENTHIRD_API int32 SetFront(const FFTLeadLook& Look, const FFTWornFronts& Fronts, USkeletalMeshComponent& Body,
	const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces, const FString& Word, bool bOpen, const TCHAR* Who);

// Adds a component per piece mesh, attached to the body and posed by it (leader pose). Missing meshes are skipped
// with a warning that names `Who`. Returns how many of the added pieces have cloth.
FORGOTTENTHIRD_API int32 AddPieces(AActor& Owner, USkeletalMeshComponent& Body, const TArray<FSoftObjectPath>& Paths,
	TArray<TObjectPtr<USkeletalMeshComponent>>& OutPieces, const TCHAR* Who);

// Adds a groom component per entry, bound to the body or to the piece whose mesh is the binding's target (the body
// when no piece is). Returns how many were added.
FORGOTTENTHIRD_API int32 AddGrooms(AActor& Owner, USkeletalMeshComponent& Body, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces,
	const TArray<FFTGroomLook>& Grooms, TArray<TObjectPtr<USceneComponent>>& OutGrooms, const TCHAR* Who);

// Puts To in every slot of the component that shows From (From itself or an instance made from it), quietly. Returns
// how many slots changed.
FORGOTTENTHIRD_API int32 SwapSlots(USkeletalMeshComponent& Component, const UMaterialInterface* From, UMaterialInterface* To);

// Puts each swap's To in every slot of the body and the pieces that shows its From. A swap that doesn't load or whose
// From no slot shows is warned about, naming `Who`. Returns how many slots changed.
FORGOTTENTHIRD_API int32 SwapMaterials(USkeletalMeshComponent& Body, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces,
	const TArray<FFTMaterialSwap>& Swaps, const TCHAR* Who);
}
