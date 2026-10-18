#include "FTFaceComponent.h"

#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "FTText.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "ftcore/FaceImport.h"

#include <span>
#include <string_view>

DEFINE_LOG_CATEGORY_STATIC(LogFTFace, Log, All);

namespace
{
FString ProjectPath(const FString& Path)
{
	return FPaths::IsRelative(Path) ? FPaths::Combine(FPaths::ProjectDir(), Path) : Path;
}
} // namespace

UFTFaceComponent::UFTFaceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

UFTFaceComponent::~UFTFaceComponent() = default;

void UFTFaceComponent::BeginPlay()
{
	Super::BeginPlay();
	Rebind();
}

void UFTFaceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Player.Reset();
	Track.Reset();
	Shapes.Reset();
	FaceMesh = nullptr;
	Super::EndPlay(EndPlayReason);
}

USkeletalMeshComponent* UFTFaceComponent::FindFaceMesh() const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}
	TArray<USkeletalMeshComponent*> Meshes;
	Owner->GetComponents(Meshes);
	for (USkeletalMeshComponent* Mesh : Meshes)
	{
		if (FaceMeshComponentName.IsNone() || Mesh->GetFName() == FaceMeshComponentName)
		{
			return Mesh;
		}
	}
	return nullptr;
}

bool UFTFaceComponent::Rebind()
{
	Player.Reset();
	Track.Reset();
	MorphNames.Reset();
	MorphMap.clear();
	MorphWeights.Reset();

	const FString Path = ProjectPath(PresetsFile);
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogFTFace, Error, TEXT("%s: can't read face presets %s"), *GetPathName(), *Path);
		return false;
	}
	TUniquePtr<ftcore::FaceShapeSet> NewShapes = MakeUnique<ftcore::FaceShapeSet>();
	std::string Error;
	if (!ftcore::importFaceShapeSet(FT::BytesAsText(Bytes), *NewShapes, &Error))
	{
		UE_LOG(LogFTFace, Error, TEXT("%s: %s: %s"), *GetPathName(), *Path, *FT::ToFString(Error));
		return false;
	}
	Shapes = MoveTemp(NewShapes);
	Player = MakeUnique<ftcore::FacePlayer>(*Shapes, static_cast<uint32>(BlinkSeed));

	FaceMesh = FindFaceMesh();
	const USkeletalMesh* Asset = FaceMesh ? FaceMesh->GetSkeletalMeshAsset() : nullptr;
	if (!Asset)
	{
		UE_LOG(LogFTFace, Warning, TEXT("%s: no face mesh; weights are computed but not shown"), *GetPathName());
		return true;
	}
	std::vector<std::string> Names;
	for (const TObjectPtr<UMorphTarget>& Morph : Asset->GetMorphTargets())
	{
		if (Morph)
		{
			MorphNames.Add(Morph->GetFName());
			Names.push_back(FT::ToUtf8(Morph->GetName()));
		}
	}
	std::vector<std::string_view> Views(Names.begin(), Names.end());
	ftcore::FacePlayer::bindMesh(*Shapes, Views, MorphMap);
	MorphWeights.SetNumZeroed(MorphNames.Num());

	int32 Bound = 0;
	for (const int S : MorphMap)
	{
		Bound += S >= 0 ? 1 : 0;
	}
	UE_LOG(LogFTFace, Log, TEXT("%s: %d of %d face shapes bound to %s"), *GetPathName(), Bound,
		static_cast<int32>(Shapes->size()), *FaceMesh->GetName());
	return true;
}

bool UFTFaceComponent::SetExpression(FName Expression, float Intensity, float FadeSeconds)
{
	if (!Player)
	{
		return false;
	}
	const ftcore::FacePreset* Preset = nullptr;
	if (!Expression.IsNone())
	{
		Preset = Shapes->expression(FT::ToUtf8(Expression.ToString()));
		if (!Preset)
		{
			UE_LOG(LogFTFace, Warning, TEXT("%s: unknown expression %s"), *GetPathName(), *Expression.ToString());
			return false;
		}
	}
	Player->setExpression(Preset, Intensity, FadeSeconds);
	return true;
}

bool UFTFaceComponent::PlayTrack(const FString& TrackFile, float StartSeconds)
{
	if (!Player)
	{
		return false;
	}
	Player->stop(); // the player points at the old track
	const FString Path = ProjectPath(TrackFile);
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogFTFace, Warning, TEXT("%s: can't read face track %s"), *GetPathName(), *Path);
		return false;
	}
	TUniquePtr<ftcore::FaceTrack> NewTrack = MakeUnique<ftcore::FaceTrack>();
	std::string Error;
	if (!ftcore::parseFaceTrack(std::span<const std::uint8_t>(Bytes.GetData(), static_cast<size_t>(Bytes.Num())), *NewTrack, &Error))
	{
		UE_LOG(LogFTFace, Warning, TEXT("%s: %s: %s"), *GetPathName(), *Path, *FT::ToFString(Error));
		return false;
	}
	Track = MoveTemp(NewTrack);
	return Player->play(*Track, StartSeconds);
}

void UFTFaceComponent::StopTrack()
{
	if (Player)
	{
		Player->stop();
	}
}

bool UFTFaceComponent::IsTrackPlaying() const { return Player && Player->playing(); }

void UFTFaceComponent::SetMicro(FName Shape, float Weight)
{
	if (Player)
	{
		Player->setMicro(Shapes->find(FT::ToUtf8(Shape.ToString())), Weight);
	}
}

void UFTFaceComponent::ClearMicro()
{
	if (Player)
	{
		Player->clearMicro();
	}
}

void UFTFaceComponent::SetBlinking(bool bOn)
{
	if (Player)
	{
		Player->setBlinking(bOn);
	}
}

float UFTFaceComponent::GetShapeWeight(FName Shape) const
{
	if (!Player)
	{
		return 0.f;
	}
	const int S = Shapes->find(FT::ToUtf8(Shape.ToString()));
	return S >= 0 && S < static_cast<int>(ftcore::kMaxFaceShapes) ? Player->weights()[static_cast<size_t>(S)] : 0.f;
}

float UFTFaceComponent::GetSpeech() const { return Player ? Player->speech() : 0.f; }

FRotator UFTFaceComponent::GetJawRotation() const
{
	// FRotator roll is the rotation about local X.
	return FRotator(0.f, 0.f, Player ? Player->jawDeg() : 0.f);
}

FRotator UFTFaceComponent::GetTongueRotation(int32 Index) const
{
	if (!Player)
	{
		return FRotator::ZeroRotator;
	}
	const ftcore::FaceVec3 T = Player->tongueDeg();
	const float Deg = Index == 0 ? T.x : Index == 1 ? T.y : Index == 2 ? T.z : 0.f;
	return FRotator(0.f, 0.f, Deg);
}

void UFTFaceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Player)
	{
		return;
	}
	Player->update(DeltaTime);
	if (!FaceMesh || MorphNames.Num() == 0)
	{
		return;
	}
	Player->writeMeshWeights(MorphMap, std::span<float>(MorphWeights.GetData(), static_cast<size_t>(MorphWeights.Num())));
	for (int32 I = 0; I < MorphNames.Num(); ++I)
	{
		if (MorphMap[static_cast<size_t>(I)] >= 0)
		{
			FaceMesh->SetMorphTarget(MorphNames[I], MorphWeights[I]);
		}
	}
}
