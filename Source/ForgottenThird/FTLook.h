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
//
// Under layers (ftcore::look::UnderChoice): an under piece (a tee) worn under an over piece (a jacket) is swapped for a
// cut copy of it while that over piece is worn, so it never shows through; taking the over piece off (or -LocoNoPiece)
// wears the whole piece again, and opening or closing the over piece picks the cut made for that front.

#include "CoreMinimal.h"

#include "FTLocomotionAnimInstance.h"

#include "FTLook.generated.h"

class AActor;
class UAnimSequence;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class USkeletalMesh;
class USkeletalMeshComponent;
namespace ftcore
{
struct WaterWetness;
}
struct FStreamableHandle;

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
	// The same hair soaked, worn in its place while the lead is wet (the wet switch, ftcore/LeadWet.h); empty keeps the
	// groom as it is. Its binding is its own (a binding is made for one groom); empty wears it unbound.
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath WetGroom;
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath WetBinding;
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

// A cut copy of an under piece (FFTLeadLook::UnderLayers): Cut is worn in place of Under while the over piece OverWord
// names is worn. OverWord is best that piece's path (it names that mesh only), else a name: a worn piece whose asset
// name is the word, else one whose asset name holds it (any case); the best match wins (ftcore::look::UnderChoice), and
// PrepareFronts warns about two layers of one under piece whose words overlap. One entry per under piece, over piece and
// front state: OverWord names the over piece's own mesh (its open or its closed half), so each front wears its own cut.
// Paths may leave off the asset suffix (/Game/A/SK_Tee is /Game/A/SK_Tee.SK_Tee). A Cut that isn't made yet (no
// package) never applies, so a look can name a cut before it is imported. Example (Config/UserGame.ini, inside a look):
//   UnderLayers=((Under=/Game/Characters/Lead/Outfit/SK_Tee,OverWord="/Game/Characters/Lead/Outfit/SK_JacketOpen",
//                 Cut=/Game/Characters/Lead/Outfit/SK_Tee_uJO))
USTRUCT()
struct FFTUnderLayer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath Under;
	UPROPERTY(EditAnywhere, Category = "Look")
	FString OverWord;
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath Cut;
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
	// The look's under layers, both meshes loaded: one entry per FFTLeadLook::UnderLayers entry, null where not made.
	UPROPERTY()
	TArray<TObjectPtr<USkeletalMesh>> Under;
	UPROPERTY()
	TArray<TObjectPtr<USkeletalMesh>> Cut;
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
	// Under pieces cut where an over piece always hides them (FFTUnderLayer); PiecePaths lists the whole under piece.
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FFTUnderLayer> UnderLayers;
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FFTGroomLook> Grooms;
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FFTMaterialSwap> MaterialSwaps;
	// Morph targets on the look's pieces turned full on while the gauntlet pieces are worn (FFTLeadWeapons), e.g. a
	// sleeve's cuff tucked under the bracer; off otherwise. Set on the body, so every piece carrying a morph of that name
	// takes it by leader pose; a name no worn mesh has does nothing.
	UPROPERTY(EditAnywhere, Category = "Look")
	TArray<FString> GauntletMorphs;
	// The gauntlet bracers this look wears (right, left): the variant fitted over its sleeve (a tucked cuff) where the
	// look's jacket has GauntletMorphs; empty is the kit's own bracer, fitted to bare skin (FTWeaponProps::GauntletPieces).
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath GauntletBracerR;
	UPROPERTY(EditAnywhere, Category = "Look")
	FSoftObjectPath GauntletBracerL;
	// The character model the look is (an id from Data/Assets/models.json). The story director puts the look on
	// screen for a scene whose cast speaker is cast as this model (Data/Voice/speakers.json). Empty: never cast.
	UPROPERTY(EditAnywhere, Category = "Look")
	FString ModelId;
};

// What the wet switch (FTLook::ShowWet) last wrote on one actor: its dynamic instances, their dry values and the shown
// amounts, so only what moved is written. One per wet-capable actor (the lead keeps its own fields; followers hold this).
USTRUCT()
struct FFTWetState
{
	GENERATED_BODY()

