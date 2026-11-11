#include "FTMiniMapSource.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ftcore/Json.h"

#include <string>
#include <vector>

namespace FTMiniMapSourceDetail
{
// The revision in a table's name, times 100: the number after its last "_r", plus a letter right after it as a
// sub-revision (sol_strip_boxes_r10.csv is 1000, _r10w.csv 1023, so r10w wins over r10); 0 when none.
int32 Revision(const FString& Name)
{
	int32 Best = 0;
	for (int32 At = Name.Find(TEXT("_r")); At != INDEX_NONE; At = Name.Find(TEXT("_r"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 2))
	{
		int32 N = 0, I = At + 2;
		for (; I < Name.Len() && FChar::IsDigit(Name[I]); ++I)
		{
			N = N * 10 + (Name[I] - TEXT('0'));
		}
		if (I > At + 2)
		{
			const int32 Sub = I < Name.Len() && FChar::IsLower(Name[I]) ? 1 + (Name[I] - TEXT('a')) : 0;
			Best = N * 100 + Sub;
		}
	}
	return Best;
}

// The file in Dir whose name holds Part (and none of Not) at the highest revision (a folder keeps a layout's past
// revisions beside the new ones); empty when there is none, an error when two share the top revision.
bool FindOne(const FString& Dir, const TCHAR* Ext, const TCHAR* Part, const TCHAR* Not, int32 MaxRev, FString& Out, FString& Error)
{
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *FPaths::Combine(Dir, FString(TEXT("*")) + Ext), true, false);
	Out.Reset();
	int32 Top = -1;
	bool bTie = false;
	for (const FString& Name : Names)
	{
		if (!Name.Contains(Part) || (Not && Name.Contains(Not)))
		{
			continue;
		}
		const int32 Rev = Revision(Name);
		if (Rev / 100 > MaxRev)
		{
			continue;
		}
		if (Rev == Top)
		{
			bTie = true;
		}
		else if (Rev > Top)
		{
			Top = Rev;
			bTie = false;
			Out = FPaths::Combine(Dir, Name);
		}
	}
	if (bTie)
	{
		Error = FString::Printf(TEXT("more than one %s%s table at revision %d.%d in %s"), Part, Ext, Top / 100, Top % 100, *Dir);
		return false;
	}
	return true;
}

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
	const FString P(Prefix);
	FString Area;
	if (!FParse::Value(Cmd, *(P + TEXT("Tables=")), Out.Dir) || !FParse::Value(Cmd, *(P + TEXT("Area=")), Area, false))
	{
		return false;
	}
	TArray<FString> Parts;
	Area.ParseIntoArray(Parts, TEXT(","));
	if (Parts.Num() != 4)
	{
		return false;
	}
	for (int32 I = 0; I < 4; ++I)
	{
		Out.Area[I] = FCString::Atod(*Parts[I]);
	}
	FParse::Value(Cmd, *(P + TEXT("Sea=")), Out.SeaFromX);
	FParse::Value(Cmd, *(P + TEXT("Side=")), Out.Side);
	FParse::Value(Cmd, *(P + TEXT("Rev=")), Out.MaxRev);
	return true;
}

bool FTMiniMapSource::Build(const FOptions& Options, ftcore::MiniMapModel& Out, FString& Error)
{
	using namespace FTMiniMapSourceDetail;
	FString Roads, Streets, Boxes, Towers, Doors, Height;
	if (!FindOne(Options.Dir, TEXT(".csv"), TEXT("_roads_"), TEXT("roadgraph"), Options.MaxRev, Roads, Error) ||
		!FindOne(Options.Dir, TEXT(".csv"), TEXT("_streets_"), TEXT("prop_streets"), Options.MaxRev, Streets, Error) ||
		!FindOne(Options.Dir, TEXT(".csv"), TEXT("_boxes_"), nullptr, Options.MaxRev, Boxes, Error) ||
		!FindOne(Options.Dir, TEXT(".csv"), TEXT("towers"), nullptr, Options.MaxRev, Towers, Error) ||
		!FindOne(Options.Dir, TEXT(".csv"), TEXT("_doors_"), nullptr, Options.MaxRev, Doors, Error) ||
		!FindOne(Options.Dir, TEXT(".json"), TEXT(""), nullptr, Options.MaxRev, Height, Error))
	{
		return false;
	}
	std::string RoadsText, StreetsText, BoxesText, TowersText, DoorsText;
	FHeight Field;
	if (!ReadText(Roads, RoadsText, Error) || !ReadText(Streets, StreetsText, Error) || !ReadText(Boxes, BoxesText, Error) ||
		!ReadText(Towers, TowersText, Error) || !ReadText(Doors, DoorsText, Error) || !ReadHeight(Height, Field, Error))
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
	Src.areaX0 = Options.Area[0];
	Src.areaY0 = Options.Area[1];
	Src.areaX1 = Options.Area[2];
	Src.areaY1 = Options.Area[3];
	std::string Why;
	if (!ftcore::buildMiniMap(Src, Out, &Why))
	{
		Error = UTF8_TO_TCHAR(Why.c_str());
		return false;
	}
	return true;
}
