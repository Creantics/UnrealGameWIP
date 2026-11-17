// Demo boot defaults (ftcore/DemoBoot.h): per-map switches after the command line, the command line winning.

#include "ftcore/DemoBoot.h"

#include <doctest/doctest.h>

#include <string>

namespace {

// The first "-Key=" value in a switch line, the way FParse::Value reads it (first match wins).
std::string firstValue(const std::string& line, const std::string& key)
{
    const std::size_t at = line.find("-" + key + "=");
    if (at == std::string::npos) {
        return {};
    }
    const std::size_t from = at + key.size() + 2;
    const std::size_t end = line.find(' ', from);
    return line.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

constexpr const char* kTable = R"({"maps": [
  {"map": "World_Solhaven_Strip", "status": "E8", "switches": "-TimeOfDay=21 -NightEV=-1.85 -NightEVRange=0"},
  {"map": "/Game/Maps/World_Other.World_Other", "switches": ""}
]})";

} // namespace

TEST_CASE("demo boot: rows parse and are found by any map-name form")
{
    ftcore::DemoBootTable t;
    std::string error;
    REQUIRE(ftcore::parseDemoBoot(kTable, t, &error));
    REQUIRE(t.rows.size() == 2);
    CHECK(t.rows[1].map == "World_Other");
    CHECK(ftcore::demoBootFind(t, "World_Solhaven_Strip") == &t.rows[0]);
    CHECK(ftcore::demoBootFind(t, "/Game/Maps/World_Solhaven_Strip.World_Solhaven_Strip") == &t.rows[0]);
    CHECK(ftcore::demoBootFind(t, "UEDPIE_0_World_Solhaven_Strip") == &t.rows[0]);
    CHECK(ftcore::demoBootFind(t, "world_solhaven_strip") == &t.rows[0]);
    CHECK(ftcore::demoBootFind(t, "World_Solhaven_Strip_HeroCorner") == nullptr);
    CHECK(ftcore::demoBootFind(t, "Demo") == nullptr);
}

TEST_CASE("demo boot: the command line comes first, so its switches win")
{
    ftcore::DemoBootTable t;
    REQUIRE(ftcore::parseDemoBoot(kTable, t));
    const ftcore::DemoBootRow* row = ftcore::demoBootFind(t, "World_Solhaven_Strip");
    const std::string plain = ftcore::demoBootSwitches("x.uproject -game", row);
    CHECK(firstValue(plain, "TimeOfDay") == "21");
    CHECK(firstValue(plain, "NightEVRange") == "0");
    const std::string given = ftcore::demoBootSwitches("x.uproject -game -TimeOfDay=8", row);
    CHECK(firstValue(given, "TimeOfDay") == "8");
    CHECK(firstValue(given, "NightEV") == "-1.85");
    CHECK(ftcore::demoBootSwitches("-game", nullptr) == "-game");
    CHECK(ftcore::demoBootSwitches("-game", ftcore::demoBootFind(t, "World_Other")) == "-game");
}

TEST_CASE("demo boot: bad tables are refused and leave nothing")
{
    ftcore::DemoBootTable t;
    std::string error;
    CHECK_FALSE(ftcore::parseDemoBoot("{\"maps\": [", t, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(ftcore::parseDemoBoot("{}", t, &error));
    CHECK_FALSE(ftcore::parseDemoBoot(R"({"maps": [{"map": "A"}]})", t, &error));
    CHECK_FALSE(ftcore::parseDemoBoot(R"({"maps": [{"map": "A", "switches": "TimeOfDay=21"}]})", t, &error));
    CHECK(error.find("TimeOfDay=21") != std::string::npos);
    CHECK_FALSE(ftcore::parseDemoBoot(R"({"maps": [{"map": "A", "switches": "-x"}, {"map": "a", "switches": "-y"}]})", t, &error));
    CHECK(t.rows.empty());
    const std::string quoted = "{\"maps\": [{\"map\": \"A\", \"switches\": \"-ExecCmds=\\\"a 1, b 2\\\" -y\"}]}";
    const bool quotedOk = ftcore::parseDemoBoot(quoted, t, &error);
    CHECK(quotedOk);
}

TEST_CASE("demo boot: a play-in-editor world finds its map's row")
{
    ftcore::DemoBootTable t;
    const char* strip = R"({"maps": [{"map": "World_Solhaven_Strip", "switches": "-TimeOfDay=21 -NightEVRange=0 -WorldEyeWet=1"}]})";
    REQUIRE(ftcore::parseDemoBoot(strip, t));
    const std::string line = ftcore::demoBootSwitches("-game", ftcore::demoBootFind(t, "UEDPIE_2_World_Solhaven_Strip"));
    CHECK(firstValue(line, "WorldEyeWet") == "1");
}
