#include "ftcore/LookBody.h"

namespace ftcore::look {

namespace {
bool Holds(std::string_view text, std::string_view word) {
    return !word.empty() && text.find(word) != std::string_view::npos;
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
