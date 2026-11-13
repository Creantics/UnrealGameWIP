#pragma once
// What the arena HUD draws for a fight event: one mark in the line language, or nothing. Never a word.
//
// The HUD has no floating words. Each event the fight reports maps to exactly one mark (or to none), drawn where the
// thing happened and carried by motion:
//
//   parry          the reticle ring snaps shut and rings once           RingSnap    at the reticle
//   block          a short flat tick on the lead's ring at the guard arc GuardTick   on the lead's ring
//   evade          the reticle ring opens and lets go                   RingOpen    at the reticle
//   stagger        the boss line breaks at a notch; the reticle trembles LineBreak   the boss line and the reticle
//   shared blow    a thin thread from the struck body to each sharer    ShareThread between the places
//   Load stored    the Load arc's leading tip flares                    LoadTip     on the Load arc
//   a read stack   a small tick grows on the reticle ring               ReadTick    at the reticle
//
// Damage is carried by the lines themselves (the boss line's rub-out, the lead ring's arc): no numbers by default.
// The pose of a mark at an age is Core's (uiEventMarkPose), so the motion is tested here and the widget only draws it.
//
// Zero allocation.
#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

enum class UiEventMark : std::uint8_t {
    None,
    RingSnap,    // a parry
    GuardTick,   // a block
    RingOpen,    // an evade
    LineBreak,   // a stagger (the weak point open)
    ShareThread, // a blow shared at the table, or half of one taken for the ally
    LoadTip,     // Load stored
    ReadTick,    // a Read stack gained
};
inline constexpr std::size_t kUiEventMarkCount = 8;

// Where a mark is drawn.
enum class UiMarkPlace : std::uint8_t {
    None,
    Reticle,  // the reticle ring (the lock's ring round the locked body; the screen's centre without a lock)
    LeadRing, // the lead's ring (its stamina arc: the guard arc)
    TopLine,  // the boss line (and the reticle with it)
    Between,  // from one body's place to another's
    LoadArc,  // the lead's Load arc
};

struct UiEventMarkSpec {
    UiEventMark mark = UiEventMark::None;
    UiMarkPlace place = UiMarkPlace::None;
    float life = 0.f;        // seconds the mark lives
    std::uint8_t words = 0;  // text it draws: always 0 (the HUD's events carry no words)
};

// The one mark for an event type (None: the event draws nothing of its own).
FTCORE_API UiEventMark uiEventMark(ArenaEventType type);
FTCORE_API const UiEventMarkSpec& uiEventMarkSpec(UiEventMark mark);

// A mark's pose at `age` seconds. The reticle marks are drawn as a ring of `arcs` arcs (the lock's ring is 4 arcs
// covering `span` of the circle); `radius` scales the ring's resting radius; `base` is how much of the resting lock to
// draw under the mark (0 while the mark replaces it); `ripple` scales the radius of a second ring that rings out once
// (`rippleAlpha` 0: none); `jitter` is the tremble's amplitude in radius units; `tip` is a stroke's drawn length
// (0..1, or a flare's size for LoadTip).
struct UiMarkPose {
    float alpha = 0.f;
    float base = 1.f;
    float span = 0.f;
    float radius = 1.f;
    float ripple = 1.f;
    float rippleAlpha = 0.f;
    float jitter = 0.f;
    float tip = 0.f;
};

inline constexpr float kUiLockSpan = 0.44f; // the resting lock: four arcs of 40 degrees

FTCORE_API UiMarkPose uiEventMarkPose(UiEventMark mark, float age);

// The steady pose of a standing stagger (the weak point open, a staggered target): the ring open and trembling.
FTCORE_API UiMarkPose uiStaggerHoldPose();

// ---- Live marks --------------------------------------------------------------------------------------------------

inline constexpr std::size_t kUiLiveMarkMax = 12;

struct UiLiveMark {
    bool live = false;
    UiEventMark mark = UiEventMark::None;
    std::uint8_t from = kArenaNobody; // ShareThread: the struck body; else the event's source
    std::uint8_t to = kArenaNobody;   // the event's target (ShareThread: the body that took the share)
    int amount = 0;
    float age = 0.f;
    std::uint32_t serial = 0;
};

struct UiLiveMarks {
    std::array<UiLiveMark, kUiLiveMarkMax> m{};
    std::uint32_t serial = 0;
};

// Pushes the event's mark (none for an event that maps to None; returns false then). A repeat of a live mark of the
// same kind on the same bodies restarts it instead of adding one; a full list loses its oldest. `from` is the event's
// source, or for a ShareThread the struck body (uiShareStruck).
FTCORE_API bool uiMarkPush(UiLiveMarks& marks, ArenaEventType type, std::uint8_t from, std::uint8_t to, int amount);
// Ages the marks; those past their life end.
FTCORE_API void uiMarkTick(UiLiveMarks& marks, float dt);
// Ends every mark (the result card opened).
FTCORE_API void uiMarkClear(UiLiveMarks& marks);
FTCORE_API std::uint32_t uiMarkLiveCount(const UiLiveMarks& marks);

// The seated lead a shared blow struck, from who took the shares of it: the player when the ally took one, the ally
// when the player did; when only enemies shared, the one lead at the table (kArenaNobody when it cannot be told).
FTCORE_API std::uint8_t uiShareStruck(const ArenaTableSeats& seats, bool sharedToPlayer, bool sharedToAlly);

} // namespace ftcore
