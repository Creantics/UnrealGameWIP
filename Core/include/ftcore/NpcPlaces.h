#pragma once

// Where each remembered NPC stands by the hour (Data/Npc/npc_places.csv): npc_id,from,to,place_id. Times are HH:MM
// from 00:00 to 24:00 (from below 24:00; to 00:00 means midnight); a span with to < from runs past midnight. One NPC's spans may not overlap; an hour no span
// covers means the NPC is off the map (home, asleep, away). Place ids are the level's own names for spots
// (the game resolves them to markers); Core only checks them, keeps them, and answers "where now".
// Plain C++20. Allocates only in importNpcPlaces (load time); npcPlaceAt does not allocate.

#include "ftcore/Api.h"
#include "ftcore/NpcMemory.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

constexpr std::uint16_t kNpcDayMinutes = 24 * 60;

struct NpcPlaceSpan {
    std::uint16_t npc = 0;      // index in NpcTables::npcs
    std::uint16_t from = 0;     // minute of the day, 0..1439
    std::uint16_t to = 0;       // minute of the day, 1..1440; to < from wraps past midnight
    std::uint16_t place = 0;    // index in NpcPlaceTable::places
};

struct NpcPlaceTable {
    std::vector<std::string> places;  // distinct place ids, in first-seen order
    std::vector<NpcPlaceSpan> spans;  // file order
};

// False (out untouched) with *error naming the row on a missing column, an unknown npc, a bad or empty time span,
// an empty place id, or two spans of one NPC that overlap.
FTCORE_API bool importNpcPlaces(std::string_view csv, const NpcTables& t, NpcPlaceTable& out, std::string* error = nullptr);
// The place index for the NPC at that hour of the day (any value, wrapped to 0..24), or -1 when off the map.
FTCORE_API int npcPlaceAt(const NpcPlaceTable& p, std::uint16_t npc, double hours);

}  // namespace ftcore
