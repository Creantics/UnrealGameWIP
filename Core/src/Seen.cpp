#include "ftcore/Seen.h"

#include "ftcore/StoryRunner.h"

#include <bit>

namespace ftcore {

namespace {
constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

void SeenLines::setDatabase(const StoryDatabase* db) {
    db_ = db;
    words_.assign(db ? (db->beatCount() + 63) / 64 : 0, 0);
}

void SeenLines::clear() {
    for (std::uint64_t& w : words_) w = 0;
}

std::size_t SeenLines::indexOf(std::uint32_t scene, std::uint32_t beat) const {
    if (!db_ || scene >= db_->sceneCount()) return kNpos;
    const Scene& s = db_->scene(scene);
    if (beat >= s.beatCount) return kNpos;
    return static_cast<std::size_t>(s.firstBeat) + beat;
}

bool SeenLines::seen(std::uint32_t scene, std::uint32_t beat) const {
    const std::size_t i = indexOf(scene, beat);
    if (i == kNpos || i / 64 >= words_.size()) return false;
    return (words_[i / 64] >> (i % 64)) & 1u;
}

void SeenLines::mark(std::uint32_t scene, std::uint32_t beat) {
    const std::size_t i = indexOf(scene, beat);
    if (i == kNpos || i / 64 >= words_.size()) return;
    words_[i / 64] |= std::uint64_t{1} << (i % 64);
}

bool SeenLines::seen(const StoryRunner& runner) const { return seen(runner.sceneIndex(), runner.beatIndex()); }
void SeenLines::mark(const StoryRunner& runner) { mark(runner.sceneIndex(), runner.beatIndex()); }

std::uint32_t SeenLines::count() const {
    std::uint32_t n = 0;
    for (const std::uint64_t w : words_) n += static_cast<std::uint32_t>(std::popcount(w));
    return n;
}

void captureSeen(const SeenLines& seen, StorySave& save) {
    save.seen.clear();
    const StoryDatabase* db = seen.database();
    if (!db) return;
    static const char kHex[] = "0123456789abcdef";
    for (std::uint32_t s = 0; s < db->sceneCount(); ++s) {
        const Scene& scene = db->scene(s);
        std::string bits;
        bits.reserve((scene.beatCount + 3) / 4);
        bool any = false;
        for (std::uint32_t b = 0; b < scene.beatCount; b += 4) {
            unsigned nibble = 0;
            for (std::uint32_t j = 0; j < 4 && b + j < scene.beatCount; ++j)
                if (seen.seen(s, b + j)) nibble |= 1u << j;
            bits += kHex[nibble];
            any = any || nibble != 0;
        }
        if (!any) continue;
        while (!bits.empty() && bits.back() == '0') bits.pop_back();
        save.seen.push_back({std::string(db->view(scene.id)), std::move(bits)});
    }
}

std::uint32_t applySeen(const StorySave& save, SeenLines& seen) {
    seen.clear();
    const StoryDatabase* db = seen.database();
    if (!db) return 0;
    std::uint32_t marked = 0;
    for (const SavedSeenScene& entry : save.seen) {
        const std::uint32_t s = db->findScene(entry.scene);
        if (s == kNone) continue;
        const std::uint32_t beats = db->scene(s).beatCount;
        for (std::size_t d = 0; d < entry.bits.size(); ++d) {
            const int v = hexValue(entry.bits[d]);
            if (v < 0) continue;
            for (std::uint32_t j = 0; j < 4; ++j) {
                if (!((static_cast<unsigned>(v) >> j) & 1u)) continue;
                const std::size_t beat = d * 4 + j;
                if (beat >= beats) continue;
                seen.mark(s, static_cast<std::uint32_t>(beat));
                ++marked;
            }
        }
    }
    return marked;
}

} // namespace ftcore
