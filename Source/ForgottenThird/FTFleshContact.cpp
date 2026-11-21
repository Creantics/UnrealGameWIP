#include "FTFleshContact.h"

#include "ForgottenThird.h"
#include "FTLocomotionAnimInstance.h"
#include "FTLook.h"

#include "AnimationRuntime.h"
#include "HAL/IConsoleManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

#if !UE_BUILD_SHIPPING
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#endif

FName FTFlesh::PosName(int32 Contact) { return FName(*FString::Printf(TEXT("FleshC%d_Pos"), Contact)); }
FName FTFlesh::NrmName(int32 Contact) { return FName(*FString::Printf(TEXT("FleshC%d_Nrm"), Contact)); }
FName FTFlesh::RotName(int32 Contact) { return FName(*FString::Printf(TEXT("FleshC%d_Rot"), Contact)); }

FString FTFlesh::CopyPath(const FString& InFolder, const FString& MeshName, const FString& Name)
{
	const FString Leaf = Name + kCopySuffix;
	return FString::Printf(TEXT("%s/%s/%s.%s"), *InFolder, *MeshName, *Leaf, *Leaf);
}

namespace
{
namespace fl = ftcore::flesh;

constexpr float kDefaultPresserRadius = 2.f;  // half a palm's thickness
constexpr float kDefaultReceiverRadius = 3.5f; // a forearm's

fl::V3 ToV3(const FVector& V) { return {static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z)}; }
FVector ToVec(const fl::V3& V) { return FVector(V.x, V.y, V.z); }
FLinearColor ToColour(const FVector4f& V) { return FLinearColor(V.X, V.Y, V.Z, V.W); }

// The radius of the physics body on Bone (a capsule's, a sphere's, half a box's thinnest side); 0 with none.
float PhysicsRadius(const USkeletalMeshComponent& Mesh, FName Bone, const TCHAR*& OutKind)
{
	OutKind = TEXT("default");
	const UPhysicsAsset* Phys = Mesh.GetPhysicsAsset();
	const int32 I = Phys ? Phys->FindBodyIndex(Bone) : INDEX_NONE;
	if (!Phys || !Phys->SkeletalBodySetups.IsValidIndex(I) || !Phys->SkeletalBodySetups[I])
	{
		return 0.f;
	}
	const FKAggregateGeom& Geom = Phys->SkeletalBodySetups[I]->AggGeom;
	if (Geom.SphylElems.Num() > 0)
	{
		OutKind = TEXT("capsule");
		return Geom.SphylElems[0].Radius;
	}
	if (Geom.SphereElems.Num() > 0)
	{
		OutKind = TEXT("sphere");
		return Geom.SphereElems[0].Radius;
	}
	if (Geom.BoxElems.Num() > 0)
	{
		OutKind = TEXT("box");
		const FKBoxElem& Box = Geom.BoxElems[0];
		return 0.5f * FMath::Min3(Box.X, Box.Y, Box.Z);
	}
	return 0.f;
}

// The slot's material under any dynamic instance on it.
UMaterialInterface* UnderDynamic(UMaterialInterface* Material)
{
	while (UMaterialInstanceDynamic* Dyn = Cast<UMaterialInstanceDynamic>(Material))
	{
		Material = Dyn->Parent;
	}
	return Material;
}

void Put(UMaterialInstanceDynamic& Dyn, int32& Index, FName (*NameOf)(int32), int32 Contact, const FVector4f& V)
{
	const FLinearColor Value = ToColour(V);
	if (!Dyn.SetVectorParameterByIndex(Index, Value))
	{
		Dyn.InitializeVectorParameterAndGetIndex(NameOf(Contact), Value, Index); // the instance's parameters moved
	}
}

#if !UE_BUILD_SHIPPING
// Proof only (-ExploreFleshAllocs): the global allocator wrapped, every allocation counted on the thread that set the
// mode (1 the solve, 2 the parameter writes). Everything else is passed straight on. Installed once and never taken out.
thread_local int32 GCountMode = 0;
uint64 GCounted[3] = {0, 0, 0};

class FFleshCountingMalloc final : public FMalloc
{
public:
	explicit FFleshCountingMalloc(FMalloc* InInner) : Inner(InInner) {}

	virtual void* Malloc(SIZE_T Count, uint32 Alignment = DEFAULT_ALIGNMENT) override { Note(); return Inner->Malloc(Count, Alignment); }
	virtual void* TryMalloc(SIZE_T Count, uint32 Alignment = DEFAULT_ALIGNMENT) override { Note(); return Inner->TryMalloc(Count, Alignment); }
	virtual void* Realloc(void* Original, SIZE_T Count, uint32 Alignment = DEFAULT_ALIGNMENT) override
	{
		if (Count > 0) { Note(); }
		return Inner->Realloc(Original, Count, Alignment);
	}
	virtual void* TryRealloc(void* Original, SIZE_T Count, uint32 Alignment = DEFAULT_ALIGNMENT) override
	{
		if (Count > 0) { Note(); }
		return Inner->TryRealloc(Original, Count, Alignment);
	}
	virtual void Free(void* Original) override { Inner->Free(Original); }
	virtual void* MallocZeroed(SIZE_T Count, uint32 Alignment = DEFAULT_ALIGNMENT) override { Note(); return Inner->MallocZeroed(Count, Alignment); }
	virtual void* TryMallocZeroed(SIZE_T Count, uint32 Alignment = DEFAULT_ALIGNMENT) override { Note(); return Inner->TryMallocZeroed(Count, Alignment); }
	virtual SIZE_T QuantizeSize(SIZE_T Count, uint32 Alignment) override { return Inner->QuantizeSize(Count, Alignment); }
	virtual bool GetAllocationSize(void* Original, SIZE_T& SizeOut) override { return Inner->GetAllocationSize(Original, SizeOut); }
	virtual void Trim(bool bTrimThreadCaches) override { Inner->Trim(bTrimThreadCaches); }
	virtual void SetupTLSCachesOnCurrentThread() override { Inner->SetupTLSCachesOnCurrentThread(); }
	virtual void MarkTLSCachesAsUsedOnCurrentThread() override { Inner->MarkTLSCachesAsUsedOnCurrentThread(); }
	virtual void MarkTLSCachesAsUnusedOnCurrentThread() override { Inner->MarkTLSCachesAsUnusedOnCurrentThread(); }
	virtual void ClearAndDisableTLSCachesOnCurrentThread() override { Inner->ClearAndDisableTLSCachesOnCurrentThread(); }
	virtual void UpdateStats() override { Inner->UpdateStats(); }
	virtual void GetAllocatorStats(FGenericMemoryStats& OutStats) override { Inner->GetAllocatorStats(OutStats); }
	virtual void DumpAllocatorStats(FOutputDevice& Ar) override { Inner->DumpAllocatorStats(Ar); }
	virtual bool IsInternallyThreadSafe() const override { return Inner->IsInternallyThreadSafe(); }
	virtual bool ValidateHeap() override { return Inner->ValidateHeap(); }
	virtual const TCHAR* GetDescriptiveName() override { return Inner->GetDescriptiveName(); }
	virtual void OnPreFork() override { Inner->OnPreFork(); }
	virtual void OnPostFork() override { Inner->OnPostFork(); }
	virtual uint64 GetImmediatelyFreeableCachedMemorySize() const override { return Inner->GetImmediatelyFreeableCachedMemorySize(); }
	virtual uint64 GetTotalFreeCachedMemorySize() const override { return Inner->GetTotalFreeCachedMemorySize(); }

private:
	static void Note()
	{
		if (GCountMode > 0 && GCountMode < 3)
		{
			++GCounted[GCountMode];
		}
	}
	FMalloc* Inner;
};

