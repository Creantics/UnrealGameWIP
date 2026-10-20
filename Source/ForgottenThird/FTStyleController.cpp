#include "FTStyleController.h"

#include "Components/PostProcessComponent.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "FTStyleParameters.h"
#include "ForgottenThird.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include <optional>
#include <string_view>

namespace
{
std::optional<ftcore::StyleSettings> ParseSpec(const FString& Spec)
{
	const FTCHARToUTF8 Utf8(*Spec);
	return ftcore::parseStyleSpec(std::string_view(Utf8.Get(), static_cast<size_t>(Utf8.Length())));
}

// ft.Style <spec>: sets every style controller in the world to a pack, inksolhaven or a dial (S3,U1).
void StyleCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}
	const FString Spec = FString::Join(Args, TEXT(","));
	int32 Count = 0;
	for (TActorIterator<AFTStyleController> It(World); It; ++It)
	{
		if (!It->SetStyleSpec(Spec))
		{
			UE_LOG(LogForgottenThird, Warning,
				TEXT("ft.Style: '%s' is not a pack (none|solhaven|lattice|unwritten|restored), inksolhaven or a dial like S3,U1."), *Spec);
			return;
		}
		++Count;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("ft.Style: %s on %d controller(s)."), *Spec, Count);
}

FAutoConsoleCommandWithWorldAndArgs GStyleCommand(TEXT("ft.Style"),
	TEXT("Sets the world style: ft.Style none|solhaven|lattice|unwritten|restored|inksolhaven, or a dial such as ft.Style S3,U1."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&StyleCommand));
}

AFTStyleController::AFTStyleController()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProcess"));
	PostProcess->bUnbound = true;
	// Above the level's post-process volume (priority 0), so the pack's finish wins.
	PostProcess->Priority = 10.f;
	RootComponent = PostProcess;
}

ftcore::StyleSettings AFTStyleController::PackStyle(EFTStylePack Pack)
{
	switch (Pack)
	{
	case EFTStylePack::Solhaven:
		return ftcore::stylePack(ftcore::StylePack::Solhaven);
	case EFTStylePack::Lattice:
		return ftcore::stylePack(ftcore::StylePack::Lattice);
	case EFTStylePack::Unwritten:
		return ftcore::stylePack(ftcore::StylePack::Unwritten);
	case EFTStylePack::Restored:
		return ftcore::stylePack(ftcore::StylePack::Restored);
	case EFTStylePack::InkSolhaven:
		return ftcore::inkSolhavenStyle();
	case EFTStylePack::None:
	default:
		return ftcore::StyleSettings{};
	}
}

void AFTStyleController::BeginPlay()
{
	Super::BeginPlay();

	if (const std::optional<ftcore::StyleSettings> Parsed = ParseSpec(StyleSpec))
	{
		Current = *Parsed;
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTStyleController: can't parse style \"%s\"; using None."), *StyleSpec);
		Current = ftcore::StyleSettings{};
	}

	// The static pack instance (if any) is only for the editor viewport; the dynamic one replaces it.
	PostProcess->Settings.WeightedBlendables.Array.Reset();
	if (StyleMaterial)
	{
		Instance = UMaterialInstanceDynamic::Create(StyleMaterial, this);
		Instance->SetScalarParameterValue(FTStyle::SkyDepthName(), FTStyle::DefaultSkyDepth);
		PostProcess->Settings.AddBlendable(Instance, 1.f);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTStyleController: no StyleMaterial; only the post finish is styled."));
	}
	// Writes every parameter, so the instance holds an entry for each and later writes don't allocate.
	Apply();
}

void AFTStyleController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (BlendSeconds <= 0.f)
	{
		SetActorTickEnabled(false);
		return;
	}
	BlendElapsed += DeltaSeconds;
	if (BlendElapsed >= BlendSeconds)
	{
		Current = To;
		BlendSeconds = 0.f;
		SetActorTickEnabled(false);
	}
	else
	{
		Current = ftcore::lerp(From, To, BlendElapsed / BlendSeconds);
	}
	Apply();
}

void AFTStyleController::SetStyle(const ftcore::StyleSettings& Style)
{
	BlendSeconds = 0.f;
	SetActorTickEnabled(false);
	Current = Style;
	Apply();
}

void AFTStyleController::BlendTo(const ftcore::StyleSettings& Style, float Seconds)
{
	if (Seconds <= 0.f)
	{
		SetStyle(Style);
		return;
	}
	From = Current;
	To = Style;
	BlendSeconds = Seconds;
	BlendElapsed = 0.f;
	SetActorTickEnabled(true);
}

bool AFTStyleController::SetStyleSpec(const FString& Spec)
{
	return BlendToSpec(Spec, 0.f);
}

void AFTStyleController::SetPack(EFTStylePack Pack)
{
	SetStyle(PackStyle(Pack));
}

void AFTStyleController::SetDial(float S, float L, float U, float R, float Seconds)
{
	ftcore::StyleDial Dial;
	Dial.s = S;
	Dial.l = L;
	Dial.u = U;
	Dial.r = R;
	BlendTo(ftcore::styleFromDial(Dial), Seconds);
}

void AFTStyleController::BlendToPack(EFTStylePack Pack, float Seconds)
{
	BlendTo(PackStyle(Pack), Seconds);
}

bool AFTStyleController::BlendToSpec(const FString& Spec, float Seconds)
{
	const std::optional<ftcore::StyleSettings> Parsed = ParseSpec(Spec);
	if (!Parsed)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTStyleController: can't parse style \"%s\"."), *Spec);
		return false;
	}
	BlendTo(*Parsed, Seconds);
	return true;
}

void AFTStyleController::Apply()
{
	if (UMaterialInstanceDynamic* MID = Instance)
	{
		FTStyle::VisitParameters(
			Current, [MID](FName Name, float Value) { MID->SetScalarParameterValue(Name, Value); },
			[MID](FName Name, const FLinearColor& Value) { MID->SetVectorParameterValue(Name, Value); });
	}

	// The post finish runs before an after-tonemapping material, so the pack's multipliers go on the
	// settings instead (docs/STYLE_PACKS_UE.md).
	FPostProcessSettings& S = PostProcess->Settings;
	S.bOverride_BloomIntensity = true;
	S.BloomIntensity = BaseBloomIntensity * Current.bloomScale;
	S.bOverride_BloomThreshold = BaseBloomThreshold >= 0.f;
	S.BloomThreshold = BaseBloomThreshold * Current.bloomThresholdScale;
	S.bOverride_VignetteIntensity = true;
	S.VignetteIntensity = BaseVignetteIntensity * Current.vignetteScale;
	S.bOverride_FilmGrainIntensity = true;
	S.FilmGrainIntensity = BaseFilmGrainIntensity * Current.filmGrainScale;
	S.bOverride_SceneFringeIntensity = true;
	S.SceneFringeIntensity = BaseFringeIntensity * Current.chromaScale;
}
