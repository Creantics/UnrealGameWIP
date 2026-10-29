#pragma once

// The hair card spec (Data/Grooms/hair_cards.json): the groom assets the editor command FT.HairCards (the opt-in
// plugin Plugins/FTHairCards, docs/HAIR_CARDS.md) turns into hair cards, and each level of detail's card budget.
// The engine's hair card generator builds every listed level on its own, with its own texture atlas, and the
// level then draws its cards in place of its strands.
//
//   ftcore::HairCardSpec spec;
//   if (!ftcore::importHairCardSpec(text, spec, &error)) ...;  // allocates: at load
//   if (const int g = spec.find("lead_hair"); g >= 0) ...;     // no allocation
//
// hair_cards.json (ids, asset paths and numbers only):
//   { "version": 1,
//     "grooms": [ { "id": "lead_hair", "groom": "/Game/Characters/Lead/MH/Grooms/lead_hair",
//                   "out": "/Game/Characters/Lead/MH/Grooms/Cards", "group": 0, "seed": 1,
//                   "lods": [ { "lod": 0, "cards": 2000, "triangles": 40000, "textures": 75, "atlas": 4096 },
//                             { "lod": 1, "cards": 1000, "triangles": 16000, "textures": 50, "atlas": 2048,
//                               "screenSize": 0.5, "flyaways": 5 } ] } ] }
//   `groom` is the groom asset's package path and `out` the folder its cards go in, both under /Game/: letters,
//   digits, '_' and '-' between single slashes. The generator puts each level in its own folder there, named
//   after the groom and the level (<groom>_LOD<n>). `group` (0 when absent) is the hair group that gets the cards,
//   which are made from every hair group's strands (docs/HAIR_CARDS.md), and `seed` (0 when absent) the
//   generator's random seed. A groom is listed once.
//   `lods` lists levels 0..7 in increasing order. `cards` is the generator's target card count, given to each of
//   the group's strand filter groups (1..100000); `triangles` its target triangle count (2..1000000, at least two
//   per card); `textures` the number of card textures in the atlas (1..300); `flyaways` (10 when absent) the most
//   flyaway cards (0..1000); `atlas` the atlas size in pixels (1024, 2048, 4096 or 8192). Cards, triangles,
//   textures and atlas don't grow from one level to the next. `screenSize` (optional, in (0, 1], shrinking from
//   level to level among the levels that give one) is where the level starts; absent, a level the groom has keeps
//   its own and a new one gets half the previous level's, as the editor's Add LOD does. `useCards` (true when
//   absent) false builds the cards but leaves the level drawing what it drew. Unknown keys are warnings; anything
//   else wrong is an error.

#include "ftcore/Api.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct HairCardLod {
    // The generator's own limits.
    static constexpr int kMaxLod = 7;
    static constexpr int kMaxCards = 100000;
    static constexpr int kMinTriangles = 2;
    static constexpr int kMaxTriangles = 1000000;
    static constexpr int kMaxTextures = 300;
    static constexpr int kMaxFlyaways = 1000;

    int lod = 0;
    int cards = 0;           // target cards for each strand filter group
    int triangles = 0;       // target triangles
    int textures = 0;        // card textures in the atlas
    int flyaways = 10;       // most flyaway cards
    int atlas = 0;           // atlas width and height in pixels: 1024, 2048, 4096 or 8192
    float screenSize = -1.f; // where the level starts, in (0, 1]; negative: the groom's own, or half the previous level's
    bool useCards = true;    // the level draws its cards once they're built
};

struct HairCardGroom {
    static constexpr int kMaxGroup = 63;
    static constexpr int kMaxSeed = 1000000;
    static constexpr std::size_t kMaxLods = 8;

    std::string id;    // lower-case letters, digits and '_'
    std::string groom; // the groom asset's package path, under /Game/
    std::string out;   // the folder its cards go in, under /Game/
    int group = 0;     // the hair group that gets the cards
    int seed = 0;      // the generator's random seed
    std::vector<HairCardLod> lods; // increasing lod
};

struct HairCardSpec {
    std::vector<HairCardGroom> grooms;

    // The groom with this id, or -1. No allocation.
    FTCORE_API int find(std::string_view id) const;
};

// The generator's atlas size setting for an atlas this many pixels across (10 for 1024 up to 13 for 8192), or -1.
FTCORE_API int hairCardAtlasLog2(int atlasPixels);

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importHairCardSpec(std::string_view json, HairCardSpec& out, std::string* error = nullptr,
                                   std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
