#include "FTNewUi.h"

#include "FTArenaFighter.h"
#include "FTArenaRunner.h"
#include "FTCutsceneDirector.h"
#include "FTExploreDirector.h"
#include "FTExploreCharacter.h"
#include "FTExploreHUD.h"
#include "FTGameInstance.h"
#include "FTArenaHUD.h"
#include "ForgottenThird.h"
#include "FTInputBindings.h"
#include "FTLookSubsystem.h"
#include "FTPauseSubsystem.h"
#include "FTPlayerController.h"
#include "FTSettingsSubsystem.h"
#include "FTStoryDirector.h"
#include "FTText.h"
#include "FTUiText.h"
#include "FTWorldClockSubsystem.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/UiBudget.h"
#include "ftcore/UiEventMarks.h"
#include "ftcore/UiExplore.h"
#include "ftcore/UiFirstUse.h"
#include "ftcore/UiLone.h"
#include "ftcore/UiPartyScreen.h"
#include "ftcore/UiSpecial.h"
#include "ftcore/Settings.h"
#include "ftcore/InputBindings.h"
#include "ftcore/UiStroke.h"
#include "ftcore/UiStyle.h"
#include "ftcore/UiViews.h"

#include "Algo/BinarySearch.h"
#include "Algo/Sort.h"
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
#include "GenericPlatform/GenericApplication.h"
#include "HDRHelper.h"
#include "HAL/IConsoleManager.h"
#include "Layout/Clipping.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Math/Float16Color.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
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
#if !UE_BUILD_SHIPPING
// Outside Shipping only: a Shipping build always has the new UI (FT::NewUiEnabled) and has no such variable.
TAutoConsoleVariable<int32> CVarNewUi(TEXT("ft.NewUi"), 1,
	TEXT("1 (the default): the new game UI (Slate, drawn from Core view-models) in place of the Canvas HUD's parts it ")
	TEXT("replaces. 0: the old Canvas HUD, as -FTOldUi on the command line also gives (which wins over this). Not in ")
	TEXT("Shipping, which always has the new UI. Read when a map's HUD begins: a change applies from the next map."));
TAutoConsoleVariable<int32> CVarNewUiWorld(TEXT("ft.NewUi.World"), 0,
	TEXT("The world whose colours and mark rules the new UI wears: 0 Solhaven (the default), 1 the Lattice, 2 the ")
	TEXT("Unwritten. For captures and review until the world's own flag reaches the HUD. Read every frame. Not in ")
	TEXT("Shipping, which wears Solhaven's."));
#endif

using ftcore::UiTextRole;
using ftcore::UiToken;

// The words a fight keeps: the call word under the boss name (a called zone, or the answer a boss's move needs).
const FTUiText kZoneUi[3] = {FTUiText(TEXT("arena.boss.left")), FTUiText(TEXT("arena.boss.right")), FTUiText(TEXT("arena.boss.path"))};
const FTUiText kCallDodgeUi(TEXT("arena.call.dodge")); // the call word has no "!" (the old HUD keeps arena.dodge)
const FTUiText kAnswerUi[3] = {FTUiText(TEXT("arena.parry")), FTUiText(TEXT("arena.block")), kCallDodgeUi};
const FTUiText kStaggeredUi(TEXT("battle.staggered")); // the call word's slot, with no line
const FTUiText kNextMoveUi(TEXT("arena.next_move")); // the Read row's prefix
const FTUiText kReleaseUi(TEXT("arena.release"));
const FTUiText kWeightUi(TEXT("arena.boss.weight"));
const FTUiText kDebtUi(TEXT("arena.debt"));
const FTUiText kLeadUi(TEXT("input.lead"));     // a party row's name without a look
const FTUiText kPartnerUi(TEXT("input.count")); // "partner": the second row's fallback name and the paired counter's prompt
const FTUiText kGuardUi(TEXT("input.guard")); // the guard row's word and the guard prompt's
const FTUiText kLoadUi(TEXT("arena.load"));   // the lead's state slot while some Load is held
const FTUiText kPoiseUi(TEXT("arena.poise")); // always beside the poise line, whenever the line is drawn
const FTUiText kWeakPointUi(TEXT("arena.label.weak_point"));
const FTUiText kWeakUi(TEXT("arena.label.weak")); // WEAK by the open weak point, once a fight (r19c base)
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
// The party screen's words (ftcore/UiPartyScreen.h): a refused lead row's word after its name (the Third's row name is
// UFTGameInstance::ThirdName, the story's current one) (DOWN is the prompts' and WAIT the state slot's), the refused switch's call prefix (SWITCH: DOWN), the Third's
// gift words (READY, a cooldown's seconds) and its whole slot refused, and explore's follow row.
const FTUiText kNotHereUi(TEXT("arena.party.not_here"));
const FTUiText kFightOverUi(TEXT("arena.party.fight_over"));
const FTUiText kSwitchUi(TEXT("arena.party.switch"));
const FTUiText kReadyUi(TEXT("arena.party.ready"));
const FTUiText kSecondsUi(TEXT("arena.party.seconds"));
const FTUiText kVacantUi(TEXT("arena.party.vacant"));
const FTUiText kHostileUi(TEXT("arena.party.hostile"));
const FTUiText kFollowUi(TEXT("arena.party.follow"));
const FTUiText kWaitHereUi(TEXT("arena.party.wait_here"));
// The explore party screen's controls line, per device (its keys are fixed, not the binding table's).
const FTUiText kPartyKeysUi(TEXT("party.controls.keys"));
const FTUiText kPartyPadUi(TEXT("party.controls.pad"));
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
const FTUiText kAttackUi(TEXT("arena.prompt.attack"));
const FTUiText kDodgeUi(TEXT("input.dodge"));
const FTUiText kJumpUi(TEXT("input.jump"));
const FTUiText kPromptUi[] = {kHealUi, kGiftHealUi, kGiftGuardUi, kPartnerUi, kAttackUi, kGuardUi, kDodgeUi, kJumpUi};
static_assert(UE_ARRAY_COUNT(kPromptUi) == ftcore::kUiPromptCount, "one word per prompt, in UiPrompt order");
// Each prompt's key: the controls line's entry with this label, or (no label) the entry that carries this action, else
// that action's own keys (inputActionKeys). HEAL is the lead's own heal (input.heal: H, on a pad D-pad up). The gifts are
// left out of the controls line, so they take their own keys: G and Y; on a pad D-pad left and right (a version-1
// bindings file leaves them NO KEY where its D-pad keeps crouch or switch lead). A rebinding that puts two rows on one key
// keeps the lowest (uiPromptsDrawable). An action the bindings import left unbound shows NO KEY in its key box, dimmed
// (inputPromptKey on FTInputBindings::Unbound), and keeps its row. There is no RELOAD prompt: the rules have no reload
// press (the reload is automatic), so the row comes back only with that press.
struct FPromptKeyOf
{
	const char* Label;
	ftcore::InputAction Action;
};
const FPromptKeyOf kPromptKeys[] = {{"input.heal", {}}, {nullptr, ftcore::InputAction::GiftHeal}, {nullptr, ftcore::InputAction::GiftGuard},
	{"input.count", {}}, {"input.light", {}}, {"input.guard", {}}, {"input.dodge", {}}, {"input.jump", {}}};
static_assert(UE_ARRAY_COUNT(kPromptKeys) == ftcore::kUiPromptCount, "one key per prompt, in UiPrompt order");
// A prompt's NO KEY, in its key box; and the word after a prompt whose press would be refused now, by ftcore::PressReason
// (Ok and Busy are taken and have none; Absent draws no prompt).
const FTUiText kNoKeyUi(TEXT("arena.prompt.no_key"));
// The special prompt's word, and the reason it stands dim in place of the lead's name, by ftcore::UiSpecialWhy (None, the
// live row, has none: the lead's name stands there).
const FTUiText kSpecialUi(TEXT("arena.prompt.special"));
const FTUiText kSpecialCallUi(TEXT("arena.prompt.call"));
const FTUiText kSpecialNoTargetUi(TEXT("arena.special.why.no_target"));
const FTUiText kSpecialOutOfSightUi(TEXT("arena.special.why.out_of_sight"));
const FTUiText kSpecialNotNowUi(TEXT("arena.special.why.not_now"));
const FTUiText* const kSpecialWhyUi[] = {nullptr, &kSpecialNoTargetUi, &kSpecialOutOfSightUi, &kSpecialNotNowUi};
static_assert(UE_ARRAY_COUNT(kSpecialWhyUi) == ftcore::kUiSpecialWhyCount, "one reason word per UiSpecialWhy");
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
static_assert(UFTArenaRunner::PromptPressCount == static_cast<int32>(ftcore::kUiPromptCount), "one press count per prompt");
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

// One world's colours, built once: its tokens, its non-text mark colour and how its danger words are drawn.
struct FUiWorldRow
{
	FLinearColor Token[ftcore::kUiTokenCount];
	FLinearColor Mark;       // the weak-point mark: uiMarkColor over the WeakPoint token (Mark gold in every world)
	FLinearColor DangerWord; // a danger word's glyphs (uiDangerWord)
	FLinearColor DangerLine; // its underline, drawn only where bDangerUnderline
	bool bMarksAccent2 = false;    // the marks wear accent2: each carries a 1 px ink rim
	bool bDangerUnderline = false; // a danger word is ink over a danger underline
	// The rest of the danger colour's places (uiDangerSet); no painter path reads the danger token itself.
	FLinearColor HitCue;   // the hit's edge glow and the lost hp while it lives (one red in every world)
	FLinearColor HitDark;  // the darkening layer under the hit's red glow
	FLinearColor TrackBody; // a value line's unfilled body (uiTrackSet): the world's backing at kUiTrackBodyA
	FLinearColor TrackHair; // its 1 device px top hairline (the track token)
	FLinearColor Fill;      // the value: the world's fill token
	// The signature devices (UiWorldRules::sigPulse, sigArcs, sigMarginRule): off until the owner picks; a pick flips one row
	// of style.json. Off, every painter takes the base path untouched.
	bool bSigPulse = false;
	bool bSigArcs = false;
	bool bSigMarginRule = false;
	bool bLoadBreaks = false; // UiWorldRules::loadBreaks: the Load's gold carries breaks (uiLoadBreaks)
	// UiWorldRules::specialPrompt: the special prompt's row is reserved in the prompt stack (uiPromptRowAt) and drawn when
	// the rules say the special is ready. Off, the stack is as it was and nothing is drawn.
	bool bSpecialPrompt = false;
	FLinearColor LowLine;  // a low hp or guard line
	FLinearColor LowUnder; // the 1 px line under it, drawn only where bLowUnder
	bool bLowUnder = false;
	FLinearColor CueMark;  // the call cue's danger triangle (a mark: rimmed where bMarksAccent2)
};

// Data/UI/style.json once (the built-in style when it is missing or refused), and every world's colours.
struct FUiTable
{
	ftcore::UiStyle Style;
	FUiWorldRow Row[ftcore::kUiPaletteCount][ftcore::kUiWorldCount]; // by the player's palette, then the world
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
		// Every palette's and world's row; the shown one is picked per frame (ShownPalette, ShownWorld).
		for (size_t P = 0; P < ftcore::kUiPaletteCount; ++P)
		for (size_t W = 0; W < ftcore::kUiWorldCount; ++W)
		{
			const ftcore::UiPalette Pal = static_cast<ftcore::UiPalette>(P);
			const ftcore::UiWorld World = static_cast<ftcore::UiWorld>(W);
			FUiWorldRow& R = T.Row[P][W];
			for (size_t I = 0; I < ftcore::kUiTokenCount; ++I)
			{
				R.Token[I] = LinOf(ftcore::uiColor(T.Style, World, Pal, static_cast<UiToken>(I)));
			}
			R.Mark = LinOf(ftcore::uiMarkColor(T.Style, World, Pal, ftcore::uiColor(T.Style, World, Pal, UiToken::WeakPoint)));
			const ftcore::UiDangerWord D = ftcore::uiDangerWord(T.Style, World, Pal);
			R.DangerWord = LinOf(D.word);
			R.DangerLine = LinOf(D.underline);
			R.bMarksAccent2 = T.Style.rules[W].marksAccent2;
			R.bDangerUnderline = D.underlined;
			const ftcore::UiDangerSet DS = ftcore::uiDangerSet(T.Style, World, Pal);
			R.HitCue = LinOf(DS.hitCue);
			R.HitDark = LinOf(DS.hitDark);
			const ftcore::UiTrackSet TS = ftcore::uiTrackSet(T.Style, World, Pal);
			R.TrackBody = LinOf(TS.body);
			R.TrackHair = LinOf(TS.hairline);
			R.Fill = LinOf(TS.fill);
			R.LowLine = LinOf(DS.lowLine);
			R.LowUnder = LinOf(DS.lowUnderline);
			R.bLowUnder = DS.lowUnderlined;
			R.CueMark = LinOf(DS.cueMark);
			R.bSigPulse = T.Style.rules[W].sigPulse;
			R.bSigArcs = T.Style.rules[W].sigArcs;
			R.bSigMarginRule = T.Style.rules[W].sigMarginRule;
			R.bLoadBreaks = T.Style.rules[W].loadBreaks;
			R.bSpecialPrompt = T.Style.rules[W].specialPrompt;
		}
		T.Edge = LinOf(T.Style.edge);
		return T;
	}();
	return Loaded;
}

// The world the screens wear this frame: Solhaven, or outside Shipping the one ft.NewUi.World names.
ftcore::UiWorld ShownWorld()
{
#if !UE_BUILD_SHIPPING
	return static_cast<ftcore::UiWorld>(FMath::Clamp(CVarNewUiWorld.GetValueOnAnyThread(), 0, static_cast<int32>(ftcore::kUiWorldCount) - 1));
#else
	return ftcore::UiWorld::Solhaven;
#endif
}

// The player's palette (the Colours row), set by the settings subsystem on every change (FT::SetUiPalette); outside
// Shipping -UiPalette=<0..4> holds one for captures.
std::atomic<uint8> GUiPalette{0};

ftcore::UiPalette ShownPalette()
{
#if !UE_BUILD_SHIPPING
	static const int32 Forced = []
	{
		int32 V = -1;
		FParse::Value(FCommandLine::Get(), TEXT("-UiPalette="), V);
		return V < static_cast<int32>(ftcore::kUiPaletteCount) ? V : -1;
	}();
	if (Forced >= 0)
	{
		return static_cast<ftcore::UiPalette>(Forced);
	}
#endif
	return static_cast<ftcore::UiPalette>(FMath::Min<uint8>(GUiPalette.load(std::memory_order_relaxed), static_cast<uint8>(ftcore::kUiPaletteCount - 1)));
}

const FUiWorldRow& Shown() { return Table().Row[static_cast<size_t>(ShownPalette())][static_cast<size_t>(ShownWorld())]; }

FLinearColor Tok(UiToken T, float Alpha = 1.f)
{
	FLinearColor C = Shown().Token[static_cast<size_t>(T)];
	C.A *= Alpha;
	return C;
}

// A value line's fill in the shown world (ftcore::uiTrackSet), at Alpha.
FLinearColor FillTok(float Alpha = 1.f)
{
	FLinearColor C = Shown().Fill;
	C.A *= Alpha;
	return C;
}

// A non-text mark's colour in the shown world (ink where the marks keep it) and, where they wear accent2, the 1 px ink
// rim that lets the dark red stand on the dark backing; both at Alpha.
struct FMarkTok
{
	FLinearColor C;
	FLinearColor Rim;
	bool bRim = false;
	const FLinearColor* RimOrNull() const { return bRim ? &Rim : nullptr; }
};
FMarkTok MarkTok(float Alpha)
{
	const FUiWorldRow& R = Shown();
	FMarkTok M;
	M.C = R.Mark;
	M.C.A *= Alpha;
	M.Rim = R.Token[static_cast<size_t>(UiToken::Ink)];
	M.Rim.A *= Alpha;
	M.bRim = R.bMarksAccent2;
	return M;
}

