
#include "FTDnaCommandlet.h"

#include "DNACalibDNAReader.h"
#include "DNAReader.h"
#include "DNAUtils.h"
#include "Commands/DNACalibCalculateMeshLowerLODsCommand.h"
#include "Commands/DNACalibSetVertexPositionsCommand.h"
#include "Engine/SkeletalMesh.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogFTDna, Log, All);

UFTDnaCommandlet::UFTDnaCommandlet()
{
	IsClient = false;
	IsEditor = true;
	LogToConsole = true;
}

namespace
{
	void DumpMesh(const IDNAReader& Reader, uint16 Mesh, const FString& Path)
	{
		const TArrayView<const float> Xs = Reader.GetVertexPositionXs(Mesh);
		const TArrayView<const float> Ys = Reader.GetVertexPositionYs(Mesh);
		const TArrayView<const float> Zs = Reader.GetVertexPositionZs(Mesh);
		const FCoordinateSystem Axes = Reader.GetCoordinateSystem();

		TArray<FString> Lines;
		Lines.Reserve(Xs.Num() + 4);
		Lines.Add(FString::Printf(TEXT("# mesh %d '%s' verts %d lods %d meshes %d"), Mesh, *Reader.GetMeshName(Mesh), Xs.Num(),
			Reader.GetLODCount(), Reader.GetMeshCount()));
		Lines.Add(FString::Printf(TEXT("# axes x %d y %d z %d unit %d"), static_cast<int32>(Axes.XAxis), static_cast<int32>(Axes.YAxis),
			static_cast<int32>(Axes.ZAxis), static_cast<int32>(Reader.GetTranslationUnit())));
		for (uint16 Lod = 0; Lod < Reader.GetLODCount(); ++Lod)
		{
			FString Row = FString::Printf(TEXT("# lod %d meshes"), Lod);
			for (const uint16 M : Reader.GetMeshIndicesForLOD(Lod))
			{
				Row += FString::Printf(TEXT(" %d:%d"), M, Reader.GetVertexPositionCount(M));
			}
			Lines.Add(Row);
		}
		for (int32 I = 0; I < Xs.Num(); ++I)
		{
			Lines.Add(FString::Printf(TEXT("v %.6f %.6f %.6f"), Xs[I], Ys[I], Zs[I]));
		}
		FFileHelper::SaveStringArrayToFile(Lines, *Path);
	}

	// All vertex positions of one DNA LOD, meshes concatenated in LOD order, in Unreal axes (DNA x, z, y).
	TArray<FVector3f> LodPositions(const IDNAReader& Reader, uint16 Lod)
	{
		TArray<FVector3f> Out;
		for (const uint16 M : Reader.GetMeshIndicesForLOD(Lod))
		{
			const TArrayView<const float> Xs = Reader.GetVertexPositionXs(M);
			const TArrayView<const float> Ys = Reader.GetVertexPositionYs(M);
			const TArrayView<const float> Zs = Reader.GetVertexPositionZs(M);
			for (int32 I = 0; I < Xs.Num(); ++I)
			{
				Out.Emplace(Xs[I], Zs[I], Ys[I]);
			}
		}
		return Out;
	}

	// Rewrites each LOD of a face skeletal mesh built from Orig with the positions of the matching LOD in New.
	// A mesh LOD matches the DNA LOD whose concatenated vertices equal its own, in order, so joints, skin
	// weights, materials and the attached rig are left as they are.
	int32 WriteSkeletalMesh(const FString& MeshPath, const IDNAReader& Orig, const IDNAReader& New, bool bDry)
	{
		USkeletalMesh* Skm = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
		if (!Skm)
		{
			UE_LOG(LogFTDna, Error, TEXT("FTDna: cannot load skeletal mesh %s"), *MeshPath);
			return 1;
		}
		const int32 LodCount = Skm->GetLODNum();
		for (int32 L = 0; L < LodCount; ++L)
		{
			FMeshDescription* Desc = Skm->GetMeshDescription(L);
			if (!Desc)
			{
				UE_LOG(LogFTDna, Error, TEXT("FTDna: LOD %d has no mesh description"), L);
				return 1;
			}
			TVertexAttributesRef<FVector3f> Pos = Desc->GetVertexPositions();
			TArray<FVertexID> Ids;
			for (const FVertexID Id : Desc->Vertices().GetElementIDs())
			{
				Ids.Add(Id);
			}
			int32 Match = -1;
			float MatchErr = 0.f;
			for (uint16 D = 0; D < Orig.GetLODCount() && Match < 0; ++D)
			{
				const TArray<FVector3f> Src = LodPositions(Orig, D);
				if (Src.Num() != Ids.Num())
				{
					continue;
				}
				float Err = 0.f;
				for (int32 I = 0; I < Ids.Num(); ++I)
				{
					Err = FMath::Max(Err, FVector3f::Distance(Src[I], Pos[Ids[I]]));
				}
				if (Err < 1e-3f)
				{
					Match = D;
					MatchErr = Err;
				}
			}
			if (Match < 0)
			{
				UE_LOG(LogFTDna, Error, TEXT("FTDna: LOD %d (%d verts) matches no DNA LOD in order"), L, Ids.Num());
				return 1;
			}
			const TArray<FVector3f> Dst = LodPositions(New, static_cast<uint16>(Match));
			if (Dst.Num() != Ids.Num())
			{
				UE_LOG(LogFTDna, Error, TEXT("FTDna: new DNA LOD %d has %d verts, mesh LOD %d has %d"), Match, Dst.Num(), L, Ids.Num());
				return 1;
			}
			float MaxMove = 0.f;
			int32 Moved = 0;
			for (int32 I = 0; I < Ids.Num(); ++I)
			{
				const float Move = FVector3f::Distance(Dst[I], Pos[Ids[I]]);
				MaxMove = FMath::Max(MaxMove, Move);
				Moved += Move > 0.01f;
				Pos[Ids[I]] = Dst[I];
			}
			UE_LOG(LogFTDna, Display, TEXT("FTDna: mesh LOD %d = DNA LOD %d (%d verts, match err %g), moved %d, max %.3f"), L, Match,
				Ids.Num(), MatchErr, Moved, MaxMove);
			if (!bDry)
			{
				Skm->CommitMeshDescription(L);
			}
		}
		if (bDry)
		{
			return 0;
		}
		Skm->PostEditChange();
		UPackage* Package = Skm->GetPackage();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, Skm, *File, Args))
		{
			UE_LOG(LogFTDna, Error, TEXT("FTDna: saving %s failed"), *File);
			return 1;
		}
		UE_LOG(LogFTDna, Display, TEXT("FTDna: saved %s"), *File);
		return 0;
	}
}

