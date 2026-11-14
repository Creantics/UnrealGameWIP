#include "FTNewUi.h"

#include "FTArenaFighter.h"
#include "FTArenaRunner.h"
#include "ForgottenThird.h"
#include "FTInputBindings.h"
#include "FTSettingsSubsystem.h"
#include "FTStoryDirector.h"
#include "FTText.h"
#include "FTUiText.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/UiBudget.h"
#include "ftcore/UiEventMarks.h"
#include "ftcore/UiFirstUse.h"
#include "ftcore/UiLone.h"
#include "ftcore/Settings.h"
#include "ftcore/InputBindings.h"
#include "ftcore/UiStroke.h"
#include "ftcore/UiStyle.h"
#include "ftcore/UiViews.h"

#include "Algo/BinarySearch.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Fonts/CompositeFont.h"
#include "Fonts/FontCache.h"
#include "Fonts/FontMeasure.h"
#include "HAL/FileManager.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Layout/Clipping.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "UnrealClient.h"
#include "Widgets/SLeafWidget.h"

#include <atomic>
#include <string>

// Core's game-thread heap hook (HAL/UnrealMemory.cpp, not in a public header), for the HUD's one-frame allocation count.
#define FT_UI_ALLOC_COUNT (!UE_BUILD_SHIPPING && MALLOC_GT_HOOKS)
#if FT_UI_ALLOC_COUNT
extern CORE_API TFunction<void(int32)>* GGameThreadMallocHook;
#endif

namespace
{
TAutoConsoleVariable<int32> CVarNewUi(TEXT("ft.NewUi"), 0,
	TEXT("1: the new game UI (Slate, drawn from Core view-models) in place of the Canvas HUD's parts it replaces. ")
	TEXT("-FTNewUi on the command line sets it. Read when a map's HUD begins: a change applies from the next map."));

using ftcore::UiTextRole;
using ftcore::UiToken;

// The words a fight keeps: the call word under the boss name (a called zone, or the answer a boss's move needs).
const FTUiText kZoneUi[3] = {FTUiText(TEXT("arena.boss.left")), FTUiText(TEXT("arena.boss.right")), FTUiText(TEXT("arena.boss.path"))};
const FTUiText kCallDodgeUi(TEXT("arena.call.dodge")); // the call word has no "!" (the old HUD keeps arena.dodge)
const FTUiText kAnswerUi[3] = {FTUiText(TEXT("arena.parry")), FTUiText(TEXT("arena.block")), kCallDodgeUi};
const FTUiText kStaggeredUi(TEXT("battle.staggered")); // the call word's slot, with no line
const FTUiText kNextMoveUi(TEXT("arena.next_move")); // the Read row's prefix
const FTUiText kReleaseUi(TEXT("arena.release"));
const FTUiText kSharedUi(TEXT("arena.label.shared"));
const FTUiText kWeightUi(TEXT("arena.boss.weight"));
const FTUiText kDebtUi(TEXT("arena.debt"));
const FTUiText kLeadUi(TEXT("input.lead"));     // a party row's name without a look
const FTUiText kPartnerUi(TEXT("input.count")); // "partner": the second row's fallback name and the paired counter's prompt
const FTUiText kGuardUi(TEXT("input.guard")); // the guard row's word and the guard prompt's
const FTUiText kLoadUi(TEXT("arena.load"));   // the lead's state slot while some Load is held
const FTUiText kPoiseUi(TEXT("arena.poise")); // always beside the poise line, whenever the line is drawn
const FTUiText kWeakPointUi(TEXT("arena.label.weak_point"));
// The first-use labels' words, by UiFirstUseItem (the controls teach by their prompts and have none here).
// (The Lone Carry's card is timed by UiLone and drawn as its own block: no label either.)
const FTUiText* const kFirstUseUi[ftcore::kUiFirstUseCount] = {&kWeakPointUi, nullptr, nullptr, nullptr, nullptr, nullptr};
// The Lone Carry's card: its title, its two heads and each line's words (by ftcore::UiLoneLine; the figures are Core's),
// and its words in the lead's state slot.
const FTUiText kLoneTitleUi(TEXT("arena.lone.title"));
const FTUiText kLoneGainUi(TEXT("arena.lone.gain"));
const FTUiText kLoneGiveUpUi(TEXT("arena.lone.give_up"));
const FTUiText kLoneLoadCapUi(TEXT("arena.lone.load_cap"));
const FTUiText kLoneDamageTakenUi(TEXT("arena.lone.damage_taken"));
const FTUiText kLoneNoCountUi(TEXT("arena.lone.no_count"));
const FTUiText kLoneNoTableUi(TEXT("arena.lone.no_table"));
const FTUiText kLoneNoTakeHalfUi(TEXT("arena.lone.no_take_half"));
const FTUiText kLoneGiftEndsUi(TEXT("arena.lone.gift_ends"));
const FTUiText* const kLoneLineUi[] = {&kReleaseUi, &kLoadUi, &kLoneLoadCapUi, &kLoneDamageTakenUi, &kLoneNoCountUi, &kLoneNoTableUi,
	&kLoneNoTakeHalfUi, &kLoneGiftEndsUi};
static_assert(UE_ARRAY_COUNT(kLoneLineUi) == ftcore::kUiLoneLineKinds, "one line's words per UiLoneLine, in its order");
const FTUiText kLoneAloneUi(TEXT("arena.lone.alone"));
const FTUiText kLoneEndedGiftUi(TEXT("arena.lone.ended_gift"));
const FTUiText kLoneEndedPartnerUi(TEXT("arena.lone.ended_partner"));
// The hallucination's read-outs: TOO SOON in the call slot (a refusal made too early), WAIT in the lead's state slot (an
// Escrow wait running).
const FTUiText kTooSoonUi(TEXT("arena.call.too_soon"));
const FTUiText kWaitUi(TEXT("arena.wait"));
// The Read row's move names, by the tell the move shows (arenaTellName).
const char* const kReadTells[9] = {"strike", "overhead", "backhand", "charge", "thrust", "slash", "sweep", "slam", "shot"};
const FTUiText kReadStrikeUi(TEXT("arena.read.strike"));
const FTUiText kReadOverheadUi(TEXT("arena.read.overhead"));
const FTUiText kReadBackhandUi(TEXT("arena.read.backhand"));
const FTUiText kReadChargeUi(TEXT("arena.read.charge"));
const FTUiText kReadThrustUi(TEXT("arena.read.thrust"));
const FTUiText kReadSlashUi(TEXT("arena.read.slash"));
const FTUiText kReadSweepUi(TEXT("arena.read.sweep"));
const FTUiText kReadSlamUi(TEXT("arena.read.slam"));
const FTUiText kReadShotUi(TEXT("arena.read.shot"));
const FTUiText kReadMoveUi[9] = {kReadStrikeUi, kReadOverheadUi, kReadBackhandUi, kReadChargeUi, kReadThrustUi, kReadSlashUi,
	kReadSweepUi, kReadSlamUi, kReadShotUi};
// The prompts' words and their hint rows (the key as bound), by ftcore::UiPrompt.
const FTUiText kHealUi(TEXT("input.heal"));
const FTUiText kGiftHealUi(TEXT("arena.prompt.gift_heal"));
const FTUiText kGiftGuardUi(TEXT("arena.prompt.gift_guard"));
const FTUiText kReloadUi(TEXT("arena.prompt.reload"));
const FTUiText kAttackUi(TEXT("arena.prompt.attack"));
const FTUiText kDodgeUi(TEXT("input.dodge"));
const FTUiText kJumpUi(TEXT("input.jump"));
const FTUiText kPromptUi[] = {kHealUi, kGiftHealUi, kGiftGuardUi, kReloadUi, kPartnerUi, kAttackUi, kGuardUi, kDodgeUi, kJumpUi};
static_assert(UE_ARRAY_COUNT(kPromptUi) == ftcore::kUiPromptCount, "one word per prompt, in UiPrompt order");
// Each prompt's key: the controls line's entry with this label, or (no label) the entry that carries this action, else
// that action's own keys (inputActionKeys). HEAL is the lead's own heal (input.heal: H, on a pad D-pad up). The gifts are
// left out of the controls line, so they take their own keys: G and Y; on a pad they have no button or route until the
// pad's layout is ruled (inputPadPending), so none there. A rebinding that puts two rows on one key
// keeps the lowest (uiPromptsDrawable). An action the bindings import left unbound shows NO KEY in its key box, dimmed
// (inputPromptKey on FTInputBindings::Unbound), and keeps its row. RELOAD has no action yet: no entry has its label, and
// its answer from the rules is Absent, so it draws nothing.
struct FPromptKeyOf
{
	const char* Label;
	ftcore::InputAction Action;
};
const FPromptKeyOf kPromptKeys[] = {{"input.heal", {}}, {nullptr, ftcore::InputAction::GiftHeal}, {nullptr, ftcore::InputAction::GiftGuard},
	{"input.reload", {}}, {"input.count", {}}, {"input.light", {}}, {"input.guard", {}}, {"input.dodge", {}}, {"input.jump", {}}};
static_assert(UE_ARRAY_COUNT(kPromptKeys) == ftcore::kUiPromptCount, "one key per prompt, in UiPrompt order");
// A prompt's NO KEY, in its key box; and the word after a prompt whose press would be refused now, by ftcore::PressReason
// (Ok and Busy are taken and have none; Absent draws no prompt).
const FTUiText kNoKeyUi(TEXT("arena.prompt.no_key"));
const FTUiText kWhyDownUi(TEXT("arena.prompt.why.down"));
const FTUiText kWhyBrokenUi(TEXT("arena.prompt.why.broken"));
const FTUiText kWhyOverloadedUi(TEXT("arena.prompt.why.overloaded"));
const FTUiText kWhyStaminaUi(TEXT("arena.prompt.why.stamina"));
const FTUiText kWhyEmptyUi(TEXT("arena.prompt.why.empty"));
const FTUiText kWhyCooldownUi(TEXT("arena.prompt.why.cooldown"));
const FTUiText kWhyFullUi(TEXT("arena.prompt.why.full"));
const FTUiText kWhyClosedUi(TEXT("arena.prompt.why.closed"));
const FTUiText kWhyRefusedUi(TEXT("arena.prompt.why.refused"));
const FTUiText* const kWhyUi[] = {nullptr, nullptr, nullptr, &kWhyDownUi, &kWhyBrokenUi, &kWhyOverloadedUi, &kWhyStaminaUi,
	&kWhyEmptyUi, &kWhyCooldownUi, &kWhyFullUi, &kWhyClosedUi, &kWhyRefusedUi};
static_assert(UE_ARRAY_COUNT(kWhyUi) == ftcore::kPressReasonCount, "one word per PressReason, in its order");
static_assert(static_cast<int>(ftcore::PressReason::Down) == 3 && static_cast<int>(ftcore::PressReason::Refused) == 11,
	"the words follow PressReason's order");
// A Qte's word in the call slot: a Tap shows its button's word (the basic controls', by ActionButton), a Hold HOLD, a
// Mash MASH. Its key cap is that button's prompt key, so the basic controls must follow ActionButton's order.
const FTUiText kQteHoldUi(TEXT("qte.hold"));
const FTUiText kQteMashUi(TEXT("qte.mash"));
static_assert(static_cast<int>(ftcore::UiPrompt::Guard) - static_cast<int>(ftcore::UiPrompt::Attack) == static_cast<int>(ftcore::ActionButton::Guard)
	&& static_cast<int>(ftcore::UiPrompt::Dodge) - static_cast<int>(ftcore::UiPrompt::Attack) == static_cast<int>(ftcore::ActionButton::Dodge)
	&& static_cast<int>(ftcore::UiPrompt::Jump) - static_cast<int>(ftcore::UiPrompt::Attack) == static_cast<int>(ftcore::ActionButton::Jump),
	"the basic controls' prompts follow ActionButton's order");
constexpr int32 kQteWords = static_cast<int32>(ftcore::kActionButtonCount) + 2; // the four Tap words, HOLD, MASH
const FTUiText kVictoryUi(TEXT("battle.victory"));
const FTUiText kDefeatUi(TEXT("battle.defeat"));
const FTUiText kAgainUi(TEXT("battle.again"));
const FTUiText kAutoPlayUi(TEXT("arena.autoplay"));
const FTUiText kIdleUi(TEXT("arena.idle"));
const FTUiText kStoryHintUi(TEXT("story.hint"));
constexpr double kImpactLife = 0.3;    // the tell's flash once the move is active
constexpr double kHitEdgeLife = 0.35;  // the edge flash when the lead is hit (under 0.4 s)
constexpr double kParryEdgeLife = 0.4;
constexpr double kResultFade = 0.3;

// 1 at Time, falling to 0 over Life (0 before and after).
float Pulse(double Now, double Time, double Life)
{
	const double Age = Now - Time;
	return (Age >= 0.0 && Age < Life) ? static_cast<float>(1.0 - Age / Life) : 0.f;
}

// ---- the style table ------------------------------------------------------------------------------------------

FLinearColor LinOf(const ftcore::UiColor& C, float Alpha = 1.f)
{
	const auto To8 = [](float V) { return static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(V, 0.f, 1.f) * 255.f)); };
	FLinearColor L(FColor(To8(C.r), To8(C.g), To8(C.b), 255)); // sRGB to linear
	L.A = FMath::Clamp(C.a * Alpha, 0.f, 1.f);
	return L;
}

// Data/UI/style.json once (the built-in style when it is missing or refused), and its colours for the world shown.
struct FUiTable
{
	ftcore::UiStyle Style;
	FLinearColor Token[ftcore::kUiTokenCount];
	FLinearColor Word[ftcore::kUiTokenCount]; // a coloured word's colour: lifted to the style's luminance floor
	FLinearColor Edge;
};

const FUiTable& Table()
{
	static const FUiTable Loaded = []()
	{
		FUiTable T;
		T.Style = ftcore::defaultUiStyle();
		FString Text;
		const FString File = FPaths::ProjectDir() / TEXT("Data/UI/style.json");
		if (FFileHelper::LoadFileToString(Text, *File))
		{
			std::string Error;
			if (!ftcore::importUiStyle(FT::ToUtf8(Text), T.Style, &Error))
			{
				UE_LOG(LogTemp, Warning, TEXT("FTNewUi: %s: %s (the built-in style is used)"), *File, *FT::ToFString(Error));
			}
		}
		// The world's own colours arrive with the world flag on the screens; Solhaven until then.
		for (size_t I = 0; I < ftcore::kUiTokenCount; ++I)
		{
			const ftcore::UiColor C = ftcore::uiColor(T.Style, ftcore::UiWorld::Solhaven, ftcore::UiPalette::Default, static_cast<UiToken>(I));
			T.Token[I] = LinOf(C);
			T.Word[I] = LinOf(ftcore::uiLumaFloor(C, T.Style.wordLumaFloor));
		}
		T.Edge = LinOf(T.Style.edge);
		return T;
	}();
	return Loaded;
}

FLinearColor Tok(UiToken T, float Alpha = 1.f)
{
	FLinearColor C = Table().Token[static_cast<size_t>(T)];
	C.A *= Alpha;
	return C;
}

// Every mark and glyph is a pale core over a tight soft dark contour that follows its shape (no plate, nothing wider than
// the mark). A dim or resting core lowers its own alpha or weight; the contour holds at full strength down to the rest
// alpha and fades below it only as the mark goes away.
constexpr float kContourGrow = 1.5f;    // px past the core's edge at 1080p where the contour is half gone
constexpr float kContourFeather = 2.f;  // px over which it falls to nothing
constexpr float kContourAlpha = 0.67f;  // of the edge colour's alpha (0.9): about 60 percent
constexpr float kContourSoft = 0.5f;    // a word's outer contour pixel, of its inner one
constexpr float kShadowEach = 0.205f;   // each of a HUD glyph's four shadow passes: 1 - (1 - 0.205)^4 = 60 percent inside

float ContourK(float CoreAlpha)
{
	const float Rest = Table().Style.fade.restAlpha;
	// Full from the rest alpha up; below it, falls faster than the core so a fading mark never ends as a dark ghost.
	const float K = Rest > 0.01f ? FMath::Min(1.f, CoreAlpha / Rest) : CoreAlpha;
	return K * K;
}

// The UI's faces, read once from Content/UI/Fonts (OFL, each beside its licence): the serif and its small caps for
// display and labels, the sans for key caps and figures only. The small-caps files are baked by
// tools/bake_small_caps.py (Slate shapes Latin kerning-only, so the font's own substitution would never run). A missing
// file leaves that face on the engine's Roboto, logged once.
const FName FaceName(TEXT("Face")); // each composite holds one typeface entry, named

struct FUiFaces
{
	TSharedPtr<const FCompositeFont> Font[ftcore::kUiFontCount];
};

const FUiFaces& Faces()
{
	static FUiFaces Built = []
	{
		FUiFaces F;
		const FString Dir = FPaths::ProjectContentDir() / TEXT("UI/Fonts");
		const TCHAR* Files[ftcore::kUiFontCount] = {
			TEXT("SourceSans3/SourceSans3-Regular.ttf"),   // Sans
			TEXT("SourceSans3/SourceSans3-Semibold.ttf"),  // SansBold
			TEXT("Spectral/Spectral-Medium.ttf"),          // Serif
			nullptr,                                       // Hand: the placeholder italic
			TEXT("Spectral/Spectral-SemiBold.ttf"),        // SerifBold
			TEXT("Spectral/SpectralSCBaked-Medium.ttf"),   // SerifSc
			TEXT("Spectral/SpectralSCBaked-SemiBold.ttf"), // SerifScBold
		};
		int32 Loaded = 0;
		for (size_t I = 0; I < ftcore::kUiFontCount; ++I)
		{
			if (!Files[I])
			{
				continue;
			}
			const FString Path = Dir / Files[I];
			if (IFileManager::Get().FileSize(*Path) < 4096) // missing (-1), or an unfetched LFS pointer (tiny)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("UI: font %s missing; that face stays on Roboto."), *Path);
				continue;
			}
			F.Font[I] = MakeShared<FStandaloneCompositeFont>(FaceName, Path, EFontHinting::Default, EFontLoadingPolicy::LazyLoad);
			++Loaded;
		}
		UE_LOG(LogForgottenThird, Log, TEXT("UI: %d of 6 font faces loaded from %s."), Loaded, *Dir);
		return F;
	}();
	return Built;
}

// Every glyph carries the hairline edge.
FSlateFontInfo FontFor(UiTextRole Role, float Height, float TextScale = 1.f)
{
	const ftcore::UiStyle& S = Table().Style;
	const ftcore::UiTextStyle& T = S.text[static_cast<size_t>(Role)];
	const float Px = ftcore::uiTextPx(S, Role, Height, TextScale);
	const TSharedPtr<const FCompositeFont>& Face = Faces().Font[static_cast<size_t>(T.font)];
	const bool bBold = T.font == ftcore::UiFont::SansBold || T.font == ftcore::UiFont::SerifBold || T.font == ftcore::UiFont::SerifScBold;
	const TCHAR* Fallback = bBold ? TEXT("Bold") : (T.font == ftcore::UiFont::Hand ? TEXT("Italic") : TEXT("Regular"));
	// Slate sizes are points at 96 dpi.
	FSlateFontInfo F = Face.IsValid() ? FSlateFontInfo(Face, Px * 0.75f, FaceName) : FCoreStyle::GetDefaultFontStyle(Fallback, Px * 0.75f);
	F.LetterSpacing = FMath::RoundToInt(T.tracking * 1000.f);
	// -FTUiEdge=<0..1> scales the glyph edge's strength (0 drops it): a capture switch for judging the edge, not a setting.
	static const float EdgeScale = []
	{
		float V = 1.f;
		FParse::Value(FCommandLine::Get(), TEXT("-FTUiEdge="), V);
		return FMath::Clamp(V, 0.f, 1.f);
	}();
	F.OutlineSettings.OutlineSize = EdgeScale <= 0.f ? 0 : FMath::Max(1, FMath::RoundToInt(ftcore::uiEdgePx(S, Height)));
	F.OutlineSettings.OutlineColor = Table().Edge;
	F.OutlineSettings.OutlineColor.A *= S.edgeText * EdgeScale; // lighter than the strokes' edge: words never read doubled
	return F;
}

// A role's face and edge at a set size in 1080p px (the fight HUD's sizes), never under 14 px on the screen.
FSlateFontInfo FontPx(UiTextRole Role, float Height, float Px1080)
{
	FSlateFontInfo F = FontFor(Role, Height);
	F.Size = FMath::Max(Px1080 * Height / 1080.f, 14.f) * 0.75f;
	return F;
}

TSharedRef<FSlateFontMeasure> FontMeasure()
{
	return FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
}

// ---- the fight HUD's laid-out words ----------------------------------------------------------------------------------

// A word drawn by MakeText is copied into its element on every pass of every frame (the shadow's four and the face). The
// fight HUD shapes each word once per pass instead and keeps the glyph runs while its string, font, size and scale stay
// (MakeShapedText holds a reference to the run: nothing is copied per frame). A slot is found by the word's hash among
// kTextProbe neighbours; a new word takes the least recently drawn of them. A pass shaped while one of its faces was still
// loading is shaped again when next drawn, and all are dropped when Slate releases its font resources.
constexpr int32 kTextPasses = 7; // the shadow's glyph pass, its three outline passes, the soft contour, the face, the faded face
constexpr int32 kTextSlots = 256;
constexpr int32 kTextProbe = 8;
static_assert((kTextSlots & (kTextSlots - 1)) == 0, "the slot index is the hash masked");
static_assert(kTextPasses <= 8, "Stale holds one bit per pass");

struct FTextRun
{
	FString Str;          // the word (the range drawn)
	FSlateFontInfo Font;  // its font as passed (each pass's font is made from it)
	float Scale = 0.f;    // the geometry's scale: the glyphs' pixel size
	float Unit = 0.f;     // the shadow's outline step
	uint32 Hash = 0;
	uint32 Drawn = 0;     // the frame it was last drawn in (0: empty)
	uint8 Stale = 0;      // the passes shaped while a face was loading
	FShapedGlyphSequencePtr Pass[kTextPasses];
};

struct FTextRuns
{
	FTextRun Slot[kTextSlots];
	uint32 Frame = 1;
	std::atomic<bool> bFlush{false}; // set by Slate's font release (from either thread), taken at the next frame

	void BeginFrame()
	{
		if (bFlush.exchange(false))
		{
			for (FTextRun& R : Slot)
			{
				R = FTextRun();
			}
		}
		++Frame;
	}

	FTextRun& Find(const TCHAR* Chars, int32 Len, const FSlateFontInfo& Font, float Scale, float Unit)
	{
		const uint32 H = HashCombine(FCrc::MemCrc32(Chars, Len * static_cast<int32>(sizeof(TCHAR))),
			HashCombine(HashCombine(GetTypeHash(Font.Size), GetTypeHash(Font.TypefaceFontName)), HashCombine(GetTypeHash(Scale), GetTypeHash(Unit))));
		FTextRun* Oldest = nullptr;
		for (int32 I = 0; I < kTextProbe; ++I)
		{
			FTextRun& R = Slot[(H + static_cast<uint32>(I)) & static_cast<uint32>(kTextSlots - 1)];
			if (R.Drawn != 0 && R.Hash == H && R.Scale == Scale && R.Unit == Unit && R.Str.Len() == Len
				&& FMemory::Memcmp(*R.Str, Chars, static_cast<SIZE_T>(Len) * sizeof(TCHAR)) == 0 && R.Font.IsIdenticalTo(Font))
			{
				R.Drawn = Frame;
				return R;
			}
			if (!Oldest || R.Drawn < Oldest->Drawn)
			{
				Oldest = &R;
			}
		}
		FTextRun& R = *Oldest;
		R.Str.Reset();
		R.Str.AppendChars(Chars, Len);
		R.Font = Font;
		R.Scale = Scale;
		R.Unit = Unit;
		R.Hash = H;
		R.Drawn = Frame;
		R.Stale = 0;
		for (FShapedGlyphSequencePtr& P : R.Pass)
		{
			P.Reset();
		}
		return R;
	}

	// The pass's glyph run in its font, shaped on its first draw (and again after a draw with a face still loading).
	static FShapedGlyphSequenceRef Shaped(FTextRun& R, int32 Pass, const FSlateFontInfo& F)
	{
		const uint8 Bit = static_cast<uint8>(1u << Pass);
		if (!R.Pass[Pass].IsValid() || (R.Stale & Bit) != 0)
		{
			TArray<TWeakPtr<FFreeTypeFace>> Loading;
			R.Pass[Pass] = FSlateApplication::Get().GetRenderer()->GetFontCache()->ShapeUnidirectionalText(R.Str, F, R.Scale,
				TextBiDi::ETextDirection::LeftToRight, ETextShapingMethod::KerningOnly, &Loading);
			R.Stale = Loading.Num() > 0 ? static_cast<uint8>(R.Stale | Bit) : static_cast<uint8>(R.Stale & ~Bit);
		}
		return R.Pass[Pass].ToSharedRef();
	}
};

// The fight HUD keeps its laid-out words but under -FTUiTextNoCache (a check, never in shipping): every pass lays out
// its string again, as MakeText does.
bool UiTextCacheOn()
{
#if !UE_BUILD_SHIPPING
	static const bool bOff = FParse::Param(FCommandLine::Get(), TEXT("FTUiTextNoCache"));
	return !bOff;
#else
	return true;
#endif
}

// The fight HUD's heap use over one frame (a check, compiled out of shipping and off by default): -FTUiAllocCount counts
// the game thread's heap calls (malloc, realloc, free) inside the HUD's Tick and OnPaint on the 120th frame of a fight
// (-FTUiAllocCount=N: the Nth) and logs them once after that paint.
#if FT_UI_ALLOC_COUNT
struct FUiAllocCount
{
	int32 Frame = 0;          // the live frame counted (0: off)
	int32 Seen = 0;           // live frames so far
	int32 Bucket = 0;         // 0 Tick, 1 OnPaint
	uint32 Calls[2][3] = {};  // per bucket: malloc, realloc, free
	bool bDone = false;
	TFunction<void(int32)> Hook;

	bool Counting() const { return Frame > 0 && !bDone && Seen == Frame; }
};

FUiAllocCount& UiAllocCount()
{
	static FUiAllocCount Count;
	static bool bInit = false;
	if (!bInit)
	{
		bInit = true;
		int32 N = 0;
		if (FParse::Value(FCommandLine::Get(), TEXT("-FTUiAllocCount="), N))
		{
			Count.Frame = FMath::Max(1, N);
		}
		else if (FParse::Param(FCommandLine::Get(), TEXT("FTUiAllocCount")))
		{
			Count.Frame = 120;
		}
		if (Count.Frame > 0)
		{
			FUiAllocCount* C = &Count;
			Count.Hook = [C](int32 Index) { ++C->Calls[C->Bucket][FMath::Clamp(Index, 0, 2)]; }; // made before any counting
		}
	}
	return Count;
}

// Hooks the game thread's heap calls for its life when bOn; after the paint's, logs the frame's counts once.
struct FUiAllocScope
{
	FUiAllocCount& C;
	const bool bOn;

	FUiAllocScope(FUiAllocCount& InC, int32 InBucket, bool bInOn)
		: C(InC)
		, bOn(bInOn && GGameThreadMallocHook == nullptr)
	{
		if (bOn)
		{
			C.Bucket = InBucket;
			GGameThreadMallocHook = &C.Hook;
		}
	}