FFleshCountingMalloc* GCounting = nullptr;

constexpr int32 kDepthSize = 128;    // the depth capture, pixels a side
constexpr float kDepthWidth = 12.f;  // cm a side
constexpr float kDepthStandOff = 30.f; // cm from the contact point to the capture
constexpr int32 kSettleFrames = 10;
constexpr int32 kPlaceTimeout = 900;
constexpr int32 kAllocFrames = 300;
constexpr int32 kShotAt = 30;  // frames after a change before its still (the temporal history settles)
constexpr int32 kShotDone = 50; // frames after a change before the next one

struct FProofStill
{
	const TCHAR* Tag;
	bool bOriginal;
	bool bSuppressed;
	bool bHidden;
	int32 Depth; // 0 none, 1 contact read, 2 no contact read (and the 1 cm calibration)
};
const FProofStill kStills[] = {
	{TEXT("a1"), true, false, false, 0},
	{TEXT("a2"), true, false, false, 0},
	{TEXT("b"), false, true, false, 0},
	{TEXT("c"), false, false, false, 0},
	{TEXT("d"), false, false, true, 1},
	{TEXT("d0"), false, true, true, 2},
};
#endif
} // namespace

UFTFleshContact::UFTFleshContact()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork; // the body's bones are final for the frame
	for (int32 I = 0; I < kMaxPairs; ++I)
	{
		PresserIndex[I] = PresserTipIndex[I] = ReceiverIndex[I] = ReceiverTipIndex[I] = INDEX_NONE;
	}
}

void UFTFleshContact::Setup(USkeletalMeshComponent* InBody, TArrayView<const FFTFleshPair> Pairs, const FString& InFolder)
{
	Body = InBody;
	Folder = InFolder.IsEmpty() ? FString(FTFlesh::kDefaultFolder) : InFolder;
	if (Pairs.Num() > kMaxPairs)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTFleshContact: %d pairs given, the first %d kept."), Pairs.Num(), kMaxPairs);
	}
	PairCount = FMath::Min(Pairs.Num(), kMaxPairs);
	for (int32 I = 0; I < PairCount; ++I)
	{
		PairSpecs[I] = Pairs[I];
	}
	bRegistered = false;
	bTried = false;
	if (Body)
	{
		AddTickPrerequisiteComponent(Body);
	}
}

