#pragma once

// Which body's retargeted clip set a body uses (Data/Anim/clip_body_aliases.json). Retargeted clips are named
// <source>_to_<body mesh name>; a body that is a copy of another on the same skeleton (a new face build, a
// hide-mask variant) has no clips under its own name and borrows the original's set through this table.
//
//   ftcore::ClipBodyAliases table;
//   if (!ftcore::importClipBodyAliases(text, table, &error, &warnings)) ...;   // allocates: at load
//   std::string_view set = table.resolve(bodyName);                           // no allocation
//
// clip_body_aliases.json:
//   { "version": 1,
//     "aliases": { "SKM_MHC_DevinReal4_BodyMesh": "SKM_MHC_DevinReal3_BodyMesh" } }
//   Each key and value is 1..128 characters of letters, digits and '_'. A body may alias a body that is itself
//   an alias (resolve follows the chain). A key listed twice (case-insensitive), a body aliased to itself, a
//   cycle, a chain longer than kMaxChain, a value that isn't a string, or a missing or wrong version is an
//   error and the whole table is refused. `about` at the top level is a note and is ignored; any other unknown
//   top-level key is a warning.

#include "ftcore/Api.h"

#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct ClipBodyAliases {
    static constexpr int kMaxChain = 8;
    struct Entry {
        std::string body;  // the body mesh's asset name
        std::string clips; // the body whose clip set it uses
    };
    std::vector<Entry> entries; // in file order

    // The body whose clips `body` uses: the end of its alias chain (case-insensitive match), or `body` itself
    // when it has no alias. No allocation.
    FTCORE_API std::string_view resolve(std::string_view body) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importClipBodyAliases(std::string_view json, ClipBodyAliases& out, std::string* error = nullptr,
                                      std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
