// The recurring city profiles on the shipped tables (Data/Npc): every profile loads, sits at its intended stance, keeps
// it through a whole day and night with no deed, and the store stays inside its budget. Also npc_places.csv and the
// where-by-hour lookup.
#include "ftcore/NpcMemory.h"
#include "ftcore/NpcPlaces.h"
#include "ftcore/WorldClock.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

std::string readShipped(const char* name) {
    std::ifstream f(std::string(FTCORE_PROJECT_DATA_DIR) + "/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Shipped {
    std::string files[10];
    NpcTables t;
    NpcPlaceTable places;
    TimeCurves curves;

    explicit Shipped(bool withExpectations = true) {
        const char* names[] = {"Npc/tags.csv",         "Npc/deeds.csv",        "Npc/npc_values.csv", "Npc/ties.csv",
                               "Npc/consequences.csv", "Npc/expectations.csv", "Npc/schedule.csv",   "Npc/memory_tuning.json",
                               "time_curves.csv",      "Npc/npc_places.csv"};
        for (int i = 0; i < 10; ++i) {
            files[i] = readShipped(names[i]);
            INFO(names[i]);
            REQUIRE(!files[i].empty());
        }
        NpcTableSources src{files[0], files[1], files[2], files[3], files[4], withExpectations ? std::string_view(files[5]) : std::string_view{},
                            files[6], files[7]};
        std::string err;
        const bool tablesOk = importNpcTables(src, t, &err);
        INFO(err);
        REQUIRE(tablesOk);
        REQUIRE(parseTimeCurves(files[8], curves, &err));
        const bool placesOk = importNpcPlaces(files[9], t, places, &err);
        INFO(err);
        REQUIRE(placesOk);
    }
    int place(const char* npc, double hours) const {
        const int i = t.npcIndex(npc);
        REQUIRE(i >= 0);
        return npcPlaceAt(places, static_cast<std::uint16_t>(i), hours);
    }
    int placeId(const char* id) const {
        for (std::size_t i = 0; i < places.places.size(); ++i)
            if (places.places[i] == id) return static_cast<int>(i);
        return -2;
    }
};

// The stance each profile is written to start at (the private profile sheet explains each one).
struct Expected {
    const char* id;
    NpcStance stance;
};
constexpr Expected kProfiles[] = {
    {"npc_sol_01", NpcStance::Warm},    {"npc_sol_02", NpcStance::Warm},    {"npc_sol_03", NpcStance::Warm},
    {"npc_sol_04", NpcStance::Neutral}, {"npc_sol_05", NpcStance::Warm},    {"npc_sol_06", NpcStance::Neutral},
    {"npc_sol_07", NpcStance::Neutral}, {"npc_sol_08", NpcStance::Neutral},
};
constexpr std::size_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

}  // namespace

