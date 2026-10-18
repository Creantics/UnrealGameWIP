#pragma once

// Drives a character's face from ftcore::FacePlayer: expression crossfades, baked lip-sync tracks
// (.nface), procedural blinks and a micro layer. All face rules live in Core; this loads the files,
// binds the face mesh's morph targets by name and writes the weights to them every tick.
//
// The jaw and tongue are bones: read GetJawRotation() / GetTongueRotation(i) in the face mesh's
// Anim Blueprint (Transform (Modify) Bone, local space, added to the rest pose).

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "ftcore/Face.h"

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
	// 0 = mouth closed .. 1 = speaking.
	UFUNCTION(BlueprintPure, Category = "Face")
	float GetSpeech() const;
	// Jaw bone rotation about its local +X, from the rest pose.
	UFUNCTION(BlueprintPure, Category = "Face")
	FRotator GetJawRotation() const;
	// Tongue bone Index (0..2 = tongue_01..03) rotation about its local +X, from the rest pose.
	UFUNCTION(BlueprintPure, Category = "Face")
	FRotator GetTongueRotation(int32 Index) const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	USkeletalMeshComponent* FindFaceMesh() const;

	TUniquePtr<ftcore::FaceShapeSet> Shapes;
	TUniquePtr<ftcore::FacePlayer> Player;
	TUniquePtr<ftcore::FaceTrack> Track;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> FaceMesh;

	// Per morph target of FaceMesh: its name, the shape index driving it (-1: none) and its weight.
	TArray<FName> MorphNames;
	std::vector<int> MorphMap;
	TArray<float> MorphWeights;
};