	~FUiAllocScope()
	{
		if (!bOn)
		{
			return;
		}
		GGameThreadMallocHook = nullptr;
		if (C.Bucket == 1)
		{
			C.bDone = true;
			UE_LOG(LogForgottenThird, Display,
				TEXT("FTUiAllocCount: fight HUD frame %d, text cache %s: Tick %u malloc, %u realloc, %u free; OnPaint %u malloc, %u realloc, %u free."),
				C.Frame, UiTextCacheOn() ? TEXT("on") : TEXT("off"), C.Calls[0][0], C.Calls[0][1], C.Calls[0][2], C.Calls[1][0], C.Calls[1][1],
				C.Calls[1][2]);
		}
	}
};
#endif

// ---- painting helpers (boxes only: no per-frame arrays) ------------------------------------------------------------

struct FPainter
{
	const FGeometry& Geometry;
	FSlateWindowElementList& Out;
	int32 Layer;
	float S; // 1080p units to local
	bool bShadow = false; // the fight HUD's words: a soft drop shadow under each glyph (Text), drawn on the base layer
	FTextRuns* Runs = nullptr; // the fight HUD's kept glyph runs (FTextRuns); null: each pass lays its string out (MakeText)

	const FSlateBrush* White() const { return FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")); }

	void Box(float X, float Y, float W, float H, const FLinearColor& C, int32 L = 1) const
	{
		if (W <= 0.f || H <= 0.f || C.A <= 0.f)
		{
			return;
		}
		FSlateDrawElement::MakeBox(Out, Layer + L, Geometry.ToPaintGeometry(FVector2f(W, H), FSlateLayoutTransform(FVector2f(X, Y))), White(),
			ESlateDrawEffect::None, C);
	}

	void Hollow(float X, float Y, float W, float H, float T, const FLinearColor& C) const
	{
		Box(X, Y, W, T, C);
		Box(X, Y + H - T, W, T, C);
		Box(X, Y + T, T, H - 2.f * T, C);
		Box(X + W - T, Y + T, T, H - 2.f * T, C);
	}

	// Shade with no edge: stacked boxes growing outward, so the middle reaches Peak and the rim fades to nothing.
	void Soft(float CX, float CY, float W, float H, float Peak, float Alpha = 1.f) const
	{
		// Many thin, round-ended (anti-aliased) bands so no step or corner reads: the backing must never show as an object.
		static const FSlateRoundedBoxBrush Round(FLinearColor::White, 256.f);
		// One brush on one layer, so Slate batches them into a single draw; 48 keeps each band under ~15 px on a wide pool
		// of shade (24 showed as rings over a bright lamp).
		constexpr int32 Bands = 48;
		const float P = FMath::Clamp(Peak * Alpha, 0.f, 0.99f);
		const float Each = 1.f - FMath::Pow(1.f - P, 1.f / static_cast<float>(Bands));
		if (Each <= 0.f || W <= 0.f || H <= 0.f)
		{
			return;
		}
		const FLinearColor C = Tok(UiToken::Backing, 0.f) + FLinearColor(0.f, 0.f, 0.f, Each);
		for (int32 I = 0; I < Bands; ++I)
		{
			// Bands spaced by the inverse of a smoothstep over the outer part: an even middle, a soft fall, no cone.
			const float Y = (static_cast<float>(I) + 0.5f) / static_cast<float>(Bands);
			const float T = 0.55f + 0.45f * (0.5f - FMath::Sin(FMath::Asin(1.f - 2.f * Y) / 3.f));
			const float BW = W * T;
			const float BH = H * T;
			FSlateDrawElement::MakeBox(Out, Layer, Geometry.ToPaintGeometry(FVector2f(BW, BH), FSlateLayoutTransform(FVector2f(CX - BW * 0.5f, CY - BH * 0.5f))),
				&Round, ESlateDrawEffect::None, C);
		}
	}

	void Diamond(float CX, float CY, float Size, const FLinearColor& C) const
	{
		const float Side = Size * 0.7071f;
		FSlateDrawElement::MakeRotatedBox(Out, Layer + 2,
			Geometry.ToPaintGeometry(FVector2f(Side, Side), FSlateLayoutTransform(FVector2f(CX - Side * 0.5f, CY - Side * 0.5f))), White(),
			ESlateDrawEffect::None, UE_PI * 0.25f, TOptional<FVector2f>(), FSlateDrawElement::RelativeToElement, C);
	}

	// Pointing up, from five bars.
	void Triangle(float CX, float CY, float Size, const FLinearColor& C) const
	{
		constexpr int32 Rows = 5;
		const float RowH = Size / Rows;
		for (int32 I = 0; I < Rows; ++I)
		{
			const float W = Size * static_cast<float>(I + 1) / Rows;
			Box(CX - W * 0.5f, CY - Size * 0.5f + RowH * I, W, RowH + 0.5f, C, 2);
		}
	}

	void Circle(float CX, float CY, float D, const FLinearColor& C) const
	{
		static const FSlateRoundedBoxBrush Round(FLinearColor::White, 256.f);
		FSlateDrawElement::MakeBox(Out, Layer + 2, Geometry.ToPaintGeometry(FVector2f(D, D), FSlateLayoutTransform(FVector2f(CX - D * 0.5f, CY - D * 0.5f))),
			&Round, ESlateDrawEffect::None, C);
	}

	// A chevron pointing right: two rotated bars.
	void Chevron(float CX, float CY, float Size, const FLinearColor& C) const
	{
		const float Len = Size * 0.7f;
		const float T = FMath::Max(2.f, Size * 0.18f);
		for (int32 Sign = -1; Sign <= 1; Sign += 2)
		{
			const float Y = CY + Sign * Size * 0.22f - T * 0.5f;
			FSlateDrawElement::MakeRotatedBox(Out, Layer + 2,
				Geometry.ToPaintGeometry(FVector2f(Len, T), FSlateLayoutTransform(FVector2f(CX - Len * 0.5f, Y))), White(), ESlateDrawEffect::None,
				-Sign * UE_PI * 0.25f, TOptional<FVector2f>(), FSlateDrawElement::RelativeToElement, C);
		}
	}

	void Text(float X, float Y, const FString& Str, const FSlateFontInfo& Font, const FLinearColor& C, int32 Start = 0, int32 End = -1) const
	{
		if (Str.IsEmpty() || C.A <= 0.f)
		{
			return;
		}
		const float Unit = static_cast<float>(FMath::Max(1, FMath::RoundToInt(S))); // the shadow's outline step
		const int32 From = End < 0 ? 0 : FMath::Clamp(Start, 0, Str.Len());
		const int32 To = End < 0 ? Str.Len() : FMath::Clamp(End, From, Str.Len());
		FTextRun* Run = Runs && To > From ? &Runs->Find(*Str + From, To - From, Font, Geometry.Scale, Unit) : nullptr;
		// Each pass by its index in the run (FTextRuns): the shadow's glyphs 0, its outlines 1 to 3, the soft contour 4, the
		// face 5 (6 faded: its edge kept out of the letter).
		const auto Draw = [&](int32 Pass, const FSlateFontInfo& F, const FLinearColor& Tint, float Back, float Drop = 0.f, int32 L = 3)
		{
			if (Run)
			{
				// A shaped run's box leaves the outline out and a laid-out string's takes it in: set down by it, so both
				// stand alike.
				const float Fix = Geometry.Scale > 0.f
					? FMath::FloorToFloat(static_cast<float>(F.OutlineSettings.OutlineSize) * Geometry.Scale) / Geometry.Scale
					: 0.f;
				const FPaintGeometry RunGeo = Geometry.ToPaintGeometry(FVector2f(4096.f, 512.f),
					FSlateLayoutTransform(FVector2f(X - Back, Y - Back + Drop + Fix)));
				FSlateDrawElement::MakeShapedText(Out, Layer + L, RunGeo, FTextRuns::Shaped(*Run, Pass, F), ESlateDrawEffect::None, Tint,
					F.OutlineSettings.OutlineColor);
				return;
			}
			const FPaintGeometry Geo = Geometry.ToPaintGeometry(FVector2f(4096.f, 512.f), FSlateLayoutTransform(FVector2f(X - Back, Y - Back + Drop)));
			if (End < 0)
			{
				FSlateDrawElement::MakeText(Out, Layer + L, Geo, Str, F, ESlateDrawEffect::None, Tint);
			}
			else if (End > Start)
			{
				FSlateDrawElement::MakeText(Out, Layer + L, Geo, Str, Start, End, F, ESlateDrawEffect::None, Tint);
			}
		};
		// The font's own 1 px edge is the contour's inner pixel; a second pass one pixel wider, fainter and with no face, its
		// soft outer one. A faded face keeps its edge out of the letter (separate fill alpha), so it never greys.
		FSlateFontInfo InnerF = Font;
		InnerF.OutlineSettings.OutlineColor.A *= ContourK(C.A);
		InnerF.OutlineSettings.bSeparateFillAlpha = C.A < 0.999f;
		const int32 Edge = Font.OutlineSettings.OutlineSize;
		if (bShadow)
		{
			// The soft shadow (2 px down, about 4 px of blur, 60 percent black), under every line and word of the painter: the
			// glyph's own shape and three outline passes 1, 2 and 3 px wider (outline only, fill included, no face), each about
			// a fifth black, so they stack to 60 percent inside the letter and fall to nothing over 3 to 4 px round it. Slate
			// sets an outlined glyph down and right by its outline's width, so each pass is set back by that width less the
			// main pass's, and lies under the letter itself. It takes the place of the soft outer contour pixel.
			const float Drop = 2.f * S;
			const float A = kShadowEach * ContourK(C.A);
			FSlateFontInfo ShadeF = Font;
			ShadeF.OutlineSettings.OutlineSize = 0;
			Draw(0, ShadeF, FLinearColor(0.f, 0.f, 0.f, A), -static_cast<float>(Edge), Drop, 0);
			ShadeF.OutlineSettings.bSeparateFillAlpha = false;
			ShadeF.OutlineSettings.OutlineColor = FLinearColor(0.f, 0.f, 0.f, A);
			for (int32 K = 1; K <= 3; ++K)
			{
				ShadeF.OutlineSettings.OutlineSize = K * static_cast<int32>(Unit);
				Draw(K, ShadeF, FLinearColor(0.f, 0.f, 0.f, 0.004f), K * Unit - static_cast<float>(Edge), Drop, 0);
			}
		}
		else if (Edge > 0)
		{
			FSlateFontInfo OuterF = InnerF;
			OuterF.OutlineSettings.OutlineSize += 1;
			OuterF.OutlineSettings.OutlineColor.A *= kContourSoft;
			OuterF.OutlineSettings.bSeparateFillAlpha = true;
			// Slate sets an outlined glyph down and right by the outline's growth (measured: 1 px); set the wider pass back so it rings
			// the letter evenly instead of reading as a drop shadow.
			Draw(4, OuterF, FLinearColor(C.R, C.G, C.B, 0.004f), 1.f); // a near-zero face: Slate culls a fully clear one
		}
		Draw(InnerF.OutlineSettings.bSeparateFillAlpha ? 6 : 5, InnerF, C, 0.f);
	}

	// A meter in the style's painter. Ring and Stroke are drawn as Line until their painters land with the pick.
	void Meter(float X, float Y, float W, float Thick, float Fraction, const FLinearColor& Fill, float Alpha) const
	{
		const float F = FMath::Clamp(Fraction, 0.f, 1.f);
		const FLinearColor Track = Tok(UiToken::Track, Alpha);
		if (Table().Style.meter == ftcore::UiMeterShape::Teeth)
		{
			const float Pitch = 9.f * S;
			const int32 Teeth = FMath::Max(4, FMath::FloorToInt(W / Pitch));
			const float ToothW = FMath::Max(1.f, Pitch * 0.4f);
			const int32 Lit = FMath::RoundToInt(F * Teeth);
			for (int32 I = 0; I < Teeth; ++I)
			{
				Box(X + I * (W / Teeth), Y - Thick * 2.f, ToothW, Thick * 4.f, I < Lit ? Fill : Track);
			}
			return;
		}
		Box(X, Y, W, Thick, Track);
		Box(X, Y, W * F, Thick, Fill, 2);
	}
};

float TextWidth(const FString& Str, const FSlateFontInfo& Font)
{
	return Str.IsEmpty() ? 0.f : static_cast<float>(FontMeasure()->Measure(Str, Font).X);
}

// The middle of a word's ink, measured from where the word is drawn. Measure gives the box of the advances, and the ink
// is not centred in it: the first letter's left bearing and the last letter's right bearing differ, and the engine moves
// an outlined word's fill and outline right by the outline's size. A word centred by its box sits that far off the
// axis (about 1.5 px for PARRY), so a centred word is placed by this instead. Built with the fonts, never per frame.
float InkMid(const FString& Str, const FSlateFontInfo& Font)
{
	const float W = TextWidth(Str, Font);
	if (Str.IsEmpty())
	{
		return 0.f;
	}
	FCharacterList& Chars = FSlateApplication::Get().GetRenderer()->GetFontCache()->GetCharacterList(Font, 1.f);
	const FCharacterEntry& First = Chars.GetCharacter(Str[0], Font.FontFallback);
	if (!First.Valid)
	{
		return 0.5f * W;
	}
	const float Left = static_cast<float>(First.HorizontalOffset); // read before the next lookup, which may move the entry
	const FCharacterEntry& Last = Chars.GetCharacter(Str[Str.Len() - 1], Font.FontFallback);
	if (!Last.Valid)
	{
		return 0.5f * W;
	}
	const float RightBearing = static_cast<float>(Last.XAdvance) - (static_cast<float>(Last.HorizontalOffset) + Last.USize * Last.BitmapRenderScale);
	const float Shift = static_cast<float>(Font.OutlineSettings.OutlineSize);
	return Shift + 0.5f * (Left + W - RightBearing);
}

// ---- the line mesh: every stroke, ring, mark and soft shade of a widget in one custom-verts element ------------------
//
// Core gives each line's points (UiStroke: place, half width, alpha); this turns them into triangles with a soft 1 px
// edge, a dark under-edge beneath (the glyph edge's twin, so a line holds on the sky), and draws the lot in one
// element: shades first, then every under-edge, then every line, so one line's dark edge never cuts another's core.
// The arrays are kept between frames (Reset, not freed): no allocation once warm. The engine copies them per element.
class FLineMesh
{
public:
	void Begin(const FGeometry& Geometry, float InS)
	{
		Transform = Geometry.GetAccumulatedRenderTransform();
		S = InS;
		for (FPart& P : Parts)
		{
			P.Verts.Reset();
			P.Idx.Reset();
		}
		if (!bHandle)
		{
			bHandle = true;
			Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")));
			const FSlateShaderResourceProxy* Proxy = Handle.GetResourceProxy();
			UV = Proxy ? FVector2f(Proxy->StartUV + Proxy->SizeUV * 0.5f) : FVector2f(0.5f, 0.5f);
		}
	}

	// A line from Core. Closed: the first and last points are the same place (a ring), joined without a seam.
	void Stroke(const ftcore::UiStroke& St, const FLinearColor& C, bool bClosed = false)
	{
		if (St.count < 2 || C.A <= 0.f)
		{
			return;
		}
		Ribbon(Parts[Under], St, Table().Edge, kContourAlpha * ContourK(C.A), kContourGrow * S, kContourFeather * S, bClosed);
		Ribbon(Parts[Core], St, C, 1.f, 0.f, 0.8f, bClosed);
	}

	// A value line (health, guard, poise, a timer): its core with a crisp 1 px dark edge along both sides (about 60 percent,
	// its fall under a pixel) in place of the soft contour, so the line's own width reads exactly.
	void ValueStroke(const ftcore::UiStroke& St, const FLinearColor& C)
	{
		if (St.count < 2 || C.A <= 0.f)
		{
			return;
		}
		Ribbon(Parts[Under], St, Table().Edge, kContourAlpha * ContourK(C.A), 1.f * S, 0.8f * S, false);
		Ribbon(Parts[Core], St, C, 1.f, 0.f, 0.8f, false);
	}

	// A filled convex shape (a dot, a lozenge, a small triangle), edged like the lines.
	void Fill(const FVector2f* Pts, int32 N, const FLinearColor& C)
	{
		if (N < 3 || C.A <= 0.f)
		{
			return;
		}
		Convex(Parts[Under], Pts, N, Table().Edge, kContourAlpha * ContourK(C.A), kContourGrow * S, kContourFeather * S);
		Convex(Parts[Core], Pts, N, C, 1.f, 0.f, 0.8f);
	}

	void Dot(float CX, float CY, float R, const FLinearColor& C)
	{
		FVector2f Pts[16];
		for (int32 I = 0; I < 16; ++I)
		{
			const float A = I * (2.f * UE_PI / 16.f);
			Pts[I] = FVector2f(CX + R * FMath::Cos(A), CY + R * FMath::Sin(A));
		}
		Fill(Pts, 16, C);
	}

	// A vignette with no edge at all: an ellipse centred on a screen corner or edge (most of it off the screen) whose alpha
	// falls from Peak at its centre to nothing at the rim, with zero slope at both ends, so no boundary or patch reads.
	// Flat: the part of the way out that stays at Peak before the fall starts (0: the fall starts at the centre).
	void Vignette(float CX, float CY, float RX, float RY, float Peak, const FLinearColor* Colour = nullptr, float Flat = 0.f)
	{
		if (Peak <= 0.f || RX <= 0.f || RY <= 0.f)
		{
			return;
		}
		constexpr int32 Rings = 24;
		constexpr int32 Segs = 48;
		FPart& P = Parts[ShadePart];
		const SlateIndex Base = static_cast<SlateIndex>(P.Verts.Num());
		const FLinearColor Back = Colour ? *Colour : Tok(UiToken::Backing, 1.f);
		P.Verts.Add(Vert(CX, CY, Back, Peak));
		for (int32 R = 1; R <= Rings; ++R)
		{
			const float T = static_cast<float>(R) / Rings;
			const float U = FMath::Clamp((T - Flat) / (1.f - Flat), 0.f, 1.f);
			const float A = Peak * FMath::Square(1.f - U * U); // flat, then zero slope at the rim: no edge or blob reads
			for (int32 K = 0; K < Segs; ++K)
			{
				const float Ang = K * (2.f * UE_PI / Segs);
				P.Verts.Add(Vert(CX + RX * T * FMath::Cos(Ang), CY + RY * T * FMath::Sin(Ang), Back, A));
			}
		}
		for (int32 K = 0; K < Segs; ++K)
		{
			const int32 K1 = (K + 1) % Segs;
			Tri(P, Base, Base + 1 + K, Base + 1 + K1);
			for (int32 R = 1; R < Rings; ++R)
			{
				const int32 I0 = Base + 1 + (R - 1) * Segs;
				const int32 I1 = I0 + Segs;
				Tri(P, I0 + K, I1 + K, I1 + K1);
				Tri(P, I0 + K, I1 + K1, I0 + K1);
			}
		}
	}

	// A gradient strip with no edge: alpha Peak along one side (bPeakAtEnd: the bottom or right side), even for the first
	// 1 - Ramp of the way, then falling to nothing at the other side with zero slope at both ends of the fall.
	void Band(float X0, float Y0, float X1, float Y1, bool bAlongY, bool bPeakAtEnd, float Ramp, float Peak, const FLinearColor& C)
	{
		if (Peak <= 0.f || X1 <= X0 || Y1 <= Y0)
		{
			return;
		}
		constexpr int32 Steps = 32;
		FPart& P = Parts[ShadePart];
		const int32 Base = P.Verts.Num();
		const float R = FMath::Clamp(Ramp, 0.05f, 1.f);
		for (int32 I = 0; I <= Steps; ++I)
		{
			const float T = static_cast<float>(I) / Steps;
			const float D = bPeakAtEnd ? 1.f - T : T; // the share of the way from the peak side
			const float U = FMath::Clamp((D - (1.f - R)) / R, 0.f, 1.f);
			const float A = Peak * (1.f - U * U * (3.f - 2.f * U));
			if (bAlongY)
			{
				const float Y = FMath::Lerp(Y0, Y1, T);
				P.Verts.Add(Vert(X0, Y, C, A));
				P.Verts.Add(Vert(X1, Y, C, A));
			}
			else
			{
				const float X = FMath::Lerp(X0, X1, T);
				P.Verts.Add(Vert(X, Y0, C, A));
				P.Verts.Add(Vert(X, Y1, C, A));
			}
		}
		for (int32 I = 0; I < Steps; ++I)
		{
			const int32 I0 = Base + 2 * I;
			Tri(P, I0, I0 + 1, I0 + 3);
			Tri(P, I0, I0 + 3, I0 + 2);
		}
	}

	void Flush(FSlateWindowElementList& Out, int32 Layer)
	{
		All.Reset();
		AllIdx.Reset();
		for (const FPart& P : Parts)
		{
			const SlateIndex Off = static_cast<SlateIndex>(All.Num());
			All.Append(P.Verts);
			for (const SlateIndex I : P.Idx)
			{
				AllIdx.Add(I + Off);
			}
		}
		if (AllIdx.Num() > 0)
		{
			FSlateDrawElement::MakeCustomVerts(Out, Layer, Handle, All, AllIdx, nullptr, 0, 0);
		}
	}

private:
	enum EPart { ShadePart, Under, Core, PartCount };
	struct FPart
	{
		TArray<FSlateVertex> Verts;
		TArray<SlateIndex> Idx;
	};

	FSlateVertex Vert(float X, float Y, const FLinearColor& C, float A) const
	{
		FLinearColor L = C;
		L.A = FMath::Clamp(A, 0.f, 1.f);
		return FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, FVector2f(X, Y), UV, L.ToFColor(true));
	}

	static void Tri(FPart& P, int32 A, int32 B, int32 C)
	{
		P.Idx.Add(static_cast<SlateIndex>(A));
		P.Idx.Add(static_cast<SlateIndex>(B));
		P.Idx.Add(static_cast<SlateIndex>(C));
	}

	// Four verts across each point (outer edge clear, two core, outer edge clear) and three quads between points.
	void Ribbon(FPart& P, const ftcore::UiStroke& St, const FLinearColor& C, float AlphaK, float Grow, float Feather, bool bClosed)
	{
		const int32 N = static_cast<int32>(St.count);
		const int32 Base = P.Verts.Num();
		for (int32 I = 0; I < N; ++I)
		{
			const ftcore::UiStrokePoint& Pt = St.pt[I];
			int32 Prev = I - 1;
			int32 Next = I + 1;
			if (bClosed)
			{
				Prev = I == 0 ? N - 2 : Prev;
				Next = I == N - 1 ? 1 : Next;
			}
			const ftcore::UiStrokePoint& A = St.pt[FMath::Clamp(Prev, 0, N - 1)];
			const ftcore::UiStrokePoint& B = St.pt[FMath::Clamp(Next, 0, N - 1)];
			FVector2f T(B.x - A.x, B.y - A.y);
			T = T.GetSafeNormal();
			const FVector2f Nrm(-T.Y, T.X);
			const float H = Pt.halfWidth + Grow;
			const float Inner = FMath::Max(0.f, H - Feather * 0.5f);
			const float Outer = H + Feather * 0.5f;
			const float Alpha = C.A * AlphaK * Pt.alpha * FMath::Min(1.f, 2.f * H); // a line under 1 px thins in alpha
			const FVector2f O(Pt.x, Pt.y);
			const FVector2f V0 = O - Nrm * Outer;
			const FVector2f V1 = O - Nrm * Inner;
			const FVector2f V2 = O + Nrm * Inner;
			const FVector2f V3 = O + Nrm * Outer;
			P.Verts.Add(Vert(V0.X, V0.Y, C, 0.f));
			P.Verts.Add(Vert(V1.X, V1.Y, C, Alpha));
			P.Verts.Add(Vert(V2.X, V2.Y, C, Alpha));
			P.Verts.Add(Vert(V3.X, V3.Y, C, 0.f));
		}
		for (int32 I = 0; I + 1 < N; ++I)
		{
			const int32 B0 = Base + I * 4;
			const int32 B1 = B0 + 4;
			for (int32 K = 0; K < 3; ++K)
			{
				Tri(P, B0 + K, B0 + K + 1, B1 + K + 1);
				Tri(P, B0 + K, B1 + K + 1, B1 + K);
			}
		}
	}

	void Convex(FPart& P, const FVector2f* Pts, int32 N, const FLinearColor& C, float AlphaK, float Grow, float Feather)
	{
		FVector2f Mid(0.f, 0.f);
		for (int32 I = 0; I < N; ++I)
		{
			Mid += Pts[I];
		}
		Mid /= static_cast<float>(N);
		const float A = C.A * AlphaK;
		const int32 Base = P.Verts.Num();
		P.Verts.Add(Vert(Mid.X, Mid.Y, C, A));
		for (int32 I = 0; I < N; ++I)
		{
			const FVector2f Out = (Pts[I] - Mid).GetSafeNormal();
			const FVector2f In = Pts[I] + Out * (Grow - Feather * 0.5f);
			const FVector2f Rim = Pts[I] + Out * (Grow + Feather * 0.5f);
			P.Verts.Add(Vert(In.X, In.Y, C, A));
			P.Verts.Add(Vert(Rim.X, Rim.Y, C, 0.f));
		}
		for (int32 I = 0; I < N; ++I)
		{
			const int32 J = (I + 1) % N;
			const int32 In0 = Base + 1 + I * 2;
			const int32 In1 = Base + 1 + J * 2;
			Tri(P, Base, In0, In1);
			Tri(P, In0, In0 + 1, In1 + 1);
			Tri(P, In0, In1 + 1, In1);
		}
	}

	FPart Parts[PartCount];
	TArray<FSlateVertex> All;
	TArray<SlateIndex> AllIdx;
	FSlateRenderTransform Transform;
	float S = 1.f;
	FSlateResourceHandle Handle;
	FVector2f UV = FVector2f(0.5f, 0.5f);
	bool bHandle = false;
};

// A hand-placed polyline (a chevron, a key ring's stadium) as a stroke of even weight.
void PolyStroke(ftcore::UiStroke& Out, const FVector2f* Pts, int32 N, float HalfWidth, float Alpha = 1.f)
{
	Out.count = 0;
	for (int32 I = 0; I < N && Out.count < ftcore::kUiStrokeMaxPoints; ++I)
	{
		ftcore::UiStrokePoint& P = Out.pt[Out.count++];
		P.x = Pts[I].X;
		P.y = Pts[I].Y;
		P.halfWidth = HalfWidth;
		P.alpha = Alpha;
	}
}

// A closed rounded box (a key cap) round (CX, CY): W wide, H high, corners of radius Corner; the first point repeats at
// the end.
void KeyCapStroke(ftcore::UiStroke& Out, float CX, float CY, float W, float H, float HalfWidth, float Corner)
{
	const float R = FMath::Clamp(Corner, 0.f, FMath::Min(W, H) * 0.5f);
	const float HX = W * 0.5f - R;
	const float HY = H * 0.5f - R;
	constexpr int32 Arc = 6;
	Out.count = 0;
	const auto Put = [&Out, HalfWidth](float X, float Y)
	{
		ftcore::UiStrokePoint& P = Out.pt[Out.count++];
		P.x = X;
		P.y = Y;
		P.halfWidth = HalfWidth;
		P.alpha = 1.f;
	};
	const float SX[4] = {1.f, 1.f, -1.f, -1.f}; // the corners clockwise from the top right
	const float SY[4] = {-1.f, 1.f, 1.f, -1.f};
	for (int32 K = 0; K < 4; ++K)
	{
		for (int32 I = 0; I <= Arc; ++I)
		{
			const float A = -0.5f * UE_PI + 0.5f * UE_PI * (K + static_cast<float>(I) / Arc);
			Put(CX + SX[K] * HX + R * FMath::Cos(A), CY + SY[K] * HY + R * FMath::Sin(A));
		}
	}
	Put(Out.pt[0].x, Out.pt[0].y);
}

