#pragma once
// What the arena HUD draws for a fight event of its own: a stagger breaks the boss line, a shared blow shows SHARED by
// the partner's row. Every other event is carried by the lines and states themselves (the hp lines' rub-out, the
// call word, the Load in the name, the cylinder), so it draws nothing here.
//
//   stagger       the boss line breaks open at a notch, holds, closes   LineBreak
//   shared blow   SHARED beside the partner's hp line, 1 s, a 0.3 s fade Shared
//
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
    LineBreak, // a stagger (the weak point open)
    Shared,    // a blow shared between the leads at the table, or half of one taken for the ally
};
inline constexpr std::size_t kUiEventMarkCount = 3;

// The one mark for an event type (None: the event draws nothing of its own), and how long it lives.
FTCORE_API UiEventMark uiEventMark(ArenaEventType type);
FTCORE_API float uiEventMarkLife(UiEventMark mark);

// A mark's pose at `age` seconds: `tip` is how far the line's break is open (0..1).
struct UiMarkPose {
    float alpha = 0.f;
    float tip = 0.f;
};
FTCORE_API UiMarkPose uiEventMarkPose(UiEventMark mark, float age);

// ---- Live marks --------------------------------------------------------------------------------------------------

inline constexpr std::size_t kUiLiveMarkMax = 12;

struct UiLiveMark {
    bool live = false;
    UiEventMark mark = UiEventMark::None;
    std::uint8_t from = kArenaNobody; // the event's source
    std::uint8_t to = kArenaNobody;   // the event's target
    float age = 0.f;
    std::uint32_t serial = 0;
};

struct UiLiveMarks {
    std::array<UiLiveMark, kUiLiveMarkMax> m{};
    std::uint32_t serial = 0;
};

// Pushes the event's mark (none for an event that maps to None; returns false then). A repeat of a live mark of the
// same kind on the same bodies restarts it instead of adding one; a full list loses its oldest.
FTCORE_API bool uiMarkPush(UiLiveMarks& marks, ArenaEventType type, std::uint8_t from, std::uint8_t to);
// Ages the marks; those past their life end.
FTCORE_API void uiMarkTick(UiLiveMarks& marks, float dt);
// Ends every mark (the result card opened).
FTCORE_API void uiMarkClear(UiLiveMarks& marks);
FTCORE_API std::uint32_t uiMarkLiveCount(const UiLiveMarks& marks);

} // namespace ftcore