	// A dynamic instance on every slot of the actor's skeletal meshes (body, face, clothes), and the MetaHuman skin's
	// dry roughness and specular multipliers per instance (-1 where the material has none).
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
	TArray<FVector2f> SkinDry;
	// Hair without a wet groom (brows, lashes, a beard): an instance per slot and its dry roughness, hair roughness
	// and melanin (-1 where missing).
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> HairMaterials;
	TArray<FVector3f> HairDry;
	uint32 Serial = 0; // FTLook::WornMeshSerial when Materials were gathered
	bool bMade = false;
	bool bHairMade = false;
	bool bGroomWet = false;
	float Shown = -1.f;
	float LineShown = -1.0e9f;
	float SiltShown = -1.f;
	float SkinEased = 0.f;
	bool bSkinSnap = false; // the next show puts the skin at its target, not eased (a member's soak given back)
	float SkinShown = -1.f;
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

// Loads both halves of each of the look's garment pairs, and both meshes of each under layer, once per look setup. Whether an open mesh exists is decided
// here: its package is looked up (in a cooked build, a package that wasn't cooked doesn't exist, so an open mesh not
// referenced by the cook stays closed) and loaded. Front asks whose word names no pair are warned about.
FORGOTTENTHIRD_API FFTWornFronts PrepareFronts(const FFTLeadLook& Look, const TCHAR* Who);

// The look's pieces as worn: each garment pair's half for its state (its default, then the scene's asks, then
// -LookFront's), the closed half when the open mesh isn't made, whichever half PiecePaths lists; then each under piece's
// cut while its over piece is worn (pieces left off by the command line are not worn). Without Fronts or UnderLayers
// this is PiecePaths.
FORGOTTENTHIRD_API TArray<FSoftObjectPath> WornPiecePaths(const FFTLeadLook& Look, const FFTWornFronts& Fronts);

// The scene's front asks ("open", "closed" or word:state items joined by +), used by every look set up after this
// call; empty clears them. False (and nothing changed) when the text doesn't parse.
FORGOTTENTHIRD_API bool SetSceneFronts(const FString& Asks);

// Switches the worn pieces of the look's pairs that `Word` names (any case, in the asset name; every pair when empty)
// to `bOpen`, using the meshes PrepareFronts held. Each such piece component gets the other fitted mesh: the new mesh's
// own slots show with the look's swaps (held in Fronts) applied again on them, quietly where its slots show none of
// their Froms; then what the old slots showed is carried to the new slots of the same name (else the same index when the
// counts match): a dynamic instance on the slot's new material is kept (so its owner's pointer stays good), one on
// another material is made again on the new one with its parameters copied (wetness, dirt), another override is kept
// where both meshes have the same own material; morph weights are kept where the new mesh has the morph, and custom
// depth and stencil are the component's own. The leader pose map is rebuilt on `Body` and the cloth actors are made
// again, and WornMeshSerial moves on. Then the under layers are worn again (WearUnderLayers). A word naming no pair is
// warned about. Returns how many pieces changed.
FORGOTTENTHIRD_API int32 SetFront(const FFTLeadLook& Look, const FFTWornFronts& Fronts, USkeletalMeshComponent& Body,
	const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces, const FString& Word, bool bOpen, const TCHAR* Who);

// Moves on every time a worn piece gets another mesh (SetFront, WearUnderLayers), so an owner holding dynamic
// instances of its pieces' slots gathers them again when it differs from the one it saw.
FORGOTTENTHIRD_API uint32 WornMeshSerial();

// Moves WornMeshSerial on from outside a swap: an owner that put another material on a worn slot (the flesh contact's
// copy, FTFleshContact.h) calls it so the wet switch gathers that slot's dynamic instance again.
FORGOTTENTHIRD_API void BumpWornMeshSerial();

// Each under piece among `Pieces` gets the copy the over pieces now worn call for (its cut while its over piece is
// worn, else the whole piece), as SetFront switches a piece. Worn pieces are the visible ones with a mesh; the command
// line's left-off rule holds (ftcore::look::UnderChoiceLeftOff). Returns how many pieces changed.
FORGOTTENTHIRD_API int32 WearUnderLayers(const FFTLeadLook& Look, const FFTWornFronts& Fronts, USkeletalMeshComponent& Body,
	const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces, const TCHAR* Who);

// Takes a worn piece off (hidden, with its shadow) or puts it back on, then wears the under layers again, so an under
// piece goes back to whole when its over piece comes off and to its cut when it goes on. This is the one path for
// taking a piece off or putting it on at run time. Returns how many under pieces changed.
FORGOTTENTHIRD_API int32 SetPieceWorn(const FFTLeadLook& Look, const FFTWornFronts& Fronts, USkeletalMeshComponent& Body,
	const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces, USkeletalMeshComponent& Piece, bool bWorn, const TCHAR* Who);

// Adds a component per piece mesh, attached to the body and posed by it (leader pose). Missing meshes are skipped
// with a warning that names `Who`. Returns how many of the added pieces simulate cloth. With -CapeStatic (off by
// default) a cloth piece whose asset name holds "Cape" is worn skinned, with no cloth actors, and isn't counted.
FORGOTTENTHIRD_API int32 AddPieces(AActor& Owner, USkeletalMeshComponent& Body, const TArray<FSoftObjectPath>& Paths,
	TArray<TObjectPtr<USkeletalMeshComponent>>& OutPieces, const TCHAR* Who);

// Adds a groom component per entry, bound to the body or to the piece whose mesh is the binding's target (the body
// when no piece is). Returns how many were added.
FORGOTTENTHIRD_API int32 AddGrooms(AActor& Owner, USkeletalMeshComponent& Body, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces,
	const TArray<FFTGroomLook>& Grooms, TArray<TObjectPtr<USceneComponent>>& OutGrooms, const TCHAR* Who);

// Loads every entry's WetGroom and WetBinding into OutHold (kept referenced there), so the swap does not load in play.
// Returns how many entries have a wet groom.
FORGOTTENTHIRD_API int32 PreloadWetGrooms(const TArray<FFTGroomLook>& Grooms, TArray<TObjectPtr<UObject>>& OutHold, const TCHAR* Who);

// Starts loading, off the game thread, every asset the looks can wear (bodies, pieces, both fronts, under layers and cuts,
// grooms and bindings dry and wet, material swaps, the idle and walk clips, the bracers), so a lead switch finds them in
// memory. The handle keeps them referenced while it lives; null when nothing exists to load. Logs the count when loaded.
FORGOTTENTHIRD_API TSharedPtr<FStreamableHandle> HoldLooks(const TArray<FFTLeadLook>& Looks, const TCHAR* Who);

// One "Level start:" line: seconds since the process started, and the process's physical and virtual memory now and at
// its peak (MB). For the party hold's cost (the 11-19 ruling, item 3): called once at each stage, never per frame; its
// body is compiled out of Shipping.
FORGOTTENTHIRD_API void LogLevelStart(const TCHAR* Stage);

// Wears each entry's wet groom (bWet) or its dry one on the component showing the other: the component is unregistered,
// given the groom and its binding, and registered again where it was. Entries without a WetGroom are left alone.
// Returns how many components changed.
FORGOTTENTHIRD_API int32 SetGroomsWet(const TArray<TObjectPtr<USceneComponent>>& Components, const TArray<FFTGroomLook>& Grooms,
	bool bWet, const TCHAR* Who);

// A dynamic material instance on every slot of each groom component whose look entry has no WetGroom (a beard, brows,
// lashes: the wet switch darkens and smooths those through their material instead). Returns how many were added to Out.
FORGOTTENTHIRD_API int32 MakeHairWetMaterials(const TArray<TObjectPtr<USceneComponent>>& Components, const TArray<FFTGroomLook>& Grooms,
	TArray<TObjectPtr<UMaterialInstanceDynamic>>& Out);

// The wet switch for any dressed actor (a follower; the lead runs the same rules in AFTExploreCharacter): Cloth is the
// soak (ftcore::leadwet::cloth) written as Wetness, WetLineZ (the line over FeetZ, in cm) and WetSilt on every skeletal
// mesh slot of Owner; Surface (ftcore::leadwet::surface) swaps wet grooms (ftcore::leadwet::groomWet), and its eased skin
// amount goes to SkinWet, the MetaHuman skin multipliers and the hair without a wet groom. Instances are made the first
// time it is wet and again after a worn mesh swap. Dry and never wet costs two compares. Returns how many material
// instances were written this call.
FORGOTTENTHIRD_API int32 ShowWet(AActor& Owner, FFTWetState& State, const ftcore::WaterWetness& Cloth, float Surface, float FeetZ,
	const TArray<TObjectPtr<USceneComponent>>& Grooms, const TArray<FFTGroomLook>& GroomLooks, float DeltaSeconds, const TCHAR* Who);

// Puts To in every slot of the component that shows From (From itself or an instance made from it), quietly. Returns
// how many slots changed.
FORGOTTENTHIRD_API int32 SwapSlots(USkeletalMeshComponent& Component, const UMaterialInterface* From, UMaterialInterface* To);

// Puts each swap's To in every slot of the body and the pieces that shows its From. A swap that doesn't load or whose
// From no slot shows is warned about, naming `Who`. Returns how many slots changed.
FORGOTTENTHIRD_API int32 SwapMaterials(USkeletalMeshComponent& Body, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces,
	const TArray<FFTMaterialSwap>& Swaps, const TCHAR* Who);
}
