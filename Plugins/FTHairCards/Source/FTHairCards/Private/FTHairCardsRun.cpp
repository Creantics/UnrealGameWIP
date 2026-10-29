#include "FTHairCardsRun.h"

#include "ftcore/HairCardSpec.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "GroomAsset.h"
#include "GroomAssetCards.h"
#include "GroomAssetInterpolation.h"
#include "HairCardGeneratorEditorModule.h"
#include "HairCardGeneratorPluginSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

#include <string>
#include <string_view>
#include <vector>

DEFINE_LOG_CATEGORY(LogFTHairCards);

namespace
{
// FTText.h's helpers: that header is in the game module, which this plugin doesn't build against.
FString ToFString(std::string_view Text)
{
	if (Text.empty())
	{
		return FString();
	}
	const FUTF8ToTCHAR Conv(reinterpret_cast<const UTF8CHAR*>(Text.data()), static_cast<int32>(Text.size()));
	return FString::ConstructFromPtrSize(Conv.Get(), Conv.Length());
}

std::string ToUtf8(const FString& Text)
{
	const FTCHARToUTF8 Conv(*Text, Text.Len());
	return std::string(reinterpret_cast<const char*>(Conv.Get()), static_cast<size_t>(Conv.Length()));
}

// File bytes as UTF-8 text, without a leading byte-order mark.
std::string_view BytesAsText(const TArray<uint8>& Bytes)
{
	std::string_view Text(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
	if (Text.size() >= 3 && Text.substr(0, 3) == "\xEF\xBB\xBF")
	{
		Text.remove_prefix(3);
	}
	return Text;
}

// True when the content path Path is Folder or inside it.
bool InFolder(FString Path, const FString& Folder)
{
	Path.RemoveFromEnd(TEXT("/"));
	return Path.Equals(Folder, ESearchCase::IgnoreCase) || Path.StartsWith(Folder + TEXT("/"), ESearchCase::IgnoreCase);
}

// Tells the groom that one of its properties changed, as the details panel does, so it rebuilds what that property
// feeds. A change without a property rebuilds nothing.
bool NotifyChanged(UGroomAsset* Groom, const FName Name, const EPropertyChangeType::Type Type)
{
	FProperty* Property = UGroomAsset::StaticClass()->FindPropertyByName(Name);
	if (!Property)
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s: the groom has no property %s"), *Groom->GetPathName(), *Name.ToString());
		return false;
	}
	FPropertyChangedEvent Event(Property, Type);
	Groom->PostEditChangeProperty(Event);
	return true;
}

const ftcore::HairCardLod* FindLevel(const ftcore::HairCardGroom& Entry, const int32 Lod)
{
	for (const ftcore::HairCardLod& Level : Entry.lods)
	{
		if (Level.lod == Lod)
		{
			return &Level;
		}
	}
	return nullptr;
}

// The groom's card description for this hair group and level, or INDEX_NONE.
int32 FindCards(const TArray<FHairGroupsCardsSourceDescription>& Descs, const int32 Group, const int32 Lod)
{
	return Descs.IndexOfByPredicate([Group, Lod](const FHairGroupsCardsSourceDescription& Desc)
	{
		return Desc.GroupIndex == Group && Desc.LODIndex == Lod;
	});
}

// Generates one level's cards into Desc in the groom editor's order, and adds the packages that hold them to
// Packages. Desc is a copy: the groom itself is left as it is.
bool BuildLevel(UGroomAsset* Groom, const ftcore::HairCardGroom& Entry, const ftcore::HairCardLod& Level,
	FHairGroupsCardsSourceDescription& Desc, int32& OutCards, TArray<UPackage*>& Packages)
{
	const FString Id = ToFString(Entry.id);
	const FString OutFolder = ToFString(Entry.out);
	FScopedSlowTask Task(5.f, FText::FromString(FString::Printf(TEXT("Hair cards: %s LOD %d"), *Id, Level.lod)));
	Task.MakeDialog();

	Task.EnterProgressFrame();
	FCardsGenerationAdvancedOptions Options;
	// The groom editor's default. The cards then hold every hair group's strands; set to false the generator keeps
	// only the cards whose strands' group id equals the group's index, which keeps none when the ids don't start at 0.
	Options.bGenerateGeometryForAllGroups = true;
	Options.RandomSeed = Entry.seed;
	Options.AtlasSize = static_cast<uint8>(ftcore::hairCardAtlasLog2(Level.atlas));
	Options.DefaultDirectory = OutFolder;
	TObjectPtr<UHairCardGeneratorPluginSettings> Settings;
	uint8 GenFlags = 0;
	uint8 PipelineFlags = 0;
	if (!FHairCardGeneratorUtils::BuildGenerationSettings(false, Groom, Desc, Settings, GenFlags, PipelineFlags, Options) || !Settings)
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s LOD %d: the generator's settings failed"), *Id, Level.lod);
		return false;
	}
	const TStrongObjectPtr<UHairCardGeneratorPluginSettings> KeepSettings(Settings.Get());
	// The generator redoes only what its settings say changed, and it compares them before the per-group targets
	// below are set, so every run rebuilds every stage.
	GenFlags = PipelineFlags = static_cast<uint8>(EHairCardGenerationPipeline::All);