bool UFTFleshContact::Register()
{
	Slots.Reset();
	Dynamics.Reset();
	Hold.Reset();
	ProofOriginals.Reset();
	ProofCopies.Reset();
	bRegistered = false;
	SerialSeen = FTLook::WornMeshSerial();
	const FString Master = FTFlesh::kDefaultMaster;
	const FString MasterCopy = Master + FTFlesh::kCopySuffix;

	if (!Body && PairCount > 0 && GetOwner())
	{
		// No body given: the owner's first skeletal mesh with the first pair's receiver bone and a skin slot.
		TInlineComponentArray<USkeletalMeshComponent*> Meshes(GetOwner());
		for (USkeletalMeshComponent* Mesh : Meshes)
		{
			if (!Mesh || Mesh->GetBoneIndex(PairSpecs[0].ReceiverBone) == INDEX_NONE)
			{
				continue;
			}
			for (int32 S = 0; S < Mesh->GetNumMaterials() && !Body; ++S)
			{
				const UMaterialInterface* Under = UnderDynamic(Mesh->GetMaterial(S));
				const UMaterial* Base = Under ? Under->GetMaterial() : nullptr;
				if (Base && (Base->GetName() == Master || Base->GetName() == MasterCopy))
				{
					Body = Mesh;
				}
			}
			if (Body)
			{
				break;
			}
		}
	}
	USkeletalMesh* MeshAsset = Body ? Body->GetSkeletalMeshAsset() : nullptr;
	if (!MeshAsset || PairCount == 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTFleshContact: no body mesh (%s) or no pairs (%d); nothing to do."),
			Body ? *Body->GetName() : TEXT("none"), PairCount);
		return false;
	}
	const FReferenceSkeleton& RefSkel = MeshAsset->GetRefSkeleton();
	if (!Body->GetComponentScale().Equals(FVector::OneVector, 1e-3))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTFleshContact: %s is scaled %s; the dent's ref-pose space is taken as world size."),
			*Body->GetName(), *Body->GetComponentScale().ToString());
	}

	for (int32 I = 0; I < PairCount; ++I)
	{
		FFTFleshPair& Pair = PairSpecs[I];
		PresserIndex[I] = Body->GetBoneIndex(Pair.PresserBone);
		PresserTipIndex[I] = Pair.PresserTip.IsNone() ? INDEX_NONE : Body->GetBoneIndex(Pair.PresserTip);
		ReceiverIndex[I] = Body->GetBoneIndex(Pair.ReceiverBone);
		ReceiverTipIndex[I] = Pair.ReceiverTip.IsNone() ? INDEX_NONE : Body->GetBoneIndex(Pair.ReceiverTip);
		if (PresserIndex[I] == INDEX_NONE || ReceiverIndex[I] == INDEX_NONE
			|| (!Pair.PresserTip.IsNone() && PresserTipIndex[I] == INDEX_NONE)
			|| (!Pair.ReceiverTip.IsNone() && ReceiverTipIndex[I] == INDEX_NONE))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTFleshContact: pair %s: a bone is missing on %s (%s %d, %s %d, %s %d, %s %d)."),
				*Pair.Name.ToString(), *MeshAsset->GetName(), *Pair.PresserBone.ToString(), PresserIndex[I],
				*Pair.PresserTip.ToString(), PresserTipIndex[I], *Pair.ReceiverBone.ToString(), ReceiverIndex[I],
				*Pair.ReceiverTip.ToString(), ReceiverTipIndex[I]);
			return false;
		}
		const TCHAR* PresserKind = TEXT("given");
		const TCHAR* ReceiverKind = TEXT("given");
		if (!(Pair.PresserRadius > 0.f))
		{
			Pair.PresserRadius = PhysicsRadius(*Body, Pair.PresserBone, PresserKind);
			if (!(Pair.PresserRadius > 0.f))
			{
				Pair.PresserRadius = kDefaultPresserRadius;
			}
		}
		if (!(Pair.ReceiverRadius > 0.f))
		{
			Pair.ReceiverRadius = PhysicsRadius(*Body, Pair.ReceiverBone, ReceiverKind);
			if (!(Pair.ReceiverRadius > 0.f))
			{
				Pair.ReceiverRadius = kDefaultReceiverRadius;
			}
		}
		ReceiverRef[I] = FAnimationRuntime::GetComponentSpaceTransformRefPose(RefSkel, ReceiverIndex[I]);
		UE_LOG(LogForgottenThird, Display,
			TEXT("FTFleshContact: pair %s: presser %s->%s r %.2f cm (%s) inset %.2f, receiver %s->%s r %.2f cm (%s) firmness %.2f, receiver ref %s %s."),
			*Pair.Name.ToString(), *Pair.PresserBone.ToString(), *Pair.PresserTip.ToString(), Pair.PresserRadius, PresserKind,
			Pair.PresserInset, *Pair.ReceiverBone.ToString(), *Pair.ReceiverTip.ToString(), Pair.ReceiverRadius, ReceiverKind,
			Pair.Firmness, *ReceiverRef[I].GetLocation().ToString(), *ReceiverRef[I].GetRotation().Rotator().ToString());
	}

	bool bChanged = false;
	const FString MeshName = MeshAsset->GetName();
	const FShown Default;
	for (int32 S = 0; S < Body->GetNumMaterials(); ++S)
	{
		UMaterialInterface* Current = Body->GetMaterial(S);
		UMaterialInterface* Under = UnderDynamic(Current);
		const UMaterial* Base = Under ? Under->GetMaterial() : nullptr;
		if (!Base)
		{
			continue;
		}
		UMaterialInterface* Copy = nullptr;
		UMaterialInterface* Original = Current;
		if (Base->GetName() == MasterCopy)
		{
			Copy = Under; // registered before (a worn mesh swap): already on the copy
			Original = nullptr;
		}
		else if (Base->GetName() == Master)
		{
			const FString Path = FTFlesh::CopyPath(Folder, MeshName, Under->GetName());
			Copy = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Copy)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("FTFleshContact: slot %d (%s): no copy at %s; left as it is (run FTBuildFlesh)."),
					S, *Under->GetName(), *Path);
				continue;
			}
		}
		else
		{
			continue;
		}

		UMaterialInstanceDynamic* Dyn = Cast<UMaterialInstanceDynamic>(Current);
		if (Dyn && Dyn->Parent != Copy)
		{
			// An instance already on the slot (the wet switch's): a new one on the copy carries its values over.
			UMaterialInstanceDynamic* Fresh = UMaterialInstanceDynamic::Create(Copy, Body);
			Fresh->CopyParameterOverrides(Dyn);
			Body->SetMaterial(S, Fresh);
			bChanged = true;
		}
		else if (!Dyn && Current != Copy)
		{
			Body->SetMaterial(S, Copy);
			bChanged = true;
		}
		Dyn = Body->CreateAndSetMaterialInstanceDynamic(S);
		if (!Dyn)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTFleshContact: slot %d: no dynamic instance."), S);
			continue;
		}
		bChanged = bChanged || Dyn != Current;

		FSlot Slot;
		Slot.Index = S;
		Slot.Dynamic = Dyn;
		bool bOk = true;
		for (int32 C = 0; C < K && bOk; ++C)
		{
			const FName Names[3] = {FTFlesh::PosName(C), FTFlesh::NrmName(C), FTFlesh::RotName(C)};
			const FVector4f Values[3] = {Default.Pos, Default.Nrm, Default.Rot};
			int32* Indices[3] = {&Slot.Pos[C], &Slot.Nrm[C], &Slot.Rot[C]};
			for (int32 J = 0; J < 3 && bOk; ++J)
			{
				FLinearColor Have;
				if (!Dyn->GetVectorParameterValue(FHashedMaterialParameterInfo(Names[J]), Have)
					|| !Dyn->InitializeVectorParameterAndGetIndex(Names[J], ToColour(Values[J]), *Indices[J]))
				{
					UE_LOG(LogForgottenThird, Error, TEXT("FTFleshContact: slot %d: the copy %s has no parameter %s (rebuild it with FTBuildFlesh)."),
						S, *Copy->GetPathName(), *Names[J].ToString());
					bOk = false;
				}
			}
		}
		if (!bOk)
		{
			continue;
		}
		Slots.Add(Slot);
		Dynamics.Add(Dyn);
		Hold.Add(Copy);
		ProofOriginals.Add(Original);
		ProofCopies.Add(Copy);
		UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact: slot %d (%s) on the copy %s, instance %s."),
			S, Body->GetMaterialSlotNames().IsValidIndex(S) ? *Body->GetMaterialSlotNames()[S].ToString() : TEXT("?"),
			*Copy->GetPathName(), *Dyn->GetName());
	}
	for (int32 C = 0; C < K; ++C)
	{
		Shown[C] = Default;
		Solved_[C] = Default;
	}
	Last = fl::Contacts{};
	if (bChanged)
	{
		FTLook::BumpWornMeshSerial(); // the wet switch gathers the slots' instances again (now these)
	}
	SerialSeen = FTLook::WornMeshSerial();
	bRegistered = Slots.Num() > 0;
	if (bRegistered)
	{
		// The dent is a world position offset: the skinned mesh must evaluate it (ray-traced geometry and velocity follow
		// the offset only when it does).
		Body->SetEvaluateWorldPositionOffset(true);
		const auto CVarInt = [](const TCHAR* Name) -> int32
		{
			const IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
			return V ? V->GetInt() : -1;
		};
		UE_LOG(LogForgottenThird, Display,
			TEXT("FTFleshContact: WPO evaluation %d; r.Velocity.EnableVertexDeformation %d, r.VelocityOutputPass %d, "
				 "r.RayTracing.Geometry.SkeletalMeshes %d, r.RayTracing.Shadows %d, r.Shadow.Virtual.Enable %d (-1: no such switch)."),
			Body->EvaluateWorldPositionOffset() ? 1 : 0, CVarInt(TEXT("r.Velocity.EnableVertexDeformation")),
			CVarInt(TEXT("r.VelocityOutputPass")), CVarInt(TEXT("r.RayTracing.Geometry.SkeletalMeshes")),
			CVarInt(TEXT("r.RayTracing.Shadows")), CVarInt(TEXT("r.Shadow.Virtual.Enable")));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact: %s: %d slot(s) on the copies from %s, %d pair(s)%s."),
		*MeshName, Slots.Num(), *Folder, PairCount, bChanged ? TEXT(", worn mesh serial moved on") : TEXT(""));
	return bRegistered;
}

