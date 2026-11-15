#include "FTLook.h"

#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

#include "ForgottenThird.h"
#include "FTText.h"
#include "ftcore/GarmentFront.h"
#include "ftcore/LookBody.h"

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

namespace
{
struct FPieceFilter
{
	bool bNoPieces = false;
	std::vector<std::string> Words;
};

const FPieceFilter& PieceFilter()
{
	static const FPieceFilter Filter = []
	{
		FPieceFilter F;
#if !UE_BUILD_SHIPPING
		// Look-dev only: a shipping build always wears every piece.
		F.bNoPieces = FParse::Param(FCommandLine::Get(), TEXT("LocoNoPieces"));
		FString NoPiece;
		if (FParse::Value(FCommandLine::Get(), TEXT("LocoNoPiece="), NoPiece))
		{
			TArray<FString> Words;
			NoPiece.ParseIntoArray(Words, TEXT("+"));
			for (const FString& Word : Words)
			{
				F.Words.push_back(FT::ToUtf8(Word));
			}
		}
#endif
		return F;
	}();
	return Filter;
}

// The body path to wear: the full body while some covering garment is left off.
FSoftObjectPath WornBodyPath(const FFTLeadLook& Look, const TArray<FSoftObjectPath>& WornPieces, const TCHAR* Who)
{
	if (Look.FullBodyMeshPath.IsNull() || Look.CoverWords.IsEmpty())
	{
		return Look.BodyMeshPath;
	}
	std::vector<std::string> Cover;
	for (const FString& Word : Look.CoverWords)
	{
		Cover.push_back(FT::ToUtf8(Word));
	}
	std::vector<std::string> Worn;
	for (const FSoftObjectPath& Piece : WornPieces)
	{
		if (!FTLook::PieceLeftOff(Piece))
		{
			Worn.push_back(FT::ToUtf8(Piece.GetAssetName()));
		}
	}
	if (!ftcore::look::NeedsFullBody(Cover, Worn))
	{
		return Look.BodyMeshPath;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: a covering piece is off; wearing the full body %s."), Who, *Look.FullBodyMeshPath.ToString());
	return Look.FullBodyMeshPath;
}
}

namespace
{
FString FromUtf8(const std::string& Text)
{
	return FString(UTF8_TO_TCHAR(Text.c_str()));
}

// The scene's asks (FTLook::SetSceneFronts) and then -LookFront's, so the command line wins for captures.
std::vector<ftcore::look::FrontAsk>& SceneFronts()
{
	static std::vector<ftcore::look::FrontAsk> Asks;
	return Asks;
}

std::vector<ftcore::look::FrontAsk> FrontAsks()
{
	std::vector<ftcore::look::FrontAsk> Asks = SceneFronts();
#if !UE_BUILD_SHIPPING
	static const std::vector<ftcore::look::FrontAsk> Line = []
	{
		std::vector<ftcore::look::FrontAsk> Parsed;
		FString Text;
		if (FParse::Value(FCommandLine::Get(), TEXT("LookFront="), Text) && !ftcore::look::ParseFrontAsks(FT::ToUtf8(Text), Parsed))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTLook: -LookFront=%s doesn't parse (open, closed, or word:state joined by +); ignored."), *Text);
		}
		return Parsed;
	}();
	Asks.insert(Asks.end(), Line.begin(), Line.end());
#endif
	return Asks;
}

// The look's pairs for Core: paths canonical (an ini path without the asset suffix gains it), openMade from what PrepareFronts loaded.
std::vector<ftcore::look::FrontPair> FrontPairs(const FFTLeadLook& Look, const FFTWornFronts& Fronts)
{
	std::vector<ftcore::look::FrontPair> Pairs;
	Pairs.reserve(Look.Fronts.Num());
	for (int32 I = 0; I < Look.Fronts.Num(); ++I)
	{
		const FFTGarmentFronts& Pair = Look.Fronts[I];
		const bool bOpenMade = Fronts.Open.IsValidIndex(I) && Fronts.Open[I] != nullptr;
		Pairs.push_back({ftcore::look::CanonicalPath(FT::ToUtf8(Pair.Closed.ToString())), ftcore::look::CanonicalPath(FT::ToUtf8(Pair.Open.ToString())),
			Pair.bOpen ? ftcore::look::Front::Open : ftcore::look::Front::Closed, bOpenMade});
	}
	return Pairs;
}

void WarnUnmatched(const std::vector<ftcore::look::FrontPair>& Pairs, const std::vector<ftcore::look::FrontAsk>& Asks, const TCHAR* Who)
{
	for (const std::string& Word : ftcore::look::UnmatchedAskWords(Pairs, Asks))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: front word \"%s\" names none of the look's garment pairs; ignored."), Who, *FromUtf8(Word));
	}
}
}