// One shape at every level: even over the lower 40 percent of the screen and fading out over the 40 percent above it,
// with zero slope at both ends of the fall (no line where it ends). EvenTop raises the even part's top (the replies over
// a subtitle stand on it); the fall above it keeps its height while the screen has room.
constexpr float kConvEven = 0.6f; // even below this share of the screen height
constexpr float kConvRamp = 0.4f; // and fading out over this share above it

void PaintLowGradient(FLineMesh& Mesh, const FVector2f& Size, float Level, float EvenTop = -1.f)
{
	const float Even = FMath::Clamp(EvenTop >= 0.f ? FMath::Min(EvenTop, Size.Y * kConvEven) : Size.Y * kConvEven, 0.f, Size.Y);
	const float Top = FMath::Max(0.f, Even - Size.Y * kConvRamp);
	const float Span = Size.Y - Top;
	Mesh.Band(0.f, Top, Size.X, Size.Y, true, true, Span > 0.f ? (Even - Top) / Span : 1.f, Level, Tok(UiToken::Backing, 1.f));
}

// ---- the arena HUD ----------------------------------------------------------------------------------------------------

constexpr uint32 SectionBit(ftcore::ArenaHudSection Sec) { return 1u << static_cast<uint32>(Sec); }

// Whether a stone bearer note wakes the boss line (answers, breaks, crits, lifts, the shock ring). The line shows it as
// motion only; no note draws a word.
bool BossNoteShown(const FFTStoneBearerNote& Note)
{
	if (Note.Kind == FFTStoneBearerNote::EKind::ShockRingSent)
	{
		return true;
	}
	if (Note.Kind != FFTStoneBearerNote::EKind::Run)
	{
		return false;
	}
	switch (Note.Event.type)
	{
	case ftcore::ArenaBossEventType::Answered:
		switch (Note.Event.outcome)
		{
		case ftcore::CalledOutcome::Intercepted:
		case ftcore::CalledOutcome::Guarded:
		case ftcore::CalledOutcome::WrongZone:
		case ftcore::CalledOutcome::Missed: return true;
		default: return false;
		}
	case ftcore::ArenaBossEventType::Broke:
	case ftcore::ArenaBossEventType::Crit:
	case ftcore::ArenaBossEventType::Lifted: return true;
	default: return false;
	}
}

// The boss section from whichever boss host runs this fight.
void FillBoss(const UFTArenaRunner& Runner, ftcore::ArenaHudBoss& Out)
{
	Out = ftcore::ArenaHudBoss{};
	const ftcore::ArenaFight& Fight = Runner.GetFight();
	size_t Slot = 0;
	bool bOpen = false;
	if (const FFTKiteHost* Kite = Runner.GetKite())
	{
		Slot = Kite->GetSlot();
		bOpen = Kite->IsOpen();
	}
	else if (const FFTStoneBearerHost* Bearer = Runner.GetStoneBearer())
	{
		Slot = Bearer->GetSlot();
		const ftcore::StoneBearer& Boss = Bearer->GetBoss();
		const ftcore::ArenaBossRun& Run = Boss.run();
		const ftcore::ArenaBossDef& Def = Bearer->GetDef().boss;
		bOpen = Run.crit().isOpen(Run.now());
		Out.invulnerable = Boss.invulnerable();
		const int32 WeightMax = FMath::Clamp(Bearer->GetDef().weight.cap, 0, 8);
		Out.weightMax = static_cast<std::uint8_t>(WeightMax);
		Out.weight = static_cast<std::uint8_t>(FMath::Clamp(static_cast<int32>(Boss.stacks()), 0, WeightMax));
		// The phases: their floors place the notches on the line.
		const int32 Phases = FMath::Clamp(static_cast<int32>(Def.phaseCount), 1, static_cast<int32>(ftcore::kArenaHudPhases));
		Out.phaseCount = static_cast<std::uint8_t>(Phases);
		Out.phase = static_cast<std::uint8_t>(FMath::Min(static_cast<int32>(Run.phase()), Phases - 1));
		for (int32 I = 0; I < Phases; ++I)
		{
			Out.phaseEnd[static_cast<size_t>(I)] = FMath::Clamp(Def.phases[static_cast<size_t>(I)].endAt, 0.f, 1.f);
		}
		Out.down = Boss.down();
		// A called blow: the zone it wants next, and whether the lead stands in it.
		const ftcore::CalledStrike& Strike = Run.strike();
		const std::uint8_t Live = Run.liveMove();
		if (!Out.invulnerable && !Out.down && Live < Def.moveCount && Def.moves[Live].intercept() && !Strike.complete())
		{
			const ftcore::CalledZone Want = Strike.nextZone();
			Out.call = Want == ftcore::CalledZone::Left ? ftcore::UiCalledZone::Left
				: Want == ftcore::CalledZone::Right   ? ftcore::UiCalledZone::Right
													  : ftcore::UiCalledZone::Path;
			Out.callIn = Bearer->ZoneNow(Runner.GetBodies()) == Want;
		}
		// The latest note that wakes the line.
		for (int32 Serial = Bearer->GetNoteTotal() - 1; Serial >= Bearer->GetNoteTotal() - Bearer->GetNoteCount(); --Serial)
		{
			if (BossNoteShown(Bearer->GetNoteBySerial(Serial)))
			{
				Out.noteSerial = Serial;
				break;
			}
		}
	}
	else
	{
		return;
	}
	if (Slot >= Fight.enemyCount() || !Fight.enemyAlive(Slot))
	{
		Out = ftcore::ArenaHudBoss{};
		return;
	}
	Out.present = true;
	Out.slot = static_cast<std::uint8_t>(Slot);
	Out.nameKey = static_cast<std::uint16_t>(Slot);
	Out.hp = Fight.enemyHp(Slot);
	Out.hpMax = Fight.def().enemies[Slot].hp;
	Out.weakPoint = bOpen ? ftcore::UiWeakPoint::Open : ftcore::UiWeakPoint::Closed;
}

// The fight HUD's places and sizes in 1080p units (scaled by the frame's height). Every mark keeps inside one margin.
namespace Hud
{
constexpr float Margin = 64.f;
constexpr float TopLine = 100.f;     // the boss's line, down from the top
constexpr float TopLen = 600.f;      // the top line, boss or target: one length
constexpr float LeadLine = 260.f;    // the lead's hp and guard lines
constexpr float PartnerLine = 200.f; // the partner's hp line
constexpr float FoeLine = 90.f;      // a minor enemy's hp line over its body
constexpr float CallLine = 160.f;    // the windup line under the call word
constexpr float LowHp = 0.25f;       // under it an hp or guard line turns danger red
// Lines, px at their thickest. A line that carries a value (an amount or the time left) is nowhere thinner than
// MinValueLine, its tapered ends included; a track or a mark may be. A value line holds its full width along 90 percent
// of its drawn length (ValueTaper at each end).
constexpr float MinValueLine = 3.f;
constexpr float ValueTaper = 0.05f;
constexpr float LeadHpW = 6.f;
constexpr float PartnerHpW = 4.f;
constexpr float GuardW = 3.f;
constexpr float PoiseW = 4.f;
constexpr float FoeHpW = 3.f; // a minor enemy's line, and the second target line under the boss's
constexpr float TimerW = 3.f; // the windup's and a Qte's shrinking line
static_assert(LeadHpW >= MinValueLine && PartnerHpW >= MinValueLine && GuardW >= MinValueLine && PoiseW >= MinValueLine
	&& FoeHpW >= MinValueLine && TimerW >= MinValueLine, "no value line under its floor");
static_assert(1.f - 2.f * ValueTaper >= 0.85f, "a value line at full width along at least 85 percent of its length");
// The one shade: the subtitles' gradient across the screen's whole width, darkest at its bottom edge and gone this far up.
constexpr float BottomShade = 0.25f;
constexpr float BottomShadeH = 220.f;
// Text, px to the em (Slate's size is points at 96 dpi). Every word the HUD sets in these sizes is in true capitals,
// which measure CapK of the em in both serif faces (cap height 660 of 1000 units; the small-caps face's small caps are
// 560). Nothing a player reads in a fight is under MinTextPx type.
constexpr float CapK = 0.66f;
constexpr float MinTextPx = 22.f;
constexpr float LeadPx = 38.f;    // capitals 25.1 px (floor 24)
constexpr float PartnerPx = 29.f; // 19.1 px (floor 18)
constexpr float SmallPx = 26.f;   // the call word over a minor enemy or at the screen's edge: 17.2 px
constexpr float RowPx = 29.f;     // the prompts' words and the Read row's moves: 19.1 px (floor 18)
constexpr float PrefixPx = 26.f;  // the Read row's NEXT MOVE: 17.2 px (floor 16)
constexpr float NotePx = 26.f;    // the labels and the state slots (LOAD, RELEASE, GUARD, POISE, SHARED, WEAK POINT,
                                  // WEIGHT, the Debt): 17.2 px (floor 16)
constexpr float FoePx = 22.f;     // a minor enemy's name over its body
// A key cap's letters, set for 14 px capitals on screen: 15.8 px by the file's cap height, and rendered small text
// measures about 0.61 of its px to the capital (11 px at 18), so 14.6 px. The cap box keeps the proportion it had round
// 18 px letters (28 px high, 12 px wider than the letters, 5 px corners), scaled by KeyPx / 18.
constexpr float KeyPx = 24.f;
constexpr float KeyScale = KeyPx / 18.f;
constexpr float KeyCapH = 28.f * KeyScale;   // every key cap's height (the prompts', the Read row's, the Release's, a Qte's): 37.3
constexpr float KeyCapPad = 12.f * KeyScale; // the cap's width over its letters': 16
constexpr float KeyCapCorner = 5.f * KeyScale;
constexpr float PromptStep = KeyCapH + 12.f; // a prompt row (a key cap) and the 12 px between rows: 49.3
static_assert(KeyPx * 0.61f >= 14.5f, "key letters at 14 px capitals on screen, half a pixel to spare");
static_assert(LeadPx >= MinTextPx && PartnerPx >= MinTextPx && SmallPx >= MinTextPx && RowPx >= MinTextPx && PrefixPx >= MinTextPx
	&& NotePx >= MinTextPx && FoePx >= MinTextPx && KeyPx >= MinTextPx, "no fight text under the floor");
static_assert(LeadPx * CapK >= 25.f && PartnerPx * CapK >= 19.f && RowPx * CapK >= 19.f && PrefixPx * CapK >= 17.f && NotePx * CapK >= 17.f,
	"capitals at least a pixel over their floors: lead 24, partner and prompts 18, labels 16");
// Ink, 0..1.
constexpr float PartnerInk = 0.85f; // the partner's name
constexpr float NoteInk = 0.8f;     // the labels
constexpr float PrefixInk = 0.7f;   // NEXT MOVE
constexpr float SlotInk = 0.7f;     // LOAD (and ALONE, ENDED) in the lead's state slot
// WAIT's line in the state slot: px after the word and long at the wait's start (a TimerW line, as a Qte's).
constexpr float WaitGap = 10.f;
constexpr float WaitLen = 80.f;
constexpr float TooSoonShow = 1.f; // seconds TOO SOON stands in the call slot
constexpr float GiftGap = 24.f;    // px between GIFT HEAL's cap and GIFT GUARD's word when the GIFT row holds both
constexpr float LoneHeadInk = 0.7f; // the Lone Carry card's heads, YOU GAIN and YOU GIVE UP
// The Lone Carry's card over the party block, left-aligned on its margin, no panel: px from its last line's foot to the
// block's top, from the title's foot to the heads, between the columns, and between lines over their ascent.
constexpr float LoneGap = 24.f;
constexpr float LoneTitleGap = 14.f;
constexpr float LoneColGap = 48.f;
constexpr float LoneLineGap = 6.f;
constexpr float LoneShadeK = 1.5f; // the bottom shade's height while the card shows, over the card's top from the bottom edge
} // namespace Hud

// What was just lost: held a moment, then rubbed away toward the value (a hit rubs the line, it does not snap).
struct FLostTrace
{
	float Was = 0.f;
	float Hold = 0.f;

	void Step(float Now, float Dt)
	{
		if (Now >= Was)
		{
			Was = Now;
			Hold = 0.f;
			return;
		}
		Hold += Dt;
		if (Hold > 0.8f)
		{
			Was = FMath::Max(Now, Was - 0.35f * Dt);
		}
	}
};

ftcore::UiStrokeStyle StrokeStyle(float Width, std::uint32_t Seed, float S, float Taper = 0.15f)
{
	ftcore::UiStrokeStyle St;
	St.width = Width * S;
	St.wobble = 0.8f * S;
	St.step = 4.f * S;
	St.taper = Taper;
	St.seed = Seed;
	if (Taper <= 0.f)
	{
		St.endWidth = 1.f;
		St.endAlpha = 1.f;
	}
	return St;
}

// A line that carries a value: straight (no drift across it, so its width reads true), the brush's taper only at its two
// ends, its thinnest point never under Hud::MinValueLine px.
ftcore::UiStrokeStyle ValueStyle(float Width, std::uint32_t Seed, float S, float Taper = Hud::ValueTaper)
{
	ftcore::UiStrokeStyle St = StrokeStyle(Width, Seed, S, Taper);
	St.wobble = 0.f;
	St.endWidth = FMath::Clamp(FMath::Max(St.endWidth, Hud::MinValueLine / FMath::Max(Width, 0.01f)), 0.f, 1.f);
	return St;
}

// A value line's middle moved so its edges fall on whole pixels: its full width then covers whole rows, never a soft
// half row each side.
float SnapLine(float Y, float WidthPx) { return FMath::RoundToFloat(Y - 0.5f * WidthPx) + 0.5f * WidthPx; }

float Frac(int32 Value, int32 Max) { return Max > 0 ? FMath::Clamp(static_cast<float>(Value) / Max, 0.f, 1.f) : 0.f; }

// The text's top that centres its capitals on MidY (the serif's capitals are 0.62 of its ascent), and the middle of the
// capitals on a baseline.
float CapTop(float MidY, float Asc) { return MidY - 0.69f * Asc; }
float CapMid(float Base, float Asc) { return Base - 0.31f * Asc; }

// Elements made inside Fn are clipped to the rectangle (local units).
template <typename FnT>
void Clipped(const FPainter& P, float X, float Y, float W, float H, FnT&& Fn)
{
	if (W <= 0.f || H <= 0.f)
	{
		return;
	}
	P.Out.PushClip(FSlateClippingZone(P.Geometry.ToPaintGeometry(FVector2f(W, H), FSlateLayoutTransform(FVector2f(X, Y)))));
	Fn();
	P.Out.PopClip();
}

// The -FTUiFirstUse=fresh|seen switch (for captures): fresh teaches everything again, seen nothing; without it the save
// decides.
ftcore::UiFirstUseMode FirstUseMode()
{
	static const ftcore::UiFirstUseMode Mode = []
	{
		ftcore::UiFirstUseMode M = ftcore::UiFirstUseMode::Saved;
		FString V;
		if (FParse::Value(FCommandLine::Get(), TEXT("-FTUiFirstUse="), V))
		{
			const FTCHARToUTF8 U(*V);
			ftcore::uiFirstUseMode(std::string_view(U.Get(), static_cast<size_t>(U.Length())), M);
		}
		return M;
	}();
	return Mode;
}

// -UiStateShot=<name>, honoured only with -ArenaShotUi and out of shipping builds: one HUD state held for a still, on
// test data where the fight has none. lone_card: the Lone Carry's card in full (the runner's terms while the lead is
// Lone, else the default tuning's) and ALONE in the state slot; lone_alone: ALONE alone; lone_ended: ENDED: GIFT;
// gift: both gifts ready (GIFT HEAL 2 at least, both taken as the slot would answer a ready gift) and the lead hurt (70
// percent if at full), so the GIFT row holds both; no_key: the gift shot with Gift Heal left unbound on the device in use,
// as the bindings import leaves a new action whose default key the player gave to something else, so GIFT HEAL stands
// dimmed with NO KEY in its key box beside a lit GIFT GUARD; too_soon: TOO SOON in the call slot; wait: WAIT with its line
// at 60 percent. A held state never counts a showing or writes the save.
enum class EUiStateShot : uint8
{
	None,
	LoneCard,
	LoneAlone,
	LoneEnded,
	Gift,
	NoKey,
	TooSoon,
	Wait,
};
bool IsLoneShot(EUiStateShot S) { return S == EUiStateShot::LoneCard || S == EUiStateShot::LoneAlone || S == EUiStateShot::LoneEnded; }

EUiStateShot UiStateShot()
{
#if !UE_BUILD_SHIPPING
	static const EUiStateShot Shot = []
	{
		FString V;
		if (!FParse::Param(FCommandLine::Get(), TEXT("ArenaShotUi")) || !FParse::Value(FCommandLine::Get(), TEXT("-UiStateShot="), V))
		{
			return EUiStateShot::None;
		}
		const EUiStateShot S = V == TEXT("lone_card") ? EUiStateShot::LoneCard
			: V == TEXT("lone_alone")                 ? EUiStateShot::LoneAlone
			: V == TEXT("lone_ended")                 ? EUiStateShot::LoneEnded
			: V == TEXT("gift")                       ? EUiStateShot::Gift
			: V == TEXT("no_key")                     ? EUiStateShot::NoKey
			: V == TEXT("too_soon")                   ? EUiStateShot::TooSoon
			: V == TEXT("wait")                       ? EUiStateShot::Wait
													  : EUiStateShot::None;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena UI: -UiStateShot=%s %s."), *V, S == EUiStateShot::None ? TEXT("is not a state; ignored") : TEXT("held"));
		return S;
	}();
	return Shot;
#else
	return EUiStateShot::None;
#endif
}

// The HUD's size over the viewport's scale, margins included: the settings' HUD scale (80-130 percent), or the
// -FTHudScale=1.1 switch for captures (out of shipping builds).
float HudScaleFor(const UFTArenaRunner& Runner)
{
#if !UE_BUILD_SHIPPING
	static float Forced = 1.f;
	static const bool bForced = FParse::Value(FCommandLine::Get(), TEXT("-FTHudScale="), Forced);
	if (bForced)
	{
		return FMath::Clamp(Forced, 0.8f, 1.3f);
	}
#endif
	const ftcore::Settings* Prefs = Runner.GetSettings();
	return Prefs ? Prefs->hudScale / 100.f : 1.f;
}

// The player's HUD safe margin, percent of the viewport on each side over the platform's title-safe padding: the
// settings' (0-10, 0 by default on PC), or the -FTHudSafeMargin=5 switch for captures (out of shipping builds).
int32 HudSafeMarginFor(const UFTArenaRunner& Runner)
{
#if !UE_BUILD_SHIPPING
	static int32 Forced = 0;
	static const bool bForced = FParse::Value(FCommandLine::Get(), TEXT("-FTHudSafeMargin="), Forced);
	if (bForced)
	{
		return FMath::Clamp(Forced, ftcore::Settings::kMinHudSafeMargin, ftcore::Settings::kMaxHudSafeMargin);
	}
#endif
	const ftcore::Settings* Prefs = Runner.GetSettings();
	return Prefs ? Prefs->hudSafeMargin : 0;
}

// A Read card's move name (kReadMoveUi), by the tell its action shows; -1 for a tell without one.
int32 ReadMoveIndex(std::string_view Tell)
{
	for (int32 I = 0; I < static_cast<int32>(UE_ARRAY_COUNT(kReadTells)); ++I)
	{
		if (Tell == kReadTells[I])
		{
			return I;
		}
	}
	return -1;
}

// The arena HUD's world marks, one per enemy (TickWorld fills them, PaintWorld draws them).
struct FFoeMark
{
	bool bShow = false; // alive, with a figure
	bool bOn = false;   // its body on the screen
	bool bLine = false; // its own name and hp line over the body (the top line carries the target's and the boss's)
	FVector2f Over = FVector2f::ZeroVector;
	FVector2f Chest = FVector2f::ZeroVector;
	float Hp01 = 0.f;
	uint8 Word = 0;   // 1 staggered, 2 open to a riposte
	uint8 Tell = 0;   // 0 none, 1 winding up, 2 the hit
	uint8 Answer = 0; // 0 parry, 1 block, 2 dodge
	float Fill = 0.f; // the windup's share gone (1 on the hit frame)
	float Impact = 0.f;
	bool bEdge = false; // drawn at the screen's edge (off frame)
	FVector2f Dir = FVector2f(0.f, 1.f);

	// A minor enemy's own block shows once it is hit or winds up (or stands staggered).
	bool Shows() const { return bOn && bLine && (Hp01 < 0.999f || Tell == 1 || Word != 0); }
};

