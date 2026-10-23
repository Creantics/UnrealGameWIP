#include "ftcore/Glb.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>

using namespace ftcore;

namespace {

void put32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

void putBe32(std::string& s, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

void pad4(std::string& s, char fill) {
    while (s.size() % 4) s += fill;
}

// A PNG signature plus an IHDR chunk header: enough for the size read.
std::string pngHeader(std::uint32_t w, std::uint32_t h) {
    std::string s("\x89PNG\r\n\x1a\n", 8);
    putBe32(s, 13);
    s += "IHDR";
    putBe32(s, w);
    putBe32(s, h);
    s += std::string("\x08\x06\x00\x00\x00", 5);
    return s;
}

// SOI, an APP0 segment, then SOF0 with the size.
std::string jpegHeader(std::uint32_t w, std::uint32_t h) {
    std::string s("\xFF\xD8\xFF\xE0\x00\x04\x00\x00", 8);
    s += std::string("\xFF\xC0\x00\x11\x08", 5);
    s += static_cast<char>(h >> 8);
    s += static_cast<char>(h & 0xFF);
    s += static_cast<char>(w >> 8);
    s += static_cast<char>(w & 0xFF);
    s += std::string(12, '\0');
    return s;
}

std::string glb(std::string json, std::string bin = {}) {
    pad4(json, ' ');
    pad4(bin, '\0');
    std::string out;
    put32(out, 0x46546C67u);
    put32(out, 2);
    put32(out, static_cast<std::uint32_t>(12 + 8 + json.size() + (bin.empty() ? 0 : 8 + bin.size())));
    put32(out, static_cast<std::uint32_t>(json.size()));
    put32(out, 0x4E4F534Au);
    out += json;
    if (!bin.empty()) {
        put32(out, static_cast<std::uint32_t>(bin.size()));
        put32(out, 0x004E4942u);
        out += bin;
    }
    return out;
}

// One skinned mesh, one material with a base colour texture, two embedded images.
constexpr const char* kCharacter = R"J({
  "asset": {"version": "2.0"},
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "targets": [{}, {}, {}]}]}],
  "skins": [{"joints": [0]}],
  "materials": [
    {"name": "Body", "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.25, 1, 1], "metallicFactor": 0, "roughnessFactor": 0.6,
      "baseColorTexture": {"index": 0}}, "normalTexture": {"index": 1}},
    {"name": "M_EyeGlow", "emissiveFactor": [0, 0.5, 1], "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4}}}
  ],
  "textures": [{"source": 0}, {"source": 1}],
  "images": [
    {"name": "body_base", "bufferView": 0, "mimeType": "image/png"},
    {"name": "body_normal", "bufferView": 1, "mimeType": "image/jpeg"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 29}, {"buffer": 0, "byteOffset": 32, "byteLength": 29}]
})J";

std::string characterBin(std::uint32_t pngSize, std::uint32_t jpegW, std::uint32_t jpegH) {
    std::string bin = pngHeader(pngSize, pngSize); // 29 bytes
    pad4(bin, '\0');                               // 32
    bin += jpegHeader(jpegW, jpegH);               // 29 bytes
    return bin;
}

bool hasCode(const std::vector<AssetIssue>& issues, std::string_view code) {
    for (const AssetIssue& i : issues)
        if (i.code == code) return true;
    return false;
}

} // namespace

TEST_CASE("glb reads materials, images and mesh facts") {
    GlbInfo info;
    std::string err;
    REQUIRE_MESSAGE(readGlb(glb(kCharacter, characterBin(2048, 1024, 1024)), info, &err), err);
    CHECK(info.meshes == 1);
    CHECK(info.primitives == 1);
    CHECK(info.skins == 1);
    CHECK(info.uvSets == 1);
    CHECK_FALSE(info.vertexColor);
    CHECK(info.morphTargets == 3);

    REQUIRE(info.materials.size() == 2);
    const GlbMaterial& body = info.materials[0];
    CHECK(body.name == "Body");
    CHECK(body.baseColor[1] == doctest::Approx(0.25));
    CHECK(body.metallic == doctest::Approx(0.0));
    CHECK(body.roughness == doctest::Approx(0.6));
    CHECK(body.texture[static_cast<int>(TextureSlot::BaseColor)] == 0);
    CHECK(body.texture[static_cast<int>(TextureSlot::Normal)] == 1);
    CHECK_FALSE(body.hasTexture(TextureSlot::Emissive));
    CHECK(body.emissiveLevel() == doctest::Approx(0.0));
    CHECK(info.materials[1].emissiveLevel() == doctest::Approx(4.0));

    REQUIRE(info.images.size() == 2);
    CHECK(info.images[0].name == "body_base");
    CHECK(info.images[0].embedded);
    CHECK(info.images[0].format == ImageFormat::Png);
    CHECK(info.images[0].width == 2048);
    CHECK(info.images[0].height == 2048);
    CHECK(info.images[1].format == ImageFormat::Jpeg);
    CHECK(info.images[1].width == 1024);
    CHECK(info.images[1].height == 1024);
}

TEST_CASE("glb rejects malformed files") {
    GlbInfo info;
    std::string err;
    CHECK_FALSE(readGlb("short", info, &err));
    CHECK_FALSE(err.empty());

    std::string bad = glb("{}");
    bad[0] = 'x';
    CHECK_FALSE(readGlb(bad, info, &err));

    std::string v1 = glb("{}");
    v1[4] = 1;
    CHECK_FALSE(readGlb(v1, info, &err));

    std::string cut = glb("{}");
    cut.resize(cut.size() - 2);
    CHECK_FALSE(readGlb(cut, info, &err));

    CHECK_FALSE(readGlb(glb("{bad json"), info, &err));
    CHECK(err.find("JSON") != std::string::npos);
}