// The under-layer helpers, named for this file (unity builds merge anonymous namespaces across files).
namespace FTLookLayers
{
// A path as the engine names its asset: an ini path without the asset suffix gains it, so it equals the loaded mesh's.
FString Canonical(const FSoftObjectPath& Path)
{
	return FromUtf8(ftcore::look::CanonicalPath(FT::ToUtf8(Path.ToString())));
}

FString Canonical(const UObject& Asset)
{
	return Canonical(FSoftObjectPath(&Asset));
}

// The look's under layers for Core, paths canonical: only those whose cut PrepareFronts loaded, or (bAll) every one.
std::vector<ftcore::look::UnderLayer> UnderLayers(const FFTLeadLook& Look, const FFTWornFronts& Fronts, bool bAll = false)
{
	std::vector<ftcore::look::UnderLayer> Layers;
	for (int32 I = 0; I < Look.UnderLayers.Num(); ++I)
	{
		if (bAll || (Fronts.Cut.IsValidIndex(I) && Fronts.Cut[I] != nullptr))
		{
			const FFTUnderLayer& Layer = Look.UnderLayers[I];
			Layers.push_back({FT::ToUtf8(Canonical(Layer.Under)), FT::ToUtf8(Layer.OverWord), FT::ToUtf8(Canonical(Layer.Cut))});
		}
	}
	return Layers;
}

uint32& Serial()
{
	static uint32 Value = 0;
	return Value;
}

// Puts another fitted mesh on a worn piece. The two meshes may have different slot counts, so the overrides set for the
// old mesh are taken off, the look's swaps are made again on the new slots, and then what each old slot showed is
// carried to its new slot by name (ftcore::look::SlotCarry): see FTLook::SetFront.
void WearMesh(USkeletalMeshComponent& Piece, USkeletalMesh& Mesh, USkeletalMeshComponent& Body, const FFTWornFronts& Fronts)
{
	const USkeletalMesh* Old = Piece.GetSkeletalMeshAsset();
	const TArray<FName> OldNames = Piece.GetMaterialSlotNames();
	TArray<UMaterialInterface*, TInlineAllocator<8>> OldShown;
	TArray<UMaterialInterface*, TInlineAllocator<8>> OldOwn;
	std::vector<std::string> OldSlots;
	for (int32 I = 0; I < OldNames.Num(); ++I)
	{
		OldShown.Add(Piece.OverrideMaterials.IsValidIndex(I) ? Piece.OverrideMaterials[I].Get() : nullptr);
		OldOwn.Add(Old && Old->GetMaterials().IsValidIndex(I) ? Old->GetMaterials()[I].MaterialInterface.Get() : nullptr);
		OldSlots.push_back(FT::ToUtf8(OldNames[I].ToString()));
	}
	TArray<TPair<FName, float>, TInlineAllocator<8>> Morphs;
	if (Old)
	{
		for (const UMorphTarget* Morph : Old->GetMorphTargets())
		{
			const float Weight = Morph ? Piece.GetMorphTarget(Morph->GetFName()) : 0.f;
			if (Weight != 0.f)
			{
				Morphs.Emplace(Morph->GetFName(), Weight);
			}
		}
	}

	Piece.SetSkeletalMeshAsset(&Mesh);
	Piece.EmptyOverrideMaterials();
	Piece.SetLeaderPoseComponent(&Body, /*bForceUpdate=*/true);
	Piece.RecreateClothingActors();
	for (int32 S = 0; S < Fronts.SwapFrom.Num() && S < Fronts.SwapTo.Num(); ++S)
	{
		FTLook::SwapSlots(Piece, Fronts.SwapFrom[S], Fronts.SwapTo[S]);
	}

	std::vector<std::string> NewSlots;
	for (const FName& Name : Piece.GetMaterialSlotNames())
	{
		NewSlots.push_back(FT::ToUtf8(Name.ToString()));
	}
	const std::vector<int> From = ftcore::look::SlotCarry(OldSlots, NewSlots);
	const TArray<FSkeletalMaterial>& NewOwn = Mesh.GetMaterials();
	for (int32 J = 0; J < static_cast<int32>(From.size()); ++J)
	{
		const int32 K = From[static_cast<size_t>(J)];
		UMaterialInterface* Shown = OldShown.IsValidIndex(K) ? OldShown[K] : nullptr;
		UMaterialInterface* Now = Piece.GetMaterial(J);
		if (!Shown || Shown == Now)
		{
			continue;
		}
		if (UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Shown))
		{
			if (Dynamic->Parent == Now)
			{
				Piece.SetMaterial(J, Dynamic);
			}
			else if (Now)
			{
				UMaterialInstanceDynamic* Made = UMaterialInstanceDynamic::Create(Now, &Piece);
				Made->CopyParameterOverrides(Dynamic);
				Piece.SetMaterial(J, Made);
			}
		}
		else if (OldOwn[K] && NewOwn.IsValidIndex(J) && NewOwn[J].MaterialInterface == OldOwn[K])
		{
			Piece.SetMaterial(J, Shown);
		}
	}
	for (const TPair<FName, float>& Morph : Morphs)
	{
		if (Mesh.FindMorphTarget(Morph.Key))
		{
			Piece.SetMorphTarget(Morph.Key, Morph.Value);
		}
	}
	++Serial();
}

