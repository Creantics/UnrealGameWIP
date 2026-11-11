#include "FTBuildMiniMapCommandlet.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "FTExploreDirector.h"
#include "FTMiniMapSource.h"
#include "ForgottenThird.h"

#include "ftcore/MiniMap.h"

#include <vector>

UFTBuildMiniMapCommandlet::UFTBuildMiniMapCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildMiniMapCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MapPath;
	if (!FParse::Value(*Params, TEXT("Map="), MapPath) || !FPackageName::IsValidLongPackageName(MapPath))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniMap: needs -Map=/Game/Maps/<map>."));
		return 1;
	}
	UPackage* Package = LoadPackage(nullptr, *MapPath, LOAD_None);
	UWorld* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
	if (!World || !World->PersistentLevel)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniMap: couldn't load %s."), *MapPath);
		return 1;
	}
	AFTExploreDirector* Director = nullptr;
	for (AActor* Actor : World->PersistentLevel->Actors)
	{
		if (AFTExploreDirector* Found = Cast<AFTExploreDirector>(Actor))
		{
			Director = Found;
			break;
		}
	}
	if (!Director)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniMap: %s has no exploration director."), *MapPath);
		return 1;
	}
	FTMiniMapSource::FOptions Options;
	Options.Side = Director->PlanSide;
	if (!FTMiniMapSource::ParseOptions(*Params, TEXT(""), Options))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniMap: needs -Roads=<csv> and/or -Streets=<csv> (the world build's tables)."));
		return 1;
	}
	ftcore::MiniMapModel Model;
	FString Error, Skipped;
	if (!FTMiniMapSource::Build(Options, Model, Error, &Skipped))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniMap: %s"), *Error);
		return 1;
	}
	if (!Skipped.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMiniMap: kinds left off the map: %s"), *Skipped);
	}
	std::vector<std::uint8_t> Blob;
	ftcore::saveMiniMap(Model, Blob);
	Director->Modify();
	Director->MiniMapBlob.SetNumUninitialized(static_cast<int32>(Blob.size()));
	FMemory::Memcpy(Director->MiniMapBlob.GetData(), Blob.data(), Blob.size());
	Package->MarkPackageDirty();

	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, World, *File, Save))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniMap: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMiniMap: %s from %s: roads=%d buildings=%d doors=%d blob=%d bytes, saved %s."), *MapPath,
		*FTMiniMapSource::Describe(Options),
		static_cast<int32>(Model.roads.size()), static_cast<int32>(Model.buildings.size()), static_cast<int32>(Model.doors.size()),
		Director->MiniMapBlob.Num(), *File);
	return 0;
#else
	return 1;
#endif
}
