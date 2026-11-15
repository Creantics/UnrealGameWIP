#pragma once

// Which body a look wears. A look's body can be cut away under its covering garments (a BodyHide copy, so skin
// never pokes through cloth); when one of those garments isn't worn the look must wear the uncut body instead,
// else a hole shows where the torso was cut (the modular-outfit rule: every garment comes off with the body whole
// underneath).
// Which copy of an under piece a look wears. An under piece (a tee) can have cut copies, one per over piece it is worn
// with (and per front state of that over piece, since an open and a closed jacket are different meshes): faces the over
// piece always hides are deleted, so the under piece never shows through it. The cut copy is worn only while its over
// piece is; else the whole piece is.

#include "ftcore/Api.h"

#include <string>
#include <string_view>
#include <vector>

namespace ftcore::look {

// True when the piece is left off: every piece is (noPieces) or its name holds one of the words (any case).
FTCORE_API bool PieceLeftOff(std::string_view piece, bool noPieces, const std::vector<std::string>& noWords);

// True when the cut body won't do: some cover word names (any case) none of the worn pieces. No cover words: never.
FTCORE_API bool NeedsFullBody(const std::vector<std::string>& coverWords, const std::vector<std::string>& wornPieces);

// One cut copy of an under piece: `cut` is worn in place of `under` while a worn piece's asset name holds `overWord`.
struct UnderLayer {
    std::string under;
    std::string overWord;
    std::string cut;
};

// The path to wear for `piece`. Its base is the piece itself, or, when the piece is some layer's cut, that layer's under
// piece (so a cut whose over piece came off goes back to the whole piece). The first layer in order whose under piece is
// the base and whose over word (any case) is in the asset name (after the last '.', else the last '/') of a worn piece
// gives its cut; else the base. Worn entries that are the base or one of its cuts are never over pieces. Paths compare
// in any case; a layer with an empty over word or cut never applies.
FTCORE_API std::string UnderChoice(std::string_view piece, const std::vector<UnderLayer>& layers,
                                   const std::vector<std::string>& wornPieces);

} // namespace ftcore::look