class SFTArenaHud : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFTArenaHud) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&, TFunction<const UFTArenaRunner*()> InGetRunner)
	{
		GetRunner = MoveTemp(InGetRunner);
		SetVisibility(EVisibility::HitTestInvisible); // never takes a click from the world
		// The kept glyph runs go when Slate releases its font resources (it may say so from either thread: a flag only).
		if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
		{
			std::atomic<bool>* Flush = &TextRuns.bFlush;
			FontRelease = FSlateApplication::Get().GetRenderer()->GetFontServices()->OnReleaseResources().AddLambda(
				[Flush](const FSlateFontCache&) { Flush->store(true); });
		}
	}

	virtual ~SFTArenaHud() override
	{
		if (FontRelease.IsValid() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
		{
			FSlateApplication::Get().GetRenderer()->GetFontServices()->OnReleaseResources().Remove(FontRelease);
		}
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1920.0, 1080.0); }

	virtual void Tick(const FGeometry& Geometry, const double, const float DeltaTime) override
	{
		Time += DeltaTime;
		const UFTArenaRunner* Runner = GetRunner ? GetRunner() : nullptr;
		bLive = Runner && Runner->IsBegun();
#if FT_UI_ALLOC_COUNT
		FUiAllocCount& Count = UiAllocCount();
		Count.Seen += bLive && Count.Frame > 0 && !Count.bDone ? 1 : 0;
		const FUiAllocScope Counting(Count, 0, bLive && Count.Counting()); // this Tick's heap calls, on the frame counted
#endif
		HudScale = Runner ? HudScaleFor(*Runner) : HudScale;
		HudSafePct = Runner ? HudSafeMarginFor(*Runner) : HudSafePct;
		// The platform's title-safe padding, read when the viewport's size changes (never per frame): Slate's safe-zone
		// ratio per side (the custom safe zone when one is set, else the display's title-safe padding over half the
		// display). A display of no size gives no padding.
		const FVector2f ViewSize = Geometry.GetLocalSize();
		if (ViewSize != SafeSize)
		{
			SafeSize = ViewSize;
			SafeRatio = FMargin(0.f);
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().GetSafeZoneRatio(SafeRatio);
			}
			const auto Clean = [](float& R) { R = FMath::IsFinite(R) ? FMath::Clamp(R, 0.f, 0.5f) : 0.f; };
			Clean(SafeRatio.Left);
			Clean(SafeRatio.Top);
			Clean(SafeRatio.Right);
			Clean(SafeRatio.Bottom);
		}
		const float H = Geometry.GetLocalSize().Y * HudScale; // the fonts' height: the viewport's, times the HUD scale
		if (H != BuiltHeight)
		{
			BuiltHeight = H;
			BuildFonts(H);
			bRebuild = true; // kept until a live frame rebuilds the strings that need the fight
		}
		if (!bLive)
		{
			RowSwap = ftcore::UiRowSwap{}; // a new fight's first seats are where the rows start, not a switch
			return;
		}
		const bool bRebuildNow = bRebuild;
		bRebuild = false;
		AimA = Runner->GetAimAlpha();
		const ftcore::ArenaFight& Fight = Runner->GetFight();
		ftcore::ArenaHudFrame Frame = View.now;
		Frame.lead.lead = Runner->GetLeadSeat();
		ftcore::fillArenaHudLead(Fight, Frame.lead);
		ftcore::fillArenaHudTarget(Fight, Frame.target);
		FillBoss(*Runner, Frame.boss);
		Frame.partner = ftcore::ArenaHudPartner{};
		Frame.partner.lead = Runner->GetAllySeat();
		ftcore::fillArenaHudPartner(Fight, Frame.partner); // the carrying runs' ally; zero (absent) otherwise
		Frame.partner.load = Frame.partner.present ? Runner->GetPartnerLoad() : 0;
		// The third place's gift prompts, from the session (without one there is no gift slot, and both are Absent): the
		// Gift Heals left (GIFT HEAL n, cooling or not), the Gift Guard off its cooldown, and each gift's answer from the
		// slot itself (GetGiftReason: would its press be taken now, and if not, why), so a cooling or spent gift stands
		// dimmed with its word. A live Qte (Frame.lead.qte, which fillArenaHudLead leaves alone) would go here too; nothing
		// reports one yet.
		Frame.third = ftcore::ArenaHudThird{};
		GiftWhy[0] = GiftWhy[1] = ftcore::PressReason::Absent;
		if (Runner->GetSession())
		{
			Frame.third.giftHeals = Runner->GetGiftHealsLeft();
			Frame.third.giftGuardReady = Runner->GetGiftGuardReadyIn() <= 0.0;
			GiftWhy[0] = Runner->GetGiftReason(ftcore::Gift::Heal);
			GiftWhy[1] = Runner->GetGiftReason(ftcore::Gift::Guard);
		}
		const EUiStateShot Shot = UiStateShot();
		if (Shot == EUiStateShot::Gift || Shot == EUiStateShot::NoKey)
		{
			Frame.third.giftHeals = FMath::Max(Frame.third.giftHeals, 2);
			Frame.third.giftGuardReady = true;
			GiftWhy[0] = GiftWhy[1] = ftcore::PressReason::Ok;
			if (Frame.lead.hpMax > 0 && Frame.lead.hp >= Frame.lead.hpMax)
			{
				Frame.lead.hp = Frame.lead.hpMax * 7 / 10;
			}
		}
		const uint32 Changed = ftcore::updateArenaHudView(View, Frame);
		const ftcore::ArenaHudLead& L = View.now.lead;
		HpLost.Step(Frac(L.hp, L.hpMax), DeltaTime);
		PartnerLost.Step(Frac(View.now.partner.hp, View.now.partner.hpMax), DeltaTime);
		ftcore::uiLoadNameStep(LoadName, L, DeltaTime);
		// The rows trade places when the seats do (a lead switch): GetLeadSeat and GetAllySeat must swap on that tick.
		ftcore::uiRowSwapStep(RowSwap, View.now, DeltaTime);
		QteLeft = ftcore::uiQteLeft01(L.qte, Fight.time());
		LeadDip = Pulse(Runner->GetNow(), Runner->GetPlayerHitTime(), 0.15f);
		PartnerDip = Pulse(Runner->GetNow(), Runner->GetAllyHitTime(), 0.15f);
		TickNames(*Runner, bRebuildNow);
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		// The name over the top line: the boss's, else the target's, from the loc table by its arena kind (none: no name, the
		// line alone). Lowercase in the small-caps face: one even small-cap height.
		const int32 WantSlot = B.present ? static_cast<int32>(B.slot) : (T.present ? static_cast<int32>(T.enemy) : -1);
		if (WantSlot != NameSlot)
		{
			NameSlot = WantSlot;
			bNamed = KindName(*Runner, WantSlot, NameFont, NameText, NameW);
		}
		TopLost.Step(Frac(B.present ? B.hp : T.hp, B.present ? B.hpMax : T.hpMax), DeltaTime);

		// The prompts' keys on the device in use (built when the device changes); the counts that sit in their words.
		const APlayerController* PC = Runner->GetWorld() ? Runner->GetWorld()->GetFirstPlayerController() : nullptr;
		const ftcore::InputDevice Device = FTInputBindings::NoteDevice(PC);
		if (bRebuildNow || Device != KeyDevice)
		{
			KeyDevice = Device;
			BuildKeys(Device);
		}
		if (bRebuildNow || L.heals != HealShown)
		{
			HealShown = L.heals;
			HealText = PromptWord[static_cast<int32>(ftcore::UiPrompt::Heal)] + TEXT(" ") + FString::FromInt(L.heals);
			HealW = TextWidth(HealText, PromptFont);
		}
		const int32 GiftHeals = View.now.third.giftHeals;
		if (bRebuildNow || GiftHeals != GiftHealShown)
		{
			GiftHealShown = GiftHeals;
			GiftHealText = GiftHeals >= 0
				? PromptWord[static_cast<int32>(ftcore::UiPrompt::GiftHeal)] + TEXT(" ") + FString::FromInt(GiftHeals)
				: FString();
			GiftHealW = TextWidth(GiftHealText, PromptFont);
		}
		if (bRebuildNow || L.read != StackShown)
		{
			StackShown = L.read;
			StackText = L.read >= 2 ? FString::Printf(TEXT("\u00D7%d"), L.read) : FString(); // a streak: from two up
			StackW = TextWidth(StackText, PrefixFont);
		}
		if (bRebuildNow || L.debt != DebtShown)
		{
			DebtShown = L.debt;
			DebtText = L.debt >= 0 ? DebtWord + TEXT(" ") + FString::FromInt(L.debt) : FString();
		}

		const ftcore::UiFadeTimes& Fade = Table().Style.fade;
		ftcore::uiFadeStep(LeadFade, (Changed & SectionBit(ftcore::ArenaHudSection::Lead)) != 0, DeltaTime, Fade);
		ftcore::uiFadeStep(TopFade, (Changed & (SectionBit(ftcore::ArenaHudSection::Target) | SectionBit(ftcore::ArenaHudSection::Boss))) != 0,
			DeltaTime, Fade);

		TickWorld(*Runner, Fight, PC, Geometry.GetLocalSize());
		TickEvents(*Runner, Fight, DeltaTime);
		TickFirstUse(*Runner, Fight, DeltaTime);
		TickLone(*Runner, Fight, DeltaTime);
		TickHall(*Runner, Fight, DeltaTime);
		TickBudget(Fight, Changed, DeltaTime);
		TickLabels(*Runner);
	}

	// The element budget (Core): the data asks for marks, the budget grants them in priority order, and what is not granted
	// is not drawn. At rest only the party rows and the prompts; never more than the cap in action.
	void TickBudget(const ftcore::ArenaFight& Fight, uint32 Changed, float DeltaTime)
	{
		using ftcore::UiMarkKind;
		const ftcore::ArenaHudFrame& Now = View.now;
		const ftcore::ArenaHudBoss& B = Now.boss;
		ftcore::UiBudgetInput In;
		In.dt = DeltaTime;
		In.changed = Changed;
		// Each prompt's answer from the rules (Core's uiPromptReasons: the fight's own presses, and the gift slot's as the
		// session gives them): a prompt whose action is not in this fight is not drawn; every other stands in its row, and
		// one whose press would be refused now is dimmed with its reason. A first fight's controls stand while the teaching
		// holds them and go when it ends.
		Why = ftcore::uiPromptReasons(Fight, GiftWhy[0], GiftWhy[1]);
		ftcore::UiPromptMask Teaching = 0;
		for (int32 I = static_cast<int32>(ftcore::UiPrompt::Attack); I < static_cast<int32>(ftcore::kUiPromptCount); ++I)
		{
			const auto Item = static_cast<ftcore::UiFirstUseItem>(static_cast<int32>(ftcore::UiFirstUseItem::Attack) + I
				- static_cast<int32>(ftcore::UiPrompt::Attack));
			if (ftcore::uiFirstUseAlpha(FirstUse, Item) >= 1.f)
			{
				Teaching |= ftcore::uiPromptBit(static_cast<ftcore::UiPrompt>(I));
			}
		}
		// Before the budget: a row nothing reaches on the device (no key and no NO KEY), or on a key a lower row shows,
		// would draw nothing, so it takes no row.
		In.prompts = ftcore::uiPromptsDrawable(ftcore::uiPromptsPresent(Why, Teaching), PromptKeyed, PromptSameKeyBelow);
		PromptDimmed = ftcore::uiPromptsDimmed(Why, PromptNoKey);
		In.resultOpen = bResult;
		In.want = ftcore::uiMarkWants(Now, In.prompts);
		// A live Qte is a timed press like a windup: it counts as a tell, so the budget never drops it. So does TOO SOON while
		// it stands, the call slot's other word of the player's own.
		const bool bQteLive = Now.lead.qte.live;
		const bool bTooSoonLive = TooSoonLeft > 0.f;
		uint8 Tells = static_cast<uint8>((bQteLive ? 1 : 0) + (bTooSoonLive ? 1 : 0));
		uint8 FoeLines = 0;
		bool bBossActs = false;
		for (int32 I = 0; I < UFTArenaRunner::MaxEnemies; ++I)
		{
			const FFoeMark& M = Foes[I];
			if (!M.bShow)
			{
				continue;
			}
			Tells += M.Tell != 0 ? 1 : 0;
			FoeLines += M.Shows() ? 1 : 0;
			bBossActs |= B.present && B.slot == I && M.Tell != 0;
		}
		const auto Want = [&In](UiMarkKind K, uint8 N) { In.want[static_cast<size_t>(K)] = N; };
		Want(UiMarkKind::Tell, Tells);
		Want(UiMarkKind::Event, static_cast<uint8>(ftcore::uiMarkLiveCount(Marks)));
		Want(UiMarkKind::Cards, ReadN > 0 ? 1 : 0);
		Want(UiMarkKind::WeakPoint, bWeakOn ? 1 : 0);
		Want(UiMarkKind::Lock, bLockOn ? 1 : 0);
		Want(UiMarkKind::Foe, FoeLines);
		if (!B.present && !bNamed)
		{
			Want(UiMarkKind::Target, 0); // a target with no name has no line (never a bar without a name)
		}
		In.activity = bMarkNew || Tells > 0 || HitA > 0.f || ParryA > 0.f || AimA > 0.01f;
		// The boss line returns when the boss acts (a windup or its hit), is hit, opens its weak point, or is locked on anew.
		const uint8 LockNow = Fight.lock();
		// Any change in the boss's line (hp, phase, a call, a note, down, invulnerable, the weak point) counts, and a live call
		// keeps it up.
		In.bossEvent = B.present
			&& (!bBossSeen || bBossActs || !(B == BossSeen) || B.call != ftcore::UiCalledZone::None || B.weakPoint == ftcore::UiWeakPoint::Open
				|| (LockNow == B.slot && LockSeen != LockNow));
		bBossSeen = B.present;
		BossSeen = B;
		LockSeen = LockNow;

		const ftcore::UiBudgetOut& G = ftcore::uiBudgetStep(Budget, In, Table().Style.budget);
		// Event marks past the grant: the oldest are not drawn.
		MarkMask = 0;
		{
			uint32 Over = ftcore::uiMarkLiveCount(Marks);
			const uint32 Allowed = G.allowed(UiMarkKind::Event);
			while (Over > Allowed)
			{
				int32 Oldest = -1;
				for (int32 I = 0; I < static_cast<int32>(ftcore::kUiLiveMarkMax); ++I)
				{
					const ftcore::UiLiveMark& M = Marks.m[static_cast<size_t>(I)];
					if (M.live && !(MarkMask & (1u << I)) && (Oldest < 0 || M.serial < Marks.m[static_cast<size_t>(Oldest)].serial))
					{
						Oldest = I;
					}
				}
				if (Oldest < 0)
				{
					break;
				}
				MarkMask |= static_cast<uint16>(1u << Oldest);
				--Over;
			}
		}
		uint8 TellsLeft = G.allowed(UiMarkKind::Tell);
		bQteOn = bQteLive && TellsLeft > 0; // the Qte first: the player's own press outranks a foe's windup
		TellsLeft = static_cast<uint8>(TellsLeft - (bQteOn ? 1 : 0));
		uint8 LinesLeft = G.allowed(UiMarkKind::Foe);
		for (FFoeMark& M : Foes)
		{
			if (!M.bShow)
			{
				continue;
			}
			if (M.Tell != 0)
			{
				if (TellsLeft > 0)
				{
					--TellsLeft;
				}
				else
				{
					M.Tell = 0;
				}
			}
			if (M.Shows())
			{
				if (LinesLeft > 0)
				{
					--LinesLeft;
				}
				else
				{
					M.bLine = false;
				}
			}
		}
		bTooSoonOn = bTooSoonLive && TellsLeft > 0; // after the windups: a foe's blow outranks a word about the player's timing
		if (G.allowed(UiMarkKind::Cards) == 0)
		{
			ReadN = 0;
		}
		bWeakOn &= G.allowed(UiMarkKind::WeakPoint) > 0;
		bLockOn &= G.allowed(UiMarkKind::Lock) > 0;
		RestA = G.actionAlpha;
		if (Budget.maxMarks > MarksLogged)
		{
			MarksLogged = Budget.maxMarks;
			UE_LOG(LogForgottenThird, Log, TEXT("Arena UI: marks max %d (cap %d)."), MarksLogged, Table().Style.budget.marks);
		}
		if (G.rest != bRestLogged)
		{
			bRestLogged = G.rest;
			UE_LOG(LogForgottenThird, Log, TEXT("Arena UI: %s at %.1f s (marks %d)."), G.rest ? TEXT("rest") : TEXT("action"), Fight.time(), G.marks);
		}
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer,
		const FWidgetStyle&, bool) const override
	{
		if (!bLive)
		{
			return Layer;
		}
#if FT_UI_ALLOC_COUNT
		FUiAllocCount& Count = UiAllocCount();
		const FUiAllocScope Counting(Count, 1, Count.Counting()); // this paint's heap calls, on the frame counted; logged after
#endif
		FTextRuns* Runs = UiTextCacheOn() ? &TextRuns : nullptr;
		if (Runs)
		{
			Runs->BeginFrame();
		}
		const FVector2f Size = Geometry.GetLocalSize();
		// Every word with its shadow, from the kept glyph runs.
		const FPainter P{Geometry, Out, Layer, FMath::Max(0.25f, Size.Y / 1080.f * HudScale), true, Runs};
		Mesh.Begin(Geometry, P.S);
		// The one shade: the subtitles' gradient across the screen's whole bottom while the party stands, darkest at the
		// bottom edge and gone by BottomShadeH up. Nothing else stands behind a word.
		if (!bResult && Budget.out.allowed(ftcore::UiMarkKind::Lead) > 0 && View.now.lead.hpMax > 0)
		{
			// The Lone Carry's card stands over the party block, above the shade's 220 px: while it shows, the shade rises
			// under it (to LoneShadeK of the card's height from the bottom edge) with the card's ink, and comes back down as it
			// fades.
			float ShadeH = Hud::BottomShadeH * P.S + (EdgesAt(Size, P.S).bottom - Hud::Margin * P.S); // up with the bottom edge
			const float CardA = ftcore::uiLoneCardAlpha(Lone);
			if (CardA > 0.f)
			{
				ShadeH = FMath::Lerp(ShadeH, FMath::Max(ShadeH, (Size.Y - LoneCardTop(Size, P.S)) * Hud::LoneShadeK), CardA);
			}
			Mesh.Band(0.f, Size.Y - ShadeH, Size.X, Size.Y, true, true, 1.f, Hud::BottomShade * TextAlpha(LeadFade.alpha),
				Tok(UiToken::Backing, 1.f));
		}
		PaintWorld(P, Size);
		PaintAim(P, Size);
		PaintParty(P, Size);
		PaintLone(P, Size);
		PaintTop(P, Size);
		PaintCall(P, Size);
		PaintSecondLine(P, Size);
		PaintPrompts(P, Size);
		PaintEvents(P, Size);
		Mesh.Flush(Out, Layer + 1); // every line, mark and shade in one element under the words
		return Layer + 5;           // the Load's gold and its bright edge stand over the name's words
	}