// The loaded mesh an under layer path names (one of the layers' under pieces or cuts, canonical), else null.
USkeletalMesh* UnderLayerMesh(const FFTLeadLook& Look, const FFTWornFronts& Fronts, const FString& Path)
{
	for (int32 I = 0; I < Look.UnderLayers.Num(); ++I)
	{
		const FFTUnderLayer& Layer = Look.UnderLayers[I];
		if (Fronts.Cut.IsValidIndex(I) && Fronts.Cut[I] && Canonical(Layer.Cut).Equals(Path, ESearchCase::IgnoreCase))
		{
			return Fronts.Cut[I];
		}
		if (Fronts.Under.IsValidIndex(I) && Fronts.Under[I] && Canonical(Layer.Under).Equals(Path, ESearchCase::IgnoreCase))
		{
			return Fronts.Under[I];
		}
	}
	return nullptr;
}

// Warns about layers that can't do what they say: an under piece the look doesn't wear, an over word that names none of
// the look's pieces (or several, all in part, so another piece could stand for the over piece), and two layers of one
// under piece whose words overlap.
void WarnLayers(const FFTLeadLook& Look, const TCHAR* Who)
{
	TArray<FString> Candidates;
	for (const FSoftObjectPath& Piece : Look.PiecePaths)
	{
		Candidates.AddUnique(Canonical(Piece));
	}
	for (const FFTGarmentFronts& Pair : Look.Fronts)
	{
		Candidates.AddUnique(Canonical(Pair.Closed));
		if (!Pair.Open.IsNull())
		{
			Candidates.AddUnique(Canonical(Pair.Open));
		}
	}
	const std::vector<ftcore::look::UnderLayer> Layers = UnderLayers(Look, FFTWornFronts(), /*bAll=*/true);
	for (const ftcore::look::UnderLayer& Layer : Layers)
	{
		const FString Under = FromUtf8(Layer.under);
		const FString UnderName = FPackageName::ObjectPathToObjectName(Under);
		if (!Candidates.ContainsByPredicate([&Under](const FString& C) { return C.Equals(Under, ESearchCase::IgnoreCase); }))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: under layer %s isn't one of the look's pieces; its cut is never worn."), Who, *Under);
		}
		TArray<FString> Named;
		bool bExact = false;
		for (const FString& Candidate : Candidates)
		{
			const int Match = Candidate.Equals(Under, ESearchCase::IgnoreCase) ? 0 : ftcore::look::OverMatch(FT::ToUtf8(Candidate), Layer.overWord);
			if (Match > 0)
			{
				Named.Add(FPackageName::ObjectPathToObjectName(Candidate));
				bExact |= Match == 2;
			}
		}
		if (Named.IsEmpty())
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: under layer over word \"%s\" (for %s) names none of the look's pieces; that cut is never worn."),
				Who, *FromUtf8(Layer.overWord), *UnderName);
		}
		else if (Named.Num() > 1 && !bExact)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: under layer over word \"%s\" (for %s) names %d pieces in part (%s); give the over piece's path."),
				Who, *FromUtf8(Layer.overWord), *UnderName, Named.Num(), *FString::Join(Named, TEXT(", ")));
		}
	}
	for (const std::pair<int, int>& Pair : ftcore::look::OverlappingLayers(Layers))
	{
		const ftcore::look::UnderLayer& A = Layers[static_cast<size_t>(Pair.first)];
		const ftcore::look::UnderLayer& B = Layers[static_cast<size_t>(Pair.second)];
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: under layers %d and %d of %s have overlapping over words (\"%s\", \"%s\"); the best match wins, give paths to be sure."),
			Who, Pair.first, Pair.second, *FPackageName::ObjectPathToObjectName(FromUtf8(A.under)), *FromUtf8(A.overWord), *FromUtf8(B.overWord));
	}
}
}

