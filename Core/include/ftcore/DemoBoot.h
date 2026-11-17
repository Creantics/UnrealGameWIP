#pragma once

// Demo boot defaults (Data/Field/demo_boot.json): per map, the switches a plain launch of that map runs with, so the
// demo starts in the proven state (night, the proven white and keys, a fixed exposure, wet ground, the player near the
// hero corner) with no command line. The game's switch readers see the process's command line FIRST and the map's
// boot switches after it; a reader takes the first match, so any switch given on the command line wins over the
// row's. -NoDemoBoot on the command line turns the rows off.
//
//   { "maps": [ { "map": "World_Solhaven_Strip", "status": "...", "switches": "-TimeOfDay=21 -NightEV=-1.85 ..." } ] }
//
// Parsing allocates (load time); demoBootFind and demoBootMapName do not.

#include "ftcore/Api.h"

#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct DemoBootRow {
    std::string map;      // the map's short name (World_Solhaven_Strip)
    std::string switches; // "-A=1 -B": appended after the command line
    std::string status;   // what proved it (free text, for the log)
};

struct DemoBootTable {
    std::vector<DemoBootRow> rows;
};

// False on bad JSON, a row without "map" or "switches", a switch list with a token not starting with '-', or a map
// named twice; *error then says which. The table is left empty on failure.
FTCORE_API bool parseDemoBoot(std::string_view text, DemoBootTable& out, std::string* error = nullptr);

// A map name as a short name: the package path's last part ("/Game/Maps/World_X.World_X" -> "World_X"), without a
// play-in-editor prefix ("UEDPIE_0_World_X" -> "World_X"). A view into `name`.
FTCORE_API std::string_view demoBootMapName(std::string_view name);

// The row for a map (any form demoBootMapName takes, case-insensitive), or null.
FTCORE_API const DemoBootRow* demoBootFind(const DemoBootTable& table, std::string_view map);

// The command line with the row's switches after it ("<cmd> <switches>"); just the command line when row is null.
FTCORE_API std::string demoBootSwitches(std::string_view commandLine, const DemoBootRow* row);

} // namespace ftcore