void UFTFleshContact::Write(const FShown (&Want)[K])
{
	WroteLast = 0;
	for (int32 C = 0; C < K; ++C)
	{
		const bool bPos = Want[C].Pos != Shown[C].Pos;
		const bool bNrm = Want[C].Nrm != Shown[C].Nrm;
		const bool bRot = Want[C].Rot != Shown[C].Rot;
		if (!bPos && !bNrm && !bRot)
		{
			continue;
		}
		for (FSlot& Slot : Slots)
		{
			UMaterialInstanceDynamic* Dyn = Slot.Dynamic.Get();
			if (!Dyn)
			{
				continue;
			}
			if (bPos) { Put(*Dyn, Slot.Pos[C], &FTFlesh::PosName, C, Want[C].Pos); }
			if (bNrm) { Put(*Dyn, Slot.Nrm[C], &FTFlesh::NrmName, C, Want[C].Nrm); }
			if (bRot) { Put(*Dyn, Slot.Rot[C], &FTFlesh::RotName, C, Want[C].Rot); }
		}
		Shown[C] = Want[C];
		++WroteLast;
	}
}

void UFTFleshContact::SetSuppressed(bool bSuppress)
{
	bSuppressed = bSuppress;
	if (!bRegistered)
	{
		return;
	}
	if (bSuppress)
	{
		const FShown None[K];
		Write(None);
	}
	else
	{
		Write(Solved_);
	}
}

void UFTFleshContact::Step()
{
	if (PairCount == 0)
	{
		return;
	}
	if (FTLook::WornMeshSerial() != SerialSeen)
	{
		// A worn mesh swap (or another owner's slot change): gather again.
		SerialSeen = FTLook::WornMeshSerial();
		bRegistered = false;
		bTried = false;
	}
	if (!bRegistered)
	{
		if (bTried)
		{
			return;
		}
		bTried = true;
		if (!Register())
		{
			return;
		}
	}
	if (bLatched || !Body)
	{
		return;
	}

	// Each named pair touched on its own; the K deepest kept, deepest first, ties in pair order.
	const fl::Params Prm;
	fl::Contacts Found;
	for (int32 I = 0; I < PairCount; ++I)
	{
		const FFTFleshPair& Pair = PairSpecs[I];
		const FTransform PW = Body->GetBoneTransform(PresserIndex[I]);
		const FVector Shift = Pair.PresserInset != 0.f
			? PW.TransformVectorNoScale(-Pair.PresserSide.GetSafeNormal()) * Pair.PresserInset : FVector::ZeroVector;
		const FVector PA = PW.GetLocation() + Shift;
		const FVector PB = (PresserTipIndex[I] != INDEX_NONE ? Body->GetBoneTransform(PresserTipIndex[I]).GetLocation()
			: PW.TransformPosition(FVector(Pair.PresserLength, 0.0, 0.0))) + Shift;
		Pressers[I] = fl::Capsule{ToV3(PA), ToV3(PB), Pair.PresserRadius};
		const FVector RA = Body->GetBoneTransform(ReceiverIndex[I]).GetLocation();
		const FVector RB = ReceiverTipIndex[I] != INDEX_NONE ? Body->GetBoneTransform(ReceiverTipIndex[I]).GetLocation() : RA;
		Receivers[I] = fl::Receiver{fl::Capsule{ToV3(RA), ToV3(RB), Pair.ReceiverRadius}, Pair.Firmness};

		fl::Contact Hit;
		if (!fl::touch(Pressers[I], Receivers[I], Prm, Hit))
		{
			continue;
		}
		Hit.receiver = I;
		Hit.presser = I;
		++Found.touching;
		int32 At = Found.count;
		while (At > 0 && Found.c[At - 1].depth < Hit.depth)
		{
			--At;
		}
		if (At >= K)
		{
			continue;
		}
		for (int32 J = FMath::Min(Found.count, K - 1); J > At; --J)
		{
			Found.c[J] = Found.c[J - 1];
		}
		Found.c[At] = Hit;
		Found.count = FMath::Min(Found.count + 1, K);
	}
	Last = Found;

	// To the receiver bone's ref-pose space (where the material measures the dent, on the pre-skinned position).
	FShown Want[K];
	for (int32 C = 0; C < Found.count; ++C)
	{
		const fl::Contact& Hit = Found.c[C];
		if (!(Hit.depth > 0.f))
		{
			continue;
		}
		const int32 I = Hit.receiver;
		const FTransform RW = Body->GetBoneTransform(ReceiverIndex[I]);
		const FTransform& RR = ReceiverRef[I];
		const FVector PRef = RR.TransformPosition(RW.InverseTransformPosition(ToVec(Hit.point)));
		const FVector NRef = RR.TransformVectorNoScale(RW.InverseTransformVectorNoScale(ToVec(Hit.normal))).GetSafeNormal();
		const FQuat Q = RW.GetRotation() * RR.GetRotation().Inverse();
		Want[C].Pos = FVector4f(FVector3f(PRef), Hit.radius);
		Want[C].Nrm = FVector4f(FVector3f(NRef), Hit.depth);
		Want[C].Rot = FVector4f(static_cast<float>(Q.X), static_cast<float>(Q.Y), static_cast<float>(Q.Z), static_cast<float>(Q.W));
	}
	for (int32 C = 0; C < K; ++C)
	{
		Solved_[C] = Want[C];
	}
	if (bSuppressed)
	{
		return;
	}
#if !UE_BUILD_SHIPPING
	const int32 Mode = GCountMode;
	if (Mode == 1)
	{
		GCountMode = 2;
	}
#endif
	Write(Want);
#if !UE_BUILD_SHIPPING
	GCountMode = Mode;
#endif
}