// Every mark and glyph is a pale core over a tight soft dark contour that follows its shape (no plate, nothing wider than
// the mark). A dim or resting core lowers its own alpha or weight; the contour holds at full strength down to the rest
// alpha and fades below it only as the mark goes away.
constexpr float kContourGrow = 1.5f;    // px past the core's edge at 1080p where the contour is half gone
constexpr float kContourFeather = 2.f;  // px over which it falls to nothing
constexpr float kContourAlpha = 0.67f;  // of the edge colour's alpha (0.9): about 60 percent
constexpr float kContourSoft = 0.5f;    // a word's outer contour pixel, of its inner one
constexpr float kRimPx = 1.f;           // an accent2 mark's ink rim, in device pixels (one at every size)
constexpr float kCrispFeather = 0.8f;   // device px: the soft edge of a core, a rim and its knock-out

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

	// The pass's glyph run in its font, shaped on its first draw (and again after a draw with a face still loading). It is
	// shaped as MakeText lays the string out, so it lands where the HUD measured it: Slate's MakeText and its font measure
	// (which places every word) never apply the font's LetterSpacing, and its shaper does, so the run is shaped without it
	// (with it, every gap between letters grew by it and a word ran past its measured end, into its key cap).
	static FShapedGlyphSequenceRef Shaped(FTextRun& R, int32 Pass, const FSlateFontInfo& F)
	{
		const uint8 Bit = static_cast<uint8>(1u << Pass);
		if (!R.Pass[Pass].IsValid() || (R.Stale & Bit) != 0)
		{
			FSlateFontInfo AsLaidOut = F;
			AsLaidOut.LetterSpacing = 0;
			TArray<TWeakPtr<FFreeTypeFace>> Loading;
			R.Pass[Pass] = FSlateApplication::Get().GetRenderer()->GetFontCache()->ShapeUnidirectionalText(R.Str, AsLaidOut, R.Scale,
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
	bool bShadow = false; // the fight HUD's words: rim-only (the font's own 1 px dark edge, no soft contour) (Text)
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
	void Soft(float CX, float CY, float W, float H, float Peak, float Alpha = 1.f, const FLinearColor* Tint = nullptr) const
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
		const FLinearColor C = (Tint ? FLinearColor(Tint->R, Tint->G, Tint->B, 0.f) : Tok(UiToken::Backing, 0.f)) + FLinearColor(0.f, 0.f, 0.f, Each);
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
		TextIn(X, Y, Str, Font, C, Start, End, 0.f);
	}

	// A word over a measured contour (Core uiLineGuard): the font's edge G.spread px wide at 1080 (whole device px, never
	// under its resting width) at G.alpha; a guard at or under the resting contour draws as Text.
	void TextGuarded(float X, float Y, const FString& Str, const FSlateFontInfo& Font, const FLinearColor& C, const ftcore::UiLineGuard& G) const
	{
		const int32 Base = Font.OutlineSettings.OutlineSize;
		const int32 Want = FMath::Max(Base, FMath::RoundToInt(G.spread * FMath::Max(1.f, S))); // px at 1080, scaled
		if (Base <= 0 || bShadow || (Want == Base && G.alpha <= Font.OutlineSettings.OutlineColor.A))
		{
			TextIn(X, Y, Str, Font, C, 0, -1, 0.f);
			return;
		}
		FSlateFontInfo F = Font;
		F.OutlineSettings.OutlineSize = Want;
		F.OutlineSettings.OutlineColor.A = FMath::Max(G.alpha, Font.OutlineSettings.OutlineColor.A);
		// Slate sets an outlined glyph down and right by its outline's width: set the wider one back by the growth.
		TextIn(X, Y, Str, F, C, 0, -1, static_cast<float>(Want - Base));
	}

	void TextIn(float X, float Y, const FString& Str, const FSlateFontInfo& Font, const FLinearColor& C, int32 Start, int32 End, float Grow) const
	{
		if (Str.IsEmpty() || C.A <= 0.f)
		{
			return;
		}
		const float Unit = static_cast<float>(FMath::Max(1, FMath::RoundToInt(S))); // the shadow's outline step
		const int32 From = End < 0 ? 0 : FMath::Clamp(Start, 0, Str.Len());
		const int32 To = End < 0 ? Str.Len() : FMath::Clamp(End, From, Str.Len());
		FTextRun* Run = Runs && To > From ? &Runs->Find(*Str + From, To - From, Font, Geometry.Scale, Unit) : nullptr;
		// Each pass by its index in the run (FTextRuns; 0 to 3, the old shadow's, are not drawn): the soft contour 4, the
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
		// The fight HUD's and the party screen's words, the names included, keep only the font's own 1 px dark rim: a soft
		// shadow or contour blurs a small letter into the picture and sits heavy under a name.
		if (Edge > 0 && !bShadow)
		{
			FSlateFontInfo OuterF = InnerF;
			OuterF.OutlineSettings.OutlineSize += 1;
			OuterF.OutlineSettings.OutlineColor.A *= kContourSoft;
			OuterF.OutlineSettings.bSeparateFillAlpha = true;
			// Slate sets an outlined glyph down and right by the outline's growth (measured: 1 px); set the wider pass back so it rings
			// the letter evenly instead of reading as a drop shadow.
			Draw(4, OuterF, FLinearColor(C.R, C.G, C.B, 0.004f), 1.f + Grow); // a near-zero face: Slate culls a fully clear one
		}
		Draw(InnerF.OutlineSettings.bSeparateFillAlpha ? 6 : 5, InnerF, C, Grow);
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
		DevScale = FMath::Max(Geometry.Scale, 0.01f);
		Px = 1.f / DevScale;
		DevOrigin = FVector2f(Transform.GetTranslation());
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

	// One device pixel in local units (1 at 1080p and 100 percent; 0.75 at 1440p).
	float PxSize() const { return Px; }

	// A width given in px at 1080p as a whole number of device pixels, at least one, in local units: a crisp line never
	// lands on 1.25 or 1.33 px and blurs.
	float WholePx(float W1080) const { return WholeDev(W1080 * S); }
	float WholeDev(float WLocal) const { return FMath::Max(1.f, FMath::RoundToFloat(WLocal / Px)) * Px; }

	// A local x or y moved onto the nearest device pixel boundary.
	float SnapX(float X) const { return (FMath::RoundToFloat(X * DevScale + DevOrigin.X) - DevOrigin.X) * Px; }
	float SnapY(float Y) const { return (FMath::RoundToFloat(Y * DevScale + DevOrigin.Y) - DevOrigin.Y) * Px; }

	// A line's centre y for a line WidthLocal wide (a whole number of device pixels) whose top and bottom edges both sit
	// on device pixel boundaries.
	float SnapLineY(float Y, float WidthLocal) const { return SnapY(Y - 0.5f * WidthLocal) + 0.5f * WidthLocal; }

	// The rim's knock-out: the edge colour at the rim's alpha over the core's own footprint, so the rim shows only as a
	// ring and a core below full alpha dims toward dark, never paling over the ink under it.
	static FLinearColor Knock(const FLinearColor& Rim)
	{
		FLinearColor K = Table().Edge;
		K.A = Rim.A;
		return K;
	}

	// A line from Core. Closed: the first and last points are the same place (a ring), joined without a seam. Rim: a mark
	// that wears accent2 (MarkTok) carries a crisp 1 px rim in that colour between its core and its dark contour, which
	// moves out by the rim's width. bCrispEdge (a ring): a crisp 1 px dark edge at the edge colour's full alpha just
	// outside the core and rim, inside the soft contour.
	void Stroke(const ftcore::UiStroke& St, const FLinearColor& C, bool bClosed = false, const FLinearColor* Rim = nullptr,
		bool bCrispEdge = false)
	{
		if (St.count < 2 || C.A <= 0.f)
		{
			return;
		}
		const float RimW = Rim ? kRimPx * Px : 0.f;
		const float EdgeW = bCrispEdge ? Px : 0.f;
		Ribbon(Parts[Under], St, Table().Edge, kContourAlpha * ContourK(C.A), kContourGrow * S + RimW + EdgeW, kContourFeather * S, bClosed);
		if (bCrispEdge)
		{
			Ribbon(Parts[Under], St, Table().Edge, ContourK(C.A), RimW + EdgeW, kCrispFeather * Px, bClosed);
		}
		if (Rim)
		{
			// The rim, the knock-out and the core all fade over the same 0.8 device px, so the knock-out's fade ends inside
			// the rim's solid row and the rim keeps its whole pixel.
			Ribbon(Parts[Under], St, *Rim, 1.f, RimW, kCrispFeather * Px, bClosed);
			Ribbon(Parts[Under], St, Knock(*Rim), 1.f, 0.f, kCrispFeather * Px, bClosed);
		}
		Ribbon(Parts[Core], St, C, 1.f, 0.f, kCrispFeather * Px, bClosed);
	}

	// A value line (health, guard, poise, a timer): its core with a crisp 1 px dark edge along both sides (about 60 percent,
	// its fall under a pixel) in place of the soft contour, so the line's own width reads exactly. EdgeK is the edge's share
	// of the edge colour; an unfilled track takes it whole, so it holds on a white sky.
	void ValueStroke(const ftcore::UiStroke& St, const FLinearColor& C, const FLinearColor* Rim = nullptr, float EdgeK = kContourAlpha)
	{
		if (St.count < 2 || C.A <= 0.f)
		{
			return;
		}
		const float RimW = Rim ? kRimPx * Px : 0.f;
		Ribbon(Parts[Under], St, Table().Edge, EdgeK * ContourK(C.A), Px + RimW, kCrispFeather * Px, false);
		if (Rim)
		{
			Ribbon(Parts[Under], St, *Rim, 1.f, RimW, kCrispFeather * Px, false);
			Ribbon(Parts[Under], St, Knock(*Rim), 1.f, 0.f, kCrispFeather * Px, false);
		}
		Ribbon(Parts[Core], St, C, 1.f, 0.f, kCrispFeather * Px, false);
	}

	// A filled convex shape (a dot, a lozenge, a small triangle), edged like the lines.
	void Fill(const FVector2f* Pts, int32 N, const FLinearColor& C, const FLinearColor* Rim = nullptr)
	{
		if (N < 3 || C.A <= 0.f)
		{
			return;
		}
		const float RimW = Rim ? kRimPx * Px : 0.f;
		Convex(Parts[Under], Pts, N, Table().Edge, kContourAlpha * ContourK(C.A), kContourGrow * S + RimW, kContourFeather * S);
		if (Rim)
		{
			Convex(Parts[Under], Pts, N, *Rim, 1.f, RimW, kCrispFeather * Px);
			Convex(Parts[Under], Pts, N, Knock(*Rim), 1.f, 0.f, kCrispFeather * Px);
		}
		Convex(Parts[Core], Pts, N, C, 1.f, 0.f, kCrispFeather * Px);
	}

	void Dot(float CX, float CY, float R, const FLinearColor& C, const FLinearColor* Rim = nullptr)
	{
		FVector2f Pts[16];
		for (int32 I = 0; I < 16; ++I)
		{
			const float A = I * (2.f * UE_PI / 16.f);
			Pts[I] = FVector2f(CX + R * FMath::Cos(A), CY + R * FMath::Sin(A));
		}
		Fill(Pts, 16, C, Rim);
	}

	// A gradient strip with no edge: alpha Peak along one side (bPeakAtEnd: the bottom or right side), even for the first
	// 1 - Ramp of the way, then falling to nothing at the other side with zero slope at both ends of the fall. bMitre cuts
	// both ends at 45 degrees, so four such strips round a screen meet without overlap: a corner takes the nearer edge's
	// strength (the edges' maximum), never the two stacked.
	void Band(float X0, float Y0, float X1, float Y1, bool bAlongY, bool bPeakAtEnd, float Ramp, float Peak, const FLinearColor& C,
		bool bMitre = false)
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
			const float In = bMitre ? D * (bAlongY ? Y1 - Y0 : X1 - X0) : 0.f; // this row's distance from the peak side
			if (bAlongY)
			{
				const float Y = FMath::Lerp(Y0, Y1, T);
				P.Verts.Add(Vert(X0 + In, Y, C, A));
				P.Verts.Add(Vert(X1 - In, Y, C, A));
			}
			else
			{
				const float X = FMath::Lerp(X0, X1, T);
				P.Verts.Add(Vert(X, Y0 + In, C, A));
				P.Verts.Add(Vert(X, Y1 - In, C, A));
			}
		}
		for (int32 I = 0; I < Steps; ++I)
		{
			const int32 I0 = Base + 2 * I;
			Tri(P, I0, I0 + 1, I0 + 3);
			Tri(P, I0, I0 + 3, I0 + 2);
		}
	}

	// A shaded patch with no edge on the shade part: an N by N grid whose alpha at each point is AlphaAt(U, V), U and V
	// running 0 to 1 from (X0, Y0) to (X1, Y1). The hit glow's corners, where two edges meet at their maximum.
	template <typename FnT>
	void Patch(float X0, float Y0, float X1, float Y1, const FLinearColor& C, FnT&& AlphaAt)
	{
		if (X1 <= X0 || Y1 <= Y0)
		{
			return;
		}
		constexpr int32 N = 16;
		FPart& P = Parts[ShadePart];
		const int32 Base = P.Verts.Num();
		for (int32 J = 0; J <= N; ++J)
		{
			const float V = static_cast<float>(J) / N;
			for (int32 I = 0; I <= N; ++I)
			{
				const float U = static_cast<float>(I) / N;
				P.Verts.Add(Vert(FMath::Lerp(X0, X1, U), FMath::Lerp(Y0, Y1, V), C, AlphaAt(U, V)));
			}
		}
		for (int32 J = 0; J < N; ++J)
		{
			for (int32 I = 0; I < N; ++I)
			{
				const int32 I0 = Base + J * (N + 1) + I;
				Tri(P, I0, I0 + 1, I0 + N + 2);
				Tri(P, I0, I0 + N + 2, I0 + N + 1);
			}
		}
	}

	// A solid rectangle with hard edges (corners in local units, best on device pixel boundaries), on the shade part
	// (under every line) or the core part (over the lines' dark edges).
	void Rect(float X0, float Y0, float X1, float Y1, const FLinearColor& C, bool bOnTop)
	{
		if (X1 <= X0 || Y1 <= Y0 || C.A <= 0.f)
		{
			return;
		}
		FPart& P = Parts[bOnTop ? Core : ShadePart];
		const int32 Base = P.Verts.Num();
		P.Verts.Add(Vert(X0, Y0, C, C.A));
		P.Verts.Add(Vert(X1, Y0, C, C.A));
		P.Verts.Add(Vert(X0, Y1, C, C.A));
		P.Verts.Add(Vert(X1, Y1, C, C.A));
		Tri(P, Base, Base + 1, Base + 3);
		Tri(P, Base, Base + 3, Base + 2);
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
			const float Alpha = C.A * AlphaK * Pt.alpha * FMath::Min(1.f, 2.f * H / Px); // a line under 1 device px thins in alpha
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
	float Px = 1.f;       // one device pixel in local units
	float DevScale = 1.f; // device pixels per local unit
	FVector2f DevOrigin = FVector2f(0.f, 0.f);
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
constexpr float TopLen = 768.f;      // the top line, boss or target: one length (0.40 of the width at 1080p)
constexpr float LeadLine = 260.f;    // the lead's hp and guard lines
constexpr float PartnerLine = 200.f; // the partner's hp line
constexpr float FoeLine = 90.f;      // a minor enemy's hp line over its body
constexpr float CallLine = 160.f;    // the windup line under the call word
constexpr float LowHp = 0.25f;       // under it an hp or guard line turns danger red
// Lines, px at their thickest. A line that carries a value (an amount or the time left) is nowhere thinner than
// MinValueLine; a track or a mark may be. A value line holds its full width its whole drawn length, square ends and a
// hard fill edge (ValueTaper 0).
constexpr float MinValueLine = 3.f;
constexpr float ValueTaper = 0.f; // square ends: no taper
constexpr float LeadHpW = 8.f; // the dominant line: twice the Load and partner lines
constexpr float PartnerHpW = 4.f;
constexpr float GuardW = 3.f;
constexpr float DangerUnderW = 1.f; // a danger word's underline, where the world draws one
// The Load line under the lead's name (the value-line family at LoadW, gold to load / loadCap, a 1 device px tick at the
// Release threshold): its top rim on the name's old baseline, the name and its slot words LoadLift px over it.
constexpr float LoadW = 4.f;
constexpr float LoadLift = 4.f;
constexpr float LoadTickLight = 236.f / 255.f; // the tick before the Release opens
constexpr float LoadTickDark = 12.f / 255.f;   // and once it is open
constexpr float ReleaseTickW = 2.f;  // the Release tick, centred on the Load line
constexpr float ReleaseTickH = 12.f;
// The r19c ledger's right-hand column and figures: a label (LOAD, GUARD) LabelGap px after its line's end; the hp figure
// FigGap px after it, right-aligned to a fixed right edge (Slate applies no tabular figures, so the width never jumps);
// the boss's percent right-aligned to the top line's end, its baseline FigLift px over the line's top, where the boss's
// name stands too.
constexpr float LabelGap = 8.f;
constexpr float FigGap = 16.f;
constexpr float FigLift = 12.f;
constexpr float FigPx = 28.f;      // figures (the boss's percent, hp, the prompts' counts): 18.5 px capitals
constexpr float BossNamePx = 29.f; // the boss's name: 19.1 px capitals
constexpr float CountGap = 16.f;   // a prompt's count after the longest verb
constexpr int32 NameTracking = 180; // the lead's and partner's names (thousandths of the em)
constexpr int32 SpacedTracking = 220; // the boss's name and the labels
constexpr float PoiseW = 4.f;
constexpr float FoeHpW = 3.f; // a minor enemy's line, and the second target line under the boss's
constexpr float TimerW = 3.f; // the windup's and a Qte's shrinking line
static_assert(LeadHpW >= MinValueLine && PartnerHpW >= MinValueLine && GuardW >= MinValueLine && LoadW >= MinValueLine && PoiseW >= MinValueLine
	&& FoeHpW >= MinValueLine && TimerW >= MinValueLine, "no value line under its floor");
static_assert(ValueTaper == 0.f, "a value line at full width its whole length: square ends, a hard fill edge");
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
constexpr float NotePx = 26.f;    // the labels and the state slots (LOAD, RELEASE, GUARD, POISE, WEAK POINT,
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
	&& NotePx >= MinTextPx && FoePx >= MinTextPx && KeyPx >= MinTextPx && FigPx >= MinTextPx && BossNamePx >= MinTextPx,
	"no fight text under the floor");
static_assert(LeadPx * CapK >= 25.f && PartnerPx * CapK >= 19.f && RowPx * CapK >= 19.f && PrefixPx * CapK >= 17.f && NotePx * CapK >= 17.f,
	"capitals at least a pixel over their floors: lead 24, partner and prompts 18, labels 16");
// Ink, 0..1.
constexpr float PartnerInk = 0.85f; // the partner's name
constexpr float NoteInk = 0.8f;     // the labels
constexpr float PrefixInk = 0.8f;   // NEXT MOVE (every label drawn stands at 80 percent ink or more)
constexpr float SlotInk = 0.8f;     // LOAD (its first three times a save), ALONE and ENDED in the lead's state slot
// WAIT's line in the state slot: px after the word and long at the wait's start (a TimerW line, as a Qte's).
constexpr float WaitGap = 10.f;
constexpr float WaitLen = 80.f;
constexpr float TooSoonShow = 1.f; // seconds TOO SOON stands in the call slot
// The partner's slot word's gap after its name, 1080p px: 10 px on the screen with the word's own side bearing.
constexpr float PartnerSlotGap = 6.f;
// The lead's name's dark rim, hard, at this alpha on screen. Its size is FontFor's, round(uiEdgePx) of the viewport's
// height: 1 device px at 1080 and 1440, 2 at 2160, and the HUD scale (-FTHudScale, up to 2.0) does not thicken it, so
// at scale 2.0 the letters double and the rim stays the screen's own edge. The -FTHudScale=2 captures check it.
constexpr float NameRimA = 0.85f;
constexpr float GiftGap = 24.f;    // px between GIFT HEAL's cap and GIFT GUARD's word when the GIFT row holds both
constexpr float LoneHeadInk = 0.8f; // the Lone Carry card's heads, YOU GAIN and YOU GIVE UP
// The Lone Carry's card over the party block, left-aligned on its margin, no panel: px from its last line's foot to the
// block's top, from the title's foot to the heads, between the columns, and between lines over their ascent.
constexpr float LoneGap = 24.f;
constexpr float LoneTitleGap = 14.f;
constexpr float LoneColGap = 48.f;
constexpr float LoneLineGap = 6.f;
constexpr float LoneShadeK = 1.5f; // the bottom shade's height while the card shows, over the card's top from the bottom edge
// The party screen, opened in place over the party block (no panel): each name rises PartyLift px over its hp line (the
// lines stay, the live read-out); the Third's row stands PartyRowGap px over the partner's capitals. Each row's ink and
// marks are Core's (ftcore::uiPartyRowView): the cursor's name at full ink with the reply underline across the whole
// name, PartyUnder px under its foot; every other lead row at kUiPartyOtherInk, a refused one at PartyRefusedInk with
// its word, the Third at kUiPartyThirdInk; never gold. The controlled lead's name has a dot hung before it
// (uiPartyDot). It opens and shuts over PartyOpenTime s.
constexpr float PartyLift = 10.f;
constexpr float PartyUnder = 8.f;
constexpr float PartyRowGap = 22.f;
constexpr float PartyRefusedInk = ftcore::kUiPartyRefusedInk;
constexpr float PartyOpenTime = 0.1f;
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

// The cursor's mark, left-aligned at X0: the party screen's runs W px (its name's measured width), the dialogue's
// selected reply's is the short one.
void CursorUnderline(FLineMesh& Mesh, ftcore::UiStroke& Line, float X0, float Y, float W, float S, const FLinearColor& C)
{
	ftcore::uiStrokeLine(Line, X0, Y, X0 + W, Y, StrokeStyle(7.f, 101, S, 0.1f), 0.f, 1.f);
	Mesh.Stroke(Line, C);
}
constexpr float kReplyUnderline = 48.f; // px at 1080 ("short")
void ReplyUnderline(FLineMesh& Mesh, ftcore::UiStroke& Line, float X0, float Y, float S, const FLinearColor& C)
{
	CursorUnderline(Mesh, Line, X0, Y, kReplyUnderline * S, S, C);
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

float Frac(int32 Value, int32 Max) { return Max > 0 ? FMath::Clamp(static_cast<float>(Value) / Max, 0.f, 1.f) : 0.f; }

// The text's top that centres its capitals on MidY (the serif's capitals are 0.62 of its ascent), and the middle of the
// capitals on a baseline.
float CapTop(float MidY, float Asc) { return MidY - 0.69f * Asc; }
float CapMid(float Base, float Asc) { return Base - 0.31f * Asc; }

// The party screen's row marks from Core's view: the cursor's underline under the name's foot at Base and, on the
// controlled lead's row, the dot before the name, both at full ink times K (the opening's ease) and A.
void PartyMarks(FLineMesh& Mesh, ftcore::UiStroke& Line, const ftcore::UiPartyRowView& V, float X0, float Base, float NameAsc,
	float S, float K, float A)
{
	if (V.underlineW > 0.f)
	{
		CursorUnderline(Mesh, Line, X0, Base + Hud::PartyUnder * S, V.underlineW, S, Tok(UiToken::Ink, K * A));
	}
	if (V.dot)
	{
		// The dot is ink like the underline: the mark colour (red where the marks wear accent2) is kept for the target
		// mark, low health and defeat.
		const ftcore::UiPartyDot D = ftcore::uiPartyDot(X0, CapMid(Base, NameAsc), 0.62f * NameAsc);
		Mesh.Dot(D.x, D.y, D.r, Tok(UiToken::Ink, K * A));
	}
}

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
// at 60 percent; party: the party screen open with its cursor on the partner, the partner row taken as the rules would
// take it (ordinary play holds the controlled lead alone, so there it reads NOT HERE and never takes the cursor; with
// -ArenaPartner, -ArenaSwitchAt shows the real screen and switch); party_refused: the partner row
// DOWN under the cursor and SWITCH: DOWN in the call slot; party_lone: the screen open on a Lone Carry (ALONE in the state
// slot), the partner row NOT HERE; marks: the Load at 60 percent of the Release's threshold, the debt at 90 and the
// boss's weak point open; release: the Load past the threshold (RELEASE, the name brightened) with its shimmer held
// halfway; defeat: the result card's defeat word; hit: the lead taking a hit, the edge flash held at its peak and the
// lead's hp low (a fifth), so its low line shows. With ft.NewUi.World these show each world's marks and danger words.
// A held state never counts a showing or writes the save.
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
	Party,
	PartyRefused,
	PartyLone,
	Marks,
	Release,
	Defeat,
	Hit,
	Refused,
	Special,    // the special prompt live: ready, a press would start it (arriving: the fade and the pulse)
	SpecialDim, // the special prompt ready but held: no target
	SpecialCall, // the special prompt with only the Third's ready: CALL and his current name
};
bool IsLoneShot(EUiStateShot S)
{
	return S == EUiStateShot::LoneCard || S == EUiStateShot::LoneAlone || S == EUiStateShot::LoneEnded || S == EUiStateShot::PartyLone;
}
bool IsPartyShot(EUiStateShot S) { return S == EUiStateShot::Party || S == EUiStateShot::PartyRefused || S == EUiStateShot::PartyLone; }

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
			: V == TEXT("party")                      ? EUiStateShot::Party
			: V == TEXT("party_refused")              ? EUiStateShot::PartyRefused
			: V == TEXT("party_lone")                 ? EUiStateShot::PartyLone
			: V == TEXT("marks")                      ? EUiStateShot::Marks
			: V == TEXT("release")                    ? EUiStateShot::Release
			: V == TEXT("defeat")                     ? EUiStateShot::Defeat
			: V == TEXT("hit")                        ? EUiStateShot::Hit
			: V == TEXT("refused")                    ? EUiStateShot::Refused
			: V == TEXT("special")                    ? EUiStateShot::Special
			: V == TEXT("special_dim")                ? EUiStateShot::SpecialDim
			: V == TEXT("special_call")               ? EUiStateShot::SpecialCall
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

// The player's teaching policy for the basic controls' prompts (the settings' Basic controls row; FirstFights, the
// teaching's own rule, with no settings).
ftcore::ShowBasicControls BasicControlsFor(const UFTArenaRunner& Runner)
{
	const ftcore::Settings* Prefs = Runner.GetSettings();
	return Prefs ? Prefs->showBasicControls : ftcore::ShowBasicControls::FirstFights;
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
		// The title-safe padding is read again when it may have changed at the same viewport size: the engine's safe-frame
		// change (a custom safe zone or its debug switches, a viewport resize) and the platform's display change (a monitor
		// switch, a display's mode or layout). Both broadcast on the game thread; each only marks the padding stale, and
		// the next Tick reads it (after Slate's own handler has cached the new display metrics).
		SafeFrameChange = FCoreDelegates::OnSafeFrameChangedEvent.AddSP(this, &SFTArenaHud::MarkSafeStale);
		if (FSlateApplication::IsInitialized())
		{
			if (const TSharedPtr<GenericApplication> Platform = FSlateApplication::Get().GetPlatformApplication())
			{
				DisplayChange = Platform->OnDisplayMetricsChanged().AddSP(this, &SFTArenaHud::MarkSafeStaleFor);
			}
		}
	}

	virtual ~SFTArenaHud() override
	{
		if (FontRelease.IsValid() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
		{
			FSlateApplication::Get().GetRenderer()->GetFontServices()->OnReleaseResources().Remove(FontRelease);
		}
		FCoreDelegates::OnSafeFrameChangedEvent.Remove(SafeFrameChange);
		if (DisplayChange.IsValid() && FSlateApplication::IsInitialized())
		{
			if (const TSharedPtr<GenericApplication> Platform = FSlateApplication::Get().GetPlatformApplication())
			{
				Platform->OnDisplayMetricsChanged().Remove(DisplayChange);
			}
		}
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1920.0, 1080.0); }

	virtual void Tick(const FGeometry& Geometry, const double, const float DeltaTime) override
	{
		Time += DeltaTime;
		const UFTArenaRunner* Runner = GetRunner ? GetRunner() : nullptr;
		bLive = Runner && Runner->IsBegun();
		Screens = AFTArenaHUD::ScreensOpen(Runner); // the party screen, the pause menu, the settings (ftcore::uiHudDraws)
#if FT_UI_ALLOC_COUNT
		FUiAllocCount& Count = UiAllocCount();
		Count.Seen += bLive && Count.Frame > 0 && !Count.bDone ? 1 : 0;
		const FUiAllocScope Counting(Count, 0, bLive && Count.Counting()); // this Tick's heap calls, on the frame counted
#endif
		HudScale = Runner ? HudScaleFor(*Runner) : HudScale;
		HudSafePct = Runner ? HudSafeMarginFor(*Runner) : HudSafePct;
		BasicPolicy = Runner ? BasicControlsFor(*Runner) : BasicPolicy;
		// The platform's title-safe padding, read when the viewport's size changes or the safe frame or the display does at
		// the same size (bSafeStale, set by the engine's safe-frame and the platform's display-change events; never per
		// frame): Slate's safe-zone ratio per side (the custom safe zone when one is set, else the display's title-safe
		// padding over half the display). A display of no size gives no padding.
		const FVector2f ViewSize = Geometry.GetLocalSize();
		if (bSafeStale || ViewSize != SafeSize)
		{
			bSafeStale = false;
			SafeSize = ViewSize;
			SafeRatio = FMargin(0.f);
			if (FSlateApplication::IsInitialized())
			{
				// The ratio itself is protected; the public size is the ratio times half a container: the override
				// (2 x 2, so the ratio back) in editor builds, the primary display in game builds.
				FSlateApplication& App = FSlateApplication::Get();
				FMargin Px(0.f);
				App.GetSafeZoneSize(Px, FVector2f(2.f, 2.f));
#if WITH_EDITOR
				SafeRatio = Px;
#else
				const FDisplayMetrics& Metrics = App.GetCachedDisplayMetricsByRef();
				const float HalfW = static_cast<float>(Metrics.PrimaryDisplayWidth) * 0.5f;
				const float HalfH = static_cast<float>(Metrics.PrimaryDisplayHeight) * 0.5f;
				if (HalfW > 0.f && HalfH > 0.f)
				{
					SafeRatio = FMargin(Px.Left / HalfW, Px.Top / HalfH, Px.Right / HalfW, Px.Bottom / HalfH);
				}
#endif
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
			// The once-a-fight labels and every stamp on the fight's clock start again with the next fight (its clock
			// starts again too, so a stamp from this one would read as the future).
			LoadLabel = ftcore::UiLoadLabel{};
			WeakLabel = ftcore::UiOnceLabel{};
			LoadLabelA = WeakLabelA = 0.f;
			LoadColumn = ftcore::UiLabelColumn{};
			HpFigA = 1.f;
			GuardLabel = ftcore::UiGuardLabel{};
			HitSegs = ftcore::UiHitSegs{};
			HitWatch.seeded = false;
			ReleasePulse = ftcore::UiReleasePulse{};
			return;
		}
		const ftcore::ArenaFight& Fight = Runner->GetFight();
		// A retry puts each lead back in its own figure (the runner's Restart): its first seats, not a switch.
		if (Fight.stats().restarts != RestartsSeen)
		{
			RestartsSeen = Fight.stats().restarts;
			RowSwap = ftcore::UiRowSwap{};
			bRebuild = true;
			// A new fight: no press is judged against the last fight's answers, and its reset hit stamp is no hit.
			WhyWatch.seeded = false;
			RefusedRows = ftcore::UiRefusedRows{};
			HitWatch.seeded = false;
			GuardLabel = ftcore::UiGuardLabel{};
			LoadLabel = ftcore::UiLoadLabel{};
			WeakLabel = ftcore::UiOnceLabel{};
			LoadColumn = ftcore::UiLabelColumn{};
			ReleasePulse = ftcore::UiReleasePulse{};
			// Each blow's red lost hp starts empty: the first step only reads the hp, so a retry at equal or lower hp draws
			// no segment from the last fight's blows.
			HitSegs = ftcore::UiHitSegs{};
			LoadFill = 0.f;
		}
		// A lead switch or a takeover traded the seats (the runner's SwapLeadFigures): each hp trail follows its lead to
		// its new row, and every string built from the controlled lead (its name, its prompts' keys and words) is built
		// again for the new one.
		if (RowSwap.lead != 0xFF && Runner->GetLeadSeat() != RowSwap.lead)
		{
			Swap(HpLost, PartnerLost);
			HitSegs = ftcore::UiHitSegs{}; // the new lead's first step only reads its hp
			bRebuild = true;
		}
		const bool bRebuildNow = bRebuild;
		bRebuild = false;
		AimA = Runner->GetAimAlpha();
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
		// Gift Heals left (GIFT HEAL n, cooling or not) and each gift's answer from the slot itself (GetGiftReason: would
		// its press be taken now, and if not, why), so a cooling or spent gift stands dimmed with its word. The Gift Guard
		// is ready when the slot answers Ok: the HUD reads the rules' reason, never a timer of its own (a figure, were one
		// drawn, would come from GetGiftGuardReadyIn). A live Qte (Frame.lead.qte, which fillArenaHudLead leaves alone)
		// would go here too; nothing reports one yet.
		Frame.third = ftcore::ArenaHudThird{};
		GiftWhy[0] = GiftWhy[1] = ftcore::PressReason::Absent;
		if (Runner->GetSession())
		{
			Frame.third.giftHeals = Runner->GetGiftHealsLeft();
			GiftWhy[0] = Runner->GetGiftReason(ftcore::Gift::Heal);
			GiftWhy[1] = Runner->GetGiftReason(ftcore::Gift::Guard);
		}
		const EUiStateShot Shot = UiStateShot();
		if (Shot == EUiStateShot::Gift || Shot == EUiStateShot::NoKey)
		{
			Frame.third.giftHeals = FMath::Max(Frame.third.giftHeals, 2);
			GiftWhy[0] = GiftWhy[1] = ftcore::PressReason::Ok;
			if (Frame.lead.hpMax > 0 && Frame.lead.hp >= Frame.lead.hpMax)
			{
				Frame.lead.hp = Frame.lead.hpMax * 7 / 10;
			}
		}
		if (Shot == EUiStateShot::Marks || Shot == EUiStateShot::Release)
		{
			ftcore::ArenaHudLead& Ld = Frame.lead;
			if (Ld.loadMax <= 0 || Ld.releaseAt <= 0 || Ld.loadCap <= Ld.releaseAt)
			{
				Ld.loadMax = Ld.loadCap = 90; // a lead with no Load of its own: the default cap's numbers
				Ld.releaseAt = 30;
			}
			Ld.load = Shot == EUiStateShot::Marks ? Ld.releaseAt * 6 / 10 : (Ld.releaseAt + Ld.loadCap) / 2;
			Ld.releaseReady = Shot == EUiStateShot::Release;
			Ld.overloaded = false;
		}
		if (Shot == EUiStateShot::Hit && Frame.lead.hpMax > 0)
		{
			Frame.lead.hp = Frame.lead.hpMax / 5; // low, under the quarter
		}
		Frame.third.giftGuardReady = GiftWhy[1] == ftcore::PressReason::Ok;
		const uint32 Changed = ftcore::updateArenaHudView(View, Frame);
		const ftcore::ArenaHudLead& L = View.now.lead;
		// The lost traces on the engine's frame clock; each blow's red lost hp and the GUARD and LOAD labels on the fight's
		// clock (the runner's Now), as the hit cue, so a step at the same time ages nothing and a held clock (a pause, a
		// capture's hold) holds them.
		const float FrameDt = static_cast<float>(FApp::GetDeltaTime());
		const double FightNow = Runner->GetNow();
		HpLost.Step(Frac(L.hp, L.hpMax), FrameDt);
		ftcore::uiHitSegsStep(HitSegs, Frac(L.hp, L.hpMax), FightNow);
		if (Shot == EUiStateShot::Hit)
		{
			const float Hp = Frac(L.hp, L.hpMax);
			HpLost.Was = FMath::Max(HpLost.Was, Hp + 0.25f); // the still's blow: a quarter just lost, held at full red
			HitSegs.seg[0] = ftcore::UiHitSeg{Hp, FMath::Min(1.f, Hp + 0.25f), 0.f};
		}
		GuardLabelA = ftcore::uiGuardLabelStep(GuardLabel,
			GuardShown() ? ftcore::uiGuardState(L.stamina01, L.guardBroken) : ftcore::UiGuardState::Full, FightNow);
		PartnerLost.Step(Frac(View.now.partner.hp, View.now.partner.hpMax), FrameDt);
		{
			// The Load line's gold: up at once, down draining the line's length per kUiLoadDrain. LOAD beside it on the
			// fight's first Load only, 2 s then a 1 s fade (uiLoadLabelStep); a retry starts a new fight.
			const float To = Frac(L.load, L.loadCap);
			LoadFill = To >= LoadFill ? To : FMath::Max(To, LoadFill - FrameDt / ftcore::kUiLoadDrain);
			// LOAD and the hp figure share a column and take turns in it (uiLabelColumnStep): never both drawn.
			const ftcore::UiLabelColumnView Col = ftcore::uiLabelColumnStep(LoadColumn, ftcore::uiLoadLabelStep(LoadLabel, L.load, FightNow), FightNow);
			LoadLabelA = Col.label;
			HpFigA = Col.fig;
			// The Release pulse (signature device 1): stepped always, drawn only where the world's row turns it on.
			ReleasePulseV = ftcore::uiReleasePulseStep(ReleasePulse, L.releaseReady, L.overloaded, FightNow);
		}
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

		// The prompts' keys on the device in use (built when the device changes, or the bindings do: their generation);
		// the counts that sit in their words.
		const APlayerController* PC = Runner->GetWorld() ? Runner->GetWorld()->GetFirstPlayerController() : nullptr;
		const ftcore::InputDevice Device = FTInputBindings::NoteDevice(PC);
		const uint32 Bindings = FTInputBindings::Generation();
		if (bRebuildNow || Device != KeyDevice || Bindings != KeyGeneration)
		{
			KeyDevice = Device;
			KeyGeneration = Bindings;
			BuildKeys(Device);
		}
		if (bRebuildNow || L.heals != HealShown)
		{
			HealShown = L.heals;
			HealText = FString::FromInt(L.heals); // the count alone: the verb stands in its own column
			HealW = TextWidth(HealText, FigFont);
		}
		const int32 GiftHeals = View.now.third.giftHeals;
		if (bRebuildNow || GiftHeals != GiftHealShown)
		{
			GiftHealShown = GiftHeals;
			GiftHealText = GiftHeals >= 0 ? FString::FromInt(GiftHeals) : FString();
			GiftHealW = TextWidth(GiftHealText, FigFont);
		}
		// The hp figure, "412 / 600", built when either number changes; its slot is as wide as "600 / 600", so its right
		// edge stays put as the hp falls.
		if (bRebuildNow || L.hp != HpFigHp || L.hpMax != HpFigMax)
		{
			HpFigHp = L.hp;
			HpFigMax = L.hpMax;
			HpFigText = L.hpMax > 0 ? FString::Printf(TEXT("%d / %d"), FMath::Max(L.hp, 0), L.hpMax) : FString();
			HpFigW = TextWidth(HpFigText, FigFont);
			HpFigSlotW = L.hpMax > 0 ? TextWidth(FString::Printf(TEXT("%d / %d"), L.hpMax, L.hpMax), FigFont) : 0.f;
		}
		{
			const ftcore::ArenaHudBoss& Bo = View.now.boss;
			const int32 Pct = Bo.present ? ftcore::uiBossPercent(Bo.hp, Bo.hpMax) : -1;
			if (bRebuildNow || Pct != BossPctShown)
			{
				BossPctShown = Pct;
				BossPctText = Pct >= 0 ? FString::Printf(TEXT("%d%%"), Pct) : FString();
				BossPctW = TextWidth(BossPctText, FigFont);
			}
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
			DebtW = TextWidth(DebtText, NoteFont);
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
		TickParty(*Runner, Fight, DeltaTime);
		TickBudget(*Runner, Fight, Changed, DeltaTime);
		TickSpecial(*Runner, Fight, DeltaTime);
		// WEAK steps after TickWorld and TickBudget have set this tick's mark, so the label comes with the diamond.
		WeakLabelA = ftcore::uiOnceLabelStep(WeakLabel, bWeakOn, FightNow);
		TickLabels(*Runner);
	}

	// The special row's inputs: ready, and while it cannot start now, why; and the Third's ready (the player calls it):
	// never a level. No gauge reports one yet, so the HUD feeds not ready; -UiStateShot=special, special_dim or
	// special_call holds the live, the dim or the CALL row for captures. The arena lane's rules fill In when they come:
	// ready (the controlled lead's), why, thirdReady; a switch of lead is their unready (the row leaves).
	void TickSpecial(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, float DeltaTime)
	{
		ftcore::UiSpecialInput In;
		const EUiStateShot Shot = UiStateShot();
		if (!Fight.over() && (Shot == EUiStateShot::Special || Shot == EUiStateShot::SpecialDim))
		{
			In.ready = true;
			In.why = Shot == EUiStateShot::SpecialDim ? ftcore::UiSpecialWhy::NoTarget : ftcore::UiSpecialWhy::None;
		}
		In.thirdReady = !Fight.over() && Shot == EUiStateShot::SpecialCall;
		const ftcore::Settings* Prefs = Runner.GetSettings();
		Special = ftcore::uiSpecialStep(SpecialState, Shown().bSpecialPrompt, In, DeltaTime, Prefs && Prefs->reduceMotion);
		// The tail after the word: the lead's name, the Third's (CALL) or the reason. While the row stands it follows what
		// is shown (copied on a change only); a leave keeps the last one, so a lead switch never shows the new name fading.
		if (SpecialState.ready)
		{
			const FString& Src = Special.call ? ThirdName : Special.live ? LeadName : SpecialWhyWord[static_cast<int32>(Special.why)];
			if (!SpecialTail.Equals(Src, ESearchCase::CaseSensitive))
			{
				SpecialTail = Src;
				SpecialTailW = TextWidth(SpecialTail, PromptFont);
			}
		}
	}

	// The element budget (Core): the data asks for marks, the budget grants them in priority order, and what is not granted
	// is not drawn. At rest only the party rows and the prompts; never more than the cap in action.
	void TickBudget(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, uint32 Changed, float DeltaTime)
	{
		using ftcore::UiMarkKind;
		const ftcore::ArenaHudFrame& Now = View.now;
		const ftcore::ArenaHudBoss& B = Now.boss;
		ftcore::UiBudgetInput In;
		In.dt = DeltaTime;
		In.changed = Changed;
		// Each prompt's answer from the rules (Core's uiPromptReasons: the fight's own presses, and the gift slot's as the
		// session gives them): a prompt whose action is not in this fight is not drawn (uiPromptsPresent, the rules' query);
		// every other stands in its row, but one whose press would be refused now is not drawn while idle: only a press of
		// its key while refused shows it, for kUiRefusedShow s at kUiRefusedInk with its reason (uiRefusedStep). Over that the
		// teaching policy, the player's Basic controls setting (uiPromptsTaught, not a rule): by default the controls stand
		// while the first-use teaching holds them and go when it ends; Always keeps them, Never shows none.
		Why = ftcore::uiPromptReasons(Fight, GiftWhy[0], GiftWhy[1]);
		// The presses since the last tick, by prompt (the runner's counts). The first read only takes the counts as they
		// stand, so a widget made while the runner already counted presses shows no refusal for them.
		ftcore::UiPromptMask Pressed = 0;
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiPromptCount); ++I)
		{
			const uint32 N = Runner.GetPromptPresses(I);
			if (N != PromptPressSeen[I])
			{
				PromptPressSeen[I] = N;
				Pressed |= bPressSeeded ? ftcore::uiPromptBit(static_cast<ftcore::UiPrompt>(I)) : ftcore::UiPromptMask{0};
			}
		}
		bPressSeeded = true;
		if (UiStateShot() == EUiStateShot::Refused)
		{
			// The still: HEAL pressed at full health, its row held showing FULL.
			constexpr size_t Heal = static_cast<size_t>(ftcore::UiPrompt::Heal);
			Why[Heal] = ftcore::PressReason::Full;
			Pressed |= ftcore::uiPromptBit(ftcore::UiPrompt::Heal);
		}
		// A press counts as refused only if it was refused before it (the last tick's answers): one that worked and left
		// its action refused (the last heal, the Pair's cooldown starting) shows no row.
		// A view's first tick, and a retry's (the watch is cleared then), only read the answers.
		if (UiStateShot() == EUiStateShot::Refused)
		{
			WhyWatch.before = Why;
			WhyWatch.seeded = true;
		}
		ftcore::uiRefusedWatchStep(RefusedRows, WhyWatch, Why, Pressed, DeltaTime);
		RefusedShowing = ftcore::uiRefusedShowing(RefusedRows);
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
		// The clutter rule over that (uiPromptsUsable): HEAL only under half health with a charge left, a GIFT row only
		// while its gift would be taken now; a row showing its refusal stays.
		const ftcore::UiPromptMask Usable = ftcore::uiPromptsUsable(ftcore::uiPromptsStanding(Why, RefusedRows), View.now.lead, Why, RefusedShowing);
		const ftcore::UiPromptMask Standing = ftcore::uiPromptsTaught(Usable, BasicPolicy, Teaching);
		In.prompts = ftcore::uiPromptsDrawable(Standing, PromptKeyed, PromptSameKeyBelow);
		PromptDimmed = ftcore::uiPromptsDimmed(Why, PromptNoKey);
		In.resultOpen = bResult;
		In.want = ftcore::uiMarkWants(Now, In.prompts);
		// A live Qte is a timed press like a windup: it counts as a tell, so the budget never drops it. So does TOO SOON while
		// it stands, the call slot's other word of the player's own, and SWITCH: <WORD> in its place (one word at a time).
		const bool bQteLive = Now.lead.qte.live;
		const bool bTooSoonLive = TooSoonLeft > 0.f;
		const bool bSwitchLive = SwitchLeft > 0.f && !SwitchText.IsEmpty();
		uint8 Tells = static_cast<uint8>((bQteLive ? 1 : 0) + (bTooSoonLive || bSwitchLive ? 1 : 0));
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
		// After the windups: a foe's blow outranks a word about the player's timing. The newer refusal, a switch's, first.
		bSwitchOn = bSwitchLive && TellsLeft > 0;
		bTooSoonOn = !bSwitchOn && bTooSoonLive && TellsLeft > 0;
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
		// Under the screens (ftcore::uiHudDraws): nothing under the pause menu or the settings; under the party screen (the
		// runner's, or a party capture's held one) its rows and the vitals, no aim, prompts or special.
		const ftcore::UiScreenMask Open = static_cast<ftcore::UiScreenMask>(Screens | (PartyShown.open ? ftcore::uiScreenBit(ftcore::UiScreen::Party) : 0));
		const auto Draws = [Open](ftcore::UiHudPart Part) { return ftcore::uiHudDraws(Part, ftcore::UiHudMode::Fight, Open); };
		if (!bLive || (!Draws(ftcore::UiHudPart::FightVitals) && !Draws(ftcore::UiHudPart::PartyRows)))
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
		// Every word rim-only, from the kept glyph runs.
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
		if (Draws(ftcore::UiHudPart::FightAim))
		{
			PaintAim(P, Size);
		}
		PaintParty(P, Size);
		PaintLone(P, Size);
		PaintTop(P, Size);
		PaintCall(P, Size);
		PaintSecondLine(P, Size);
		if (Draws(ftcore::UiHudPart::FightPrompts))
		{
			PaintPrompts(P, Size);
		}
		if (Draws(ftcore::UiHudPart::FightSpecial))
		{
			PaintSpecial(P, Size);
		}
		PaintEvents(P, Size);
		Mesh.Flush(Out, Layer + 1); // every line, mark and shade in one element under the words
		return Layer + 5;           // the Load's gold and its bright edge stand over the name's words
	}

private:
	// The safe frame or the display changed (FCoreDelegates::OnSafeFrameChangedEvent, the platform's
	// OnDisplayMetricsChanged): the title-safe padding is read again at the next Tick, whatever the viewport's size.
	void MarkSafeStale() { bSafeStale = true; }
	void MarkSafeStaleFor(const FDisplayMetrics&) { bSafeStale = true; }

	// The fonts and the words that need no fight, at this height.
	void BuildFonts(float H)
	{
		const TSharedRef<FSlateFontMeasure> FM = FontMeasure();
		const auto Asc = [&FM](const FSlateFontInfo& F) { return FM->GetMaxCharacterHeight(F) + FM->GetBaseline(F); };
		// The names in spaced small caps, as the boss's.
		const auto Spaced = [](FSlateFontInfo F, int32 Tracking = Hud::SpacedTracking)
		{
			F.LetterSpacing = Tracking;
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
		NameFont = Spaced(FontPx(UiTextRole::Title, H, Hud::BossNamePx));
		NameAsc = Asc(NameFont);
		LeadNameFont = Spaced(FontPx(UiTextRole::Title, H, Hud::LeadPx), Hud::NameTracking);
		LeadNameFont.OutlineSettings.OutlineColor.A = Hud::NameRimA; // the alpha on screen, not a share of the edge's
		LeadNameAsc = Asc(LeadNameFont);
		PartnerNameFont = Spaced(FontPx(UiTextRole::Title, H, Hud::PartnerPx), Hud::NameTracking);
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
		NoteFont = Spaced(FontPx(UiTextRole::Label, H, Hud::NotePx));
		NoteAsc = Asc(NoteFont);
		FigFont = FontPx(UiTextRole::Value, H, Hud::FigPx);
		FigFont.LetterSpacing = 0;
		FigAsc = Asc(FigFont);
		WeakWord = kWeakUi.Get().ToUpper();
		// NO KEY for the key box, and the reason words a refused prompt carries, at label size.
		NoKeyWord = kNoKeyUi.Get().ToUpper();
		SpecialWord = kSpecialUi.Get().ToUpper();
		SpecialWordW = TextWidth(SpecialWord, PromptFont);
		CallWord = kSpecialCallUi.Get().ToUpper();
		CallWordW = TextWidth(CallWord, PromptFont);
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiSpecialWhyCount); ++I)
		{
			SpecialWhyWord[I] = kSpecialWhyUi[I] ? kSpecialWhyUi[I]->Get().ToUpper() : FString();
			SpecialWhyW[I] = TextWidth(SpecialWhyWord[I], PromptFont);
		}
		SpecialTailW = TextWidth(SpecialTail, PromptFont);
		for (int32 I = 0; I < static_cast<int32>(ftcore::kPressReasonCount); ++I)
		{
			WhyWord[I] = kWhyUi[I] ? kWhyUi[I]->Get().ToUpper() : FString();
			WhyW[I] = TextWidth(WhyWord[I], NoteFont);
		}
		ResultFont = FontFor(UiTextRole::Subtitle, H);
		ResultLineH = FM->GetMaxCharacterHeight(ResultFont) * 1.15f; // the subtitle's line
		ResultAsc = Asc(ResultFont);
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
		// The party screen's words: a refused lead row's at label size after the name (DOWN as the prompts', WAIT as the
		// state slot's), the Third's row, and the refused switch's call prefix (its text measured again in the new face).
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiPartyWhyCount); ++I)
		{
			PartyWhyWord[I].Reset();
		}
		PartyWhyWord[static_cast<int32>(ftcore::UiPartyWhy::Down)] = kWhyDownUi.Get().ToUpper();
		PartyWhyWord[static_cast<int32>(ftcore::UiPartyWhy::NotHere)] = kNotHereUi.Get().ToUpper();
		PartyWhyWord[static_cast<int32>(ftcore::UiPartyWhy::FightOver)] = kFightOverUi.Get().ToUpper();
		PartyWhyWord[static_cast<int32>(ftcore::UiPartyWhy::Wait)] = WaitWord;
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiPartyWhyCount); ++I)
		{
			PartyWhyW[I] = TextWidth(PartyWhyWord[I], NoteFont);
		}
		GiftReadyWord = kReadyUi.Get().ToUpper();
		GiftVacantWord = kVacantUi.Get().ToUpper();
		GiftHostileWord = kHostileUi.Get().ToUpper();
		GiftSecondsFmt = kSecondsUi.Get();
		ThirdKey = MAX_uint32; // its line built again in the new words
		SwitchPrefix = kSwitchUi.Get().ToUpper();
		BuildSwitchText();
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
		// The special's key: its own action (Z; on a pad the shoulders chord, LB+RB), NO KEY when the import left it unbound.
		{
			const ftcore::InputPromptKey K = ftcore::inputPromptKey(*Bound, ftcore::InputAction::Special, Device, Left);
			SpecialKey = K.noKey ? NoKeyWord : FT::ToFString(K.keys);
			SpecialKeyW = TextWidth(SpecialKey, KeyFont);
		}
		// The jump press reaches the fight only under the jump switch (UFTArenaRunner::JumpBound; never in Shipping):
		// without it nothing reaches JUMP, so its row is not drawn, whatever the Basic controls setting.
		if (!UFTArenaRunner::JumpBound())
		{
			PromptKeyed &= static_cast<ftcore::UiPromptMask>(~ftcore::uiPromptBit(ftcore::UiPrompt::Jump));
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

	// The party rows' names: the looks' own, in true capitals; without a look, a plain word. The Third's is the story's
	// current one (UFTGameInstance::ThirdName: one of two strings that live until exit, so a change is a new address).
	void TickNames(const UFTArenaRunner& Runner, bool bForce)
	{
		const auto Build = [bForce](const FString& Src, const FString& Fallback, FString& Seen, FString& Out, float& W, const FSlateFontInfo& F)
		{
			if (bForce || !Seen.Equals(Src, ESearchCase::CaseSensitive))
			{
				Seen = Src;
				Out = (Src.IsEmpty() ? Fallback : Src).ToUpper();
				W = TextWidth(Out, F);
				return true;
			}
			return false;
		};
		Build(Runner.GetLeadName(), kLeadUi, LeadNameSeen, LeadName, LeadNameW, LeadNameFont);
		Build(Runner.GetAllyName(), kPartnerUi, PartnerNameSeen, PartnerName, PartnerNameW, PartnerNameFont);
		const FString& Third = UFTGameInstance::ThirdName(&Runner);
		if (bForce || &Third != ThirdNameSrc)
		{
			ThirdNameSrc = &Third;
			ThirdName = Third.ToUpper();
			ThirdNameW = TextWidth(ThirdName, PartnerNameFont);
		}
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
			bSaveTaught = FirstUseMode() == ftcore::UiFirstUseMode::Saved && !Runner.IsAutoPlay() && !Runner.IsIdle()
				&& UiStateShot() == EUiStateShot::None;
			ftcore::uiFirstUseBegin(FirstUse, Saved, Mode);
			// The teaching policy: with Basic controls set to Never no control is shown, so none is taught or counted
			// (a later fight under another setting still teaches them).
			if (BasicControlsFor(Runner) != ftcore::ShowBasicControls::Never)
			{
				Teach(Runner, UiFirstUseItem::Attack);
				Teach(Runner, UiFirstUseItem::Guard);
				Teach(Runner, UiFirstUseItem::Dodge);
				if (UFTArenaRunner::JumpBound())
				{
					Teach(Runner, UiFirstUseItem::Jump);
				}
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
	// never stands beside nothing). Every label is cued every tick, so each sees its element's rise. LOAD in the lead's
	// state slot is one: it shows as the slot turns to the Load. The guard and the poise need none: their lines carry
	// their own words whenever they are drawn.
	void TickLabels(const UFTArenaRunner& Runner)
	{
		if (bTeaching && ftcore::uiFirstUseCue(FirstUse, ftcore::UiFirstUseItem::WeakPointLabel, bWeakOn))
		{
			SaveTaught(Runner);
		}
		const bool bLoadSlot = View.now.lead.hpMax > 0 && Budget.out.allowed(ftcore::UiMarkKind::Lead) > 0
			&& ftcore::uiLeadWord(ftcore::uiLeadSlot(View.now.lead), bWaiting, ftcore::uiLoneWord(Lone)) == ftcore::UiLeadWord::Load;
		if (bTeaching && ftcore::uiFirstUseCue(FirstUse, ftcore::UiFirstUseItem::LoadLabel, bLoadSlot))
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
		case EUiStateShot::PartyLone:
			Lone.alone = true;
			break;
		case EUiStateShot::None:
		case EUiStateShot::Gift:
		case EUiStateShot::NoKey:
		case EUiStateShot::TooSoon:
		case EUiStateShot::Wait:
		case EUiStateShot::Party:
		case EUiStateShot::PartyRefused:
		case EUiStateShot::Marks:
		case EUiStateShot::Release:
		case EUiStateShot::Defeat:
		case EUiStateShot::Hit:
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

	// The party screen (Core's UiPartyScreen; the runner steps it and sends the switch): its rows open over the party
	// block. The partner row's word is the rules' (GetLeadSwitchReason: the session's leadSwitchReason, the step's close's
	// own predicate); the Third's line is the gift slot's (GetThirdSlotRefusal and GetThirdSlot for the slot, GetGiftReason
	// for each gift; the count and the seconds from GetGiftHealsLeft and GetGiftGuardReadyIn). A switch the session refused
	// (GetSwitchRefusedSerial) puts SWITCH: <WORD> in the call slot for kUiPartySwitchShow, in TOO SOON's place. Strings
	// are built when what they show changes, never per frame.
	void TickParty(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, float DeltaTime)
	{
		using ftcore::UiPartyWhy;
		const bool bFight = !Fight.over();
		PartyShown = Runner.GetPartyScreen();
		const uint8 Other = Runner.GetSessionLead() == ftcore::kLeadA ? ftcore::kLeadB : ftcore::kLeadA;
		PartnerWhy = Runner.GetLeadSwitchReason(Other);
		UiPartyWhy Refused = UiPartyWhy::Unknown;
		const uint32 Serial = Runner.GetSwitchRefusedSerial();
		if (Serial != SwitchSerialSeen)
		{
			SwitchSerialSeen = Serial;
			Refused = Runner.GetSwitchRefusedWhy();
		}
		const EUiStateShot Shot = UiStateShot();
		const bool bShot = bFight && IsPartyShot(Shot);
		if (bShot)
		{
			PartyShown.open = true;
			PartyShown.explore = false;
			PartyShown.cursor = Shot == EUiStateShot::PartyLone ? ftcore::UiPartyRow::Lead : ftcore::UiPartyRow::Partner;
			PartnerWhy = Shot == EUiStateShot::Party ? UiPartyWhy::Ok : Shot == EUiStateShot::PartyRefused ? UiPartyWhy::Down : UiPartyWhy::NotHere;
			PartyShown.partnerHere = PartnerWhy != UiPartyWhy::NotHere; // the held state's partner, as the screen would take it
			if (Shot == EUiStateShot::PartyRefused)
			{
				Refused = UiPartyWhy::Down;
			}
		}
		if (ftcore::uiPartyRefused(Refused))
		{
			if (Refused != SwitchWhy)
			{
				SwitchWhy = Refused;
				BuildSwitchText();
			}
			SwitchLeft = ftcore::kUiPartySwitchShow;
		}
		else
		{
			SwitchLeft = FMath::Max(0.f, SwitchLeft - DeltaTime);
		}
		if (!bFight)
		{
			SwitchLeft = 0.f;
		}
		PartyOpenK = bShot ? 1.f : FMath::Clamp(PartyOpenK + (PartyShown.open ? DeltaTime : -DeltaTime) / Hud::PartyOpenTime, 0.f, 1.f);
		if (PartyShown.open)
		{
			PartyDrawn = PartyShown; // a closing screen fades with the rows it had
		}

		// The Third's row: drawn only with a gift slot (a session). The slot refused is one word; else GIFT HEAL n (its
		// reason word after the count when refused) and GIFT GUARD READY (its reason word, or its seconds while it cools,
		// in READY's place), each only while the rules have it in this fight.
		bThirdRow = Runner.GetSession() != nullptr;
		if (!bThirdRow)
		{
			return;
		}
		const ftcore::GiftError SlotWhy = Runner.GetThirdSlotRefusal();
		const ftcore::SlotState Slot = Runner.GetThirdSlot();
		const bool bVacant = SlotWhy == ftcore::GiftError::Vacant || (SlotWhy == ftcore::GiftError::None && Slot == ftcore::SlotState::Vacant);
		const bool bHostile = SlotWhy == ftcore::GiftError::Hostile || (SlotWhy == ftcore::GiftError::None && Slot == ftcore::SlotState::Hostile);
		const uint32 SlotWord = bVacant ? 1u : (bHostile ? 2u : 0u);
		const int32 Heals = FMath::Max(0, View.now.third.giftHeals);
		const bool bGuardCools = GiftWhy[1] == ftcore::PressReason::Cooldown;
		const int32 GuardS = bGuardCools ? FMath::Max(1, static_cast<int32>(FMath::CeilToFloat(static_cast<float>(Runner.GetGiftGuardReadyIn())))) : 0;
		const uint32 Key = (SlotWord << 30) | (static_cast<uint32>(GiftWhy[0]) << 24) | (static_cast<uint32>(GiftWhy[1]) << 18)
			| (static_cast<uint32>(FMath::Min(Heals, 255)) << 10) | static_cast<uint32>(FMath::Min(GuardS, 1023));
		if (Key == ThirdKey)
		{
			return;
		}
		ThirdKey = Key;
		ThirdText.Reset();
		if (SlotWord != 0u)
		{
			ThirdText = SlotWord == 1u ? GiftVacantWord : GiftHostileWord;
			return;
		}
		if (GiftWhy[0] != ftcore::PressReason::Absent)
		{
			ThirdText = PromptWord[static_cast<int32>(ftcore::UiPrompt::GiftHeal)] + TEXT(" ") + FString::FromInt(Heals);
			const FString& HealWhy = WhyWord[static_cast<int32>(GiftWhy[0])];
			if (!HealWhy.IsEmpty())
			{
				ThirdText += TEXT(" ") + HealWhy;
			}
		}
		if (GiftWhy[1] != ftcore::PressReason::Absent)
		{
			if (!ThirdText.IsEmpty())
			{
				ThirdText += TEXT(" · "); // a middle dot between the gifts
			}
			const FString& GuardWhy = WhyWord[static_cast<int32>(GiftWhy[1])];
			ThirdText += PromptWord[static_cast<int32>(ftcore::UiPrompt::GiftGuard)] + TEXT(" ")
				+ (bGuardCools ? FString::Format(*GiftSecondsFmt, {GuardS}) : GuardWhy.IsEmpty() ? GiftReadyWord : GuardWhy);
		}
	}

	// SWITCH: <WORD> for the refusal shown (SwitchWhy), measured in the call slot's face.
	void BuildSwitchText()
	{
		const FString& Word = PartyWhyWord[static_cast<size_t>(SwitchWhy) % ftcore::kUiPartyWhyCount];
		SwitchText = Word.IsEmpty() ? FString() : SwitchPrefix + TEXT(" ") + Word;
		SwitchMid = InkMid(SwitchText, CallFont);
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
	// no display name (a kind with no arena.name key) keeps its line, drawn alone without a name. That is intended, not a
	// gap: its body carries no hp block of its own (the foe marks leave the target to the top line), so without the line
	// its hp would show nowhere.
	bool TopShown() const
	{
		const ftcore::ArenaHudBoss& B = View.now.boss;
		return B.present ? Budget.out.allowed(ftcore::UiMarkKind::Boss) > 0
						 : View.now.target.present && Budget.out.allowed(ftcore::UiMarkKind::Target) > 0;
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
		float Taper = Hud::ValueTaper, const FLinearColor* Rim = nullptr) const
	{
		if (To01 - From01 < 0.002f || C.A <= 0.f)
		{
			return;
		}
		const float WD = DevWidth(Width, S);
		const float YS = SnapLine(Y, Width, S);
		ftcore::uiStrokeLine(Line, X0, YS, X1, YS, ValueStyle(WD, Seed, S, Taper), From01, To01);
		Mesh.ValueStroke(Line, C, Rim);
	}

	// A value line's unfilled track the full length (health, guard: one family; ftcore::uiTrackSet): an untapered black
	// body with its 1 px dark edge, and along its top edge a 1 device px hairline at L 0.42 on the unlit length (from
	// Lit01 on; the fill never sits on it). Over any stage one of the two holds 3:1.
	void TrackLine(float X0, float X1, float Y, float Width, std::uint32_t Seed, float S, float A, float Lit01 = 0.f) const
	{
		if (A <= 0.f)
		{
			return;
		}
		const FUiWorldRow& R = Shown();
		const float WD = DevWidth(Width, S);
		const float YS = SnapLine(Y, Width, S);
		ftcore::uiStrokeLine(Line, X0, YS, X1, YS, ValueStyle(WD, Seed, S, 0.f), 0.f, 1.f);
		FLinearColor Body = R.TrackBody;
		Body.A *= A;
		Mesh.ValueStroke(Line, Body, nullptr, 1.f);
		if (Mesh.WholeDev(Width * S) < 2.f * Mesh.PxSize())
		{
			return; // a 1 device px body has no room for a hairline over it: the body alone
		}
		const float HX0 = Mesh.SnapX(X0 + (X1 - X0) * FMath::Clamp(Lit01, 0.f, 1.f));
		const float HX1 = Mesh.SnapX(X1);
		const float Top = YS - 0.5f * Mesh.WholeDev(Width * S);
		FLinearColor Hair = R.TrackHair;
		Hair.A *= A;
		Mesh.Rect(HX0, Top, HX1, Top + Mesh.PxSize(), Hair, true);
	}

	// A value line's end cap (the r19c base): a square of ink, the line's width plus lines.capGrow px a side, on the fill's
	// end and moving with it; at 0 it sits on the track's start, at full on its end (ftcore::uiCapX). Edged like the lines
	// (Mesh.Fill). None on the dotted guard; a line with no value draws none.
	void EndCap(float X0, float Len, float YS, float Fill01, float Width, float S, float A) const
	{
		if (A <= 0.f)
		{
			return;
		}
		const float Side = Mesh.WholeDev((Width + Table().Style.lines.capGrow) * S);
		const float L = Mesh.SnapX(ftcore::uiCapX(X0, Len, Fill01, Side) - 0.5f * Side);
		const float T = Mesh.SnapLineY(YS, Side) - 0.5f * Side;
		const FVector2f Pts[4] = {FVector2f(L, T), FVector2f(L + Side, T), FVector2f(L + Side, T + Side), FVector2f(L, T + Side)};
		Mesh.Fill(Pts, 4, Tok(UiToken::Ink, A));
	}

	// The guard (the r19c base): the value-line family as dots, lines.guardDot px square at lines.guardPitch, whole device
	// pixels; the dots up to the guard's share in the world's fill (low: the low colour, as LowValueLine, with its
	// underline where the world draws one), the rest in the track's body. No cap. A fixed loop: nothing allocated.
	void GuardDots(float X0, float Len, float Y, float G, float S, bool bLow, float A) const
	{
		if (A <= 0.f)
		{
			return;
		}
		const FUiWorldRow& R = Shown();
		const ftcore::UiLineShape& Ln = Table().Style.lines;
		const float Dot = Mesh.WholeDev(Ln.guardDot * S);
		const float Pitch = FMath::Max(Mesh.WholeDev(Ln.guardPitch * S), Dot + Mesh.PxSize());
		const float Top = Mesh.SnapLineY(Y, Dot) - 0.5f * Dot;
		FLinearColor On = bLow ? R.LowLine : R.Fill;
		On.A *= A * (bLow && !R.bLowUnder ? LowPulse() : 1.f);
		FLinearColor Off = R.TrackBody;
		Off.A *= A;
		const float Lit = X0 + Len * FMath::Clamp(G, 0.f, 1.f);
		const int32 N = Len > Dot ? FMath::FloorToInt((Len - Dot) / Pitch) + 1 : 1;
		for (int32 I = 0; I < N; ++I)
		{
			const float DX = Mesh.SnapX(X0 + Pitch * static_cast<float>(I));
			const FVector2f Pts[4] = {FVector2f(DX, Top), FVector2f(DX + Dot, Top), FVector2f(DX + Dot, Top + Dot), FVector2f(DX, Top + Dot)};
			Mesh.Fill(Pts, 4, DX + 0.5f * Dot < Lit ? On : Off);
		}
		if (bLow && R.bLowUnder)
		{
			FLinearColor U = R.LowUnder;
			U.A *= A * LowPulse();
			const ftcore::UiLowGuardGaps Gp = ftcore::uiLowGuardGaps(S, Mesh.PxSize());
			const float UY = Top + Dot + Gp.gap + 0.5f * Gp.under;
			ValueLine(X0, X0 + Len, UY, 0.f, 1.f, ftcore::kUiLowUnderW, 209, S, U, 0.f);
		}
	}

	// The margin rule (signature device 3, Shown().bSigMarginRule; off by default): a vertical stroke margin_rule.width
	// px wide at margin_rule.alpha of ink, margin_rule.gap px left of the lines' start, from Bottom up to Top (the cap top
	// of the highest shown name, which eases with the rows). One fill, no motion of its own.
	void MarginRule(float X0, float Top, float Bottom, float S, float A) const
	{
		const ftcore::UiMarginRule& M = Table().Style.marginRule;
		const float W = Mesh.WholeDev(M.width * S);
		const float RX = Mesh.SnapX(X0 - M.gap * S - W);
		if (Bottom <= Top || A <= 0.f)
		{
			return;
		}
		const FVector2f Pts[4] = {FVector2f(RX, Top), FVector2f(RX + W, Top), FVector2f(RX + W, Bottom), FVector2f(RX, Bottom)};
		Mesh.Fill(Pts, 4, Tok(UiToken::Ink, M.alpha * A));
	}

	// Width px at 1080p (at scale S) as a whole number of device pixels, still in px at 1080p: 1 px is 1.33 at 1440p and
	// would blur, so it draws as one device pixel there, and 3 px as four.
	float DevWidth(float Width, float S) const { return Mesh.WholeDev(Width * S) / FMath::Max(S, 0.01f); }

	// A value line's middle moved so its top and bottom edges fall on device pixel boundaries: its full width then covers
	// whole rows, never a soft half row each side.
	float SnapLine(float Y, float Width, float S) const { return Mesh.SnapLineY(Y, Mesh.WholeDev(Width * S)); }

	// A whole ring, joined without a seam: a true circle (no hand wobble, a point every 1.5 px, so its edge never steps
	// at 1080p) with a crisp 1 px dark edge right round its outside, so it holds on a mid-tone.
	void Ring(float CX, float CY, float R, float Width, std::uint32_t Seed, float S, const FLinearColor& C, const FLinearColor* Rim = nullptr) const
	{
		ftcore::UiStrokeStyle St = StrokeStyle(Width, Seed, S, 0.f);
		St.wobble = 0.f;
		St.step = 1.5f * S;
		ftcore::uiStrokeArc(Line, CX, CY, R, 0.f, 360.f, St);
		if (Line.count > 1)
		{
			Line.pt[Line.count - 1].x = Line.pt[0].x;
			Line.pt[Line.count - 1].y = Line.pt[0].y;
		}
		Mesh.Stroke(Line, C, true, Rim, true);
	}

	void Poly(const FVector2f* Pts, int32 N, float HalfWidth, const FLinearColor& C, bool bClosed = false) const
	{
		PolyStroke(Line, Pts, N, HalfWidth);
		Mesh.Stroke(Line, C, bClosed);
	}

	// The call cue's danger triangle, Size px across (15 px at 1080p): a small mark, so in a world whose marks wear accent2
	// it is accent2 with the ink rim like the others.
	void Danger(float CX, float CY, float Size, float A) const
	{
		const FUiWorldRow& R = Shown();
		FLinearColor C = R.CueMark;
		C.A *= A;
		FLinearColor Rim = R.Token[static_cast<size_t>(UiToken::Ink)];
		Rim.A *= A;
		const FVector2f Pts[3] = {FVector2f(CX, CY - Size * 0.55f), FVector2f(CX + Size * 0.55f, CY + Size * 0.45f), FVector2f(CX - Size * 0.55f, CY + Size * 0.45f)};
		Mesh.Fill(Pts, 3, C, R.bMarksAccent2 ? &Rim : nullptr);
	}

	// Low is not told by colour alone: the low colour, and a pulse between 60 and 100 percent at 1 Hz.
	float LowPulse() const { return 0.8f + 0.2f * FMath::Cos(UE_TWO_PI * Time); }

	// A value line that turns low (uiDangerSet): ink when not low. Low, in every world a 2 px line in the low colour the
	// track's full length carries the pulse, 5 px below the line's bottom edge (one geometry for the family); the line
	// itself takes the world's low ink: the low colour pulsing, or, where the world keeps red to marks and underlines,
	// ink at full strength.
	void LowValueLine(float X0, float X1, float Y, float To01, float Width, std::uint32_t Seed, float S, bool bLow, float A, float Taper) const
	{
		const FUiWorldRow& R = Shown();
		if (!bLow)
		{
			ValueLine(X0, X1, Y, 0.f, To01, Width, Seed, S, FillTok(A), Taper);
			return;
		}
		FLinearColor C = R.LowLine;
		C.A *= A * (R.bLowUnder ? 1.f : LowPulse());
		ValueLine(X0, X1, Y, 0.f, To01, Width, Seed, S, C, Taper);
		{
			FLinearColor U = R.LowUnder;
			U.A *= A * LowPulse();
			const float Bottom = SnapLine(Y, Width, S) + 0.5f * Mesh.WholeDev(Width * S);
			const ftcore::UiLowGuardGaps G = ftcore::uiLowGuardGaps(S, Mesh.PxSize());
			const float UY = Bottom + G.gap + 0.5f * G.under;
			ValueLine(X0, X1, UY, 0.f, 1.f, ftcore::kUiLowUnderW, Seed + 7u, S, U, 0.f);
		}
	}

	// A danger word (uiDangerWord) with its top at Top (Asc its face's ascent, W its measured width), the glyphs at A. Where
	// the world underlines danger words the glyphs are ink and a 1 px danger line runs under their width, just below the
	// baseline; Pulse then moves from the glyphs onto that line. Elsewhere the glyphs are danger at A times Pulse.
	void DangerText(const FPainter& P, float X, float Top, float Asc, float W, const FString& Text, const FSlateFontInfo& Font, float A,
		float Pulse = 1.f) const
	{
		const FUiWorldRow& R = Shown();
		FLinearColor C = R.DangerWord;
		C.A *= A * (R.bDangerUnderline ? 1.f : Pulse);
		P.Text(X, Top, Text, Font, C);
		if (R.bDangerUnderline && W > 0.f)
		{
			FLinearColor L = R.DangerLine;
			L.A *= A * Pulse;
			const float Base = Top + Asc;
			ValueLine(X, X + W, Base + FMath::Max(2.f * P.S, 0.12f * Asc), 0.f, 1.f, Hud::DangerUnderW, 151, P.S, L, 0.f);
		}
	}

	// An hp line: the hp in the stroke over a hairline track (the track only where the hp is not, so it never reads as a
	// second line), what was just lost rubbed out to a broken trace, low under a quarter.
	// In every world the track is the line's own stroke the full length (TrackLine), the value over it at full ink from
	// the same left edge as the name.
	// Segs (the lead's line only): each blow's lost hp drawn solid at the line's width in the hit cue's red over the
	// trace, its far end draining back to its near end over 350 ms (uiHitSegsStep), one segment per blow whatever the
	// glow's cap.
	// bCap: the end cap on the hp's end (the lead's and the partner's lines; a minor enemy's line has none).
	void HpLine(float X0, float Len, float Y, float Hp, float Was, float Width, std::uint32_t Seed, float S, float A,
		const ftcore::UiHitSegs* Segs = nullptr, bool bCap = false) const
	{
		const float YS = SnapLine(Y, Width, S); // the track and the rub on the value line's own middle
		TrackLine(X0, X0 + Len, YS, Width, Seed + 1u, S, A, Hp);
		LowValueLine(X0, X0 + Len, YS, Hp, Width, Seed + 1u, S, Hp < Hud::LowHp, A, Hud::ValueTaper);
		if (Was > Hp + 0.004f)
		{
			ftcore::UiStrokeStyle Rub = StrokeStyle(1.6f, Seed + 2u, S, 0.f);
			Rub.broken = true;
			HLine(X0, X0 + Len, YS, Hp, Was, Rub, Tok(UiToken::Ink, 0.4f * A));
		}
		if (Segs != nullptr)
		{
			FLinearColor Red = Shown().HitCue;
			Red.A *= A;
			bool bMeetsFill = false;
			for (const ftcore::UiHitSeg& G : Segs->seg)
			{
				const float K = ftcore::uiHitFlashLevel(G.age);
				const float Lo = FMath::Max(G.lo, Hp);
				const float Hi = FMath::Min(G.hi, G.lo + (G.hi - G.lo) * K);
				if (K > 0.f && Hi > Lo + 0.002f)
				{
					ValueLine(X0, X0 + Len, YS, Lo, Hi, Width, Seed + 3u, S, Red, 0.f);
					if (Lo <= Hp + 0.002f) bMeetsFill = true;
				}
			}
			// Red never meets the fill edge to edge (2026-11-16 ruling): a 1 device px ink separator, the line's
			// height, on the fill's end wherever a red segment starts there (under the end cap where one is drawn).
			if (bMeetsFill && Hp > 0.002f)
			{
				const float WD = Mesh.WholeDev(Width * S);
				const float SX = Mesh.SnapX(X0 + Len * Hp);
				const float Top = Mesh.SnapLineY(YS, WD) - 0.5f * WD;
				Mesh.Rect(SX, Top, SX + Mesh.PxSize(), Top + WD, Tok(UiToken::Ink, A), true);
			}
		}
		if (bCap)
		{
			EndCap(X0, Len, YS, Hp, Width, S, A);
		}
	}

	static float KeyCapW(float KeyW, float S) { return FMath::Max(Hud::KeyCapH * S, KeyW + Hud::KeyCapPad * S); }

	// A key cap: a 1 px rounded outline round its letters, centred on (CX, MidY). Every key on the HUD is this one cap.
	// Ring: the cap's outline where it differs from its letters (the prompts': the world's accent).
	void KeyCap(const FPainter& P, float CX, float MidY, float W, const FString& Key, float KeyW, const FLinearColor& C,
		const FLinearColor* CapRing = nullptr) const
	{
		KeyCapStroke(Line, CX, MidY, W, Hud::KeyCapH * P.S, 0.5f * P.S, Hud::KeyCapCorner * P.S);
		Mesh.Stroke(Line, CapRing ? *CapRing : C, true);
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
	// The party screen opens the block in place: each name rises PartyLift over its line (the lines, the live read-out,
	// stay), the partner's row stands even with no partner (NOT HERE), and the Third's row comes in PartyRowGap px over
	// the partner's capitals.
	struct FPartyRows
	{
		float GuardBase = 0.f, HpY = 0.f, NameBase = 0.f, PY = 0.f, PBase = 0.f, TBase = 0.f;
		float LoadTop = 0.f; // the Load line's top rim: the name's rest baseline, which the lines keep on the party screen
		bool bPartner = false;
		bool bThird = false; // the party screen's Third row (open, with a gift slot)
		float Open = 0.f;    // the screen's opening, eased (0 shut, 1 open)
		float Top = 0.f; // the block's top, without a hit's dip: the higher name's box
	};

	FPartyRows PartyRows(const FVector2f& Size, float S) const
	{
		FPartyRows R;
		const float GuardBase0 = Size.Y - EdgesAt(Size, S).bottom;
		// The guard row's top (its line and its word's caps) stands kUiGuardClear under the low hp line's underline, in
		// whole device pixels and never under 10 of them (Core's uiLowGuardGaps).
		const ftcore::UiLowGuardGaps G = ftcore::uiLowGuardGaps(S, Mesh.PxSize());
		const float HpY0 = GuardBase0 - 0.62f * NoteAsc - (G.gap + G.under + G.clear) - 0.5f * Mesh.WholeDev(Hud::LeadHpW * S);
		const float NameBase0 = HpY0 - (0.5f * Hud::LeadHpW + 12.f) * S;
		const float PY0 = NameBase0 - 0.62f * LeadNameAsc - (22.f + 0.5f * Hud::PartnerHpW) * S; // the partner's line's middle
		const float SwapDy = FMath::SmoothStep(0.f, 1.f, RowSwap.left01) * (PY0 - HpY0);
		R.Open = FMath::SmoothStep(0.f, 1.f, PartyOpenK);
		const float Lift = Hud::PartyLift * S * R.Open;
		R.GuardBase = GuardBase0 + SwapDy;
		R.HpY = HpY0 + SwapDy;
		const float NameRest = NameBase0 + SwapDy - Lift;
		// A hit drops the name, its slot and the Load line under it. The name and the slot words stand LoadLift px over
		// the Load line's top rim; the party screen lifts the name off the line, which stays.
		R.LoadTop = NameBase0 + SwapDy + 3.f * S * LeadDip;
		R.NameBase = NameRest - Hud::LoadLift * S + 3.f * S * LeadDip;
		const ftcore::ArenaHudPartner& Pa = View.now.partner;
		R.bPartner = (Pa.present && Budget.out.allowed(ftcore::UiMarkKind::Party) > 0) || R.Open > 0.f;
		R.PY = PY0 - SwapDy; // 22 px over the lead's name at rest (the lead's dip does not move it)
		const float PRest = R.PY - (0.5f * Hud::PartnerHpW + 8.f) * S - Lift; // the partner's name, 8 px over its line
		R.PBase = PRest + 3.f * S * PartnerDip;
		R.bThird = R.Open > 0.f && bThirdRow;
		R.TBase = PRest - 0.62f * PartnerNameAsc - Hud::PartyRowGap * S;
		R.Top = NameRest - Hud::LoadLift * S - LeadNameAsc;
		if (R.bPartner)
		{
			R.Top = FMath::Min(R.Top, PRest - PartnerNameAsc);
		}
		if (R.bThird)
		{
			R.Top = FMath::Min(R.Top, FMath::Lerp(PRest - PartnerNameAsc, R.TBase - PartnerNameAsc, R.Open));
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

		HpLine(X0, Len, HpY, Frac(L.hp, L.hpMax), HpLost.Was, Hud::LeadHpW, 201, S, A, bNoHitRed ? nullptr : &HitSegs, true);
		if (Shown().bSigMarginRule)
		{
			MarginRule(X0, Rows.Top, GuardBase, S, A);
		}
		if (GuardShown())
		{
			// Only while under full: the hp line's family, the same left edge, stroke, track and taper at GuardW, GUARD at its
			// right end in the labels' size, as POISE sits by its line, for 2 s after the line appears or its state changes
			// (ftcore::uiGuardLabelStep), then gone: steady play shows the bare line.
			const float G = FMath::Clamp(L.stamina01, 0.f, 1.f);
			const float GY = CapMid(GuardBase, NoteAsc);
			GuardDots(X0, Len, GY, G, S, G < Hud::LowHp, A);
			if (GuardLabelA > 0.f)
			{
				P.Text(X0 + Len + Hud::LabelGap * S, CapTop(GY, NoteAsc), GuardWord, NoteFont, Tok(UiToken::Ink, Hud::NoteInk * A * GuardLabelA));
			}
		}
		if (L.debt >= 0)
		{
			if (L.debt >= 80)
			{
				// The debt shares the figure's column with LOAD, so it takes the figure's turn in it (uiLabelColumnStep).
				DangerText(P, X0 + Len + 12.f * S, CapTop(HpY, NoteAsc), NoteAsc, DebtW, DebtText, NoteFont, Hud::NoteInk * A * HpFigA, LowPulse());
			}
			else
			{
				P.Text(X0 + Len + 12.f * S, CapTop(HpY, NoteAsc), DebtText, NoteFont, Tok(UiToken::Ink, Hud::NoteInk * A * HpFigA));
			}
		}
		else if (!HpFigText.IsEmpty())
		{
			// The hp figure, FigGap px after the line, right-aligned to its slot's fixed right edge, centred on the line. It
			// shares its column with LOAD over it, so it is out while LOAD's once-a-fight label shows at all, and comes back
			// once LOAD has gone, each with a gap (uiLabelColumnStep): never the two over each other.
			const float FigA = A * HpFigA;
			if (FigA > 0.f)
			{
				const float Right = X0 + Len + Hud::FigGap * S + HpFigSlotW;
				P.Text(Right - HpFigW, CapTop(HpY, FigAsc), HpFigText, FigFont, Tok(UiToken::Ink, FigA));
			}
		}

		// The name in ink in every world (on the party screen its row's ink: full on the cursor, kUiPartyOtherInk off it),
		// LoadLift px over the Load line: the value-line family at LoadW the hp line's length, gold (St.loadGold, hard edge,
		// square ends) to load / loadCap, and a 1 device px tick at the Release threshold, light until the Release opens and
		// dark once it is. LOAD at the line's right end while uiLoadLabelStep keeps it.
		const float K = Rows.Open;
		const ftcore::UiPartyRowView LeadView = ftcore::uiPartyRowView(PartyDrawn, ftcore::UiPartyRow::Lead, ftcore::UiPartyWhy::Ok, LeadNameW);
		const float NameTop = NameBase - LeadNameAsc;
		const float NameA = FMath::Lerp(1.f, LeadView.ink, K) * A;
		P.Text(X0, NameTop, LeadName, LeadNameFont, Tok(UiToken::Ink, NameA));
		const FLinearColor Gold = LinOf(St.loadGold, A);
		if (L.loadMax > 0 && L.loadCap > 0)
		{
			const float WD = Mesh.WholeDev(Hud::LoadW * S);
			const float LY = Rows.LoadTop + Mesh.PxSize() + 0.5f * WD; // the body's middle: its top rim on LoadTop
			const float YS = SnapLine(LY, Hud::LoadW, S);
			TrackLine(X0, X0 + Len, YS, Hud::LoadW, 221, S, A, LoadFill);
			ValueLine(X0, X0 + Len, YS, 0.f, LoadFill, Hud::LoadW, 221, S, Gold, 0.f);
			bool bBreaks = Shown().bLoadBreaks;
#if !UE_BUILD_SHIPPING
			// -UiLoadOld (captures): the Load without its breaks, for the side-by-side with the current look.
			static const bool bLoadOld = FParse::Param(FCommandLine::Get(), TEXT("UiLoadOld"));
			bBreaks = bBreaks && !bLoadOld;
#endif
			if (bBreaks && LoadFill > 0.f)
			{
				// The breaks (uiLoadBreaks): kUiLoadBreakW px of the world's backing across the gold's body every
				// kUiLoadBreakPitch px from the line's start, clear of the cap and the Release tick. A fixed buffer.
				float Bx[64];
				const float TickX = L.releaseAt > 0 && L.releaseAt < L.loadCap
					? X0 + Len * static_cast<float>(L.releaseAt) / static_cast<float>(L.loadCap) : std::numeric_limits<float>::quiet_NaN();
				const int N = ftcore::uiLoadBreaks(X0, Len * LoadFill, ftcore::kUiLoadBreakPitch * S, TickX, ftcore::kUiLoadBreakKeep * S, Bx, 64);
				const float BW = Mesh.WholeDev(ftcore::kUiLoadBreakW * S);
				const float BTop = Mesh.SnapLineY(YS, WD) - 0.5f * WD;
				FLinearColor Notch = Shown().TrackBody;
				Notch.A = A;
				for (int i = 0; i < N; ++i)
				{
					const float BL = Mesh.SnapX(Bx[i] - 0.5f * BW);
					Mesh.Rect(BL, BTop, BL + BW, BTop + WD, Notch, true);
				}
			}
			EndCap(X0, Len, YS, LoadFill, Hud::LoadW, S, A);
			if (L.releaseAt > 0 && L.releaseAt < L.loadCap && Shown().bSigPulse)
			{
				// Signature device 1: the tick becomes one heartbeat spike in the Load stroke (a dip, a tall peak, a trough,
				// back to the line; 14 px wide, 7 over, 5 under), in the tick's colours. It moves only on a change: the Release
				// opening sends one pulse along the line to it over kUiReleaseTravel s and its peak grows kUiReleaseGrow for
				// kUiReleasePeak s; overloaded, that peak repeats every kUiReleaseRepeat s (uiReleasePulseStep).
				const float TX = X0 + Len * static_cast<float>(L.releaseAt) / static_cast<float>(L.loadCap);
				const float V = L.releaseReady ? Hud::LoadTickDark : Hud::LoadTickLight;
				const float Pk = ReleasePulseV.peak;
				const FVector2f Spike[7] = {FVector2f(TX - 7.f * S, YS), FVector2f(TX - 5.f * S, YS), FVector2f(TX - 3.f * S, YS + 2.f * S),
					FVector2f(TX - 1.f * S, YS - 7.f * S * Pk), FVector2f(TX + 2.f * S, YS + 5.f * S), FVector2f(TX + 4.f * S, YS),
					FVector2f(TX + 7.f * S, YS)};
				Poly(Spike, 7, 0.5f * WD, LinOf(ftcore::UiColor{V, V, V, 1.f}, A));
				if (ReleasePulseV.travel >= 0.f)
				{
					const float PX = FMath::Lerp(X0, TX, FMath::Clamp(ReleasePulseV.travel, 0.f, 1.f));
					Mesh.Dot(PX, YS, WD, LinOf(St.loadBright, A));
				}
			}
			else if (L.releaseAt > 0 && L.releaseAt < L.loadCap)
			{
				// The Release tick, ReleaseTickW by ReleaseTickH, centred on the line at the threshold.
				const float TW = Mesh.WholeDev(Hud::ReleaseTickW * S);
				const float TH = Mesh.WholeDev(Hud::ReleaseTickH * S);
				const float TX = Mesh.SnapX(X0 + Len * static_cast<float>(L.releaseAt) / static_cast<float>(L.loadCap) - 0.5f * TW);
				const float V = L.releaseReady ? Hud::LoadTickDark : Hud::LoadTickLight;
				const float TTop = Mesh.SnapLineY(YS, TH) - 0.5f * TH;
				Mesh.Rect(TX, TTop, TX + TW, TTop + TH, LinOf(ftcore::UiColor{V, V, V, 1.f}, A), true);
			}
			if (LoadLabelA > 0.f)
			{
				P.Text(X0 + Len + Hud::LabelGap * S, CapTop(YS, NoteAsc), LoadWord, NoteFont, Tok(UiToken::Ink, Hud::NoteInk * A * LoadLabelA));
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
			break; // the Load line and its own LOAD label tell it
		case ftcore::UiLeadWord::Release:
		{
			// The Load's gold in every world, as the line's fill.
			const FLinearColor RelC = Gold;
			P.Text(SlotX, NameBase - NoteAsc, ReleaseWord, NoteFont, RelC);
			if (!ReleaseKey.IsEmpty())
			{
				const float CapW = KeyCapW(ReleaseKeyW, S);
				KeyCap(P, SlotX + ReleaseWordW + 8.f * S + CapW * 0.5f, CapMid(NameBase, NoteAsc), CapW, ReleaseKey, ReleaseKeyW, RelC);
			}
			break;
		}
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
		// The party screen's marks on the controlled lead's row: the cursor's underline across the name when it rests
		// there, and the dot before the name always.
		if (K > 0.f)
		{
			PartyMarks(Mesh, Line, LeadView, X0, NameBase, LeadNameAsc, S, K, A);
		}

		// The partner's row, whenever the fight has a partner, and on the party screen always. Open, it takes its row's
		// ink (Core's): full on the cursor with the underline across the name, kUiPartyOtherInk off it; a switch the rules
		// would refuse now dims the row to PartyRefusedInk with the reason after the name at label size (NOT HERE with no
		// partner at all), the cursor's underline still under the name. Never gold.
		if (!bPartner)
		{
			return;
		}
		const ftcore::UiPartyWhy PWhy = Pa.present ? PartnerWhy : ftcore::UiPartyWhy::NotHere;
		const bool bPRefused = K > 0.f && ftcore::uiPartyRefused(PWhy);
		const ftcore::UiPartyRowView PView = ftcore::uiPartyRowView(PartyDrawn, ftcore::UiPartyRow::Partner, PWhy, PartnerNameW);
		const float PInk = FMath::Lerp(Hud::PartnerInk, PView.ink, K);
		if (Pa.present)
		{
			HpLine(X0, PLen, PY, Frac(Pa.hp, Pa.hpMax), PartnerLost.Was, Hud::PartnerHpW, 211, S,
				A * (bPRefused ? FMath::Lerp(1.f, Hud::PartyRefusedInk, K) : 1.f), nullptr, true);
		}
		P.Text(X0, PBase - PartnerNameAsc, PartnerName, PartnerNameFont, Tok(UiToken::Ink, PInk * A));
		const float PSlotX = X0 + PartnerNameW + Hud::PartnerSlotGap * S;
		if (bPRefused)
		{
			const int32 Wi = static_cast<int32>(PWhy);
			P.Text(PSlotX, PBase - NoteAsc, PartyWhyWord[Wi], NoteFont, Tok(UiToken::Ink, Hud::PartyRefusedInk * K * A));
		}
		if (K > 0.f)
		{
			PartyMarks(Mesh, Line, PView, X0, PBase, PartnerNameAsc, S, K, A);
		}
		// No SHARED tag: the partner's hp line tells a shared blow.

		// The Third's row, on the party screen with a gift slot: its name at the partner's size at kUiPartyThirdInk, no hp
		// line, never the cursor; its gift line after it at label size.
		if (Rows.bThird)
		{
			const ftcore::UiPartyRowView TView = ftcore::uiPartyRowView(PartyDrawn, ftcore::UiPartyRow::Third, ftcore::UiPartyWhy::Unknown, ThirdNameW);
			const FLinearColor TC = Tok(UiToken::Ink, TView.ink * K * A);
			P.Text(X0, Rows.TBase - PartnerNameAsc, ThirdName, PartnerNameFont, TC);
			if (!ThirdText.IsEmpty())
			{
				P.Text(X0 + ThirdNameW + 16.f * S, Rows.TBase - NoteAsc, ThirdText, NoteFont, TC);
			}
		}
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

	// The top: the boss's hp in the value-line family (TrackLine's black body and top hairline the full length, the hp
	// over it in ink to a hard edge, square ends, no hook) with phase notches, the stagger's break and the poise under it;
	// a plain target's line alone. The name over it in spaced small caps.
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

		const float HpW = B.present ? 5.f : 3.8f;
		const float YL = SnapLine(Y, HpW, S); // the track and the rub on the value line's own middle
		TrackLine(X0, X0 + Len, YL, HpW, 99, S, A, Hp);
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
			ValueLine(X0, X0 + Len, YL, 0.f, FMath::Max(BreakAt - Gap, 0.f), HpW, 101, S, FillTok(A));
			ValueLine(X0, X0 + Len, YL + 6.f * S * BreakK, BreakAt + Gap, Hp, HpW, 101, S, FillTok(A));
			EndCap(X0, Len, YL + 6.f * S * BreakK, Hp, HpW, S, A);
		}
		else
		{
			ValueLine(X0, X0 + Len, YL, 0.f, Hp, HpW, 101, S, FillTok(A));
			EndCap(X0, Len, YL, Hp, HpW, S, A);
		}
		// The name's and the percent's baseline: FigLift px over the line's top.
		const float NameBaseY = YL - 0.5f * Mesh.WholeDev(HpW * S) - Hud::FigLift * S;
		if (B.present && !BossPctText.IsEmpty())
		{
			P.Text(X0 + Len - BossPctW, NameBaseY - FigAsc, BossPctText, FigFont, Tok(UiToken::Ink, TA));
		}
		if (Shown().bSigMarginRule)
		{
			// The margin rule's tick on the top line's left end (signature device 3): margin_rule.tick_w by tick_h, centred.
			const ftcore::UiMarginRule& M = Table().Style.marginRule;
			const float TW = Mesh.WholeDev(M.tickW * S);
			const float TH = Mesh.WholeDev(M.tickH * S);
			const float TX = Mesh.SnapX(X0 - 0.5f * TW);
			const float TT = Mesh.SnapLineY(YL, TH) - 0.5f * TH;
			const FVector2f Pts[4] = {FVector2f(TX, TT), FVector2f(TX + TW, TT), FVector2f(TX + TW, TT + TH), FVector2f(TX, TT + TH)};
			Mesh.Fill(Pts, 4, Tok(UiToken::Ink, M.alpha * A));
		}
		if (TopLost.Was > Hp + 0.004f)
		{
			ftcore::UiStrokeStyle Rub = StrokeStyle(1.6f, 101, S, 0.f);
			Rub.broken = true;
			HLine(X0, X0 + Len, YL, Hp, TopLost.Was, Rub, Tok(UiToken::Ink, 0.4f * A));
		}
		if (bNamed)
		{
			P.Text(CX - NameW * 0.5f, NameBaseY - NameAsc, NameText, NameFont, Tok(UiToken::Ink, TA));
		}
		if (B.present && B.weight > 0)
		{
			// The fight's own gauge beside the name: WEIGHT and a dot per stack of the cap, pulsing from three.
			const float MidY = CapMid(NameBaseY, NameAsc);
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
					Danger(TX - 20.f * S, MidY, 15.f * S, TA);
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
		// (the call word, a Qte, the Read row), so nothing live moves for it. A refused lead switch's SWITCH: <WORD> takes
		// the same place for kUiPartySwitchShow.
		const auto TooSoon = [&](float At)
		{
			if (bSwitchOn)
			{
				P.Text(CX - SwitchMid, At, SwitchText, CallFont, Tok(UiToken::Ink));
			}
			else if (bTooSoonOn)
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
		const float OYL = SnapLine(OY, Hud::FoeHpW, S);
		HLine(OX, OX + OLen, OYL, OHp, 1.f, StrokeStyle(1.f, 105, S, 0.f), Tok(UiToken::Ink, 0.3f * A));
		ValueLine(OX, OX + OLen, OYL, 0.f, OHp, Hud::FoeHpW, 106, S, Tok(UiToken::Ink, A));
		ValueLine(OX, OX + OLen, OY + 10.f * S, 0.f, FMath::Clamp(T.poise01, 0.f, 1.f), Hud::PoiseW, 107, S, Tok(UiToken::Ink, 0.7f * A));
	}

	// The prompts, bottom right: each in its own row counted up from the bottom margin, which it never leaves (Core's
	// uiPromptRow: HEAL, GIFT, PARTNER, then the basic controls ATTACK, GUARD, DODGE and JUMP); right-aligned, its word
	// and its key cap. Every prompt whose action is in this fight stands in its row (Core's uiPromptsPresent, the basic
	// controls as the player's teaching policy keeps them, uiPromptsTaught, then the grant); one is gone when its action
	// is not in this fight (no gift slot, a move the kit lacks), or, for a basic control, the policy hides it, or while its
	// press would be refused now. A press of a refused prompt's key shows its row for kUiRefusedShow s at kUiRefusedInk,
	// its key cap too, its reason word (the rules' PressReason: COOLDOWN, FULL, CLOSED...) after its word, before its key
	// cap, at label size (Core's uiRefusedStep); one whose action the import left unbound shows NO KEY in its key box, at
	// the labels' 80 percent ink. Nothing behind them but the bottom
	// shade. A gift row is ink like the rest: gold is the lead's own power and a gift is not. The GIFT row is the one that
	// may hold two: with both gifts GIFT GUARD keeps the margin and GIFT HEAL stands to its left, GiftGap px from GIFT
	// GUARD's word (or its reason); either alone takes the margin.
	void PaintPrompts(const FPainter& P, const FVector2f& Size) const
	{
		const bool bSpecialRow = ::Shown().bSpecialPrompt; // the special's row reserved: the basic controls stand one row up
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
		// The r19c ledger column: key cap, then verb, then count. The caps right-aligned in their column, the verbs left-
		// aligned after them, the counts (figures) right-aligned in a column CountGap px after the longest verb; the block's
		// right edge on the margin. A refused row's reason word stands left of its cap, so the column never moves. GIFT
		// HEAL sharing the GIFT row with GIFT GUARD stands left of the block, GiftGap px from it (or from GIFT GUARD's
		// reason), as its own cap, verb and count.
		constexpr int32 GH = static_cast<int32>(ftcore::UiPrompt::GiftHeal);
		constexpr int32 GG = static_cast<int32>(ftcore::UiPrompt::GiftGuard);
		constexpr int32 HH = static_cast<int32>(ftcore::UiPrompt::Heal);
		const auto Drawn = [&](int32 I) { return ((Shown >> I) & 1u) && !PromptKey[I].IsEmpty(); };
		const auto CountOf = [this](int32 I) -> const FString* { return I == HH ? &HealText : I == GH ? &GiftHealText : nullptr; };
		const auto CountW = [this](int32 I) { return I == HH ? HealW : I == GH ? GiftHealW : 0.f; };
		const bool bGiftPair = Drawn(GH) && Drawn(GG);
		float KeyColW = 0.f;
		float VerbColW = 0.f;
		float CountColW = 0.f;
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiPromptCount) && Shown != 0; ++I)
		{
			if (!Drawn(I) || (bGiftPair && I == GH))
			{
				continue;
			}
			KeyColW = FMath::Max(KeyColW, KeyCapW(PromptKeyW[I], S));
			VerbColW = FMath::Max(VerbColW, PromptWordW[I]);
			const FString* Count = CountOf(I);
			if (Count && !Count->IsEmpty())
			{
				CountColW = FMath::Max(CountColW, CountW(I));
			}
		}
		const float BX = RX - (KeyColW + Gap + VerbColW + (CountColW > 0.f ? Hud::CountGap * S + CountColW : 0.f));
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiPromptCount) && Shown != 0; ++I)
		{
			if (!Drawn(I))
			{
				continue; // nothing reaches it on this device: not drawn
			}
			const FLinearColor C = ((RefusedShowing >> I) & 1u) ? Tok(UiToken::Ink, ftcore::kUiRefusedInk)
				: ((PromptDimmed >> I) & 1u)                       ? Tok(UiToken::Ink, ftcore::kUiRefusedInk)
																	: Tok(UiToken::Ink);
			const FLinearColor CapRing = Tok(UiToken::Accent, C.A);
			const float Row = static_cast<float>(ftcore::uiPromptRowAt(static_cast<ftcore::UiPrompt>(I), bSpecialRow));
			const float MidY = Size.Y - E.bottom - (0.5f * Hud::KeyCapH + Row * Hud::PromptStep) * S;
			const float CapW = KeyCapW(PromptKeyW[I], S);
			const FString* Count = CountOf(I);
			const bool bCount = Count && !Count->IsEmpty();
			float CapRight = BX + KeyColW;
			if (bGiftPair && I == GH)
			{
				const int32 GuardWhy = WhyOf(GG);
				const float GroupRight = BX + KeyColW - KeyCapW(PromptKeyW[GG], S) - (GuardWhy >= 0 ? Gap + WhyW[GuardWhy] : 0.f) - Hud::GiftGap * S;
				CapRight = GroupRight - (PromptWordW[I] + Gap + (bCount ? Gap + CountW(I) : 0.f));
			}
			KeyCap(P, CapRight - CapW * 0.5f, MidY, CapW, PromptKey[I], PromptKeyW[I], C, &CapRing);
			const float VX = CapRight + Gap;
			P.Text(VX, CapTop(MidY, PromptAsc), PromptWord[I], PromptFont, C);
			if (bCount)
			{
				const float CX = (bGiftPair && I == GH) ? VX + PromptWordW[I] + Gap : RX - CountW(I);
				P.Text(CX, CapTop(MidY, FigAsc), *Count, FigFont, C);
			}
			const int32 WhyAt = WhyOf(I);
			if (WhyAt >= 0)
			{
				P.Text(CapRight - CapW - Gap - WhyW[WhyAt], CapTop(MidY, NoteAsc), WhyWord[WhyAt], NoteFont, C);
			}
		}
	}

	// The special prompt (Core uiSpecialStep, the world's special_prompt switch): its own row, kUiSpecialRow up the prompt
	// stack, reserved while the switch is on so nothing moves when it appears. Right-aligned on the margin, outside the
	// ledger column (so the column never widens when it comes): the key cap, the word SPECIAL, then the lead's name in
	// ink_dim, or, while it cannot start, the reason; with only the Third's ready, CALL and his current name. Live: two outlines round the key (the second kUiSpecialRingGap px
	// out, always closed), full ink, both outlines in ink (never the accent: Solhaven's is the Load gold). Dim: one
	// outline, ink_dim. The pulse: the outer outline grows kUiSpecialPulseGrow px and fades, once, on arrival.
	// Not a meter: nothing here has a level; the view gives only drawn, live, a reason, the fade and the pulse clock.
	void PaintSpecial(const FPainter& P, const FVector2f& Size) const
	{
		if (!Special.drawn || SpecialKey.IsEmpty())
		{
			return;
		}
		const float S = P.S;
		const ftcore::UiMargins E = EdgesAt(Size, S);
		const float RX = Size.X - E.right;
		const float Gap = 10.f * S;
		const float A = Special.alpha;
		const FLinearColor Ink = Special.live ? Tok(UiToken::Ink, A) : Tok(UiToken::InkDim, A);
		const FLinearColor Dim = Tok(UiToken::InkDim, A);
		const float MidY = Size.Y - E.bottom - (0.5f * Hud::KeyCapH + static_cast<float>(ftcore::kUiSpecialRow) * Hud::PromptStep) * S;
		const float TailX = RX - SpecialTailW;
		P.Text(TailX, CapTop(MidY, PromptAsc), SpecialTail, PromptFont, Dim);
		const float WordX = TailX - Gap - (Special.call ? CallWordW : SpecialWordW);
		P.Text(WordX, CapTop(MidY, PromptAsc), Special.call ? CallWord : SpecialWord, PromptFont, Ink);
		const float CapW = KeyCapW(SpecialKeyW, S);
		const float CX = WordX - Gap - CapW * 0.5f;
		KeyCap(P, CX, MidY, CapW, SpecialKey, SpecialKeyW, Ink, &Ink);
		if (!Special.live)
		{
			return;
		}
		const float Out = ftcore::kUiSpecialRingGap * S;
		const float H = Hud::KeyCapH * S;
		KeyCapStroke(Line, CX, MidY, CapW + 2.f * Out, H + 2.f * Out, 0.5f * S, (Hud::KeyCapCorner + ftcore::kUiSpecialRingGap) * S);
		Mesh.Stroke(Line, Ink, true);
		if (Special.pulse >= 0.f)
		{
			const float Grow = Out + ftcore::kUiSpecialPulseGrow * S * Special.pulse;
			KeyCapStroke(Line, CX, MidY, CapW + 2.f * Grow, H + 2.f * Grow, 0.5f * S, Hud::KeyCapCorner * S + Grow);
			Mesh.Stroke(Line, Tok(UiToken::Ink, A * 0.85f * (1.f - Special.pulse)), true);
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
		if (B.present && (B.weakPoint == ftcore::UiWeakPoint::Open || UiStateShot() == EUiStateShot::Marks))
		{
			FVector Weak;
			bWeakOn = Runner.GetBossWeakPoint(static_cast<int32>(B.slot), Weak) && Project(PC, Weak, Size, WeakAt) && OnScreen(WeakAt);
			if (!bWeakOn && UiStateShot() == EUiStateShot::Marks)
			{
				// The marks' still on a stage whose stand-in boss has no weak point in view: the mark on its chest, so every
				// stage's still shows one.
				const AFTArenaFighter* Figure = Runner.GetEnemyFigure(static_cast<int32>(B.slot));
				bWeakOn = Figure && Project(PC, Figure->GetChest(), Size, WeakAt) && OnScreen(WeakAt);
			}
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
		{
			// The hit cue: full for 100 ms, gone by 350 ms (uiHitFlashLevel), in every world, at most three a second.
			// Aged on the fight's clock (the runner's Now: a second paint at the same time ages nothing; Slate's own delta is
			// wall time and ran ahead of the fight under a fixed step) from the tick the runner's hit stamp moves later. The
			// clock holds in a pause, so the cue holds too and is hidden while the game is paused; a retry, which resets the
			// stamp, shows none (uiHitWatchStep). The struck side is taken when the cue starts, from the blow whose
			// stamp it is; a stamp with no such blow (a table's share, a hazard) has no side: all four edges. The side is
			// latched there, at age 0 only: a later blow inside the cue's life, which starts no cue, never moves it.
			const float HitAge = ftcore::uiHitWatchStep(HitWatch, Runner.GetPlayerHitTime(), Runner.GetNow());
			if (HitAge == 0.f)
			{
				FVector2D From;
				double At = -1e9;
				const bool bSide = Runner.GetLastHitFrom(From, At) && At == HitWatch.seen;
				HitEdges = ftcore::uiHitEdges(bSide, static_cast<float>(From.X), static_cast<float>(From.Y));
			}
			const UWorld* CueWorld = Runner.GetWorld();
			const bool bGamePaused = CueWorld && CueWorld->IsPaused();
			HitA = UiStateShot() == EUiStateShot::Hit ? 1.f : ftcore::uiHitCueLevel(HitAge, bGamePaused);
			if (UiStateShot() == EUiStateShot::Hit)
			{
				HitEdges = ftcore::uiHitEdges(true, 1.f, 0.f); // the still's blow: from the right
			}
#if !UE_BUILD_SHIPPING
			// -UiHitNoCue (captures): the same frame with no cue at all, for the over-the-text diff.
			static const bool bNoCue = FParse::Param(FCommandLine::Get(), TEXT("UiHitNoCue"));
			bNoHitRed = bNoCue;
			if (bNoCue)
			{
				HitA = 0.f;
			}
			// -FTUiLogHit (captures): the cue's age, level and edges each frame it lives, for the strips' key.
			static const bool bLogHit = FParse::Param(FCommandLine::Get(), TEXT("FTUiLogHit"));
			if (bLogHit && HitAge < 0.5f)
			{
				UE_LOG(LogTemp, Display, TEXT("FTNewUi: hit cue age %.3f level %.3f edges L%.2f R%.2f T%.2f B%.2f frame %llu"), HitAge, HitA,
					HitEdges.left, HitEdges.right, HitEdges.top, HitEdges.bottom, static_cast<unsigned long long>(GFrameCounter));
			}
#endif
			const ftcore::Settings* Prefs = Runner.GetSettings();
			bReduceFlashes = Prefs && Prefs->reduceFlashes;
		}
		ParryA = Pulse(Now, Runner.GetParryTime(), kParryEdgeLife);
		bResult = Fight.over();
		if (bResult)
		{
			bWon = Fight.phase() == ftcore::ArenaPhase::Victory;
			ResultA = FMath::Clamp(static_cast<float>(Runner.GetResultAge() / kResultFade), 0.f, 1.f);
			bAgain = !Runner.IsAutoPlay() && Runner.CanFightAgain();
		}
		else if (UiStateShot() == EUiStateShot::Defeat)
		{
			bResult = true; // the defeat card held for the still, over the fight
			bWon = false;
			ResultA = 1.f;
			bAgain = false;
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
			const FMarkTok M = MarkTok(0.8f + 0.2f * Br); // a mark: the world's mark colour; its label stays ink
			const float HD = 10.f * S; // half-diagonal: 20 across at 1080p
			const FVector2f Pts[4] = {FVector2f(WeakAt.X, WeakAt.Y - HD), FVector2f(WeakAt.X + HD, WeakAt.Y), FVector2f(WeakAt.X, WeakAt.Y + HD),
				FVector2f(WeakAt.X - HD, WeakAt.Y)};
			const FLinearColor DiamondRim = Tok(UiToken::Backing, M.C.A); // the 1 px rim that holds it on a pale body
			Mesh.Fill(Pts, 4, M.C, &DiamondRim);
			Ring(WeakAt.X, WeakAt.Y, (30.f + 4.f * Br) * S, 2.f, 141, S, M.C, M.RimOrNull());
			if (WeakLabelA > 0.f)
			{
				// WEAK, once a fight while the point is open: 2 s, then a 1 s fade (uiOnceLabelStep).
				P.Text(WeakAt.X + 44.f * S, CapTop(WeakAt.Y, NoteAsc), WeakWord, NoteFont, Tok(UiToken::Ink, Hud::NoteInk * WeakLabelA));
			}
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
		// A hit, the same in every world (uiDangerSet): two layers on the screen's edges, alpha over the picture on the
		// shade part under every line and word. First a dark one (hitDark, the danger red at kUiHitDarkK), then the red
		// (hitCue), each at Core's uiHitLayers for the share of the way in (a smooth fall to nothing 6 percent of that
		// edge's axis in) and the edge's weight (the struck side full; all four with no side). One fall and one grid for
		// edges and corners: where two edges meet each point takes the stronger of the two, never the two stacked.
		// Reduced flashes: none (the lost hp keeps the cue).
		if (HitA > 0.f && !bReduceFlashes)
		{
			const float DX = ftcore::kUiHitGlowShare * Size.X;
			const float DY = ftcore::kUiHitGlowShare * Size.Y;
			const ftcore::UiHitEdges& W = HitEdges;
			const auto Layer = [&](const FLinearColor& C, bool bDark)
			{
				const auto At = [&](float In, float Edge)
				{
					const ftcore::UiHitLayers L = ftcore::uiHitLayers(In, Edge);
					return HitA * (bDark ? L.dark : L.red);
				};
				Mesh.Patch(DX, 0.f, Size.X - DX, DY, C, [&](float, float V) { return At(V, W.top); });
				Mesh.Patch(DX, Size.Y - DY, Size.X - DX, Size.Y, C, [&](float, float V) { return At(1.f - V, W.bottom); });
				Mesh.Patch(0.f, DY, DX, Size.Y - DY, C, [&](float U, float) { return At(U, W.left); });
				Mesh.Patch(Size.X - DX, DY, Size.X, Size.Y - DY, C, [&](float U, float) { return At(1.f - U, W.right); });
				const auto Corner = [&](float X0, float Y0, bool bLeft, bool bTop)
				{
					const float WX = bLeft ? W.left : W.right;
					const float WY = bTop ? W.top : W.bottom;
					Mesh.Patch(X0, Y0, X0 + DX, Y0 + DY, C, [&](float U, float V)
					{
						return FMath::Max(At(bLeft ? U : 1.f - U, WX), At(bTop ? V : 1.f - V, WY));
					});
				};
				Corner(0.f, 0.f, true, true);
				Corner(Size.X - DX, 0.f, false, true);
				Corner(0.f, Size.Y - DY, true, false);
				Corner(Size.X - DX, Size.Y - DY, false, false);
			};
			Layer(Shown().HitDark, true);
			Layer(Shown().HitCue, false);
		}
		// The result: the word in the subtitle's face at the subtitle's place, and how to fight again in the key hint's.
		// Both up, and the tag in, by what the safe zone adds to the HUD's own margin.
		const ftcore::UiMargins E = EdgesAt(Size, S);
		if (bResult && ResultA > 0.f)
		{
			PaintLowGradient(Mesh, Size, Table().Style.backingSoftDialogue * ResultA);
			const float WordW = bWon ? VictoryW : DefeatW;
			const float Up = E.bottom - Hud::Margin * S;
			const float WordTop = Size.Y - 96.f * S - Up - ResultLineH;
			if (bWon)
			{
				P.Text(CX - WordW * 0.5f, WordTop, VictoryWord, ResultFont, Tok(UiToken::Ink, ResultA));
			}
			else
			{
				// In ink in every world, and where the world underlines danger words the underline is ink too: red is for
				// harm to the player, and the fight is over.
				P.Text(CX - WordW * 0.5f, WordTop, DefeatWord, ResultFont, Tok(UiToken::Ink, ResultA));
				if (Shown().bDangerUnderline && DefeatW > 0.f)
				{
					ValueLine(CX - WordW * 0.5f, CX + WordW * 0.5f, WordTop + ResultAsc + FMath::Max(2.f * S, 0.12f * ResultAsc), 0.f, 1.f,
						Hud::DangerUnderW, 151, S, Tok(UiToken::Ink, ResultA), 0.f);
				}
			}
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
	FDelegateHandle SafeFrameChange; // the engine's safe-frame change (MarkSafeStale)
	FDelegateHandle DisplayChange;   // the platform's display change (MarkSafeStaleFor)
	ftcore::ArenaHudView View;
	ftcore::UiFade LeadFade;
	ftcore::UiFade TopFade;
	FLostTrace HpLost;
	ftcore::UiHitSegs HitSegs; // each blow's red lost hp on the lead's line
	bool bNoHitRed = false;    // -UiHitNoCue: no red lost hp either
	FLostTrace PartnerLost;
	FLostTrace TopLost;
	float LoadFill = 0.f; // the Load line's gold, 0..1 of loadCap, drained
	ftcore::UiLoadLabel LoadLabel;
	float LoadLabelA = 0.f;
	ftcore::UiLabelColumn LoadColumn; // LOAD and the hp figure take turns in one column
	float HpFigA = 1.f;
	ftcore::UiOnceLabel WeakLabel; // WEAK by the open weak point: once a fight
	ftcore::UiReleasePulse ReleasePulse; // signature device 1's state, on the fight's clock
	ftcore::UiReleasePulseView ReleasePulseV;
	float WeakLabelA = 0.f;
	FString WeakWord;
	FSlateFontInfo FigFont; // the figures: the boss's percent, the hp figure, the prompts' counts
	float FigAsc = 0.f;
	FString HpFigText, BossPctText;
	float HpFigW = 0.f, HpFigSlotW = 0.f, BossPctW = 0.f;
	int32 HpFigHp = -1, HpFigMax = -1, BossPctShown = -2;
	float AimA = 0.f; // the aim stance's fade (the runner's): the aim dot and the cylinder
	float HudScale = 1.f; // over the viewport's scale (the setting, or -FTHudScale)
	ftcore::ShowBasicControls BasicPolicy = ftcore::ShowBasicControls::FirstFights; // the Basic controls setting
	float LeadDip = 0.f, PartnerDip = 0.f; // a hit's name drop: 1 on the hit frame, 0 after 0.15 s
	ftcore::InputDevice KeyDevice = ftcore::InputDevice::Keys;
	uint32 KeyGeneration = 0; // the bindings' generation the keys were built from (FTInputBindings::Generation)
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
	FSlateFontInfo PartnerNameFont;
	float PartnerNameAsc = 0.f;
	FString LeadNameSeen, LeadName, PartnerNameSeen, PartnerName;
	float LeadNameW = 0.f, PartnerNameW = 0.f;
	FString LoadWord, ReleaseWord, ReleaseKey, GuardWord, PoiseWord, WeightWord, DebtWord, DebtText;
	float ReleaseWordW = 0.f, ReleaseKeyW = 0.f, GuardWordW = 0.f, WeightW = 0.f, DebtW = 0.f;
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
	FString GiftHealText; // the Gift Heals' count, built when it changes (empty while none is reported)
	float GiftHealW = 0.f;
	int32 GiftHealShown = -2;
	ftcore::UiPromptMask PromptKeyed = 0; // the prompts something reaches on the device in use (a key, a route, NO KEY)
	ftcore::UiPromptMask PromptNoKey = 0; // the prompts whose action the import left unbound on the device: NO KEY
	ftcore::UiPromptMask PromptDimmed = 0; // the prompts drawn dimmed this tick (Core's uiPromptsDimmed)
	ftcore::UiRefusedRows RefusedRows;        // each refused prompt's moment after a press of its key (Core's uiRefusedStep)
	ftcore::UiPromptMask RefusedShowing = 0; // the prompts showing their refusal this tick
	uint32 PromptPressSeen[ftcore::kUiPromptCount] = {}; // the runner's press counts as last read
	bool bPressSeeded = false;                          // PromptPressSeen has been read once
	ftcore::UiRefusedWatch WhyWatch;          // the prompts' answers as the last tick read them (cleared on a retry)
	std::array<ftcore::UiPromptMask, ftcore::kUiPromptCount> PromptSameKeyBelow{}; // per prompt, the lower ones on its key
	ftcore::UiPromptReasons Why{};    // each prompt's answer from the rules this tick
	ftcore::PressReason GiftWhy[2] = {ftcore::PressReason::Absent, ftcore::PressReason::Absent}; // Gift Heal's, Gift Guard's
	FString NoKeyWord;                // NO KEY, in the key box
	// The special prompt: this tick's view (uiSpecialStep) and its state, its key, its words (SPECIAL, CALL), the reason
	// words by UiSpecialWhy, and the tail drawn after the word (held through the leave) with its width in the prompts' face.
	ftcore::UiSpecialState SpecialState;
	ftcore::UiSpecialView Special;
	FString SpecialKey, SpecialWord, CallWord, SpecialTail;
	float SpecialKeyW = 0.f, SpecialWordW = 0.f, CallWordW = 0.f, SpecialTailW = 0.f;
	FString SpecialWhyWord[ftcore::kUiSpecialWhyCount];
	float SpecialWhyW[ftcore::kUiSpecialWhyCount] = {};
	FString WhyWord[ftcore::kPressReasonCount]; // a refused prompt's reason word, by PressReason (empty: none)
	float WhyW[ftcore::kPressReasonCount] = {};
	// The HUD's edges: the platform's title-safe padding as Slate's ratio of half the viewport per side, the viewport size
	// it was read at, and the HUD safe margin setting in percent.
	FMargin SafeRatio = FMargin(0.f);
	FVector2f SafeSize = FVector2f(-1.f, -1.f);
	bool bSafeStale = false; // the safe frame or the display changed since the padding was read
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
	// The party screen: the runner's screen as of this tick (or a -UiStateShot's), the partner row's answer from the
	// rules, the opening's ease, each refused lead row's word (by UiPartyWhy; empty: none), the Third's row (its name and
	// its gift line, rebuilt when ThirdKey changes, and its words), and the refused switch's call (its prefix, the
	// refusal shown, its text and ink middle in the call face, its time left, the runner's count as of the last tick and
	// whether the budget drew it).
	ftcore::UiPartyScreen PartyShown;
	ftcore::UiPartyScreen PartyDrawn; // the screen as last open: its rows' inks and marks while it fades shut
	ftcore::UiPartyWhy PartnerWhy = ftcore::UiPartyWhy::Unknown;
	float PartyOpenK = 0.f;
	ftcore::UiScreenMask Screens = 0; // the screens open over the fight this tick (AFTArenaHUD::ScreensOpen)
	FString PartyWhyWord[ftcore::kUiPartyWhyCount];
	float PartyWhyW[ftcore::kUiPartyWhyCount] = {};
	bool bThirdRow = false;
	FString ThirdName, ThirdText, GiftReadyWord, GiftVacantWord, GiftHostileWord, GiftSecondsFmt;
	float ThirdNameW = 0.f;
	const FString* ThirdNameSrc = nullptr; // the GameInstance string ThirdName was made from
	uint32 ThirdKey = MAX_uint32;
	FString SwitchPrefix, SwitchText;
	ftcore::UiPartyWhy SwitchWhy = ftcore::UiPartyWhy::Unknown;
	float SwitchMid = 0.f;
	float SwitchLeft = 0.f;
	uint32 SwitchSerialSeen = 0;
	int RestartsSeen = 0; // ArenaStats::restarts at the last live frame: a retry's seats are no switch
	bool bSwitchOn = false;
	bool bWaiting = false;
	ftcore::UiWaitLine WaitLine;
	float Wait01 = 0.f;
	// The rows' trade of places on a lead switch.
	ftcore::UiRowSwap RowSwap;
	// The first-use labels.
	FSlateFontInfo NoteFont;
	float NoteAsc = 0.f;
	FString NoteWord[UE_ARRAY_COUNT(kFirstUseUi)];
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
	ftcore::UiHitWatch HitWatch; // the runner's last hit stamp as read and the cue's age on the frame clock
	ftcore::UiHitEdges HitEdges; // the edges' weights, taken when the cue starts
	bool bReduceFlashes = false; // the settings' reduce flashes: the cue keeps only the lost hp in red
	ftcore::UiGuardLabel GuardLabel; // GUARD's 2 s after a guard state change
	float GuardLabelA = 0.f;
	FSlateFontInfo ResultFont;
	float ResultLineH = 0.f, ResultAsc = 0.f;
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

// The subtitle's metrics: the block's foot over the screen's bottom, the line height over the face's, the gap after an
// inline speaker (px at 1080).
constexpr float kSubBottom = 96.f;
constexpr float kSubLineK = 1.15f;
constexpr float kSubSpeakerGap = 18.f;

// The brightest tenth of the game viewport's picture inside a rectangle of a widget Size big. Read only at the end of
// the viewport's draw (the Slate layers are not in the picture yet), never per frame; false when nothing could be read.
// The game viewport's pixels under a rectangle of a widget Size big; false when there is none.
bool PicturePx(const FBox2f& Rc, const FVector2f& Size, FIntRect& Out)
{
	FViewport* VP = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
	const FIntPoint VS = VP ? VP->GetSizeXY() : FIntPoint::ZeroValue;
	if (!Rc.bIsValid || Rc.Max.X <= Rc.Min.X || Rc.Max.Y <= Rc.Min.Y || VS.X <= 0 || VS.Y <= 0 || Size.X <= 0.f || Size.Y <= 0.f)
	{
		return false;
	}
	const float KX = VS.X / Size.X;
	const float KY = VS.Y / Size.Y;
	Out = FIntRect(FMath::Clamp(FMath::FloorToInt(Rc.Min.X * KX), 0, VS.X - 1), FMath::Clamp(FMath::FloorToInt(Rc.Min.Y * KY), 0, VS.Y - 1),
		FMath::Clamp(FMath::CeilToInt(Rc.Max.X * KX), 1, VS.X), FMath::Clamp(FMath::CeilToInt(Rc.Max.Y * KY), 1, VS.Y));
	return Out.Width() > 0 && Out.Height() > 0;
}

// The synchronous read: the menus' one read when a screen opens (it waits for the render thread and the GPU; never in
// play, where FTPictureReader reads instead).
bool PictureBrightest(const FBox2f& Rc, const FVector2f& Size, ftcore::UiColor& Out, TArray<FColor>* Buf = nullptr)
{
	FViewport* VP = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
	FIntRect R;
	if (!VP || !PicturePx(Rc, Size, R))
	{
		return false;
	}
	TArray<FColor> Local;
	TArray<FColor>& Px = Buf ? *Buf : Local;
	if (!VP->ReadPixels(Px, FReadSurfaceDataFlags(), R) || Px.Num() == 0)
	{
		return false;
	}
	ftcore::UiLumaHistogram Hist;
	for (const FColor& Q : Px)
	{
		ftcore::uiLumaAdd(Hist, ftcore::UiColor{Q.R / 255.f, Q.G / 255.f, Q.B / 255.f, 1.f});
	}
	Out = ftcore::uiLumaBrightest(Hist, 0.1f);
	return Out.r + Out.g + Out.b > 0.f; // all black is no picture yet
}

// The reads of the picture in play (2026-11-17: the synchronous read cost a +25 ms frame at 1080 and +43 ms at 4K each
// time). Asked at the end of the game viewport's draw; in the next frame's scene render a view extension, inside the
// renderer's own graph after post-processing, copies the rectangle's pixels into a small staging texture
// (FRHIGPUTextureReadback). At the end of a later render frame, once the GPU has it, the render thread maps it and makes
// its brightest tenth; the game thread takes that a frame or two after asking. Nothing waits: no ReadPixels and no flush.
// The copy reads the family's own registered target (the back buffer in the standalone game, where the game thread holds
// no texture), so nothing is kept across frames and the renderer sets the target's final state. One request in flight
// per slot; a slot's staging texture is kept and only grows. Results go to the game thread through an atomic serial, the
// colour written before it. With HDR output on, the back buffer is not display-referred sRGB: no reads (logged once).
class FTPictureReader
{
public:
	static constexpr int32 kSlots = 10;
	// The guard needs a luminance estimate, not every pixel: a larger rectangle is decoded every Stride-th pixel each way.
	// A small line stays exact; at 4K a line is read every 2nd-3rd pixel, the dialogue's band every 6th-10th.
	static constexpr int32 kMaxSamples = 16384;

	FTPictureReader() : State(MakeShared<FState, ESPMode::ThreadSafe>())
	{
		Ext = FSceneViewExtensions::NewExtension<FExt>(State);
	}
	FTPictureReader(const FTPictureReader&) = delete;
	FTPictureReader& operator=(const FTPictureReader&) = delete;

	~FTPictureReader()
	{
		// The engine holds the extension weakly; a family already gathered keeps it to that frame's end.
		Ext.Reset();
		// The render thread unhooks its end of frame after any want still queued, frees the readbacks (so none is ever
		// destroyed on the game thread, wherever the state's last reference drops), then lets go. With the RHI already
		// gone (engine exit), nothing is enqueued: the hook is removed here and the readbacks are left to the process.
		if (GIsRHIInitialized)
		{
			ENQUEUE_RENDER_COMMAND(FTPictureReaderDrop)([S = MoveTemp(State)](FRHICommandListImmediate&) mutable
			{
				Drop(S, true);
			});
		}
		else
		{
			Drop(State, false);
		}
	}

	bool Busy(int32 Slot) const { return Slot < 0 || Slot >= kSlots || Taken[Slot] != Asked[Slot]; }

	// Asks for the brightest tenth of the game viewport's pixels in Px. Only at the end of the viewport's draw (the
	// Slate layers are not in the picture). False when the slot is busy, there is no viewport, or HDR output is on.
	bool Request(int32 Slot, const FIntRect& Px)
	{
		FViewport* VP = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
		if (!VP || Busy(Slot) || Px.Width() <= 0 || Px.Height() <= 0)
		{
			return false;
		}
		if (IsHDREnabled())
		{
			if (!bHdrLogged)
			{
				bHdrLogged = true;
				UE_LOG(LogForgottenThird, Log, TEXT("UI: picture read: HDR output is on; no picture reads, the guards stand at rest."));
			}
			return false;
		}
		const uint32 Serial = ++Asked[Slot];
		AskedFrame[Slot] = GFrameCounter;
		State->Outstanding.fetch_add(1, std::memory_order_relaxed); // before the want: the next frame's family copies it
		ENQUEUE_RENDER_COMMAND(FTPictureRead)([S = State, Px, Slot, Serial](FRHICommandListImmediate&)
		{
			Want(S, Slot, Px, Serial);
		});
		return true;
	}

	// The slot's result, once per request: true when it came (bOk: a picture was read, not all black).
	bool Take(int32 Slot, ftcore::UiColor& Out, bool& bOk, int32* OutFrames = nullptr, float* OutRtMs = nullptr)
	{
		if (Slot < 0 || Slot >= kSlots || !Busy(Slot))
		{
			return false;
		}
		const FSlot& L = State->Slots[Slot];
		if (L.Done.load(std::memory_order_acquire) != Asked[Slot])
		{
			return false;
		}
		Taken[Slot] = Asked[Slot];
		Out = L.Pic;
		bOk = L.bOk;
		if (OutFrames)
		{
			*OutFrames = static_cast<int32>(GFrameCounter - AskedFrame[Slot]);
		}
		if (OutRtMs)
		{
			*OutRtMs = L.RtMs;
		}
		return true;
	}

private:
	struct FSlot
	{
		// The render thread's.
		TUniquePtr<FRHIGPUTextureReadback> Readback;
		FIntPoint Extent = FIntPoint::ZeroValue; // the staging texture's size
		EPixelFormat Format = PF_Unknown;
		FIntRect WantPx;                         // the asked pixels, until the copy
		FIntRect Sub;                            // the asked pixels inside the staging copy
		int32 Stride = 1;
		uint32 Serial = 0;
		int32 Frames = 0;                        // render frames ended since the want or the copy
		bool bWanted = false;                    // asked, not copied yet
		bool bInFlight = false;                  // copied, not read yet
		// Handed to the game thread (written before Done).
		ftcore::UiColor Pic{0.f, 0.f, 0.f, 1.f};
		bool bOk = false;
		float RtMs = 0.f;
		std::atomic<uint32> Done{0};
	};
	struct FState
	{
		FSlot Slots[kSlots];
		FDelegateHandle Hook;                    // the render thread's
		bool bFormatLogged = false;              // the render thread's
		bool bTargetLogged = false;              // the render thread's (not shipping)
		std::atomic<int32> Outstanding{0};       // wants not yet copied: the game thread adds, the render thread takes
	};

	// The reader's end: unhook, then free each slot's readback (on the render thread when `bRhi`; at engine exit with the
	// RHI gone, released unfreed instead), and let go of the state.
	static void Drop(TSharedPtr<FState, ESPMode::ThreadSafe>& S, bool bRhi)
	{
		if (S.IsValid())
		{
			if (S->Hook.IsValid())
			{
				FCoreDelegates::OnEndFrameRT.Remove(S->Hook);
				S->Hook.Reset();
			}
			S->Outstanding.store(0, std::memory_order_relaxed);
			for (FSlot& L : S->Slots)
			{
				L.bWanted = false;
				L.bInFlight = false;
				if (bRhi)
				{
					L.Readback.Reset();
				}
				else
				{
					(void)L.Readback.Release();
				}
			}
		}
		S.Reset();
	}

	// The copy's hook in the renderer: active only for the game viewport's family while a want waits.
	class FExt final : public FSceneViewExtensionBase
	{
	public:
		FExt(const FAutoRegister& Reg, TSharedPtr<FState, ESPMode::ThreadSafe> InState)
			: FSceneViewExtensionBase(Reg), St(MoveTemp(InState))
		{
		}

		virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& Family) override
		{
			if (Family.bIsMainViewFamily && Family.RenderTarget)
			{
				CopyWanted(*St, GraphBuilder, Family.RenderTarget->GetRenderTargetTexture().GetReference());
			}
		}

	protected:
		virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Ctx) const override
		{
			return St->Outstanding.load(std::memory_order_relaxed) > 0 && Ctx.Viewport && GEngine && GEngine->GameViewport &&
				Ctx.Viewport == GEngine->GameViewport->Viewport;
		}

	private:
		TSharedPtr<FState, ESPMode::ThreadSafe> St;
	};

	// sRGB and linear light for each 8-bit and 10-bit code, and sRGB for linear light in 4096 steps (the float back
	// buffer's path): built once, so a pixel costs table loads and no power.
	struct FLuts
	{
		float Lin8[256];
		float Lin10[1024];
		float Srgb12[4096];
		FLuts()
		{
			for (int32 I = 0; I < 256; ++I)
			{
				Lin8[I] = ftcore::uiSrgbToLinear(I / 255.f);
			}
			for (int32 I = 0; I < 1024; ++I)
			{
				Lin10[I] = ftcore::uiSrgbToLinear(I / 1023.f);
			}
			for (int32 I = 0; I < 4096; ++I)
			{
				Srgb12[I] = ftcore::uiLinearToSrgb(I / 4095.f);
			}
		}
	};
	static const FLuts& Luts()
	{
		static const FLuts T;
		return T;
	}

	static void Publish(FSlot& L, uint32 Serial, const ftcore::UiColor& Pic, bool bOk, float RtMs)
	{
		L.Pic = Pic;
		L.bOk = bOk;
		L.RtMs = RtMs;
		L.bWanted = false;
		L.bInFlight = false;
		L.Done.store(Serial, std::memory_order_release);
	}

	static bool Decodable(EPixelFormat F)
	{
		return F == PF_B8G8R8A8 || F == PF_R8G8B8A8 || F == PF_A2B10G10R10 || F == PF_FloatRGBA;
	}

	// Render thread: a want, and the end-of-frame hook (held weakly, so a hook outliving the reader does nothing).
	static void Want(const TSharedPtr<FState, ESPMode::ThreadSafe>& SP, int32 Slot, const FIntRect& Px, uint32 Serial)
	{
		FState& S = *SP;
		if (!S.Hook.IsValid())
		{
			S.Hook = FCoreDelegates::OnEndFrameRT.AddLambda([W = TWeakPtr<FState, ESPMode::ThreadSafe>(SP)]()
			{
				if (const TSharedPtr<FState, ESPMode::ThreadSafe> P = W.Pin())
				{
					Poll(*P);
				}
			});
		}
		FSlot& L = S.Slots[Slot];
		L.WantPx = Px;
		L.Serial = Serial;
		L.Frames = 0;
		L.bWanted = true;
	}

	// Render thread, in the game viewport family's graph after post-processing: each wanted slot's copy.
	static void CopyWanted(FState& S, FRDGBuilder& GraphBuilder, FRHITexture* Tex)
	{
		for (FSlot& L : S.Slots)
		{
			if (L.bWanted)
			{
				S.Outstanding.fetch_sub(1, std::memory_order_relaxed);
				Copy(S, L, GraphBuilder, Tex);
			}
		}
	}

	static void Copy(FState& S, FSlot& L, FRDGBuilder& GraphBuilder, FRHITexture* Tex)
	{
		L.bWanted = false;
		const EPixelFormat Format = Tex ? Tex->GetFormat() : PF_Unknown;
		if (!Tex || Tex->IsMultisampled() || !Decodable(Format))
		{
			if (!S.bFormatLogged)
			{
				S.bFormatLogged = true;
				UE_LOG(LogForgottenThird, Warning, TEXT("UI: picture read: the viewport's texture (%s) cannot be read; the guards stand at rest."),
					Tex ? GetPixelFormatString(Format) : TEXT("none"));
			}
			Publish(L, L.Serial, ftcore::UiColor{0.f, 0.f, 0.f, 1.f}, false, 0.f);
			return;
		}
#if !UE_BUILD_SHIPPING
		if (!S.bTargetLogged)
		{
			S.bTargetLogged = true;
			UE_LOG(LogForgottenThird, Log, TEXT("UI: picture read: reading the viewport's %s target in the renderer's graph."), GetPixelFormatString(Format));
		}
#endif
		FIntRect Px = L.WantPx;
		const FIntPoint TS = Tex->GetSizeXY();
		Px.Clip(FIntRect(0, 0, TS.X, TS.Y));
		if (Px.Width() <= 0 || Px.Height() <= 0)
		{
			Publish(L, L.Serial, ftcore::UiColor{0.f, 0.f, 0.f, 1.f}, false, 0.f);
			return;
		}
		// The staging texture: kept, grown in steps (128 x 64 px) when a larger rectangle or another format comes.
		if (!L.Readback || L.Format != Format || Px.Width() > L.Extent.X || Px.Height() > L.Extent.Y || L.Extent.X > TS.X || L.Extent.Y > TS.Y)
		{
			L.Extent = FIntPoint(FMath::Min(Align(FMath::Max(Px.Width(), L.Extent.X), 128), TS.X), FMath::Min(Align(FMath::Max(Px.Height(), L.Extent.Y), 64), TS.Y));
			L.Format = Format;
			L.Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("FTUiPictureRead"));
		}
		const FIntPoint Origin(FMath::Clamp(Px.Min.X, 0, TS.X - L.Extent.X), FMath::Clamp(Px.Min.Y, 0, TS.Y - L.Extent.Y));
		L.Sub = FIntRect(Px.Min - Origin, Px.Max - Origin);
		L.Stride = 1;
		while ((Px.Width() / L.Stride) * (Px.Height() / L.Stride) > kMaxSamples)
		{
			++L.Stride;
		}
		L.Frames = 0;
		L.bInFlight = true;
		L.Readback->ResetReady(); // the copy's pass runs when the graph executes: not ready until it has
		// Registered once per graph: this is the renderer's own registration of the family's target, whose end state
		// the renderer sets.
		const FRDGTextureRef Src = RegisterExternalTexture(GraphBuilder, Tex, TEXT("FTUiPictureSource"));
		AddEnqueueCopyPass(GraphBuilder, L.Readback.Get(), Src, FResolveRect(Origin.X, Origin.Y, Origin.X + L.Extent.X, Origin.Y + L.Extent.Y));
	}

	// Render thread, at each frame's end: the slots whose copies the GPU has finished, mapped and made into their result.
	// Frames counts render frames ended (a minimised window still ends them), so a want that is never copied or a copy that
	// never comes lets its slot go after about two seconds.
	static void Poll(FState& S)
	{
		const FLuts& T = Luts();
		for (FSlot& L : S.Slots)
		{
			if (L.bWanted)
			{
				if (++L.Frames > 120) // no family copied it (the viewport did not draw): let it go
				{
					S.Outstanding.fetch_sub(1, std::memory_order_relaxed);
					Publish(L, L.Serial, ftcore::UiColor{0.f, 0.f, 0.f, 1.f}, false, 0.f);
				}
				continue;
			}
			if (!L.bInFlight)
			{
				continue;
			}
			if (!L.Readback->IsReady())
			{
				if (++L.Frames > 120) // never came: let the slot go (a new staging texture next time)
				{
					L.Readback.Reset();
					Publish(L, L.Serial, ftcore::UiColor{0.f, 0.f, 0.f, 1.f}, false, 0.f);
				}
				continue;
			}
			const double T0 = FPlatformTime::Seconds();
			int32 Pitch = 0; // in pixels
			const uint8* Data = static_cast<const uint8*>(L.Readback->Lock(Pitch));
			ftcore::UiLumaHistogram Hist;
			if (Data && Pitch > 0)
			{
				const int32 Bpp = L.Format == PF_FloatRGBA ? 8 : 4;
				for (int32 Y = L.Sub.Min.Y; Y < L.Sub.Max.Y; Y += L.Stride)
				{
					const uint8* Row = Data + static_cast<SIZE_T>(Y) * Pitch * Bpp;
					for (int32 X = L.Sub.Min.X; X < L.Sub.Max.X; X += L.Stride)
					{
						const uint8* Q = Row + static_cast<SIZE_T>(X) * Bpp;
						switch (L.Format)
						{
						case PF_B8G8R8A8:
							ftcore::uiLumaAddDecoded(Hist, ftcore::UiColor{Q[2] / 255.f, Q[1] / 255.f, Q[0] / 255.f, 1.f}, T.Lin8[Q[2]], T.Lin8[Q[1]], T.Lin8[Q[0]]);
							break;
						case PF_R8G8B8A8:
							ftcore::uiLumaAddDecoded(Hist, ftcore::UiColor{Q[0] / 255.f, Q[1] / 255.f, Q[2] / 255.f, 1.f}, T.Lin8[Q[0]], T.Lin8[Q[1]], T.Lin8[Q[2]]);
							break;
						case PF_A2B10G10R10:
						{
							uint32 V;
							FMemory::Memcpy(&V, Q, 4);
							const uint32 R = V & 0x3FF, G = (V >> 10) & 0x3FF, B = (V >> 20) & 0x3FF;
							ftcore::uiLumaAddDecoded(Hist, ftcore::UiColor{R / 1023.f, G / 1023.f, B / 1023.f, 1.f}, T.Lin10[R], T.Lin10[G], T.Lin10[B]);
							break;
						}
						default: // PF_FloatRGBA: linear light, encoded through the table as ReadPixels would
						{
							const FFloat16Color* H = reinterpret_cast<const FFloat16Color*>(Q);
							const float R = H->R.GetFloat(), G = H->G.GetFloat(), B = H->B.GetFloat();
							const auto Enc = [&T](float C) { return T.Srgb12[static_cast<int32>((C > 0.f ? (C < 1.f ? C : 1.f) : 0.f) * 4095.f + 0.5f)]; };
							ftcore::uiLumaAddDecoded(Hist, ftcore::UiColor{Enc(R), Enc(G), Enc(B), 1.f}, R, G, B);
							break;
						}
						}
					}
				}
			}
			if (Data)
			{
				L.Readback->Unlock();
			}
			const ftcore::UiColor Pic = ftcore::uiLumaBrightest(Hist, 0.1f);
			Publish(L, L.Serial, Pic, Hist.total > 0 && Pic.r + Pic.g + Pic.b > 0.f, static_cast<float>(1000.0 * (FPlatformTime::Seconds() - T0)));
		}
	}

	TSharedPtr<FState, ESPMode::ThreadSafe> State;
	TSharedPtr<FExt, ESPMode::ThreadSafe> Ext;
	uint32 Asked[kSlots] = {};
	uint32 Taken[kSlots] = {};
	uint64 AskedFrame[kSlots] = {};
	bool bHdrLogged = false;
};

// The band behind a subtitle block (2026-11-16 ruling): black at Peak (the least Core uiSubtitleBand measured, up to
// kUiSubtitleBandMax) under the whole block, fading to nothing on every side, so it never reads as a plate. Painted only
// where uiSubtitleBand asked for it. The explore HUD lays the same shade under a small line whose contour fell short.
void PaintSubtitleBand(const FPainter& P, float CX, float Top, float Bottom, float Widest, float LineH, float Peak)
{
	if (Peak <= 0.f || Bottom <= Top)
	{
		return;
	}
	static const FLinearColor Black(0.f, 0.f, 0.f, 1.f);
	// Soft's even middle is its inner 0.55: the block and a margin of most of a line sit inside it.
	const float Pad = 0.8f * LineH;
	P.Soft(CX, 0.5f * (Top + Bottom), (Widest + 2.f * Pad) / 0.55f, (Bottom - Top + Pad) / 0.55f, Peak, 1.f, &Black);
}

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
		const float Scale = FMath::Clamp(D->GetSubtitleScale(), ftcore::Settings::kMinSubtitleScale, ftcore::Settings::kMaxSubtitleScale);
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
		// The page (two lines) the reveal stands in; the block, for the read of the picture under it.
		{
			const int32 RevealedUnit = CpToUnit.Num() > 0 ? CpToUnit[FMath::Clamp(Revealed, 0, CpToUnit.Num() - 1)] : 0;
			PageFirst = static_cast<int32>(ftcore::uiSubtitlePage(Fit, static_cast<std::uint32_t>(RevealedUnit)) * ftcore::kUiSubtitleLines);
			PageRows = FMath::Clamp(Lines.Num() - PageFirst, 0, static_cast<int32>(ftcore::kUiSubtitleLines));
			const float S = FMath::Max(0.25f, Size.Y / 1080.f);
			const float Bottom = Size.Y - kSubBottom * S;
			float Widest = 0.f;
			for (int32 I = PageFirst; I < PageFirst + PageRows; ++I)
			{
				Widest = FMath::Max(Widest, LineW[I] + (I == PageFirst ? SpeakerW : 0.f));
			}
			SubWidest = Widest;
			SubRead = FBox2f(FVector2f(Size.X * 0.5f - 0.5f * Widest, Bottom - LineH * PageRows), FVector2f(Size.X * 0.5f + 0.5f * Widest, Bottom));
		}

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
				const float SubTop = Size.Y - kSubBottom * S - LineH * PageRows; // the subtitle's block, as painted
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
		TakeReads();
		const float Target = ConvTarget();
		ConvDim = FMath::Min(Target, ConvDim + FMath::Max(Target - ConvDim, 0.02f) * FMath::Min(1.f, DeltaTime * 10.f));
		// The gradient stands behind the replies only (2026-11-16: a plain line has no backing but the measured band).
		GradK = FMath::Clamp(GradK + (OptionRows > 0 ? DeltaTime : -DeltaTime) / 0.25f, 0.f, 1.f);
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
				PaintLowGradient(Mesh, Size, ConvDim * Hold * GradK, EvenTop);
				Mesh.Flush(Out, Layer + 1);
			}
			return Layer;
		}
		const FVector2f Size = Geometry.GetLocalSize();
		const FPainter P{Geometry, Out, Layer, FMath::Max(0.25f, Size.Y / 1080.f)};
		const float S = P.S;
		const FString& Text = D->GetText();
		// The subtitle's block (the page on show), where it always stands; the replies stand above it.
		const float Bottom = Size.Y - kSubBottom * S;
		const float Top = Bottom - LineH * PageRows;
		const float CX = Size.X * 0.5f;
		Mesh.Begin(Geometry, S);
		PaintLowGradient(Mesh, Size, ConvDim * GradK, EvenTop);
		PaintSubtitleBand(P, CX, Top, Bottom, SubWidest, LineH, SubBand * (1.f - GradK));
		// The keys, once, under the block at the prompt size (never under the 18 px floor); gone after a few quiet seconds,
		// back on any input.
		if (HintA > 0.f)
		{
			P.Text(CX - HintW * 0.5f, Size.Y - 50.f * S, HintText, HintFont, Tok(UiToken::InkDim, HintA));
		}

		const FLinearColor Body = View.kind == ftcore::DialogueViewKind::Direction ? Tok(UiToken::InkDim) : Tok(UiToken::Ink);
		const int32 RevealedUnits = CpToUnit.Num() > 0 ? CpToUnit[FMath::Min(static_cast<int32>(View.revealedChars), CpToUnit.Num() - 1)] : 0;
		float LastRight = CX;
		const FLinearColor SpeakerC = ::Shown().Fill; // the speaker in the world's fill
		for (int32 I = PageFirst; I < PageFirst + PageRows; ++I)
		{
			const float Lead = I == PageFirst ? SpeakerW : 0.f; // the speaker heads each page
			float X = CX - (LineW[I] + Lead) * 0.5f;
			const float Y = Top + LineH * (I - PageFirst);
			if (I == PageFirst && !SpeakerUpper.IsEmpty())
			{
				P.Text(X, Y + SpeakerDy, SpeakerUpper, SpeakerFont, SpeakerC);
			}
			X += Lead;
			const int32 End = FMath::Min(Lines[I].Y, RevealedUnits);
			PaintArriving(P, X, Y, Text, Lines[I].X, End, Body);
			LastRight = X + LineW[I];
		}
		if (OptionRows > 0 && (bReadWanted || ConvDim < ConvTarget() - 0.005f || EvenTop > EvenWant + 1.f || GradK < 0.99f) && OptWait < 0.5f)
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
			for (int32 I = PageFirst; I < PageFirst + PageRows; ++I)
			{
				X0 = FMath::Min(X0, CX - (LineW[I] + (I == PageFirst ? SpeakerW : 0.f)) * 0.5f);
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
					ReplyUnderline(Mesh, Line, X0, Y + LineH / 1.15f, S, C); // at the glyph cell's foot, above the next row
				}
			}
		}
		else if (View.continuePrompt)
		{
			const float A = 0.55f + 0.45f * FMath::Sin(Time * 5.f);
			P.Chevron(LastRight + 22.f * S, Top + LineH * FMath::Max(PageRows - 1, 0) + LineH * 0.45f, 18.f * S, Tok(UiToken::Accent, A));
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
			SubBand = 0.f;
			EvenTop = -1.f;
			bReadWanted = false;
		}
	}

	// The gradient's level for the conversation: the soft dialogue backing, or more where a picture asked for it.
	float ConvTarget() const { return FMath::Max(Table().Style.backingSoftDialogue, ConvNeed); }

	// A new beat's picture under the lower screen and under the subtitle's block, asked for once (never per frame; never
	// on a screenshot's frame). FTPictureReader reads it without waiting; TakeReads has it a frame or two later.
	void MeasureAtEndDraw()
	{
		if (!bReadWanted || bAsked || !Shown.IsValid() || FScreenshotRequest::IsScreenshotRequested() || !NeedPx(DimSize, NeedRc))
		{
			return;
		}
		if (!Reader.Request(kNeedSlot, NeedRc))
		{
			return;
		}
		FIntRect SubPx;
		bSubAsked = PicturePx(SubRead, DimSize, SubPx) && Reader.Request(kSubSlot, SubPx);
		bAsked = true;
		bNeedIn = bSubIn = false;
		AskedBeat = ArrivalSerial;
	}

	// The beat's reads, once both are in: the gradient's need, then the subtitle's band.
	void TakeReads()
	{
		bool bOk = false;
		ftcore::UiColor Pic;
		if (Reader.Take(kNeedSlot, Pic, bOk))
		{
			bNeedIn = true;
			NeedOk = bOk;
			NeedPic = Pic;
		}
		if (Reader.Take(kSubSlot, Pic, bOk))
		{
			bSubIn = true;
			SubOk = bOk;
			SubPic = Pic;
		}
		if (!bAsked || !bNeedIn || (bSubAsked && !bSubIn))
		{
			return;
		}
		bAsked = false;
		if (!bReadWanted || AskedBeat != ArrivalSerial || !Shown.IsValid())
		{
			return; // asked for a beat that has gone (a new beat asks again)
		}
		const float Need = NeedOk ? NeedFrom(NeedPic) : -1.f;
		// Below 0: no picture yet; read again next frame, ten tries at most (a truly black scene needs no more).
		if (Need >= 0.f || ++ReadTries >= 10)
		{
			bReadWanted = false;
			ReadTries = 0;
			ConvNeed = FMath::Max(ConvNeed, Need);
			// The subtitle's band: the off-white over the brightest tenth under the block; within a conversation it only
			// comes in, never flickers out between beats.
			if (bSubAsked && SubOk)
			{
				const ftcore::UiColor& Pic2 = SubPic;
				// The words in ink and the speaker in the world's fill: the band the darker of the two asks for.
				const ftcore::UiColor Ink = ftcore::uiColor(Table().Style, ShownWorld(), ShownPalette(), UiToken::Ink);
				const ftcore::UiColor Name = ftcore::uiColor(Table().Style, ShownWorld(), ShownPalette(), UiToken::Fill);
				float Bare = 0.f, Banded = 0.f, NameBare = 0.f, NameBanded = 0.f;
				const float Want = ftcore::uiSubtitleBand(Ink, Pic2, &Bare, &Banded);
				const float NameWant = ftcore::uiSubtitleBand(Name, Pic2, &NameBare, &NameBanded);
				SubBand = FMath::Max3(SubBand, Want, NameWant);
				UE_LOG(LogForgottenThird, Display, TEXT("UI: subtitle contrast %.2f over the picture (%.2f over the band), speaker %.2f (%.2f); band %.2f."),
					Bare, Banded, NameBare, NameBanded, SubBand);
			}
		}
	}

	// The gradient's even part in the game viewport's pixels; false when there is no viewport.
	static bool NeedPx(const FVector2f& Size, FIntRect& Rc)
	{
		FViewport* VP = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
		const FIntPoint VS = VP ? VP->GetSizeXY() : FIntPoint::ZeroValue;
		if (VS.X <= 0 || VS.Y <= 0 || Size.X <= 0.f || Size.Y <= 0.f)
		{
			return false;
		}
		const float S = FMath::Max(0.25f, Size.Y / 1080.f);
		const float KY = VS.Y / Size.Y;
		Rc = FIntRect(FMath::FloorToInt(VS.X * 0.1f), FMath::Clamp(FMath::FloorToInt(VS.Y * kConvEven), 0, VS.Y - 1),
			FMath::CeilToInt(VS.X * 0.9f), FMath::Clamp(FMath::CeilToInt((Size.Y - 40.f * S) * KY), 1, VS.Y));
		return Rc.Width() > 0 && Rc.Height() > 0;
	}

	// The level the option ink needs over the brightest tenth of the gradient's even part (Core uiOptionDim).
	float NeedFrom(const ftcore::UiColor& Pic) const
	{
		const FIntRect& Rc = NeedRc;
		const ftcore::UiStyle& St = Table().Style;
		const ftcore::UiColor Ink = ftcore::uiColor(St, ShownWorld(), ShownPalette(), UiToken::Ink);
		const ftcore::UiColor Back = ftcore::uiColor(St, ShownWorld(), ShownPalette(), UiToken::Backing);
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
		LineH = M->GetMaxCharacterHeight(TextFont) * kSubLineK;
		OptionAsc = M->GetMaxCharacterHeight(OptionFont) + M->GetBaseline(OptionFont); // the baseline under a row's top
		const bool bSpeakerUpper = Table().Style.text[static_cast<size_t>(UiTextRole::Speaker)].upper;
		SpeakerUpper = View.kind != ftcore::DialogueViewKind::Line ? FString() : bSpeakerUpper ? D.GetSpeaker().ToUpper() : D.GetSpeaker();
		SpeakerW = SpeakerUpper.IsEmpty() ? 0.f : TextWidth(SpeakerUpper, SpeakerFont) + kSubSpeakerGap * S;
		SpeakerDy = (M->GetMaxCharacterHeight(TextFont) - M->GetMaxCharacterHeight(SpeakerFont)) * 0.62f;

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

		// Core's fit: 42 characters a line at most, broken at spaces (a longer word stands alone), shown two lines a page.
		static_assert(sizeof(TCHAR) == sizeof(char16_t), "the subtitle's fit reads UTF-16");
		ftcore::uiSubtitleFit(std::u16string_view(reinterpret_cast<const char16_t*>(*T), static_cast<size_t>(T.Len())), Fit);
		Lines.Reset();
		LineW.Reset();
		for (int32 I = 0; I < static_cast<int32>(Fit.count); ++I)
		{
			const int32 B = static_cast<int32>(Fit.line[static_cast<size_t>(I)].begin);
			const int32 E = static_cast<int32>(Fit.line[static_cast<size_t>(I)].end);
			Lines.Add(FIntPoint(B, E));
			LineW.Add(E > B ? static_cast<float>(M->Measure(FStringView(T), B, E - 1, TextFont).X) : 0.f);
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
	TArray<int32> CpToUnit;
	ftcore::UiSubtitleFit Fit; // Core's breaks of the beat's line
	TArray<FIntPoint> Lines; // [start, end) in UTF-16 units, as Fit
	TArray<float> LineW;
	int32 PageFirst = 0; // the first line of the page on show
	int32 PageRows = 0;  // its lines (two at most)
	float SubWidest = 0.f; // the page's widest line, the speaker's lead in
	FBox2f SubRead = FBox2f(ForceInit); // the page's block, for the read of the picture under it
	float SubBand = 0.f; // the band's black now (0 or the least Core uiSubtitleBand measured); only comes in within a conversation
	float GradK = 0.f;   // the low gradient's share: 1 while replies show
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
	// The conversation's gradient.
	float ConvDim = 0.f;      // its level now (eases up, never down, within a conversation)
	float ConvNeed = 0.f;     // the highest level the conversation's pictures have asked for
	float EvenTop = -1.f;     // px: the top of its even part now (-1: the usual place; only rises within a conversation)
	float EvenWant = -1.f;    // px: where that top is going (over the replies while they show)
	bool bReadWanted = false; // a new beat's picture is still to be read
	int32 ReadTries = 0;      // reads that found no picture yet for this beat
	// The beat's two reads (FTPictureReader: asked at the end of the draw, in a frame or two later).
	static constexpr int32 kNeedSlot = 0;
	static constexpr int32 kSubSlot = 1;
	FTPictureReader Reader;
	FIntRect NeedRc;
	ftcore::UiColor NeedPic, SubPic;
	uint32 AskedBeat = 0;
	bool bAsked = false, bSubAsked = false, bNeedIn = false, bSubIn = false, NeedOk = false, SubOk = false;
	float OptWait = 0.f;      // seconds the options on show have waited
	float HiddenFor = 0.f;    // seconds the dialogue has been hidden (the conversation is over after one)
	FVector2f DimSize = FVector2f::ZeroVector; // the widget size the options are placed in, for the end-of-draw read
	bool bDimBound = false;     // the end-of-draw read is bound to the game viewport
	bool bHintUp = false;
	mutable FLineMesh Mesh;
	mutable ftcore::UiStroke Line; // the underline being drawn (Core fills it, the mesh takes it)
};

// The party screen in explore (the new UI, FT::NewUiEnabled): explore has no new-UI party block to open in place, so
// the fight HUD's party rows stand alone, bottom left, while the explore character's screen is open, in the same
// fonts, shade and marks, each row's ink and marks from Core's uiPartyRowView as PaintParty's, fading over
// PartyOpenTime. From the bottom margin up: FOLLOW or WAIT HERE (what picking the row does to the followers) at label
// size; the lead's name at the lead's size with the current lead's dot before it; the partner's at the partner's size
// (with no other playable look PARTNER at PartyRefusedInk with NOT HERE); the Third's name alone at kUiPartyThirdInk
// (no gift slot out of a fight), not drawn without one. The cursor's row at full ink with the underline across its name
// or word, the others at kUiPartyOtherInk. Each row stands PartyRowGap px over the capitals of the one under it.
class SFTExploreParty : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFTExploreParty) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&, TFunction<const AFTExploreCharacter*()> InGetLead)
	{
		GetLead = MoveTemp(InGetLead);
		SetVisibility(EVisibility::HitTestInvisible);
		if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
		{
			std::atomic<bool>* Flush = &TextRuns.bFlush;
			FontRelease = FSlateApplication::Get().GetRenderer()->GetFontServices()->OnReleaseResources().AddLambda(
				[Flush](const FSlateFontCache&) { Flush->store(true); });
		}
	}

	virtual ~SFTExploreParty() override
	{
		if (FontRelease.IsValid() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
		{
			FSlateApplication::Get().GetRenderer()->GetFontServices()->OnReleaseResources().Remove(FontRelease);
		}
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1920.0, 1080.0); }

	virtual void Tick(const FGeometry& Geometry, const double, const float DeltaTime) override
	{
		const AFTExploreCharacter* C = GetLead ? GetLead() : nullptr;
		const bool bOpen = C && C->GetPartyScreen().open;
		OpenK = FMath::Clamp(OpenK + (bOpen ? DeltaTime : -DeltaTime) / Hud::PartyOpenTime, 0.f, 1.f);
		if (!bOpen)
		{
			return; // a closing screen fades with the rows it had
		}
		const float H = Geometry.GetLocalSize().Y;
		const bool bFonts = H != BuiltHeight;
		if (bFonts)
		{
			BuiltHeight = H;
			BuildFonts(H);
		}
		ScreenShown = C->GetPartyScreen();
		const TArray<FFTLeadLook>& Looks = C->PartyLooks;
		const auto LookName = [&Looks](int32 I) -> const FString& { return Looks.IsValidIndex(I) ? Looks[I].Name : NoName(); };
		bPartnerHere = C->IsPartnerHere(); // the same truth the screen's step reads
		bThirdHere = Looks.IsValidIndex(C->GetThirdIndex());
		bFollowing = !C->IsPartyHidden();
		// The controls line names the device in use; both lines are built with the fonts, so this only picks.
		bPad = FTInputBindings::NoteDevice(Cast<APlayerController>(C->GetController())) == ftcore::InputDevice::Pad;
		BuildName(LeadRow, LookName(C->GetLeadIndex()), kLeadUi, LeadNameFont, bFonts);
		BuildName(PartnerRow, LookName(C->GetPartnerIndex()), kPartnerUi, PartnerNameFont, bFonts);
		BuildName(ThirdRow, UFTGameInstance::ThirdName(C), NoName(), PartnerNameFont, bFonts); // the story's current name
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer,
		const FWidgetStyle&, bool) const override
	{
		if (OpenK <= 0.f || BuiltHeight <= 0.f)
		{
			return Layer;
		}
		FTextRuns* Runs = UiTextCacheOn() ? &TextRuns : nullptr;
		if (Runs)
		{
			Runs->BeginFrame();
		}
		const FVector2f Size = Geometry.GetLocalSize();
		const FPainter P{Geometry, Out, Layer, FMath::Max(0.25f, Size.Y / 1080.f), true, Runs};
		const float S = P.S;
		const float K = FMath::SmoothStep(0.f, 1.f, OpenK);
		const float X0 = Hud::Margin * S;
		const float Gap = Hud::PartyRowGap * S;
		const float FollowBase = Size.Y - Hud::Margin * S;
		const float LeadBase = FollowBase - 0.62f * NoteAsc - Gap;
		const float PartnerBase = LeadBase - 0.62f * LeadNameAsc - Gap;
		const float ThirdBase = PartnerBase - 0.62f * PartnerNameAsc - Gap;
		const auto Ink = [K](float Alpha) { return Tok(UiToken::Ink, Alpha * K); };
		const auto RowView = [this](ftcore::UiPartyRow Row, ftcore::UiPartyWhy Why, float W) { return ftcore::uiPartyRowView(ScreenShown, Row, Why, W); };
		Mesh.Begin(Geometry, S);
		// The fight HUD's one shade under the party.
		Mesh.Band(0.f, Size.Y - Hud::BottomShadeH * S, Size.X, Size.Y, true, true, 1.f, Hud::BottomShade * K, Tok(UiToken::Backing, 1.f));
		const ftcore::UiPartyRowView FView = RowView(ftcore::UiPartyRow::Follow, ftcore::UiPartyWhy::Unknown, bFollowing ? WaitHereW : FollowW);
		P.Text(X0, FollowBase - NoteAsc, bFollowing ? WaitHereWord : FollowWord, NoteFont, Ink(FView.ink));
		PartyMarks(Mesh, Line, FView, X0, FollowBase, NoteAsc, S, K, 1.f);
		const ftcore::UiPartyRowView LView = RowView(ftcore::UiPartyRow::Lead, ftcore::UiPartyWhy::Ok, LeadRow.W);
		P.Text(X0, LeadBase - LeadNameAsc, LeadRow.Text, LeadNameFont, Ink(LView.ink));
		PartyMarks(Mesh, Line, LView, X0, LeadBase, LeadNameAsc, S, K, 1.f);
		const ftcore::UiPartyWhy PWhy = bPartnerHere ? ftcore::UiPartyWhy::Unknown : ftcore::UiPartyWhy::NotHere;
		const ftcore::UiPartyRowView PView = RowView(ftcore::UiPartyRow::Partner, PWhy, PartnerRow.W);
		P.Text(X0, PartnerBase - PartnerNameAsc, PartnerRow.Text, PartnerNameFont, Ink(PView.ink));
		if (!bPartnerHere)
		{
			P.Text(X0 + PartnerRow.W + 16.f * S, PartnerBase - NoteAsc, NotHereWord, NoteFont, Ink(Hud::PartyRefusedInk));
		}
		PartyMarks(Mesh, Line, PView, X0, PartnerBase, PartnerNameAsc, S, K, 1.f);
		if (bThirdHere)
		{
			const ftcore::UiPartyRowView TView = RowView(ftcore::UiPartyRow::Third, ftcore::UiPartyWhy::Unknown, ThirdRow.W);
			P.Text(X0, ThirdBase - PartnerNameAsc, ThirdRow.Text, PartnerNameFont, Ink(TView.ink));
		}
		// The controls line, centred at the foot where the field's stands (hidden under the screen): how to work it.
		const FString& Ctl = bPad ? CtlPad : CtlKeys;
		const float CtlX = 0.5f * (Size.X - (bPad ? CtlPadW : CtlKeysW));
		P.Text(CtlX, Size.Y - kCtlBottom * S - NoteAsc, Ctl, NoteFont, Ink(kCtlInk));
		Mesh.Flush(Out, Layer + 1);
		return Layer + 5;
	}

private:
	// A row's name: the look's own in capitals, without a look a plain word; built when the look or the fonts change.
	struct FRowName
	{
		FString Seen;
		FString Text;
		float W = 0.f;
		bool bBuilt = false;
	};

	static const FString& NoName()
	{
		static const FString Empty;
		return Empty;
	}

	static void BuildName(FRowName& R, const FString& Src, const FString& Fallback, const FSlateFontInfo& F, bool bForce)
	{
		if (bForce || !R.bBuilt || !R.Seen.Equals(Src, ESearchCase::CaseSensitive))
		{
			R.bBuilt = true;
			R.Seen = Src;
			R.Text = (Src.IsEmpty() ? Fallback : Src).ToUpper();
			R.W = TextWidth(R.Text, F);
		}
	}

	void BuildFonts(float H)
	{
		const TSharedRef<FSlateFontMeasure> FM = FontMeasure();
		const auto Asc = [&FM](const FSlateFontInfo& F) { return FM->GetMaxCharacterHeight(F) + FM->GetBaseline(F); };
		const auto Spaced = [](FSlateFontInfo F, int32 Tracking = Hud::SpacedTracking)
		{
			F.LetterSpacing = Tracking;
			return F;
		};
		LeadNameFont = Spaced(FontPx(UiTextRole::Title, H, Hud::LeadPx), Hud::NameTracking);
		LeadNameAsc = Asc(LeadNameFont);
		PartnerNameFont = Spaced(FontPx(UiTextRole::Title, H, Hud::PartnerPx), Hud::NameTracking);
		PartnerNameAsc = Asc(PartnerNameFont);
		NoteFont = Spaced(FontPx(UiTextRole::Label, H, Hud::NotePx));
		NoteAsc = Asc(NoteFont);
		FollowWord = kFollowUi.Get().ToUpper();
		WaitHereWord = kWaitHereUi.Get().ToUpper();
		NotHereWord = kNotHereUi.Get().ToUpper();
		FollowW = TextWidth(FollowWord, NoteFont);
		WaitHereW = TextWidth(WaitHereWord, NoteFont);
		CtlKeys = kPartyKeysUi.Get().ToUpper();
		CtlPad = kPartyPadUi.Get().ToUpper();
		CtlKeysW = TextWidth(CtlKeys, NoteFont);
		CtlPadW = TextWidth(CtlPad, NoteFont);
	}

	TFunction<const AFTExploreCharacter*()> GetLead;
	float OpenK = 0.f;
	float BuiltHeight = 0.f;
	ftcore::UiPartyScreen ScreenShown; // as of the last tick it stood open: a closing screen fades with the rows it had
	bool bPartnerHere = false;
	bool bThirdHere = false;
	bool bFollowing = true;
	FRowName LeadRow, PartnerRow, ThirdRow;
	FSlateFontInfo LeadNameFont, PartnerNameFont, NoteFont;
	float LeadNameAsc = 0.f, PartnerNameAsc = 0.f, NoteAsc = 0.f;
	FString FollowWord, WaitHereWord, NotHereWord;
	float FollowW = 0.f, WaitHereW = 0.f;
	FString CtlKeys, CtlPad;
	float CtlKeysW = 0.f, CtlPadW = 0.f;
	bool bPad = false;
	static constexpr float kCtlInk = 0.8f;    // as the field's controls line
	static constexpr float kCtlBottom = 28.f; // its foot above the screen's, as the field's
	mutable FTextRuns TextRuns;
	FDelegateHandle FontRelease;
	mutable FLineMesh Mesh;
	mutable ftcore::UiStroke Line; // the underline being drawn (Core fills it, the mesh takes it)
};

// The explore HUD (the new UI, FT::NewUiEnabled; 2026-11-16 ruling, item A): the r19 HUD's language out of a fight. Top
// left, the party stack: each member's name in capitals over a thin line in the world's fill with its square end cap
// (no figures: explore carries no hurt state); under the stack the objective, one line in small caps, quieting to 40
// percent after 4 s without change (Core uiObjectiveStep). The prompt stands at the focused spot on screen: its key in a
// 1 device px ring and one verb, never centred; with the spot off screen there is none. Notices at the right edge, one
// at a time, 2.5 s each, a queue of 3 (Core UiNoticeQueue). No plate under any word: the words' own soft contour, measured
// (the (A) fix, 2026-11-16): each line's contour is raised in spread and alpha, the least that holds it over the brightest
// tenth of the bare scene under it (Core uiLineGuard: 4.5:1, 3:1 for the names), and at its cap a shade under that line
// only; the quiet objective rests no lower than holds (uiQuietFloor). Hidden while the pause or settings screen is open.
// Play reads one line every kReadEvery s (the one whose words just changed first); a guard rises at once and eases back
// only after kGuardHold s of weaker reads.
// Non-shipping: -UiExploreFake fills every piece with placeholders (and a two-line subtitle, in the dialogue's metrics,
// over the measured band of uiSubtitleBand, the most any of its lines needs); -UiExploreShot=<file prefix> takes the still
// after -UiExploreShotDelay= s (10) and quits; -UiExploreShotYaw= turns the view first; -UiSubScale= sets the placeholder
// subtitle's text scale. Stills read every line twice a second, only rising, and log each ("UI: explore contrast").
#if !UE_BUILD_SHIPPING
const TCHAR* const kFakeObjective = TEXT("Find a way across the canal");
const TCHAR* const kFakeNotice = TEXT("Map updated");
const TCHAR* const kFakePrompt = TEXT("E  Open the gate");
const TCHAR* const kFakeLine = TEXT("Stay close. The lamps along this street went out an hour ago.");
#endif
const TCHAR* const kGuardName[] = {TEXT("lead name"), TEXT("partner name"), TEXT("objective"), TEXT("notice"), TEXT("prompt verb"),
	TEXT("controls"), TEXT("subtitle line 1"), TEXT("subtitle line 2"), TEXT("subtitle speaker")};

class SFTExploreHud : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFTExploreHud) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&, TFunction<const AFTExploreDirector*()> InGetDirector, TFunction<const AFTExploreCharacter*()> InGetLead)
	{
		GetDirector = MoveTemp(InGetDirector);
		GetLead = MoveTemp(InGetLead);
		SetVisibility(EVisibility::HitTestInvisible);
		if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
		{
			std::atomic<bool>* Flush = &TextRuns.bFlush;
			FontRelease = FSlateApplication::Get().GetRenderer()->GetFontServices()->OnReleaseResources().AddLambda(
				[Flush](const FSlateFontCache&) { Flush->store(true); });
		}
#if !UE_BUILD_SHIPPING
		const TCHAR* Cmd = FCommandLine::Get();
		bFake = FParse::Param(Cmd, TEXT("UiExploreFake"));
		// -UiExploreDevice=pad|keys: the controls line on that device whatever was used last (the stills of both).
		FString Dev;
		if (FParse::Value(Cmd, TEXT("UiExploreDevice="), Dev))
		{
			ForcedDevice = Dev.Equals(TEXT("pad"), ESearchCase::IgnoreCase) ? static_cast<int32>(ftcore::InputDevice::Pad)
				: static_cast<int32>(ftcore::InputDevice::Keys);
		}
		bShot = FParse::Value(Cmd, TEXT("UiExploreShot="), ShotPrefix) && !ShotPrefix.IsEmpty();
		FParse::Value(Cmd, TEXT("UiExploreShotDelay="), ShotDelay);
		bShotYaw = FParse::Value(Cmd, TEXT("UiExploreShotYaw="), ShotYaw);
		bSweep = FParse::Param(Cmd, TEXT("UiExploreShotSweep")); // turn a full circle, shoot the view worst for the subtitle
		FParse::Value(Cmd, TEXT("UiSubScale="), FakeSubScale);
		FakeSubScale = FMath::Clamp(FakeSubScale, 1.f, 2.f);
		if (bFake)
		{
			FakeLine = kFakeLine;
		}
		// -UiExplorePlayReads: the play path's reads (one line each, easing back) in a still run, each logged with its cost.
		bPlayReads = FParse::Param(Cmd, TEXT("UiExplorePlayReads"));
		// -UiExploreReadsOff (with -UiExplorePlayReads): the same run with no reads, the frames' baseline.
		bReadsOff = bPlayReads && FParse::Param(Cmd, TEXT("UiExploreReadsOff"));
		// -UiExploreSkipOff (with -UiExplorePlayReads): every read real, the worst case; without it the skip runs as in play.
		bSkipOn = !(bPlayReads && FParse::Param(Cmd, TEXT("UiExploreSkipOff")));
		bSweep = bSweep && !bPlayReads; // the sweep reads every line
		bStills = (bFake || bShot) && !bPlayReads;
#endif
	}

	virtual ~SFTExploreHud() override
	{
		if (bReadBound && GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->OnEndDraw().RemoveAll(this);
		}
		if (FontRelease.IsValid() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
		{
			FSlateApplication::Get().GetRenderer()->GetFontServices()->OnReleaseResources().Remove(FontRelease);
		}
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1920.0, 1080.0); }

	virtual void Tick(const FGeometry& Geometry, const double, const float DeltaTime) override
	{
		Time += DeltaTime;
#if !UE_BUILD_SHIPPING
		if (bPlayReads)
		{
			NoteRunFrame(DeltaTime);
		}
#endif
		const AFTExploreDirector* D = GetDirector ? GetDirector() : nullptr;
		const AFTExploreCharacter* C = GetLead ? GetLead() : nullptr;
		Size = Geometry.GetLocalSize();
		{
			const APlayerController* CamPC = C && C->GetWorld() ? C->GetWorld()->GetFirstPlayerController() : nullptr;
			if (CamPC && CamPC->PlayerCameraManager)
			{
				CamAt = CamPC->PlayerCameraManager->GetCameraLocation();
				CamRot = CamPC->PlayerCameraManager->GetCameraRotation();
			}
			// The scene's clock and wetness, for the read skip: a still camera over a changing scene reads again.
			const UWorld* SceneWorld = C ? C->GetWorld() : nullptr;
			if (const UFTWorldClockSubsystem* SceneClock = SceneWorld ? SceneWorld->GetSubsystem<UFTWorldClockSubsystem>() : nullptr)
			{
				SceneHours = SceneClock->GetHours();
			}
			if (const UFTLookSubsystem* SceneLookSub = SceneWorld ? SceneWorld->GetSubsystem<UFTLookSubsystem>() : nullptr)
			{
				SceneWet = SceneLookSub->GetLook().wetness;
			}
		}
		// Under the screens (ftcore::uiHudDraws): the party screen has the left side while it is open, the map overlay
		// and the pause and settings screens the whole screen; none shows the field's own lines.
		bOn = D && C && ftcore::uiHudDraws(ftcore::UiHudPart::FieldHud, ftcore::UiHudMode::Explore, AFTExploreHUD::ScreensOpen(C));
		TickShot(C);
		TickReads();
		// The controls line's device: noted each frame, also while the HUD is hidden, so the last device used stays fresh
		// (the Button prompts setting may hold one; FTInputBindings::NoteDevice).
		ftcore::InputDevice Device = FTInputBindings::NoteDevice(C ? Cast<APlayerController>(C->GetController()) : nullptr);
#if !UE_BUILD_SHIPPING
		Device = ForcedDevice >= 0 ? static_cast<ftcore::InputDevice>(ForcedDevice) : Device;
#endif
		if (!bOn)
		{
			return;
		}
		const float H = Size.Y;
		if (H != BuiltHeight || FakeSubScale != BuiltSubScale)
		{
			BuiltHeight = H;
			BuiltSubScale = FakeSubScale;
			BuildFonts(H);
		}
		// The field's keys on that device (kept by FTInputBindings::HintLine per device and bindings generation): measured
		// again only when one of those, or the fonts, change.
		const uint32 Bindings = FTInputBindings::Generation();
		if (!CtlText || Device != CtlDevice || Bindings != CtlGeneration)
		{
			CtlDevice = Device;
			CtlGeneration = Bindings;
			CtlText = &FTInputBindings::HintLine(ftcore::InputHintScreen::Field, Device);
			CtlW = TextWidth(*CtlText, CtlFont);
			ReadSoon(GuardControls);
		}
		// The cutscene in place has the screen.
		const AFTCutsceneDirector* Cut = D->GetCutsceneDirector();
		bCutscene = Cut && Cut->IsPlayingInPlace();

		// The party stack's names.
		const TArray<FFTLeadLook>& Looks = C->PartyLooks;
		const auto LookName = [&Looks](int32 I) -> const FString& { return Looks.IsValidIndex(I) ? Looks[I].Name : NoName(); };
		bPartnerHere = C->IsPartnerHere();
		if (BuildName(LeadRow, LookName(C->GetLeadIndex()), kLeadUi, LeadFont))
		{
			ReadSoon(GuardLead);
		}
		if (BuildName(PartnerRow, LookName(C->GetPartnerIndex()), kPartnerUi, PartnerFont))
		{
			ReadSoon(GuardPartner);
		}

		// The objective: the story step's, a world's line, or the demo's current one.
		const FString* Obj = &NoName();
		if (D->IsStoryWalk())
		{
			Obj = &D->GetFieldObjective();
		}
		else if (D->IsWorldMode() || D->IsOverworld())
		{
			Obj = &D->GetWorldLine();
		}
		else
		{
			Obj = &D->GetObjectiveText(D->GetExploration().currentObjective());
		}
#if !UE_BUILD_SHIPPING
		static const FString FakeObj(kFakeObjective);
		Obj = bFake ? &FakeObj : Obj;
#endif
		if (!ObjectiveSeen.Equals(*Obj, ESearchCase::CaseSensitive))
		{
			ObjectiveSeen = *Obj; // copied once per change of the words
			++ObjectiveSerial;
			ObjW = 0.f;
			ReadSoon(GuardObjective);
		}
		if (ObjW <= 0.f && !ObjectiveSeen.IsEmpty())
		{
			ObjW = TextWidth(ObjectiveSeen, ObjFont); // once per change (or new fonts)
		}
		ftcore::uiObjectiveStep(ObjFade, ObjectiveSerial, DeltaTime);

		// Notices: a new one each time the director's notice restarts its age (the same words again are a new notice).
		const FString* Note = &D->GetNotice();
		double NoteAge = D->GetNoticeAge();
#if !UE_BUILD_SHIPPING
		static const FString FakeNote(kFakeNotice);
		if (bFake)
		{
			Note = &FakeNote;
			NoteAge = FMath::Fmod(static_cast<double>(Time), 2.0); // a fresh one every 2 s: the queue always shows one
		}
#endif
		if (!Note->IsEmpty() && NoteAge < LastNoticeAge)
		{
			const std::uint8_t Slot = ftcore::uiNoticePush(Notices);
			NoticeText[Slot] = *Note;
		}
		LastNoticeAge = Note->IsEmpty() ? 1e9 : NoteAge;
		ftcore::uiNoticeStep(Notices, DeltaTime);
		const int32 NoteShown = ftcore::uiNoticeShown(Notices);
		if (NoteShown != NoteSlotSeen)
		{
			NoteSlotSeen = NoteShown;
			if (NoteShown >= 0)
			{
				ReadSoon(GuardNotice); // a notice's words just came up
			}
		}

		// The prompt at its spot, from the director's "<key>  <words>": the key in the ring, the first word the verb.
		bPrompt = false;
		const FString* Prompt = &D->GetPrompt();
		FVector At = FVector::ZeroVector;
		bool bAt = D->GetFocusLocation(At);
#if !UE_BUILD_SHIPPING
		static const FString FakePrompt(kFakePrompt);
		if (bFake)
		{
			Prompt = &FakePrompt;
			const FRotator Yaw(0.f, C->GetControlRotation().Yaw, 0.f);
			At = C->GetActorLocation() + Yaw.RotateVector(FVector(450.f, 140.f, 60.f));
			bAt = true;
		}
#endif
		if (!Prompt->IsEmpty() && bAt)
		{
			if (!PromptSeen.Equals(*Prompt, ESearchCase::CaseSensitive))
			{
				PromptSeen = *Prompt;
				SplitPrompt(PromptSeen);
				ReadSoon(GuardVerb);
			}
			bPrompt = Project(C, At, PromptAt);
		}
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer,
		const FWidgetStyle&, bool) const override
	{
		if (!bOn || bCutscene || BuiltHeight <= 0.f)
		{
			return Layer;
		}
		FTextRuns* Runs = UiTextCacheOn() ? &TextRuns : nullptr;
		if (Runs)
		{
			Runs->BeginFrame();
		}
		const FVector2f Sz = Geometry.GetLocalSize();
		const FPainter P{Geometry, Out, Layer, FMath::Max(0.25f, Sz.Y / 1080.f), false, Runs};
		const float S = P.S;
		const FUiWorldRow& R = Shown();
		Mesh.Begin(Geometry, S);
		for (FGuardLine& L : Lines)
		{
			L.Rect = FBox2f(ForceInit); // set again by each line drawn
		}

		// The party stack, top left: name, then its thin line (full: no value shown) with the cap at its end.
		const float X0 = Hud::Margin * S;
		float Y = Hud::Margin * S;
		const auto Member = [&](const FRowName& Row, const FSlateFontInfo& Font, float Asc, float Ink, float LineLen, float LineW, int32 G)
		{
			FGuardLine& L = Lines[G];
			L.Rect = FBox2f(FVector2f(X0, Y), FVector2f(X0 + Row.W, Y + Asc));
			LineShade(P, L);
			P.TextGuarded(X0, Y, Row.Text, Font, Tok(UiToken::Ink, Ink), L.G);
			const float Base = Y + Asc;
			const float LineY = Base + (0.5f * LineW + 8.f) * S;
			const float WD = Mesh.WholeDev(LineW * S);
			const float YS = Mesh.SnapLineY(LineY, WD);
			ftcore::uiStrokeLine(Line, X0, YS, X0 + LineLen * S, YS, ValueStyle(WD / S, 301, S), 0.f, 1.f);
			Mesh.ValueStroke(Line, R.Fill);
			const float Side = Mesh.WholeDev((LineW + Table().Style.lines.capGrow) * S);
			const float CapL = Mesh.SnapX(ftcore::uiCapX(X0, LineLen * S, 1.f, Side) - 0.5f * Side);
			const float T = Mesh.SnapLineY(YS, Side) - 0.5f * Side;
			const FVector2f Pts[4] = {FVector2f(CapL, T), FVector2f(CapL + Side, T), FVector2f(CapL + Side, T + Side), FVector2f(CapL, T + Side)};
			Mesh.Fill(Pts, 4, Tok(UiToken::Ink));
			Y = LineY + 0.5f * WD + 18.f * S;
		};
		Member(LeadRow, LeadFont, LeadAsc, 1.f, Hud::LeadLine, Hud::PartnerHpW, GuardLead);
		if (bPartnerHere)
		{
			Member(PartnerRow, PartnerFont, PartnerAsc, Hud::PartnerInk, Hud::PartnerLine, Hud::GuardW, GuardPartner);
		}

		// The objective under the stack, quieting no lower than its measured floor.
		if (!ObjectiveSeen.IsEmpty())
		{
			FGuardLine& L = Lines[GuardObjective];
			const float OY = Y + 10.f * S;
			L.Rect = FBox2f(FVector2f(X0, OY), FVector2f(X0 + ObjW, OY + ObjAsc));
			LineShade(P, L);
			P.TextGuarded(X0, OY, ObjectiveSeen, ObjFont, Tok(UiToken::Ink, QuietAlpha(ObjFade.alpha, L.Floor)), L.G);
		}

		// The notice at the right edge, right-aligned.
		const int32 ShownSlot = ftcore::uiNoticeShown(Notices);
		if (ShownSlot >= 0)
		{
			const FString& N = NoticeText[ShownSlot];
			const float W = TextWidth(N, NoteFont); // measured while one stands: a few words, no allocation
			const float NX = Sz.X - Hud::Margin * S - W;
			const float NY = Sz.Y * 0.38f;
			FGuardLine& L = Lines[GuardNotice];
			L.Rect = FBox2f(FVector2f(NX, NY), FVector2f(NX + W, NY + NoteAsc));
			LineShade(P, L);
			P.TextGuarded(NX, NY, N, NoteFont, Tok(UiToken::Ink, ftcore::uiNoticeAlpha(Notices)), L.G);
		}

		// The prompt: the key in a 1 device px ring centred on the spot, the verb after it.
		if (bPrompt)
		{
			const float Rr = 0.5f * Hud::KeyCapH * S;
			const float CX = FMath::Clamp(PromptAt.X, Hud::Margin * S + Rr, Sz.X - Hud::Margin * S - Rr - VerbW - 12.f * S);
			const float CY = FMath::Clamp(PromptAt.Y, Hud::Margin * S + Rr, Sz.Y - (kSubBottom + 140.f) * S);
			constexpr int32 N = 64;
			FVector2f Pts[N + 1];
			for (int32 I = 0; I <= N; ++I)
			{
				const float A = 2.f * UE_PI * static_cast<float>(I) / N;
				Pts[I] = FVector2f(CX + Rr * FMath::Cos(A), CY + Rr * FMath::Sin(A));
			}
			PolyStroke(Line, Pts, N + 1, 0.5f * Mesh.PxSize());
			Mesh.Stroke(Line, Tok(UiToken::Ink), true);
			FGuardLine& L = Lines[GuardVerb];
			const float VX = CX + Rr + 12.f * S;
			const float VY = CapTop(CY, VerbAsc);
			L.Rect = FBox2f(FVector2f(VX, VY), FVector2f(VX + VerbW, VY + VerbAsc));
			LineShade(P, L);
			P.TextGuarded(CX - 0.5f * KeyW, CapTop(CY, KeyAsc), KeyText, KeyFont, Tok(UiToken::Ink), L.G); // the key under the verb's guard
			P.TextGuarded(VX, VY, VerbText, VerbFont, Tok(UiToken::Ink), L.G);
		}

		// The controls line, centred at the foot under the subtitle band: the field's keys on the device in use.
		if (CtlText && !CtlText->IsEmpty())
		{
			FGuardLine& L = Lines[GuardControls];
			const float CXl = 0.5f * (Sz.X - CtlW);
			const float CYl = Sz.Y - kCtlBottom * S - CtlAsc;
			L.Rect = FBox2f(FVector2f(CXl, CYl), FVector2f(CXl + CtlW, CYl + CtlAsc));
			LineShade(P, L);
			P.TextGuarded(CXl, CYl, *CtlText, CtlFont, Tok(UiToken::Ink, kCtlInk), L.G);
		}

#if !UE_BUILD_SHIPPING
		// The placeholder subtitle: the dialogue's block and metrics, the speaker in the world's fill, over the band only
		// where the measured contrast asks for it.
		if (bFake && Fit.count > 0)
		{
			const int32 Rows = FMath::Min<int32>(Fit.count, ftcore::kUiSubtitleLines);
			const float Bottom = Sz.Y - kSubBottom * S;
			const float Top = Bottom - SubLineH * Rows;
			const float CX = Sz.X * 0.5f;
			float Widest = 0.f;
			for (int32 I = 0; I < Rows; ++I)
			{
				Widest = FMath::Max(Widest, SubLineW[I] + (I == 0 ? SpeakerW : 0.f));
			}
			PaintSubtitleBand(P, CX, Top, Bottom, Widest, SubLineH, SubBand());
			for (int32 I = 0; I < Rows; ++I)
			{
				const float Lead = I == 0 ? SpeakerW : 0.f;
				const float X = CX - (SubLineW[I] + Lead) * 0.5f;
				const float LY = Top + SubLineH * I;
				Lines[GuardSub0 + I].Rect = FBox2f(FVector2f(X + Lead, LY), FVector2f(X + Lead + SubLineW[I], LY + SubLineH));
				if (I == 0)
				{
					Lines[GuardSpeaker].Rect = FBox2f(FVector2f(X, LY), FVector2f(X + FMath::Max(1.f, Lead - kSubSpeakerGap * S), LY + SubLineH));
					P.Text(X, LY + SpeakerDy, SpeakerText, SpeakerFont, R.Fill);
				}
				P.Text(X + Lead, LY, FakeLine, SubFont, Tok(UiToken::Ink), static_cast<int32>(Fit.line[I].begin), static_cast<int32>(Fit.line[I].end));
			}
		}
#endif
		Mesh.Flush(Out, Layer + 1);
		return Layer + 5;
	}

