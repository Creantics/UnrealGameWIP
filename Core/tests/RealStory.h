#pragma once

// The real story script (newengine assets/story through TestAssets.h), imported once per test run
// and shared by every test file that plays it. Tests on it assert counts and ids only: never print
// or compare its text.

#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryImport.h"

#include "TestAssets.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore_test {

struct RealStory {
    ftcore::StoryDatabase db;
    bool present = false;
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
};

// Loaded once: the script is large and every test only reads it.
inline const RealStory& realStory() {
    static const RealStory story = [] {
        RealStory s;
        std::string storyText;
        if (!readAsset("story/story.json", storyText)) return s;
        s.present = true;
        std::vector<std::string> ids;
        if (!ftcore::storySceneIds(storyText, ids, &s.error)) return s;
        std::vector<std::string> texts(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (!readAsset("story/scenes/" + ids[i] + ".json", texts[i])) {
                s.error = "cannot read scenes/" + ids[i] + ".json";
                return s;
            }
        const std::vector<std::string_view> views(texts.begin(), texts.end());
        s.ok = ftcore::importStory(storyText, views, s.db, &s.error, &s.warnings);
        return s;
    }();
    return story;
}

} // namespace ftcore_test

// Binds `s` to the real story, or skips the test when the newengine assets are not there.
#define FTCORE_REQUIRE_REAL_STORY(s)                                                                                   \
    const ftcore_test::RealStory& s = ftcore_test::realStory();                                                        \
    if (!s.present) {                                                                                                  \
        MESSAGE("newengine assets/story not present; skipping");                                                       \
        return;                                                                                                        \
    }                                                                                                                  \
    REQUIRE_MESSAGE(s.ok, s.error)