	const FString TexturePath = Settings->GetTextureContentPath();
	if (Settings->BaseFilename.IsEmpty())
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s LOD %d: the generator gave the cards no name"), *Id, Level.lod);
		return false;
	}
	if (!InFolder(Settings->DestinationPath.Path, OutFolder) || !InFolder(TexturePath, OutFolder))
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s LOD %d: the generator would write to %s and %s, outside %s. Its earlier settings for this groom (from this editor session, or saved in Intermediate/GroomHairCardGen/Metadata) are for another folder: delete that metadata, restart the editor and run again."),
			*Id, Level.lod, *Settings->DestinationPath.Path, *TexturePath, *OutFolder);
		return false;
	}

	TArray<TObjectPtr<UHairCardGeneratorGroupSettings>>& Groups = Settings->GetFilterGroupSettings();
	if (Groups.IsEmpty() || Groups.Contains(nullptr))
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s LOD %d: the generator has no strand filter groups"), *Id, Level.lod);
		return false;
	}
	for (const TObjectPtr<UHairCardGeneratorGroupSettings>& Group : Groups)
	{
		Group->TargetNumberOfCards = Level.cards;
		Group->TargetTriangleCount = Level.triangles;
		Group->NumberOfTexturesInAtlas = Level.textures;
		Group->MaxNumberOfFlyaways = Level.flyaways;
	}

	Task.EnterProgressFrame();
	if (!FHairCardGeneratorUtils::LoadGroomStrands(Groom, [](TArray<TArray<FVector>>&) {}))
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s LOD %d: loading the strands failed"), *Id, Level.lod);
		return false;
	}
	Settings->UpdateStrandFilterAssignment();
	Settings->WriteGenerationSettings();
	if (!FHairCardGeneratorUtils::LoadGenerationSettings(Settings))
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s LOD %d: loading the settings into the generator failed"), *Id, Level.lod);
		return false;
	}

	Task.EnterProgressFrame();
	int32 Cards = 0;
	const bool bGenerated = FHairCardGeneratorUtils::RunCardsGeneration(Settings, PipelineFlags,
		[&Cards](const TObjectPtr<const UHairCardGeneratorPluginSettings>& GroupSettings, const int32 FilterIndex, const uint8 Flags)
		{
			FHairCardClumpData Clumps;
			FHairCardGeomData Geometry;
			FHairCardMeshData Mesh;
			FHairCardTextureClusterData Clusters;
			int32 GroupCards = 0;
			if (!FHairCardGeneratorUtils::GenerateCardsClumps(GroupSettings, FilterIndex, Flags, Clumps)
				|| !FHairCardGeneratorUtils::GenerateCardsGeometry(GroupSettings, FilterIndex, Flags, Geometry, Mesh, GroupCards)
				|| !FHairCardGeneratorUtils::GenerateCardsTexturesClusters(GroupSettings, FilterIndex, Flags, Clusters))
			{
				return false;
			}
			Cards += GroupCards;
			return true;
		});
	if (!bGenerated)
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s LOD %d: generating the cards failed"), *Id, Level.lod);
		return false;
	}

	Task.EnterProgressFrame();
	FHairCardAtlasLayoutData Layout;
	FHairCardAtlasUVData AtlasUVs;
	if (!FHairCardGeneratorUtils::GenerateTexturesLayoutAndAtlases(Settings, GenFlags, Layout, AtlasUVs))
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s LOD %d: the texture atlas failed"), *Id, Level.lod);
		return false;
	}

	Task.EnterProgressFrame();
	if (!FHairCardGeneratorUtils::BuildCardsAssets(Groom, Desc, Settings, GenFlags) || !Desc.ImportedMesh)
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s LOD %d: building the card mesh and textures failed"), *Id, Level.lod);
		return false;
	}
	Packages.AddUnique(Desc.ImportedMesh->GetPackage());
	for (const TObjectPtr<UTexture2D>& Texture : Desc.Textures.Textures)
	{
		if (Texture)
		{
			Packages.AddUnique(Texture->GetPackage());
		}
	}
	OutCards = Cards;
	return true;
}