private:
	struct FRowName
	{
		FString Seen;
		FString Text;
		float W = 0.f; // its width in its font
		bool bBuilt = false;
	};

	// The lines whose picture is read, each drawn under its measured guard.
	enum EGuardLine : int32 { GuardLead, GuardPartner, GuardObjective, GuardNotice, GuardVerb, GuardControls, GuardSub0, GuardSub1, GuardSpeaker,
		GuardCount };
	static constexpr float kCtlInk = 0.8f;    // the controls line, quieter than the words in play
	static constexpr float kCtlBottom = 28.f; // its foot above the screen's (the subtitle band ends at kSubBottom)
	static_assert(kCtlBottom < kSubBottom - 40.f, "the controls line stays under the subtitle band");
	static_assert(UE_ARRAY_COUNT(kGuardName) == GuardCount, "a name for each read line");
	static constexpr float kReadEvery = 1.5f; // s between reads in play (one line each)
	static constexpr float kGuardHold = 3.f;  // s of weaker reads before a guard eases back
	struct FGuardLine
	{
		FBox2f Rect = FBox2f(ForceInit);              // where it was drawn this frame (invalid: not shown)
		ftcore::UiLineGuard G;                        // drawn with (the subtitle's lines: shade = their band)
		float Floor = ftcore::kUiObjectiveQuiet;      // the objective's quiet level
		float LowSince = -1.f;                        // when a weaker read than G was first seen
		float Bare = -1.f;                            // the last read's bare contrast
		ftcore::UiColor Pic{0.f, 0.f, 0.f, 1.f};      // the last read's brightest tenth
		bool bRead = false;
	};

	static const FString& NoName()
	{
		static const FString Empty;
		return Empty;
	}

	// True when the words (or the fonts) changed: measured again.
	static bool BuildName(FRowName& Row, const FString& Src, const FString& Fallback, const FSlateFontInfo& Font)
	{
		if (!Row.bBuilt || !Row.Seen.Equals(Src, ESearchCase::CaseSensitive))
		{
			Row.bBuilt = true;
			Row.Seen = Src;
			Row.Text = (Src.IsEmpty() ? Fallback : Src).ToUpper();
			Row.W = TextWidth(Row.Text, Font);
			return true;
		}
		return false;
	}

	// The objective's alpha with its quiet level lifted to Floor: 0..quiet maps to 0..Floor, quiet..1 to Floor..1.
	static float QuietAlpha(float A, float Floor)
	{
		const float Q = ftcore::kUiObjectiveQuiet;
		return A <= Q ? A * Floor / Q : Floor + (A - Q) * (1.f - Floor) / (1.f - Q);
	}

	// A small line's shade, under it only, where its contour at the cap fell short.
	static void LineShade(const FPainter& P, const FGuardLine& L)
	{
		if (L.G.shade > 0.f && L.Rect.bIsValid)
		{
			const FVector2f Ext = L.Rect.GetSize();
			PaintSubtitleBand(P, L.Rect.GetCenter().X, L.Rect.Min.Y, L.Rect.Max.Y, Ext.X, Ext.Y, L.G.shade);
		}
	}

	// The subtitle's band: the most any of its lines asks for.
	float SubBand() const { return FMath::Max3(Lines[GuardSub0].G.shade, Lines[GuardSub1].G.shade, Lines[GuardSpeaker].G.shade); }

	float Target(int32 I) const { return I <= GuardPartner ? ftcore::kUiNameMinContrast : ftcore::kUiLineMinContrast; }

	float RestAlpha(int32 I) const
	{
		const FSlateFontInfo* F[GuardSub0] = {&LeadFont, &PartnerFont, &ObjFont, &NoteFont, &VerbFont, &CtlFont};
		return I < GuardSub0 ? F[I]->OutlineSettings.OutlineColor.A : 0.f;
	}

	// The line's ink as drawn at rest: the partner's dimmed, the objective at its quiet level (bRest), the speaker in fill.
	ftcore::UiColor LineInk(int32 I, bool bRest = true) const
	{
		ftcore::UiColor C = ftcore::uiColor(Table().Style, ShownWorld(), ShownPalette(), I == GuardSpeaker ? UiToken::Fill : UiToken::Ink);
		C.a *= I == GuardPartner ? Hud::PartnerInk : I == GuardControls ? kCtlInk : (I == GuardObjective && bRest ? Lines[I].Floor : 1.f);
		return C;
	}

	// The line's contrast as drawn over its last read.
	float DrawnRatio(int32 I) const
	{
		const FGuardLine& L = Lines[I];
		if (I >= GuardSub0)
		{
			return ftcore::uiContrast(LineInk(I), ftcore::UiColor{0.f, 0.f, 0.f, SubBand()}, L.Pic);
		}
		const float K = 1.f - L.G.shade;
		const float Mean = FMath::Max(ftcore::uiContourMean(1.f, RestAlpha(I)), ftcore::uiContourMean(L.G.spread, L.G.alpha));
		return ftcore::uiContrast(LineInk(I), ftcore::UiColor{0.f, 0.f, 0.f, Mean}, ftcore::UiColor{L.Pic.r * K, L.Pic.g * K, L.Pic.b * K, 1.f});
	}

	void ReadSoon(int32 I)
	{
		++TextSerial[I]; // new words: the skip reads again
		ReadFirst = I;
		ReadFirstAt = Time;
	}

	void ResetGuards()
	{
		++ReadGen; // reads asked before this are dropped when they come
		for (int32 I = 0; I < GuardCount; ++I)
		{
			FGuardLine& L = Lines[I];
			L.G = ftcore::UiLineGuard();
			L.Floor = ftcore::kUiObjectiveQuiet;
			L.LowSince = -1.f;
			L.bRead = false;
			Memo[I] = ftcore::UiReadMemo();
		}
	}

	void BuildFonts(float H)
	{
		const TSharedRef<FSlateFontMeasure> FM = FontMeasure();
		const auto Asc = [&FM](const FSlateFontInfo& F) { return static_cast<float>(FM->GetMaxCharacterHeight(F) + FM->GetBaseline(F)); };
		const auto Spaced = [](FSlateFontInfo F, int32 Tracking)
		{
			F.LetterSpacing = Tracking;
			return F;
		};
		LeadFont = Spaced(FontPx(UiTextRole::Title, H, Hud::LeadPx), Hud::NameTracking);
		LeadAsc = Asc(LeadFont);
		PartnerFont = Spaced(FontPx(UiTextRole::Title, H, Hud::PartnerPx), Hud::NameTracking);
		PartnerAsc = Asc(PartnerFont);
		ObjFont = Spaced(FontPx(UiTextRole::Label, H, Hud::RowPx), Hud::SpacedTracking / 2); // small caps (the label face)
		ObjAsc = Asc(ObjFont);
		NoteFont = Spaced(FontPx(UiTextRole::Label, H, Hud::RowPx), Hud::SpacedTracking / 2);
		NoteAsc = Asc(NoteFont);
		KeyFont = FontPx(UiTextRole::Label, H, Hud::KeyPx);
		KeyAsc = Asc(KeyFont);
		VerbFont = Spaced(FontPx(UiTextRole::Label, H, Hud::RowPx), Hud::SpacedTracking);
		VerbAsc = Asc(VerbFont);
		CtlFont = FontPx(UiTextRole::Label, H, Hud::KeyPx);
		CtlAsc = Asc(CtlFont);
		CtlText = nullptr; // measured again in the new font
		ObjW = 0.f;
		PromptSeen.Reset(); // measured again in the new fonts
		LeadRow.bBuilt = false;
		PartnerRow.bBuilt = false;
#if !UE_BUILD_SHIPPING
		if (bFake)
		{
			// The placeholder subtitle in the dialogue's faces at its text scale, broken by Core's 42-character fit.
			const float S = FMath::Max(0.25f, H / 1080.f);
			SubFont = FontFor(UiTextRole::Subtitle, H, FakeSubScale);
			SpeakerFont = FontFor(UiTextRole::Speaker, H, FakeSubScale);
			SubLineH = FM->GetMaxCharacterHeight(SubFont) * kSubLineK;
			SpeakerDy = (FM->GetMaxCharacterHeight(SubFont) - FM->GetMaxCharacterHeight(SpeakerFont)) * 0.62f;
			const AFTExploreCharacter* C = GetLead ? GetLead() : nullptr;
			const FString Name = C && C->PartyLooks.IsValidIndex(C->GetLeadIndex()) ? C->PartyLooks[C->GetLeadIndex()].Name : kLeadUi.Get();
			const bool bUpper = Table().Style.text[static_cast<size_t>(UiTextRole::Speaker)].upper;
			SpeakerText = bUpper ? Name.ToUpper() : Name;
			SpeakerW = TextWidth(SpeakerText, SpeakerFont) + kSubSpeakerGap * S;
			ftcore::uiSubtitleFit(std::u16string_view(reinterpret_cast<const char16_t*>(*FakeLine), static_cast<size_t>(FakeLine.Len())), Fit);
			for (int32 I = 0; I < FMath::Min<int32>(Fit.count, ftcore::kUiSubtitleLines); ++I)
			{
				const int32 B = static_cast<int32>(Fit.line[I].begin);
				const int32 E = static_cast<int32>(Fit.line[I].end);
				SubLineW[I] = E > B ? static_cast<float>(FM->Measure(FStringView(FakeLine), B, E - 1, SubFont).X) : 0.f;
			}
		}
#endif
	}

	// "<key>  <words>": the key alone (one token before two spaces), the verb its first word, in capitals.
	void SplitPrompt(const FString& Src)
	{
		int32 Gap = Src.Find(TEXT("  "));
		FString Key = Gap > 0 ? Src.Left(Gap) : FString();
		FString Rest = Gap > 0 ? Src.Mid(Gap + 2).TrimStart() : Src;
		int32 Space = INDEX_NONE;
		Rest.FindChar(TEXT(' '), Space);
		VerbText = (Space > 0 ? Rest.Left(Space) : Rest).ToUpper();
		KeyText = Key.IsEmpty() ? FString(TEXT("E")) : Key.ToUpper();
		VerbW = TextWidth(VerbText, VerbFont);
		KeyW = TextWidth(KeyText, KeyFont);
	}

	// The spot on screen in the widget's units; false when it is behind the view or off the screen.
	bool Project(const AFTExploreCharacter* C, const FVector& At, FVector2f& OutPt) const
	{
		const APlayerController* PC = C ? Cast<APlayerController>(C->GetController()) : nullptr;
		FViewport* VP = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
		const FIntPoint VS = VP ? VP->GetSizeXY() : FIntPoint::ZeroValue;
		FVector2D Screen;
		if (!PC || VS.X <= 0 || VS.Y <= 0 || !PC->ProjectWorldLocationToScreen(At, Screen, false))
		{
			return false;
		}
		OutPt = FVector2f(static_cast<float>(Screen.X) * Size.X / VS.X, static_cast<float>(Screen.Y) * Size.Y / VS.Y);
		return OutPt.X >= 0.f && OutPt.Y >= 0.f && OutPt.X <= Size.X && OutPt.Y <= Size.Y;
	}

