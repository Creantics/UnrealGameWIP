#include "FTMiniMapSource.h"

#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ftcore/Json.h"

#include <string>
#include <vector>

namespace FTMiniMapSourceDetail
{
bool ReadText(const FString& File, std::string& Out, FString& Error)
{
	Out.clear();
	if (File.IsEmpty())
	{
		return true;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		Error = FString::Printf(TEXT("can't read %s"), *File);
		return false;
	}
	Out.assign(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
	return true;
}

struct FHeight
{
	std::vector<uint16_t> Grid;
	size_t SamplesX = 0, SamplesY = 0;
	double Spacing = 0.0, OriginX = 0.0, OriginY = 0.0;
};

bool ReadHeight(const FString& JsonFile, FHeight& Out, FString& Error)
{
	std::string Text;
	if (JsonFile.IsEmpty() || !ReadText(JsonFile, Text, Error))
	{
		return !JsonFile.IsEmpty() ? false : true;
	}
	ftcore::JsonDocument Doc;
	std::string Why;
	if (!Doc.parse(Text, &Why))
	{
		Error = FString::Printf(TEXT("%s: %s"), *JsonFile, UTF8_TO_TCHAR(Why.c_str()));
		return false;
	}
	const ftcore::JsonValue Root = Doc.root();
	Out.SamplesX = static_cast<size_t>(Root["samples_x"].integer(0));
	Out.SamplesY = static_cast<size_t>(Root["samples_y"].integer(0));
	Out.Spacing = Root["spacing_m"].number(0.0);
	Out.OriginX = Root["origin_of_sample_0_0_m"][0u].number(0.0);
	Out.OriginY = Root["origin_of_sample_0_0_m"][1u].number(0.0);
	const std::string Name(Root["file"].string());
	TArray<uint8> Raw;
	if (Out.SamplesX < 2 || Out.SamplesY < 2 || Out.Spacing <= 0.0 || Name.empty() ||
		!FFileHelper::LoadFileToArray(Raw, *FPaths::Combine(FPaths::GetPath(JsonFile), UTF8_TO_TCHAR(Name.c_str()))) ||
		static_cast<size_t>(Raw.Num()) != Out.SamplesX * Out.SamplesY * 2)
	{
		Error = FString::Printf(TEXT("%s: a bad height field"), *JsonFile);
		return false;
	}
	Out.Grid.resize(Out.SamplesX * Out.SamplesY);
	for (size_t I = 0; I < Out.Grid.size(); ++I)
	{
		Out.Grid[I] = static_cast<uint16_t>(Raw[static_cast<int32>(2 * I)] | (Raw[static_cast<int32>(2 * I + 1)] << 8));
	}
	return true;
}
} // namespace FTMiniMapSourceDetail

bool FTMiniMapSource::ParseOptions(const TCHAR* Cmd, const TCHAR* Prefix, FOptions& Out)
{
	const FString P = FString(TEXT("-")) + Prefix;
	FParse::Value(Cmd, *(P + TEXT("Roads=")), Out.Roads);
	FParse::Value(Cmd, *(P + TEXT("Streets=")), Out.Streets);
	if (Out.Roads.IsEmpty() && Out.Streets.IsEmpty())
	{
		return false;
	}
	FParse::Value(Cmd, *(P + TEXT("Boxes=")), Out.Boxes);
	FParse::Value(Cmd, *(P + TEXT("StandInTowers=")), Out.Towers);
	FParse::Value(Cmd, *(P + TEXT("RoadDoors=")), Out.Doors);
	FParse::Value(Cmd, *(P + TEXT("Height=")), Out.Height);
	FParse::Value(Cmd, *(P + TEXT("Sea=")), Out.SeaFromX);
	FParse::Value(Cmd, *(P + TEXT("Side=")), Out.Side);
	FString Area;
	if (FParse::Value(Cmd, TEXT("-MiniMapArea="), Area, false)) // false: the commas are part of the value
	{
		TArray<FString> Parts;
		Area.ParseIntoArray(Parts, TEXT(","));
		Out.bArea = Parts.Num() == 4;
		if (!Out.bArea)
		{
			Out.AreaError = FString::Printf(TEXT("-MiniMapArea=%s isn't x0,y0,x1,y1"), *Area);
		}
		for (int32 I = 0; Out.bArea && I < 4; ++I)
		{
			Out.Area[I] = FCString::Atod(*Parts[I]);
		}
	}
	return true;
}

bool FTMiniMapSource::Build(const FOptions& Options, ftcore::MiniMapModel& Out, FString& Error, FString* Skipped)
{
	using namespace FTMiniMapSourceDetail;
	if (!Options.AreaError.IsEmpty())
	{
		Error = Options.AreaError;
		return false;
	}
	if (Options.Side <= 0.0)
	{
		Error = TEXT("needs the plan side (-Side=, or the level's director)");
		return false;
	}
	std::string RoadsText, StreetsText, BoxesText, TowersText, DoorsText;
	FHeight Field;
	if (!ReadText(Options.Roads, RoadsText, Error) || !ReadText(Options.Streets, StreetsText, Error) ||
		!ReadText(Options.Boxes, BoxesText, Error) || !ReadText(Options.Towers, TowersText, Error) ||
		!ReadText(Options.Doors, DoorsText, Error) || !ReadHeight(Options.Height, Field, Error))
	{
		return false;
	}
	ftcore::MiniMapSource Src;
	Src.roads = RoadsText;
	Src.streets = StreetsText;
	Src.boxes = BoxesText;
	Src.towers = TowersText;
	Src.doors = DoorsText;
	Src.height = Field.Grid.empty() ? nullptr : Field.Grid.data();
	Src.samplesX = Field.SamplesX;
	Src.samplesY = Field.SamplesY;
	Src.spacing = Field.Spacing;
	Src.originX = Field.OriginX;
	Src.originY = Field.OriginY;
	Src.seaFromX = Options.SeaFromX;
	Src.side = Options.Side;
	if (Options.bArea)
	{
		Src.areaX0 = Options.Area[0];
		Src.areaY0 = Options.Area[1];
		Src.areaX1 = Options.Area[2];
		Src.areaY1 = Options.Area[3];
	}
	std::string Why, Left;
	if (!ftcore::buildMiniMap(Src, Out, &Why, &Left))
	{
		Error = UTF8_TO_TCHAR(Why.c_str());
		return false;
	}
	if (Skipped)
	{
		*Skipped = UTF8_TO_TCHAR(Left.c_str());
	}
	return true;
}

FString FTMiniMapSource::Describe(const FOptions& Options)
{
	FString Out;
	for (const FString* File : {&Options.Roads, &Options.Streets, &Options.Boxes, &Options.Towers, &Options.Doors, &Options.Height})
	{
		if (!File->IsEmpty())
		{
			Out += (Out.IsEmpty() ? TEXT("") : TEXT(" ")) + FPaths::GetCleanFilename(*File);
		}
	}
	return Out;
}
