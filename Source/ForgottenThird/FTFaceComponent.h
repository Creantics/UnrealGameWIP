#pragma once

// Drives a character's face from ftcore::FacePlayer: expression crossfades, baked lip-sync tracks
// (.nface), procedural blinks and a micro layer. All face rules live in Core; this loads the files,
// binds the face mesh's morph targets by name and writes the weights to them every tick.
//
// The jaw and tongue are bones: UFTFaceAnimInstance (set as the mesh's anim class) applies
// GetJawRotation() / GetTongueRotation(i) in local space, added to the rest pose.
//
// Curve mode (a MetaHuman face): a face mesh with none of the shapes as morph targets but a post-process
// anim blueprint (its rig logic) is driven by control curves instead. The rig's own mapping pose asset
// (CurveMapPose: a pose per ARKit shape, each a value per CTRL_expressions curve) is read at bind time into
// an ftcore::FaceCurveMap; each tick the weights become curve values (GetCurveValues), which
// UFTFaceCurveAnimInstance (the face mesh's anim class) hands to the rig logic. Jaw and tongue come from the
// rig, not from bones.

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "ftcore/Face.h"
#include "ftcore/StoryCast.h"

#include <string>
#include <vector>

#include "FTFaceComponent.generated.h"

class USkeletalMeshComponent;

UCLASS(ClassGroup = (ForgottenThird), meta = (BlueprintSpawnableComponent))
class FORGOTTENTHIRD_API UFTFaceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFTFaceComponent();
	virtual ~UFTFaceComponent() override;

	// face_presets.json, relative to the project folder unless absolute.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Face")
	FString PresetsFile = TEXT("Data/Rig/face_presets.json");

	// The skeletal mesh component that has the face's morph targets. None: the owner's first one.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Face")
	FName FaceMeshComponentName;

	// The character model this face belongs to (an id from Data/Assets/models.json). The story director
	// plays a voiced line's lip-sync track on the faces whose model the speaker is cast as
	// (Data/Voice/speakers.json). Empty: the face never speaks story lines.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Face")
	FString ModelId;

	// A stand-in face speaks a voiced line of any cast speaker when no face of the speaker's own model
	// is in the map (the dialogue map's preview stands in for the whole cast).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Face")
	bool bStandIn = false;

	// Curve mode's mapping pose asset (see the top of this file).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Face")
	FSoftObjectPath CurveMapPose = FSoftObjectPath(TEXT("/Game/Characters/MHCommon/Face/ARKit/PA_MetaHuman_ARKit_Mapping.PA_MetaHuman_ARKit_Mapping"));

	// BeginPlay calls Rebind (off for an owner that binds once its meshes are on).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Face")
	bool bBindOnBeginPlay = true;

	// Seed for the blink timing (same seed, same blinks).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Face")
	int32 BlinkSeed = 0x5eed;

	// Loads the presets and binds the face mesh. Called by BeginPlay; call again after swapping the
	// mesh or the presets file. False (and logged) when the presets don't load.
	UFUNCTION(BlueprintCallable, Category = "Face")
	bool Rebind();

	// Crossfades to an expression preset (None: neutral). Intensity 0..5 (5 = preset weights).
	UFUNCTION(BlueprintCallable, Category = "Face")
	bool SetExpression(FName Expression, float Intensity = 5.f, float FadeSeconds = 0.25f);

	// Plays an .nface lip-sync track from StartSeconds (relative paths: the project folder).
	UFUNCTION(BlueprintCallable, Category = "Face")
	bool PlayTrack(const FString& TrackFile, float StartSeconds = 0.f);
	UFUNCTION(BlueprintCallable, Category = "Face")
	void StopTrack();
	UFUNCTION(BlueprintPure, Category = "Face")
	bool IsTrackPlaying() const;

	// Micro layer (gaze darts, twitches): held until changed.
	UFUNCTION(BlueprintCallable, Category = "Face")
	void SetMicro(FName Shape, float Weight);
	UFUNCTION(BlueprintCallable, Category = "Face")
	void ClearMicro();
	UFUNCTION(BlueprintCallable, Category = "Face")
	void SetBlinking(bool bOn);

	// The composed weight of a face shape (0 when unknown).
	UFUNCTION(BlueprintPure, Category = "Face")
	float GetShapeWeight(FName Shape) const;
	// The face shapes bound by the last Rebind, to the mesh's morph targets or (curve mode) to rig curves with a
	// post-process rig to read them (0: a track plays unseen).
	UFUNCTION(BlueprintPure, Category = "Face")
	int32 GetBoundShapes() const { return BoundShapes; }
	// 0 = mouth closed .. 1 = speaking.
	UFUNCTION(BlueprintPure, Category = "Face")
	float GetSpeech() const;
	// Jaw bone rotation about its local +X, from the rest pose.
	UFUNCTION(BlueprintPure, Category = "Face")
	FRotator GetJawRotation() const;
	// Tongue bone Index (0..2 = tongue_01..03) rotation about its local +X, from the rest pose.
	UFUNCTION(BlueprintPure, Category = "Face")
	FRotator GetTongueRotation(int32 Index) const;

	// Curve mode: the rig's curve names and this tick's values (same order). Empty otherwise.
	bool IsCurveMode() const { return bCurveMode; }
	const TArray<FName>& GetCurveNames() const { return CurveNames; }
	const TArray<float>& GetCurveValues() const { return CurveValues; }
	// Curve mode: the most rig morph targets seen moving at once on the face mesh while a track played (the
	// rig logic's output: proof the curves reach the mesh, not just the anim graph).
	int32 GetPeakRigMorphs() const { return PeakRigMorphs; }
	// Curve mode: the most of this face's driven curves (value > 0.05) seen at once in the mesh's anim output
	// while a track played (the curves reach the anim graph), and whether the mesh runs its rig.
	int32 GetPeakCurvesArrived() const { return PeakCurvesArrived; }
	// Curve mode: the most the rig turned the face's jaw joint from its rest pose while a track played, in
	// degrees (the rig logic's joint output), and the most morph curves the mesh held at once.
	float GetPeakRigJawDeg() const { return PeakRigJawDeg; }
	int32 GetPeakMorphCurves() const { return PeakMorphCurves; }
	// Curve mode: the most the lips-together control was driven (mouthClose's hand mapping), 0..1.
	float GetPeakLipsTogether() const { return PeakLipsTogether; }
	bool HasRigInstance() const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	USkeletalMeshComponent* FindFaceMesh() const;
	bool BindCurves();

	TUniquePtr<ftcore::FaceShapeSet> Shapes;
	TUniquePtr<ftcore::FacePlayer> Player;
	TUniquePtr<ftcore::FaceTrack> Track;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> FaceMesh;

	// Per morph target of FaceMesh: its name, the shape index driving it (-1: none) and its weight.
	TArray<FName> MorphNames;
	std::vector<int> MorphMap;
	int32 BoundShapes = 0;
	TArray<float> MorphWeights;

	TUniquePtr<ftcore::FaceCurveMap> CurveMap;
	TArray<FName> CurveNames;
	TArray<float> CurveValues;
	bool bCurveMode = false;
	int32 PeakRigMorphs = 0;
	int32 PeakCurvesArrived = 0;
	float PeakRigJawDeg = 0.f;
	int32 PeakMorphCurves = 0;
	int32 RigJawBone = INDEX_NONE;
	float PeakLipsTogether = 0.f;
	int32 LipsCurve = INDEX_NONE;
};