TEST_CASE("npc profiles: every shipped profile keeps its stance through a quiet day and night") {
    // Without the expectation table: a missed meeting is a deed by design (the next test).
    Shipped s(false);
    REQUIRE(s.t.npcs.size() <= kNpcMax);
    auto w = std::make_unique<NpcMemoryWorld>();
    npcInit(*w, s.t);
    REQUIRE(w->npcCount == s.t.npcs.size());

    // The list above is every city profile shipped, and only they and the slice's three stand anywhere.
    std::size_t shipped = 0;
    for (const NpcDef& d : s.t.npcs) shipped += d.id.rfind("npc_sol_", 0) == 0 ? 1u : 0u;
    CHECK(shipped == kProfileCount);
    for (const NpcPlaceSpan& sp : s.places.spans) {
        const std::string& id = s.t.npcs[sp.npc].id;
        INFO(id);
        CHECK((id.rfind("npc_sol_", 0) == 0 || id == "npc_shop_01" || id == "npc_fixer_01" || id == "npc_kid_01"));
    }

    for (const Expected& e : kProfiles) {
        INFO(e.id);
        const int i = s.t.npcIndex(e.id);
        REQUIRE(i >= 0);
        // A lead offset starts at zero, so every lead and the party see the base.
        for (std::uint8_t lead : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}, kNoLead})
            CHECK(npcStance(*w, s.t, static_cast<std::uint16_t>(i), lead) == e.stance);
        // Each starting stance has its own consequence row: never the filled-in default.
        const std::size_t c = static_cast<std::size_t>(i) * 5 + static_cast<std::size_t>(e.stance);
        REQUIRE(c < s.t.consequences.size());
        CHECK(s.t.consequences[c].greetDistCm > 0);
        // And somewhere to stand for part of the day.
        bool placed = false;
        for (const NpcPlaceSpan& sp : s.places.spans) placed = placed || sp.npc == i;
        CHECK(placed);
    }

    // Snapshot every NPC (the slice's three as well), then walk 06:00 day 1 to 06:00 day 2, hour by hour.
    std::vector<std::array<NpcStance, kLeadCount + 1>> before(w->npcCount);
    std::vector<std::array<std::int32_t, 3>> axesBefore(w->npcCount);
    for (std::uint16_t n = 0; n < w->npcCount; ++n) {
        for (std::uint8_t l = 0; l < kLeadCount; ++l) before[n][l] = npcStance(*w, s.t, n, l);
        before[n][kLeadCount] = npcStance(*w, s.t, n, kNoLead);
        for (int a = 0; a < 3; ++a) axesBefore[n][static_cast<std::size_t>(a)] = npcAxis(*w, s.t, n, static_cast<NpcAxis>(a));
    }
    WorldClock clock;
    for (int step = 0; step <= 24; ++step) {
        clock.day = 1 + static_cast<std::uint32_t>((6 + step) / 24);
        clock.hours = static_cast<double>((6 + step) % 24);
        npcOnClock(*w, s.t, clock, s.curves);
        INFO("hour step " << step);
        for (std::uint16_t n = 0; n < w->npcCount; ++n) {
            INFO(s.t.npcs[n].id);
            for (std::uint8_t l = 0; l < kLeadCount; ++l) CHECK(npcStance(*w, s.t, n, l) == before[n][l]);
            CHECK(npcStance(*w, s.t, n, kNoLead) == before[n][kLeadCount]);
            for (int a = 0; a < 3; ++a)
                CHECK(npcAxis(*w, s.t, n, static_cast<NpcAxis>(a)) == axesBefore[n][static_cast<std::size_t>(a)]);
            CHECK(w->npcs[n].memCount == 0);
            CHECK(w->npcs[n].memCount <= kMemPerNpc);
        }
        CHECK(w->rumourCount == 0);
        CHECK(w->droppedRumours == 0);
        CHECK(w->nextSeq == 1);  // no deed was recorded
    }
    CHECK(w->now == npcMinute(2, 6.0));
}

TEST_CASE("npc profiles: with the shipped expectations, a quiet day records only the missed meetings") {
    Shipped s;
    auto w = std::make_unique<NpcMemoryWorld>();
    npcInit(*w, s.t);
    std::vector<std::uint16_t> owners;
    for (const NpcExpDef& e : s.t.expectations) owners.push_back(e.npc);
    std::array<NpcStance, kProfileCount> before{};
    for (std::size_t i = 0; i < kProfileCount; ++i)
        before[i] = npcStance(*w, s.t, static_cast<std::uint16_t>(s.t.npcIndex(kProfiles[i].id)), kNoLead);
    WorldClock clock;
    for (int step = 0; step <= 24; ++step) {
        clock.day = 1 + static_cast<std::uint32_t>((6 + step) / 24);
        clock.hours = static_cast<double>((6 + step) % 24);
        npcOnClock(*w, s.t, clock, s.curves);
    }
    // Every deed recorded is an expectation's missed deed; no profile owns an expectation or hears of one.
    for (std::uint32_t seq = 1; seq < w->nextSeq; ++seq) {
        const NpcDeed* d = npcDeedAt(*w, seq);
        REQUIRE(d != nullptr);
        bool fromExp = false;
        for (const NpcExpDef& e : s.t.expectations) fromExp = fromExp || d->type == e.deedMissed;
        CHECK(fromExp);
    }
    CHECK(w->nextSeq - 1 <= s.t.expectations.size());
    for (std::size_t i = 0; i < kProfileCount; ++i) {
        INFO(kProfiles[i].id);
        const auto n = static_cast<std::uint16_t>(s.t.npcIndex(kProfiles[i].id));
        CHECK(std::find(owners.begin(), owners.end(), n) == owners.end());
        CHECK(npcStance(*w, s.t, n, kNoLead) == before[i]);
        CHECK(w->npcs[n].memCount == 0);
    }
    CHECK(w->droppedRumours == 0);
    CHECK(w->rumourCount <= kRumourQueue);
}