#if !UE_BUILD_SHIPPING
	// -UiExplorePlayReads: every frame of the run from 4 s to just before the still, each marked when it is within a few
	// frames of a read's request or result; logged once (p50, p99, max; the frames near a read against the rest).
	void NoteRunFrame(float Dt)
	{
		const bool bNear = FlagLeft > 0;
		FlagLeft = FMath::Max(0, FlagLeft - 1);
		if (Time > 4.f && Time < ShotDelay - 0.5f)
		{
			if (RunN < kRunKeep)
			{
				RunMs[RunN] = 1000.f * Dt;
				RunNear[RunN] = bNear;
				++RunN;
			}
			return;
		}
		if (!bRunLogged && Time >= ShotDelay - 0.5f && RunN > 0)
		{
			bRunLogged = true;
			LogRun();
		}
	}

	void LogRun()
	{
		FMemory::Memcpy(RunSorted, RunMs, sizeof(float) * RunN);
		Algo::Sort(MakeArrayView(RunSorted, RunN));
		const float P50 = RunSorted[RunN / 2];
		const float P99 = RunSorted[FMath::Min(RunN - 1, static_cast<int32>(0.99f * RunN))];
		int32 Near = 0, NearOver = 0, OtherOver = 0;
		float NearMax = 0.f, OtherMax = 0.f;
		for (int32 I = 0; I < RunN; ++I)
		{
			if (RunNear[I])
			{
				++Near;
				NearMax = FMath::Max(NearMax, RunMs[I]);
				NearOver += RunMs[I] > P99 ? 1 : 0;
			}
			else
			{
				OtherMax = FMath::Max(OtherMax, RunMs[I]);
				OtherOver += RunMs[I] > P99 ? 1 : 0;
			}
		}
		UE_LOG(LogForgottenThird, Display,
			TEXT("UI: explore run frames %d, reads %s: p50 %.2f ms, p99 %.2f, max %.2f; near a read %d frames, max %.2f, %d over p99; other frames max %.2f, %d over p99; reads %d (dropped %d, skipped %d), in %.1f frames mean (max %d), render thread %.3f ms mean (max %.3f)."),
			RunN, bReadsOff ? TEXT("off") : TEXT("on"), P50, P99, RunSorted[RunN - 1], Near, NearMax, NearOver, OtherMax, OtherOver, RunReads, RunDropped, RunSkipped,
			RunReads > 0 ? static_cast<float>(RunLatSum) / RunReads : 0.f, RunLatMax, RunReads > 0 ? RunRtSum / RunReads : 0.f, RunRtMax);
	}
