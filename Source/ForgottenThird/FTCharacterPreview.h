#pragma once

// A character on a stand, for rigging tests: a skeletal mesh driven by UFTFaceComponent, cycling
// through the face's expression presets so every morph target can be checked by eye. Set Body's
// mesh (and its Anim Blueprint, for the jaw and tongue) in the Details panel.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "FTCharacterPreview.generated.h"

class USkeletalMeshComponent;
class UFTFaceComponent;

UCLASS()
class FORGOTTENTHIRD_API AFTCharacterPreview : public AActor
{
	GENERATED_BODY()

public:
	AFTCharacterPreview();

	virtual void Tick(float DeltaSeconds) override;

	// Step through CycleExpressions, holding each for CycleSeconds.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preview")
	bool bAutoCycle = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preview", meta = (ClampMin = "0.1"))
	float CycleSeconds = 2.f;

	// Expression intensity 0..5 (5 = the preset's weights).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preview", meta = (ClampMin = "0", ClampMax = "5"))
	float Intensity = 5.f;

	// Preset names from face_presets.json.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preview")
	TArray<FName> CycleExpressions;

	USkeletalMeshComponent* GetBody() const { return Body; }
	UFTFaceComponent* GetFace() const { return Face; }

protected:
	virtual void BeginPlay() override;

private:
	void ShowExpression(int32 Index);

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> Body;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UFTFaceComponent> Face;

	int32 Current = 0;
	float Held = 0.f;
};
