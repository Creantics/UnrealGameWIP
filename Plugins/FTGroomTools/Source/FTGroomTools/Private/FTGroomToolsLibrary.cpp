#include "FTGroomToolsLibrary.h"

#include "GroomAsset.h"
#include "HairDescription.h"
#include "Misc/FileHelper.h"
#include "Serialization/MemoryReader.h"
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

FString UFTGroomToolsLibrary::ExportGroomColors(UGroomAsset* Groom, const FString& Path)
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

	const TVertexAttributesConstRef<FVector3f> Colors = Desc.VertexAttributes().GetAttributesRef<FVector3f>(HairAttribute::Vertex::Color);
	const TVertexAttributesConstRef<float> Roughness = Desc.VertexAttributes().GetAttributesRef<float>(HairAttribute::Vertex::Roughness);
	const TVertexAttributesConstRef<float> AO = Desc.VertexAttributes().GetAttributesRef<float>(HairAttribute::Vertex::AO);
	const TStrandAttributesConstRef<FVector2f> RootUVs = Desc.StrandAttributes().GetAttributesRef<FVector2f>(HairAttribute::Strand::RootUV);

	int32 NumVertices = Desc.GetNumVertices();
	int32 NumStrands = Desc.GetNumStrands();
	TArray<uint8> Bytes;
	Bytes.Reserve(32 + NumVertices * 20 + NumStrands * 8);
	FMemoryWriter Ar(Bytes);
	ANSICHAR Magic[4] = {'F', 'T', 'G', 'C'};
	Ar.Serialize(Magic, 4);
	int32 Version = 2;
	int32 HasColor = Colors.IsValid(), HasRoughness = Roughness.IsValid(), HasAO = AO.IsValid(), HasRootUV = RootUVs.IsValid();
	Ar << Version << NumVertices << HasColor << HasRoughness << HasAO << NumStrands << HasRootUV;

	FVector3f Lo(TNumericLimits<float>::Max()), Hi(TNumericLimits<float>::Lowest());
	for (int32 I = 0; I < NumVertices; ++I)
	{
		const FVertexID Id(I);
		FVector3f C = HasColor ? Colors[Id] : FVector3f::ZeroVector;
		float R = HasRoughness ? Roughness[Id] : 0.f;
		float O = HasAO ? AO[Id] : 0.f;
		Ar << C.X << C.Y << C.Z << R << O;
		Lo = FVector3f::Min(Lo, C);
		Hi = FVector3f::Max(Hi, C);
	}
	for (int32 I = 0; I < NumStrands; ++I)
	{
		FVector2f UV = HasRootUV ? RootUVs[FStrandID(I)] : FVector2f::ZeroVector;
		Ar << UV.X << UV.Y;
	}
	if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
	{
		UE_LOG(LogFTGroom, Warning, TEXT("could not write %s"), *Path);
		return FString();
	}
	return FString::Printf(TEXT("{\"vertices\": %d, \"strands\": %d, \"color\": %s, \"roughness\": %s, \"ao\": %s, \"root_uv\": %s, \"color_min\": [%g, %g, %g], \"color_max\": [%g, %g, %g]}"),
		NumVertices, NumStrands, HasColor ? TEXT("true") : TEXT("false"), HasRoughness ? TEXT("true") : TEXT("false"), HasAO ? TEXT("true") : TEXT("false"),
		HasRootUV ? TEXT("true") : TEXT("false"),
		NumVertices ? Lo.X : 0.f, NumVertices ? Lo.Y : 0.f, NumVertices ? Lo.Z : 0.f, NumVertices ? Hi.X : 0.f, NumVertices ? Hi.Y : 0.f, NumVertices ? Hi.Z : 0.f);
#else
	return FString();
#endif
}

