#include "ftcore/UiText.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

UiTextTable load(std::string_view json) {
    UiTextTable t;
    std::string error;
    REQUIRE_MESSAGE(importUiText(json, t, &error), error);
    return t;
}

const char* const kEn = R"({"version":1,"culture":"en","strings":{
    "b.two":"Two","a.one":"One","c.try":"Try {0} of {1}"}})";

} // namespace

TEST_CASE("ui text: import sorts keys and finds them") {
    const UiTextTable en = load(kEn);
    CHECK(en.culture == "en");
    REQUIRE(en.entries.size() == 3);
    CHECK(en.entries[0].key == "a.one");
    REQUIRE(en.find("b.two"));
    CHECK(*en.find("b.two") == "Two");
    CHECK(en.find("b.three") == nullptr);
    CHECK(en.find("") == nullptr);
}

TEST_CASE("ui text: import rejects bad files") {
    UiTextTable t;
    std::string error;
    CHECK_FALSE(importUiText("{", t, &error));
    CHECK_FALSE(importUiText(R"({"version":2,"culture":"en","strings":{}})", t, &error));
    CHECK_FALSE(importUiText(R"({"version":1,"strings":{}})", t, &error));
    CHECK_FALSE(importUiText(R"({"version":1,"culture":"en","strings":{"Bad Key":"x"}})", t, &error));
    CHECK_FALSE(importUiText(R"({"version":1,"culture":"en","strings":{"a":3}})", t, &error));
    CHECK_FALSE(importUiText(R"({"version":1,"culture":"en","strings":{"a":"x","a":"y"}})", t, &error));
    CHECK(error.find("duplicate") != std::string::npos);

    std::vector<std::string> warnings;
    CHECK(importUiText(R"({"version":1,"culture":"en","strings":{},"extra":1})", t, &error, &warnings));
    CHECK(warnings.size() == 1);
}

TEST_CASE("ui text: lookup falls back to English, then the key") {
    const UiTextTable en = load(kEn);
    const UiTextTable fr = load(R"({"version":1,"culture":"fr","strings":{"a.one":"Un"}})");
    CHECK(lookupUiText(&fr, en, "a.one") == "Un");
    CHECK(lookupUiText(&fr, en, "b.two") == "Two");
    CHECK(lookupUiText(nullptr, en, "b.two") == "Two");
    CHECK(lookupUiText(&fr, en, "z.missing") == "z.missing");
}

TEST_CASE("ui text: lookups don't allocate") {
    const UiTextTable en = load(kEn);
    REQUIRE(ftcore_test::allocHookInstalled());
    const auto before = ftcore_test::allocStats().count;
    std::size_t total = 0;
    for (int i = 0; i < 100; ++i) total += lookupUiText(nullptr, en, "c.try").size();
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(total == 1400);
}

TEST_CASE("ui text: culture chain") {
    CHECK(uiCultureChain("pt-BR") == std::vector<std::string>{"pt-BR", "pt", "en"});
    CHECK(uiCultureChain("pt_BR") == std::vector<std::string>{"pt-BR", "pt", "en"});
    CHECK(uiCultureChain("fr") == std::vector<std::string>{"fr", "en"});
    CHECK(uiCultureChain("en-GB") == std::vector<std::string>{"en-GB", "en"});
    CHECK(uiCultureChain("en") == std::vector<std::string>{"en"});
    CHECK(uiCultureChain("") == std::vector<std::string>{"en"});
}

TEST_CASE("ui text: translation check") {
    const UiTextTable en = load(kEn);
    const UiTextTable good = load(R"({"version":1,"culture":"de","strings":{
        "a.one":"Eins","b.two":"Zwei","c.try":"Versuch {0} von {1}"}})");
    CHECK(checkUiTranslation(en, good).empty());
    const UiTextTable bad = load(R"({"version":1,"culture":"de","strings":{
        "a.one":"Eins","c.try":"Versuch {0}","d.extra":"X"}})");
    const auto issues = checkUiTranslation(en, bad);
    CHECK(issues.size() == 3); // b.two missing, c.try placeholders, d.extra unknown
}

TEST_CASE("ui text: pseudo-localisation keeps keys and placeholders") {
    const UiTextTable en = load(kEn);
    const UiTextTable p = pseudoLocalise(en);
    CHECK(checkUiTranslation(en, p).empty());
    const std::string& t = *p.find("c.try");
    CHECK(t.front() == '[');
    CHECK(t.back() == ']');
    CHECK(t.find("{0}") != std::string::npos);
    CHECK(t.find("{1}") != std::string::npos);
    CHECK(t.find("Try") == std::string::npos); // letters were accented
    CHECK(t.size() > en.find("c.try")->size());
}

TEST_CASE("ui text: the project's English table loads and holds no story") {
    std::string json;
    REQUIRE(ftcore_test::readProjectData("Loc/en/ui.json", json));
    UiTextTable en;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importUiText(json, en, &error, &warnings), error);
    CHECK(warnings.empty());
    CHECK(en.culture == "en");
    CHECK(en.entries.size() >= 40);
    for (const UiTextEntry& e : en.entries) CHECK_MESSAGE(e.text.size() < 160, e.key);
    CHECK(checkUiTranslation(en, pseudoLocalise(en)).empty());
}