private:
	// The fonts and the words that need no fight, at this height.
	void BuildFonts(float H)
	{
		const TSharedRef<FSlateFontMeasure> FM = FontMeasure();
		const auto Asc = [&FM](const FSlateFontInfo& F) { return FM->GetMaxCharacterHeight(F) + FM->GetBaseline(F); };
		// The names in spaced small caps, as the boss's.
		const auto Spaced = [](FSlateFontInfo F)
		{
			F.LetterSpacing = 220;
			return F;
		};
		// Each role says whether it shows in capitals (bForce: always); in the small-caps face "capitals" is all small caps
		// (lowercase in, one even height out).
		const auto Cased = [](UiTextRole Role, const FString& In, bool bForce = false)
		{
			const ftcore::UiTextStyle& St = Table().Style.text[static_cast<size_t>(Role)];
			const bool bSc = St.font == ftcore::UiFont::SerifSc || St.font == ftcore::UiFont::SerifScBold;
			return !(St.upper || bForce) ? In : bSc ? In.ToLower() : In.ToUpper();
		};
		LabelFont = FontFor(UiTextRole::Label, H);
		NameFont = Spaced(FontFor(UiTextRole::Title, H));
		NameAsc = Asc(NameFont);
		LeadNameFont = Spaced(FontPx(UiTextRole::Title, H, Hud::LeadPx));
		LeadNameAsc = Asc(LeadNameFont);
		// The gold part of the name keeps a 1 px dark outline at 60 percent.
		GoldFont = LeadNameFont;
		GoldFont.OutlineSettings.OutlineSize = FMath::Max(1, FMath::RoundToInt(H / 1080.f));
		GoldFont.OutlineSettings.OutlineColor = FLinearColor(Table().Edge.R, Table().Edge.G, Table().Edge.B, 0.6f);
		PartnerNameFont = Spaced(FontPx(UiTextRole::Title, H, Hud::PartnerPx));
		PartnerNameAsc = Asc(PartnerNameFont);
		FoeNameFont = Spaced(FontPx(UiTextRole::Title, H, Hud::FoePx));
		FoeNameAsc = Asc(FoeNameFont);
		CallFont = FontFor(UiTextRole::Popup, H, 1.35f);
		CallAsc = Asc(CallFont);
		SmallCallFont = FontPx(UiTextRole::Popup, H, Hud::SmallPx);
		SmallCallAsc = Asc(SmallCallFont);
		ReadFont = FontPx(UiTextRole::Popup, H, Hud::RowPx);
		ReadAsc = Asc(ReadFont);
		PrefixFont = FontPx(UiTextRole::Popup, H, Hud::PrefixPx);
		PrefixAsc = Asc(PrefixFont);
		PromptFont = FontPx(UiTextRole::Verb, H, Hud::RowPx);
		PromptAsc = Asc(PromptFont);
		KeyFont = FontPx(UiTextRole::Label, H, Hud::KeyPx);
		KeyAsc = Asc(KeyFont);
		NoteFont = FontPx(UiTextRole::Label, H, Hud::NotePx);
		NoteAsc = Asc(NoteFont);
		// NO KEY for the key box, and the reason words a refused prompt carries, at label size.
		NoKeyWord = kNoKeyUi.Get().ToUpper();
		for (int32 I = 0; I < static_cast<int32>(ftcore::kPressReasonCount); ++I)
		{
			WhyWord[I] = kWhyUi[I] ? kWhyUi[I]->Get().ToUpper() : FString();
			WhyW[I] = TextWidth(WhyWord[I], NoteFont);
		}
		ResultFont = FontFor(UiTextRole::Subtitle, H);
		ResultLineH = FM->GetMaxCharacterHeight(ResultFont) * 1.15f; // the subtitle's line
		NameSlot = -2; // measured again in the new fonts
		bFoeNames = false;
		for (int32 I = 0; I < static_cast<int32>(UE_ARRAY_COUNT(kReadTells)); ++I)
		{
			ReadMoveWord[I] = kReadMoveUi[I].Get().ToUpper(); // in capitals, as the call word over the row
			ReadMoveW[I] = TextWidth(ReadMoveWord[I], ReadFont);
		}
		// Every word the fight HUD sets is in true capitals (capital letters in, whatever the face): in the small-caps faces
		// lowercase draws small caps of 560 units, under the cap heights the sizes above are set for.
		for (int32 I = 0; I < 3; ++I)
		{
			ZoneWord[I] = kZoneUi[I].Get().ToUpper();
			ZoneMid[I] = InkMid(ZoneWord[I], CallFont);
			AnswerWord[I] = kAnswerUi[I].Get().ToUpper();
			AnswerMid[I] = InkMid(AnswerWord[I], CallFont);
			AnswerSW[I] = TextWidth(AnswerWord[I], SmallCallFont);
			AnswerSMid[I] = InkMid(AnswerWord[I], SmallCallFont);
			ReadNum[I] = FString::FromInt(I + 1); // the Read pick's keys (1-3), numerals, not text
			ReadNumW[I] = TextWidth(ReadNum[I], KeyFont);
		}
		StaggerWord = kStaggeredUi.Get().ToUpper();
		StaggerMid = InkMid(StaggerWord, CallFont);
		StaggerSMid = InkMid(StaggerWord, SmallCallFont);
		ReadWord = kNextMoveUi.Get().ToUpper();
		ReadWordW = TextWidth(ReadWord, PrefixFont);
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiPromptCount); ++I)
		{
			PromptWord[I] = kPromptUi[I].Get().ToUpper();
			PromptWordW[I] = TextWidth(PromptWord[I], PromptFont);
		}
		// A Qte's word is a call: the call slot's face.
		for (int32 I = 0; I < kQteWords; ++I)
		{
			const int32 Button = static_cast<int32>(ftcore::UiPrompt::Attack) + I;
			const FTUiText& Src = I < static_cast<int32>(ftcore::kActionButtonCount) ? kPromptUi[Button]
				: (I == static_cast<int32>(ftcore::kActionButtonCount) ? kQteHoldUi : kQteMashUi);
			QteWord[I] = Src.Get().ToUpper();
			QteWordW[I] = TextWidth(QteWord[I], CallFont);
			QteMid[I] = InkMid(QteWord[I], CallFont);
		}
		// The state slots' and the labels' words, all in the labels' size.
		LoadWord = kLoadUi.Get().ToUpper();
		ReleaseWord = kReleaseUi.Get().ToUpper();
		ReleaseWordW = TextWidth(ReleaseWord, NoteFont);
		GuardWord = kGuardUi.Get().ToUpper();
		GuardWordW = TextWidth(GuardWord, NoteFont);
		PoiseWord = kPoiseUi.Get().ToUpper();
		WeightWord = kWeightUi.Get().ToUpper();
		WeightW = TextWidth(WeightWord, NoteFont);
		DebtWord = kDebtUi.Get().ToUpper();
		LoneAloneWord = kLoneAloneUi.Get().ToUpper();
		LoneEndedWord[0] = kLoneEndedGiftUi.Get().ToUpper();
		LoneEndedWord[1] = kLoneEndedPartnerUi.Get().ToUpper();
		WaitWord = kWaitUi.Get().ToUpper();
		WaitWordW = TextWidth(WaitWord, NoteFont);
		TooSoonWord = kTooSoonUi.Get().ToUpper();
		TooSoonMid = InkMid(TooSoonWord, CallFont);
		// The Lone Carry's card: the title in the lead name's size, the heads and lines in the labels'.
		LoneTitle = kLoneTitleUi.Get().ToUpper();
		LoneGainHead = kLoneGainUi.Get().ToUpper();
		LoneGiveHead = kLoneGiveUpUi.Get().ToUpper();
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiLoneLineKinds); ++I)
		{
			LoneLineWord[I] = kLoneLineUi[I]->Get().ToUpper();
		}
		bLoneCardBuilt = false; // its lines measured again in the new fonts
		for (int32 I = 0; I < static_cast<int32>(UE_ARRAY_COUNT(kFirstUseUi)); ++I)
		{
			NoteWord[I] = kFirstUseUi[I] ? kFirstUseUi[I]->Get().ToUpper() : FString();
		}
		SharedWord = kSharedUi.Get().ToUpper();
		AgainWord = kAgainUi;
		AgainW = TextWidth(AgainWord, LabelFont);
		TagWord[0] = kAutoPlayUi;
		TagWord[1] = kIdleUi;
		VictoryWord = Cased(UiTextRole::Subtitle, kVictoryUi);
		VictoryW = TextWidth(VictoryWord, ResultFont);
		DefeatWord = Cased(UiTextRole::Subtitle, kDefeatUi);
		DefeatW = TextWidth(DefeatWord, ResultFont);
	}

	// The prompts' keys and Release's, as bound on the device: Ctrl, R. An action the bindings import left unbound there
	// (FTInputBindings::Unbound) shows NO KEY in its key box and keeps its row, dimmed; a prompt nothing reaches on the
	// device (no key and no route) is not drawn. The no_key capture leaves Gift Heal unbound on the device in use.
	void BuildKeys(ftcore::InputDevice Device)
	{
		const ftcore::InputBindings* Bound = &FTInputBindings::Get();
		const ftcore::InputUnbound* Left = &FTInputBindings::Unbound();
		ftcore::InputBindings ShotBound;
		ftcore::InputUnbound ShotLeft;
		if (UiStateShot() == EUiStateShot::NoKey)
		{
			ShotBound = *Bound;
			ShotLeft = *Left;
			const size_t D = static_cast<size_t>(Device) % ftcore::kInputDeviceCount;
			const size_t A = static_cast<size_t>(ftcore::InputAction::GiftHeal) % ftcore::kInputActionCount;
			ShotBound.keys[D][A] = ftcore::InputKeyName{};
			if (!ShotLeft.left[D][A])
			{
				ShotLeft.left[D][A] = true;
				++ShotLeft.count;
			}
			Bound = &ShotBound;
			Left = &ShotLeft;
		}
		const ftcore::InputHintEntry* Entries = nullptr;
		const size_t Count = ftcore::inputHintEntries(ftcore::InputHintScreen::ArenaJump, Entries); // the arena's line and the jump
		// The entry with this label, or (no label) the first that carries this action; null when the line has none.
		const auto EntryOf = [&](const FPromptKeyOf& Of) -> const ftcore::InputHintEntry*
		{
			for (size_t I = 0; I < Count; ++I)
			{
				const ftcore::InputHintEntry& E = Entries[I];
				if (Of.Label)
				{
					if (FCStringAnsi::Strcmp(E.label, Of.Label) == 0) return &E;
					continue;
				}
				for (uint8 A = 0; A < E.count; ++A)
				{
					if (E.actions[A] == Of.Action) return &E;
				}
			}
			return nullptr;
		};
		// A prompt's key (inputPromptKey): true when it is NO KEY (every action it names left unbound on the device). NO KEY
		// is the loc word, alone or beside another action's key in a mixed entry.
		const FString CoreNoKey = FT::ToFString(ftcore::kInputNoKey);
		const auto KeyOf = [&](const FPromptKeyOf& Of, FString& Out, float& W) -> bool
		{
			Out.Reset();
			ftcore::InputPromptKey K;
			if (const ftcore::InputHintEntry* E = EntryOf(Of))
			{
				K = ftcore::inputPromptKey(*Bound, *E, Device, Left);
			}
			else if (!Of.Label)
			{
				K = ftcore::inputPromptKey(*Bound, Of.Action, Device, Left); // the line leaves it out
			}
			Out = K.noKey ? NoKeyWord : FT::ToFString(K.keys).Replace(*CoreNoKey, *NoKeyWord, ESearchCase::CaseSensitive);
			W = TextWidth(Out, KeyFont);
			return K.noKey;
		};
		PromptKeyed = 0;
		PromptNoKey = 0;
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiPromptCount); ++I)
		{
			const ftcore::UiPromptMask Bit = ftcore::uiPromptBit(static_cast<ftcore::UiPrompt>(I));
			if (KeyOf(kPromptKeys[I], PromptKey[I], PromptKeyW[I])) PromptNoKey |= Bit;
			if (!PromptKey[I].IsEmpty()) PromptKeyed |= Bit;
		}
		// The lower prompts on each prompt's key, so one key never shows two rows (uiPromptsDrawable keeps the lowest).
		// NO KEY is no key: two unbound rows are not on one key.
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiPromptCount); ++I)
		{
			PromptSameKeyBelow[I] = 0;
			if (((PromptNoKey >> I) & 1u) || PromptKey[I].IsEmpty())
			{
				continue;
			}
			for (int32 J = 0; J < I; ++J)
			{
				if (!((PromptNoKey >> J) & 1u) && PromptKey[J].Equals(PromptKey[I], ESearchCase::CaseSensitive))
				{
					PromptSameKeyBelow[I] |= ftcore::uiPromptBit(static_cast<ftcore::UiPrompt>(J));
				}
			}
		}
		KeyOf(FPromptKeyOf{"input.release", {}}, ReleaseKey, ReleaseKeyW);
	}

	// The party rows' names: the looks' own, in true capitals; without a look, a plain word.
	void TickNames(const UFTArenaRunner& Runner, bool bForce)
	{
		const auto Build = [bForce](const FString& Src, const FString& Fallback, FString& Seen, FString& Out, float& W, const FSlateFontInfo& F)
		{
			if (bForce || !Seen.Equals(Src, ESearchCase::CaseSensitive))
			{
				Seen = Src;
				Out = (Src.IsEmpty() ? Fallback : Src).ToUpper();
				W = TextWidth(Out, F);
			}
		};
		Build(Runner.GetLeadName(), kLeadUi, LeadNameSeen, LeadName, LeadNameW, LeadNameFont);
		Build(Runner.GetAllyName(), kPartnerUi, PartnerNameSeen, PartnerName, PartnerNameW, PartnerNameFont);
	}

	// A slot's name from the loc table by its arena kind (false: none, the line alone).
	static bool KindName(const UFTArenaRunner& Runner, int32 Slot, const FSlateFontInfo& F, FString& Out, float& W)
	{
		Out.Reset();
		W = 0.f;
		const char* Kind = Slot >= 0 ? Runner.GetSlotKindId(Slot) : nullptr;
		if (!Kind || !*Kind)
		{
			return false;
		}
		const FString Key = FString(TEXT("arena.name.")) + UTF8_TO_TCHAR(Kind);
		const FString& Name = FTUiText::Lookup(*Key);
		if (Name == Key)
		{
			return false;
		}
		Out = Name.ToLower();
		W = TextWidth(Out, F);
		return true;
	}

	// The first fight's teaching (Core's UiFirstUse): begun with each fight, the basic controls shown at its start and
	// each let go a second after its first use; ended with the fight.
	void TickFirstUse(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, float DeltaTime)
	{
		using ftcore::UiFirstUseItem;
		if (Fight.over())
		{
			if (bTeaching)
			{
				ftcore::uiFirstUseEnd(FirstUse);
				bTeaching = false;
			}
			return;
		}
		if (!bTeaching)
		{
			bTeaching = true;
			const ftcore::Settings* Prefs = Runner.GetSettings();
			ftcore::UiFirstUseMode Mode = FirstUseMode();
			uint32 Saved = Prefs ? Prefs->uiFirstUse : 0u;
			if (bTaughtOnce)
			{
				// A fight again keeps what this run has counted (fresh counts from nothing once per run; a run that writes
				// no save still counts each showing).
				const uint32 Run = ftcore::uiFirstUseSaved(FirstUse);
				Saved = Mode == ftcore::UiFirstUseMode::Fresh ? Run : ftcore::uiFirstUseMerge(Saved, Run);
				if (Mode == ftcore::UiFirstUseMode::Fresh)
				{
					Mode = ftcore::UiFirstUseMode::Saved;
				}
			}
			bTaughtOnce = true;
			// Only a played fight under the save's own rule writes it (never a capture switch, autoplay or idle run).
			bSaveTaught = FirstUseMode() == ftcore::UiFirstUseMode::Saved && !Runner.IsAutoPlay() && !Runner.IsIdle();
			ftcore::uiFirstUseBegin(FirstUse, Saved, Mode);
			Teach(Runner, UiFirstUseItem::Attack);
			Teach(Runner, UiFirstUseItem::Guard);
			Teach(Runner, UiFirstUseItem::Dodge);
			if (UFTArenaRunner::JumpBound())
			{
				Teach(Runner, UiFirstUseItem::Jump);
			}
		}
		switch (Fight.output().player.action)
		{
		case ftcore::ArenaAction::Light:
		case ftcore::ArenaAction::Heavy: ftcore::uiFirstUseUsed(FirstUse, UiFirstUseItem::Attack); break;
		case ftcore::ArenaAction::Guard: ftcore::uiFirstUseUsed(FirstUse, UiFirstUseItem::Guard); break;
		case ftcore::ArenaAction::Dodge: ftcore::uiFirstUseUsed(FirstUse, UiFirstUseItem::Dodge); break;
		case ftcore::ArenaAction::Jump: ftcore::uiFirstUseUsed(FirstUse, UiFirstUseItem::Jump); break;
		default: break;
		}
		ftcore::uiFirstUseTick(FirstUse, DeltaTime);
	}

	// The labels show as their element starts to be drawn, the first three times a save (after the grants, so a label
	// never stands beside nothing). Every label is cued every tick, so each sees its element's rise. The Load, the guard
	// and the poise need none: the lead's state slot names the Load every time, and the guard and poise lines carry their
	// own words whenever they are drawn.
	void TickLabels(const UFTArenaRunner& Runner)
	{
		if (bTeaching && ftcore::uiFirstUseCue(FirstUse, ftcore::UiFirstUseItem::WeakPointLabel, bWeakOn))
		{
			SaveTaught(Runner);
		}
	}

	static bool IsAttack(ftcore::ArenaAction Act)
	{
		return Act == ftcore::ArenaAction::Light || Act == ftcore::ArenaAction::Heavy || Act == ftcore::ArenaAction::Riposte
			|| Act == ftcore::ArenaAction::Release;
	}

	// The Lone Carry (Core's UiLone): its card as the runner's bargain comes due, in full the first three times a save
	// shows it (a first-use count) and its title alone after; its word in the lead's state slot while it lasts, and the
	// end's word as the session reports it ended (a gift's or the partner's; the fight's own end shows nothing).
	void TickLone(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, float DeltaTime)
	{
		const bool bFight = !Fight.over();
		const bool bDue = bFight && Runner.IsLoneBargainDue();
		const ftcore::ArenaAction Act = Fight.output().player.action;
		ftcore::UiLoneInput In;
		In.fight = bFight;
		In.alone = bFight && Fight.lone();
		In.began = bDue && !bLoneDueWas;
		In.attack = IsAttack(Act) && Act != LoneActWas; // an attack's first tick: the press, not the swing held
		const UFTFightSession* Session = Runner.GetSession();
		In.endedBy = Session ? Session->Get().loneEndedBy() : ftcore::LoneEnd::None;
		bLoneDueWas = bDue;
		LoneActWas = Act;
		const EUiStateShot Shot = UiStateShot();
		if (In.began && Shot == EUiStateShot::None)
		{
			In.full = bTeaching && ftcore::uiFirstUseShow(FirstUse, ftcore::UiFirstUseItem::LoneCard);
			if (In.full)
			{
				SaveTaught(Runner); // counted as it shows, once
			}
			LoneShown = Runner.GetLoneBargain();
			bLoneCardBuilt = false;
		}
		ftcore::uiLoneStep(Lone, In, DeltaTime);
		if (bFight && IsLoneShot(Shot))
		{
			HoldLoneShot(Runner, Fight, Shot);
		}
		if (Lone.card && !bLoneCardBuilt)
		{
			BuildLoneCard();
		}
	}

	// A -UiStateShot still's Lone Carry, held each tick (never counted or saved).
	void HoldLoneShot(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, EUiStateShot Shot)
	{
		Lone = ftcore::UiLone{};
		switch (Shot)
		{
		case EUiStateShot::LoneCard:
			if (!bLoneShotTerms)
			{
				bLoneShotTerms = true;
				LoneShown = Runner.GetSession() && Fight.lone() ? Runner.GetLoneBargain() : ftcore::loneBargain(ftcore::LoneTuning{});
				bLoneCardBuilt = false;
			}
			Lone.card = Lone.full = Lone.holding = true;
			Lone.alone = true;
			break;
		case EUiStateShot::LoneAlone:
			Lone.alone = true;
			break;
		case EUiStateShot::LoneEnded:
			Lone.ended = ftcore::UiLoneWord::EndedGift;
			Lone.endedLeft = ftcore::kUiLoneEndedShow;
			break;
		case EUiStateShot::None:
		case EUiStateShot::Gift:
		case EUiStateShot::NoKey:
		case EUiStateShot::TooSoon:
		case EUiStateShot::Wait:
			break;
		}
	}

	// The hallucination's read-outs (the runner's, live only in a hallucination run): TOO SOON stands in the call slot for
	// a second from the moment a refusal comes too early (the runner reports it longer; its rising edge starts the HUD's
	// own second), and the Escrow wait gives the state slot WAIT and its shrinking line (Core's uiWaitLineStep). The weak
	// point's lock (GetWeakPointLockFor) draws nothing.
	void TickHall(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, float DeltaTime)
	{
		const bool bFight = !Fight.over();
		const bool bTooEarly = bFight && Runner.IsRefusedTooEarly();
		TooSoonLeft = bTooEarly && !bTooEarlyWas ? Hud::TooSoonShow : FMath::Max(0.f, TooSoonLeft - DeltaTime);
		bTooEarlyWas = bTooEarly;
		bWaiting = bFight && Runner.IsEscrowWaiting();
		Wait01 = ftcore::uiWaitLineStep(WaitLine, bWaiting, Runner.GetEscrowWaitLeft());
		if (!bFight)
		{
			TooSoonLeft = 0.f;
			return;
		}
		switch (UiStateShot())
		{
		case EUiStateShot::TooSoon:
			TooSoonLeft = Hud::TooSoonShow;
			break;
		case EUiStateShot::Wait:
			bWaiting = true;
			Wait01 = 0.6f;
			break;
		default:
			break;
		}
	}

	// The card's lines from the terms it shows: each line's words and Core's figure, the left column's width. Built as the
	// card begins and when the fonts change, never per frame.
	void BuildLoneCard()
	{
		bLoneCardBuilt = true;
		const ftcore::UiLoneCard Card = ftcore::uiLoneCard(LoneShown);
		const auto LineText = [this](const ftcore::UiLoneCardLine& Of) -> FString
		{
			const FString& Words = LoneLineWord[static_cast<int32>(Of.kind)];
			return Of.figure.empty() ? Words : Words + TEXT(" ") + FString(UTF8_TO_TCHAR(Of.figure.text));
		};
		LoneGainN = FMath::Min<int32>(Card.gainCount, static_cast<int32>(ftcore::kUiLoneGainMax));
		LoneGiveN = FMath::Min<int32>(Card.giveCount, static_cast<int32>(ftcore::kUiLoneGiveMax));
		LoneGainColW = TextWidth(LoneGainHead, NoteFont);
		for (int32 I = 0; I < LoneGainN; ++I)
		{
			LoneGainText[I] = LineText(Card.gain[static_cast<size_t>(I)]);
			LoneGainColW = FMath::Max(LoneGainColW, TextWidth(LoneGainText[I], NoteFont));
		}
		for (int32 I = 0; I < LoneGiveN; ++I)
		{
			LoneGiveText[I] = LineText(Card.give[static_cast<size_t>(I)]);
		}
	}

	// Shows a control; the save is written only when that counted something.
	void Teach(const UFTArenaRunner& Runner, ftcore::UiFirstUseItem Item)
	{
		if (ftcore::uiFirstUseShow(FirstUse, Item))
		{
			SaveTaught(Runner);
		}
	}

	// The counts into the save (never per frame: only when a showing was counted), for a fight that writes it.
	void SaveTaught(const UFTArenaRunner& Runner) const
	{
		if (!bSaveTaught)
		{
			return;
		}
		const UWorld* World = Runner.GetWorld();
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		if (UFTSettingsSubsystem* Prefs = GI ? GI->GetSubsystem<UFTSettingsSubsystem>() : nullptr)
		{
			Prefs->MarkUiFirstUse(ftcore::uiFirstUseSaved(FirstUse));
		}
	}

	bool GuardShown() const { return View.now.lead.hpMax > 0 && View.now.lead.stamina01 < 0.999f; }

	// The target's poise under the top line (the boss's own when the target is the boss), only once some is spent: whole,
	// it ran the line's length and read as the line splitting.
	bool PoiseShown() const
	{
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		return TopShown() && T.present && (!B.present || T.enemy == B.slot) && T.poise01 < 0.98f;
	}

	// The top line comes and goes with the boss (the budget's boss fade); a plain target's line goes at rest. A target with
	// no display name has no line at all: never a bar without a name.
	bool TopShown() const
	{
		const ftcore::ArenaHudBoss& B = View.now.boss;
		return B.present ? Budget.out.allowed(ftcore::UiMarkKind::Boss) > 0
						 : View.now.target.present && bNamed && Budget.out.allowed(ftcore::UiMarkKind::Target) > 0;
	}

	void HLine(float X0, float X1, float Y, float From01, float To01, const ftcore::UiStrokeStyle& St, const FLinearColor& C) const
	{
		if (To01 - From01 < 0.002f || C.A <= 0.f)
		{
			return;
		}
		ftcore::uiStrokeLine(Line, X0, Y, X1, Y, St, From01, To01);
		Mesh.Stroke(Line, C);
	}

	// A line that carries a value (ValueStyle, Width px at 1080p), its edges on whole pixels and the 1 px dark edge along
	// both sides.
	void ValueLine(float X0, float X1, float Y, float From01, float To01, float Width, std::uint32_t Seed, float S, const FLinearColor& C,
		float Taper = Hud::ValueTaper) const
	{
		if (To01 - From01 < 0.002f || C.A <= 0.f)
		{
			return;
		}
		const float YS = SnapLine(Y, Width * S);
		ftcore::uiStrokeLine(Line, X0, YS, X1, YS, ValueStyle(Width, Seed, S, Taper), From01, To01);
		Mesh.ValueStroke(Line, C);
	}

	// A whole ring, joined without a seam.
	void Ring(float CX, float CY, float R, float Width, std::uint32_t Seed, float S, const FLinearColor& C) const
	{
		ftcore::uiStrokeArc(Line, CX, CY, R, 0.f, 360.f, StrokeStyle(Width, Seed, S, 0.f));
		if (Line.count > 1)
		{
			Line.pt[Line.count - 1].x = Line.pt[0].x;
			Line.pt[Line.count - 1].y = Line.pt[0].y;
		}
		Mesh.Stroke(Line, C, true);
	}

	void Poly(const FVector2f* Pts, int32 N, float HalfWidth, const FLinearColor& C, bool bClosed = false) const
	{
		PolyStroke(Line, Pts, N, HalfWidth);
		Mesh.Stroke(Line, C, bClosed);
	}

	void Danger(float CX, float CY, float Size, const FLinearColor& C) const
	{
		const FVector2f Pts[3] = {FVector2f(CX, CY - Size * 0.55f), FVector2f(CX + Size * 0.55f, CY + Size * 0.45f), FVector2f(CX - Size * 0.55f, CY + Size * 0.45f)};
		Mesh.Fill(Pts, 3, C);
	}

	// Low is not told by colour alone: danger red, and a pulse between 60 and 100 percent at 1 Hz.
	FLinearColor LowTok(bool bLow, float A) const
	{
		return bLow ? Tok(UiToken::Danger, A * (0.8f + 0.2f * FMath::Cos(UE_TWO_PI * Time))) : Tok(UiToken::Ink, A);
	}

	// An hp line: the hp in the stroke over a hairline track (the track only where the hp is not, so it never reads as a
	// second line), what was just lost rubbed out to a broken trace, low under a quarter.
	void HpLine(float X0, float Len, float Y, float Hp, float Was, float Width, std::uint32_t Seed, float S, float A) const
	{
		const float YS = SnapLine(Y, Width * S); // the track and the rub on the value line's own middle
		ftcore::UiStrokeStyle Hair = StrokeStyle(1.f, Seed, S, 0.f);
		Hair.wobble = 0.3f * S;
		HLine(X0, X0 + Len, YS, Hp, 1.f, Hair, Tok(UiToken::Ink, 0.8f * A));
		ValueLine(X0, X0 + Len, YS, 0.f, Hp, Width, Seed + 1u, S, LowTok(Hp < Hud::LowHp, A));
		if (Was > Hp + 0.004f)
		{
			ftcore::UiStrokeStyle Rub = StrokeStyle(1.6f, Seed + 2u, S, 0.f);
			Rub.broken = true;
			HLine(X0, X0 + Len, YS, Hp, Was, Rub, Tok(UiToken::Ink, 0.4f * A));
		}
	}

	static float KeyCapW(float KeyW, float S) { return FMath::Max(Hud::KeyCapH * S, KeyW + Hud::KeyCapPad * S); }

	// A key cap: a 1 px rounded outline round its letters, centred on (CX, MidY). Every key on the HUD is this one cap.
	void KeyCap(const FPainter& P, float CX, float MidY, float W, const FString& Key, float KeyW, const FLinearColor& C) const
	{
		KeyCapStroke(Line, CX, MidY, W, Hud::KeyCapH * P.S, 0.5f * P.S, Hud::KeyCapCorner * P.S);
		Mesh.Stroke(Line, C, true);
		P.Text(CX - KeyW * 0.5f, CapTop(MidY, KeyAsc), Key, KeyFont, C);
	}

	// A first-use label beside its element, at the labels' ink while Core keeps it live.
	void Note(const FPainter& P, ftcore::UiFirstUseItem Item, float X, float MidY, float A) const
	{
		const float Live = ftcore::uiFirstUseAlpha(FirstUse, Item);
		if (Live > 0.f)
		{
			P.Text(X, CapTop(MidY, NoteAsc), NoteWord[static_cast<int32>(Item)], NoteFont, Tok(UiToken::Ink, Hud::NoteInk * A * Live));
		}
	}

	// Each live event mark of a kind the budget grants, with its pose now.
	template <typename FnT>
	void EachMark(ftcore::UiEventMark Kind, FnT&& Fn) const
	{
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiLiveMarkMax); ++I)
		{
			const ftcore::UiLiveMark& M = Marks.m[static_cast<size_t>(I)];
			if (M.live && M.mark == Kind && !(MarkMask & (1u << I)))
			{
				Fn(M, ftcore::uiEventMarkPose(Kind, M.age));
			}
		}
	}

	// The party, bottom left, stacked upward: the guard row (GUARD and its line, drawn only while guard is spent; its place
	// is always kept, so nothing over it moves when it comes), the lead's hp line (the Debt beside it once a world reports
	// one), the lead's name with the Load in it as gold and its state slot to the right, and the partner's row above.
	// Under each name one line, its hp. No numbers. A hit drops the struck one's name 3 px, back over 0.15 s.
	// The party rows' places (PaintParty draws them; the Lone Carry's card stands over them), from the bottom margin up:
	// the guard row's foot on the margin, 8 px over its capitals the lead's hp line, 12 px over that the name's foot (so
	// the RELEASE key cap, centred on its word's capitals, clears the line), and the partner's line 22 px over the name's
	// capitals. On a lead switch the two rows trade places over kUiRowSwap (the lead's row comes down from the partner's
	// place, the partner's goes up from the lead's), so the eye follows each name to its new seat.
	struct FPartyRows
	{
		float GuardBase = 0.f, HpY = 0.f, NameBase = 0.f, PY = 0.f, PBase = 0.f;
		bool bPartner = false;
		float Top = 0.f; // the block's top, without a hit's dip: the higher name's box
	};

	FPartyRows PartyRows(const FVector2f& Size, float S) const
	{
		FPartyRows R;
		const float GuardBase0 = Size.Y - EdgesAt(Size, S).bottom;
		const float HpY0 = GuardBase0 - 0.62f * NoteAsc - (8.f + 0.5f * Hud::LeadHpW) * S;
		const float NameBase0 = HpY0 - (0.5f * Hud::LeadHpW + 12.f) * S;
		const float PY0 = NameBase0 - 0.62f * LeadNameAsc - (22.f + 0.5f * Hud::PartnerHpW) * S; // the partner's line's middle
		const float SwapDy = FMath::SmoothStep(0.f, 1.f, RowSwap.left01) * (PY0 - HpY0);
		R.GuardBase = GuardBase0 + SwapDy;
		R.HpY = HpY0 + SwapDy;
		const float NameRest = NameBase0 + SwapDy;
		R.NameBase = NameRest + 3.f * S * LeadDip; // a hit drops the name, and its slot with it
		const ftcore::ArenaHudPartner& Pa = View.now.partner;
		R.bPartner = Pa.present && Budget.out.allowed(ftcore::UiMarkKind::Party) > 0;
		R.PY = PY0 - SwapDy; // 22 px over the lead's name at rest (the lead's dip does not move it)
		const float PRest = R.PY - (0.5f * Hud::PartnerHpW + 8.f) * S; // the partner's name, 8 px over its line
		R.PBase = PRest + 3.f * S * PartnerDip;
		R.Top = NameRest - LeadNameAsc;
		if (R.bPartner)
		{
			R.Top = FMath::Min(R.Top, PRest - PartnerNameAsc);
		}
		return R;
	}

	void PaintParty(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudLead& L = View.now.lead;
		if (Budget.out.allowed(ftcore::UiMarkKind::Lead) == 0 || L.hpMax <= 0)
		{
			return; // the result card stands alone
		}
		const ftcore::UiStyle& St = Table().Style;
		const float S = P.S;
		const float A = TextAlpha(LeadFade.alpha); // full ink at rest; fades only as the HUD goes
		const float X0 = EdgesAt(Size, S).left;
		const float Len = Hud::LeadLine * S;
		const FPartyRows Rows = PartyRows(Size, S);
		const float GuardBase = Rows.GuardBase;
		const float HpY = Rows.HpY;
		const float NameBase = Rows.NameBase;
		const ftcore::ArenaHudPartner& Pa = View.now.partner;
		const bool bPartner = Rows.bPartner;
		const float PLen = Hud::PartnerLine * S;
		const float PY = Rows.PY;
		const float PBase = Rows.PBase;

		HpLine(X0, Len, HpY, Frac(L.hp, L.hpMax), HpLost.Was, Hud::LeadHpW, 201, S, A);
		if (GuardShown())
		{
			// GUARD in the labels' size at the row's left, its line from past the word to the hp line's end.
			const float G = FMath::Clamp(L.stamina01, 0.f, 1.f);
			P.Text(X0, GuardBase - NoteAsc, GuardWord, NoteFont, Tok(UiToken::Ink, Hud::NoteInk * A));
			ValueLine(X0 + GuardWordW + 12.f * S, X0 + Len, CapMid(GuardBase, NoteAsc), 0.f, G, Hud::GuardW, 205, S, LowTok(G < Hud::LowHp, A), 0.f);
		}
		if (L.debt >= 0)
		{
			P.Text(X0 + Len + 12.f * S, CapTop(HpY, NoteAsc), DebtText, NoteFont, LowTok(L.debt >= 80, 0.7f * A));
		}

		// The Load fills the name in gold from the left. Past the Release threshold the whole name brightens.
		const float NameTop = NameBase - LeadNameAsc;
		P.Text(X0, NameTop, LeadName, LeadNameFont, Tok(UiToken::Ink, A));
		const FLinearColor Gold0 = LinOf(St.loadGold, A);
		const FLinearColor Bright = LinOf(St.loadBright, A);
		const FLinearColor Gold = Gold0 + (Bright - Gold0) * LoadName.bright;
		if (LoadName.fill > 0.001f)
		{
			const FPainter G{P.Geometry, P.Out, P.Layer + 1, S}; // over the ink name
			const FPainter E{P.Geometry, P.Out, P.Layer + 2, S}; // over the gold
			const float FillX = X0 + LeadNameW * LoadName.fill;
			const float ClipTop = NameTop - 4.f * S;
			const float ClipH = LeadNameAsc * 1.6f + 8.f * S;
			Clipped(G, X0 - 4.f * S, ClipTop, FillX - X0 + 4.f * S, ClipH, [&] { G.Text(X0, NameTop, LeadName, GoldFont, Gold); });
			if (LoadName.fill < 0.999f)
			{
				Clipped(E, FillX - 2.f * S, ClipTop, 2.f * S, ClipH, [&] { E.Text(X0, NameTop, LeadName, GoldFont, Bright); }); // the leading edge
			}
			if (LoadName.shimmer >= 0.f)
			{
				// The threshold crossed: one sweep of the bright gold over the name.
				const float BandW = 0.22f * LeadNameW;
				const float BX = X0 - BandW + (LeadNameW + BandW) * LoadName.shimmer;
				Clipped(E, BX, ClipTop, BandW, ClipH, [&] { E.Text(X0, NameTop, LeadName, GoldFont, Bright); });
			}
		}
		// The state slot, right of the name on its baseline, one word (Core's uiLeadWord): RELEASE and its key once the
		// Release is open, else WAIT while an Escrow wait runs (in full ink, its line after it shrinking over the wait),
		// else LOAD while some is held, else ALONE while the lead carries alone (or, for a moment after a gift or the
		// partner ends it, ENDED: GIFT or ENDED: PARTNER in its place), else nothing.
		const float SlotX = X0 + LeadNameW + 16.f * S;
		const FLinearColor SlotC = Tok(UiToken::Ink, Hud::SlotInk * A);
		const ftcore::UiLeadWord SlotWord = ftcore::uiLeadWord(ftcore::uiLeadSlot(L), bWaiting, ftcore::uiLoneWord(Lone));
		switch (SlotWord)
		{
		case ftcore::UiLeadWord::Wait:
		{
			const FLinearColor WaitC = Tok(UiToken::Ink, A);
			P.Text(SlotX, NameBase - NoteAsc, WaitWord, NoteFont, WaitC);
			const float LX = SlotX + WaitWordW + Hud::WaitGap * S;
			ValueLine(LX, LX + Hud::WaitLen * S, CapMid(NameBase, NoteAsc), 0.f, Wait01, Hud::TimerW, 91, S, WaitC, 0.f);
			break;
		}
		case ftcore::UiLeadWord::Load:
			P.Text(SlotX, NameBase - NoteAsc, LoadWord, NoteFont, SlotC);
			break;
		case ftcore::UiLeadWord::Release:
			P.Text(SlotX, NameBase - NoteAsc, ReleaseWord, NoteFont, Gold);
			if (!ReleaseKey.IsEmpty())
			{
				const float CapW = KeyCapW(ReleaseKeyW, S);
				KeyCap(P, SlotX + ReleaseWordW + 8.f * S + CapW * 0.5f, CapMid(NameBase, NoteAsc), CapW, ReleaseKey, ReleaseKeyW, Gold);
			}
			break;
		case ftcore::UiLeadWord::Alone:
			P.Text(SlotX, NameBase - NoteAsc, LoneAloneWord, NoteFont, SlotC);
			break;
		case ftcore::UiLeadWord::EndedGift:
		case ftcore::UiLeadWord::EndedPartner:
			P.Text(SlotX, NameBase - NoteAsc, LoneEndedWord[SlotWord == ftcore::UiLeadWord::EndedGift ? 0 : 1], NoteFont, SlotC);
			break;
		case ftcore::UiLeadWord::None:
			break;
		}

		// The partner's row, whenever the fight has a partner.
		if (!bPartner)
		{
			return;
		}
		HpLine(X0, PLen, PY, Frac(Pa.hp, Pa.hpMax), PartnerLost.Was, Hud::PartnerHpW, 211, S, A);
		P.Text(X0, PBase - PartnerNameAsc, PartnerName, PartnerNameFont, Tok(UiToken::Ink, Hud::PartnerInk * A));
		// A shared blow: SHARED in the partner's slot, right of the name on its baseline, for a second, then fading.
		bool bShared = false;
		EachMark(ftcore::UiEventMark::Shared, [&](const ftcore::UiLiveMark&, const ftcore::UiMarkPose& Po)
		{
			if (!bShared)
			{
				bShared = true;
				P.Text(X0 + PartnerNameW + 16.f * S, PBase - NoteAsc, SharedWord, NoteFont, Tok(UiToken::Ink, Hud::NoteInk * Po.alpha * A));
			}
		});
	}

	// The Lone Carry card's rows under its title: the heads and the longer column's lines (none for the title alone).
	int32 LoneCardRows() const { return Lone.full ? 1 + FMath::Max(LoneGainN, LoneGiveN) : 0; }
	float LoneLineStep(float S) const { return NoteAsc + Hud::LoneLineGap * S; }

	// The card's top: its last line's foot LoneGap px over the party block.
	float LoneCardTop(const FVector2f& Size, float S) const
	{
		const int32 N = LoneCardRows();
		const float H = N > 0 ? LeadNameAsc + Hud::LoneTitleGap * S + static_cast<float>(N - 1) * LoneLineStep(S) + NoteAsc : LeadNameAsc;
		return PartyRows(Size, S).Top - Hud::LoneGap * S - H;
	}

	// The Lone Carry's card as it begins: over the party block, left-aligned on its margin, no panel (the bottom shade
	// rises under it). CARRYING ALONE in the lead name's size; in full, YOU GAIN and YOU GIVE UP over their columns, all in
	// the labels' size: the gains from the bargain's fields, then what is given up, the gift's end last.
	void PaintLone(const FPainter& P, const FVector2f& Size) const
	{
		const float CardA = ftcore::uiLoneCardAlpha(Lone);
		if (CardA <= 0.f || !bLoneCardBuilt || Budget.out.allowed(ftcore::UiMarkKind::Lead) == 0 || View.now.lead.hpMax <= 0)
		{
			return;
		}
		const float S = P.S;
		const float A = CardA * TextAlpha(LeadFade.alpha);
		const float X0 = EdgesAt(Size, S).left;
		const float TitleTop = LoneCardTop(Size, S);
		P.Text(X0, TitleTop, LoneTitle, LeadNameFont, Tok(UiToken::Ink, A));
		if (!Lone.full)
		{
			return;
		}
		const float Step = LoneLineStep(S);
		const float HeadTop = TitleTop + LeadNameAsc + Hud::LoneTitleGap * S;
		const float GiveX = X0 + LoneGainColW + Hud::LoneColGap * S;
		const FLinearColor HeadC = Tok(UiToken::Ink, Hud::LoneHeadInk * A);
		const FLinearColor LineC = Tok(UiToken::Ink, A);
		P.Text(X0, HeadTop, LoneGainHead, NoteFont, HeadC);
		P.Text(GiveX, HeadTop, LoneGiveHead, NoteFont, HeadC);
		for (int32 I = 0; I < LoneGainN; ++I)
		{
			P.Text(X0, HeadTop + static_cast<float>(I + 1) * Step, LoneGainText[I], NoteFont, LineC);
		}
		for (int32 I = 0; I < LoneGiveN; ++I)
		{
			P.Text(GiveX, HeadTop + static_cast<float>(I + 1) * Step, LoneGiveText[I], NoteFont, LineC);
		}
	}

	// The top: the boss as one line from a small oval, its hp over a hairline track with phase notches, the stagger's break
	// and the poise under it; a plain target's line alone. The name over it in spaced small caps.
	void PaintTop(const FPainter& P, const FVector2f& Size) const
	{
		if (!TopShown())
		{
			return;
		}
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		const float S = P.S;
		const float A = B.present ? Budget.out.bossAlpha : TopFade.alpha * RestA;
		const float TA = B.present ? A : TextAlpha(TopFade.alpha) * RestA;
		const float CX = Size.X * 0.5f;
		const float Y = Hud::TopLine * S + (EdgesAt(Size, S).top - Hud::Margin * S); // down with the top edge
		const float Len = Hud::TopLen * S;
		const float X0 = CX - Len * 0.5f;
		const float Hp = B.present ? Frac(B.hp, B.hpMax) : Frac(T.hp, T.hpMax);

		const float HpW = B.present ? 7.f : 3.8f;
		const float YL = SnapLine(Y, HpW * S); // the track and the rub on the value line's own middle
		ftcore::UiStrokeStyle Hair = StrokeStyle(1.f, 99, S, 0.f);
		Hair.wobble = 0.3f * S;
		HLine(X0, X0 + Len, YL, Hp, 1.f, Hair, Tok(UiToken::Ink, 0.8f * A));
		// A stagger: the line breaks open at the phase notch nearest below its end (or at its middle), the right piece knocked
		// aside a little.
		float BreakAt = -1.f;
		if (B.present && BreakK > 0.01f && Hp > 0.02f)
		{
			BreakAt = Hp * 0.5f;
			for (int32 I = 0; I + 1 < B.phaseCount; ++I)
			{
				const float E = B.phaseEnd[static_cast<size_t>(I)];
				if (E < Hp - 0.01f && E > 0.01f)
				{
					BreakAt = E;
					break;
				}
			}
		}
		if (BreakAt > 0.f)
		{
			const float Gap = 0.045f * BreakK;
			ValueLine(X0, X0 + Len, YL, 0.f, FMath::Max(BreakAt - Gap, 0.f), HpW, 101, S, Tok(UiToken::Ink, A));
			ValueLine(X0, X0 + Len, YL + 6.f * S * BreakK, BreakAt + Gap, Hp, HpW, 101, S, Tok(UiToken::Ink, A));
		}
		else
		{
			ValueLine(X0, X0 + Len, YL, 0.f, Hp, HpW, 101, S, Tok(UiToken::Ink, A));
		}
		if (B.present)
		{
			// The line starts from a small drawn oval, open where the line leaves it.
			ftcore::uiStrokeArc(Line, X0 - 12.f * S, Y, 9.f * S, 120.f, 420.f, StrokeStyle(1.6f, 109, S, 0.2f));
			for (uint32 I = 0; I < Line.count; ++I)
			{
				Line.pt[I].y = Y + (Line.pt[I].y - Y) * 0.7f; // flatten the circle to an oval
			}
			Mesh.Stroke(Line, Tok(UiToken::Ink, A));
		}
		if (TopLost.Was > Hp + 0.004f)
		{
			ftcore::UiStrokeStyle Rub = StrokeStyle(1.6f, 101, S, 0.f);
			Rub.broken = true;
			HLine(X0, X0 + Len, YL, Hp, TopLost.Was, Rub, Tok(UiToken::Ink, 0.4f * A));
		}
		if (bNamed)
		{
			P.Text(CX - NameW * 0.5f, Y - 22.f * S - NameAsc, NameText, NameFont, Tok(UiToken::Ink, TA));
		}
		if (B.present && B.weight > 0)
		{
			// The fight's own gauge beside the name: WEIGHT and a dot per stack of the cap, pulsing from three.
			const float MidY = CapMid(Y - 22.f * S, NameAsc);
			const float WX = CX + NameW * 0.5f + 24.f * S;
			P.Text(WX, CapTop(MidY, NoteAsc), WeightWord, NoteFont, Tok(UiToken::Ink, Hud::NoteInk * TA));
			const float Beat = B.weight >= 3 ? 0.8f + 0.2f * FMath::Cos(UE_TWO_PI * Time) : 1.f;
			for (int32 I = 0; I < B.weightMax; ++I)
			{
				Mesh.Dot(WX + WeightW + (13.f + 12.f * I) * S, MidY, 3.f * S, Tok(UiToken::Ink, (I < B.weight ? Beat : 0.3f) * TA));
			}
		}
		// A notch across the line at each phase's floor; the phases already passed step back.
		for (int32 I = 0; B.present && I + 1 < B.phaseCount; ++I)
		{
			const float X = X0 + Len * B.phaseEnd[static_cast<size_t>(I)];
			const FVector2f Pts[2] = {FVector2f(X, Y - 16.f * S), FVector2f(X, Y + 16.f * S)};
			Poly(Pts, 2, 1.8f * S, Tok(I < B.phase ? UiToken::InkDim : UiToken::Ink, (I < B.phase ? 0.55f : 1.f) * A));
		}
		if (PoiseShown())
		{
			// POISE beside its line whenever the line is drawn, at the labels' ink.
			ValueLine(X0, X0 + Len, Y + 12.f * S, 0.f, FMath::Clamp(T.poise01, 0.f, 1.f), Hud::PoiseW, 103, S, Tok(UiToken::Ink, 0.7f * A));
			P.Text(X0 + Len + 12.f * S, CapTop(Y + 12.f * S, NoteAsc), PoiseWord, NoteFont, Tok(UiToken::Ink, Hud::NoteInk * TA));
		}
	}

	// The call word in its one slot on the screen's middle under the top line: a called zone (its in or out mark beside
	// it) or the answer the windup needs, over a 160 px line that shrinks evenly from both ends to nothing on the hit
	// frame; STAGGERED in the same slot with no line. The slot is the boss's while there is one, else the foe's whose
	// windup dealt the Read cards, else the target's (named or not: a call needs no name), so the word never stands off
	// the axis over a body. The Read row stands in the slot, a row lower under a word.
	void PaintCall(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const float S = P.S;
		const float CX = Size.X * 0.5f;
		const float Top = (Hud::TopLine + 40.f) * S + (EdgesAt(Size, S).top - Hud::Margin * S);
		const FFoeMark* BM = SlotFoe >= 0 ? &Foes[SlotFoe] : nullptr;
		// A boss's word comes and goes with its line; a target's is a tell, never at rest: full ink while it stands.
		const float TA = B.present ? (Budget.out.allowed(ftcore::UiMarkKind::Boss) > 0 ? Budget.out.bossAlpha : 0.f)
								   : (BM && BM->bShow ? 1.f : 0.f);
		const bool bCall = CallKind >= 0;
		const bool bStagger = !bCall && BM && BM->bShow && !(B.present && B.down)
			&& (BM->Word != 0 || (B.present && B.weakPoint == ftcore::UiWeakPoint::Open));
		const bool bWord = TA > 0.01f && (bCall || bStagger);
		if (bWord)
		{
			const bool bZone = bCall && CallKind < 3;
			const FString& Word = bStagger ? StaggerWord : bZone ? ZoneWord[CallKind] : AnswerWord[CallKind - 3];
			// The word's ink centred on the axis (not its measured box: see InkMid), so the line under it, centred on the
			// same axis, is centred under the word.
			const float TX = CX - (bStagger ? StaggerMid : bZone ? ZoneMid[CallKind] : AnswerMid[CallKind - 3]);
			if (bZone)
			{
				const float MidY = CapMid(Top + CallAsc, CallAsc);
				if (B.callIn)
				{
					P.Circle(TX - 20.f * S, MidY, 13.f * S, Tok(UiToken::Ally, TA));
				}
				else
				{
					Danger(TX - 20.f * S, MidY, 15.f * S, Tok(UiToken::Danger, TA));
				}
			}
			P.Text(TX, Top, Word, CallFont, Tok(UiToken::Ink, TA));
			const float Half = BM && !bStagger && BM->Tell == 1 ? 0.5f * Hud::CallLine * S * (1.f - BM->Fill) : 0.f;
			if (Half > 0.5f * S)
			{
				ValueLine(CX - Half, CX + Half, Top + CallAsc + 8.f * S, 0.f, 1.f, Hud::TimerW, 87, S, Tok(UiToken::Ink, TA), 0.f);
			}
		}
		// A Qte is a call the player answers: its word in the slot as a call word stands (its ink on the axis), its key cap
		// after it, over the same 160 px line under the word shrinking evenly to nothing at the window's end. It is the only
		// prompt with a line. A live call word keeps the slot, so the Qte sits a row lower, as the Read row does; the Read
		// row then goes under the Qte.
		float SlotTop = bWord ? Top + CallAsc + 42.f * S : Top; // under a word: 34 px under its line
		const ftcore::ArenaHudQte& Q = View.now.lead.qte;
		const int32 QteButton = static_cast<int32>(ftcore::UiPrompt::Attack) + static_cast<int32>(Q.input);
		if (bQteOn && QteButton < static_cast<int32>(ftcore::kUiPromptCount) && !PromptKey[QteButton].IsEmpty())
		{
			const int32 Wi = Q.kind == ftcore::QteKind::Tap ? static_cast<int32>(Q.input)
				: static_cast<int32>(ftcore::kActionButtonCount) + (Q.kind == ftcore::QteKind::Hold ? 0 : 1);
			const float CapW = KeyCapW(PromptKeyW[QteButton], S);
			const float TX = CX - QteMid[Wi];
			const FLinearColor C = Tok(UiToken::Ink);
			P.Text(TX, SlotTop, QteWord[Wi], CallFont, C);
			KeyCap(P, TX + QteWordW[Wi] + 10.f * S + CapW * 0.5f, CapMid(SlotTop + CallAsc, CallAsc), CapW, PromptKey[QteButton],
				PromptKeyW[QteButton], C);
			const float Half = 0.5f * Hud::CallLine * S * QteLeft;
			if (Half > 0.5f * S)
			{
				ValueLine(CX - Half, CX + Half, SlotTop + CallAsc + 8.f * S, 0.f, 1.f, Hud::TimerW, 89, S, C, 0.f);
			}
			SlotTop += CallAsc + 42.f * S;
		}
		// TOO SOON: a refusal made too early, in ink as a call word, for a second. It goes under whatever the slot holds
		// (the call word, a Qte, the Read row), so nothing live moves for it.
		const auto TooSoon = [&](float At)
		{
			if (bTooSoonOn)
			{
				P.Text(CX - TooSoonMid, At, TooSoonWord, CallFont, Tok(UiToken::Ink));
			}
		};
		if (ReadN <= 0)
		{
			TooSoon(SlotTop);
			return;
		}
		// NEXT MOVE (and xN on a streak of two or more), then each card's key cap and then its move (only the picked card
		// once a pick is made; one card dealt still shows its cap first): the whole row centred on the call word's axis,
		// every part's capitals on one middle. A card's cap is drawn as every key cap on the screen (KeyCap: the prompts'
		// box, height and letters), 10 px before its word as a prompt's cap is 10 px after its word.
		const int32 From = ReadPicked >= 0 ? ReadPicked : 0;
		const int32 To = ReadPicked >= 0 ? ReadPicked + 1 : ReadN;
		const float Gap = 24.f * S; // three spaces
		float W = ReadWordW + (StackW > 0.f ? 8.f * S + StackW : 0.f);
		for (int32 I = From; I < To; ++I)
		{
			W += Gap + KeyCapW(ReadNumW[I], S) + (ReadCard[I] >= 0 ? 10.f * S + ReadMoveW[ReadCard[I]] : 0.f);
		}
		const float RowTop = SlotTop;
		const float MidY = CapMid(RowTop + ReadAsc, ReadAsc);
		const FLinearColor C = Tok(UiToken::Ink);
		float X = CX - W * 0.5f;
		P.Text(X, CapTop(MidY, PrefixAsc), ReadWord, PrefixFont, Tok(UiToken::Ink, Hud::PrefixInk));
		X += ReadWordW;
		if (StackW > 0.f)
		{
			X += 8.f * S;
			P.Text(X, CapTop(MidY, PrefixAsc), StackText, PrefixFont, C);
			X += StackW;
		}
		for (int32 I = From; I < To; ++I)
		{
			X += Gap;
			const float CapW = KeyCapW(ReadNumW[I], S);
			KeyCap(P, X + CapW * 0.5f, MidY, CapW, ReadNum[I], ReadNumW[I], C);
			X += CapW;
			if (ReadCard[I] >= 0)
			{
				X += 10.f * S;
				P.Text(X, RowTop, ReadMoveWord[ReadCard[I]], ReadFont, C);
				X += ReadMoveW[ReadCard[I]];
			}
		}
		TooSoon(MidY + (0.5f * Hud::KeyCapH + 34.f) * S); // 34 px under the row's key caps, as a row under a call line
	}

	// The second hp line under the boss's: the target, when it is not the boss. Its own grant and fade (the boss line may be
	// gone at rest while an add is still targeted).
	void PaintSecondLine(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		if (!(B.present && T.present && T.enemy != B.slot) || Budget.out.allowed(ftcore::UiMarkKind::Target) == 0)
		{
			return;
		}
		const float S = P.S;
		const float A = TopFade.alpha * RestA;
		const float OY = (Hud::TopLine + 96.f) * S + (EdgesAt(Size, S).top - Hud::Margin * S);
		const float OLen = 300.f * S;
		const float OX = Size.X * 0.5f - OLen * 0.5f;
		const float OHp = Frac(T.hp, T.hpMax);
		const float OYL = SnapLine(OY, Hud::FoeHpW * S);
		HLine(OX, OX + OLen, OYL, OHp, 1.f, StrokeStyle(1.f, 105, S, 0.f), Tok(UiToken::Ink, 0.3f * A));
		ValueLine(OX, OX + OLen, OYL, 0.f, OHp, Hud::FoeHpW, 106, S, Tok(UiToken::Ink, A));
		ValueLine(OX, OX + OLen, OY + 10.f * S, 0.f, FMath::Clamp(T.poise01, 0.f, 1.f), Hud::PoiseW, 107, S, Tok(UiToken::Ink, 0.7f * A));
	}

	// The prompts, bottom right: each in its own row counted up from the bottom margin, which it never leaves (Core's
	// uiPromptRow: HEAL, GIFT, RELOAD, PARTNER, then a first fight's ATTACK, GUARD, DODGE and JUMP); right-aligned, its
	// word and its key cap. Every prompt whose action is in this fight stands in its row (Core's uiPromptsPresent, then
	// the grant); one is gone only when its action is not in this fight (no partner, no gift slot, no reload press). One
	// whose press would be refused now (Core's uiPromptsDimmed) is drawn at 40 percent ink, its key cap too, its reason
	// word (the rules' PressReason: COOLDOWN, FULL, BROKEN...) after its word, before its key cap, at label size; one whose
	// action the import left unbound shows NO KEY in its key box, dimmed the same. Nothing behind them but the bottom
	// shade. A gift row is ink like the rest: gold is the lead's own power and a gift is not. The GIFT row is the one that
	// may hold two: with both gifts GIFT GUARD keeps the margin and GIFT HEAL stands to its left, GiftGap px from GIFT
	// GUARD's word (or its reason); either alone takes the margin.
	void PaintPrompts(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::UiPromptMask Shown = Budget.out.promptsShown;
		const float S = P.S;
		const ftcore::UiMargins E = EdgesAt(Size, S);
		const float RX = Size.X - E.right;
		const float Gap = 10.f * S;
		// The reason word a prompt carries (its PressReason's index), or -1: a press that would be taken has none.
		const auto WhyOf = [this](int32 Of) -> int32
		{
			const ftcore::PressReason Answer = Why[static_cast<size_t>(Of)];
			const int32 R = static_cast<int32>(Answer);
			return ftcore::pressRefused(Answer) && !WhyWord[R].IsEmpty() ? R : -1;
		};
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiPromptCount) && Shown != 0; ++I)
		{
			if (!((Shown >> I) & 1u) || PromptKey[I].IsEmpty())
			{
				continue; // nothing reaches it on this device: not drawn
			}
			const FLinearColor C = ((PromptDimmed >> I) & 1u) ? Tok(UiToken::Ink, 0.4f) : Tok(UiToken::Ink);
			const float Row = static_cast<float>(ftcore::uiPromptRow(static_cast<ftcore::UiPrompt>(I)));
			const float MidY = Size.Y - E.bottom - (0.5f * Hud::KeyCapH + Row * Hud::PromptStep) * S;
			const float CapW = KeyCapW(PromptKeyW[I], S);
			float At = RX;
			constexpr int32 GG = static_cast<int32>(ftcore::UiPrompt::GiftGuard);
			if (I == static_cast<int32>(ftcore::UiPrompt::GiftHeal) && ((Shown >> GG) & 1u) && !PromptKey[GG].IsEmpty())
			{
				const int32 GuardWhy = WhyOf(GG);
				At -= KeyCapW(PromptKeyW[GG], S) + Gap + PromptWordW[GG] + (GuardWhy >= 0 ? WhyW[GuardWhy] + Gap : 0.f)
					+ Hud::GiftGap * S;
			}
			KeyCap(P, At - CapW * 0.5f, MidY, CapW, PromptKey[I], PromptKeyW[I], C);
			float WX = At - CapW - Gap;
			const int32 WhyAt = WhyOf(I);
			if (WhyAt >= 0)
			{
				WX -= WhyW[WhyAt];
				P.Text(WX, CapTop(MidY, NoteAsc), WhyWord[WhyAt], NoteFont, C);
				WX -= Gap;
			}
			// The two rows with a count carry it in their word (built when it changes).
			const bool bHeal = I == static_cast<int32>(ftcore::UiPrompt::Heal);
			const bool bGiftHeal = I == static_cast<int32>(ftcore::UiPrompt::GiftHeal);
			const float WW = bHeal ? HealW : bGiftHeal ? GiftHealW : PromptWordW[I];
			const FString& Word = bHeal ? HealText : bGiftHeal ? GiftHealText : PromptWord[I];
			P.Text(WX - WW, CapTop(MidY, PromptAsc), Word, PromptFont, C);
		}
	}

	// The HUD's edges, px: its own margin (Hud::Margin at 1080p, by S), plus the platform's title-safe padding (SafeRatio,
	// read when the viewport's size changes: a ratio of half the viewport on each side), plus the HUD safe margin setting
	// (a percent of the viewport's width at the sides, of its height at the top and bottom), per side (Core's
	// uiHudMargins). Edge marks sit on these; centred marks keep the viewport's centre.
	ftcore::UiMargins EdgesAt(const FVector2f& Size, float S) const
	{
		const ftcore::UiMargins Pad{SafeRatio.Left * Size.X * 0.5f, SafeRatio.Top * Size.Y * 0.5f, SafeRatio.Right * Size.X * 0.5f,
			SafeRatio.Bottom * Size.Y * 0.5f};
		return ftcore::uiHudMargins(Size.X, Size.Y, Hud::Margin * S, Pad, HudSafePct);
	}

	// The lock: four static corner arcs round the locked body. Without a lock, a dot at the centre while aiming a gun. The
	// cylinder's chambers beside either while aiming, filled while loaded, a faint ring once spent; a reload draws along
	// the same arc as it fills.
	void PaintAim(const FPainter& P, const FVector2f& Size) const
	{
		const float S = P.S;
		if (bLockOn)
		{
			for (int32 K = 0; K < 4; ++K)
			{
				const float Mid = 45.f + 90.f * K;
				ftcore::uiStrokeArc(Line, LockAt.X, LockAt.Y, 58.f * S, Mid - 14.f, Mid + 14.f, StrokeStyle(1.7f, 111u + static_cast<uint32>(K), S, 0.f));
				Mesh.Stroke(Line, Tok(UiToken::Ink));
			}
		}
		const ftcore::ArenaHudLead& L = View.now.lead;
		if (L.roundsMax == 0 || AimA <= 0.01f)
		{
			return;
		}
		const FVector2f Mid(Size.X * 0.5f, Size.Y * 0.5f);
		if (!bLockOn)
		{
			Mesh.Dot(Mid.X, Mid.Y, 1.5f * S, Tok(UiToken::Ink, AimA));
		}
		if (Budget.out.allowed(ftcore::UiMarkKind::Rounds) == 0)
		{
			return;
		}
		const FVector2f At = bLockOn ? LockAt : Mid;
		const float R0 = bLockOn ? 74.f : 46.f;
		const float R = R0 * S;
		const float Step = 19.f * 46.f / R0; // degrees between chambers: one spacing at either radius
		const float A0 = 90.f - Step * 0.5f * (L.roundsMax - 1);
		const float A1 = 90.f + Step * 0.5f * (L.roundsMax - 1);
		for (int32 I = 0; I < L.roundsMax; ++I)
		{
			float X;
			float Y;
			ftcore::uiArcPoint(At.X, At.Y, R, A0 + Step * I, X, Y);
			if (I < L.rounds)
			{
				Mesh.Dot(X, Y, 7.f * S, Tok(UiToken::Ink, AimA));
			}
			else
			{
				ftcore::uiStrokeArc(Line, X, Y, 6.f * S, 0.f, 360.f, StrokeStyle(1.4f, 63u + static_cast<uint32>(I), S, 0.f));
				Mesh.Stroke(Line, Tok(UiToken::Ink, 0.5f * AimA), true);
			}
		}
		if (L.reload01 > 0.f)
		{
			ftcore::uiStrokeArc(Line, At.X, At.Y, R + 13.f * S, A0 - Step * 0.5f, A1 + Step * 0.5f, StrokeStyle(1.2f, 61, S, 0.f), 0.f,
				FMath::Clamp(L.reload01, 0.f, 1.f));
			Mesh.Stroke(Line, Tok(UiToken::Ink, AimA));
		}
	}

	// Text at the rest alpha keeps full strength (a faded word on bright sky reads as a hollow outline); below the rest
	// alpha (the HUD going away) it fades with the rest.
	static float TextAlpha(float A)
	{
		const float Rest = Table().Style.fade.restAlpha;
		return Rest > 0.01f ? FMath::Min(1.f, A / Rest) : A;
	}

	// A world point on this widget (false: behind the camera or no view).
	static bool Project(const APlayerController* PC, const FVector& World, const FVector2f& Size, FVector2f& Out)
	{
		int32 VX = 0;
		int32 VY = 0;
		if (!PC)
		{
			return false;
		}
		PC->GetViewportSize(VX, VY);
		FVector2D Px;
		if (VX <= 0 || VY <= 0 || !PC->ProjectWorldLocationToScreen(World, Px, true))
		{
			return false;
		}
		Out = FVector2f(static_cast<float>(Px.X) * Size.X / VX, static_cast<float>(Px.Y) * Size.Y / VY);
		return true;
	}

	// The world's marks this frame: each enemy's place, hp, state and tell (on the body, or at the screen's edge when off
	// frame), the Read cards, the weak point, the lock and the call word. Fixed arrays, no allocation.
	void TickWorld(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, const APlayerController* PC, const FVector2f& Size)
	{
		const auto OnScreen = [&Size](const FVector2f& Q) { return Q.X >= 0.f && Q.Y >= 0.f && Q.X <= Size.X && Q.Y <= Size.Y; };
		const double Now = Runner.GetNow();
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		ReadN = 0;
		ReadPicked = -1;
		int32 ReadSlot = -1; // the foe whose windup dealt the Read cards on show
		for (int32 I = 0; I < UFTArenaRunner::MaxEnemies; ++I)
		{
			// A minor enemy's name by its kind, built when the kind changes (most kinds have none).
			const char* Kind = Runner.GetSlotKindId(I);
			if (!bFoeNames || Kind != FoeKind[I])
			{
				FoeKind[I] = Kind;
				FoeNamed[I] = KindName(Runner, I, FoeNameFont, FoeName[I], FoeNameW[I]);
			}
			FFoeMark& M = Foes[I];
			M = FFoeMark{};
			const AFTArenaFighter* Figure = Runner.GetEnemyFigure(I);
			if (I >= static_cast<int32>(Fight.enemyCount()) || !Figure || !Fight.enemyAlive(static_cast<size_t>(I)))
			{
				continue;
			}
			const size_t Ix = static_cast<size_t>(I);
			M.bShow = true;
			const bool bOver = Project(PC, Figure->GetAnchor(), Size, M.Over);
			const bool bChest = Project(PC, Figure->GetChest(), Size, M.Chest);
			M.bOn = bOver && bChest && OnScreen(M.Chest);
			// The top line carries the target's and the boss's hp: over those no block of their own.
			M.bLine = !(B.present && B.slot == I) && !(T.present && T.enemy == I);
			const ftcore::ArenaEnemyKind& EK = Fight.def().enemies[Ix];
			M.Hp01 = EK.hp > 0 ? FMath::Clamp(static_cast<float>(Fight.enemyHp(Ix)) / EK.hp, 0.f, 1.f) : 0.f;
			const ftcore::ArenaEnemyOut& E = Fight.output().enemies[Ix];
			M.Word = E.brain == ftcore::ArenaBrain::Staggered ? 1 : (E.brain == ftcore::ArenaBrain::Open ? 2 : 0);
			const ftcore::ArenaEnemyMove* Move = Runner.GetEnemyMove(I);
			if (!Move)
			{
				continue;
			}
			M.Answer = Move->unblockable ? 2 : (!Move->parryable ? 1 : 0);
			M.Impact = Pulse(Now, Runner.GetEnemyActiveSince(I), kImpactLife);
			const bool bStartup = E.part == ftcore::ArenaPart::Startup;
			if (!bStartup && M.Impact <= 0.f)
			{
				continue;
			}
			M.Tell = bStartup ? 1 : 2;
			M.Fill = bStartup ? FMath::Clamp(E.stateTime / FMath::Max(Move->attack.startup, 0.01f), 0.f, 1.f) : 1.f;
			if (bStartup && Fight.readFoe() == static_cast<uint8>(I) && Fight.readCards().dealt())
			{
				const ftcore::ReadCards& Cards = Fight.readCards();
				ReadN = FMath::Min(static_cast<int32>(Cards.size()), 3);
				ReadSlot = I;
				ReadPicked = Cards.picked() ? static_cast<int32>(Cards.pickedIndex()) : -1;
				for (int32 C = 0; C < ReadN; ++C)
				{
					ReadCard[C] = ReadMoveIndex(Cards[static_cast<size_t>(C)].tell);
				}
			}
			// Out of the camera's view (the runner's off-frame test, the one the stinger answers to), or off the screen.
			if (!M.bOn || (bStartup && Runner.IsOffFrame(I)))
			{
				M.bEdge = true;
				if (PC && PC->PlayerCameraManager)
				{
					const FVector Local = PC->PlayerCameraManager->GetCameraRotation().UnrotateVector(
						Figure->GetChest() - PC->PlayerCameraManager->GetCameraLocation());
					M.Dir = FVector2f(static_cast<float>(Local.Y), static_cast<float>(-Local.Z));
				}
				if (M.Dir.SizeSquared() < 1e-6f)
				{
					M.Dir = FVector2f(0.f, 1.f);
				}
				M.Dir.Normalize();
			}
		}
		bFoeNames = true;
		// The weak point while it is open: on the boss's body itself.
		bWeakOn = false;
		if (B.present && B.weakPoint == ftcore::UiWeakPoint::Open)
		{
			FVector Weak;
			bWeakOn = Runner.GetBossWeakPoint(static_cast<int32>(B.slot), Weak) && Project(PC, Weak, Size, WeakAt) && OnScreen(WeakAt);
		}
		// The lock: on the locked body while it is on the screen.
		bLockOn = false;
		const uint8 Lock = Fight.lock();
		if (Lock != ftcore::kArenaNobody)
		{
			const AFTArenaFighter* Figure = Runner.GetEnemyFigure(static_cast<int32>(Lock));
			bLockOn = Figure && Project(PC, Figure->GetChest(), Size, LockAt) && OnScreen(LockAt);
		}
		// The call slot's foe: the boss; else the foe whose windup dealt the Read cards (the row stands under its word);
		// else the target. Its word: a called zone, else the answer its winding-up move needs.
		SlotFoe = B.present ? (B.slot < UFTArenaRunner::MaxEnemies ? static_cast<int32>(B.slot) : -1)
			: ReadSlot >= 0 ? ReadSlot
							: (T.present && T.enemy < UFTArenaRunner::MaxEnemies ? static_cast<int32>(T.enemy) : -1);
		CallKind = -1;
		if (B.present && !B.invulnerable && !B.down && B.call != ftcore::UiCalledZone::None)
		{
			CallKind = FMath::Clamp(static_cast<int32>(B.call) - 1, 0, 2);
		}
		else if ((!B.present || (!B.invulnerable && !B.down)) && SlotFoe >= 0 && Foes[SlotFoe].bShow && Foes[SlotFoe].Tell == 1)
		{
			CallKind = 3 + FMath::Min<int32>(Foes[SlotFoe].Answer, 2);
		}
	}

	// The fight's events this frame: each new event's mark (Core's table: a stagger breaks the boss line, a shared blow
	// shows SHARED), the hit's edge flash, the result and the autoplay tag.
	void TickEvents(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, float DeltaTime)
	{
		const double Now = Runner.GetNow();
		const int32 Count = Runner.GetPopupCount();
		const int32 First = Runner.GetPopupTotal() - Count;
		ftcore::uiMarkTick(Marks, DeltaTime);
		bMarkNew = false;
		if (Fight.over())
		{
			ftcore::uiMarkClear(Marks);
			MarkPushed = Runner.GetPopupTotal();
		}
		else
		{
			for (int32 Serial = FMath::Max(MarkPushed, First); Serial < First + Count; ++Serial)
			{
				const UFTArenaRunner::FPopup& Pp = Runner.GetPopup(Serial - First);
				uint8 From = Pp.Source;
				uint8 To = Pp.Target;
				switch (Pp.Type)
				{
				case ftcore::ArenaEventType::TableShare:
				case ftcore::ArenaEventType::TookHalf:
					// One SHARED by the partner's row however many shares the blow made: the same bodies restart it.
					From = ftcore::kArenaNobody;
					To = ftcore::kArenaNobody;
					break;
				case ftcore::ArenaEventType::Staggered:
				case ftcore::ArenaEventType::CounterStagger:
					if (To >= UFTArenaRunner::MaxEnemies)
					{
						continue; // the break is the boss line's
					}
					break;
				default:
					break;
				}
				bMarkNew |= ftcore::uiMarkPush(Marks, Pp.Type, From, To);
			}
			MarkPushed = First + Count;
			TickStrip();
		}
		// The stagger's break on the boss line: a live break on the boss, else held open while it stands staggered or open.
		BreakK = 0.f;
		const ftcore::ArenaHudBoss& B = View.now.boss;
		if (B.present)
		{
			for (const ftcore::UiLiveMark& M : Marks.m)
			{
				if (M.live && M.mark == ftcore::UiEventMark::LineBreak && M.to == B.slot)
				{
					const ftcore::UiMarkPose Po = ftcore::uiEventMarkPose(M.mark, M.age);
					BreakK = FMath::Max(BreakK, Po.tip * Po.alpha);
				}
			}
			if (B.weakPoint == ftcore::UiWeakPoint::Open || (B.slot < UFTArenaRunner::MaxEnemies && Foes[B.slot].bShow && Foes[B.slot].Word != 0))
			{
				BreakK = 1.f;
			}
		}
		HitA = Pulse(Now, Runner.GetPlayerHitTime(), kHitEdgeLife);
		ParryA = Pulse(Now, Runner.GetParryTime(), kParryEdgeLife);
		bResult = Fight.over();
		if (bResult)
		{
			bWon = Fight.phase() == ftcore::ArenaPhase::Victory;
			ResultA = FMath::Clamp(static_cast<float>(Runner.GetResultAge() / kResultFade), 0.f, 1.f);
			bAgain = !Runner.IsAutoPlay() && Runner.CanFightAgain();
		}
		static const bool bShotRun = FParse::Param(FCommandLine::Get(), TEXT("ArenaShotUi")); // review stills carry no debug tag
		TagMode = bShotRun ? 0 : (Runner.IsIdle() ? 2 : (Runner.IsAutoPlay() ? 1 : 0));
	}

	// -FTNewUiStrip=<path prefix>: the marks' motion strips for review. The first live mark of each kind is held at fixed
	// ages, one still a step (<prefix>_<mark>_<age in ms>.png), while the fight runs on under it; then it ages on as usual.
	void TickStrip()
	{
		static FString Prefix;
		static const bool bStrip = FParse::Value(FCommandLine::Get(), TEXT("FTNewUiStrip="), Prefix);
		if (!bStrip)
		{
			return;
		}
		static constexpr float kAges[] = {0.f, // fractions of the mark's life
			0.04f, 0.08f, 0.12f, 0.2f, 0.3f, 0.45f, 0.6f, 0.8f, 0.95f};
		static const TCHAR* const kNames[ftcore::kUiEventMarkCount] = {TEXT("none"), TEXT("linebreak"), TEXT("shared")};
		if (StripSlot < 0)
		{
			for (int32 I = 0; I < static_cast<int32>(ftcore::kUiLiveMarkMax); ++I)
			{
				const ftcore::UiLiveMark& M = Marks.m[I];
				if (M.live && !(StripDone & (1u << static_cast<uint32>(M.mark))))
				{
					StripSlot = I;
					StripKind = M.mark;
					StripStep = 0;
					StripWait = 0;
					break;
				}
			}
			if (StripSlot < 0)
			{
				return;
			}
		}
		ftcore::UiLiveMark& M = Marks.m[StripSlot];
		const uint32 Bit = 1u << static_cast<uint32>(StripKind);
		if (M.mark != StripKind)
		{
			StripDone |= Bit; // its slot was taken: the strip ends where it got to
			StripSlot = -1;
			return;
		}
		if (StripStep > 0)
		{
			M.live = true;
			M.age = StripAge;
		}
		if (FScreenshotRequest::IsScreenshotRequested() || ++StripWait < 3)
		{
			return;
		}
		if (StripStep >= static_cast<int32>(UE_ARRAY_COUNT(kAges)))
		{
			StripDone |= Bit;
			StripSlot = -1;
			return;
		}
		StripAge = kAges[StripStep] * ftcore::uiEventMarkLife(StripKind);
		M.live = true;
		M.age = StripAge;
		const FString File = FString::Printf(TEXT("%s_%s_%04d.png"), *Prefix, kNames[static_cast<int32>(StripKind)],
			FMath::RoundToInt(StripAge * 1000.f));
		FScreenshotRequest::RequestScreenshot(File, true, false);
		UE_LOG(LogForgottenThird, Display, TEXT("FTNewUi strip: %s (from %d to %d)."), *File, M.from, M.to);
		++StripStep;
		StripWait = 0;
	}

	// The world's marks, under everything else: a minor enemy's name and hp line over its body once hit or winding up, the
	// call word under it (the call slot's foe's stands in the slot on the screen's middle instead), a hit's corner arcs,
	// the off-frame chevrons and the weak point.
	void PaintWorld(const FPainter& P, const FVector2f& Size) const
	{
		const float S = P.S;
		for (int32 I = 0; I < UFTArenaRunner::MaxEnemies; ++I)
		{
			const FFoeMark& M = Foes[I];
			if (!M.bShow)
			{
				continue;
			}
			const bool bSlot = I == SlotFoe; // its word stands in the call slot (the boss's under its name)
			if (M.bOn && !bSlot)
			{
				const float W = Hud::FoeLine * S;
				const float Y = M.Over.Y;
				if (M.Shows())
				{
					HpLine(M.Over.X - W * 0.5f, W, Y, M.Hp01, M.Hp01, Hud::FoeHpW, 71u + static_cast<uint32>(I) * 3u, S, 1.f);
					if (FoeNamed[I])
					{
						P.Text(M.Over.X - FoeNameW[I] * 0.5f, Y - 7.f * S - FoeNameAsc, FoeName[I], FoeNameFont, Tok(UiToken::Ink));
					}
				}
				const bool bAnswer = M.Tell == 1;
				if (bAnswer || M.Word != 0)
				{
					const float Mid = bAnswer ? AnswerSMid[M.Answer] : StaggerSMid; // centred under the line by its ink
					P.Text(M.Over.X - Mid, Y + 8.f * S, bAnswer ? AnswerWord[M.Answer] : StaggerWord, SmallCallFont, Tok(UiToken::Ink));
				}
			}
			if (M.Tell == 0)
			{
				continue;
			}
			if (M.bEdge)
			{
				PaintEdge(P, Size, M, !bSlot);
			}
			else if (M.Tell == 2)
			{
				// The hit: the lock's own shape, its four corner arcs spreading out from the body as they fade.
				const float Hit = M.Impact;
				for (int32 K = 0; K < 4; ++K)
				{
					const float Mid = 45.f + 90.f * K;
					ftcore::uiStrokeArc(Line, M.Chest.X, M.Chest.Y, (58.f + 70.f * (1.f - Hit)) * S, Mid - 14.f, Mid + 14.f,
						StrokeStyle(1.7f + 1.3f * Hit, 95u + static_cast<uint32>(K), S, 0.f));
					Mesh.Stroke(Line, Tok(UiToken::Ink, Hit));
				}
			}
		}
		if (bWeakOn)
		{
			// The weak point open on the body: a diamond in a ring, breathing.
			const float Br = 0.5f + 0.5f * FMath::Sin(Time * 6.f);
			const FLinearColor C = Tok(UiToken::Ink, 0.8f + 0.2f * Br);
			const FVector2f Pts[4] = {FVector2f(WeakAt.X, WeakAt.Y - 7.f * S), FVector2f(WeakAt.X + 7.f * S, WeakAt.Y), FVector2f(WeakAt.X, WeakAt.Y + 7.f * S),
				FVector2f(WeakAt.X - 7.f * S, WeakAt.Y)};
			Mesh.Fill(Pts, 4, C);
			Ring(WeakAt.X, WeakAt.Y, (24.f + 4.f * Br) * S, 1.6f, 141, S, C);
			Note(P, ftcore::UiFirstUseItem::WeakPointLabel, WeakAt.X + 40.f * S, WeakAt.Y, 1.f);
		}
	}

	// An off-frame tell: a chevron on the HUD's edge (EdgesAt: 64 px inside the screen's, plus the safe zone) pointing at
	// the enemy, the answer word beside it while it winds up (a minor enemy's; the boss's stands under its name). At the
	// hit, the chevron fades.
	void PaintEdge(const FPainter& P, const FVector2f& Size, const FFoeMark& M, bool bWord) const
	{
		const float S = P.S;
		const float CX = Size.X * 0.5f;
		const float CY = Size.Y * 0.5f;
		const ftcore::UiMargins E = EdgesAt(Size, S); // each side its own: the way out picks the side
		const float HalfW = FMath::Max(0.f, CX - (M.Dir.X < 0.f ? E.left : E.right));
		const float HalfH = FMath::Max(0.f, CY - (M.Dir.Y < 0.f ? E.top : E.bottom));
		const float T = FMath::Min(M.Dir.X != 0.f ? HalfW / FMath::Abs(M.Dir.X) : 1e9f, M.Dir.Y != 0.f ? HalfH / FMath::Abs(M.Dir.Y) : 1e9f);
		const FVector2f Tip(CX + M.Dir.X * T, CY + M.Dir.Y * T);
		const bool bWind = M.Tell == 1;
		const FLinearColor C = Tok(UiToken::Ink, bWind ? 1.f : M.Impact);
		const float Len = 16.f * S;
		const FVector2f Back = Tip - M.Dir * Len;
		const FVector2f Side(-M.Dir.Y * Len * 0.8f, M.Dir.X * Len * 0.8f);
		const FVector2f Pts[3] = {Back + Side, Tip, Back - Side};
		Poly(Pts, 3, 1.2f * S, C);
		if (!bWind || !bWord)
		{
			return;
		}
		// The word's middle set inward from the chevron by its own half size along the way in.
		const float HW = AnswerSW[M.Answer] * 0.5f;
		const float HH = SmallCallAsc * 0.5f;
		const FVector2f At = Back - M.Dir * (12.f * S + FMath::Abs(M.Dir.X) * HW + FMath::Abs(M.Dir.Y) * HH);
		P.Text(At.X - AnswerSMid[M.Answer], CapTop(At.Y, SmallCallAsc), AnswerWord[M.Answer], SmallCallFont, C);
	}

	// The events, over everything else: the hit's edge flash, the result over the subtitles' gradient, the debug tag.
	void PaintEvents(const FPainter& P, const FVector2f& Size) const
	{
		const float S = P.S;
		const float CX = Size.X * 0.5f;
		// A hit: a brief flash of the danger colour in from the screen's edges (under 0.4 s), each edge a soft lens falloff with
		// no inner edge or line anywhere.
		if (HitA > 0.f)
		{
			const FLinearColor C = Tok(UiToken::Danger, 1.f);
			const float Pk = 0.3f * HitA * HitA;
			const float D = 120.f * S;
			Mesh.Vignette(CX, 0.f, Size.X * 0.7f, D, Pk, &C);
			Mesh.Vignette(CX, Size.Y, Size.X * 0.7f, D, Pk, &C);
			Mesh.Vignette(0.f, Size.Y * 0.5f, D, Size.Y * 0.7f, Pk, &C);
			Mesh.Vignette(Size.X, Size.Y * 0.5f, D, Size.Y * 0.7f, Pk, &C);
		}
		// The result: the word in the subtitle's face at the subtitle's place, and how to fight again in the key hint's.
		// Both up, and the tag in, by what the safe zone adds to the HUD's own margin.
		const ftcore::UiMargins E = EdgesAt(Size, S);
		if (bResult && ResultA > 0.f)
		{
			PaintLowGradient(Mesh, Size, Table().Style.backingSoftDialogue * ResultA);
			const float WordW = bWon ? VictoryW : DefeatW;
			const float Up = E.bottom - Hud::Margin * S;
			P.Text(CX - WordW * 0.5f, Size.Y - 96.f * S - Up - ResultLineH, bWon ? VictoryWord : DefeatWord, ResultFont,
				Tok(bWon ? UiToken::Ink : UiToken::Danger, ResultA));
			if (bAgain)
			{
				P.Text(CX - AgainW * 0.5f, Size.Y - 50.f * S - Up, AgainWord, LabelFont, Tok(UiToken::InkDim, ResultA));
			}
		}
		if (TagMode > 0)
		{
			P.Text(40.f * S + (E.left - Hud::Margin * S), 30.f * S + (E.top - Hud::Margin * S), TagWord[TagMode - 1], LabelFont,
				Tok(UiToken::InkDim));
		}
	}

	TFunction<const UFTArenaRunner*()> GetRunner;
	mutable FTextRuns TextRuns;    // the words' kept glyph runs (FTextRuns), drawn from in OnPaint
	FDelegateHandle FontRelease;   // Slate's font release, which drops them
	ftcore::ArenaHudView View;
	ftcore::UiFade LeadFade;
	ftcore::UiFade TopFade;
	FLostTrace HpLost;
	FLostTrace PartnerLost;
	FLostTrace TopLost;
	ftcore::UiLoadName LoadName;
	float AimA = 0.f; // the aim stance's fade (the runner's): the aim dot and the cylinder
	float HudScale = 1.f; // over the viewport's scale (the setting, or -FTHudScale)
	float LeadDip = 0.f, PartnerDip = 0.f; // a hit's name drop: 1 on the hit frame, 0 after 0.15 s
	ftcore::InputDevice KeyDevice = ftcore::InputDevice::Keys;
	bool bRebuild = true;
	mutable FLineMesh Mesh;
	mutable ftcore::UiStroke Line; // the stroke being drawn (Core fills it, the mesh takes it)
	bool bLive = false;
	float Time = 0.f;
	float BuiltHeight = -1.f;
	FSlateFontInfo LabelFont;
	// The party rows' names, the Load's gold over the lead's, and the Release beside it.
	FSlateFontInfo LeadNameFont;
	float LeadNameAsc = 0.f;
	FSlateFontInfo GoldFont;
	FSlateFontInfo PartnerNameFont;
	float PartnerNameAsc = 0.f;
	FString LeadNameSeen, LeadName, PartnerNameSeen, PartnerName;
	float LeadNameW = 0.f, PartnerNameW = 0.f;
	FString LoadWord, ReleaseWord, ReleaseKey, GuardWord, PoiseWord, WeightWord, DebtWord, DebtText;
	float ReleaseWordW = 0.f, ReleaseKeyW = 0.f, GuardWordW = 0.f, WeightW = 0.f;
	int32 DebtShown = -2;
	// The top line's name, and the call word under it (a called zone, the answer the boss's move needs, or STAGGERED).
	FSlateFontInfo NameFont;
	float NameAsc = 0.f;
	FString NameText;
	float NameW = 0.f;
	int32 NameSlot = -2; // the slot the name was built for (-1: none, -2: not built)
	bool bNamed = false;
	FSlateFontInfo CallFont;
	float CallAsc = 0.f;
	FSlateFontInfo SmallCallFont; // the call word over a minor enemy or at the screen's edge
	float SmallCallAsc = 0.f;
	FString ZoneWord[3], AnswerWord[3], StaggerWord;
	// Each word's ink middle from where it is drawn (InkMid), so a centred word is centred by its ink; AnswerSW places the
	// word at the screen's edge.
	float ZoneMid[3] = {}, AnswerMid[3] = {}, AnswerSMid[3] = {}, AnswerSW[3] = {}, StaggerMid = 0.f, StaggerSMid = 0.f;
	int32 CallKind = -1; // -1 none, 0..2 a called zone, 3 + the answer the slot foe's windup needs
	int32 SlotFoe = -1;  // the foe whose word stands in the call slot: the boss, else the Read cards' foe, else the target
	// A minor enemy's name over its body.
	FSlateFontInfo FoeNameFont;
	float FoeNameAsc = 0.f;
	const char* FoeKind[UFTArenaRunner::MaxEnemies] = {};
	FString FoeName[UFTArenaRunner::MaxEnemies];
	float FoeNameW[UFTArenaRunner::MaxEnemies] = {};
	bool FoeNamed[UFTArenaRunner::MaxEnemies] = {};
	bool bFoeNames = false; // built for the fonts in use
	// The Read row.
	FSlateFontInfo ReadFont, PrefixFont; // the moves; NEXT MOVE and the streak
	float ReadAsc = 0.f, PrefixAsc = 0.f;
	FString ReadWord, StackText;
	float ReadWordW = 0.f, StackW = 0.f;
	int32 StackShown = -1;
	FString ReadNum[3];
	float ReadNumW[3] = {};
	FString ReadMoveWord[UE_ARRAY_COUNT(kReadTells)];
	float ReadMoveW[UE_ARRAY_COUNT(kReadTells)] = {};
	int32 ReadN = 0;       // the Read pick's cards on show (0: none dealt on a windup)
	int32 ReadPicked = -1; // the card picked, -1 while the pick is open
	int32 ReadCard[3] = {-1, -1, -1}; // each card's move name (kReadMoveUi), -1 for none
	// The prompts.
	FSlateFontInfo PromptFont, KeyFont;
	float PromptAsc = 0.f, KeyAsc = 0.f;
	FString PromptWord[ftcore::kUiPromptCount], PromptKey[ftcore::kUiPromptCount], HealText;
	float PromptWordW[ftcore::kUiPromptCount] = {}, PromptKeyW[ftcore::kUiPromptCount] = {}, HealW = 0.f;
	int32 HealShown = -1;
	FString GiftHealText; // GIFT HEAL n, built when the count changes (empty while none is reported)
	float GiftHealW = 0.f;
	int32 GiftHealShown = -2;
	ftcore::UiPromptMask PromptKeyed = 0; // the prompts something reaches on the device in use (a key, a route, NO KEY)
	ftcore::UiPromptMask PromptNoKey = 0; // the prompts whose action the import left unbound on the device: NO KEY
	ftcore::UiPromptMask PromptDimmed = 0; // the prompts drawn dimmed this tick (Core's uiPromptsDimmed)
	std::array<ftcore::UiPromptMask, ftcore::kUiPromptCount> PromptSameKeyBelow{}; // per prompt, the lower ones on its key
	ftcore::UiPromptReasons Why{};    // each prompt's answer from the rules this tick
	ftcore::PressReason GiftWhy[2] = {ftcore::PressReason::Absent, ftcore::PressReason::Absent}; // Gift Heal's, Gift Guard's
	FString NoKeyWord;                // NO KEY, in the key box
	FString WhyWord[ftcore::kPressReasonCount]; // a refused prompt's reason word, by PressReason (empty: none)
	float WhyW[ftcore::kPressReasonCount] = {};
	// The HUD's edges: the platform's title-safe padding as Slate's ratio of half the viewport per side, the viewport size
	// it was read at, and the HUD safe margin setting in percent.
	FMargin SafeRatio = FMargin(0.f);
	FVector2f SafeSize = FVector2f(-1.f, -1.f);
	int32 HudSafePct = 0;
	// A Qte in the call slot: its words (the four Tap words, HOLD, MASH), whether the budget drew it, and its line left.
	FString QteWord[kQteWords];
	float QteWordW[kQteWords] = {}, QteMid[kQteWords] = {};
	bool bQteOn = false;
	float QteLeft = 0.f;
	// The hallucination's read-outs: TOO SOON's word and second (and whether the budget drew it), the runner's too-early
	// flag as of the last tick, and the Escrow wait's WAIT, its line's state and what is left of it.
	FString TooSoonWord, WaitWord;
	float TooSoonMid = 0.f, WaitWordW = 0.f;
	float TooSoonLeft = 0.f;
	bool bTooEarlyWas = false;
	bool bTooSoonOn = false;
	bool bWaiting = false;
	ftcore::UiWaitLine WaitLine;
	float Wait01 = 0.f;
	// The rows' trade of places on a lead switch.
	ftcore::UiRowSwap RowSwap;
	// The first-use labels and SHARED.
	FSlateFontInfo NoteFont;
	float NoteAsc = 0.f;
	FString NoteWord[UE_ARRAY_COUNT(kFirstUseUi)], SharedWord;
	ftcore::UiFirstUse FirstUse;
	bool bTeaching = false;   // a fight's teaching begun and not yet ended
	bool bTaughtOnce = false; // this run has begun one
	bool bSaveTaught = false; // what this fight teaches is written to the save
	// The Lone Carry: Core's timing, the terms the card shows, the runner's bargain edge and the lead's action as of the
	// last tick, and the card's words (built with the fonts) and lines (built from the terms).
	ftcore::UiLone Lone;
	ftcore::LoneBargain LoneShown;
	bool bLoneDueWas = false;
	ftcore::ArenaAction LoneActWas = ftcore::ArenaAction::Idle;
	bool bLoneShotTerms = false; // a -UiStateShot card's terms taken
	FString LoneAloneWord, LoneEndedWord[2]; // ALONE; ENDED: GIFT, ENDED: PARTNER
	FString LoneTitle, LoneGainHead, LoneGiveHead;
	FString LoneLineWord[ftcore::kUiLoneLineKinds];
	FString LoneGainText[ftcore::kUiLoneGainMax], LoneGiveText[ftcore::kUiLoneGiveMax];
	int32 LoneGainN = 0, LoneGiveN = 0;
	float LoneGainColW = 0.f; // the left column: its head's width or its widest line's
	bool bLoneCardBuilt = false;

	FFoeMark Foes[UFTArenaRunner::MaxEnemies];
	bool bLockOn = false;
	FVector2f LockAt = FVector2f::ZeroVector;
	bool bWeakOn = false;
	FVector2f WeakAt = FVector2f::ZeroVector;
	float BreakK = 0.f; // the boss line's break (0 whole, 1 broken open)
	// The element budget and the event marks.
	ftcore::UiBudgetState Budget;
	ftcore::UiLiveMarks Marks;
	int32 StripSlot = -1; // -FTNewUiStrip: the mark held for its strip, its step, and the kinds done
	ftcore::UiEventMark StripKind = ftcore::UiEventMark::None;
	int32 StripStep = 0;
	int32 StripWait = 0;
	float StripAge = 0.f;
	uint32 StripDone = 1u; // the None kind has no strip
	int32 MarkPushed = 0;  // runner events already turned into marks
	bool bMarkNew = false;
	uint16 MarkMask = 0; // live marks the budget does not grant this frame
	float RestA = 1.f;   // the in-action marks: 1 in action, 0 at rest
	uint8 MarksLogged = 0;
	bool bRestLogged = false;
	bool bBossSeen = false;
	ftcore::ArenaHudBoss BossSeen;
	uint8 LockSeen = ftcore::kArenaNobody;
	float HitA = 0.f, ParryA = 0.f;
	FSlateFontInfo ResultFont;
	float ResultLineH = 0.f;
	FString VictoryWord, DefeatWord, AgainWord;
	float VictoryW = 0.f, DefeatW = 0.f, AgainW = 0.f;
	bool bResult = false;
	bool bWon = false;
	bool bAgain = false;
	float ResultA = 0.f;
	int32 TagMode = 0; // 1 autoplay, 2 idle
	FString TagWord[2];
};