#endif

	// Non-shipping stills: the turn, the reads of the picture under the words, the still, the exit.
	void TickShot(const AFTExploreCharacter* C)
	{
		(void)C;
#if !UE_BUILD_SHIPPING
		if (!bFake && !bShot)
		{
			return;
		}
		APlayerController* SweepPC = C && C->GetWorld() ? C->GetWorld()->GetFirstPlayerController() : nullptr;
		if (bSweep && !bSweepDone && SweepPC && Time > 1.f)
		{
			if (SweepStepAt < 0.f)
			{
				FRotator Rot = SweepPC->GetControlRotation();
				if (SweepStep == 0)
				{
					SweepBase = Rot.Yaw;
				}
				Rot.Yaw = SweepBase + kSweepDeg * static_cast<float>(SweepStep);
				SweepPC->SetControlRotation(Rot);
				SweepStepAt = Time;
			}
			else if (!bSweepPending && Time - SweepStepAt > 0.4f)
			{
				bSweepPending = true; // the next read belongs to this step's view
				bReadWanted = true;
			}
			if (SweepStep >= kSweepSteps)
			{
				FRotator Rot = SweepPC->GetControlRotation();
				Rot.Yaw = SweepWorstYaw;
				SweepPC->SetControlRotation(Rot);
				bSweepDone = true;
				ResetGuards(); // every line is judged again over the view that is shot
				ShotDelay = Time + 3.f;
				UE_LOG(LogForgottenThird, Display, TEXT("UI: explore sweep: worst subtitle view at yaw %.0f (bare %.2f)."),
					SweepWorstYaw, SweepWorst);
			}
		}
		if (bShotYaw && !bYawDone && C && Time > 1.f)
		{
			if (APlayerController* PC = C->GetWorld() ? C->GetWorld()->GetFirstPlayerController() : nullptr)
			{
				FRotator Rot = PC->GetControlRotation();
				Rot.Yaw = ShotYaw;
				PC->SetControlRotation(Rot);
				bYawDone = true;
				ResetGuards(); // judged over the view that is shot, not the one before the turn
			}
		}
		// Stills only rise: judged afresh over the last 2 s before the still (not the start-up exposure).
		if (bShot && !bGuardsFresh && (!bSweep || bSweepDone) && Time >= ShotDelay - 2.f)
		{
			bGuardsFresh = true;
			ResetGuards();
		}
		if (bShot && !bShotTaken && (!bSweep || bSweepDone) && Time >= ShotDelay)
		{
			bShotTaken = true;
			const FString File = ShotPrefix + TEXT(".png");
			FScreenshotRequest::RequestScreenshot(File, true, false);
			UE_LOG(LogForgottenThird, Display, TEXT("UI: explore still %s (world %d, subtitle scale %.1f)."), *File,
				static_cast<int32>(ShownWorld()), FakeSubScale);
		}
		if (bShotTaken && Time >= ShotDelay + 2.f && !bExitAsked)
		{
			bExitAsked = true;
			FPlatformMisc::RequestExit(false, TEXT("UiExploreShot"));
		}
#endif
	}

	// The reads of the picture under the words: asked at the end of the game viewport's draw (the Slate layers are not in
	// it yet, so each is of the bare scene), read by FTPictureReader without waiting, applied here a frame or two later.
	// Stills read every line twice a second; play one line every kReadEvery s in turn, a line whose words just changed
	// first, skipped while nothing under it changed (Core uiReadSkip).
	void TickReads()
	{
		if (!bReadBound && GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->OnEndDraw().AddSP(this, &SFTExploreHud::ReadAtEndDraw);
			bReadBound = true;
		}
		for (int32 I = 0; I < GuardCount; ++I)
		{
			ftcore::UiColor Pic{0.f, 0.f, 0.f, 1.f};
			bool bOk = false;
			int32 Frames = 0;
			float RtMs = 0.f;
			if (!Reader.Take(I, Pic, bOk, &Frames, &RtMs))
			{
				continue;
			}
			--Pending;
			// A result for a line that has since moved, or from before the guards were judged afresh, is dropped.
			const bool bUse = bOk && AskedGen[I] == ReadGen && NearRect(Lines[I].Rect, AskedRect[I]);
			if (bUse)
			{
				ApplyRead(I, Pic);
				ftcore::uiReadNote(Memo[I], AskedView[I], Pic, Time);
			}
#if !UE_BUILD_SHIPPING
			if (bPlayReads)
			{
				FlagLeft = FMath::Max(FlagLeft, 2);
				++RunReads;
				RunDropped += bUse ? 0 : 1;
				RunLatSum += Frames;
				RunLatMax = FMath::Max(RunLatMax, Frames);
				RunRtSum += RtMs;
				RunRtMax = FMath::Max(RunRtMax, RtMs);
				UE_LOG(LogForgottenThird, Display, TEXT("UI: explore play read %s %s in %d frames (render thread %.3f ms) at %.1f s: contrast %.2f; contour %.0f px %.2f, shade %.2f."),
					kGuardName[I], bUse ? TEXT("applied") : TEXT("dropped"), Frames, RtMs, Time, DrawnRatio(I), Lines[I].G.spread, Lines[I].G.alpha,
					I >= GuardSub0 ? SubBand() : Lines[I].G.shade);
			}
			if (bBatch && bUse)
			{
				++BatchGot;
				if (I >= GuardSub0 && I != GuardSpeaker)
				{
					BatchSubBare = BatchSubBare < 0.f ? Lines[I].Bare : FMath::Min(BatchSubBare, Lines[I].Bare);
				}
			}
#endif
		}
#if !UE_BUILD_SHIPPING
		if (bBatch && Pending == 0)
		{
			FinishBatch();
		}
#endif
		if (bOn)
		{
			bReadWanted = bReadWanted || Time - LastRead > (bStills ? 0.5f : kReadEvery) || (ReadFirst >= 0 && Time - ReadFirstAt > 0.1f);
		}
	}

	// The same place: a result is kept while the line has moved under half its height.
	static bool NearRect(const FBox2f& Now, const FBox2f& Asked)
	{
		if (!Now.bIsValid || !Asked.bIsValid)
		{
			return false;
		}
		const float Tol = 0.5f * FMath::Max(1.f, Asked.Max.Y - Asked.Min.Y);
		return FMath::Abs(Now.Min.X - Asked.Min.X) <= Tol && FMath::Abs(Now.Max.X - Asked.Max.X) <= Tol && FMath::Abs(Now.Min.Y - Asked.Min.Y) <= Tol &&
			FMath::Abs(Now.Max.Y - Asked.Max.Y) <= Tol;
	}

	ftcore::UiReadView ViewOf(int32 I) const
	{
		const FBox2f& R = Lines[I].Rect;
		ftcore::UiReadView V;
		V.x0 = R.Min.X;
		V.y0 = R.Min.Y;
		V.x1 = R.Max.X;
		V.y1 = R.Max.Y;
		V.px = static_cast<float>(CamAt.X);
		V.py = static_cast<float>(CamAt.Y);
		V.pz = static_cast<float>(CamAt.Z);
		V.yaw = static_cast<float>(CamRot.Yaw);
		V.pitch = static_cast<float>(CamRot.Pitch);
		V.hours = SceneHours;
		V.wet = SceneWet;
		V.text = TextSerial[I];
		return V;
	}

	bool Ask(int32 I)
	{
		FIntRect Px;
		if (!Lines[I].Rect.bIsValid || !PicturePx(Lines[I].Rect, Size, Px) || !Reader.Request(I, Px))
		{
			return false;
		}
		AskedRect[I] = Lines[I].Rect;
		AskedView[I] = ViewOf(I);
		AskedGen[I] = ReadGen;
		++Pending;
#if !UE_BUILD_SHIPPING
		FlagLeft = 5; // this frame and the next four carry the copy and its result
#endif
		return true;
	}

	void ReadAtEndDraw()
	{
		if (!bReadWanted || Pending > 0 || FScreenshotRequest::IsScreenshotRequested() || !bOn || bCutscene)
		{
			return;
		}
		bReadWanted = false;
		LastRead = Time;
#if !UE_BUILD_SHIPPING
		if (bReadsOff)
		{
			// The control: no read, but the frames where one would be asked, copied and taken are flagged the same way
			// (the ask's five, plus the take's re-arm a frame or two later), so on and off compare like for like.
			ReadFirst = -1;
			FlagLeft = 7;
			return;
		}
#endif
		if (!bStills)
		{
			// One line a read, the one whose words just changed first.
			int32 I = ReadFirst;
			ReadFirst = -1;
			for (int32 K = 0; (I < 0 || !Lines[I].Rect.bIsValid) && K < GuardCount; ++K)
			{
				const int32 J = (ReadNext + K) % GuardCount;
				if (Lines[J].Rect.bIsValid)
				{
					I = J;
					ReadNext = (J + 1) % GuardCount;
				}
			}
			if (I < 0)
			{
				return;
			}
			if (bSkipOn && Lines[I].bRead && ftcore::uiReadSkip(Memo[I], ViewOf(I), Time))
			{
				ApplyRead(I, Lines[I].Pic); // nothing changed: the last read stands (its hold still runs)
#if !UE_BUILD_SHIPPING
				++RunSkipped;
#endif
				return;
			}
			Ask(I);
			return;
		}
#if !UE_BUILD_SHIPPING
		// Stills: every line at once; the batch's tail (the sweep's step, the log) once all are in.
		bBatch = true;
		bBatchSweep = bSweepPending; // the sweep steps on a batch asked after its view settled
		BatchGot = 0;
		BatchSubBare = -1.f;
		for (int32 I = 0; I < GuardCount; ++I)
		{
			Ask(I);
		}
		if (Pending == 0)
		{
			FinishBatch();
		}
#endif
	}