uint32 FTLook::WornMeshSerial()
{
	return FTLookLayers::Serial();
}

FFTWornFronts FTLook::PrepareFronts(const FFTLeadLook& Look, const TCHAR* Who)
{
	FFTWornFronts Fronts;
	for (const FFTGarmentFronts& Pair : Look.Fronts)
	{
		USkeletalMesh* Closed = Cast<USkeletalMesh>(Pair.Closed.TryLoad());
		if (!Closed)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: closed garment %s doesn't load."), Who, *Pair.Closed.ToString());
		}
		// An open mesh not made yet (no package) keeps the garment closed rather than leaving it off.
		USkeletalMesh* Open = nullptr;
		if (!Pair.Open.IsNull() && FPackageName::DoesPackageExist(Pair.Open.GetLongPackageName()))
		{
			Open = Cast<USkeletalMesh>(Pair.Open.TryLoad());
		}
		if (!Open && !Pair.Open.IsNull())
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s: open garment %s isn't made; %s is worn closed."), Who, *Pair.Open.ToString(),
				*Pair.Closed.GetAssetName());
		}
		Fronts.Closed.Add(Closed);
		Fronts.Open.Add(Open);
	}
	for (const FFTUnderLayer& Layer : Look.UnderLayers)
	{
		const FSoftObjectPath UnderPath(FTLookLayers::Canonical(Layer.Under));
		const FSoftObjectPath CutPath(FTLookLayers::Canonical(Layer.Cut));
		USkeletalMesh* Under = Cast<USkeletalMesh>(UnderPath.TryLoad());
		if (!Under)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: under piece %s doesn't load; its cut is never worn."), Who, *Layer.Under.ToString());
		}
		// A cut not made yet (no package) leaves the whole piece worn, as an open mesh not made leaves a garment closed.
		USkeletalMesh* Cut = nullptr;
		if (Under && !CutPath.IsNull() && FPackageName::DoesPackageExist(CutPath.GetLongPackageName()))
		{
			Cut = Cast<USkeletalMesh>(CutPath.TryLoad());
			if (!Cut)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("%s: under cut %s doesn't load; the whole %s is worn."), Who, *Layer.Cut.ToString(),
					*Layer.Under.GetAssetName());
			}
		}
		else if (Under)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s: under cut %s isn't made; the whole %s is worn."), Who, *Layer.Cut.ToString(),
				*Layer.Under.GetAssetName());
		}
		Fronts.Under.Add(Under);
		Fronts.Cut.Add(Cut);
	}
	if (!Look.Fronts.IsEmpty() || !Look.UnderLayers.IsEmpty())
	{
		// SwapMaterials warns at setup about a swap that doesn't load; here it is only left out.
		for (const FFTMaterialSwap& Swap : Look.MaterialSwaps)
		{
			UMaterialInterface* From = Cast<UMaterialInterface>(Swap.From.TryLoad());
			UMaterialInterface* To = Cast<UMaterialInterface>(Swap.To.TryLoad());
			if (From && To)
			{
				Fronts.SwapFrom.Add(From);
				Fronts.SwapTo.Add(To);
			}
		}
	}
	if (!Look.Fronts.IsEmpty())
	{
		WarnUnmatched(FrontPairs(Look, Fronts), FrontAsks(), Who);
	}
	if (!Look.UnderLayers.IsEmpty())
	{
		FTLookLayers::WarnLayers(Look, Who);
	}
	return Fronts;
}