TEST_CASE("npc profiles: where each one stands by the hour") {
    Shipped s;
    // Mid-span, a span's first minute, a span's end (the next span's start), past midnight, and off the map.
    CHECK(s.place("npc_sol_01", 10.0) == s.placeId("sol_place_01"));
    CHECK(s.place("npc_sol_01", 19.0) == s.placeId("sol_place_06"));
    CHECK(s.place("npc_sol_01", 3.0) == -1);
    CHECK(s.place("npc_sol_06", 23.5) == s.placeId("sol_place_05"));
    CHECK(s.place("npc_sol_06", 1.99) == s.placeId("sol_place_05"));
    CHECK(s.place("npc_sol_06", 2.0) == -1);
    CHECK(s.place("npc_sol_06", 12.0) == -1);
    CHECK(s.place("npc_sol_06", 25.0) == s.placeId("sol_place_05"));   // wraps to 01:00
    CHECK(s.place("npc_sol_06", -1.0) == s.placeId("sol_place_05"));   // 23:00 the day before
    CHECK(s.place("npc_sol_07", 12.0) == -1);                           // the gap between his two spans
    CHECK(s.place("npc_sol_07", 20.0) == s.placeId("sol_place_09"));
    CHECK(npcPlaceAt(s.places, kNoNpc, 12.0) == -1);
    // Odd clock values: NaN and infinity read as midnight, huge hours wrap, a tiny negative is the day's last minute.
    CHECK(s.place("npc_sol_06", std::nan("")) == s.placeId("sol_place_05"));
    CHECK(s.place("npc_sol_06", 1e9 + 1.0) == s.place("npc_sol_06", std::fmod(1e9 + 1.0, 24.0)));
    CHECK(s.place("npc_sol_06", -1e-9) == s.placeId("sol_place_05"));
    CHECK(s.place("npc_sol_01", -1e-9) == -1);
}

TEST_CASE("npc profiles: a span starting off the hour reads from its first minute") {
    Shipped s;
    NpcPlaceTable p;
    std::string err;
    REQUIRE(importNpcPlaces("\xEF\xBB\xBFnpc_id,from,to,place_id\n\nnpc_sol_01,08:33,09:00,a\n  \n", s.t, p, &err));
    const auto n = static_cast<std::uint16_t>(s.t.npcIndex("npc_sol_01"));
    CHECK(npcPlaceAt(p, n, 8.55) == 0);  // 08:33 exactly
    CHECK(npcPlaceAt(p, n, 8.54) == -1);
}

