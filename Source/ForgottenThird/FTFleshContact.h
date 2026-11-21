#pragma once

// Flesh contact on a lead: named presser/receiver capsule pairs on the body's bones go through ftcore::flesh::solve each
// tick, and the deepest contacts go to the skin copy's dent parameters (FTBuildFleshCommandlet.h). The body's skin slots
// are moved onto the copies once, at registration (an instance already on a slot is carried over), so the one dynamic
// instance per slot is shared with the wet switch: CreateAndSetMaterialInstanceDynamic hands both the same one, and
// FTLook::BumpWornMeshSerial makes the wet switch gather it again. A worn mesh swap registers again.
//
// Parameters are written only when they change, through the instance's parameter indices; the tick allocates nothing.
// A slot whose copy is missing is left as it was, with a warning; with no copy at all the component does nothing.
//
// Non-shipping proof (-ExploreFlesh=<shot prefix>, FTExploreCharacter): the right palm is placed on the left forearm by
// the anim instance's hand place, at a stated overlap (-ExploreFleshOverlap=, cm, 0.6 by default), settled, latched,
// and the stills taken at 1 m: _a1 and _a2 (the original skin, twice, for the noise floor), _b (the copy, no contact),
// _c (contact), _d (contact, the presser arm hidden) and _d0 (no contact, arm hidden). An orthographic depth capture
// along the contact normal reads the dent's centre depth (no contact against contact, arm hidden), checked against a
// 1 cm move of the capture. The tick's heap allocations are counted over 300 latched frames.

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "ftcore/FleshContact.h"

#include "FTFleshContact.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class USkeletalMeshComponent;
class UTextureRenderTarget2D;
class USceneCaptureComponent2D;
class ACameraActor;

namespace FTFlesh
{
// Where the commandlet writes the copies (and where the component looks), unless -Out= / -FleshFolder= say otherwise.
inline const TCHAR* const kDefaultFolder = TEXT("/Game/Local/FleshContact");
inline const TCHAR* const kCopySuffix = TEXT("_Flesh");
// The skin master the copies are made from.
inline const TCHAR* const kDefaultMaster = TEXT("M_skin_unified_baked");

// Parameter names on the copy master, per contact.
FName PosName(int32 Contact);
FName NrmName(int32 Contact);
FName RotName(int32 Contact);
inline const TCHAR* const kBulgeName = TEXT("FleshBulge");
inline const TCHAR* const kBulgeWidthName = TEXT("FleshBulgeWidth");

// <Folder>/<mesh short name>/<Name>_Flesh, as an object path.
FString CopyPath(const FString& Folder, const FString& MeshName, const FString& Name);
} // namespace FTFlesh

// One named pair: a presser capsule on one bone pressed into a receiver capsule on another. Each capsule runs from its
// bone's head toward the child bone (or along the bone's own axis, Length cm), with the radius given.
struct FFTFleshPair
{
	FName Name;
	FName PresserBone;
	FName PresserTip; // the capsule's other end; None: the bone's X axis, PresserLength cm
	float PresserLength = 0.f;
	float PresserRadius = 0.f;
	float PresserInset = 0.f; // the presser's axis moved this far along -PresserSide, so its surface is the palm's face
	FVector PresserSide = FVector::ZeroVector; // out of the palm, in the presser bone's frame
	FName ReceiverBone;
	FName ReceiverTip;
	float ReceiverRadius = 0.f;
	float Firmness = 0.f;
};

UCLASS()
class FORGOTTENTHIRD_API UFTFleshContact : public UActorComponent
{
	GENERATED_BODY()

public:
	UFTFleshContact();

	// The body mesh (its skin slots are moved onto the copies) and the pairs; registration happens on the next tick and
	// again whenever FTLook::WornMeshSerial moves on.
	void Setup(USkeletalMeshComponent* Body, TArrayView<const FFTFleshPair> Pairs, const FString& Folder);

	// Freezes the shown contacts (the proof latches them once settled). While latched nothing is solved or written.
	void SetLatched(bool bLatch) { bLatched = bLatch; }
	// Forces every contact to depth 0 (exactly the original skin) while on; restores the solved ones when off.
	void SetSuppressed(bool bSuppress);

