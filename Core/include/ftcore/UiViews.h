#pragma once
// What the new game UI shows, as plain data the Slate layer paints (production/ui plan, section d).
//
// Each view is fixed-size and split into sections; every section carries a revision that bumps only when its values
// change, so the Slate side rebuilds a section's text and layout only then and never makes strings per frame.
//
//   ArenaHudView view;
//   each tick:  ArenaHudFrame f;  fillArenaHudLead(fight, f.lead);  fillArenaHudTarget(fight, f.target);
//               (the host fills f.partner, f.third and f.boss from what it runs)
//               updateArenaHudView(view, f);
//   DialogueView dv;  each frame:  updateDialogueView(dv, presenter, flags);
//
// Data Core does not have yet (the third place's gift slot, the shared count) is a section with `planned` set: the
// UI draws a reserved, labelled slot and never invents values.
//
// Zero allocation.
#include "ftcore/Api.h"
#include "ftcore/Chairs.h"
#include "ftcore/Places.h"
#include "ftcore/StoryRunner.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ftcore {

class ArenaFight;
class DialoguePresenter;

// ---- Arena HUD -------------------------------------------------------------------------------------------------

struct ArenaHudLead {
    std::uint8_t lead = 0;     // index into the party (the lead's place, 0 to 2), for the name key and the portrait
    int hp = 0, hpMax = 0;
    float stamina01 = 0.f;     // 0..1
    bool guarding = false;     // guard held (or a refusal held)
    bool guardBroken = false;
    int load = 0, loadMax = 0; // the Load gauge (0 max: the lead has none); loadMax is the track's length (kMaxLoad)
    int loadCap = 0;           // where the Load stops (the named chairs' cap; loadMax when carrying is off)
    int releaseAt = 0;         // the Release opens while load > this (a third of the cap)
    bool overloaded = false;   // load at the cap
    bool releaseReady = false; // more Load than the Release needs
    std::uint8_t heals = 0, healsMax = 0; // healsMax: the kit's charges plus the chairs'
    std::array<ChairKind, kMaxPlaces> chairs{}; // the Named chairs' kinds, in chair order
    std::uint8_t chairCount = 0;
    bool tookHalf = false;     // this tick's events hold a TookHalf (an edge: the take-half mark pulses)
    int read = 0, readMax = 0; // Read stacks
    std::uint8_t roundsMax = 0; // the cylinder's size (0: the kit has none; set only under a Brief)
    std::uint8_t rounds = 0;    // rounds left in the cylinder
    float reload01 = 0.f;       // reload progress 0..1 (0 when not reloading)
    bool down = false;
    bool operator==(const ArenaHudLead&) const = default;
};

struct ArenaHudPartner {
    bool present = false;
    std::uint8_t lead = 0;
    int hp = 0, hpMax = 0;
    bool down = false;
    int load = 0;           // the partner's kept Load (the runner's; 0 without a cap)
    bool operator==(const ArenaHudPartner&) const = default;
};

// The third place in the party: the gift slot and the shared count (FightSession, not hooked to the arena runner yet).
struct ArenaHudThird {
    bool present = false;
    bool planned = true;           // true: no data yet, draw the reserved slot only
    std::uint8_t giftsLeft = 0, giftsMax = 0;
    std::uint8_t count = 0, countMax = 0; // the shared count
    bool operator==(const ArenaHudThird&) const = default;
};

struct ArenaHudTarget {
    bool present = false;
    std::uint8_t enemy = 0;
    bool locked = false;    // locked on (else the soft target)
    int hp = 0, hpMax = 0;
    float poise01 = 0.f;
    bool operator==(const ArenaHudTarget&) const = default;
};

// A weak point's state. How it opens differs per world (sharing in Solhaven, reading in the Lattice, refusing in the
// Unwritten); the cue is the same shape in all three, with the word beside it.
enum class UiWeakPoint : std::uint8_t { None, Closed, Opening, Open };
enum class UiWeakPointWay : std::uint8_t { None, Share, Read, Refuse };

// The zone a boss calls for its next blow (the stone bearer's called strike), as the line shows it.
enum class UiCalledZone : std::uint8_t { None, Left, Right, Path };

inline constexpr std::size_t kArenaHudPhases = 4;

