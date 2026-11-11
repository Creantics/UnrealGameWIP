// The NPC memory slice on the shipped tables and lines (Data/Npc): the save text, the story-fact clause, and one
// full day and night for the three neighbours.
#include "ftcore/NpcMemory.h"
#include "ftcore/WorldClock.h"
#include <doctest/doctest.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace ftcore;

namespace {

std::string readShipped(const char* name) {
    std::ifstream f(std::string(FTCORE_PROJECT_DATA_DIR) + "/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Two story facts, "f_choice" (a choice) and "f_event"; 0 means unset.
struct StubFacts final : NpcFactSource {
    std::uint8_t values[2] = {0, 0};
    int npcFactIndex(std::string_view id) const override { return id == "f_choice" ? 0 : id == "f_event" ? 1 : -1; }
    bool npcFactValue(int index, std::uint8_t& value) const override {
        if (index < 0 || index > 1 || values[index] == 0) return false;
        value = values[index];
        return true;
    }
};

struct Town {
    std::string files[10];
    NpcTables t;
    NpcLineTable lines;
    NpcGameDeeds g;
    TimeCurves curves;
    std::unique_ptr<NpcMemoryWorld> w = std::make_unique<NpcMemoryWorld>();
    WorldClock clock;

    Town() {
        const char* names[] = {"Npc/tags.csv",     "Npc/deeds.csv",        "Npc/npc_values.csv",     "Npc/ties.csv",
                               "Npc/consequences.csv", "Npc/expectations.csv", "Npc/schedule.csv",
                               "Npc/memory_tuning.json", "time_curves.csv", "Npc/npc_lines.csv"};
        for (int i = 0; i < 10; ++i) {
            files[i] = readShipped(names[i]);
            INFO(names[i]);
            REQUIRE(!files[i].empty());
        }
        NpcTableSources src{files[0], files[1], files[2], files[3], files[4], files[5], files[6], files[7]};
        std::string err;
        REQUIRE(importNpcTables(src, t, &err));
        INFO(err);
        REQUIRE(importNpcLines(files[9], t, lines, &err));
        REQUIRE(npcResolveGameDeeds(t, g, &err));
        REQUIRE(parseTimeCurves(files[8], curves, &err));
        npcInit(*w, t);
        at(1, 6.0);
    }
    std::uint16_t npc(const char* id) const { return static_cast<std::uint16_t>(t.npcIndex(id)); }
    void at(std::uint32_t day, double hours) {
        clock.day = day;
        clock.hours = hours;
        npcOnClock(*w, t, clock, curves);
    }
    NpcWhere where(std::uint8_t lead, std::uint16_t place = 7) const {
        NpcWhere a;
        a.place = place;
        a.district = 1;
        a.lead = lead;
        return a;
    }
    std::string greet(const char* who, std::uint8_t lead = 0, NpcLineKind k = NpcLineKind::Greet) {
        const int i = npcPickLine(*w, t, lines, npc(who), lead, k).line;
        return i < 0 ? std::string("-") : lines.lines[static_cast<std::size_t>(i)].id;
    }
    std::string saveText() const { return encodeNpcMemorySave(npcCaptureSave(*w, t, lines)); }
};

}  // namespace

TEST_CASE("npc save text: a world written, read back and applied writes the same text again") {
    Town a;
    const std::uint16_t shop = a.npc("npc_shop_01"), kid = a.npc("npc_kid_01");
    a.at(1, 9.0);
    npcOnDelivery(*a.w, a.t, a.g, a.where(0), shop);
    npcOnDelivery(*a.w, a.t, a.g, a.where(1), shop);  // both leads now on that memory
    npcOnGift(*a.w, a.t, a.g, a.where(2), kid, false);
    npcOnVanKnockdown(*a.w, a.t, a.g, a.where(1, 3), kNoNpc);
    a.greet("npc_shop_01");
    a.at(1, 12.0);
    const std::string text = a.saveText();
    CHECK(text.find("\"leads\":3") != std::string::npos);

    NpcMemorySave read;
    std::string err;
    REQUIRE(decodeNpcMemorySave(text, read, &err));
    Town b;
    const NpcMemoryLoadReport rep = npcApplySave(*b.w, b.t, b.lines, read);
    CHECK(rep.formatOk);
    CHECK(rep.unknownIds.empty());
    CHECK(b.saveText() == text);

    SUBCASE("an older text without leads gets them back from the doer") {
        std::string old = text;
        for (std::size_t p; (p = old.find(",\"leads\":")) != std::string::npos;) {
            const std::size_t end = old.find_first_of(",}", p + 1);
            old.erase(p, end - p);
        }
        REQUIRE(old.find("leads") == std::string::npos);
        NpcMemorySave o;
        REQUIRE(decodeNpcMemorySave(old, o));
        Town c;
        npcApplySave(*c.w, c.t, c.lines, o);
        const NpcMemoryEntry* e = npcRecall(*c.w, c.t, shop, c.t.deedIndex("delivery_made"), -1);
        REQUIRE(e != nullptr);
        CHECK(e->leads == (1u << e->lead));  // only the latest doer is known
    }
}

TEST_CASE("npc save text: damaged text is refused and leaves the save untouched") {
    NpcMemorySave s;
    s.now = 77;
    for (const char* bad : {"", "[]", "{\"format\":9}", "{\"now\":1}", "{\"format\":1}", "{\"format\":1,\"npcs\":{}}",
                            "{\"format\":1,\"npcs\":[{\"axes\":[1,2,3]}]}",
                            "{\"format\":1,\"npcs\":[],\"deeds\":[{\"seq\":1}]}", "{\"format\":1,\"npcs\":[],\"said\":[{\"at\":2}]}"}) {
        INFO(bad);
        std::string err;
        CHECK_FALSE(decodeNpcMemorySave(bad, s, &err));
        CHECK(!err.empty());
        CHECK(s.now == 77u);
    }
    // Out-of-range numbers are clamped, not refused.
    REQUIRE(decodeNpcMemorySave("{\"format\":1,\"now\":-5,\"npcs\":[{\"id\":\"x\",\"axes\":[99999,-99999,0]}]}", s));
    CHECK(s.now == 0u);
    REQUIRE(s.npcs.size() == 1u);
    CHECK(s.npcs[0].axes[0] == 32767);
    CHECK(s.npcs[0].axes[1] == -32768);
}

TEST_CASE("npc lines: a fact clause reads a story fact through the fact source") {
    Town s;
    StubFacts facts;
    const char* csv = "line_id,npc_id,kind,lead,priority,cooldown_min,once,cond\n"
                      "plain,npc_kid_01,greet,any,1,0,0,\n"
                      "chose_b,npc_kid_01,greet,any,5,0,0,fact:f_choice=B\n"
                      "event_seen,npc_kid_01,greet,any,4,0,0,fact:f_event=1\n";
    NpcLineTable lines;
    std::string err;
    CHECK_FALSE(importNpcLines(csv, s.t, lines, &err));  // a fact clause needs the source
    REQUIRE(importNpcLines(csv, s.t, &facts, lines, &err));
    for (const char* bad : {"fact:f_nope=A", "fact:f_choice=E", "fact:f_choice=255", "fact:=A", "fact:f_choice",
                            "fact:f_choice=-1"}) {
        INFO(bad);
        NpcLineTable l;
        const std::string row = std::string("line_id,npc_id,kind,lead,priority,cooldown_min,once,cond\nx,npc_kid_01,greet,any,1,0,0,") + bad + "\n";
        CHECK_FALSE(importNpcLines(row, s.t, &facts, l));
    }
    const std::uint16_t kid = s.npc("npc_kid_01");
    auto pick = [&](const NpcFactSource* f) {
        const int i = npcPickLine(*s.w, s.t, lines, kid, 0, NpcLineKind::Greet, f).line;
        return i < 0 ? std::string("-") : lines.lines[static_cast<std::size_t>(i)].id;
    };
    CHECK(pick(&facts) == "plain");  // nothing set yet
    facts.values[0] = 1;             // chose A
    CHECK(pick(&facts) == "plain");
    facts.values[1] = 1;
    CHECK(pick(&facts) == "event_seen");
    facts.values[0] = 2;  // B
    CHECK(pick(&facts) == "chose_b");
    CHECK(pick(nullptr) == "plain");  // no source: the clause fails
}

TEST_CASE("npc slice: one day and night for the three neighbours on the shipped lines, saved and loaded at midnight") {
    Town s;
    const std::uint16_t shop = s.npc("npc_shop_01"), fixer = s.npc("npc_fixer_01"), kid = s.npc("npc_kid_01");
    // 08:00 the day opens: plain greets (the shopkeeper starts warm).
    s.at(1, 8.0);
    CHECK(s.greet("npc_shop_01") == "shop_greet_warm");
    CHECK(s.greet("npc_fixer_01") == "fixer_greet_plain");
    CHECK(s.greet("npc_kid_01") == "kid_greet_plain");

    // 09:00 a delivery to the shop by lead 0: thanked as yours to lead 0, as your friend's to lead 1.
    s.at(1, 9.0);
    npcOnDelivery(*s.w, s.t, s.g, s.where(0), shop);
    CHECK(s.greet("npc_shop_01", 0) == "shop_greet_delivery_self");
    CHECK(s.greet("npc_shop_01", 1) == "shop_greet_delivery_other");

    // 10:00 the kid's gift is kept by lead 2: asked about at every meeting.
    s.at(1, 10.0);
    npcOnGift(*s.w, s.t, s.g, s.where(2), kid, true);
    CHECK(s.greet("npc_kid_01", 2) == "kid_greet_gift_kept");

    // 12:00 an unseen van knock-down on a busy street reaches the shopkeeper as hearsay by 13:30.
    s.at(1, 12.0);
    const std::uint32_t noon = npcOnVanKnockdown(*s.w, s.t, s.g, s.where(1, 3), kNoNpc);
    REQUIRE(noon != 0u);
    s.at(1, 13.5);
    CHECK(npcFindMemory(*s.w, shop, noon) != nullptr);

    // 18:00 the fixer reminds of the 19:00 meeting; at 19:30 it is kept and thanked once.
    s.at(1, 18.0);
    CHECK(s.greet("npc_fixer_01", 0, NpcLineKind::Remind) == "fixer_remind_meet");
    s.at(1, 19.5);
    NpcWhere meet = s.where(0, 11);
    meet.witnesses[0] = fixer;
    npcOnMeet(*s.w, s.t, s.g, meet, fixer);
    CHECK(s.w->exp[0] == NpcExpState::Kept);
    CHECK(s.greet("npc_fixer_01") == "fixer_greet_kept");
    CHECK(s.greet("npc_fixer_01") != "fixer_greet_kept");  // once

    // 22:00 lead 1 breaks the shop's gate: raised at the next meeting, over everything else.
    s.at(1, 22.0);
    npcOnGate(*s.w, s.t, s.g, s.where(1, 9), shop, false);
    CHECK(s.greet("npc_shop_01", 0) == "shop_greet_gate_broken");

    // Midnight: saved and loaded; the night goes on in the loaded world.
    s.at(2, 0.0);
    NpcMemorySave saved;
    REQUIRE(decodeNpcMemorySave(s.saveText(), saved));
    Town n;
    CHECK(npcApplySave(*n.w, n.t, n.lines, saved).unknownIds.empty());
    n.clock = s.clock;
    CHECK(n.saveText() == s.saveText());

    // 02:00 another unseen knock-down: the night street is thin, so by 03:30 nobody has heard.
    n.at(2, 2.0);
    const std::uint32_t late = npcOnVanKnockdown(*n.w, n.t, n.g, n.where(1, 3), kNoNpc);
    REQUIRE(late != 0u);
    n.at(2, 3.5);
    CHECK(npcFindMemory(*n.w, shop, late) == nullptr);
    CHECK(npcFindMemory(*n.w, kid, late) == nullptr);
    CHECK(n.greet("npc_shop_01", 0) == "shop_greet_gate_broken");  // still down
    CHECK(n.greet("npc_kid_01", 2) == "kid_greet_gift_kept");

    // Day 2, 09:00: the gate mended by lead 1 is thanked; the kid still asks about the gift.
    n.at(2, 9.0);
    npcOnGate(*n.w, n.t, n.g, n.where(1, 9), shop, true);
    CHECK(n.greet("npc_shop_01", 0) == "shop_greet_gate_mended");
    CHECK(n.greet("npc_kid_01", 2) == "kid_greet_gift_kept");
}