void UFTFleshContact::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
#if !UE_BUILD_SHIPPING
	if (ProofStage >= 0)
	{
		TickProof();
		return;
	}
#endif
	Step();
}

#if !UE_BUILD_SHIPPING
void UFTFleshContact::StartProof(const FString& Prefix, float OverlapCm)
{
	ProofPrefix = Prefix;
	ProofOverlap = OverlapCm > 0.f ? OverlapCm : 0.6f;
	ProofStage = 0;
	ProofFrames = 0;
	ProofCorrection = 0.f;
	ProofSettleCount = 0;
	bProofAllocs = FParse::Param(FCommandLine::Get(), TEXT("ExploreFleshAllocs"));
	if (bProofAllocs && !GCounting)
	{
		GCounting = new FFleshCountingMalloc(UE::Private::GMalloc);
		UE::Private::GMalloc = GCounting;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: stills %s_*.png, overlap %.2f cm, allocation count %s."),
		*ProofPrefix, ProofOverlap, bProofAllocs ? TEXT("on") : TEXT("off (-ExploreFleshAllocs)"));
}

void UFTFleshContact::ProofFinish(int32 Code)
{
	ProofHide(false);
	ProofSetOriginal(false);
	SetSuppressed(false);
	SetLatched(false);
	ProofStage = -1;
	UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: done (%d)."), Code);
	FTRequestExit(Code, TEXT("ExploreFlesh"));
}

void UFTFleshContact::ProofPlace()
{
	UFTLocomotionAnimInstance* Anim = Body ? Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance()) : nullptr;
	const AActor* Owner = GetOwner();
	if (!Anim || !Owner || PairCount == 0)
	{
		return;
	}
	const FVector Fwd = Owner->GetActorForwardVector();
	const FVector Right = Owner->GetActorRightVector();
	const FVector Up = FVector::UpVector;
	const int32 Spine = Body->GetBoneIndex(TEXT("spine_05"));
	const FVector Chest = Spine != INDEX_NONE ? Body->GetBoneTransform(Spine).GetLocation() : Owner->GetActorLocation() + Up * 50.0;
	// The left forearm out in front of the chest, a little across, the elbow a little bent.
	const int32 HandL = Body->GetBoneIndex(TEXT("hand_l"));
	const int32 MidL = Body->GetBoneIndex(TEXT("middle_01_l"));
	FVector AxisL(1.0, 0.0, 0.0);
	if (HandL != INDEX_NONE && MidL != INDEX_NONE)
	{
		const FReferenceSkeleton& RefSkel = Body->GetSkeletalMeshAsset()->GetRefSkeleton();
		const FTransform Hand = FAnimationRuntime::GetComponentSpaceTransformRefPose(RefSkel, HandL);
		const FTransform Mid = FAnimationRuntime::GetComponentSpaceTransformRefPose(RefSkel, MidL);
		AxisL = Hand.InverseTransformPosition(Mid.GetLocation()).GetSafeNormal();
	}
	Anim->SetArmAim(0, Chest + Fwd * 55.0 - Up * 12.0 - Right * 5.0, 1.f, AxisL, 8.f);

	// The right palm down on the left forearm, 45% of the way to the wrist, fingers across it.
	const FVector RA = ToVec(Receivers[0].c.a);
	const FVector RB = ToVec(Receivers[0].c.b);
	const FVector Axis = (RB - RA).GetSafeNormal();
	if (Axis.IsNearlyZero())
	{
		return;
	}
	const FVector Centre = RA + (RB - RA) * 0.45;
	const FVector N = (Up - Axis * FVector::DotProduct(Up, Axis)).GetSafeNormal();
	FVector Fingers = FVector::CrossProduct(N, Axis).GetSafeNormal();
	const int32 ShoulderR = Body->GetBoneIndex(TEXT("upperarm_r"));
	if (ShoulderR != INDEX_NONE && FVector::DotProduct(Fingers, Centre - Body->GetBoneTransform(ShoulderR).GetLocation()) < 0.0)
	{
		Fingers = -Fingers;
	}
	const FQuat Local = FRotationMatrix::MakeFromXZ(ProofFingerLocal, ProofPalmLocal).ToQuat();
	const FQuat World = FRotationMatrix::MakeFromXZ(Fingers, -N).ToQuat();
	const FQuat Q = World * Local.Inverse();
	const float Rise = PairSpecs[0].ReceiverRadius + PairSpecs[0].PresserRadius - ProofOverlap + ProofCorrection;
	const FVector Wrist = Centre + N * Rise - Q.RotateVector(ProofHandCentre);
	Anim->SetHandPlace(1, Wrist, Q, 1.f);
}