TArray<FSoftObjectPath> FTLook::WornPiecePaths(const FFTLeadLook& Look, const FFTWornFronts& Fronts)
{
	TArray<FSoftObjectPath> Worn;
	if (Look.Fronts.IsEmpty())
	{
		Worn = Look.PiecePaths;
	}
	else
	{
		const std::vector<ftcore::look::FrontPair> Pairs = FrontPairs(Look, Fronts);
		const std::vector<ftcore::look::FrontAsk> Asks = FrontAsks();
		Worn.Reserve(Look.PiecePaths.Num());
		for (const FSoftObjectPath& Piece : Look.PiecePaths)
		{
			const ftcore::look::FrontChoice Choice = ftcore::look::WornChoice(FT::ToUtf8(FTLookLayers::Canonical(Piece)), Pairs, Asks);
			if (Choice.pair < 0)
			{
				Worn.Add(Piece);
				continue;
			}
			const FFTGarmentFronts& Pair = Look.Fronts[Choice.pair];
			Worn.Add(Choice.half == ftcore::look::Front::Open ? Pair.Open : Pair.Closed);
		}
	}
	const std::vector<ftcore::look::UnderLayer> Layers = FTLookLayers::UnderLayers(Look, Fronts);
	if (Layers.empty())
	{
		return Worn;
	}
	// The over pieces are those actually worn: a piece the command line leaves off covers nothing (UnderChoiceLeftOff).
	std::vector<std::string> WornNames;
	for (const FSoftObjectPath& Piece : Worn)
	{
		WornNames.push_back(FT::ToUtf8(FTLookLayers::Canonical(Piece)));
	}
	const FPieceFilter& Filter = PieceFilter();
	for (FSoftObjectPath& Piece : Worn)
	{
		const std::string Path = FT::ToUtf8(FTLookLayers::Canonical(Piece));
		const std::string Choice = ftcore::look::UnderChoiceLeftOff(Path, Layers, WornNames, Filter.bNoPieces, Filter.Words);
		if (Choice != Path)
		{
			Piece = FSoftObjectPath(FromUtf8(Choice));
		}
	}
	return Worn;
}

bool FTLook::SetSceneFronts(const FString& Asks)
{
	std::vector<ftcore::look::FrontAsk> Parsed;
	if (!Asks.IsEmpty() && !ftcore::look::ParseFrontAsks(FT::ToUtf8(Asks), Parsed))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTLook: scene fronts \"%s\" don't parse; kept as they were."), *Asks);
		return false;
	}
	SceneFronts() = std::move(Parsed);
	return true;
}

int32 FTLook::SetFront(const FFTLeadLook& Look, const FFTWornFronts& Fronts, USkeletalMeshComponent& Body,
	const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces, const FString& Word, bool bOpen, const TCHAR* Who)
{
	const std::vector<ftcore::look::FrontPair> Pairs = FrontPairs(Look, Fronts);
	const std::vector<ftcore::look::FrontAsk> Ask = {{FT::ToUtf8(Word), bOpen ? ftcore::look::Front::Open : ftcore::look::Front::Closed}};
	WarnUnmatched(Pairs, Ask, Who);
	const std::vector<ftcore::look::UnderLayer> Layers = FTLookLayers::UnderLayers(Look, Fronts);
	int32 Changed = 0;
	for (USkeletalMeshComponent* Piece : Pieces)
	{
		const USkeletalMesh* Mesh = Piece ? Piece->GetSkeletalMeshAsset() : nullptr;
		if (!Mesh)
		{
			continue;
		}
		// A piece wearing an under layer's cut is its whole piece for its front pair (the pair lists that one); with
		// no worn pieces, UnderChoice gives that base back.
		const std::string Path = FT::ToUtf8(FTLookLayers::Canonical(*Mesh));
		const std::string Base = Layers.empty() ? Path : ftcore::look::UnderChoice(Path, Layers, {});
		const USkeletalMesh* BaseMesh = Base == Path ? Mesh : FTLookLayers::UnderLayerMesh(Look, Fronts, FromUtf8(Base));
		const ftcore::look::FrontChoice Choice = ftcore::look::WornChoice(Base, Pairs, Ask);
		if (Choice.pair < 0)
		{
			continue;
		}
		const TArray<TObjectPtr<USkeletalMesh>>& Half = Choice.half == ftcore::look::Front::Open ? Fronts.Open : Fronts.Closed;
		USkeletalMesh* Other = Half.IsValidIndex(Choice.pair) ? Half[Choice.pair].Get() : nullptr;
		// The same half as the base: the under pass below decides between it and its cuts.
		if (!Other || Other == Mesh || Other == BaseMesh)
		{
			continue;
		}
		FTLookLayers::WearMesh(*Piece, *Other, Body, Fronts);
		++Changed;
	}
	// The under pieces follow: each wears the copy the over pieces as now worn call for.
	return Changed + (Layers.empty() ? 0 : WearUnderLayers(Look, Fronts, Body, Pieces, Who));
}

