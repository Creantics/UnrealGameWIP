#include "FTMeshPieces.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Misc/CommandLine.h"

#include "ForgottenThird.h"

int32 FTMeshPieces::Attach(AActor& Owner, USkeletalMeshComponent& Body, const TArray<FSoftObjectPath>& Paths,
	TArray<TObjectPtr<USkeletalMeshComponent>>& Out, const TCHAR* Who)
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
		Out.Add(Piece);
	}
	return Cloth;
}