void UFTFleshContact::ProofHide(bool bHide)
{
	const FName Arm(TEXT("upperarm_r"));
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(GetOwner());
	for (USkeletalMeshComponent* Mesh : Meshes)
	{
		if (!Mesh || (Mesh != Body.Get() && Mesh->LeaderPoseComponent.Get() != Body.Get()) || Mesh->GetBoneIndex(Arm) == INDEX_NONE)
		{
			continue;
		}
		if (bHide)
		{
			Mesh->HideBoneByName(Arm, PBO_None);
		}
		else
		{
			Mesh->UnHideBoneByName(Arm);
		}
	}
}

void UFTFleshContact::ProofSetOriginal(bool bOriginal)
{
	if (!Body)
	{
		return;
	}
	for (int32 I = 0; I < Slots.Num(); ++I)
	{
		UMaterialInterface* Material = bOriginal ? ProofOriginals[I].Get() : Slots[I].Dynamic.Get();
		if (!Material)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTFleshContact proof: slot %d has no original material kept."), Slots[I].Index);
			continue;
		}
		Body->SetMaterial(Slots[I].Index, Material);
	}
}

void UFTFleshContact::ProofShot(const TCHAR* Tag)
{
	const FString File = FString::Printf(TEXT("%s_%s.png"), *ProofPrefix, Tag);
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1600x1200 filename=\"%s\""), *File), *GLog);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: still %s -> %s."), Tag, *File);
}

void UFTFleshContact::ProofLogHandBox(const TCHAR* Tag)
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !Body)
	{
		return;
	}
	static const TCHAR* const Bones[] = {TEXT("hand_r"), TEXT("lowerarm_r"), TEXT("thumb_03_r"), TEXT("index_03_r"),
		TEXT("middle_03_r"), TEXT("ring_03_r"), TEXT("pinky_03_r"), TEXT("index_metacarpal_r"), TEXT("pinky_metacarpal_r")};
	FBox2D Box(ForceInit);
	for (const TCHAR* Bone : Bones)
	{
		const int32 I = Body->GetBoneIndex(Bone);
		FVector2D At;
		if (I != INDEX_NONE && UGameplayStatics::ProjectWorldToScreen(PC, Body->GetBoneTransform(I).GetLocation(), At))
		{
			Box += At;
		}
	}
	int32 W = 0, H = 0;
	PC->GetViewportSize(W, H);
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTFleshContact proof: %s presser hand box (bone centres, viewport px of %dx%d; the still is 1600x1200): %.0f,%.0f .. %.0f,%.0f."),
		Tag, W, H, Box.Min.X, Box.Min.Y, Box.Max.X, Box.Max.Y);
}

bool UFTFleshContact::ProofDepthRead(TArray<float>& Out, float Back)
{
	UWorld* World = GetWorld();
	if (!World || !Body)
	{
		return false;
	}
	if (!ProofCapture)
	{
		ProofTarget = NewObject<UTextureRenderTarget2D>(this);
		ProofTarget->RenderTargetFormat = RTF_R32f;
		ProofTarget->ClearColor = FLinearColor(1e6f, 0.f, 0.f, 0.f);
		ProofTarget->bAutoGenerateMips = false;
		ProofTarget->InitCustomFormat(kDepthSize, kDepthSize, PF_R32_FLOAT, true);
		ProofTarget->UpdateResourceImmediate(true);
		ProofCapture = NewObject<USceneCaptureComponent2D>(GetOwner());
		ProofCapture->bCaptureEveryFrame = false;
		ProofCapture->bCaptureOnMovement = false;
		ProofCapture->ProjectionType = ECameraProjectionMode::Orthographic;
		ProofCapture->OrthoWidth = kDepthWidth;
		ProofCapture->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
		ProofCapture->TextureTarget = ProofTarget;
		ProofCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		ProofCapture->ShowOnlyComponents.Add(Body.Get());
		ProofCapture->RegisterComponentWithWorld(World);
	}
	ProofCapture->SetWorldLocationAndRotation(ProofPoint + ProofNormal * (kDepthStandOff + Back),
		FRotationMatrix::MakeFromX(-ProofNormal).ToQuat());
	ProofCapture->CaptureScene();
	FTextureRenderTargetResource* Resource = ProofTarget->GameThread_GetRenderTargetResource();
	TArray<FLinearColor> Pixels;
	if (!Resource || !Resource->ReadLinearColorPixels(Pixels) || Pixels.Num() != kDepthSize * kDepthSize)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTFleshContact proof: the depth capture read nothing."));
		return false;
	}
	Out.SetNum(Pixels.Num());
	for (int32 I = 0; I < Pixels.Num(); ++I)
	{
		Out[I] = Pixels[I].R;
	}
	return true;
}