FString UFTGroomToolsLibrary::SetGroomColors(UGroomAsset* Groom, const FString& Path)
{
#if WITH_EDITORONLY_DATA
	if (!Groom)
	{
		return FString();
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 24 || FMemory::Memcmp(Bytes.GetData(), "FTGC", 4) != 0)
	{
		UE_LOG(LogFTGroom, Warning, TEXT("%s is missing or not a colour file"), *Path);
		return FString();
	}
	FMemoryReader Ar(Bytes);
	Ar.Seek(4);
	int32 Version = 0, NumVertices = 0, HasColor = 0, HasRoughness = 0, HasAO = 0, NumStrands = 0, HasRootUV = 0;
	Ar << Version << NumVertices << HasColor << HasRoughness << HasAO;
	if (Version == 2 && Bytes.Num() >= 32)
	{
		Ar << NumStrands << HasRootUV;
	}

	FHairDescription Desc = Groom->GetHairDescription();
	if (!Desc.IsValid())
	{
		UE_LOG(LogFTGroom, Warning, TEXT("%s has no hair description"), *Groom->GetPathName());
		return FString();
	}
	const int64 Expected = Version == 1 ? 24 + int64(NumVertices) * 20 : 32 + int64(NumVertices) * 20 + int64(NumStrands) * 8;
	if ((Version != 1 && Version != 2) || NumVertices != Desc.GetNumVertices() || (Version == 2 && NumStrands != Desc.GetNumStrands())
		|| Bytes.Num() != Expected)
	{
		UE_LOG(LogFTGroom, Warning, TEXT("%s: version %d with %d vertices, %d strands (%d bytes) doesn't fit %s's %d vertices, %d strands"),
			*Path, Version, NumVertices, NumStrands, Bytes.Num(), *Groom->GetPathName(), Desc.GetNumVertices(), Desc.GetNumStrands());
		return FString();
	}
	if (!HasColor && !HasRoughness && !HasAO && !HasRootUV)
	{
		return FString();
	}

	TAttributesSet<FVertexID>& Attributes = Desc.VertexAttributes();
	if (HasColor && !Attributes.GetAttributesRef<FVector3f>(HairAttribute::Vertex::Color).IsValid())
	{
		Attributes.RegisterAttribute<FVector3f>(HairAttribute::Vertex::Color);
	}
	if (HasRoughness && !Attributes.GetAttributesRef<float>(HairAttribute::Vertex::Roughness).IsValid())
	{
		Attributes.RegisterAttribute<float>(HairAttribute::Vertex::Roughness);
	}
	if (HasAO && !Attributes.GetAttributesRef<float>(HairAttribute::Vertex::AO).IsValid())
	{
		Attributes.RegisterAttribute<float>(HairAttribute::Vertex::AO);
	}
	TVertexAttributesRef<FVector3f> Colors = Attributes.GetAttributesRef<FVector3f>(HairAttribute::Vertex::Color);
	TVertexAttributesRef<float> Roughness = Attributes.GetAttributesRef<float>(HairAttribute::Vertex::Roughness);
	TVertexAttributesRef<float> AO = Attributes.GetAttributesRef<float>(HairAttribute::Vertex::AO);
	for (int32 I = 0; I < NumVertices; ++I)
	{
		const FVertexID Id(I);
		FVector3f C;
		float R, O;
		Ar << C.X << C.Y << C.Z << R << O;
		if (HasColor)
		{
			Colors[Id] = C;
		}
		if (HasRoughness)
		{
			Roughness[Id] = R;
		}
		if (HasAO)
		{
			AO[Id] = O;
		}
	}
	if (HasRootUV)
	{
		TAttributesSet<FStrandID>& StrandAttributes = Desc.StrandAttributes();
		if (!StrandAttributes.GetAttributesRef<FVector2f>(HairAttribute::Strand::RootUV).IsValid())
		{
			StrandAttributes.RegisterAttribute<FVector2f>(HairAttribute::Strand::RootUV);
		}
		TStrandAttributesRef<FVector2f> RootUVs = StrandAttributes.GetAttributesRef<FVector2f>(HairAttribute::Strand::RootUV);
		for (int32 I = 0; I < NumStrands; ++I)
		{
			FVector2f UV;
			Ar << UV.X << UV.Y;
			RootUVs[FStrandID(I)] = UV;
		}
	}

	// Commit back to the description the groom is using (Source or Edit; the getter is private, the property is not),
	// then the same order as the engine's groom edit path: rebuild the derived data, tell the components
	EHairDescriptionType Type = EHairDescriptionType::Source;
	if (const FProperty* TypeProp = UGroomAsset::StaticClass()->FindPropertyByName(TEXT("HairDescriptionType")))
	{
		Type = static_cast<EHairDescriptionType>(*TypeProp->ContainerPtrToValuePtr<uint8>(Groom));
	}
	Groom->Modify();
	Groom->CommitHairDescription(MoveTemp(Desc), Type);
	const bool bBuilt = Groom->CacheDerivedDatas();
	Groom->GetOnGroomAssetChanged().Broadcast();
	Groom->MarkPackageDirty();
	return FString::Printf(TEXT("{\"vertices\": %d, \"strands\": %d, \"color\": %s, \"roughness\": %s, \"ao\": %s, \"root_uv\": %s, \"built\": %s}"),
		NumVertices, NumStrands, HasColor ? TEXT("true") : TEXT("false"), HasRoughness ? TEXT("true") : TEXT("false"), HasAO ? TEXT("true") : TEXT("false"),
		HasRootUV ? TEXT("true") : TEXT("false"), bBuilt ? TEXT("true") : TEXT("false"));
#else
	return FString();
#endif
}
