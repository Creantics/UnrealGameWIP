#include "FTFaceComponent.h"

#include "Animation/MorphTarget.h"
#include "Animation/PoseAsset.h"
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
	if (bBindOnBeginPlay)
	{
		Rebind();
	}
}

void UFTFaceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Player.Reset();
	Track.Reset();
	Shapes.Reset();
	CurveMap.Reset();
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
	BoundShapes = 0;
	CurveMap.Reset();
	CurveNames.Reset();
	CurveValues.Reset();
	bCurveMode = false;
	PeakRigMorphs = 0;
	PeakCurvesArrived = 0;
	PeakRigJawDeg = 0.f;
	PeakMorphCurves = 0;
	RigJawBone = INDEX_NONE;
	PeakLipsTogether = 0.f;
	LipsCurve = INDEX_NONE;

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
	BoundShapes = Bound;
	if (Bound == 0 && BindCurves())
	{
		return true;
	}
	UE_LOG(LogFTFace, Log, TEXT("%s: %d of %d face shapes bound to %s"), *GetPathName(), Bound,
		static_cast<int32>(Shapes->size()), *FaceMesh->GetName());
	return true;
}

bool UFTFaceComponent::HasRigInstance() const
{
	return FaceMesh && FaceMesh->GetPostProcessInstance() != nullptr;
}

bool UFTFaceComponent::BindCurves()
{
	// A rig that reads curves: the mesh's post-process anim blueprint (a MetaHuman face's rig logic).
	if (!FaceMesh->GetPostProcessAnimBPClassToBeUsed())
	{
		return false;
	}
	const UPoseAsset* Pose = Cast<UPoseAsset>(CurveMapPose.TryLoad());
	if (!Pose)
	{
		UE_LOG(LogFTFace, Warning, TEXT("%s: %s has a rig but no curve map pose asset loads (%s); the face stays still."),
			*GetPathName(), *FaceMesh->GetName(), *CurveMapPose.ToString());
		return false;
	}
	const TArray<FName>& PoseNames = Pose->GetPoseFNames();
	const TArray<FName> Curves = Pose->GetCurveFNames();
	std::vector<std::string> PoseText, CurveText;
	PoseText.reserve(static_cast<size_t>(PoseNames.Num()));
	CurveText.reserve(static_cast<size_t>(Curves.Num()));
	for (const FName& N : PoseNames)
	{
		PoseText.push_back(FT::ToUtf8(N.ToString()));
	}
	for (const FName& N : Curves)
	{
		CurveText.push_back(FT::ToUtf8(N.ToString()));
	}
	std::vector<float> Values(PoseText.size() * CurveText.size(), 0.f);
	for (int32 P = 0; P < PoseNames.Num(); ++P)
	{
		const TArray<float>& V = Pose->GetCurveValues(P);
		for (int32 C = 0; C < V.Num() && C < Curves.Num(); ++C)
		{
			Values[static_cast<size_t>(P) * CurveText.size() + static_cast<size_t>(C)] = V[C];
		}
	}
	std::vector<std::string_view> PoseViews(PoseText.begin(), PoseText.end());
	std::vector<std::string_view> CurveViews(CurveText.begin(), CurveText.end());
	TUniquePtr<ftcore::FaceCurveMap> Map = MakeUnique<ftcore::FaceCurveMap>();
	std::vector<std::string> Unmatched;
	if (!ftcore::buildFaceCurveMap(*Shapes, PoseViews, CurveViews, Values, *Map, &Unmatched, ftcore::faceCurveHandMap()) || Map->empty())
	{
		UE_LOG(LogFTFace, Warning, TEXT("%s: the curve map pose asset maps no face shape (%d poses, %d curves); the face stays still."),
			*GetPathName(), PoseNames.Num(), Curves.Num());
		return false;
	}
	CurveNames = Curves;
	CurveValues.SetNumZeroed(Curves.Num());
	BoundShapes = static_cast<int32>(Map->mappedShapes);
	CurveMap = MoveTemp(Map);
	bCurveMode = true;
	RigJawBone = FaceMesh->GetBoneIndex(TEXT("FACIAL_C_Jaw"));
	LipsCurve = CurveNames.IndexOfByKey(FName(TEXT("CTRL_expressions_mouthLipsTogetherUL")));
	FString Missing;
	for (const std::string& U : Unmatched)
	{
		Missing += (Missing.IsEmpty() ? TEXT("") : TEXT(", ")) + FT::ToFString(U);
	}
	UE_LOG(LogFTFace, Log, TEXT("%s: curve mode on %s: %d of %d face shapes mapped (%u by hand) to %d rig curves (%d poses, %d terms, additive %d)%s%s"),
		*GetPathName(), *FaceMesh->GetName(), BoundShapes, static_cast<int32>(Shapes->size()), CurveMap->handShapes, Curves.Num(), PoseNames.Num(),
		static_cast<int32>(CurveMap->terms.size()), Pose->IsValidAdditive() ? 1 : 0,
		Missing.IsEmpty() ? TEXT("") : TEXT("; no pose for: "), *Missing);
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
	if (bCurveMode)
	{
		ftcore::writeFaceCurves(*CurveMap, Player->weights(), std::span<float>(CurveValues.GetData(), static_cast<size_t>(CurveValues.Num())));
		if (FaceMesh && Player->playing())
		{
			int32 Moving = 0;
			for (const TPair<FName, float>& M : FaceMesh->GetMorphTargetCurves())
			{
				Moving += M.Value > 0.01f ? 1 : 0;
			}
			PeakRigMorphs = FMath::Max(PeakRigMorphs, Moving);
			PeakMorphCurves = FMath::Max(PeakMorphCurves, FaceMesh->GetMorphTargetCurves().Num());
			if (CurveValues.IsValidIndex(LipsCurve))
			{
				PeakLipsTogether = FMath::Max(PeakLipsTogether, CurveValues[LipsCurve]);
			}
			const TArray<FTransform>& Local = FaceMesh->GetBoneSpaceTransforms();
			const USkeletalMesh* Asset = FaceMesh->GetSkeletalMeshAsset();
			if (RigJawBone != INDEX_NONE && Asset && Local.IsValidIndex(RigJawBone) && Asset->GetRefSkeleton().GetRefBonePose().IsValidIndex(RigJawBone))
			{
				const FQuat Rest = Asset->GetRefSkeleton().GetRefBonePose()[RigJawBone].GetRotation();
				PeakRigJawDeg = FMath::Max(PeakRigJawDeg, FMath::RadiansToDegrees(Rest.AngularDistance(Local[RigJawBone].GetRotation())));
			}
			if (const UAnimInstance* Anim = FaceMesh->GetAnimInstance())
			{
				int32 Arrived = 0;
				for (int32 I = 0; I < CurveValues.Num(); ++I)
				{
					Arrived += CurveValues[I] > 0.05f && Anim->GetCurveValue(CurveNames[I]) > 0.05f ? 1 : 0;
				}
				PeakCurvesArrived = FMath::Max(PeakCurvesArrived, Arrived);
			}
		}
		return;
	}
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
