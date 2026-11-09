#include "FTLookGrade.h"

#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"
#include "UnrealClient.h"

namespace
{
TAutoConsoleVariable<int32> CVarLookGrade(TEXT("r.FT.LookGrade"), 1,
	TEXT("The photoreal look's after-tonemap grade: 1 draws it where the world's look asks for it, 0 never."),
	ECVF_RenderThreadSafe);

class FFTLookGradePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFTLookGradePS);
	SHADER_USE_PARAMETER_STRUCT(FFTLookGradePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER_ARRAY(FVector4f, Grade, [5])
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFTLookGradePS, "/Plugin/FTLook/Private/FTLookGrade.usf", "MainPS", SF_Pixel);
}

FFTLookGradeExtension::FFTLookGradeExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
}

void FFTLookGradeExtension::SetConstants(const FFTLookGradeConstants& InConstants)
{
	FScopeLock ScopeLock(&Lock);
	Constants = InConstants;
}

bool FFTLookGradeExtension::IsGrading() const
{
	FScopeLock ScopeLock(&Lock);
	return Constants.bActive;
}

bool FFTLookGradeExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return CVarLookGrade.GetValueOnAnyThread() != 0 && IsGrading() && FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context);
}

void FFTLookGradeExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	// Right after the tonemapper, on the sRGB display values; HDR output is left alone.
	if (Pass != EPostProcessingPass::Tonemap || !bIsPassEnabled || !InView.Family || InView.Family->bIsHDR)
	{
		return;
	}
	if (InView.Family->RenderTarget && InView.Family->RenderTarget->GetDisplayOutputFormat() != EDisplayOutputFormat::SDR_sRGB)
	{
		return;
	}
	InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FFTLookGradeExtension::Grade_RenderThread));
}

FScreenPassTexture FFTLookGradeExtension::Grade_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	check(SceneColor.IsValid());

	// When the override output is set this is the last pass, and it must be written to.
	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, View.GetOverwriteLoadAction(), TEXT("FTLookGrade"));
	}

	FFTLookGradeConstants Now;
	{
		FScopeLock ScopeLock(&Lock);
		Now = Constants;
	}

	FFTLookGradePS::FParameters* Parameters = GraphBuilder.AllocParameters<FFTLookGradePS::FParameters>();
	Parameters->InputTexture = SceneColor.Texture;
	Parameters->InputSampler = TStaticSamplerState<SF_Point>::GetRHI();
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Parameters->Grade[Index] = FVector4f(Now.C[Index][0], Now.C[Index][1], Now.C[Index][2], Now.C[Index][3]);
	}
	Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
	TShaderMapRef<FFTLookGradePS> PixelShader(ShaderMap);
	AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("FTLookGrade %dx%d", Output.ViewRect.Width(), Output.ViewRect.Height()), View,
		FScreenPassTextureViewport(Output), FScreenPassTextureViewport(SceneColor), VertexShader, PixelShader, Parameters);
	return MoveTemp(Output);
}