#if !UE_BUILD_SHIPPING
	void FinishBatch()
	{
		bBatch = false;
		if (bBatchSweep && bSweepPending)
		{
			if (BatchSubBare >= 0.f && BatchSubBare < SweepWorst)
			{
				SweepWorst = BatchSubBare;
				SweepWorstYaw = SweepBase + kSweepDeg * static_cast<float>(SweepStep);
			}
			bSweepPending = false;
			SweepStepAt = -1.f;
			++SweepStep;
		}
		LogLines();
		UE_LOG(LogForgottenThird, Display, TEXT("UI: explore reads %d lines (in %.2f s)."), BatchGot, Time - LastRead);
	}
#endif

	// One line's read: the least guard that holds it (the subtitle's lines: their band); it rises at once and eases back
	// only after kGuardHold s of weaker reads (never in a still).
	void ApplyRead(int32 I, const ftcore::UiColor& Pic)
	{
		FGuardLine& L = Lines[I];
		L.Pic = Pic;
		L.bRead = true;
		ftcore::UiLineGuard Want;
		float Floor = ftcore::kUiObjectiveQuiet;
		if (I >= GuardSub0)
		{
			Want.shade = ftcore::uiSubtitleBand(LineInk(I), Pic, &Want.bare, &Want.ratio);
		}
		else
		{
			ftcore::UiColor Ink = LineInk(I, false);
			if (I == GuardObjective)
			{
				Floor = ftcore::uiQuietFloor(Ink, Pic, Target(I), ftcore::kUiObjectiveQuiet);
				Ink.a *= Floor; // judged at rest: the quiet line is the worst it stands
			}
			Want = ftcore::uiLineGuard(Ink, Pic, Target(I), 1.f, RestAlpha(I));
		}
		L.Bare = Want.bare;
		const auto Strength = [](const ftcore::UiLineGuard& G) { return ftcore::uiContourMean(G.spread, G.alpha) + G.shade; };
		const bool bUp = Strength(Want) > Strength(L.G) + 1e-4f;
		const bool bFloorUp = Floor > L.Floor + 1e-4f;
		if (bUp)
		{
			L.G = Want;
		}
		if (bFloorUp)
		{
			L.Floor = Floor;
		}
		const bool bWeaker = Strength(Want) < Strength(L.G) - 1e-4f || Floor < L.Floor - 1e-4f;
		if (!bWeaker || bStills)
		{
			L.LowSince = -1.f;
		}
		else if (L.LowSince < 0.f)
		{
			L.LowSince = Time;
		}
		else if (Time - L.LowSince > kGuardHold)
		{
			L.G = Want;
			L.Floor = Floor;
			L.LowSince = -1.f;
		}
	}

