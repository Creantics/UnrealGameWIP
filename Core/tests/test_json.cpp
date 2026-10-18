#include "ftcore/Json.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <string_view>

using namespace ftcore;

// Outside the macro: MSVC mangles a raw string with escapes when it is a macro argument.
constexpr const char* kAllTypes = R"J( { "n": null, "t": true, "f": false, "i": -42, "big": 9007199254740993,
  "d": 1.5e2, "z": 0, "s": "a\"b\\c\/\n\u00e9\ud83d\ude00", "raw": "café",
  "a": [1, [2, 3], {}], "o": {"k": "v", "k": "second"} } )J";

TEST_CASE("json reads every value type") {
    JsonDocument doc;
    std::string err;
    REQUIRE_MESSAGE(doc.parse(kAllTypes, &err), err);
    const JsonValue r = doc.root();
    REQUIRE(r.isObject());
    CHECK(r.size() == 11);
    CHECK(r["n"].isNull());
    CHECK(r["t"].boolean());
    CHECK_FALSE(r["f"].boolean(true));
    CHECK(r["i"].isInteger());
    CHECK(r["i"].integer() == -42);
    CHECK(r["i"].number() == -42.0);
    CHECK(r["big"].integer() == 9007199254740993);
    CHECK(r["d"].isNumber());
    CHECK_FALSE(r["d"].isInteger());
    CHECK(r["d"].number() == 150.0);
    CHECK(r["d"].integer(7) == 7);
    CHECK(r["d"].raw() == "1.5e2");
    CHECK(r["z"].integer(5) == 0);
    CHECK(r["s"].string() == "a\"b\\c/\n\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(r["raw"].string() == "caf\xC3\xA9");
    const JsonValue a = r["a"];
    REQUIRE(a.isArray());
    CHECK(a.size() == 3);
    CHECK(a[1][1].integer() == 3);
    CHECK(a[2].isObject());
    CHECK(a[2].size() == 0);
    CHECK(a.raw() == "[1, [2, 3], {}]");
    CHECK(r["o"]["k"].string() == "v"); // the first of duplicate keys
    CHECK(r["o"].key(1) == "k");
    CHECK(r["o"].value(1).string() == "second");
    CHECK(r.key(0) == "n");
}

TEST_CASE("json lookups on missing or mistyped values read as absent") {
    JsonDocument doc;
    REQUIRE(doc.parse(R"({"a": [1], "s": "x"})"));
    const JsonValue r = doc.root();
    CHECK(r["missing"].isAbsent());
    CHECK(r["missing"]["deeper"][3].isAbsent());
    CHECK(r["a"][1].isAbsent());
    CHECK(r["a"][-1].isAbsent());
    CHECK(r["a"]["key"].isAbsent());
    CHECK(r["s"][0].isAbsent());
    CHECK(r["s"].integer(9) == 9);
    CHECK(r["s"].boolean(true));
    CHECK(r["a"].string("fallback") == "fallback");
    CHECK(r["missing"].size() == 0);
    CHECK(r["missing"].raw().empty());
    CHECK_FALSE(r.has("missing"));
    CHECK(r.has("s"));
    JsonDocument empty;
    CHECK(empty.root().isAbsent());
}

TEST_CASE("json rejects bad text with a position") {
    const char* bad[] = {
        "",            "{",          "[1,]",       "{\"a\":1,}", "{'a':1}",  "[01]",        "[1.]",
        "[.5]",        "[1e]",       "[-]",        "tru",        "nul",      "\"abc",       "\"a\x01\"",
        "\"\\x\"",     "\"\\u12\"",  "\"\\ud800\"", "\"\\udc00\"", "[1] [2]",  "{\"a\" 1}",   "[1 2]",
        "// c\n1",     "[1e999]",
    };
    for (const char* text : bad) {
        CAPTURE(text);
        JsonDocument doc;
        std::string err;
        CHECK_FALSE(doc.parse(text, &err));
        CHECK(err.rfind("line ", 0) == 0);
        CHECK(doc.empty());
    }
    JsonDocument doc;
    std::string err;
    CHECK_FALSE(doc.parse("{\n  \"a\": [1,\n  ?]\n}", &err));
    CHECK(err == "line 3, column 3: expected a value");
}

TEST_CASE("json caps nesting depth") {
    std::string deep(JsonDocument::kMaxDepth + 5, '[');
    deep.append(JsonDocument::kMaxDepth + 5, ']');
    JsonDocument doc;
    std::string err;
    CHECK_FALSE(doc.parse(deep, &err));
    CHECK(err.find("nesting too deep") != std::string::npos);

    std::string ok(50, '[');
    ok.append(50, ']');
    CHECK(doc.parse(ok));
}

TEST_CASE("json document keeps no reference to the source text") {
    JsonDocument doc;
    {
        std::string text = R"({"k": "value", "n": 12})";
        REQUIRE(doc.parse(text));
        text.assign(text.size(), 'x');
    }
    CHECK(doc.root()["k"].string() == "value");
    CHECK(doc.root()["n"].raw() == "12");
    REQUIRE(doc.parse("[true]"));
    CHECK(doc.root()[0].boolean());
    doc.clear();
    CHECK(doc.root().isAbsent());
}