int32 FTLook::WearUnderLayers(const FFTLeadLook& Look, const FFTWornFronts& Fronts, USkeletalMeshComponent& Body,
	const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces, const TCHAR* Who)
{
	const std::vector<ftcore::look::UnderLayer> Layers = FTLookLayers::UnderLayers(Look, Fronts);
	if (Layers.empty())
	{
		return 0;
	}
	// Worn = shown: a piece taken off (SetPieceWorn) is hidden and covers nothing.
	std::vector<std::string> WornNames;
	for (const USkeletalMeshComponent* Piece : Pieces)
	{
		const USkeletalMesh* Mesh = Piece ? Piece->GetSkeletalMeshAsset() : nullptr;
		if (Mesh && Piece->GetVisibleFlag())
		{
			WornNames.push_back(FT::ToUtf8(FTLookLayers::Canonical(*Mesh)));
		}
	}
	const FPieceFilter& Filter = PieceFilter();
	int32 Changed = 0;
	for (USkeletalMeshComponent* Piece : Pieces)
	{
		const USkeletalMesh* Mesh = Piece ? Piece->GetSkeletalMeshAsset() : nullptr;
		if (!Mesh)
		{
			continue;
		}
		const std::string Path = FT::ToUtf8(FTLookLayers::Canonical(*Mesh));
		const std::string Choice = ftcore::look::UnderChoiceLeftOff(Path, Layers, WornNames, Filter.bNoPieces, Filter.Words);
		USkeletalMesh* Other = Choice == Path ? nullptr : FTLookLayers::UnderLayerMesh(Look, Fronts, FromUtf8(Choice));
		if (!Other || Other == Mesh)
		{
			continue;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("%s: under piece %s now wears %s."), Who, *Mesh->GetName(), *Other->GetName());
		FTLookLayers::WearMesh(*Piece, *Other, Body, Fronts);
		++Changed;
	}
	return Changed;
}

int32 FTLook::SetPieceWorn(const FFTLeadLook& Look, const FFTWornFronts& Fronts, USkeletalMeshComponent& Body,
	const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces, USkeletalMeshComponent& Piece, bool bWorn, const TCHAR* Who)
{
	Piece.SetVisibility(bWorn, /*bPropagateToChildren=*/true);
	return WearUnderLayers(Look, Fronts, Body, Pieces, Who);
}

bool FTLook::PieceLeftOff(const FSoftObjectPath& Piece)
{
	const FPieceFilter& Filter = PieceFilter();
	return ftcore::look::PieceLeftOff(FT::ToUtf8(Piece.GetAssetName()), Filter.bNoPieces, Filter.Words);
}

