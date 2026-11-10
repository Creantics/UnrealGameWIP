#pragma once

// Which body a look wears. A look's body can be cut away under its covering garments (a BodyHide copy, so skin
// never pokes through cloth); when one of those garments isn't worn the look must wear the uncut body instead,
// else a hole shows where the torso was cut (the modular-outfit rule: every garment comes off with the body whole
// underneath).

#include "ftcore/Api.h"

#include <string>
#include <string_view>
#include <vector>

namespace ftcore::look {

// True when the piece is left off: every piece is (noPieces) or its name holds one of the words.
FTCORE_API bool PieceLeftOff(std::string_view piece, bool noPieces, const std::vector<std::string>& noWords);

// True when the cut body won't do: some cover word names none of the worn pieces. No cover words: never.
FTCORE_API bool NeedsFullBody(const std::vector<std::string>& coverWords, const std::vector<std::string>& wornPieces);

} // namespace ftcore::look