struct FLevelCards
{
	FHairGroupsCardsSourceDescription Desc;
	bool bUseCards = true;
};

// Builds every listed level of one groom, then gives the groom the cards and saves it with them. Nothing is saved
// unless every level builds.
bool BuildGroom(const ftcore::HairCardGroom& Entry, int32& OutLevels, int32& OutCards)
{
	OutLevels = 0;
	OutCards = 0;
	const FString Id = ToFString(Entry.id);
	const FString GroomPath = ToFString(Entry.groom);
	const FString OutFolder = ToFString(Entry.out);
	const TStrongObjectPtr<UGroomAsset> Groom(LoadObject<UGroomAsset>(nullptr, *(GroomPath + TEXT(".") + FPackageName::GetShortName(GroomPath))));
	if (!Groom)
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s: no groom at %s"), *Id, *GroomPath);
		return false;
	}
	const int32 Group = Entry.group;
	if (Group >= Groom->GetNumHairGroups() || Group >= Groom->GetHairGroupsLOD().Num())
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s: the groom has no hair group %d"), *Id, Group);
		return false;
	}
	if (Groom->GetNumHairGroups() > 1)
	{
		UE_LOG(LogFTHairCards, Warning, TEXT("%s: the groom has %d hair groups; group %d gets cards made from all of them, and the others keep drawing their strands (docs/HAIR_CARDS.md)"),
			*Id, Groom->GetNumHairGroups(), Group);
	}
	Groom->Modify();

	// Levels the groom doesn't have yet are added as the groom editor's Add LOD adds them, and listed levels take the
	// spec's screen sizes, before the generator reads the groom.
	bool bLodsChanged = false;
	{
		TArray<FHairLODSettings>& Lods = Groom->GetHairGroupsLOD()[Group].LODs;
		for (int32 L = 0; L <= Entry.lods.back().lod; ++L)
		{
			if (L >= Lods.Num())
			{
				FHairLODSettings& Added = Lods.AddDefaulted_GetRef();
				if (Lods.Num() > 1)
				{
					const FHairLODSettings& Prev = Lods[Lods.Num() - 2];
					Added.VertexDecimation = Prev.VertexDecimation * 0.5f;
					Added.AngularThreshold = Prev.AngularThreshold * 2.f;
					Added.CurveDecimation = Prev.CurveDecimation * 0.5f;
					Added.ScreenSize = Prev.ScreenSize * 0.5f;
					Added.GeometryType = Prev.GeometryType;
				}
				bLodsChanged = true;
			}
			const ftcore::HairCardLod* Level = FindLevel(Entry, L);
			if (Level && Level->screenSize > 0.f && !FMath::IsNearlyEqual(Lods[L].ScreenSize, Level->screenSize))
			{
				Lods[L].ScreenSize = Level->screenSize;
				bLodsChanged = true;
			}
		}
	}
	if (bLodsChanged && !NotifyChanged(Groom.Get(), UGroomAsset::GetHairGroupsLODMemberName(), EPropertyChangeType::ValueSet))
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s: left unsaved"), *Id);
		return false;
	}

	TArray<FLevelCards> Built;
	TArray<UPackage*> Packages;
	for (const ftcore::HairCardLod& Level : Entry.lods)
	{
		const TArray<FHairGroupsCardsSourceDescription>& Existing = Groom->GetHairGroupsCards();
		FLevelCards Made;
		if (const int32 Index = FindCards(Existing, Group, Level.lod); Index != INDEX_NONE)
		{
			Made.Desc = Existing[Index];
		}
		Made.Desc.GroupIndex = Group;
		Made.Desc.LODIndex = Level.lod;
		// The level keeps its material slot and options. What the generator makes starts empty: an earlier mesh would
		// send the output to that mesh's folder and could skip the level as up to date.
		Made.Desc.Textures = FHairGroupCardsTextures();
		Made.Desc.ImportedMesh = nullptr;
		Made.Desc.GenerationSettings = nullptr;
		int32 LevelCards = 0;
		if (!BuildLevel(Groom.Get(), Entry, Level, Made.Desc, LevelCards, Packages))
		{
			UE_LOG(LogFTHairCards, Error, TEXT("%s: left unsaved"), *Id);
			return false;
		}
		UE_LOG(LogFTHairCards, Display, TEXT("%s LOD %d: %d cards"), *Id, Level.lod, LevelCards);
		Made.bUseCards = Level.useCards;
		OutCards += LevelCards;
		Built.Add(MoveTemp(Made));
	}

	// Every level built: the groom takes the cards, and the levels that use them draw them.
	{
		TArray<FHairGroupsCardsSourceDescription>& Descs = Groom->GetHairGroupsCards();
		TArray<FHairLODSettings>& Lods = Groom->GetHairGroupsLOD()[Group].LODs;
		for (FLevelCards& Made : Built)
		{
			const int32 Lod = Made.Desc.LODIndex;
			if (const int32 Index = FindCards(Descs, Group, Lod); Index != INDEX_NONE)
			{
				Descs[Index] = MoveTemp(Made.Desc);
			}
			else
			{
				Descs.Add(MoveTemp(Made.Desc));
			}
			if (Made.bUseCards && Lods.IsValidIndex(Lod))
			{
				Lods[Lod].GeometryType = EGroomGeometryType::Cards;
			}
		}
	}
	if (!NotifyChanged(Groom.Get(), UGroomAsset::GetHairGroupsCardsMemberName(), EPropertyChangeType::ValueSet))
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s: left unsaved"), *Id);
		return false;
	}

	Packages.AddUnique(Groom->GetPackage());
	TArray<UPackage*> Dirty;
	UEditorLoadingAndSavingUtils::GetDirtyContentPackages(Dirty);
	for (UPackage* Package : Dirty)
	{
		if (Package && InFolder(Package->GetName(), OutFolder))
		{
			Packages.AddUnique(Package);
		}
	}
	if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, false))
	{
		UE_LOG(LogFTHairCards, Error, TEXT("%s: saving failed"), *Id);
		return false;
	}
	OutLevels = Built.Num();
	UE_LOG(LogFTHairCards, Display, TEXT("%s: %d levels, %d cards, %d packages saved"), *Id, OutLevels, OutCards, Packages.Num());
	return true;
}
} // namespace