int32 UFTDnaCommandlet::Main(const FString& Params)
{
	FString In, Dump, Deltas, Out, Skm, DnaNew;
	int32 Mesh = 0;
	FParse::Value(*Params, TEXT("skm="), Skm);
	FParse::Value(*Params, TEXT("dna_new="), DnaNew);
	FParse::Value(*Params, TEXT("dna_in="), In);
	FParse::Value(*Params, TEXT("dump="), Dump);
	FParse::Value(*Params, TEXT("deltas="), Deltas);
	FParse::Value(*Params, TEXT("dna_out="), Out);
	FParse::Value(*Params, TEXT("mesh="), Mesh);
	const bool bLowerLods = !FParse::Param(*Params, TEXT("nolowerlods"));

	if (!Skm.IsEmpty())
	{
		TSharedPtr<IDNAReader> Orig = LoadDNAFromFile(In);
		TSharedPtr<IDNAReader> New = LoadDNAFromFile(DnaNew);
		if (!Orig.IsValid() || !New.IsValid())
		{
			UE_LOG(LogFTDna, Error, TEXT("FTDna: -skm needs readable -dna_in= (the mesh's DNA) and -dna_new="));
			return 1;
		}
		return WriteSkeletalMesh(Skm, *Orig, *New, FParse::Param(*Params, TEXT("dry")));
	}
	if (In.IsEmpty() || (Dump.IsEmpty() && (Deltas.IsEmpty() || Out.IsEmpty())))
	{
		UE_LOG(LogFTDna, Error, TEXT("FTDna: need -dna_in= and either -dump=, -deltas= with -dna_out=, or -skm= with -dna_new="));
		return 1;
	}
	if (!Out.IsEmpty() && FPaths::IsSamePath(In, Out))
	{
		UE_LOG(LogFTDna, Error, TEXT("FTDna: -dna_out must differ from -dna_in"));
		return 1;
	}

	TSharedPtr<IDNAReader> Reader = LoadDNAFromFile(In);
	if (!Reader.IsValid() || Mesh < 0 || Mesh >= Reader->GetMeshCount())
	{
		UE_LOG(LogFTDna, Error, TEXT("FTDna: cannot read %s or mesh %d is out of range"), *In, Mesh);
		return 1;
	}
	const uint16 MeshIndex = static_cast<uint16>(Mesh);

	if (!Dump.IsEmpty())
	{
		DumpMesh(*Reader, MeshIndex, Dump);
		UE_LOG(LogFTDna, Display, TEXT("FTDna: dumped mesh %d (%d verts) to %s"), Mesh, Reader->GetVertexPositionCount(MeshIndex), *Dump);
		return 0;
	}

	TArray<FString> Lines;
	FFileHelper::LoadFileToStringArray(Lines, *Deltas);
	const int32 Count = static_cast<int32>(Reader->GetVertexPositionCount(MeshIndex));
	TArray<float> Dx, Dy, Dz;
	Dx.Reserve(Count);
	Dy.Reserve(Count);
	Dz.Reserve(Count);
	double MaxMove = 0.0;
	for (const FString& Line : Lines)
	{
		TArray<FString> Parts;
		Line.ParseIntoArrayWS(Parts);
		if (Parts.Num() < 3 || Parts[0].StartsWith(TEXT("#")))
		{
			continue;
		}
		Dx.Add(FCString::Atof(*Parts[0]));
		Dy.Add(FCString::Atof(*Parts[1]));
		Dz.Add(FCString::Atof(*Parts[2]));
		MaxMove = FMath::Max(MaxMove, FVector(Dx.Last(), Dy.Last(), Dz.Last()).Size());
	}
	if (Dx.Num() != Count)
	{
		UE_LOG(LogFTDna, Error, TEXT("FTDna: %s has %d deltas, mesh %d has %d vertices"), *Deltas, Dx.Num(), Mesh, Count);
		return 1;
	}

	FDNACalibDNAReader Calib(Reader.Get());
	FDNACalibSetVertexPositionsCommand Move(MeshIndex, Dx, Dy, Dz, EDNACalibVectorOperation::Add);
	Move.Run(&Calib);
	if (bLowerLods)
	{
		FDNACalibCalculateMeshLowerLODsCommand Lods(MeshIndex);
		Lods.Run(&Calib);
	}
	SaveDNAToFile(&Calib, EDNADataLayer::All, Out);
	UE_LOG(LogFTDna, Display, TEXT("FTDna: added deltas to mesh %d (%d verts, max move %.3f), lower LODs %s, wrote %s"), Mesh, Count,
		MaxMove, bLowerLods ? TEXT("recalculated") : TEXT("kept"), *Out);
	return 0;
}
