#pragma once

// The photoreal look's after-tonemap grade: one full-screen pass, on the display-referred image the filmic tonemapper
// writes, that works in OKLab. It scales chroma (one global scale, two hue windows a world keeps, a window for the
// fight's accent and a floor for skin) and adds a split tone weighted to the shadows and the highlights. Hue is kept
// except by the split tone. The maths is ftcore::gradeSrgb (Core/include/ftcore/LookGrade.h); the shader
// (Shaders/Private/FTLookGrade.usf) mirrors it line by line, so the doctest tests on the CPU side check the GPU's.
//
// A world gets one extension, made by the game's look subsystem, which hands it the constants whenever the look
// changes. The pass draws only while they would change something: the default look and -LookOff cost no pass at all.
// It is skipped on HDR output, where the image isn't the sRGB display values the grade is tuned on. r.FT.LookGrade 0
// turns it off at run time. See docs/PHOTOREAL_LOOK.md.

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "SceneViewExtension.h"

// ftcore::GradeConstants as the GPU takes it: five float4s, hues and widths in radians.
struct FFTLookGradeConstants
{
	float C[5][4] = {};
	// False: the grade changes nothing, so no pass is drawn.
	bool bActive = false;
};

class FTLOOK_API FFTLookGradeExtension : public FWorldSceneViewExtension
{
public:
	FFTLookGradeExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);

	// Game thread. Takes effect from the next frame drawn.
	void SetConstants(const FFTLookGradeConstants& InConstants);
	bool IsGrading() const;

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	FScreenPassTexture Grade_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	// The game thread writes, the render thread reads: a small value copied under the lock, so nothing is queued or
	// allocated per frame.
	mutable FCriticalSection Lock;
	FFTLookGradeConstants Constants;
};