// ---- dialogue ---------------------------------------------------------------------------------------------------------

bool IsLowSurrogateUnit(TCHAR C) { return C >= 0xDC00 && C <= 0xDFFF; }

#if !UE_BUILD_SHIPPING
// -UiChoiceShot placeholders (non-shipping stills only; the script has no choice beats yet). The last cannot be taken.
const TCHAR* const kFakeOptions[3] = {TEXT("1   First option"), TEXT("2   Second option"), TEXT("3   A locked option")};
#endif

class SFTDialogue : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFTDialogue) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&, TFunction<const AFTStoryDirector*()> InGetDirector)
	{
		GetDirector = MoveTemp(InGetDirector);
		SetVisibility(EVisibility::HitTestInvisible);
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1920.0, 1080.0); }

	virtual void Tick(const FGeometry& Geometry, const double, const float DeltaTime) override
	{
		Time += DeltaTime;
		const AFTStoryDirector* D = GetDirector ? GetDirector() : nullptr;
		const bool bWasShown = Shown.IsValid();
		Shown = nullptr;
		if (!D)
		{
			HideTick(DeltaTime);
			return;
		}
		// The key hint: shown when the dialogue opens and on any input, gone after the style's quiet time.
		const double LastInput = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetLastUserInteractionTime() : 0.0;
		HintQuiet = (!bWasShown || LastInput != HintInputSeen) ? 0.f : HintQuiet + DeltaTime;
		HintInputSeen = LastInput;
		HintA = FMath::Clamp(1.f - (HintQuiet - Table().Style.budget.hint) / 0.6f, 0.f, 1.f);
		if ((HintA > 0.f) != bHintUp)
		{
			bHintUp = HintA > 0.f;
			UE_LOG(LogForgottenThird, Display, TEXT("UI: key hint %s after %.1f s quiet."), bHintUp ? TEXT("up") : TEXT("gone"), HintQuiet);
		}
		ftcore::DialogueViewFlags Flags;
		Flags.autoOn = D->IsAutoAdvanceOn();
		Flags.skipOn = D->IsSkipping();
		const ftcore::DialogueBacklog* Log = D->GetBacklog();
		Flags.logAvailable = Log && Log->size() > 0;
		ftcore::updateDialogueView(View, D->GetPresenter(), Flags);
		if (View.kind == ftcore::DialogueViewKind::Hidden || D->GetText().IsEmpty())
		{
			HideTick(DeltaTime);
			return;
		}
		HiddenFor = 0.f;
		Shown = D;
		const FVector2f Size = Geometry.GetLocalSize();
		const float Scale = D->GetSubtitleScale();
		if (WrappedSerial != D->GetBeatSerial() || WrappedBeat != View.beatRevision || WrappedSize != Size || WrappedScale != Scale)
		{
			Rewrap(*D, Size, Scale);
		}
		// Line arrival: each codepoint's time out, so the words come up by alpha behind the reveal.
		if (ArrivalSerial != D->GetBeatSerial())
		{
			ArrivalSerial = D->GetBeatSerial();
			ArriveAt.Init(TNumericLimits<float>::Max(), CpToUnit.Num()); // once per beat
			SeenRevealed = 0;
			bReadWanted = true; // each beat's picture can raise the conversation's gradient
			ReadTries = 0;
		}
		const int32 Revealed = FMath::Min(static_cast<int32>(View.revealedChars), ArriveAt.Num());
		for (int32 I = SeenRevealed; I < Revealed; ++I)
		{
			ArriveAt[I] = Time;
		}
		SeenRevealed = FMath::Max(SeenRevealed, Revealed);

		// The options: the beat's own, or (non-shipping -UiChoiceShot, for stills while the script has no choices)
		// three placeholders under any finished line.
		OptionRows = 0;
		bFakeOptions = false;
		if (View.kind == ftcore::DialogueViewKind::Choice && D->GetPresenter().optionsVisible())
		{
			OptionRows = FMath::Min(static_cast<int32>(View.optionCount), D->GetOptions().Num());
		}
#if !UE_BUILD_SHIPPING
		static const bool bChoiceShot = FParse::Param(FCommandLine::Get(), TEXT("UiChoiceShot"));
		if (bChoiceShot && View.kind == ftcore::DialogueViewKind::Line && View.complete)
		{
			OptionRows = UE_ARRAY_COUNT(kFakeOptions);
			bFakeOptions = true;
		}
#endif
		OptionRows = FMath::Min(OptionRows, static_cast<int32>(ftcore::kDialogueViewOptions));
		if (OptionRows > 0 && OptionsFor != (ArrivalSerial ^ (bFakeOptions ? 0x80000000u : 0u)))
		{
			OptionsFor = ArrivalSerial ^ (bFakeOptions ? 0x80000000u : 0u);
			OptWait = 0.f;
			for (int32 I = 0; I < OptionRows; ++I)
			{
				OptionTextW[I] = TextWidth(OptionText(*D, I), OptionFont);
				// A locked reply says why after its padlock when the data has a reason (an interface text key), else the
				// padlock stands alone. Built here, once per set of options.
				const std::string_view Reason = bFakeOptions || I >= static_cast<int32>(View.optionCount)
					? std::string_view() : View.options[static_cast<size_t>(I)].lockReason;
				LockText[I] = Reason.empty() ? FString() : FTUiText::Lookup(*FT::ToFString(Reason));
			}
		}
		// The conversation's gradient: its level read from the picture under the lower screen at each new beat (at the end
		// of the game viewport's draw, the only time its picture can be read; the dialogue is not in it yet). Within a
		// conversation it only rises, easing there; new options wait until it has.
		OptWait = OptionRows > 0 ? OptWait + DeltaTime : 0.f;
		// The gradient's even part reaches up behind the replies, a line and a half over the first (the one gradient, never
		// a second panel); within a conversation it only rises, easing there.
		{
			const float S = FMath::Max(0.25f, Size.Y / 1080.f);
			const float Floor = Size.Y * kConvEven;
			EvenWant = Floor;
			if (OptionRows > 0)
			{
				const float SubTop = Size.Y - 96.f * S - LineH * Lines.Num(); // the subtitle's block, as painted
				EvenWant = FMath::Min(Floor, SubTop - LineH * 0.4f - LineH * 1.05f * OptionRows - 1.5f * LineH);
			}
			EvenTop = EvenTop < 0.f ? Floor : FMath::Min(EvenTop, Floor);
			if (EvenWant < EvenTop)
			{
				EvenTop = EvenTop - FMath::Max(EvenTop - EvenWant, 1.f) * FMath::Min(1.f, DeltaTime * 10.f);
				EvenTop = FMath::Max(EvenTop, EvenWant);
			}
		}
		DimSize = Size;
		if (!bDimBound && GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->OnEndDraw().AddSP(this, &SFTDialogue::MeasureAtEndDraw);
			bDimBound = true;
		}
		const float Target = ConvTarget();
		ConvDim = FMath::Min(Target, ConvDim + FMath::Max(Target - ConvDim, 0.02f) * FMath::Min(1.f, DeltaTime * 10.f));
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer,
		const FWidgetStyle&, bool) const override
	{
		// The director is re-checked here (weak pointer): it may be gone between Tick and Paint. View's string_views are
		// not read in Paint; the text comes from the director's own FString.
		const AFTStoryDirector* D = Shown.Get();
		if (!D || Lines.Num() == 0)
		{
			// Between lines the conversation's gradient holds; once the conversation is over it fades out.
			const float Hold = FMath::Clamp((1.f - HiddenFor) / 0.4f, 0.f, 1.f);
			if (ConvDim > 0.f && Hold > 0.f)
			{
				const FVector2f Size = Geometry.GetLocalSize();
				Mesh.Begin(Geometry, FMath::Max(0.25f, Size.Y / 1080.f));
				PaintLowGradient(Mesh, Size, ConvDim * Hold, EvenTop);
				Mesh.Flush(Out, Layer + 1);
			}
			return Layer;
		}
		const FVector2f Size = Geometry.GetLocalSize();
		const FPainter P{Geometry, Out, Layer, FMath::Max(0.25f, Size.Y / 1080.f)};
		const float S = P.S;
		const FString& Text = D->GetText();
		// The subtitle's block, where it always stands; the replies stand above it.
		const float Bottom = Size.Y - 96.f * S;
		const float Top = Bottom - LineH * Lines.Num();
		const float CX = Size.X * 0.5f;
		Mesh.Begin(Geometry, S);
		PaintLowGradient(Mesh, Size, ConvDim, EvenTop);
		// The keys, once, under the block at the prompt size (never under the 18 px floor); gone after a few quiet seconds,
		// back on any input.
		if (HintA > 0.f)
		{
			P.Text(CX - HintW * 0.5f, Size.Y - 50.f * S, HintText, HintFont, Tok(UiToken::InkDim, HintA));
		}

		const FLinearColor Body = View.kind == ftcore::DialogueViewKind::Direction ? Tok(UiToken::InkDim) : Tok(UiToken::Ink);
		const int32 RevealedUnits = CpToUnit.Num() > 0 ? CpToUnit[FMath::Min(static_cast<int32>(View.revealedChars), CpToUnit.Num() - 1)] : 0;
		float LastRight = CX;
		for (int32 I = 0; I < Lines.Num(); ++I)
		{
			const float Lead = I == 0 ? SpeakerW : 0.f;
			float X = CX - (LineW[I] + Lead) * 0.5f;
			const float Y = Top + LineH * I;
			if (I == 0 && !SpeakerUpper.IsEmpty())
			{
				P.Text(X, Y + SpeakerDy, SpeakerUpper, SpeakerFont, Tok(UiToken::Accent));
			}
			X += Lead;
			const int32 End = FMath::Min(Lines[I].Y, RevealedUnits);
			PaintArriving(P, X, Y, Text, Lines[I].X, End, Body);
			LastRight = X + LineW[I];
		}
		if (OptionRows > 0 && (bReadWanted || ConvDim < ConvTarget() - 0.005f || EvenTop > EvenWant + 1.f) && OptWait < 0.5f)
		{
			// New options wait until the gradient is read and in place (its level and its reach behind them), so they
			// never show over a lighter one or bare scene; half a second at most.
		}
		else if (OptionRows > 0)
		{
			// The numbered replies over the subtitle, at its left edge, in its face. The selected reply is full ink with a
			// short underline in the boss line's stroke: the one under the host's cursor, and until a cursor is sent
			// (picking is by number today) the first reply that can be taken. Every other reply that can be taken is ink at
			// 70 percent; one that cannot is at half ink with a small drawn padlock after it, then its reason when the data
			// gives one.
			const float RowH = LineH * 1.05f;
			const float OptTop = Top - LineH * 0.4f - RowH * OptionRows;
			float X0 = CX;
			for (int32 I = 0; I < Lines.Num(); ++I)
			{
				X0 = FMath::Min(X0, CX - (LineW[I] + (I == 0 ? SpeakerW : 0.f)) * 0.5f);
			}
			std::uint32_t Takeable = 0;
			for (int32 I = 0; I < OptionRows; ++I)
			{
				Takeable |= OptionPick(*D, I) ? (1u << I) : 0u;
			}
			const int32 Selected = ftcore::uiSelectedReply(bFakeOptions ? -1 : View.flags.cursor, Takeable, OptionRows);
			for (int32 I = 0; I < OptionRows; ++I)
			{
				const float Y = OptTop + RowH * I;
				const bool bPick = ((Takeable >> I) & 1u) != 0u;
				const FLinearColor C = Tok(UiToken::Ink, I == Selected ? 1.f : bPick ? 0.7f : 0.5f);
				P.Text(X0, Y, OptionText(*D, I), OptionFont, C);
				if (!bPick)
				{
					const float LockX = X0 + OptionTextW[I] + 16.f * S;
					PaintPadlock(LockX, Y + OptionAsc, C);
					if (!LockText[I].IsEmpty())
					{
						P.Text(LockX + PadlockW() + 12.f * S, Y, LockText[I], OptionFont, C);
					}
				}
				if (I == Selected)
				{
					const float UY = Y + LineH / 1.15f; // the glyph cell's foot, above the next row
					ftcore::uiStrokeLine(Line, X0, UY, X0 + kReplyUnderline * S, UY, StrokeStyle(7.f, 101, S, 0.1f), 0.f, 1.f);
					Mesh.Stroke(Line, C);
				}
			}
		}
		else if (View.continuePrompt)
		{
			const float A = 0.55f + 0.45f * FMath::Sin(Time * 5.f);
			P.Chevron(LastRight + 22.f * S, Top + LineH * (Lines.Num() - 1) + LineH * 0.45f, 18.f * S, Tok(UiToken::Accent, A));
		}
		Mesh.Flush(Out, Layer + 1);
		return Layer + 3;
	}

