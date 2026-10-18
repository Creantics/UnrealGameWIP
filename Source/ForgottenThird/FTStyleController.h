#pragma once

// Applies a world style (ftcore::StyleSettings) to the scene. Owns an unbound post-process component
// (above the level's own post-process volume) holding a dynamic instance of /Game/Rendering/PP_Style,
// and writes the style's material parameters and post finish (bloom, vignette, grain, fringe) to it.
// BlendTo crossfades between styles with ftcore::lerp. See docs/STYLE_PACKS_UE.md.
//
// No heap allocation per frame: every material parameter is written once in BeginPlay, so later writes
// update existing entries, and the blend works on plain StyleSettings values.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ftcore/StylePack.h"

#include "FTStyleController.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPostProcessComponent;

UENUM(BlueprintType)
enum class EFTStylePack : uint8
{
	None,
	Solhaven,
	Lattice,
	Unwritten,
	Restored,
	// The Ink Solhaven preset (a dial blend, not a pack).
	InkSolhaven,
};

UCLASS()
class FORGOTTENTHIRD_API AFTStyleController : public AActor
{
	GENERATED_BODY()

public:
	AFTStyleController();

	virtual void Tick(float DeltaSeconds) override;

	// The style at BeginPlay: a pack name ("solhaven"), "inksolhaven" or a dial ("S3,U0.5").
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Style")
	FString StyleSpec = TEXT("solhaven");

	// The post-process material (PP_Style). Its dynamic instance replaces any static blendable on
	// PostProcess at BeginPlay; the static one is only there so the editor viewport shows the style.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Style")
	TObjectPtr<UMaterialInterface> StyleMaterial;

	// The post finish before the pack's multipliers (bloomScale, vignetteScale, ...). The defaults are
	// Unreal's own. A negative BaseBloomThreshold leaves the threshold alone.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Style|Finish")
	float BaseBloomIntensity = 0.675f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Style|Finish")
	float BaseBloomThreshold = -1.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Style|Finish")
	float BaseVignetteIntensity = 0.4f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Style|Finish")
	float BaseFilmGrainIntensity = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Style|Finish")
	float BaseFringeIntensity = 0.f;

	// Switch now. False (and no change) when Spec doesn't parse.
	UFUNCTION(BlueprintCallable, Category = "Style")
	bool SetStyleSpec(const FString& Spec);
	UFUNCTION(BlueprintCallable, Category = "Style")
	void SetPack(EFTStylePack Pack);
	// Needles 0..3 (NDD 8.1.2).
	UFUNCTION(BlueprintCallable, Category = "Style")
	void SetDial(float S, float L, float U, float R, float Seconds = 0.f);

	UFUNCTION(BlueprintCallable, Category = "Style")
	void BlendToPack(EFTStylePack Pack, float Seconds);
	UFUNCTION(BlueprintCallable, Category = "Style")
	bool BlendToSpec(const FString& Spec, float Seconds);

	UFUNCTION(BlueprintPure, Category = "Style")
	bool IsBlending() const { return BlendSeconds > 0.f; }

	void SetStyle(const ftcore::StyleSettings& Style);
	// Crossfade from the current style; Seconds <= 0 switches at once.
	void BlendTo(const ftcore::StyleSettings& Style, float Seconds);
	const ftcore::StyleSettings& GetStyle() const { return Current; }

	UPostProcessComponent* GetPostProcess() const { return PostProcess; }

	static ftcore::StyleSettings PackStyle(EFTStylePack Pack);

protected:
	virtual void BeginPlay() override;

private:
	void Apply();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UPostProcessComponent> PostProcess;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Instance;

	ftcore::StyleSettings Current;
	ftcore::StyleSettings From;
	ftcore::StyleSettings To;
	float BlendSeconds = 0.f;
	float BlendElapsed = 0.f;
};
