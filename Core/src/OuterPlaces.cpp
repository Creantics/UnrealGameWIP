#include "ftcore/OuterPlaces.h"

#include "ftcore/Json.h"
#include "ftcore/WorldLayout.h"

#include <cmath>
#include <limits>

namespace ftcore {

static_assert(static_cast<std::size_t>(kOuterDistrictFirstIndex) == WorldLayout::kMaxDistricts,
              "the outer districts' discovery indices start after the inner city's");

namespace {

bool failWith(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

// A finite number that stays finite as a float.
bool readFloat(JsonValue obj, const char* key, float& out) {
    const JsonValue v = obj[key];
    if (!v.isNumber()) return false;
    const double d = v.number();
    if (!std::isfinite(d) || std::fabs(d) > static_cast<double>(std::numeric_limits<float>::max())) return false;
    out = static_cast<float>(d);
    return std::isfinite(out);
}

} // namespace

std::string_view outerPlaceKindName(OuterPlace::Kind kind) {
    switch (kind) {
    case OuterPlace::Kind::District: return "district";
    case OuterPlace::Kind::RestPoint: return "rest_point";
    case OuterPlace::Kind::Place: return "place";
    }
    return "?";
}

bool importOuterPlaces(std::string_view json, std::vector<OuterPlace>& out, std::string* error) {
    out.clear();
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) return failWith(error, parseError);
    const auto bad = [&](const std::string& what) {
        out.clear();
        return failWith(error, what);
    };
    const JsonValue root = doc.root();
    if (!root.isObject()) return bad("the top level must be an object");
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "places") return bad("unknown member \"" + std::string(k) + "\"");
    }
    if (!root["version"].isInteger() || root["version"].integer() != 1) return bad("version: 1 is needed");
    const JsonValue places = root["places"];
    if (!places.isArray()) return bad("places: an array is needed");
    out.reserve(places.size());
    std::vector<bool> seen(65536, false);
    for (std::uint32_t i = 0; i < places.size(); ++i) {
        const JsonValue p = places[i];
        const std::string where = "places[" + std::to_string(i) + "]";
        if (!p.isObject()) return bad(where + ": an object is needed");
        for (std::uint32_t m = 0; m < p.size(); ++m) {
            const std::string_view k = p.key(m);
            if (k != "id" && k != "kind" && k != "x" && k != "y" && k != "radius")
                return bad(where + ": unknown member \"" + std::string(k) + "\"");
        }
        OuterPlace place;
        const JsonValue id = p["id"];
        if (!id.isInteger() || id.integer() < 0 || id.integer() > 65535) return bad(where + ".id: a whole number 0..65535 is needed");
        place.id = static_cast<std::uint16_t>(id.integer());
        if (seen[place.id]) return bad(where + ".id: " + std::to_string(place.id) + " twice");
        seen[place.id] = true;
        const JsonValue kind = p["kind"];
        if (!kind.isString()) return bad(where + ".kind: a string is needed");
        const std::string_view k = kind.string();
        if (k == "district") place.kind = OuterPlace::Kind::District;
        else if (k == "rest_point") place.kind = OuterPlace::Kind::RestPoint;
        else if (k == "place") place.kind = OuterPlace::Kind::Place;
        else return bad(where + ".kind: district, rest_point or place");
        if (!readFloat(p, "x", place.x) || !readFloat(p, "y", place.y) || !readFloat(p, "radius", place.radius))
            return bad(where + ": x, y and radius must be finite numbers");
        if (place.radius < 0.0f) return bad(where + ".radius: must be >= 0");
        out.push_back(place);
    }
    return true;
}

int outerDistrictDiscoveryIndex(const std::vector<OuterPlace>& places, std::size_t index) {
    if (index >= places.size() || places[index].kind != OuterPlace::Kind::District) return -1;
    int before = 0;
    for (std::size_t i = 0; i < index; ++i)
        if (places[i].kind == OuterPlace::Kind::District) ++before;
    return kOuterDistrictFirstIndex + before;
}

} // namespace ftcore