private:
	// The dialogue hidden: after a second the conversation is over and its gradient starts again from nothing.
	void HideTick(float Dt)
	{
		HiddenFor += Dt;
		if (HiddenFor > 1.f)
		{
			ConvDim = 0.f;
			ConvNeed = 0.f;
			EvenTop = -1.f;
			bReadWanted = false;
		}
	}

	// The gradient's level for the conversation: the soft dialogue backing, or more where a picture asked for it.
	float ConvTarget() const { return FMath::Max(Table().Style.backingSoftDialogue, ConvNeed); }

	// A new beat's picture under the lower screen, read once (never per frame; never on a screenshot's frame).
	void MeasureAtEndDraw()
	{
		if (!bReadWanted || !Shown.IsValid() || FScreenshotRequest::IsScreenshotRequested())
		{
			return;
		}
		const float Need = MeasureNeed(DimSize);
		// Below 0: no picture yet; read again next frame, ten tries at most (a truly black scene needs no more).
		if (Need >= 0.f || ++ReadTries >= 10)
		{
			bReadWanted = false;
			ReadTries = 0;
			ConvNeed = FMath::Max(ConvNeed, Need);
		}
	}

	// The level the option ink needs over the brightest tenth of the gradient's even part (Core uiOptionDim); -1 when
	// nothing could be read.
	float MeasureNeed(const FVector2f& Size) const
	{
		FViewport* VP = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
		const FIntPoint VS = VP ? VP->GetSizeXY() : FIntPoint::ZeroValue;
		if (VS.X <= 0 || VS.Y <= 0 || Size.X <= 0.f || Size.Y <= 0.f)
		{
			return -1.f;
		}
		const float S = FMath::Max(0.25f, Size.Y / 1080.f);
		const float KY = VS.Y / Size.Y;
		const FIntRect Rc(FMath::FloorToInt(VS.X * 0.1f), FMath::Clamp(FMath::FloorToInt(VS.Y * kConvEven), 0, VS.Y - 1),
			FMath::CeilToInt(VS.X * 0.9f), FMath::Clamp(FMath::CeilToInt((Size.Y - 40.f * S) * KY), 1, VS.Y));
		TArray<FColor> Px;
		if (Rc.Width() <= 0 || Rc.Height() <= 0 || !VP->ReadPixels(Px, FReadSurfaceDataFlags(), Rc) || Px.Num() == 0)
		{
			return -1.f;
		}
		ftcore::UiLumaHistogram Hist;
		for (int32 I = 0; I < Px.Num(); I += 2)
		{
			ftcore::uiLumaAdd(Hist, ftcore::UiColor{Px[I].R / 255.f, Px[I].G / 255.f, Px[I].B / 255.f, 1.f});
		}
		const ftcore::UiColor Pic = ftcore::uiLumaBrightest(Hist, 0.1f);
		if (Pic.r + Pic.g + Pic.b <= 0.f)
		{
			return -1.f; // an all-black read is no picture yet: read again next frame
		}
		const ftcore::UiStyle& St = Table().Style;
		const ftcore::UiColor Ink = ftcore::uiColor(St, ftcore::UiWorld::Solhaven, ftcore::UiPalette::Default, UiToken::Ink);
		const ftcore::UiColor Back = ftcore::uiColor(St, ftcore::UiWorld::Solhaven, ftcore::UiPalette::Default, UiToken::Backing);
		const float Need = ftcore::uiOptionDim(St, Ink, Back, Pic);
		UE_LOG(LogForgottenThird, Display, TEXT("UI: conversation gradient %.2f (this beat needs %.2f over the lower screen's brightest tenth (%.2f, %.2f, %.2f), %dx%d px read)."),
			FMath::Max(ConvTarget(), Need), Need, Pic.r, Pic.g, Pic.b, Rc.Width(), Rc.Height());
		return Need;
	}

	const FString& OptionText(const AFTStoryDirector& D, int32 I) const
	{
#if !UE_BUILD_SHIPPING
		if (bFakeOptions)
		{
			static const FString Fake[UE_ARRAY_COUNT(kFakeOptions)] = {kFakeOptions[0], kFakeOptions[1], kFakeOptions[2]};
			return Fake[I];
		}
#endif
		return D.GetOptions()[I];
	}

	bool OptionPick(const AFTStoryDirector& D, int32 I) const
	{
		return bFakeOptions ? I < OptionRows - 1 : D.IsOptionSelectable(I);
	}

	// A locked reply's padlock, standing on the reply's baseline from LeftX and as tall as its capitals: two ink strokes,
	// the shackle (an arch on two legs) and the body (a rounded box under it), no asset. PadlockW wide.
	float PadlockW() const { return 0.78f * 0.62f * OptionAsc; }
	void PaintPadlock(float LeftX, float Base, const FLinearColor& C) const
	{
		const float CapH = 0.62f * OptionAsc;
		const float BodyW = PadlockW();
		const float BodyH = 0.56f * CapH;
		const float Half = FMath::Max(0.75f, 0.075f * CapH); // the strokes' half width, about a stem's
		const float MidX = LeftX + 0.5f * BodyW;
		const float BodyTop = Base - BodyH;
		const float R = 0.27f * CapH;          // the arch's radius, inside the body's sides
		const float ArchY = Base - CapH + R;   // the arch's centre: its top on the capitals' top
		constexpr int32 ArchSteps = 10;
		FVector2f Pts[ArchSteps + 3];          // a leg's foot, the arch's points, the other foot
		int32 N = 0;
		Pts[N++] = FVector2f(MidX - R, BodyTop);
		for (int32 K = 0; K <= ArchSteps; ++K)
		{
			const float Ang = UE_PI * (1.f + static_cast<float>(K) / ArchSteps); // from the left round the top to the right
			Pts[N++] = FVector2f(MidX + R * FMath::Cos(Ang), ArchY + R * FMath::Sin(Ang));
		}
		Pts[N++] = FVector2f(MidX + R, BodyTop);
		PolyStroke(Line, Pts, N, Half);
		Mesh.Stroke(Line, C);
		KeyCapStroke(Line, MidX, BodyTop + 0.5f * BodyH, BodyW, BodyH, Half, 0.12f * CapH);
		Mesh.Stroke(Line, C, true);
	}

	// One line's revealed text [From, To), in runs of equal arrival alpha (a few steps): the words just out come up by
	// alpha behind the reveal; once every codepoint is out a while, one run, as before.
	void PaintArriving(const FPainter& P, float X, float Y, const FString& T, int32 From, int32 To, const FLinearColor& Body) const
	{
		if (To <= From)
		{
			return;
		}
		constexpr float Fade = 0.4f; // s from out to full
		constexpr int32 Steps = 4;
		const TSharedRef<FSlateFontMeasure> M = FontMeasure();
		const auto Draw = [&](int32 U0, int32 U1, int32 Level)
		{
			const float Off = U0 > From ? static_cast<float>(M->Measure(FStringView(T), From, U0, TextFont).X) : 0.f;
			FLinearColor C = Body;
			C.A *= static_cast<float>(Level) / Steps;
			P.Text(X + Off, Y, T, TextFont, C, U0, U1);
		};
		int32 Cp = Algo::LowerBound(CpToUnit, From);
		int32 RunStart = From;
		int32 RunLevel = -1;
		for (; Cp < CpToUnit.Num() && CpToUnit[Cp] < To; ++Cp)
		{
			const float Age = Cp < ArriveAt.Num() ? Time - ArriveAt[Cp] : Fade;
			const int32 Level = FMath::Clamp(FMath::CeilToInt(FMath::Clamp(Age / Fade, 0.f, 1.f) * Steps), 1, Steps);
			if (Level != RunLevel)
			{
				if (RunLevel >= 0 && CpToUnit[Cp] > RunStart)
				{
					Draw(RunStart, CpToUnit[Cp], RunLevel);
				}
				RunStart = CpToUnit[Cp];
				RunLevel = Level;
			}
		}
		if (RunLevel >= 0)
		{
			Draw(RunStart, To, RunLevel);
		}
	}

	// Once per beat (or on a size change): fonts, the speaker's capitals, the codepoint-to-unit map and the line breaks.
	void Rewrap(const AFTStoryDirector& D, const FVector2f& Size, float Scale)
	{
		OptionsFor = MAX_uint32; // the options are measured again in the new font
		WrappedSerial = D.GetBeatSerial();
		WrappedBeat = View.beatRevision;
		WrappedSize = Size;
		WrappedScale = Scale;
		const float S = FMath::Max(0.25f, Size.Y / 1080.f);
		TextFont = FontFor(UiTextRole::Subtitle, Size.Y, Scale);
		SpeakerFont = FontFor(UiTextRole::Speaker, Size.Y, Scale);
		OptionFont = FontFor(UiTextRole::Subtitle, Size.Y, Scale); // the subtitle's face
		HintFont = FontFor(UiTextRole::Prompt, Size.Y, Scale);
		HintText = kStoryHintUi;
		HintW = TextWidth(HintText, HintFont);
		const TSharedRef<FSlateFontMeasure> M = FontMeasure();
		LineH = M->GetMaxCharacterHeight(TextFont) * 1.15f;
		OptionAsc = M->GetMaxCharacterHeight(OptionFont) + M->GetBaseline(OptionFont); // the baseline under a row's top
		const bool bSpeakerUpper = Table().Style.text[static_cast<size_t>(UiTextRole::Speaker)].upper;
		SpeakerUpper = View.kind != ftcore::DialogueViewKind::Line ? FString() : bSpeakerUpper ? D.GetSpeaker().ToUpper() : D.GetSpeaker();
		SpeakerW = SpeakerUpper.IsEmpty() ? 0.f : TextWidth(SpeakerUpper, SpeakerFont) + 18.f * S;
		SpeakerDy = (M->GetMaxCharacterHeight(TextFont) - M->GetMaxCharacterHeight(SpeakerFont)) * 0.62f;
		MaxW = FMath::Min(Size.X - 160.f * S, 1500.f * S);

		const FString& T = D.GetText();
		CpToUnit.Reset();
		for (int32 I = 0; I < T.Len(); ++I)
		{
			if (!IsLowSurrogateUnit(T[I]))
			{
				CpToUnit.Add(I);
			}
		}
		CpToUnit.Add(T.Len());

		// Greedy by spaces; a word wider than a line stands alone.
		Lines.Reset();
		LineW.Reset();
		const auto Width = [&](int32 From, int32 To) // [From, To)
		{ return To > From ? static_cast<float>(M->Measure(FStringView(T), From, To - 1, TextFont).X) : 0.f; };
		int32 Start = 0;
		int32 Break = -1;
		float Avail = MaxW - SpeakerW;
		for (int32 I = 0; I <= T.Len(); ++I)
		{
			const bool bEnd = I == T.Len();
			if (!bEnd && T[I] != TEXT(' ') && T[I] != TEXT('\n'))
			{
				continue;
			}
			if (Width(Start, I) > Avail && Break > Start)
			{
				Lines.Add(FIntPoint(Start, Break));
				Start = Break + 1;
				Avail = MaxW;
				I = Break; // re-run from the new line's start
				Break = -1;
				continue;
			}
			if (bEnd)
			{
				Lines.Add(FIntPoint(Start, T.Len()));
				break;
			}
			if (T[I] == TEXT('\n'))
			{
				Lines.Add(FIntPoint(Start, I));
				Start = I + 1;
				Avail = MaxW;
				Break = -1;
				continue;
			}
			Break = I;
		}
		for (const FIntPoint& L : Lines)
		{
			LineW.Add(Width(L.X, L.Y));
		}
	}

	TFunction<const AFTStoryDirector*()> GetDirector;
	TWeakObjectPtr<const AFTStoryDirector> Shown; // this frame's director when there is text to show
	ftcore::DialogueView View;
	float Time = 0.f;
	uint32 WrappedSerial = MAX_uint32;
	uint32 WrappedBeat = MAX_uint32;
	FVector2f WrappedSize = FVector2f::ZeroVector;
	float WrappedScale = -1.f;
	FSlateFontInfo TextFont;
	FSlateFontInfo SpeakerFont;
	FSlateFontInfo OptionFont;
	FSlateFontInfo HintFont;
	FString HintText;
	float HintW = 0.f;
	float HintQuiet = 0.f;     // s since the dialogue opened or the last input
	double HintInputSeen = 0.0;
	float HintA = 1.f;
	FString SpeakerUpper;
	float SpeakerW = 0.f;
	float SpeakerDy = 0.f;
	float LineH = 0.f;
	float OptionAsc = 0.f; // the reply face's ascent: a row's top to its baseline
	float MaxW = 0.f;
	TArray<int32> CpToUnit;
	TArray<FIntPoint> Lines; // [start, end) in UTF-16 units
	TArray<float> LineW;
	// Line arrival.
	uint32 ArrivalSerial = MAX_uint32;
	TArray<float> ArriveAt; // by codepoint: the widget time it came out (sized once per beat)
	int32 SeenRevealed = 0;
	// Options.
	int32 OptionRows = 0;
	bool bFakeOptions = false;
	uint32 OptionsFor = MAX_uint32;
	float OptionTextW[ftcore::kDialogueViewOptions] = {}; // each reply's width, for a lock's reason after it
	FString LockText[ftcore::kDialogueViewOptions]; // each locked reply's reason (empty: none given)
	static constexpr float kReplyUnderline = 48.f;  // the selected reply's underline, px at 1080 ("short")
	// The conversation's gradient.
	float ConvDim = 0.f;      // its level now (eases up, never down, within a conversation)
	float ConvNeed = 0.f;     // the highest level the conversation's pictures have asked for
	float EvenTop = -1.f;     // px: the top of its even part now (-1: the usual place; only rises within a conversation)
	float EvenWant = -1.f;    // px: where that top is going (over the replies while they show)
	bool bReadWanted = false; // a new beat's picture is still to be read
	int32 ReadTries = 0;      // reads that found no picture yet for this beat
	float OptWait = 0.f;      // seconds the options on show have waited
	float HiddenFor = 0.f;    // seconds the dialogue has been hidden (the conversation is over after one)
	FVector2f DimSize = FVector2f::ZeroVector; // the widget size the options are placed in, for the end-of-draw read
	bool bDimBound = false;     // the end-of-draw read is bound to the game viewport
	bool bHintUp = false;
	mutable FLineMesh Mesh;
	mutable ftcore::UiStroke Line; // the underline being drawn (Core fills it, the mesh takes it)
};
}

namespace FT
{
bool NewUiEnabled()
{
	static const bool bFlag = FParse::Param(FCommandLine::Get(), TEXT("FTNewUi"));
	return bFlag || CVarNewUi.GetValueOnGameThread() != 0;
}

TSharedRef<SWidget> MakeArenaHudWidget(TFunction<const UFTArenaRunner*()> GetRunner)
{
	return SNew(SFTArenaHud, MoveTemp(GetRunner));
}

TSharedRef<SWidget> MakeDialogueWidget(TFunction<const AFTStoryDirector*()> GetDirector)
{
	return SNew(SFTDialogue, MoveTemp(GetDirector));
}
}