TEST_CASE("glb external images, uv sets and vertex colour are counted") {
    constexpr const char* json = R"J({"meshes": [{"primitives": [{"attributes": {"TEXCOORD_0": 0, "TEXCOORD_1": 1, "COLOR_0": 2}}]}],
      "images": [{"uri": "outside.png"}]})J";
    GlbInfo info;
    REQUIRE(readGlb(glb(json), info));
    CHECK(info.uvSets == 2);
    CHECK(info.vertexColor);
    CHECK(info.skins == 0);
    REQUIRE(info.images.size() == 1);
    CHECK_FALSE(info.images[0].embedded);

    const auto issues = checkGlb(info, characterRules(AssetPack::Solhaven, AssetRole::Supporting));
    CHECK(hasCode(issues, "uv-sets"));
    CHECK(hasCode(issues, "vertex-color"));
    CHECK(hasCode(issues, "no-skin"));
    CHECK(hasCode(issues, "image-external"));
    CHECK(countErrors(issues) == 4);
}

TEST_CASE("asset rules by pack and role") {
    CHECK(assetPackFromName("LATTICE") == AssetPack::Lattice);
    CHECK(assetPackFromName("ink-solhaven") == AssetPack::InkSolhaven);
    CHECK(assetPackFromName("Ink Solhaven") == AssetPack::InkSolhaven);
    CHECK(assetPackFromName("nowhere") == AssetPack::None);
    CHECK(assetRoleFromName("Lead") == AssetRole::Lead);
    CHECK(assetRoleFromName("prop") == AssetRole::Prop);
    CHECK(assetRoleFromName("") == AssetRole::Supporting);
    CHECK(assetRoleFromName("Outfit") == AssetRole::Outfit);

    const AssetRules lead = characterRules(AssetPack::Lattice, AssetRole::Lead);
    CHECK(lead.maxTextureSize == 2048);
    CHECK(lead.emissiveRule);
    CHECK(lead.requireSkin);
    const AssetRules other = characterRules(AssetPack::Solhaven, AssetRole::Supporting);
    CHECK(other.maxTextureSize == 1024);
    CHECK_FALSE(other.emissiveRule);
    const AssetRules outfit = characterRules(AssetPack::Solhaven, AssetRole::Outfit);
    CHECK(outfit.maxTextureSize == 2048);
    CHECK_FALSE(outfit.requireSkin);
    const AssetRules prop = propRules(AssetPack::Lattice);
    CHECK_FALSE(prop.requireSkin);
    CHECK(prop.emissiveRule);

    CHECK(isGlowMaterialName("M_EyeGlow"));
    CHECK(isGlowMaterialName("skill_trim"));
    CHECK(isGlowMaterialName("RULE_lines"));
    CHECK_FALSE(isGlowMaterialName("Body"));
}

TEST_CASE("a clean lead passes, and texture size follows the role") {
    GlbInfo info;
    REQUIRE(readGlb(glb(kCharacter, characterBin(2048, 1024, 1024)), info));
    CHECK(checkGlb(info, characterRules(AssetPack::Lattice, AssetRole::Lead)).empty());

    const auto supporting = checkGlb(info, characterRules(AssetPack::Lattice, AssetRole::Supporting));
    CHECK(hasCode(supporting, "image-too-big"));
    CHECK(countErrors(supporting) == 1);
    for (const AssetIssue& i : supporting)
        if (i.code == "image-too-big") CHECK(i.message.find("image 0 'body_base'") != std::string::npos);
}

TEST_CASE("non-square images warn") {
    GlbInfo info;
    REQUIRE(readGlb(glb(kCharacter, characterBin(1024, 1024, 512)), info));
    const auto issues = checkGlb(info, characterRules(AssetPack::Solhaven, AssetRole::Lead));
    CHECK(hasCode(issues, "image-not-square-pot"));
    CHECK(countErrors(issues) == 0);
}

TEST_CASE("the Lattice emissive split") {
    GlbInfo info;
    REQUIRE(readGlb(glb(kCharacter, characterBin(1024, 1024, 1024)), info));
    const AssetRules lattice = characterRules(AssetPack::Lattice, AssetRole::Lead);
    CHECK(checkGlb(info, lattice).empty());

    info.materials[1].emissiveStrength = 2.0f; // glow at 2.0
    auto issues = checkGlb(info, lattice);
    CHECK(hasCode(issues, "emissive-glow-low"));
    CHECK(issues[0].message.find("material 1 'M_EyeGlow'") != std::string::npos);
    CHECK(issues[0].message.find("2.00") != std::string::npos);

    // Outside the Lattice there is no emissive rule.
    CHECK(checkGlb(info, characterRules(AssetPack::Solhaven, AssetRole::Lead)).empty());

    info.materials[1].emissiveStrength = 4.0f;
    info.materials[0].emissiveFactor[0] = 1.0f;
    info.materials[0].emissiveStrength = 2.5f; // a plain material at the cap
    issues = checkGlb(info, lattice);
    CHECK(hasCode(issues, "emissive-plain-high"));
    info.materials[0].emissiveStrength = 2.4f;
    CHECK(checkGlb(info, lattice).empty());
}
