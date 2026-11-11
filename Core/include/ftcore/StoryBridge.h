#pragma once

// Story bridge: keeps a StoryRunner's story-fact flags and a StoryState in step. The state is the truth;
// the runner's flags named `sf.<fact id>` are a view of it.
//
//   ftcore::StoryBridge bridge;
//   bridge.bind(runner, state, registry, links, &error); // load time (allocates); links empty: identity
//   runner.start();                                      // resets the state, begins the first scene
//   ...                                                  // a load: runner.restore(pos), decode the state, then
//   bridge.sync();
//
// - Scenes. A runner scene plays the registry scene its link names; with no links, the registry scene
//   whose code is the runner scene's id (identity). Entering a mapped scene commits the open one and
//   begins it (storyEnterScene); several runner scenes in a row may play one registry scene, which stays
//   open across them. An unmapped runner scene leaves the state and the flags as they are. start() resets the state first;
//   the story's end commits the open scene.
// - Restart. restartScene() restarts the registry scene (storyRestartScene) back to the facts as the
//   runner scene began, the runner's own starting point: a second runner scene of one registry scene keeps
//   what the first one set.
// - Facts into flags. When a scene is entered or restarted, every bound flag whose fact the scene reads
//   or sets takes the fact's value (a bool flag: the fact is set; an int flag: its value, 255 when never
//   offered); every other bound flag goes back to its initial value, so a scene branches only on what it
//   reads.
// - Flags into facts. When the story sets a bound flag (a flag beat, an option effect, a gameplay
//   result), the bridge calls storySet (a bool flag: true sets 1; an int flag: its value, which must fit
//   a byte) and writes the fact's value back to the flag (as above), so a refused set leaves the flag as
//   the fact is.
//   A refusal is passed to the report handler. setFlag() is not forwarded.
// - A load. restore() pauses the bridge (the runner's moves while it settles are not forwarded) until
//   sync(), called once the state is decoded. The runner stays where it was restored and the state as
//   decoded (a save takes both at one moment). When the decoded state has another registry scene open, or
//   none, the runner's scene is entered; at the story's end the open scene commits. The flags are pushed,
//   and a restart after the load goes back to the load point (as the runner's does).
//
// Rebind after the runner's database or the registry changes: the bridge checks the database's address
// only, so a database rebuilt in place needs a rebind too. No allocation after bind().

#include "ftcore/Api.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/StoryState.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ftcore {

struct StoryBridgeReport {
    enum class Kind : std::uint8_t { FactRefused, SceneRefused };
    Kind kind = Kind::FactRefused;
    std::uint32_t flag = kNone;          // FactRefused: the runner flag
    std::uint16_t fact = 0;              // FactRefused: the fact slot
    std::int64_t value = 0;              // FactRefused: the flag's value
    SetResult result = SetResult::Ok;    // FactRefused
    std::uint32_t scene = kNone;         // the runner scene
    std::uint8_t ordinal = kNoScene;     // the registry scene
};

class StoryBridge {
public:
    using ReportHandler = void (*)(void* user, const StoryBridgeReport& r);

    StoryBridge() = default;
    ~StoryBridge() { unbind(); }
    StoryBridge(const StoryBridge&) = delete;
    StoryBridge& operator=(const StoryBridge&) = delete;

    // Takes over the runner's scene and flag handlers. False, with every problem in `error` (one per line),
    // when the runner has no database, a `sf.` flag names no fact of the registry or is a string flag, a
    // link names a scene the database does not have, or the story has fact flags while no runner scene plays a
    // registry scene (every write would be refused); the bridge is then unbound.
    FTCORE_API bool bind(StoryRunner& runner, StoryState& state, const FactRegistry& reg,
                         std::span<const StorySceneLink> links, std::string* error = nullptr);
    FTCORE_API void unbind(); // gives the runner's handlers back (cleared); unbind (or destroy) before the runner goes
    bool bound() const { return runner_ != nullptr; }
    void setReportHandler(ReportHandler h, void* user) {
        onReport_ = h;
        reportUser_ = user;
    }

    // After a load: the runner restored and the state decoded. See the header comment.
    FTCORE_API void sync();

    std::uint32_t boundFlags() const { return boundFlags_; }
    std::uint32_t mappedScenes() const { return mappedScenes_; }
    // The registry scene a runner scene plays (kNoScene when unmapped or unbound).
    std::uint8_t ordinalOf(std::uint32_t runnerScene) const {
        return runnerScene < sceneOrdinal_.size() ? sceneOrdinal_[runnerScene] : kNoScene;
    }

    static constexpr std::string_view kFlagPrefix = "sf.";

private:
    static void onScene(void* user, StoryRunner& r, SceneEntry how);
    static void onFlag(void* user, StoryRunner& r, std::uint32_t flag);
    void enter(std::uint32_t runnerScene);
    void markRestartPoint(std::uint32_t runnerScene);
    bool restartState(std::uint32_t runnerScene);
    void push(std::uint8_t ordinal);
    void pushOne(std::uint32_t flag, std::uint8_t ordinal);
    void report(const StoryBridgeReport& r) const {
        if (onReport_) onReport_(reportUser_, r);
    }

    StoryRunner* runner_ = nullptr;
    const StoryDatabase* db_ = nullptr; // the runner's database when bound (a later change makes the bridge inert)
    StoryState* state_ = nullptr;
    const FactRegistry* reg_ = nullptr;
    std::vector<std::uint16_t> flagFact_;     // per runner flag: its fact slot (0: not bound)
    std::vector<std::uint8_t> sceneOrdinal_;  // per runner scene: its registry scene
    std::uint32_t lastScene_ = kNone;         // the runner scene last entered
    // The facts as the runner scene `restartScene_` began (or as loaded): what a restart goes back to.
    std::array<std::uint8_t, kFactSlots> restartFacts_{};
    std::array<std::uint8_t, kFactSlots> restartStamp_{};
    std::array<std::uint32_t, kWideSlots> restartWide_{};
    std::uint32_t restartScene_ = kNone;
    bool restoring_ = false; // between restore() and sync()
    std::uint32_t boundFlags_ = 0, mappedScenes_ = 0;
    ReportHandler onReport_ = nullptr;
    void* reportUser_ = nullptr;
};

} // namespace ftcore