FFTLoadedLook FTLook::LoadLook(const FFTLeadLook& Look, const TCHAR* Who)
{
	FFTLoadedLook Loaded;
	Loaded.Fronts = PrepareFronts(Look, Who);
	Loaded.Pieces = WornPiecePaths(Look, Loaded.Fronts);
	Loaded.Body = LoadOrOwn<USkeletalMesh>(WornBodyPath(Look, Loaded.Pieces, Who), Look.Name, TEXT(""), Who);
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
	{
		// One line per look at setup: each base state and the clip it plays (the clip set shows the body alias at work).
		static const TCHAR* const States[UFTLocomotionAnimInstance::NumBaseClips] = {
			TEXT("idle"), TEXT("walk"), TEXT("run"), TEXT("sprint"), TEXT("jump"), TEXT("fall"), TEXT("land"), TEXT("runjump"), TEXT("runland")};
		FString List;
		for (int32 I = 0; I < UFTLocomotionAnimInstance::NumBaseClips; ++I)
		{
			List += FString::Printf(TEXT("%s%s=%s"), I ? TEXT(", ") : TEXT(""), States[I], Loaded.Clips[I] ? *Loaded.Clips[I]->GetName() : TEXT("(none)"));
		}
		UE_LOG(LogForgottenThird, Display, TEXT("%s: base clips for %s (clip set %s): %s"), Who, *Body.GetName(),
			*UFTLocomotionAnimInstance::ClipSetBody(Body), *List);
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
	int32 Cloth = 0;
	for (const FSoftObjectPath& Path : Paths)
	{
		const FString Name = Path.GetAssetName();
		if (PieceLeftOff(Path))
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

int32 FTLook::SwapSlots(USkeletalMeshComponent& Component, const UMaterialInterface* From, UMaterialInterface* To)
{
	if (!From || !To)
	{
		return 0;
	}
	// A slot shows From when its material is From or an instance made from it (a runtime or per-LOD copy).
	auto Shows = [From](const UMaterialInterface* Material)
	{
		for (int32 Depth = 0; Material && Depth < 8; ++Depth)
		{
			if (Material == From || Material->GetPathName() == From->GetPathName())
			{
				return true;
			}
			const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material);
			Material = Instance ? Instance->Parent.Get() : nullptr;
		}
		return false;
	};
	int32 Slots = 0;
	for (int32 I = 0; I < Component.GetNumMaterials(); ++I)
	{
		if (Shows(Component.GetMaterial(I)))
		{
			Component.SetMaterial(I, To);
			++Slots;
		}
	}
	return Slots;
}

int32 FTLook::SwapMaterials(USkeletalMeshComponent& Body, const TArray<TObjectPtr<USkeletalMeshComponent>>& Pieces,
	const TArray<FFTMaterialSwap>& Swaps, const TCHAR* Who)
{
	int32 Total = 0;
	for (const FFTMaterialSwap& Swap : Swaps)
	{
		const UMaterialInterface* From = Cast<UMaterialInterface>(Swap.From.TryLoad());
		UMaterialInterface* To = Cast<UMaterialInterface>(Swap.To.TryLoad());
		if (!From || !To)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: material swap %s -> %s skipped; %s doesn't load."), Who, *Swap.From.ToString(),
				*Swap.To.ToString(), From ? *Swap.To.ToString() : *Swap.From.ToString());
			continue;
		}
		int32 Slots = SwapSlots(Body, From, To);
		for (USkeletalMeshComponent* Piece : Pieces)
		{
			Slots += Piece ? SwapSlots(*Piece, From, To) : 0;
		}
		if (Slots == 0)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: material swap: no slot shows %s."), Who, *From->GetPathName());
			// What the slots do show, once per component that has slots, so the swap's From can be put right.
			auto List = [Who](const USkeletalMeshComponent* Component)
			{
				FString Names;
				for (int32 I = 0; Component && I < Component->GetNumMaterials(); ++I)
				{
					const UMaterialInterface* Material = Component->GetMaterial(I);
					Names += FString::Printf(TEXT(" [%d]%s"), I, Material ? *Material->GetPathName() : TEXT("none"));
				}
				UE_LOG(LogForgottenThird, Warning, TEXT("%s:   %s slots:%s"), Who,
					Component && Component->GetSkeletalMeshAsset() ? *Component->GetSkeletalMeshAsset()->GetName() : TEXT("none"), *Names);
			};
			List(&Body);
			for (const USkeletalMeshComponent* Piece : Pieces)
			{
				List(Piece);
			}
		}
		else
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s: material swap: %s -> %s in %d slots."), Who, *From->GetName(), *To->GetName(), Slots);
		}
		Total += Slots;
	}
	return Total;
}
