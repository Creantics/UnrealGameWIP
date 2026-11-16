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
#include <utility>
#include <vector>

namespace ftcore::look {

// True when the piece is left off: every piece is (noPieces) or its name holds one of the words (any case).
FTCORE_API bool PieceLeftOff(std::string_view piece, bool noPieces, const std::vector<std::string>& noWords);

// True when the cut body won't do: some cover word names (any case) none of the worn pieces. No cover words: never.
FTCORE_API bool NeedsFullBody(const std::vector<std::string>& coverWords, const std::vector<std::string>& wornPieces);

// One cut copy of an under piece: `cut` is worn in place of `under` while a worn piece is the over piece `overWord` names.
// The word is a path (it holds '/': that piece only), else a name: a worn piece whose asset name is the word names it
// exactly, one whose asset name holds it (any case) names it in part.
struct UnderLayer {
    std::string under;
    std::string overWord;
    std::string cut;
};

// An object path as the engine names an asset: "/Pkg/Name" (an ini path without the asset suffix) becomes
// "/Pkg/Name.Name"; any other text is given back as it is.
FTCORE_API std::string CanonicalPath(std::string_view path);

// How `overWord` names the worn piece `worn` (a path or a bare asset name): 0 not, 1 in part, 2 exactly.
FTCORE_API int OverMatch(std::string_view worn, std::string_view overWord);

// The path to wear for `piece`. Its base is the piece itself, or, when the piece is some layer's cut, that layer's under
// piece (so a cut whose over piece came off goes back to the whole piece). Of the layers whose under piece is the base,
// the one whose over word names a worn piece best gives its cut: an exact name before one in part, then the longer
// word, then the first in order; with none, the base. Worn entries that are the base or one of its cuts are never over
// pieces. Paths compare canonically (CanonicalPath) in any case; a layer with an empty over word or cut never applies.
FTCORE_API std::string UnderChoice(std::string_view piece, const std::vector<UnderLayer>& layers,
                                   const std::vector<std::string>& wornPieces);

// UnderChoice with the command line's left-off rule (PieceLeftOff on asset names): a worn entry left off covers nothing,
// and a cut whose own name is left off while its base's isn't gives the base, so the under piece is never dropped.
FTCORE_API std::string UnderChoiceLeftOff(std::string_view piece, const std::vector<UnderLayer>& layers,
                                          const std::vector<std::string>& wornPieces, bool noPieces,
                                          const std::vector<std::string>& noWords);

// Pairs of layers (indices, first < second) that can both apply to one worn piece: the same under piece and over words
// that are equal or one inside the other (any case; a path word only when equal). UnderChoice still settles them (see
// above), but a look should say which over piece each cut is for.
FTCORE_API std::vector<std::pair<int, int>> OverlappingLayers(const std::vector<UnderLayer>& layers);

// For each slot of a new mesh, the slot of the old mesh whose state it takes: the first with the same name (any case),
// else, when both meshes have as many slots and the name is empty or matches none, the same index; else -1.
FTCORE_API std::vector<int> SlotCarry(const std::vector<std::string>& oldSlots, const std::vector<std::string>& newSlots);

} // namespace ftcore::look