#if !UE_BUILD_SHIPPING
	// Each line's contrast as drawn over its read, with its pixel box (as the menus' log), then the lowest.
	void LogLines() const
	{
		FViewport* VP = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
		const FIntPoint VS = VP ? VP->GetSizeXY() : FIntPoint::ZeroValue;
		const float KX = Size.X > 0.f ? VS.X / Size.X : 1.f;
		const float KY = Size.Y > 0.f ? VS.Y / Size.Y : 1.f;
		float Low = -1.f;
		int32 LowI = -1;
		int32 N = 0;
		for (int32 I = 0; I < GuardCount; ++I)
		{
			const FGuardLine& L = Lines[I];
			if (!L.bRead || !L.Rect.bIsValid)
			{
				continue;
			}
			const float Ratio = DrawnRatio(I);
			++N;
			if (LowI < 0 || Ratio < Low)
			{
				Low = Ratio;
				LowI = I;
			}
			UE_LOG(LogForgottenThird, Display,
				TEXT("UI: explore contrast %.2f (bare %.2f, ink %.2f, target %.1f) px %d,%d..%d,%d: %s; contour %.0f px %.2f, shade %.2f."), Ratio,
				ftcore::uiContrast(LineInk(I), ftcore::UiColor{0.f, 0.f, 0.f, 0.f}, L.Pic), LineInk(I).a, I >= GuardSub0 ? ftcore::kUiSubtitleMinContrast : Target(I),
				FMath::FloorToInt(L.Rect.Min.X * KX), FMath::FloorToInt(L.Rect.Min.Y * KY), FMath::CeilToInt(L.Rect.Max.X * KX),
				FMath::CeilToInt(L.Rect.Max.Y * KY), kGuardName[I], I >= GuardSub0 ? 0.f : L.G.spread, I >= GuardSub0 ? 0.f : L.G.alpha,
				I >= GuardSub0 ? SubBand() : L.G.shade);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("UI: explore contrast lowest %.2f (%s) over %d lines; band %.2f, objective quiet %.2f."), Low,
			LowI >= 0 ? kGuardName[LowI] : TEXT("none"), N, SubBand(), Lines[GuardObjective].Floor);
	}
#endif

	TFunction<const AFTExploreDirector*()> GetDirector;
	TFunction<const AFTExploreCharacter*()> GetLead;
	float Time = 0.f;
	FVector2f Size = FVector2f::ZeroVector;
	bool bOn = false;
	bool bCutscene = false;
	float BuiltHeight = 0.f;
	float BuiltSubScale = -1.f;
	bool bPartnerHere = false;
	FRowName LeadRow, PartnerRow;
	FSlateFontInfo LeadFont, PartnerFont, ObjFont, NoteFont, KeyFont, VerbFont;
	float LeadAsc = 0.f, PartnerAsc = 0.f, ObjAsc = 0.f, NoteAsc = 0.f, KeyAsc = 0.f, VerbAsc = 0.f;
	// The objective.
	FString ObjectiveSeen;
	uint32 ObjectiveSerial = 0;
	float ObjW = 0.f;
	ftcore::UiObjectiveFade ObjFade;
	// The notices.
	ftcore::UiNoticeQueue Notices;
	FString NoticeText[ftcore::kUiNoticeQueue];
	double LastNoticeAge = 1e9;
	// The prompt.
	bool bPrompt = false;
	FVector2f PromptAt = FVector2f::ZeroVector;
	FString PromptSeen, KeyText, VerbText;
	float KeyW = 0.f, VerbW = 0.f;
	// The controls line (its words are FTInputBindings::HintLine's, kept there).
	FSlateFontInfo CtlFont;
	float CtlAsc = 0.f, CtlW = 0.f;
	const FString* CtlText = nullptr;
	ftcore::InputDevice CtlDevice = ftcore::InputDevice::Keys;
	uint32 CtlGeneration = 0;
	mutable FTextRuns TextRuns; // the words' kept glyph runs, drawn from in OnPaint
	FDelegateHandle FontRelease;
	// The reads and each line's guard.
	mutable FGuardLine Lines[GuardCount];
	FTPictureReader Reader; // one slot per line
	static_assert(GuardCount <= FTPictureReader::kSlots, "a read slot for each guarded line");
	FBox2f AskedRect[GuardCount];
	ftcore::UiReadView AskedView[GuardCount];
	uint32 AskedGen[GuardCount] = {};
	ftcore::UiReadMemo Memo[GuardCount]; // the skip's memory of each line's real reads
	uint32 ReadGen = 0;
	int32 Pending = 0; // reads asked, not yet in
	bool bSkipOn = true;
	FVector CamAt = FVector::ZeroVector;
	FRotator CamRot = FRotator::ZeroRotator;
	float SceneHours = 0.f; // the world clock and the look's wetness, for the read skip
	float SceneWet = 0.f;
	uint32 TextSerial[GuardCount] = {}; // each line's words, for the read skip
	bool bReadBound = false;
	bool bReadWanted = false;
	bool bStills = false; // -UiExploreFake or -UiExploreShot: every line each read, logged, only rising
	float LastRead = -1.f;
	int32 ReadFirst = -1;
	float ReadFirstAt = 0.f;
	int32 ReadNext = 0;
	int32 NoteSlotSeen = -1;
	// The placeholder subtitle and the stills.
	float FakeSubScale = 1.f;
	bool bFake = false;
#if !UE_BUILD_SHIPPING
	int32 ForcedDevice = -1; // -UiExploreDevice=: the controls line's device (-1: the one in use)
	// -UiExplorePlayReads: the run's frame times (and -UiExploreReadsOff: the same run without reads).
	static constexpr int32 kRunKeep = 4096;
	float RunMs[kRunKeep] = {};
	float RunSorted[kRunKeep] = {};
	bool RunNear[kRunKeep] = {};
	int32 RunN = 0;
	int32 FlagLeft = 0;
	bool bRunLogged = false;
	bool bReadsOff = false;
	int32 RunReads = 0, RunDropped = 0, RunSkipped = 0, RunLatSum = 0, RunLatMax = 0;
	float RunRtSum = 0.f, RunRtMax = 0.f;
	// Stills: a batch of reads (every line), its tail once all are in.
	bool bBatch = false;
	bool bBatchSweep = false;
	int32 BatchGot = 0;
	float BatchSubBare = -1.f;
	bool bShot = false;
	bool bShotTaken = false;
	bool bExitAsked = false;
	FString ShotPrefix;
	float ShotDelay = 10.f;
	bool bShotYaw = false;
	bool bYawDone = false;
	bool bGuardsFresh = false;
	bool bPlayReads = false;
	static constexpr int32 kSweepSteps = 12;
	static constexpr float kSweepDeg = 30.f;
	bool bSweep = false;
	bool bSweepDone = false;
	bool bSweepPending = false;
	int32 SweepStep = 0;
	float SweepStepAt = -1.f;
	float SweepBase = 0.f;
	float SweepWorst = 1000.f;
	float SweepWorstYaw = 0.f;
	float ShotYaw = 0.f;
	FString FakeLine;
	ftcore::UiSubtitleFit Fit;
	FSlateFontInfo SubFont, SpeakerFont;
	FString SpeakerText;
	float SubLineH = 0.f, SpeakerDy = 0.f, SpeakerW = 0.f;
	float SubLineW[ftcore::kUiSubtitleLines] = {};
#endif
	mutable FLineMesh Mesh;
	mutable ftcore::UiStroke Line;
};

// The pause menu and the settings screen (the new UI, FT::NewUiEnabled; 2026-11-16 ruling, item B): one leaf widget on
// the game viewport (FT::EnsureMenuScreens, from the Canvas overlays' entry points, which then draw nothing). Words on
// the left over the world, no panels: the paused world under a light dim and a shade, even under all the words and fading
// out right of them, whose strength is measured: the least (kMenuShade and up) that holds every text line at 4.5:1 over
// the brightest tenth of the bare scene under it (one read of the picture when a screen opens or changes). The selection is the r19 value line under the word, in the world's fill, with its square ink end cap. The pause
// list (Resume, Settings, Quit to Title, Quit Game; a greyed item at kMenuOffInk) or, in its place, a quit item's Yes / No
// prompt. Settings: its six sections on one line under the title (measured: they fit at 200% text at 1080), the selected
// row's section marked; under them that section's rows, label and value, the value's two chevrons on the selected row;
// Back under the rows; a hint at the foot. Everything is drawn at the Text size row's scale (a live preview) and in the
// Colours row's palette (FT::SetUiPalette). Input stays with the subsystems (controller events); this widget reports
// where it drew each row, button and section in viewport pixels and passes the mouse on for hover.
// Non-shipping: -UiMenuShot=<file prefix> opens the menu after -UiMenuShotDelay= s (8) on the map's own HUD, takes the
// still 1.5 s later and quits; -UiMenuShotOf=pause|settings (settings); -UiMenuShotSection=<0..5> picks the section;
// -UiTextScale=<100..200> holds the screens' text size; -UiPalette=<0..4> the palette (ShownPalette); -UiMenuShotSweep
// first turns the camera a full circle in 12 steps and shoots the brightest view. Each read logs "UI: menu contrast".
const FTUiText kMenuPauseTitleUi(TEXT("pause.title"));
const FTUiText kMenuResumeUi(TEXT("pause.resume"));
const FTUiText kMenuSettingsUi(TEXT("pause.settings"));
const FTUiText kMenuQuitTitleUi(TEXT("pause.quit_title"));
const FTUiText kMenuQuitGameUi(TEXT("pause.quit_game"));
const FTUiText kMenuAskTitleUi(TEXT("pause.ask_title"));
const FTUiText kMenuAskGameUi(TEXT("pause.ask_game"));
const FTUiText kMenuAskNoteUi(TEXT("pause.ask_note"));
const FTUiText kMenuYesUi(TEXT("pause.yes"));
const FTUiText kMenuNoUi(TEXT("pause.no"));
const FTUiText kMenuHintUi(TEXT("pause.hint"));
const FTUiText kMenuAskHintUi(TEXT("pause.ask_hint"));
// The settings screen's hint on the device in use (A cycles a row's value, B goes back), and its section names.
const FTUiText kMenuSettingsHintUi(TEXT("settings.hint"));
const FTUiText kMenuSettingsHintPadUi(TEXT("settings.hint_pad"));
const FTUiText kMenuSectionTextUi(TEXT("settings.section.text"));
const FTUiText kMenuSectionSoundUi(TEXT("settings.section.sound"));
const FTUiText kMenuSectionPlayUi(TEXT("settings.section.play"));
const FTUiText kMenuSectionCameraUi(TEXT("settings.section.camera"));
const FTUiText kMenuSectionDisplayUi(TEXT("settings.section.display"));
const FTUiText kMenuSectionAccessUi(TEXT("settings.section.access"));

constexpr float kMenuOffInk = 0.4f;    // a greyed pause item
constexpr float kMenuRestInk = 0.85f;  // a row, section or button not selected
constexpr float kMenuDim = 0.35f;      // the whole paused world, in the backing colour
constexpr float kMenuShade = 0.6f;     // the shade under the words at least: even under all of them, then fading out
constexpr float kMenuShadeMax = 0.94f; // the most the shade rises to for a bright scene
constexpr float kMenuShadeFall = 480.f; // px (at 1080) the shade takes to fade out right of the words
constexpr float kMenuContrast = 4.6f;  // what every text line is held to over the brightest tenth under it (4.5:1 and a margin)
constexpr int32 kMenuSweepSteps = 12;  // shots: the turn that finds the brightest view (30 degrees a step)
constexpr float kMenuLineW = 3.f;      // the selection's value line
constexpr float kMenuLineDrop = 12.f;  // under the word's baseline
constexpr float kMenuTitleTop = 150.f; // the title's top, down from the top
constexpr float kMenuRowStep = 1.85f;  // a row's step, in its capitals' ascent
constexpr float kMenuRowStepMin = 1.2f; // the tightest a settings row's step goes (large text); past it the section shows a window
constexpr int32 kMenuMaxShown = static_cast<int32>(ftcore::kSettingsRowCount);
constexpr int32 kMenuMaxLines = 2 * kMenuMaxShown + 16; // every text line a screen draws (the contrast read)

// One text line's box (widget px), its ink alpha and a name for the shots' log.
struct FMenuLine
{
	FBox2f Rc = FBox2f(ForceInit);
	float InkA = 1.f;
	FString Name;
};

ftcore::UiColor MenuOver(const ftcore::UiColor& Under, const ftcore::UiColor& C, float A)
{
	return ftcore::UiColor{FMath::Lerp(Under.r, C.r, A), FMath::Lerp(Under.g, C.g, A), FMath::Lerp(Under.b, C.b, A), 1.f};
}

class SFTMenuScreens : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFTMenuScreens) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		SetVisibility(EVisibility::HitTestInvisible);
		if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
		{
			std::atomic<bool>* Flush = &TextRuns.bFlush;
			FontRelease = FSlateApplication::Get().GetRenderer()->GetFontServices()->OnReleaseResources().AddLambda(
				[Flush](const FSlateFontCache&) { Flush->store(true); });
		}
#if !UE_BUILD_SHIPPING
		const TCHAR* Cmd = FCommandLine::Get();
		bShot = FParse::Value(Cmd, TEXT("UiMenuShot="), ShotPrefix);
		FString Of;
		FParse::Value(Cmd, TEXT("UiMenuShotOf="), Of);
		bShotPause = Of.Equals(TEXT("pause"), ESearchCase::IgnoreCase);
		bShotSweep = FParse::Param(Cmd, TEXT("UiMenuShotSweep"));
		FParse::Value(Cmd, TEXT("UiMenuShotDelay="), ShotDelay);
		FParse::Value(Cmd, TEXT("UiMenuShotSection="), ShotSection);
		ShotSection = FMath::Clamp(ShotSection, 0, static_cast<int32>(ftcore::kSettingsGroupCount) - 1);
		int32 Forced = 0;
		if (FParse::Value(Cmd, TEXT("UiTextScale="), Forced))
		{
			ForcedTextScale = FMath::Clamp(Forced, ftcore::Settings::kMinTextScale, ftcore::Settings::kMaxTextScale) / 100.f;
		}
#endif
	}

	virtual ~SFTMenuScreens() override
	{
		if (FontRelease.IsValid() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
		{
			FSlateApplication::Get().GetRenderer()->GetFontServices()->OnReleaseResources().Remove(FontRelease);
		}
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1920.0, 1080.0); }

	virtual void Tick(const FGeometry& Geometry, const double, const float) override
	{
		UGameViewportClient* VC = GEngine ? GEngine->GameViewport : nullptr;
		UWorld* World = VC ? VC->GetWorld() : nullptr;
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		UFTPauseSubsystem* Pause = GI ? GI->GetSubsystem<UFTPauseSubsystem>() : nullptr;
		UFTSettingsSubsystem* Set = GI ? GI->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
		APlayerController* PC = GI ? GI->GetFirstLocalPlayerController() : nullptr;
		if (!bReadBound && VC)
		{
			VC->OnEndDraw().AddSP(this, &SFTMenuScreens::ReadAtEndDraw);
			bReadBound = true;
		}
		const FVector2f Size = Geometry.GetLocalSize();
		LastSize = Size;
		TickShot(World, Pause, Set, PC);
		bSettings = Set && Set->IsOpen();
		bPause = !bSettings && Pause && Pause->IsOpen();
		if (!bSettings && !bPause)
		{
			ReadKey = 0;
			return;
		}
		if (Size.X <= 0.f || Size.Y <= 0.f)
		{
			return;
		}
		float T = Set ? Set->Get().uiTextScale() : 1.f;
#if !UE_BUILD_SHIPPING
		T = ForcedTextScale > 0.f ? ForcedTextScale : T;
#endif
		const ftcore::InputDevice Device = FTInputBindings::NoteDevice(PC); // the hint names this device's buttons
		if (Size.Y != BuiltHeight || T != BuiltScale || Device != BuiltDevice)
		{
			BuiltHeight = Size.Y;
			BuiltScale = T;
			BuiltDevice = Device;
			ShownSize = Size; // before the fonts: their log line reads it
			BuildFonts(Size.Y, T, Set);
		}
		S = FMath::Max(0.25f, Size.Y / 1080.f);
		ShownSize = Size;
		FVector2D VP(Size.X, Size.Y);
		if (VC)
		{
			VC->GetViewportSize(VP);
		}
		ToVpX = static_cast<float>(VP.X) / Size.X;
		ToVpY = static_cast<float>(VP.Y) / Size.Y;
		if (bSettings)
		{
			LayoutSettings(*Set);
		}
		else
		{
			LayoutPause(*Pause);
		}
		// A screen that opened or changed what it shows (screen, section, ask, palette, rows' window, size, text size) reads the
		// scene under it again;
		// on open the shade starts from its least.
		uint32 Key = (bSettings ? 2u : 0u) | (bConfirm && !bSettings ? 4u : 0u) | (static_cast<uint32>(ShownGroup) << 3) |
			(static_cast<uint32>(ShownPalette()) << 6) | (static_cast<uint32>(bSettings ? RowStart : 0) << 9);
		Key = HashCombineFast(Key, GetTypeHash(FMath::RoundToInt(BuiltScale * 100.f)));
		Key = HashCombineFast(Key, GetTypeHash(FMath::RoundToInt(Size.Y))) | 1u; // 0 is "no screen open"
		if (Key != ReadKey)
		{
			if (ReadKey == 0)
			{
				ShadePeak = kMenuShade;
			}
			ReadKey = Key;
			bReadWanted = true;
		}
		float MX = 0.f;
		float MY = 0.f;
		if (PC && PC->GetMousePosition(MX, MY))
		{
			if (bSettings)
			{
				Set->UpdateHover(FVector2D(MX, MY));
			}
			else
			{
				Pause->UpdateHover(FVector2D(MX, MY));
			}
		}
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer,
		const FWidgetStyle&, bool) const override
	{
		if ((!bSettings && !bPause) || BuiltHeight <= 0.f)
		{
			return Layer;
		}
		FTextRuns* Runs = UiTextCacheOn() ? &TextRuns : nullptr;
		if (Runs)
		{
			Runs->BeginFrame();
		}
		const FVector2f Size = Geometry.GetLocalSize();
		const FPainter P{Geometry, Out, Layer, S, false, Runs};
		Mesh.Begin(Geometry, S);
		Mesh.Rect(0.f, 0.f, Size.X, Size.Y, Tok(UiToken::Backing, kMenuDim), false);
		Mesh.Rect(0.f, 0.f, TextRight, Size.Y, Tok(UiToken::Backing, ShadePeak), false);
		Mesh.Band(TextRight, 0.f, ShadeRight, Size.Y, false, false, 1.f, ShadePeak, Tok(UiToken::Backing, 1.f));
		if (bSettings)
		{
			PaintSettings(P);
		}
		else
		{
			PaintPause(P);
		}
		Mesh.Flush(Out, Layer + 1);
		return Layer + 5;
	}