void UFTFleshContact::ProofDepth(const TArray<float>& Calib)
{
	const int32 N = kDepthSize * kDepthSize;
	if (ProofDepthNone.Num() != N || ProofDepthContact.Num() != N)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTFleshContact proof: no depth pair to compare."));
		return;
	}
	const float Cm = kDepthWidth / kDepthSize;
	auto Valid = [](float D) { return D > 0.f && D < 1000.f; };
	auto CentreMean = [&](const TArray<float>& A, const TArray<float>& B, int32& OutCount)
	{
		double Sum = 0.0;
		OutCount = 0;
		for (int32 Y = kDepthSize / 2 - 1; Y <= kDepthSize / 2 + 1; ++Y)
		{
			for (int32 X = kDepthSize / 2 - 1; X <= kDepthSize / 2 + 1; ++X)
			{
				const int32 I = Y * kDepthSize + X;
				if (Valid(A[I]) && Valid(B[I]))
				{
					Sum += A[I] - B[I];
					++OutCount;
				}
			}
		}
		return OutCount > 0 ? static_cast<float>(Sum / OutCount) : 0.f;
	};
	int32 CentreCount = 0;
	const float Centre = CentreMean(ProofDepthContact, ProofDepthNone, CentreCount);
	float Max = 0.f, MinD = 0.f;
	FVector2D MaxAt(0.0, 0.0);
	int32 Covered = 0;
	for (int32 I = 0; I < N; ++I)
	{
		if (!Valid(ProofDepthContact[I]) || !Valid(ProofDepthNone[I]))
		{
			continue;
		}
		++Covered;
		const float D = ProofDepthContact[I] - ProofDepthNone[I];
		if (D > Max)
		{
			Max = D;
			MaxAt = FVector2D((I % kDepthSize - kDepthSize / 2) * Cm, (I / kDepthSize - kDepthSize / 2) * Cm);
		}
		MinD = FMath::Min(MinD, D);
	}
	int32 CalibCount = 0;
	const float Cal = Calib.Num() == N ? CentreMean(Calib, ProofDepthNone, CalibCount) : 0.f;
	const float Asked = Last.count > 0 ? Last.c[0].depth : 0.f;
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTFleshContact proof: depth: centre %.3f cm (asked %.3f, %d px), deepest %.3f cm at (%.2f, %.2f) cm, highest ring %.3f cm, ")
		TEXT("calibration %.3f for a 1 cm move (%d px), %d of %d px on the skin, overlap stated %.2f cm measured %.3f cm."),
		Centre, Asked, CentreCount, Max, MaxAt.X, MaxAt.Y, -MinD, Cal, CalibCount, Covered, N, ProofOverlap, ProofPen);
	// The centre row and column, every 4th pixel (cm deep; + into the skin), for the dent and its ring.
	FString Row, Col;
	for (int32 X = 0; X < kDepthSize; X += 4)
	{
		const int32 IR = (kDepthSize / 2) * kDepthSize + X;
		const int32 IC = X * kDepthSize + kDepthSize / 2;
		Row += Valid(ProofDepthContact[IR]) && Valid(ProofDepthNone[IR])
			? FString::Printf(TEXT(" %.3f"), ProofDepthContact[IR] - ProofDepthNone[IR]) : FString(TEXT(" -"));
		Col += Valid(ProofDepthContact[IC]) && Valid(ProofDepthNone[IC])
			? FString::Printf(TEXT(" %.3f"), ProofDepthContact[IC] - ProofDepthNone[IC]) : FString(TEXT(" -"));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: depth row (%.3f cm/px x4):%s"), Cm, *Row);
	UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: depth column:%s"), *Col);
}