struct ArenaHudBoss {
    bool present = false;
    std::uint8_t slot = 0;          // the boss's enemy slot (a target with this slot is the boss)
    std::uint16_t nameKey = 0;      // the host's index of the boss's name string
    std::uint8_t rank = 0;          // the boss's rank marks beside its name (0: none drawn; no data sets it yet)
    int hp = 0, hpMax = 0;
    std::uint8_t phase = 0, phaseCount = 1; // 0-based phase, and how many; drawn as marks on the bar
    std::array<float, kArenaHudPhases> phaseEnd{}; // each phase's floor as a share of max hp (where its notch sits)
    UiCalledZone call = UiCalledZone::None; // the zone called for the next blow
    bool callIn = false;            // the lead stands in the called zone
    bool down = false;              // knocked down (open to blows)
    std::int32_t noteSerial = -1;   // the host's latest answer or rule note (-1: none); the word and its fade are the host's
    std::uint8_t weight = 0, weightMax = 0; // the stone bearer's WEIGHT pips (0 max: none)
    UiWeakPoint weakPoint = UiWeakPoint::None;
    UiWeakPointWay way = UiWeakPointWay::None;
    float opening01 = 0.f;          // Opening: how far open, 0..1
    bool invulnerable = false;
    bool operator==(const ArenaHudBoss&) const = default;
};

enum class ArenaHudSection : std::uint8_t { Lead, Partner, Third, Target, Boss };
inline constexpr std::size_t kArenaHudSectionCount = 5;

// One tick's values, filled by the fill functions and the host.
struct ArenaHudFrame {
    ArenaHudLead lead;
    ArenaHudPartner partner;
    ArenaHudThird third;
    ArenaHudTarget target;
    ArenaHudBoss boss;
};

struct ArenaHudView {
    ArenaHudFrame now;
    std::array<std::uint32_t, kArenaHudSectionCount> revision{};
    std::uint32_t revisionOf(ArenaHudSection s) const { return revision[static_cast<std::size_t>(s)]; }
};

// The lead's section from the fight (`lead` is kept from `out`).
FTCORE_API void fillArenaHudLead(const ArenaFight& fight, ArenaHudLead& out);
// The locked enemy, else the current attack's soft target, else none.
FTCORE_API void fillArenaHudTarget(const ArenaFight& fight, ArenaHudTarget& out);
// The ally of a carrying run (present only when the fight has one); `lead` is left as the host set it.
FTCORE_API void fillArenaHudPartner(const ArenaFight& fight, ArenaHudPartner& out);
// Copies `f` into the view; bumps each changed section's revision. Returns a mask of the changed sections (bit i:
// ArenaHudSection i).
FTCORE_API std::uint32_t updateArenaHudView(ArenaHudView& view, const ArenaHudFrame& f);

// ---- Dialogue --------------------------------------------------------------------------------------------------

inline constexpr std::size_t kDialogueViewOptions = 8;

enum class DialogueViewKind : std::uint8_t { Hidden, Line, Direction, Choice };

struct DialogueViewFlags {
    bool autoOn = false;
    bool skipOn = false;
    bool logAvailable = false; // the backlog has lines
};

struct DialogueViewOption {
    std::string_view text;
    bool selectable = true;
    bool returns = false;
};

// Lifetime: the string_views (here and in DialogueViewOption) point into the presenter's storage. They are valid only
// until that presenter's next update or until it is destroyed; read them in the same frame, or copy what must be kept.
struct DialogueView {
    DialogueViewKind kind = DialogueViewKind::Hidden;
    std::string_view speaker;  // shown on every line, in the subtitle block (never only on the body)
    std::string_view lineId;   // the voice and the backlog key
    std::string_view text;     // the text so far (whole codepoints)
    std::string_view fullText; // the whole text of the beat: wrap this once per beatRevision, reveal by revealedChars
    std::uint32_t revealedChars = 0; // codepoints of fullText out so far
    bool complete = false;     // the whole text is out
    bool continuePrompt = false; // the "next" glyph: a complete line or direction waiting for a press
    std::uint32_t optionCount = 0;
    std::array<DialogueViewOption, kDialogueViewOptions> options{};
    DialogueViewFlags flags;
    // Bumps on a new beat (speaker, text start, options); on more text revealed; on a change of the prompt or flags.
    std::uint32_t beatRevision = 0;
    std::uint32_t textRevision = 0;
    std::uint32_t promptRevision = 0;

    std::uint32_t epoch = 0xFFFFFFFFu; // the presenter's beat epoch last seen
};

// Updates `view` from the presenter (and its runner). Returns true when anything changed.
FTCORE_API bool updateDialogueView(DialogueView& view, const DialoguePresenter& p, const DialogueViewFlags& flags);

} // namespace ftcore
