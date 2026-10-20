#pragma once

// The dialogue backlog: the last lines, directions, choice prompts and chosen options the player
// has been shown, newest last. It stores where each one is in the story database (scene index,
// beat index, option index), not copies of the text, so a push is a few integer writes and the
// ring never allocates. The text is read back from the database on demand (speaker(), text()).
// A backlog is only meaningful for the database it was filled from: clear() it when the story is
// loaded again, on a new game and on loading a save. It is not saved.
//
//   DialogueBacklog log;
//   each frame, after presenter.update():  log.noteShown(runner, presenter);
//   before applying a Choose command:      log.noteChoice(runner, option);
//   to draw: for i in 0..log.size(): log.at(i), DialogueBacklog::text(log.at(i), db)
//
// noteShown() pushes a beat once, when its text is fully revealed (a line, a direction, or a
// choice's prompt); gameplay beats and holds are not logged.

#include "ftcore/Api.h"
#include "ftcore/Dialogue.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ftcore {

struct BacklogEntry {
    // Prompt: the text of a choice. Choice: the option the player picked (beat is the choice).
    enum class Kind : std::uint8_t { Line, Direction, Prompt, Choice };
    Kind kind = Kind::Line;
    std::uint32_t scene = kNone, beat = 0, option = kNone;
};

class DialogueBacklog {
public:
    static constexpr std::uint32_t kCapacity = 200;

    void clear() {
        head_ = 0;
        count_ = 0;
        lastEpoch_ = kNone;
        ++serial_;
    }
    std::uint32_t size() const { return count_; }
    // Bumped by every push and clear (a view can tell when to rebuild).
    std::uint32_t serial() const { return serial_; }
    // 0 is the oldest entry still held, size() - 1 the newest. i must be below size().
    const BacklogEntry& at(std::uint32_t i) const { return entries_[(head_ + kCapacity - count_ + i) % kCapacity]; }

    void push(const BacklogEntry& e) {
        entries_[head_] = e;
        head_ = (head_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
        ++serial_;
    }

    // Pushes the beat the presenter shows when its text is fully out and it has not been noted
    // yet (once per beat epoch). True when something was pushed.
    FTCORE_API bool noteShown(const StoryRunner& runner, const DialoguePresenter& presenter);
    // Pushes the option about to be chosen on the runner's current choice. False when the runner
    // is not on a choice or the option does not exist.
    FTCORE_API bool noteChoice(const StoryRunner& runner, std::uint32_t option);

    // The entry's speaker (lines only; "" otherwise) and text, as views into `db`. Empty when the
    // entry no longer fits the database.
    FTCORE_API static std::string_view speaker(const BacklogEntry& e, const StoryDatabase& db);
    FTCORE_API static std::string_view text(const BacklogEntry& e, const StoryDatabase& db);

private:
    std::array<BacklogEntry, kCapacity> entries_{};
    std::uint32_t head_ = 0, count_ = 0;
    std::uint32_t lastEpoch_ = kNone;
    std::uint32_t serial_ = 0;
};

} // namespace ftcore
