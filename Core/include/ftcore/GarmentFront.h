#pragma once

// Open and closed garments. A jacket or coat has two fitted meshes, one worn closed and one worn open (the lining
// shows); a look names both and which one it wears by default, and a scene or the command line can pick the other.
// Pieces without a pair are worn as listed.

#include "ftcore/Api.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore::look {

enum class Front : std::uint8_t { Closed, Open };

// One garment's two meshes (asset names) and the state the look wears unless told otherwise.
struct FrontPair {
    std::string closed;
    std::string open;
    Front byDefault = Front::Closed;
};

// A state asked for: for every pair whose closed or open name holds `word`, or for every pair when `word` is empty.
struct FrontAsk {
    std::string word;
    Front state = Front::Closed;
};

// "open" or "closed" (any case); false leaves `out` as it was.
FTCORE_API bool ParseFront(std::string_view text, Front& out);

// "open", "closed", or "+"-joined "word:state" items ("jacket:open+coat:closed"). Appends to `out`; false (and
// nothing appended) when any item doesn't parse.
FTCORE_API bool ParseFrontAsks(std::string_view text, std::vector<FrontAsk>& out);

// The state a pair is worn in: the last ask that names it, else its default.
FTCORE_API Front FrontFor(const FrontPair& pair, const std::vector<FrontAsk>& asks);

// The piece worn for a listed piece: when it is either half of a pair, that pair's half for its state; else itself.
FTCORE_API const std::string& WornPiece(const std::string& piece, const std::vector<FrontPair>& pairs,
                                        const std::vector<FrontAsk>& asks);

} // namespace ftcore::look
