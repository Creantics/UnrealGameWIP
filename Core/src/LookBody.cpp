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
bool SamePath(std::string_view a, std::string_view b) {
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

std::string UnderChoice(std::string_view piece, const std::vector<UnderLayer>& layers,
                        const std::vector<std::string>& wornPieces) {
    std::string_view base = piece;
    for (const UnderLayer& layer : layers) {
        if (!layer.cut.empty() && SamePath(piece, layer.cut)) {
            base = layer.under;
            break;
        }
    }
    for (const UnderLayer& layer : layers) {
        if (layer.cut.empty() || layer.overWord.empty() || !SamePath(layer.under, base)) {
            continue;
        }
        for (const std::string& worn : wornPieces) {
            if (!IsUnderOrCut(worn, base, layers) && Holds(AssetName(worn), layer.overWord)) {
                return layer.cut;
            }
        }
    }
    return std::string(base);
}

} // namespace ftcore::look
