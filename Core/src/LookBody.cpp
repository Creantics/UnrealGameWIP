#include "ftcore/LookBody.h"

#include <cstddef>

namespace ftcore::look {

namespace {
char Lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// True when `word` (non-empty) is found in `text`, in any case.
bool Holds(std::string_view text, std::string_view word) {
    if (word.empty() || word.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i + word.size() <= text.size(); ++i) {
        std::size_t j = 0;
        while (j < word.size() && Lower(text[i + j]) == Lower(word[j])) {
            ++j;
        }
        if (j == word.size()) {
            return true;
        }
    }
    return false;
}
bool SameText(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) {
            return false;
        }
    }
    return true;
}

bool SamePath(std::string_view a, std::string_view b) {
    return SameText(CanonicalPath(a), CanonicalPath(b));
}

// The asset name of a path: after the last '.', else after the last '/', else the whole path.
std::string_view AssetName(std::string_view path) {
    std::size_t at = path.rfind('.');
    if (at == std::string_view::npos) {
        at = path.rfind('/');
    }
    return at == std::string_view::npos ? path : path.substr(at + 1);
}

// True when `worn` is `base` or one of base's cut copies: the under piece itself, never an over piece.
bool IsUnderOrCut(std::string_view worn, std::string_view base, const std::vector<UnderLayer>& layers) {
    if (SamePath(worn, base)) {
        return true;
    }
    for (const UnderLayer& layer : layers) {
        if (!layer.cut.empty() && SamePath(layer.under, base) && SamePath(worn, layer.cut)) {
            return true;
        }
    }
    return false;
}
} // namespace

bool PieceLeftOff(std::string_view piece, bool noPieces, const std::vector<std::string>& noWords) {
    if (noPieces) {
        return true;
    }
    for (const std::string& word : noWords) {
        if (Holds(piece, word)) {
            return true;
        }
    }
    return false;
}

bool NeedsFullBody(const std::vector<std::string>& coverWords, const std::vector<std::string>& wornPieces) {
    for (const std::string& word : coverWords) {
        bool worn = false;
        for (const std::string& piece : wornPieces) {
            if (Holds(piece, word)) {
                worn = true;
                break;
            }
        }
        if (!worn) {
            return true;
        }
    }
    return false;
}

std::string CanonicalPath(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos || slash + 1 >= path.size() || path.find('.', slash) != std::string_view::npos) {
        return std::string(path);
    }
    std::string out(path);
    out += '.';
    out += path.substr(slash + 1);
    return out;
}

int OverMatch(std::string_view worn, std::string_view overWord) {
    if (overWord.empty()) {
        return 0;
    }
    if (overWord.find('/') != std::string_view::npos) {
        return SamePath(worn, overWord) ? 2 : 0;
    }
    const std::string_view name = AssetName(worn);
    return SameText(name, overWord) ? 2 : Holds(name, overWord) ? 1 : 0;
}

std::string UnderChoice(std::string_view piece, const std::vector<UnderLayer>& layers,
                        const std::vector<std::string>& wornPieces) {
    std::string_view base = piece;
    for (const UnderLayer& layer : layers) {
        if (!layer.cut.empty() && SamePath(piece, layer.cut)) {
            base = layer.under;
            break;
        }
    }
    const UnderLayer* best = nullptr;
    int bestRank = 0;
    for (const UnderLayer& layer : layers) {
        if (layer.cut.empty() || layer.overWord.empty() || !SamePath(layer.under, base)) {
            continue;
        }
        int rank = 0;
        for (const std::string& worn : wornPieces) {
            if (!IsUnderOrCut(worn, base, layers)) {
                const int match = OverMatch(worn, layer.overWord);
                rank = match > rank ? match : rank;
            }
        }
        if (rank > bestRank || (rank > 0 && rank == bestRank && layer.overWord.size() > best->overWord.size())) {
            best = &layer;
            bestRank = rank;
        }
    }
    if (best) {
        return best->cut;
    }
    return std::string(base);
}

std::string UnderChoiceLeftOff(std::string_view piece, const std::vector<UnderLayer>& layers,
                               const std::vector<std::string>& wornPieces, bool noPieces,
                               const std::vector<std::string>& noWords) {
    std::vector<std::string> worn;
    worn.reserve(wornPieces.size());
    for (const std::string& entry : wornPieces) {
        if (!PieceLeftOff(AssetName(entry), noPieces, noWords)) {
            worn.push_back(entry);
        }
    }
    std::string choice = UnderChoice(piece, layers, worn);
    if (PieceLeftOff(AssetName(choice), noPieces, noWords)) {
        std::string base = UnderChoice(piece, layers, {});
        if (!PieceLeftOff(AssetName(base), noPieces, noWords)) {
            return base;
        }
    }
    return choice;
}

std::vector<std::pair<int, int>> OverlappingLayers(const std::vector<UnderLayer>& layers) {
    std::vector<std::pair<int, int>> pairs;
    const int count = static_cast<int>(layers.size());
    for (int i = 0; i < count; ++i) {
        const UnderLayer& a = layers[static_cast<std::size_t>(i)];
        if (a.cut.empty() || a.overWord.empty()) {
            continue;
        }
        for (int j = i + 1; j < count; ++j) {
            const UnderLayer& b = layers[static_cast<std::size_t>(j)];
            if (b.cut.empty() || b.overWord.empty() || !SamePath(a.under, b.under)) {
                continue;
            }
            const bool path = a.overWord.find('/') != std::string::npos || b.overWord.find('/') != std::string::npos;
            const bool overlap = path ? SamePath(a.overWord, b.overWord)
                                      : Holds(a.overWord, b.overWord) || Holds(b.overWord, a.overWord);
            if (overlap) {
                pairs.emplace_back(i, j);
            }
        }
    }
    return pairs;
}

std::vector<int> SlotCarry(const std::vector<std::string>& oldSlots, const std::vector<std::string>& newSlots) {
    std::vector<int> from(newSlots.size(), -1);
    const bool sameCount = oldSlots.size() == newSlots.size();
    for (std::size_t j = 0; j < newSlots.size(); ++j) {
        if (!newSlots[j].empty()) {
            for (std::size_t i = 0; i < oldSlots.size(); ++i) {
                if (SameText(oldSlots[i], newSlots[j])) {
                    from[j] = static_cast<int>(i);
                    break;
                }
            }
        }
        if (from[j] < 0 && sameCount) {
            from[j] = static_cast<int>(j);
        }
    }
    return from;
}

} // namespace ftcore::look
