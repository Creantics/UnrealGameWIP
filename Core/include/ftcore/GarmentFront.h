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

// One garment's two meshes (object paths, short "/Game/A/B" or long "/Game/A/B.B"), the state the look wears unless
// told otherwise, and whether the open mesh exists yet. An open mesh not made (or empty) means the garment is worn
// closed whichever half a look lists.
struct FrontPair {
    std::string closed;
    std::string open;
    Front byDefault = Front::Closed;
    bool openMade = true;
};

// A state asked for: for every pair whose closed or open asset name holds `word` (any case; folders don't count), or
// for every pair when `word` is empty.
struct FrontAsk {
    std::string word;
    Front state = Front::Closed;
};

// Which half of which pair a listed piece is worn as; pair is -1 when the piece belongs to no pair.
struct FrontChoice {
    int pair = -1;
    Front half = Front::Closed;
};

// "open" or "closed" (any case); false leaves `out` as it was.
FTCORE_API bool ParseFront(std::string_view text, Front& out);

// "open", "closed", or "+"-joined "word:state" items ("jacket:open+coat:closed"). Appends to `out`; false (and
// nothing appended) when any item doesn't parse.
FTCORE_API bool ParseFrontAsks(std::string_view text, std::vector<FrontAsk>& out);

// An object path's comparison key: lower case, and "/Game/A/B.B" shortened to "/game/a/b" (a different object name
// after the dot is kept).
FTCORE_API std::string PiecePathKey(std::string_view path);

// The asset name of an object path: "B" for "/Game/A/B" or "/Game/A/B.B".
FTCORE_API std::string_view PieceAssetName(std::string_view path);

// True when the ask names the pair: an empty word, or a word found (any case) in either half's asset name.
FTCORE_API bool AskNames(const FrontPair& pair, std::string_view word);

// The state a pair is worn in: the last ask that names it, else its default; Closed when the open mesh isn't made.
FTCORE_API Front FrontFor(const FrontPair& pair, const std::vector<FrontAsk>& asks);

// The pair and half a listed piece is worn as, matching either half by PiecePathKey.
FTCORE_API FrontChoice WornChoice(std::string_view piece, const std::vector<FrontPair>& pairs,
                                  const std::vector<FrontAsk>& asks);

// The piece worn for a listed piece: when it is either half of a pair, that pair's half for its state; else itself.
FTCORE_API const std::string& WornPiece(const std::string& piece, const std::vector<FrontPair>& pairs,
                                        const std::vector<FrontAsk>& asks);

// The asks' non-empty words that name no pair (for a warning), each once, in order.
FTCORE_API std::vector<std::string> UnmatchedAskWords(const std::vector<FrontPair>& pairs,
                                                      const std::vector<FrontAsk>& asks);

} // namespace ftcore::look
