// The outer field's places (ftcore/OuterPlaces.h): the shipped Data/Field/ring_places.json, the reader's
// refusals and the outer districts' discovery indices. Made-up places only; ids and metres.

#include "ftcore/OuterPlaces.h"
#include "ftcore/WorldLayout.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {

std::string withPlaces(const std::string& items) { return "{ \"version\": 1, \"places\": [" + items + "] }"; }

bool refused(const std::string& json) {
    std::vector<OuterPlace> out(1);
    std::string error;
    const bool ok = importOuterPlaces(json, out, &error);
    return !ok && out.empty() && !error.empty();
}

OuterPlace place(std::uint16_t id, OuterPlace::Kind kind) {
    OuterPlace p;
    p.id = id;
    p.kind = kind;
    return p;
}

} // namespace

TEST_CASE("OuterPlaces: the shipped file reads, with rest points and districts") {
    std::string text, error;
    REQUIRE(ftcore_test::readProjectData("Field/ring_places.json", text));
    std::vector<OuterPlace> out(3);
    const bool ok = importOuterPlaces(text, out, &error);
    INFO(error);
    REQUIRE(ok);
    int rest = 0, districts = 0;
    for (const OuterPlace& p : out) {
        rest += p.kind == OuterPlace::Kind::RestPoint ? 1 : 0;
        districts += p.kind == OuterPlace::Kind::District ? 1 : 0;
        CHECK(p.radius > 0.0f);
    }
    CHECK(rest >= 9);
    CHECK(districts >= 1);
}

TEST_CASE("OuterPlaces: a list reads in file order") {
    std::vector<OuterPlace> out;
    std::string error;
    REQUIRE(importOuterPlaces(withPlaces("{ \"id\": 4, \"kind\": \"district\", \"x\": 1200.5, \"y\": -340, \"radius\": 250 },"
                                         "{ \"id\": 0, \"kind\": \"rest_point\", \"x\": 0, \"y\": 0, \"radius\": 0 },"
                                         "{ \"id\": 65535, \"kind\": \"place\", \"x\": -5e3, \"y\": 7.25, \"radius\": 12.5 }"),
                              out, &error));
    REQUIRE(out.size() == 3);
    CHECK(out[0].id == 4);
    CHECK(out[0].kind == OuterPlace::Kind::District);
    CHECK(out[0].x == doctest::Approx(1200.5f));
    CHECK(out[0].y == doctest::Approx(-340.0f));
    CHECK(out[0].radius == doctest::Approx(250.0f));
    CHECK(out[1].kind == OuterPlace::Kind::RestPoint);
    CHECK(out[2].id == 65535);
    CHECK(out[2].kind == OuterPlace::Kind::Place);
    CHECK(out[2].x == doctest::Approx(-5000.0f));
    CHECK(outerPlaceKindName(OuterPlace::Kind::District) == "district");
    CHECK(outerPlaceKindName(OuterPlace::Kind::RestPoint) == "rest_point");
    CHECK(outerPlaceKindName(OuterPlace::Kind::Place) == "place");
}

TEST_CASE("OuterPlaces: the reader refuses duplicate ids, unknown kinds and numbers that are not finite") {
    const std::string a = "{ \"id\": 1, \"kind\": \"district\", \"x\": 0, \"y\": 0, \"radius\": 10 }";
    CHECK_FALSE(refused(withPlaces(a)));
    CHECK(refused(withPlaces(a + "," + a)));                                                           // id 1 twice
    CHECK(refused(withPlaces("{ \"id\": 1, \"kind\": \"town\", \"x\": 0, \"y\": 0, \"radius\": 10 }"))); // unknown kind
    CHECK(refused(withPlaces("{ \"id\": 1, \"kind\": 3, \"x\": 0, \"y\": 0, \"radius\": 10 }")));
    CHECK(refused(withPlaces("{ \"id\": 1, \"kind\": \"place\", \"x\": 1e300, \"y\": 0, \"radius\": 10 }"))); // inf as a float
    CHECK(refused(withPlaces("{ \"id\": 1, \"kind\": \"place\", \"x\": 0, \"y\": -1e39, \"radius\": 10 }")));
    CHECK(refused(withPlaces("{ \"id\": 1, \"kind\": \"place\", \"x\": 0, \"y\": 0, \"radius\": 1e999 }"))); // out of range
    CHECK(refused(withPlaces("{ \"id\": 1, \"kind\": \"place\", \"x\": \"0\", \"y\": 0, \"radius\": 10 }")));
    CHECK(refused(withPlaces("{ \"id\": 1, \"kind\": \"place\", \"x\": 0, \"y\": 0 }")));                  // no radius
    CHECK(refused(withPlaces("{ \"id\": 1, \"kind\": \"place\", \"x\": 0, \"y\": 0, \"radius\": -1 }")));
    CHECK(refused(withPlaces("{ \"id\": 70000, \"kind\": \"place\", \"x\": 0, \"y\": 0, \"radius\": 1 }")));
    CHECK(refused(withPlaces("{ \"id\": 1.5, \"kind\": \"place\", \"x\": 0, \"y\": 0, \"radius\": 1 }")));
    CHECK(refused(withPlaces("{ \"id\": 1, \"kind\": \"place\", \"x\": 0, \"y\": 0, \"radius\": 1, \"label\": \"x\" }")));
    CHECK(refused("{ \"version\": 2, \"places\": [] }"));
    CHECK(refused("{ \"version\": 1 }"));
    CHECK(refused("{ \"version\": 1, \"places\": [], \"extra\": 0 }"));
    CHECK(refused("[ ]"));
    CHECK(refused("{ \"version\": 1, \"places\": [ "));
}

TEST_CASE("OuterPlaces: outer districts take discovery indices from 16, in file order") {
    CHECK(static_cast<std::size_t>(kOuterDistrictFirstIndex) == WorldLayout::kMaxDistricts);
    const std::vector<OuterPlace> places = {place(5, OuterPlace::Kind::District), place(9, OuterPlace::Kind::RestPoint),
                                            place(2, OuterPlace::Kind::District), place(3, OuterPlace::Kind::Place),
                                            place(7, OuterPlace::Kind::District)};
    CHECK(outerDistrictDiscoveryIndex(places, 0) == 16);
    CHECK(outerDistrictDiscoveryIndex(places, 1) == -1);
    CHECK(outerDistrictDiscoveryIndex(places, 2) == 17);
    CHECK(outerDistrictDiscoveryIndex(places, 3) == -1);
    CHECK(outerDistrictDiscoveryIndex(places, 4) == 18);
    CHECK(outerDistrictDiscoveryIndex(places, 5) == -1);
    CHECK(outerDistrictDiscoveryIndex({}, 0) == -1);
}
