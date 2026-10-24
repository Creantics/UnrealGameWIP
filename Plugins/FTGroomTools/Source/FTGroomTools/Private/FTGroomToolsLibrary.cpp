#include "FTGroomToolsLibrary.h"

#include "GroomAsset.h"
#include "HairDescription.h"
#include "Misc/FileHelper.h"
#include "Serialization/MemoryWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogFTGroom, Log, All);

FString UFTGroomToolsLibrary::ExportGroomStrands(UGroomAsset* Groom, const FString& Path)
{
#if WITH_EDITORONLY_DATA
	if (!Groom)
	{
		return FString();
	}
	const FHairDescription Desc = Groom->GetHairDescription();
	if (!Desc.IsValid())
	{
		UE_LOG(LogFTGroom, Warning, TEXT("%s has no hair description"), *Groom->GetPathName());
		return FString();
	}

	const TVertexAttributesConstRef<FVector3f> Positions = Desc.VertexAttributes().GetAttributesRef<FVector3f>(HairAttribute::Vertex::Position);
	const TVertexAttributesConstRef<float> VertexWidths = Desc.VertexAttributes().GetAttributesRef<float>(HairAttribute::Vertex::Width);
	const TStrandAttributesConstRef<int> VertexCounts = Desc.StrandAttributes().GetAttributesRef<int>(HairAttribute::Strand::VertexCount);
	const TStrandAttributesConstRef<int> GroupIDs = Desc.StrandAttributes().GetAttributesRef<int>(HairAttribute::Strand::GroupID);
	const TStrandAttributesConstRef<int> Guides = Desc.StrandAttributes().GetAttributesRef<int>(HairAttribute::Strand::Guide);
	const TStrandAttributesConstRef<FVector2f> RootUVs = Desc.StrandAttributes().GetAttributesRef<FVector2f>(HairAttribute::Strand::RootUV);
	const TStrandAttributesConstRef<float> StrandWidths = Desc.StrandAttributes().GetAttributesRef<float>(HairAttribute::Strand::Width);
	const TStrandAttributesConstRef<FName> GroupNames = Desc.StrandAttributes().GetAttributesRef<FName>(HairAttribute::Strand::GroupName);
	if (!Positions.IsValid() || !VertexCounts.IsValid())
	{
		UE_LOG(LogFTGroom, Warning, TEXT("%s has no positions or vertex counts"), *Groom->GetPathName());
		return FString();
	}

	int32 NumStrands = Desc.GetNumStrands();
	int32 NumVertices = Desc.GetNumVertices();
	TArray<uint8> Bytes;
	Bytes.Reserve(16 + NumStrands * 24 + NumVertices * 16);
	FMemoryWriter Ar(Bytes);
	ANSICHAR Magic[4] = {'F', 'T', 'G', 'S'};
	Ar.Serialize(Magic, 4);
	int32 Version = 1;
	Ar << Version << NumStrands << NumVertices;

	TMap<int32, FName> Groups;
	int32 NumGuides = 0;
	int64 Counted = 0;
	for (int32 S = 0; S < NumStrands; ++S)
	{
		const FStrandID Id(S);
		int32 Count = VertexCounts[Id];
		int32 Group = GroupIDs.IsValid() ? GroupIDs[Id] : -1;
		int32 Guide = Guides.IsValid() ? Guides[Id] : 0;
		const FVector2f UV = RootUVs.IsValid() ? RootUVs[Id] : FVector2f::ZeroVector;
		float U = UV.X, V = UV.Y;
		float Width = StrandWidths.IsValid() ? StrandWidths[Id] : 0.f;
		Ar << Count << Group << Guide << U << V << Width;
		if (!Groups.Contains(Group))
		{
			Groups.Add(Group, GroupNames.IsValid() ? GroupNames[Id] : NAME_None);
		}
		NumGuides += Guide != 0;
		Counted += Count;
	}
	if (Counted != NumVertices)
	{
		UE_LOG(LogFTGroom, Warning, TEXT("%s: strand vertex counts sum to %lld, not %d"), *Groom->GetPathName(), Counted, NumVertices);
		return FString();
	}
	for (int32 I = 0; I < NumVertices; ++I)
	{
		const FVertexID Id(I);
		FVector3f P = Positions[Id];
		float Width = VertexWidths.IsValid() ? VertexWidths[Id] : 0.f;
		Ar << P.X << P.Y << P.Z << Width;
	}
	if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
	{
		UE_LOG(LogFTGroom, Warning, TEXT("could not write %s"), *Path);
		return FString();
	}

	FString GroupList;
	for (const TPair<int32, FName>& G : Groups)
	{
		GroupList += FString::Printf(TEXT("%s\"%d\": \"%s\""), GroupList.IsEmpty() ? TEXT("") : TEXT(", "), G.Key, *G.Value.ToString());
	}
	return FString::Printf(TEXT("{\"strands\": %d, \"vertices\": %d, \"guides\": %d, \"root_uv\": %s, \"vertex_width\": %s, \"strand_width\": %s, \"groups\": {%s}}"),
		NumStrands, NumVertices, NumGuides, RootUVs.IsValid() ? TEXT("true") : TEXT("false"), VertexWidths.IsValid() ? TEXT("true") : TEXT("false"),
		StrandWidths.IsValid() ? TEXT("true") : TEXT("false"), *GroupList);
#else
	return FString();
#endif
}