namespace FTHairCards
{
bool IsGeneratorReady()
{
	// As the generator finds its controller.
	UClass* Controller = FindObject<UClass>(nullptr, TEXT("/Script/HairCardGeneratorEditor.HairCardGenControllerBase"));
	if (!Controller)
	{
		return false;
	}
	TArray<UObject*> Found;
	GetObjectsOfClass(Controller, Found);
	return Found.Num() > 0;
}

int32 Run(const FString& SpecPath, const FString& Only)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), SpecPath);
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogFTHairCards, Error, TEXT("FT.HairCards: can't read %s"), *Path);
		return -1;
	}
	ftcore::HairCardSpec Spec;
	std::string Reason;
	std::vector<std::string> Notes;
	const bool bAccepted = ftcore::importHairCardSpec(BytesAsText(Bytes), Spec, &Reason, &Notes);
	for (const std::string& Note : Notes)
	{
		UE_LOG(LogFTHairCards, Warning, TEXT("FT.HairCards: %s: %s"), *Path, *ToFString(Note));
	}
	if (!bAccepted)
	{
		UE_LOG(LogFTHairCards, Error, TEXT("FT.HairCards: %s refused: %s"), *Path, *ToFString(Reason).TrimEnd());
		return -1;
	}
	const std::string OnlyId = ToUtf8(Only);
	if (!Only.IsEmpty() && Spec.find(OnlyId) < 0)
	{
		UE_LOG(LogFTHairCards, Error, TEXT("FT.HairCards: %s lists no groom %s"), *Path, *Only);
		return -1;
	}

	int32 Built = 0;
	int32 Failed = 0;
	int32 Levels = 0;
	int32 Cards = 0;
	for (const ftcore::HairCardGroom& Entry : Spec.grooms)
	{
		if (!Only.IsEmpty() && Entry.id != OnlyId)
		{
			continue;
		}
		int32 GroomLevels = 0;
		int32 GroomCards = 0;
		if (BuildGroom(Entry, GroomLevels, GroomCards))
		{
			++Built;
			Levels += GroomLevels;
			Cards += GroomCards;
		}
		else
		{
			++Failed;
		}
	}
	UE_LOG(LogFTHairCards, Display, TEXT("FT.HairCards: %d grooms built (%d levels, %d cards), %d failed"), Built, Levels, Cards, Failed);
	return Failed;
}
} // namespace FTHairCards
