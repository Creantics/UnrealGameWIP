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

} // namespace ftcore::look
