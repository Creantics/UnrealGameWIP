#include "FTLook.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Misc/CommandLine.h"
#include "UObject/UnrealType.h"

#include "ForgottenThird.h"

namespace
{
// The HairStrands plugin's groom component, or null when the plugin isn't enabled.
UClass* GroomComponentClass()
{
	return FindObject<UClass>(nullptr, TEXT("/Script/HairStrandsCore.GroomComponent"));
}

UObject* GetObjectProperty(const UObject& Source, const TCHAR* Name)
{
	const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Source.GetClass(), Name);
	return Property ? Property->GetObjectPropertyValue_InContainer(&Source) : nullptr;
}

// False when the class has no object property of that name or the value isn't of its class.
bool SetObjectProperty(UObject& Target, const TCHAR* Name, UObject* Value)
{
	const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Target.GetClass(), Name);
	if (!Property || (Value && !Value->IsA(Property->PropertyClass)))
	{
		return false;
	}
	Property->SetObjectPropertyValue_InContainer(&Target, Value);
	return true;
}

// The member's own /Game/Characters/<Name>/<name>/SkeletalMeshes/<name><Suffix>; null without a name.
template <typename T>
T* LoadOwn(const FString& Name, const TCHAR* Suffix)
{
	if (Name.IsEmpty())
	{
		return nullptr;
	}
	const FString Lower = Name.ToLower();
	const FString Asset = Lower + Suffix;
	return LoadObject<T>(nullptr, *FString::Printf(TEXT("/Game/Characters/%s/%s/SkeletalMeshes/%s.%s"), *Name, *Lower, *Asset, *Asset));
}

// Path when it loads, else the member's own (LoadOwn).
template <typename T>
T* LoadOrOwn(const FSoftObjectPath& Path, const FString& Name, const TCHAR* Suffix, const TCHAR* Who)
{
	if (!Path.IsNull())
	{
		if (T* Loaded = Cast<T>(Path.TryLoad()))
		{
			return Loaded;
		}
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: no %s at %s; trying %s's own."), Who, *T::StaticClass()->GetName(),
			*Path.ToString(), *Name);
	}
	return LoadOwn<T>(Name, Suffix);
}
}

FFTLoadedLook FTLook::LoadLook(const FFTLeadLook& Look, const TCHAR* Who)
{
	FFTLoadedLook Loaded;
	Loaded.Body = LoadOrOwn<USkeletalMesh>(Look.BodyMeshPath, Look.Name, TEXT(""), Who);
	Loaded.ClipFolder = !Look.ClipFolder.IsEmpty() || Look.Name.IsEmpty() ? Look.ClipFolder
		: FString::Printf(TEXT("/Game/Characters/%s/Rig/Retargeted"), *Look.Name);
	if (!Loaded.Body)
	{
		return Loaded;
	}
	const USkeletalMesh& Body = *Loaded.Body;
	auto Fitting = [&Body, Who](UAnimSequence* Clip) { return Clip && UFTLocomotionAnimInstance::FitsBody(*Clip, Body, Who) ? Clip : nullptr; };
	int32 Missing = 0;
	for (int32 I = 0; I < UFTLocomotionAnimInstance::NumBaseClips; ++I)
	{
		const ftcore::LocoClip Which = static_cast<ftcore::LocoClip>(I);
		// The idle and the walk can be the look's own (a path), else they are the member's own once nothing is retargeted.
		const bool bIdle = Which == ftcore::LocoClip::Idle;
		const bool bOwn = bIdle || Which == ftcore::LocoClip::Walk;
		const FSoftObjectPath& Path = bIdle ? Look.IdleClipPath : Look.WalkClipPath;
		UAnimSequence* Clip = nullptr;
		if (bOwn && !Path.IsNull())
		{
			UAnimSequence* Set = Cast<UAnimSequence>(Path.TryLoad());
			if (!Set)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("%s: no AnimSequence at %s; trying the retargeted one."), Who, *Path.ToString());
			}
			Clip = Fitting(Set);
		}
		if (!Clip)
		{
			Clip = UFTLocomotionAnimInstance::FindRetargeted(Which, Loaded.ClipFolder, Body, Who);
		}
		if (!Clip && bOwn)
		{
			Clip = Fitting(LoadOwn<UAnimSequence>(Look.Name, bIdle ? TEXT("idle") : TEXT("walk")));
		}
		Loaded.Clips[I] = Clip;
		Missing += Clip == nullptr;
	}
	if (Missing > 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: %d of the %d base clips weren't found for %s (retargeted ones in %s)."), Who,
			Missing, UFTLocomotionAnimInstance::NumBaseClips, *Body.GetName(), *Loaded.ClipFolder);
	}
	return Loaded;
}