	const ftcore::flesh::Contacts& Solved() const { return Last; }
	// The pair capsules last solved, world space.
	const ftcore::flesh::Capsule& PresserCapsule(int32 Pair) const { return Pressers[Pair]; }
	const ftcore::flesh::Receiver& ReceiverCapsule(int32 Pair) const { return Receivers[Pair]; }
	int32 SlotsOnCopy() const { return Slots.Num(); }
	bool IsRegistered() const { return bRegistered; }
	// The contact as the material gets it, ref-pose space (for the proof's log).
	FVector4f ShownPos(int32 Contact) const { return Shown[Contact].Pos; }
	FVector4f ShownNrm(int32 Contact) const { return Shown[Contact].Nrm; }
	// Moves this component's own work forward one step (the tick calls it; the proof calls it under the allocation count).
	void Step();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

#if !UE_BUILD_SHIPPING
	// The proof driver (see the top of this file); Prefix is the shot path prefix.
	void StartProof(const FString& Prefix, float OverlapCm);
#endif

private:
	static constexpr int32 kMaxPairs = 8;
	static constexpr int32 K = ftcore::flesh::kMaxContacts;

	struct FShown
	{
		FVector4f Pos = FVector4f(0.f, 0.f, 0.f, 0.f);
		FVector4f Nrm = FVector4f(0.f, 0.f, 1.f, 0.f);
		FVector4f Rot = FVector4f(0.f, 0.f, 0.f, 1.f);
	};
	struct FSlot
	{
		int32 Index = INDEX_NONE; // the body's material slot
		TObjectPtr<UMaterialInstanceDynamic> Dynamic;
		int32 Pos[K];
		int32 Nrm[K];
		int32 Rot[K];
	};

	bool Register();
	void Write(const FShown (&Want)[K]);

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> Body;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> Hold; // the copies loaded, kept from garbage collection
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Dynamics; // Slots' instances, for the garbage collector

	FString Folder;
	FFTFleshPair PairSpecs[kMaxPairs];
	int32 PairCount = 0;
	// Per pair: bone indices and the receiver bone's ref-pose component transform.
	int32 PresserIndex[kMaxPairs];
	int32 PresserTipIndex[kMaxPairs];
	int32 ReceiverIndex[kMaxPairs];
	int32 ReceiverTipIndex[kMaxPairs];
	FTransform ReceiverRef[kMaxPairs];
	ftcore::flesh::Capsule Pressers[kMaxPairs];
	ftcore::flesh::Receiver Receivers[kMaxPairs];

	TArray<FSlot> Slots;
	FShown Shown[K];
	FShown Solved_[K];
	ftcore::flesh::Contacts Last;
	uint32 SerialSeen = 0;
	bool bRegistered = false;
	bool bTried = false;
	bool bLatched = false;
	bool bSuppressed = false;
	int32 WroteLast = 0; // parameters written by the last Write

	// The proof's objects (non-shipping only; reflected members can't sit under a shipping guard).
	UPROPERTY(Transient)
	TObjectPtr<ACameraActor> ProofCamera;
	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> ProofCapture;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ProofTarget;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> ProofOriginals; // per slot, the material before the copy
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> ProofCopies; // per slot, the copy (the slot's instance's parent)

#if !UE_BUILD_SHIPPING
	void TickProof();
	void ProofDepth(const TArray<float>& Calib);
	bool ProofDepthRead(TArray<float>& Out, float Back);
	void ProofFinish(int32 Code);
	void ProofShot(const TCHAR* Tag);
	void ProofHide(bool bHide);
	void ProofSetOriginal(bool bOriginal);
	void ProofLogHandBox(const TCHAR* Tag);
	void ProofPlace();

	FString ProofPrefix;
	float ProofOverlap = 0.6f;
	int32 ProofStage = -1;
	int32 ProofFrames = 0;
	double ProofStageAt = 0.0;
	float ProofCorrection = 0.f;
	int32 ProofSettleCount = 0;
	bool bProofAllocs = false;
	int32 ProofAllocFrames = 0;
	int32 ProofAllocWriteFrames = 0;
	uint64 ProofAllocSolve = 0; // allocations inside the solve (Step before Write)
	uint64 ProofAllocWrite = 0; // allocations inside Write (the engine's parameter update)
	FVector ProofHandCentre = FVector::ZeroVector;  // presser capsule centre, hand bone space
	FVector ProofPalmLocal = FVector::ZeroVector;   // palm normal, hand bone space
	FVector ProofFingerLocal = FVector::ZeroVector; // the fingers' way, hand bone space
	FVector ProofPoint = FVector::ZeroVector;       // latched contact point, world
	FVector ProofNormal = FVector::UnitZ();         // latched contact normal, world
	float ProofPen = 0.f;
	TArray<float> ProofDepthNone;
	TArray<float> ProofDepthContact;
#endif
};