void UFTFleshContact::TickProof()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	++ProofFrames;
	switch (ProofStage)
	{
	case 0: // the copies registered, the shaders built, the hand's own frame read from the ref pose
	{
		if (!bRegistered && ProofFrames % 60 == 0)
		{
			bTried = false; // the lead's mesh may have come after the first try
		}
		Step();
		const bool bShaders = !GShaderCompilingManager || GShaderCompilingManager->GetNumRemainingJobs() == 0;
		if (ProofFrames > 20000)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTFleshContact proof: not ready after %d frames (registered %d, shaders %d)."),
				ProofFrames, bRegistered ? 1 : 0, bShaders ? 1 : 0);
			ProofFinish(1);
			return;
		}
		if (!bRegistered || !bShaders || ProofFrames < 120 || !Body)
		{
			return;
		}
		const FReferenceSkeleton& RefSkel = Body->GetSkeletalMeshAsset()->GetRefSkeleton();
		const int32 Hand = RefSkel.FindBoneIndex(TEXT("hand_r"));
		const int32 Mid = RefSkel.FindBoneIndex(TEXT("middle_01_r"));
		const int32 Index = RefSkel.FindBoneIndex(TEXT("index_metacarpal_r"));
		const int32 Pinky = RefSkel.FindBoneIndex(TEXT("pinky_metacarpal_r"));
		if (Hand == INDEX_NONE || Mid == INDEX_NONE || Index == INDEX_NONE || Pinky == INDEX_NONE || !Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance()))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTFleshContact proof: the right hand's bones or the locomotion anim instance are missing."));
			ProofFinish(1);
			return;
		}
		const FTransform HandRef = FAnimationRuntime::GetComponentSpaceTransformRefPose(RefSkel, Hand);
		const FVector F = HandRef.InverseTransformPosition(FAnimationRuntime::GetComponentSpaceTransformRefPose(RefSkel, Mid).GetLocation());
		const FVector Across = HandRef.InverseTransformPosition(FAnimationRuntime::GetComponentSpaceTransformRefPose(RefSkel, Index).GetLocation())
			- HandRef.InverseTransformPosition(FAnimationRuntime::GetComponentSpaceTransformRefPose(RefSkel, Pinky).GetLocation());
		ProofPalmLocal = FVector::CrossProduct(F, Across).GetSafeNormal(); // right hand, the engine's axes: out of the palm
		ProofFingerLocal = (F - ProofPalmLocal * FVector::DotProduct(F, ProofPalmLocal)).GetSafeNormal();
		ProofHandCentre = F * 0.5;
		UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: hand_r frame: fingers %s, palm %s, capsule centre %s (hand bone space)."),
			*ProofFingerLocal.ToString(), *ProofPalmLocal.ToString(), *ProofHandCentre.ToString());
		ProofStage = 1;
		ProofFrames = 0;
		return;
	}
	case 1: // place and settle at the stated overlap
	{
		ProofPlace();
		Step();
		float S = 0.f, T = 0.f;
		const fl::Capsule& P = Pressers[0];
		const fl::Capsule& R = Receivers[0].c;
		fl::closestOnSegments(R.a, R.b, P.a, P.b, S, T);
		const fl::V3 Gap = (P.a + (P.b - P.a) * T) - (R.a + (R.b - R.a) * S);
		ProofPen = P.r + R.r - fl::length(Gap);
		const float Off = ProofPen - ProofOverlap;
		ProofCorrection = FMath::Clamp(ProofCorrection + 0.5f * Off, -5.f, 5.f);
		ProofSettleCount = FMath::Abs(Off) < 0.02f ? ProofSettleCount + 1 : 0;
		if (ProofFrames % 30 == 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: placing, frame %d: overlap %.3f cm (want %.2f), correction %.3f, contacts %d."),
				ProofFrames, ProofPen, ProofOverlap, ProofCorrection, Last.count);
		}
		if (ProofSettleCount >= kSettleFrames || ProofFrames >= kPlaceTimeout)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: %s after %d frames: overlap %.3f cm (want %.2f), correction %.3f cm."),
				ProofSettleCount >= kSettleFrames ? TEXT("settled") : TEXT("NOT SETTLED"), ProofFrames, ProofPen, ProofOverlap, ProofCorrection);
			bProofUnsettled = ProofSettleCount < kSettleFrames; // the stills are still taken, but the run exits 1
			ProofStage = bProofAllocs ? 2 : 3;
			ProofFrames = 0;
		}
		return;
	}
	case 2: // allocations over the unlatched solve
	{
		ProofPlace();
		const uint64 Solve0 = GCounted[1], Write0 = GCounted[2];
		GCountMode = 1;
		Step();
		GCountMode = 0;
		ProofAllocSolve += GCounted[1] - Solve0;
		ProofAllocWrite += GCounted[2] - Write0;
		++ProofAllocFrames;
		ProofAllocWriteFrames += WroteLast > 0 ? 1 : 0;
		if (ProofAllocFrames >= kAllocFrames)
		{
			UE_LOG(LogForgottenThird, Display,
				TEXT("FTFleshContact proof: allocations over %d solved frames: solve %llu, parameter writes %llu (over the %d frames that wrote)."),
				ProofAllocFrames, ProofAllocSolve, ProofAllocWrite, ProofAllocWriteFrames);
			ProofStage = 3;
			ProofFrames = 0;
		}
		return;
	}
	case 3: // latch, and the camera at 1 m
	{
		ProofPlace();
		Step();
		if (Last.count == 0 || !(Last.c[0].depth > 0.f))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTFleshContact proof: no contact to latch (overlap %.3f cm)."), ProofPen);
			ProofFinish(1);
			return;
		}
		ProofPoint = ToVec(Last.c[0].point);
		ProofNormal = ToVec(Last.c[0].normal);
		SetLatched(true);
		UE_LOG(LogForgottenThird, Display,
			TEXT("FTFleshContact proof: latched: point %s, normal %s, patch radius %.3f cm, depth asked %.3f cm; shown pos %s, nrm %s."),
			*ProofPoint.ToString(), *ProofNormal.ToString(), Last.c[0].radius, Last.c[0].depth,
			*ShownPos(0).ToString(), *ShownNrm(0).ToString());
		const int32 Lower = Body->GetBoneIndex(TEXT("lowerarm_l"));
		static const TCHAR* const Twists[] = {TEXT("lowerarm_twist_01_l"), TEXT("lowerarm_twist_02_l"), TEXT("hand_l")};
		for (const TCHAR* Twist : Twists)
		{
			const int32 I = Body->GetBoneIndex(Twist);
			if (I != INDEX_NONE && Lower != INDEX_NONE)
			{
				const FQuat Rel = Body->GetBoneTransform(Lower).GetRotation().Inverse() * Body->GetBoneTransform(I).GetRotation();
				UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: %s against lowerarm_l: %s."), Twist, *Rel.Rotator().ToString());
			}
		}
		const FVector Palm = Body->GetBoneTransform(Body->GetBoneIndex(TEXT("hand_r"))).GetRotation().RotateVector(ProofPalmLocal);
		UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: palm . -normal = %.3f (1: the palm faces the forearm)."),
			FVector::DotProduct(Palm, -ProofNormal));

		const FVector Axis = (ToVec(Receivers[0].c.b) - ToVec(Receivers[0].c.a)).GetSafeNormal();
		FVector Side = FVector::CrossProduct(ProofNormal, Axis).GetSafeNormal();
		if (FVector::DotProduct(Side, GetOwner()->GetActorForwardVector()) < 0.0)
		{
			Side = -Side;
		}
		const FVector Dir = (Side + ProofNormal * 0.5).GetSafeNormal();
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ProofCamera = World->SpawnActor<ACameraActor>(ProofPoint + Dir * 100.0, FRotationMatrix::MakeFromX(-Dir).Rotator(), Spawn);
		if (ProofCamera)
		{
			ProofCamera->GetCameraComponent()->SetFieldOfView(30.f);
			ProofCamera->GetCameraComponent()->bConstrainAspectRatio = false;
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				PC->SetViewTarget(ProofCamera);
				if (AHUD* Hud = PC->GetHUD())
				{
					Hud->bShowHUD = false;
				}
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTFleshContact proof: camera 100 cm from the contact, from %s."), *Dir.ToString());
		ProofStage = 4;
		ProofFrames = 0;
		return;
	}
	default: // 4 + still index
	{
		ProofPlace(); // the arms held where they are
		const int32 Still = ProofStage - 4;
		if (Still >= static_cast<int32>(UE_ARRAY_COUNT(kStills)))
		{
			TArray<float> Calib;
			ProofHide(true);
			SetSuppressed(true);
			if (!ProofDepthRead(Calib, 1.f))
			{
				Calib.Reset();
			}
			ProofDepth(Calib);
			ProofFinish(ProofDepthContact.Num() > 0 && ProofDepthNone.Num() > 0 && !bProofUnsettled ? 0 : 1);
			return;
		}
		const FProofStill& S = kStills[Still];
		if (ProofFrames == 1)
		{
			ProofSetOriginal(S.bOriginal);
			SetSuppressed(S.bSuppressed);
			ProofHide(S.bHidden);
		}
		else if (ProofFrames == kShotAt)
		{
			ProofShot(S.Tag);
			if (!S.bHidden)
			{
				ProofLogHandBox(S.Tag);
			}
		}
		else if (ProofFrames == kShotAt + 5 && S.Depth > 0)
		{
			if (!ProofDepthRead(S.Depth == 1 ? ProofDepthContact : ProofDepthNone, 0.f))
			{
				(S.Depth == 1 ? ProofDepthContact : ProofDepthNone).Reset();
			}
		}
		else if (ProofFrames >= kShotDone)
		{
			++ProofStage;
			ProofFrames = 0;
		}
		return;
	}
	}
}
#endif