int32 FTLook::AddPieces(AActor& Owner, USkeletalMeshComponent& Body, const TArray<FSoftObjectPath>& Paths,
	TArray<TObjectPtr<USkeletalMeshComponent>>& OutPieces, const TCHAR* Who)
{
	const bool bNoPieces = FParse::Param(FCommandLine::Get(), TEXT("LocoNoPieces"));
	FString NoPiece;
	TArray<FString> NoPieceWords;
	if (FParse::Value(FCommandLine::Get(), TEXT("LocoNoPiece="), NoPiece))
	{
		NoPiece.ParseIntoArray(NoPieceWords, TEXT("+"));
	}
	if (!FParse::Param(FCommandLine::Get(), TEXT("LocoCollar")))
	{
		NoPieceWords.Add(TEXT("collar"));
	}
	int32 Cloth = 0;
	for (const FSoftObjectPath& Path : Paths)
	{
		const FString Name = Path.GetAssetName();
		if (bNoPieces || NoPieceWords.ContainsByPredicate([&Name](const FString& Word) { return Name.Contains(Word); }))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s: piece %s left off."), Who, *Name);
			continue;
		}
		USkeletalMesh* PieceMesh = Cast<USkeletalMesh>(Path.TryLoad());
		if (!PieceMesh)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: no skeletal mesh at %s; piece skipped."), Who, *Path.ToString());
			continue;
		}
		USkeletalMeshComponent* Piece = NewObject<USkeletalMeshComponent>(&Owner);
		Piece->SetupAttachment(&Body);
		Piece->SetSkeletalMesh(PieceMesh);
		Piece->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Piece->bCollideWithEnvironment = true; // cloth pieces (FTBuildCloth) also push off the level, not just the body
		Piece->RegisterComponent();
		Cloth += PieceMesh->GetMeshClothingAssets().Num() > 0;
		Piece->SetLeaderPoseComponent(&Body);
		OutPieces.Add(Piece);
	}
	return Cloth;
}

int32 FTLook::AddGrooms(AActor& Owner, USkeletalMeshComponent& Body, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces,
	const TArray<FFTGroomLook>& Grooms, TArray<TObjectPtr<USceneComponent>>& OutGrooms, const TCHAR* Who)
{
	if (Grooms.IsEmpty())
	{
		return 0;
	}
	UClass* GroomClass = GroomComponentClass();
	if (!GroomClass)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: %d grooms skipped; the HairStrands plugin isn't enabled."), Who, Grooms.Num());
		return 0;
	}
	int32 Added = 0;
	for (const FFTGroomLook& Look : Grooms)
	{
		UObject* Groom = Look.Groom.TryLoad();
		UObject* Binding = Look.Binding.IsNull() ? nullptr : Look.Binding.TryLoad();
		if (!Groom)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: no groom at %s; skipped."), Who, *Look.Groom.ToString());
			continue;
		}
		USkeletalMeshComponent* Parent = &Body;
		if (const UObject* Target = Binding ? GetObjectProperty(*Binding, TEXT("TargetSkeletalMesh")) : nullptr)
		{
			for (USkeletalMeshComponent* Piece : Pieces)
			{
				if (Piece && Piece->GetSkeletalMeshAsset() == Target)
				{
					Parent = Piece;
					break;
				}
			}
		}
		USceneComponent* Component = NewObject<USceneComponent>(&Owner, GroomClass);
		// Set before registering, as a template's values would be: registering builds the groom's resources.
		if (!SetObjectProperty(*Component, TEXT("GroomAsset"), Groom)
			|| (Binding && !SetObjectProperty(*Component, TEXT("BindingAsset"), Binding)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: %s or its binding doesn't fit the groom component; skipped."), Who,
				*Look.Groom.ToString());
			Component->DestroyComponent();
			continue;
		}
		Component->SetupAttachment(Parent);
		Component->RegisterComponent();
		OutGrooms.Add(Component);
		++Added;
	}
	return Added;
}
