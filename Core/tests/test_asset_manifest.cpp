#include "ftcore/AssetManifest.h"

#include "RealStory.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

TEST_CASE("the model manifest reads ids, kinds, roles, packs and default files") {
    const char* json = R"({ "version": 1, "models": [
        { "id": "MDL-001", "kind": "character", "role": "lead", "pack": "Solhaven" },
        { "id": "MDL-002", "kind": "character" },
        { "id": "MDL-003", "kind": "prop", "pack": "Lattice", "file": "kits/k3.glb" } ] })";
    ModelTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importModelManifest(json, t, &error, &warnings), error);
    CHECK(warnings.empty());
    REQUIRE(t.models.size() == 3);

    const ModelEntry* lead = t.find("MDL-001");
    REQUIRE(lead);
    CHECK(lead->kind == ModelKind::Character);
    CHECK(lead->role == AssetRole::Lead);
    CHECK(lead->pack == AssetPack::Solhaven);
    CHECK(lead->file == "MDL-001.glb");
    CHECK(modelRules(*lead).maxTextureSize == 2048);
    CHECK(modelRules(*lead).requireSkin);

    const ModelEntry* support = t.find("MDL-002");
    REQUIRE(support);
    CHECK(support->role == AssetRole::Supporting);
    CHECK(support->pack == AssetPack::None);
    CHECK(modelRules(*support).maxTextureSize == 1024);

    const ModelEntry* prop = t.find("MDL-003");
    REQUIRE(prop);
    CHECK(prop->kind == ModelKind::Prop);
    CHECK(prop->file == "kits/k3.glb");
    CHECK_FALSE(modelRules(*prop).requireSkin);
    CHECK(modelRules(*prop).emissiveRule);
    CHECK(t.find("MDL-999") == nullptr);
}

TEST_CASE("the model manifest reports every bad entry and keeps nothing") {
    const char* json = R"({ "version": 1, "models": [
        { "id": "MDL-001", "kind": "character", "role": "hero" },
        { "id": "MDL-001", "kind": "prop" },
        { "kind": "prop" },
        { "id": "MDL-004", "kind": "tree" },
        { "id": "MDL-005", "kind": "prop", "role": "lead" },
        { "id": "MDL-006", "kind": "prop", "pack": "Elsewhere" },
        { "id": "MDL-007", "kind": "prop", "name": "x" } ] })";
    ModelTable t;
    std::string error;
    std::vector<std::string> warnings;
    CHECK_FALSE(importModelManifest(json, t, &error, &warnings));
    CHECK(t.models.empty());
    CHECK(error.find("models[0].role") != std::string::npos);
    CHECK(error.find("duplicate model MDL-001") != std::string::npos);
    CHECK(error.find("models[2].id") != std::string::npos);
    CHECK(error.find("models[3].kind") != std::string::npos);
    CHECK(error.find("models[4].role: only characters") != std::string::npos);
    CHECK(error.find("unknown pack Elsewhere") != std::string::npos);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "models[6]: unknown key: name");

    CHECK_FALSE(importModelManifest("{ \"version\": 2, \"models\": [] }", t, &error));
    CHECK(error.find("version") != std::string::npos);
    CHECK_FALSE(importModelManifest("[", t, &error));
}

TEST_CASE("voice line ids come from the story's lines, in order, each once") {
    StoryBuilder b;
    b.setStart("SC-A");
    b.beginChapter("01", "One");
    b.beginScene("SC-A", "A");
    b.line("L-1", "SPK-X", "One.");
    b.direction("Two.");
    b.line("", "SPK-Y", "No id.");
    b.line("L-2", "SPK-Y", "Three.");
    b.end();
    b.beginScene("SC-B", "B");
    b.line("L-1", "SPK-X", "One again.");
    b.line("L-3", "SPK-X", "Four.");
    b.end();
    StoryDatabase db;
    std::string error;
    REQUIRE_MESSAGE(b.finish(db, &error), error);
    CHECK(voiceLineIds(db) == std::vector<std::string>{"L-1", "L-2", "L-3"});
}

TEST_CASE("asset states and the tally") {
    CHECK(assetState(false, 0, false) == AssetState::Missing);
    CHECK(assetState(false, 3, true) == AssetState::Missing);
    CHECK(assetState(true, 1, true) == AssetState::Failed);
    CHECK(assetState(true, 0, false) == AssetState::Delivered);
    CHECK(assetState(true, 0, true) == AssetState::Imported);
    CHECK(std::string(assetStateName(AssetState::Delivered)) == "delivered");

    AssetTally tally;
    tally.add(AssetState::Missing);
    tally.add(AssetState::Missing);
    tally.add(AssetState::Imported);
    tally.add(AssetState::Failed);
    CHECK(tally.total() == 4);
    CHECK(tally.summary() == "4 expected: 1 imported, 0 delivered, 2 missing, 1 failed");
}

TEST_CASE("the real model manifest loads, and the real story has a voice line id per line") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Assets/models.json", text));
    ModelTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importModelManifest(text, t, &error, &warnings), error);
    CHECK(warnings.empty());
    CHECK(t.models.size() >= 80);

    const ftcore_test::RealStory& story = ftcore_test::realStory();
    if (!story.present) return;
    REQUIRE_MESSAGE(story.ok, story.error);
    const std::vector<std::string> ids = voiceLineIds(story.db);
    CHECK(ids.size() >= 1000);
    for (const std::string& id : ids) CHECK(id.rfind("VO-", 0) == 0);
}