private:
	static FSlateFontInfo Tracked(UiTextRole Role, float H, float T)
	{
		return FontFor(Role, H, T);
	}

	static FString CasedFor(UiTextRole Role, const FString& In)
	{
		return Table().Style.text[static_cast<size_t>(Role)].upper ? In.ToUpper() : In;
	}

	void BuildFonts(float H, float T, const UFTSettingsSubsystem* Set)
	{
		const TSharedRef<FSlateFontMeasure> FM = FontMeasure();
		const auto Asc = [&FM](const FSlateFontInfo& F) { return FM->GetMaxCharacterHeight(F) + FM->GetBaseline(F); };
		TitleFont = Tracked(UiTextRole::Title, H, T);
		TitleAsc = Asc(TitleFont);
		HeadFont = Tracked(UiTextRole::Heading, H, T);
		HeadAsc = Asc(HeadFont);
		HeadEm = HeadFont.Size / 0.75f;
		RowFont = Tracked(UiTextRole::Label, H, T);
		RowAsc = Asc(RowFont);
		RowEm = RowFont.Size / 0.75f;
		NoteFont = Tracked(UiTextRole::Label, H, T);
		NoteFont.Size *= 0.85f;
		NoteAsc = Asc(NoteFont);

		PauseTitle = CasedFor(UiTextRole::Title, kMenuPauseTitleUi.Get());
		PauseTitleW = TextWidth(PauseTitle, TitleFont);
		const FTUiText* const Items[UFTPauseSubsystem::RowCount] = {&kMenuResumeUi, &kMenuSettingsUi, &kMenuQuitTitleUi, &kMenuQuitGameUi};
		MaxItemW = 0.f;
		for (int32 I = 0; I < UFTPauseSubsystem::RowCount; ++I)
		{
			ItemWord[I] = CasedFor(UiTextRole::Label, Items[I]->Get());
			ItemW[I] = TextWidth(ItemWord[I], RowFont);
			MaxItemW = FMath::Max(MaxItemW, ItemW[I]);
		}
		AskTitle = kMenuAskTitleUi.Get();
		AskGame = kMenuAskGameUi.Get();
		AskNote = kMenuAskNoteUi.Get();
		YesWord = CasedFor(UiTextRole::Label, kMenuYesUi.Get());
		NoWord = CasedFor(UiTextRole::Label, kMenuNoUi.Get());
		YesW = TextWidth(YesWord, RowFont);
		NoW = TextWidth(NoWord, RowFont);
		AskW = FMath::Max3(TextWidth(AskTitle, HeadFont), TextWidth(AskGame, HeadFont), TextWidth(AskNote, NoteFont));
		PauseHint = kMenuHintUi.Get();
		AskHint = kMenuAskHintUi.Get();

		SettingsTitle = CasedFor(UiTextRole::Title, kMenuSettingsUi.Get());
		SettingsHint = (BuiltDevice == ftcore::InputDevice::Pad ? kMenuSettingsHintPadUi : kMenuSettingsHintUi).Get();
		const float Usable = FMath::Max(0.f, ShownSize.X - 2.f * Hud::Margin * FMath::Max(0.25f, H / 1080.f));
		FitHint(PauseHint, PauseHint2, Usable, NoteFont);
		FitHint(AskHint, AskHint2, Usable, NoteFont);
		FitHint(SettingsHint, SettingsHint2, Usable, NoteFont);
		PauseHintW = FMath::Max(TextWidth(PauseHint, NoteFont), TextWidth(PauseHint2, NoteFont));
		AskHintW = FMath::Max(TextWidth(AskHint, NoteFont), TextWidth(AskHint2, NoteFont));
		SettingsHintW = FMath::Max(TextWidth(SettingsHint, NoteFont), TextWidth(SettingsHint2, NoteFont));
		SettingsTitleW = TextWidth(SettingsTitle, TitleFont);
		float X = 0.f;
		const FTUiText* const Sections[] = {&kMenuSectionTextUi, &kMenuSectionSoundUi, &kMenuSectionPlayUi, &kMenuSectionCameraUi,
			&kMenuSectionDisplayUi, &kMenuSectionAccessUi};
		static_assert(UE_ARRAY_COUNT(Sections) == ftcore::kSettingsGroupCount, "a name for each section, in SettingsGroup order");
		for (int32 G = 0; G < static_cast<int32>(ftcore::kSettingsGroupCount); ++G)
		{
			SectionWord[G] = CasedFor(UiTextRole::Heading, Sections[G]->Get());
			SectionW[G] = TextWidth(SectionWord[G], HeadFont);
			SectionX[G] = X; // from the margin
			X += SectionW[G] + HeadEm; // one em between names
		}
		SectionLineW = X - HeadEm;
		MaxLabelW = 0.f;
		for (int32 R = 0; R < UFTSettingsSubsystem::RowCount; ++R)
		{
			LabelW[R] = Set ? TextWidth(Set->GetRowLabel(R), RowFont) : 0.f;
			MaxLabelW = FMath::Max(MaxLabelW, LabelW[R]);
		}
#if !UE_BUILD_SHIPPING
		UE_LOG(LogForgottenThird, Display, TEXT("UI: menu fonts at text %.0f%%: section line %.0f of %.0f px usable (height %.0f)."), T * 100.f,
			SectionLineW, FMath::Max(0.f, ShownSize.X - 2.f * Hud::Margin * FMath::Max(0.25f, H / 1080.f)), H);
#endif
	}

	// A hint too wide for the line (200% text on a tall screen) breaks at the gap (four spaces) nearest
	// its middle; it is never shrunk. Second is empty when the hint fits.
	static void FitHint(FString& Line, FString& Second, float Usable, const FSlateFontInfo& Font)
	{
		Second.Reset();
		const float Full = TextWidth(Line, Font);
		if (Full <= Usable)
		{
			return;
		}
		int32 Best = INDEX_NONE;
		float BestOff = TNumericLimits<float>::Max();
		for (int32 At = Line.Find(TEXT("    ")); At != INDEX_NONE;
			 At = Line.Find(TEXT("    "), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 4))
		{
			const float Off = FMath::Abs(TextWidth(Line.Left(At), Font) - 0.5f * Full);
			if (Off < BestOff)
			{
				BestOff = Off;
				Best = At;
			}
		}
		if (Best != INDEX_NONE)
		{
			Second = Line.Mid(Best).TrimStart();
			Line.LeftInline(Best);
		}
	}

	// The hint's first line sits one step above the bottom one when it broke.
	float HintFirst(const FString& Second) const { return Second.IsEmpty() ? HintTop : HintTop - 1.5f * NoteAsc; }

	void PaintHint(const FPainter& P, const FString& First, const FString& Second) const
	{
		P.Text(X0, HintFirst(Second), First, NoteFont, Tok(UiToken::Ink, Hud::NoteInk));
		if (!Second.IsEmpty())
		{
			P.Text(X0, HintTop, Second, NoteFont, Tok(UiToken::Ink, Hud::NoteInk));
		}
	}

	FBox2D ToVp(float L, float T, float R, float B) const
	{
		return FBox2D(FVector2D(L * ToVpX, T * ToVpY), FVector2D(R * ToVpX, B * ToVpY));
	}

	// A word's hit box: the capitals with a little air round them.
	FBox2D WordBox(float X, float Top, float W, float Asc) const
	{
		return ToVp(X - 16.f * S, Top - 0.35f * Asc, X + W + 16.f * S, Top + 1.45f * Asc);
	}

	void LayoutPause(UFTPauseSubsystem& Pause)
	{
		const ftcore::PauseMenu& Menu = Pause.GetMenu();
		X0 = Hud::Margin * S;
		TitleTop = kMenuTitleTop * S;
		const float First = TitleTop + TitleAsc + 72.f * S;
		const float Step = kMenuRowStep * RowAsc;
		bConfirm = Menu.confirming();
		bAskGame = Menu.selected() == ftcore::PauseItem::QuitGame;
		bYes = Menu.confirmYes();
		Selected = static_cast<int32>(Menu.selected());
		for (int32 I = 0; I < UFTPauseSubsystem::RowCount; ++I)
		{
			RowTop[I] = First + Step * I;
			bItemOn[I] = Menu.enabled(static_cast<ftcore::PauseItem>(I));
			Pause.SetRowRect(I, bConfirm ? FBox2D(ForceInit) : WordBox(X0, RowTop[I], ItemW[I], RowAsc));
		}
		AskTop = First;
		NoteTop = AskTop + HeadAsc + 24.f * S;
		ChoiceTop = NoteTop + NoteAsc + 56.f * S;
		NoX = X0 + YesW + 2.f * RowEm;
		Pause.SetChoiceRect(0, bConfirm ? WordBox(X0, ChoiceTop, YesW, RowAsc) : FBox2D(ForceInit));
		Pause.SetChoiceRect(1, bConfirm ? WordBox(NoX, ChoiceTop, NoW, RowAsc) : FBox2D(ForceInit));
		HintTop = ShownSize.Y - Hud::Margin * S - NoteAsc;
		TextRight = X0 + FMath::Max3(bConfirm ? FMath::Max(AskW, YesW + 2.f * RowEm + NoW) : MaxItemW, PauseTitleW, bConfirm ? AskHintW : PauseHintW);
		ShadeRight = TextRight + kMenuShadeFall * S;
	}

	void LayoutSettings(UFTSettingsSubsystem& Set)
	{
		X0 = Hud::Margin * S;
		TitleTop = kMenuTitleTop * S * 0.6f;
		SectionTop = TitleTop + TitleAsc + 40.f * S;
		Selected = Set.GetSelectedRow();
		const ftcore::SettingsRow SelRow = static_cast<ftcore::SettingsRow>(Selected);
		if (SelRow != ftcore::SettingsRow::Back)
		{
			ShownGroup = static_cast<int32>(ftcore::settingsRowGroup(SelRow)); // Back keeps the section it came from
		}
		for (int32 G = 0; G < static_cast<int32>(ftcore::kSettingsGroupCount); ++G)
		{
			Set.SetSectionRect(G, WordBox(X0 + SectionX[G], SectionTop, SectionW[G], HeadAsc));
		}
		ValueX = X0 + MaxLabelW + 3.f * RowEm;
		ValueColW = 9.f * RowEm;
		HintTop = ShownSize.Y - Hud::Margin * S - NoteAsc;
		// The section's rows (Back aside), in screen order.
		int32 InSection[kMenuMaxShown];
		int32 NumIn = 0;
		int32 SelAt = INDEX_NONE;
		for (uint32 I = 0; I < ftcore::kSettingsRowCount; ++I)
		{
			const ftcore::SettingsRow Row = ftcore::settingsScreenRow(I);
			const int32 R = static_cast<int32>(Row);
			Set.SetRowRect(R, FBox2D(ForceInit));
			if (Row != ftcore::SettingsRow::Back && static_cast<int32>(ftcore::settingsRowGroup(Row)) == ShownGroup && NumIn < kMenuMaxShown)
			{
				SelAt = R == Selected ? NumIn : SelAt;
				InSection[NumIn++] = R;
			}
		}
		// Below the section line: a gap, the rows, Back 0.6 of a step under them and 1.6 steps above the hint. At a large text
		// size that does not fit at kMenuRowStep: the gap tightens, then the step (to kMenuRowStepMin); past that the section
		// shows a window of its rows that follows the selection, with a chevron on the side rows are hidden.
		const float Room = HintFirst(SettingsHint2) - SectionTop - HeadAsc;
		const auto Fits = [Room](float G, float St, int32 N) { return G + St * (static_cast<float>(N) + 2.2f) <= Room; };
		float Gap = 72.f * S;
		float Step = kMenuRowStep * RowAsc;
		if (!Fits(Gap, Step, NumIn))
		{
			Gap = 40.f * S;
			Step = FMath::Clamp((Room - Gap) / (static_cast<float>(NumIn) + 2.2f), kMenuRowStepMin * RowAsc, Step);
		}
		int32 Fit = NumIn;
		while (Fit > 1 && !Fits(Gap, Step, Fit))
		{
			--Fit;
		}
		RowStart = Fit < NumIn ? FMath::Clamp((SelAt == INDEX_NONE ? NumIn : SelAt) - Fit / 2, 0, NumIn - Fit) : 0;
		bRowsAbove = RowStart > 0;
		bRowsBelow = RowStart + Fit < NumIn;
		const float First = SectionTop + HeadAsc + Gap;
		const float Half = FMath::Min(0.9f * RowAsc, 0.5f * Step); // a row's hit box, never into its neighbour's
		NumShown = 0;
		for (int32 N = RowStart; N < RowStart + Fit && N < NumIn; ++N)
		{
			const int32 R = InSection[N];
			ShownRow[NumShown] = R;
			RowTop[NumShown] = First + Step * NumShown;
			const float Mid = RowTop[NumShown] + 0.55f * RowAsc;
			Set.SetRowRect(R, ToVp(X0 - 16.f * S, Mid - Half, ValueX + ValueColW + 2.f * RowEm, Mid + Half));
			++NumShown;
		}
		BackTop = FMath::Max(First + Step * NumShown + 0.6f * Step, 0.f);
		BackTop = FMath::Min(BackTop, HintFirst(SettingsHint2) - 1.6f * Step);
		const int32 BackRow = static_cast<int32>(ftcore::SettingsRow::Back);
		Set.SetRowRect(BackRow, WordBox(X0, BackTop, LabelW[BackRow], RowAsc));
		// Left of the value's middle steps down, right of it up (the subsystem's rule for a click).
		Set.SetValueColumn((ValueX - 1.5f * RowEm) * ToVpX, (ValueX + 0.5f * ValueColW) * ToVpX);
		TextRight = X0 + FMath::Max3(FMath::Max(SectionLineW, SettingsTitleW), ValueX - X0 + ValueColW + (bRowsAbove || bRowsBelow ? 3.f * RowEm : 0.f),
			SettingsHintW);
		ShadeRight = TextRight + kMenuShadeFall * S;
		RowsSet = &Set;
	}

	// The selection: the r19 value line under the word, in the world's fill, its square ink end cap at the word's end.
	void SelectMark(float X, float W, float Top, float Asc) const
	{
		const float Y = Mesh.SnapLineY(Top + Asc + kMenuLineDrop * S, kMenuLineW * S);
		ftcore::uiStrokeLine(Line, X, Y, X + W, Y, ValueStyle(kMenuLineW, 307, S), 0.f, 1.f);
		Mesh.ValueStroke(Line, FillTok());
		const float Side = Mesh.WholeDev((kMenuLineW + Table().Style.lines.capGrow) * S);
		const float L = Mesh.SnapX(X + W - 0.5f * Side);
		const float T = Mesh.SnapLineY(Y, Side) - 0.5f * Side;
		const FVector2f Pts[4] = {FVector2f(L, T), FVector2f(L + Side, T), FVector2f(L + Side, T + Side), FVector2f(L, T + Side)};
		Mesh.Fill(Pts, 4, Tok(UiToken::Ink));
	}

	// A value's chevron, pointing left (Dir -1) or right, centred on the capitals' middle.
	void Chevron(float CX, float MidY, float Dir) const
	{
		const float H = 0.32f * RowAsc;
		const FVector2f Pts[3] = {FVector2f(CX - 0.5f * Dir * H, MidY - H), FVector2f(CX + 0.5f * Dir * H, MidY), FVector2f(CX - 0.5f * Dir * H, MidY + H)};
		PolyStroke(Line, Pts, 3, FMath::Max(1.f, 1.25f * S));
		Mesh.Stroke(Line, Tok(UiToken::Ink));
	}

	// The settings rows' window: a chevron pointing up (Dir -1) or down at the side rows are hidden.
	void MoreMark(float CX, float MidY, float Dir) const
	{
		const float H = 0.32f * RowAsc;
		const FVector2f Pts[3] = {FVector2f(CX - H, MidY - 0.5f * Dir * H), FVector2f(CX, MidY + 0.5f * Dir * H), FVector2f(CX + H, MidY - 0.5f * Dir * H)};
		PolyStroke(Line, Pts, 3, FMath::Max(1.f, 1.25f * S));
		Mesh.Stroke(Line, Tok(UiToken::Ink, kMenuRestInk));
	}

	void PaintPause(const FPainter& P) const
	{
		P.Text(X0, TitleTop, PauseTitle, TitleFont, Tok(UiToken::Ink));
		if (!bConfirm)
		{
			for (int32 I = 0; I < UFTPauseSubsystem::RowCount; ++I)
			{
				const bool bSel = I == Selected && bItemOn[I];
				P.Text(X0, RowTop[I], ItemWord[I], RowFont, Tok(UiToken::Ink, !bItemOn[I] ? kMenuOffInk : bSel ? 1.f : kMenuRestInk));
				if (bSel)
				{
					SelectMark(X0, ItemW[I], RowTop[I], RowAsc);
				}
			}
			PaintHint(P, PauseHint, PauseHint2);
			return;
		}
		P.Text(X0, AskTop, bAskGame ? AskGame : AskTitle, HeadFont, Tok(UiToken::Ink));
		P.Text(X0, NoteTop, AskNote, NoteFont, Tok(UiToken::Ink, Hud::NoteInk));
		P.Text(X0, ChoiceTop, YesWord, RowFont, Tok(UiToken::Ink, bYes ? 1.f : kMenuRestInk));
		P.Text(NoX, ChoiceTop, NoWord, RowFont, Tok(UiToken::Ink, bYes ? kMenuRestInk : 1.f));
		if (bYes)
		{
			SelectMark(X0, YesW, ChoiceTop, RowAsc);
		}
		else
		{
			SelectMark(NoX, NoW, ChoiceTop, RowAsc);
		}
		PaintHint(P, AskHint, AskHint2);
	}

	void PaintSettings(const FPainter& P) const
	{
		const UFTSettingsSubsystem* Set = RowsSet;
		if (!Set)
		{
			return;
		}
		P.Text(X0, TitleTop, SettingsTitle, TitleFont, Tok(UiToken::Ink));
		for (int32 G = 0; G < static_cast<int32>(ftcore::kSettingsGroupCount); ++G)
		{
			const bool bSel = G == ShownGroup;
			P.Text(X0 + SectionX[G], SectionTop, SectionWord[G], HeadFont, Tok(UiToken::Ink, bSel ? 1.f : kMenuRestInk));
			if (bSel)
			{
				SelectMark(X0 + SectionX[G], SectionW[G], SectionTop, HeadAsc);
			}
		}
		for (int32 N = 0; N < NumShown; ++N)
		{
			const int32 R = ShownRow[N];
			const bool bSel = R == Selected;
			const float Ink = bSel ? 1.f : kMenuRestInk;
			P.Text(X0, RowTop[N], Set->GetRowLabel(R), RowFont, Tok(UiToken::Ink, Ink));
			P.Text(ValueX, RowTop[N], Set->GetRowValue(R), RowFont, Tok(UiToken::Ink, Ink));
			if (bSel)
			{
				SelectMark(X0, LabelW[R], RowTop[N], RowAsc);
				const float Mid = CapMid(RowTop[N] + RowAsc, RowAsc);
				Chevron(ValueX - 1.f * RowEm, Mid, -1.f);
				Chevron(ValueX + ValueColW + 1.f * RowEm, Mid, 1.f);
			}
		}
		if (NumShown > 0 && bRowsAbove)
		{
			MoreMark(ValueX + ValueColW + 2.5f * RowEm, CapMid(RowTop[0] + RowAsc, RowAsc), -1.f);
		}
		if (NumShown > 0 && bRowsBelow)
		{
			MoreMark(ValueX + ValueColW + 2.5f * RowEm, CapMid(RowTop[NumShown - 1] + RowAsc, RowAsc), 1.f);
		}
		const int32 BackRow = static_cast<int32>(ftcore::SettingsRow::Back);
		const bool bBack = Selected == BackRow;
		P.Text(X0, BackTop, Set->GetRowLabel(BackRow), RowFont, Tok(UiToken::Ink, bBack ? 1.f : kMenuRestInk));
		if (bBack)
		{
			SelectMark(X0, LabelW[BackRow], BackTop, RowAsc);
		}
		PaintHint(P, SettingsHint, SettingsHint2);
	}

	void AddLine(float X, float Top, float W, float Asc, float InkA, const FString& Name)
	{
		if (NumLines >= kMenuMaxLines || W <= 0.f || InkA <= 0.f)
		{
			return;
		}
		FMenuLine& L = Lines[NumLines++];
		L.Rc = FBox2f(FVector2f(X, Top - 0.35f * Asc), FVector2f(X + W, Top + 1.45f * Asc));
		L.InkA = InkA;
		L.Name = Name;
	}

	// Every text line the shown screen draws, a choosable one at its resting ink (the selection moves without a new read; the
	// selected one only reads brighter). A greyed pause item is left out: it says it is off.
	void CollectLines()
	{
		NumLines = 0;
		const auto HintLines = [this](const FString& First, const FString& Second)
		{
			AddLine(X0, HintFirst(Second), TextWidth(First, NoteFont), NoteAsc, Hud::NoteInk, TEXT("hint"));
			if (!Second.IsEmpty())
			{
				AddLine(X0, HintTop, TextWidth(Second, NoteFont), NoteAsc, Hud::NoteInk, TEXT("hint, second line"));
			}
		};
		if (bPause)
		{
			AddLine(X0, TitleTop, PauseTitleW, TitleAsc, 1.f, PauseTitle);
			if (!bConfirm)
			{
				for (int32 I = 0; I < UFTPauseSubsystem::RowCount; ++I)
				{
					if (bItemOn[I])
					{
						AddLine(X0, RowTop[I], ItemW[I], RowAsc, kMenuRestInk, ItemWord[I]);
					}
				}
				HintLines(PauseHint, PauseHint2);
				return;
			}
			const FString& Ask = bAskGame ? AskGame : AskTitle;
			AddLine(X0, AskTop, TextWidth(Ask, HeadFont), HeadAsc, 1.f, Ask);
			AddLine(X0, NoteTop, TextWidth(AskNote, NoteFont), NoteAsc, Hud::NoteInk, AskNote);
			AddLine(X0, ChoiceTop, YesW, RowAsc, kMenuRestInk, YesWord);
			AddLine(NoX, ChoiceTop, NoW, RowAsc, kMenuRestInk, NoWord);
			HintLines(AskHint, AskHint2);
			return;
		}
		const UFTSettingsSubsystem* Rows = RowsSet;
		if (!Rows)
		{
			return;
		}
		AddLine(X0, TitleTop, SettingsTitleW, TitleAsc, 1.f, SettingsTitle);
		for (int32 G = 0; G < static_cast<int32>(ftcore::kSettingsGroupCount); ++G)
		{
			AddLine(X0 + SectionX[G], SectionTop, SectionW[G], HeadAsc, kMenuRestInk, SectionWord[G]);
		}
		for (int32 N = 0; N < NumShown; ++N)
		{
			const int32 R = ShownRow[N];
			const float InkA = kMenuRestInk;
			AddLine(X0, RowTop[N], LabelW[R], RowAsc, InkA, Rows->GetRowLabel(R));
			const FString& Value = Rows->GetRowValue(R);
			AddLine(ValueX, RowTop[N], TextWidth(Value, RowFont), RowAsc, InkA, Value);
		}
		const int32 BackRow = static_cast<int32>(ftcore::SettingsRow::Back);
		AddLine(X0, BackTop, LabelW[BackRow], RowAsc, kMenuRestInk, Rows->GetRowLabel(BackRow));
		HintLines(SettingsHint, SettingsHint2);
	}

	// A line's contrast: its ink over the picture under the dim and the shade at Peak.
	static float MenuRatio(const ftcore::UiColor& Fg, const ftcore::UiColor& Pic, const ftcore::UiColor& Back, float Peak)
	{
		const ftcore::UiColor Under = MenuOver(MenuOver(Pic, Back, kMenuDim), Back, Peak);
		return ftcore::uiContrast(Fg, ftcore::UiColor{0.f, 0.f, 0.f, 0.f}, Under);
	}

	// At the end of the game viewport's draw (the Slate layers are not in the picture yet, so this is the bare scene), when a
	// screen opened or changed: one read of the picture under all its words, then for each line the least shade that holds it
	// at kMenuContrast over the brightest tenth under it; the shade takes the most any line needs (only rising while open).
	void ReadAtEndDraw()
	{
		if (!bReadWanted || FScreenshotRequest::IsScreenshotRequested())
		{
			return;
		}
		FViewport* VP = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
		const FIntPoint VS = VP ? VP->GetSizeXY() : FIntPoint::ZeroValue;
		if (VS.X <= 0 || VS.Y <= 0 || LastSize.X <= 0.f || LastSize.Y <= 0.f)
		{
			return;
		}
#if !UE_BUILD_SHIPPING
		if (bSweepPending)
		{
			bReadWanted = false;
			bSweepPending = false;
			const float S0 = FMath::Max(0.25f, LastSize.Y / 1080.f);
			ftcore::UiColor Pic;
			if (PictureBrightest(FBox2f(FVector2f(Hud::Margin * S0, 0.08f * LastSize.Y), FVector2f(0.6f * LastSize.X, LastSize.Y - Hud::Margin * S0)),
					LastSize, Pic))
			{
				const float Luma = 0.2126f * Pic.r + 0.7152f * Pic.g + 0.0722f * Pic.b;
				if (Luma > SweepBest)
				{
					SweepBest = Luma;
					SweepBestYaw = SweepBase + 30.f * static_cast<float>(SweepStep);
				}
			}
			SweepStepAt = -1.0;
			++SweepStep;
			return;
		}
#endif
		if ((!bSettings && !bPause) || BuiltHeight <= 0.f)
		{
			return;
		}
		bReadWanted = false;
		CollectLines();
		if (NumLines == 0)
		{
			return;
		}
		const float KX = VS.X / LastSize.X;
		const float KY = VS.Y / LastSize.Y;
		const auto ToPx = [KX, KY, VS](const FBox2f& Rc)
		{
			return FIntRect(FMath::Clamp(FMath::FloorToInt(Rc.Min.X * KX), 0, VS.X), FMath::Clamp(FMath::FloorToInt(Rc.Min.Y * KY), 0, VS.Y),
				FMath::Clamp(FMath::CeilToInt(Rc.Max.X * KX), 0, VS.X), FMath::Clamp(FMath::CeilToInt(Rc.Max.Y * KY), 0, VS.Y));
		};
		FBox2f All(ForceInit);
		for (int32 I = 0; I < NumLines; ++I)
		{
			All += Lines[I].Rc;
		}
		const FIntRect R = ToPx(All);
		ReadPx.Reset();
		if (R.Width() <= 0 || R.Height() <= 0 || !VP->ReadPixels(ReadPx, FReadSurfaceDataFlags(), R) || ReadPx.Num() != R.Width() * R.Height())
		{
			return;
		}
		const ftcore::UiStyle& St = Table().Style;
		const ftcore::UiColor Ink = ftcore::uiColor(St, ShownWorld(), ShownPalette(), UiToken::Ink);
		const ftcore::UiColor Back = ftcore::uiColor(St, ShownWorld(), ShownPalette(), UiToken::Backing);
		ftcore::UiColor Pics[kMenuMaxLines];
		bool bRead[kMenuMaxLines] = {};
		float Need = kMenuShade;
		for (int32 I = 0; I < NumLines; ++I)
		{
			FIntRect Sub = ToPx(Lines[I].Rc);
			Sub.Clip(R);
			if (Sub.Width() <= 0 || Sub.Height() <= 0)
			{
				continue;
			}
			ftcore::UiLumaHistogram Hist;
			for (int32 Y = Sub.Min.Y; Y < Sub.Max.Y; ++Y)
			{
				const FColor* Row = ReadPx.GetData() + static_cast<int64>(Y - R.Min.Y) * R.Width();
				for (int32 X = Sub.Min.X; X < Sub.Max.X; ++X)
				{
					const FColor& Q = Row[X - R.Min.X];
					ftcore::uiLumaAdd(Hist, ftcore::UiColor{Q.R / 255.f, Q.G / 255.f, Q.B / 255.f, 1.f});
				}
			}
			Pics[I] = ftcore::uiLumaBrightest(Hist, 0.1f);
			bRead[I] = true;
			const ftcore::UiColor Fg{Ink.r, Ink.g, Ink.b, Ink.a * Lines[I].InkA};
			float Peak = kMenuShade;
			while (Peak < kMenuShadeMax && MenuRatio(Fg, Pics[I], Back, Peak) < kMenuContrast)
			{
				Peak += 0.02f;
			}
			Need = FMath::Max(Need, FMath::Min(Peak, kMenuShadeMax));
		}
		ShadePeak = FMath::Max(ShadePeak, Need);
#if !UE_BUILD_SHIPPING
		float Lowest = TNumericLimits<float>::Max();
		int32 LowestAt = INDEX_NONE;
		for (int32 I = 0; I < NumLines; ++I)
		{
			if (!bRead[I])
			{
				continue;
			}
			const ftcore::UiColor Fg{Ink.r, Ink.g, Ink.b, Ink.a * Lines[I].InkA};
			const float Ratio = MenuRatio(Fg, Pics[I], Back, ShadePeak);
			const FIntRect Px = ToPx(Lines[I].Rc);
			UE_LOG(LogForgottenThird, Display, TEXT("UI: menu contrast %.2f (bare %.2f, ink %.2f) px %d,%d..%d,%d: %s"), Ratio,
				ftcore::uiContrast(Fg, ftcore::UiColor{0.f, 0.f, 0.f, 0.f}, Pics[I]), Lines[I].InkA, Px.Min.X, Px.Min.Y, Px.Max.X, Px.Max.Y,
				*Lines[I].Name);
			if (Ratio < Lowest)
			{
				Lowest = Ratio;
				LowestAt = I;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("UI: menu contrast lowest %.2f (%s) over %d lines; shade %.2f, dim %.2f (%s)."), Lowest,
			LowestAt != INDEX_NONE ? *Lines[LowestAt].Name : TEXT("none"), NumLines, ShadePeak, kMenuDim,
			bSettings ? TEXT("settings") : TEXT("pause"));
#endif
	}

	void TickShot(UWorld* World, UFTPauseSubsystem* Pause, UFTSettingsSubsystem* Set, APlayerController* PC)
	{
		(void)World;
		(void)Pause;
		(void)Set;
		(void)PC;
#if !UE_BUILD_SHIPPING
		if (!bShot)
		{
			return;
		}
		const double Now = FPlatformTime::Seconds();
		if (ShotStart < 0.0)
		{
			ShotStart = Now;
		}
		// The sweep (-UiMenuShotSweep): a step of the turn, a read of the bare scene half a second later, the next step; then
		// back to the brightest view, and the menu opens over it.
		if (bShotSweep && !bSweepDone && PC && Now - ShotStart >= ShotDelay)
		{
			if (SweepStep >= kMenuSweepSteps)
			{
				FRotator Rot = PC->GetControlRotation();
				Rot.Yaw = SweepBestYaw;
				PC->SetControlRotation(Rot);
				bSweepDone = true;
				SweepDoneAt = Now;
				UE_LOG(LogForgottenThird, Display, TEXT("UI: menu sweep: brightest view at yaw %.0f (brightest tenth luma %.2f)."), SweepBestYaw, SweepBest);
			}
			else if (SweepStepAt < 0.0)
			{
				FRotator Rot = PC->GetControlRotation();
				if (SweepStep == 0)
				{
					SweepBase = Rot.Yaw;
					SweepBestYaw = Rot.Yaw;
				}
				Rot.Yaw = SweepBase + 30.f * static_cast<float>(SweepStep);
				PC->SetControlRotation(Rot);
				SweepStepAt = Now;
			}
			else if (!bSweepPending && Now - SweepStepAt > 0.5)
			{
				bSweepPending = true;
				bReadWanted = true;
			}
		}
		if (!bShotOpened && Now - ShotStart >= ShotDelay && (!bShotSweep || (bSweepDone && Now - SweepDoneAt >= 1.5)) && World && Pause && Set)
		{
			bShotOpened = true;
			ShotOpenedAt = Now;
			const bool bOpen = Pause->Open(World);
			if (!bShotPause)
			{
				Set->Open(Cast<AFTPlayerController>(PC));
				Set->SelectRow(UFTSettingsSubsystem::FirstRowOf(ShotSection));
			}
			UE_LOG(LogForgottenThird, Display, TEXT("UI: menu shot opened %s (pause %s, section %d)."), bShotPause ? TEXT("pause") : TEXT("settings"),
				bOpen ? TEXT("open") : TEXT("refused"), ShotSection);
		}
		if (bShotOpened && !bShotTaken && Now - ShotOpenedAt >= 1.5)
		{
			bShotTaken = true;
			ShotTakenAt = Now;
			const FString File = ShotPrefix + TEXT(".png");
			FScreenshotRequest::RequestScreenshot(File, true, false);
			UE_LOG(LogForgottenThird, Display, TEXT("UI: menu still %s (world %d, palette %d, text %.0f%%)."), *File, static_cast<int32>(ShownWorld()),
				static_cast<int32>(ShownPalette()), BuiltScale * 100.f);
		}
		if (bShotTaken && !bExitAsked && Now - ShotTakenAt >= 2.0)
		{
			bExitAsked = true;
			FPlatformMisc::RequestExit(false, TEXT("UiMenuShot"));
		}
#endif
	}

	bool bPause = false;
	bool bSettings = false;
	float S = 1.f;
	FVector2f ShownSize = FVector2f(1920.f, 1080.f);
	float ToVpX = 1.f;
	float ToVpY = 1.f;
	float BuiltHeight = 0.f;
	float BuiltScale = 0.f;
	ftcore::InputDevice BuiltDevice = ftcore::InputDevice::Keys;
	FSlateFontInfo TitleFont, HeadFont, RowFont, NoteFont;
	float TitleAsc = 0.f, HeadAsc = 0.f, RowAsc = 0.f, NoteAsc = 0.f, HeadEm = 0.f, RowEm = 0.f;

	// pause
	FString PauseTitle, AskTitle, AskGame, AskNote, YesWord, NoWord, PauseHint, AskHint, PauseHint2, AskHint2;
	FString ItemWord[UFTPauseSubsystem::RowCount];
	float ItemW[UFTPauseSubsystem::RowCount] = {};
	bool bItemOn[UFTPauseSubsystem::RowCount] = {};
	float MaxItemW = 0.f, YesW = 0.f, NoW = 0.f, AskW = 0.f, PauseTitleW = 0.f, PauseHintW = 0.f, AskHintW = 0.f;
	bool bConfirm = false, bAskGame = false, bYes = false;
	float AskTop = 0.f, NoteTop = 0.f, ChoiceTop = 0.f, NoX = 0.f;

	// settings
	FString SettingsTitle, SettingsHint, SettingsHint2;
	FString SectionWord[ftcore::kSettingsGroupCount];
	float SectionW[ftcore::kSettingsGroupCount] = {};
	float SectionX[ftcore::kSettingsGroupCount] = {};
	float SectionLineW = 0.f, SettingsTitleW = 0.f, SettingsHintW = 0.f;
	float LabelW[UFTSettingsSubsystem::RowCount] = {};
	float MaxLabelW = 0.f;
	int32 ShownGroup = 0;
	int32 ShownRow[kMenuMaxShown] = {};
	int32 NumShown = 0;
	float SectionTop = 0.f, ValueX = 0.f, ValueColW = 0.f, BackTop = 0.f;
	int32 RowStart = 0; // the section's first shown row (a window at a large text size)
	bool bRowsAbove = false, bRowsBelow = false;
	const UFTSettingsSubsystem* RowsSet = nullptr; // this tick's (the game instance outlives the frame)

	// both
	int32 Selected = 0;
	float X0 = 0.f, TitleTop = 0.f, HintTop = 0.f, ShadeRight = 0.f, TextRight = 0.f;
	float RowTop[kMenuMaxShown] = {};

	// the scrim's measure
	FVector2f LastSize = FVector2f::ZeroVector;
	float ShadePeak = kMenuShade;
	uint32 ReadKey = 0;
	bool bReadWanted = false;
	bool bReadBound = false;
	FMenuLine Lines[kMenuMaxLines];
	int32 NumLines = 0;
	TArray<FColor> ReadPx;

#if !UE_BUILD_SHIPPING
	bool bShot = false;
	bool bShotPause = false;
	FString ShotPrefix;
	float ShotDelay = 8.f;
	int32 ShotSection = 0;
	float ForcedTextScale = 0.f;
	double ShotStart = -1.0;
	double ShotOpenedAt = 0.0;
	double ShotTakenAt = 0.0;
	bool bShotOpened = false;
	bool bShotTaken = false;
	bool bExitAsked = false;
	bool bShotSweep = false;
	bool bSweepDone = false;
	bool bSweepPending = false;
	int32 SweepStep = 0;
	double SweepStepAt = -1.0;
	double SweepDoneAt = 0.0;
	float SweepBase = 0.f;
	float SweepBest = -1.f;
	float SweepBestYaw = 0.f;
#endif

	mutable FTextRuns TextRuns;
	FDelegateHandle FontRelease;
	mutable FLineMesh Mesh;
	mutable ftcore::UiStroke Line;
};

TWeakPtr<SWidget> GMenuScreens;
TWeakObjectPtr<UGameViewportClient> GMenuViewport;
}

namespace FT
{
bool NewUiEnabled()
{
	// The one switch every HUD asks (FTArenaHUD, FTStoryHUD, FTExploreHUD) and the party
	// screen's input (FTArenaRunner, FTExploreCharacter). A Shipping build always has the new UI: it parses no flag and
	// has no ft.NewUi. Outside Shipping the new UI is the default: -FTOldUi brings the old Canvas HUD back for one more
	// round and wins over ft.NewUi, ft.NewUi decides otherwise, and -FTNewUi (which turned the new UI on before it was
	// the default) is a no-op: it still parses (noted in the log once) and changes nothing.
#if UE_BUILD_SHIPPING
	return true;
#else
	static const bool bOldUi = []()
	{
		if (FParse::Param(FCommandLine::Get(), TEXT("FTNewUi")))
		{
			UE_LOG(LogForgottenThird, Log, TEXT("UI: -FTNewUi does nothing now: the new UI is the default (-FTOldUi gives the old HUD)."));
		}
		return FParse::Param(FCommandLine::Get(), TEXT("FTOldUi"));
	}();
	return !bOldUi && CVarNewUi.GetValueOnGameThread() != 0;
#endif
}

TSharedRef<SWidget> MakeArenaHudWidget(TFunction<const UFTArenaRunner*()> GetRunner)
{
	return SNew(SFTArenaHud, MoveTemp(GetRunner));
}

TSharedRef<SWidget> MakeDialogueWidget(TFunction<const AFTStoryDirector*()> GetDirector)
{
	return SNew(SFTDialogue, MoveTemp(GetDirector));
}

TSharedRef<SWidget> MakeExplorePartyWidget(TFunction<const AFTExploreCharacter*()> GetLead)
{
	return SNew(SFTExploreParty, MoveTemp(GetLead));
}

TSharedRef<SWidget> MakeExploreHudWidget(TFunction<const AFTExploreDirector*()> GetDirector, TFunction<const AFTExploreCharacter*()> GetLead)
{
	return SNew(SFTExploreHud, MoveTemp(GetDirector), MoveTemp(GetLead));
}

void SetUiPalette(uint8 Palette)
{
	GUiPalette.store(Palette, std::memory_order_relaxed);
}

bool EnsureMenuScreens()
{
	if (!NewUiEnabled() || !GEngine || !GEngine->GameViewport)
	{
		return false;
	}
	if (GMenuScreens.IsValid() && GMenuViewport.Get() == GEngine->GameViewport)
	{
		return true;
	}
	// A map load clears the viewport's widgets (the weak pointer lapses with it): mount again, over every HUD layer.
	const TSharedRef<SWidget> W = SNew(SFTMenuScreens);
	GEngine->GameViewport->AddViewportWidgetContent(W, 1000);
	GMenuScreens = W;
	GMenuViewport = GEngine->GameViewport;
	return true;
}
}
