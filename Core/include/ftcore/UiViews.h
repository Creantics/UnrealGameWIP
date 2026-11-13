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
#include "ftcore/StoryRunner.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ftcore {

class ArenaFight;
class DialoguePresenter;

// ---- Arena HUD -------------------------------------------------------------------------------------------------

struct ArenaHudLead {
    std::uint8_t lead = 0;     // index into the party (0 Devin, 1 Duy, 2 the Third), for the name key and the portrait
    int hp = 0, hpMax = 0;
    float stamina01 = 0.f;     // 0..1
    bool guarding = false;     // guard held (or a refusal held)
    bool guardBroken = false;
    int load = 0, loadMax = 0; // the Load gauge (0 max: the lead has none)
    bool overloaded = false;
    bool releaseReady = false; // more Load than the Release needs
    std::uint8_t heals = 0, healsMax = 0;
    int read = 0, readMax = 0; // Read stacks
    bool down = false;
    bool operator==(const ArenaHudLead&) const = default;
};

struct ArenaHudPartner {
    bool present = false;
    std::uint8_t lead = 0;
    int hp = 0, hpMax = 0;
    bool down = false;
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

struct ArenaHudBoss {
    bool present = false;
    std::uint16_t nameKey = 0;      // the host's index of the boss's name string
    int hp = 0, hpMax = 0;
    std::uint8_t phase = 0, phaseCount = 1; // 0-based phase, and how many; drawn as marks on the bar
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