TEST_CASE("npc profiles: a storm of deeds keeps every store inside its budget") {
    Shipped s;
    auto w = std::make_unique<NpcMemoryWorld>();
    npcInit(*w, s.t);
    std::vector<std::uint16_t> profiles;
    for (const Expected& e : kProfiles) profiles.push_back(static_cast<std::uint16_t>(s.t.npcIndex(e.id)));
    // 300 deeds (more than the ring holds), every deed type in turn, at four places in the profiles' districts,
    // each seen by up to eight profiles and done to one of them, by each lead in turn.
    std::uint32_t recorded = 0;
    for (std::uint32_t k = 0; k < 300; ++k) {
        NpcDeed d;
        d.minute = npcMinute(1, 8.0) + k;
        d.type = static_cast<std::uint16_t>(k % s.t.deeds.size());
        d.place = static_cast<std::uint16_t>(1 + k % 4);
        d.district = static_cast<std::uint16_t>(2 + k % 3);
        d.lead = static_cast<std::uint8_t>(k % kLeadCount);
        d.victim = profiles[k % profiles.size()];
        for (std::size_t j = 0; j < kDeedWitnessMax && j < profiles.size(); ++j)
            d.witnesses[j] = profiles[(k + j + 1) % profiles.size()];
        npcAdvance(*w, d.minute);
        if (npcRecordDeed(*w, s.t, d) != 0) ++recorded;
    }
    CHECK(recorded == 300);
    WorldClock clock;
    for (int step = 0; step <= 24; ++step) {
        clock.day = 1 + static_cast<std::uint32_t>((14 + step) / 24);
        clock.hours = static_cast<double>((14 + step) % 24);
        npcOnClock(*w, s.t, clock, s.curves);
        for (std::uint16_t n = 0; n < w->npcCount; ++n) {
            INFO(s.t.npcs[n].id);
            CHECK(w->npcs[n].memCount <= kMemPerNpc);
            for (std::uint8_t l = 0; l < kLeadCount; ++l) CHECK(static_cast<int>(npcStance(*w, s.t, n, l)) <= 4);
        }
        CHECK(w->rumourCount <= kRumourQueue);
    }
    // The storm was felt: every profile remembers something.
    for (std::uint16_t n : profiles) CHECK(w->npcs[n].memCount > 0);
}

TEST_CASE("npc profiles: npc_places.csv refuses bad rows") {
    Shipped s;
    const auto bad = [&](const char* csv) {
        NpcPlaceTable p;
        p.places.emplace_back("kept");
        std::string err;
        const bool ok = importNpcPlaces(csv, s.t, p, &err);
        INFO(std::string(csv));
        CHECK(!ok);
        CHECK(!err.empty());
        CHECK(p.places.size() == 1);  // untouched on failure
        CHECK(p.spans.empty());
    };
    bad("npc,from,to,place_id\n");
    bad("npc_id,from,to,place_id\nnobody,08:00,09:00,a\n");
    bad("npc_id,from,to,place_id\nnpc_sol_01,8:00,09:00,a\n");
    bad("npc_id,from,to,place_id\nnpc_sol_01,08:00,24:30,a\n");
    bad("npc_id,from,to,place_id\nnpc_sol_01,08:60,09:00,a\n");
    bad("npc_id,from,to,place_id\nnpc_sol_01,08:00,08:00,a\n");
    bad("npc_id,from,to,place_id\nnpc_sol_01,24:00,00:00,a\n");
    bad("npc_id,from,to,place_id\nnpc_sol_01,08:00,09:00,\n");
    bad("npc_id,from,to,place_id\nnpc_sol_01,08:00,12:00,a\nnpc_sol_01,11:59,13:00,b\n");
    bad("npc_id,from,to,place_id\nnpc_sol_01,22:00,02:00,a\nnpc_sol_01,01:00,03:00,b\n");
    bad("");

    // Good: touching spans, a wrap to midnight written as 00:00, the whole day, CRLF lines.
    NpcPlaceTable p;
    std::string err;
    REQUIRE(importNpcPlaces("npc_id,from,to,place_id\r\nnpc_sol_01,08:00,12:00,a\r\nnpc_sol_01,12:00,00:00,b\r\n"
                            "npc_sol_02,00:00,24:00,a\r\n",
                            s.t, p, &err));
    REQUIRE(p.places.size() == 2);
    const std::uint16_t first = static_cast<std::uint16_t>(s.t.npcIndex("npc_sol_01"));
    const std::uint16_t second = static_cast<std::uint16_t>(s.t.npcIndex("npc_sol_02"));
    CHECK(npcPlaceAt(p, first, 11.99) == 0);
    CHECK(npcPlaceAt(p, first, 12.0) == 1);
    CHECK(npcPlaceAt(p, first, 23.99) == 1);
    CHECK(npcPlaceAt(p, first, 0.0) == -1);
    CHECK(npcPlaceAt(p, second, 0.0) == 0);
    CHECK(npcPlaceAt(p, second, 23.99) == 0);
}
