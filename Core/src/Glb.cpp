#include "ftcore/Glb.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cstddef>

namespace ftcore {

namespace {

constexpr std::uint32_t kMagic = 0x46546C67u; // "glTF"
constexpr std::uint32_t kChunkJson = 0x4E4F534Au;
constexpr std::uint32_t kChunkBin = 0x004E4942u;

std::uint32_t le32(std::string_view b, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24;
}

std::uint32_t be32(std::string_view b, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) << 24 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3]));
}

std::uint32_t be16(std::string_view b, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1]));
}

bool fail(std::string* error, const char* what) {
    if (error) *error = what;
    return false;
}

// Reads the pixel size from a PNG or JPEG header.
void readImageHeader(std::string_view data, GlbImage& img) {
    static constexpr unsigned char kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (data.size() >= 24 && std::equal(kPng, kPng + 8, reinterpret_cast<const unsigned char*>(data.data())) &&
        data.substr(12, 4) == "IHDR") {
        img.format = ImageFormat::Png;
        img.width = be32(data, 16);
        img.height = be32(data, 20);
        return;
    }
    if (data.size() >= 4 && static_cast<unsigned char>(data[0]) == 0xFF && static_cast<unsigned char>(data[1]) == 0xD8) {
        img.format = ImageFormat::Jpeg;
        std::size_t i = 2;
        while (i + 9 < data.size()) {
            if (static_cast<unsigned char>(data[i]) != 0xFF) return;
            const unsigned char marker = static_cast<unsigned char>(data[i + 1]);
            if (marker == 0xFF) { ++i; continue; }
            const std::uint32_t length = be16(data, i + 2);
            const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (sof) {
                img.height = be16(data, i + 5);
                img.width = be16(data, i + 7);
                return;
            }
            if (marker == 0xD9 || marker == 0xDA) return;
            i += 2 + length;
        }
    }
}

float num(const JsonValue& v, double fallback) { return static_cast<float>(v.number(fallback)); }

int imageOf(const JsonValue& root, const JsonValue& textureInfo) {
    if (!textureInfo.isObject()) return -1;
    const JsonValue tex = root["textures"][static_cast<int>(textureInfo["index"].integer(-1))];
    if (!tex.isObject()) return -1;
    return static_cast<int>(tex["source"].integer(-1));
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool containsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == needle[j]) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

bool isPowerOfTwo(std::uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

std::string label(const char* kind, std::size_t index, const std::string& name) {
    std::string s = kind;
    s += ' ';
    s += std::to_string(index);
    if (!name.empty()) {
        s += " '";
        s += name;
        s += '\'';
    }
    return s;
}

std::string fixed2(float v) {
    const long scaled = static_cast<long>(v * 100.f + (v >= 0.f ? 0.5f : -0.5f));
    std::string s = std::to_string(scaled / 100);
    s += '.';
    const long frac = (scaled < 0 ? -scaled : scaled) % 100;
    if (frac < 10) s += '0';
    s += std::to_string(frac);
    return s;
}

void add(std::vector<AssetIssue>& out, AssetSeverity sev, const char* code, std::string message) {
    AssetIssue i;
    i.severity = sev;
    i.code = code;
    i.message = std::move(message);
    out.push_back(std::move(i));
}

} // namespace

float GlbMaterial::emissiveLevel() const {
    return std::max({emissiveFactor[0], emissiveFactor[1], emissiveFactor[2]}) * emissiveStrength;
}

bool readGlb(std::string_view bytes, GlbInfo& out, std::string* error) {
    out = GlbInfo{};
    if (bytes.size() < 20) return fail(error, "file is too short for a glb");
    if (le32(bytes, 0) != kMagic) return fail(error, "not a glb (bad magic)");
    if (le32(bytes, 4) != 2) return fail(error, "glb version is not 2");
    const std::uint32_t total = le32(bytes, 8);
    if (total > bytes.size()) return fail(error, "glb length is past the end of the file");

    std::string_view json, bin;
    std::size_t at = 12;
    while (at + 8 <= total) {
        const std::uint32_t length = le32(bytes, at);
        const std::uint32_t type = le32(bytes, at + 4);
        if (at + 8 + length > total) return fail(error, "glb chunk runs past the end of the file");
        const std::string_view chunk = bytes.substr(at + 8, length);
        if (type == kChunkJson && json.empty()) json = chunk;
        else if (type == kChunkBin && bin.empty()) bin = chunk;
        at += 8 + length;
    }
    if (json.empty()) return fail(error, "glb has no JSON chunk");

    JsonDocument doc;
    std::string jsonError;
    if (!doc.parse(json, &jsonError)) {
        if (error) *error = "glb JSON: " + jsonError;
        return false;
    }
    const JsonValue root = doc.root();

    const JsonValue images = root["images"];
    for (std::uint32_t i = 0; i < images.size(); ++i) {
        const JsonValue im = images[i];
        GlbImage img;
        img.name = std::string(im["name"].string());
        const JsonValue view = root["bufferViews"][static_cast<int>(im["bufferView"].integer(-1))];
        if (view.isObject() && view["buffer"].integer(0) == 0) {
            img.embedded = true;
            const std::uint64_t offset = static_cast<std::uint64_t>(view["byteOffset"].integer(0));
            const std::uint64_t length = static_cast<std::uint64_t>(view["byteLength"].integer(0));
            if (offset + length <= bin.size()) readImageHeader(bin.substr(offset, length), img);
        }
        if (img.format == ImageFormat::Unknown) {
            const std::string_view mime = im["mimeType"].string();
            if (mime == "image/png") img.format = ImageFormat::Png;
            else if (mime == "image/jpeg") img.format = ImageFormat::Jpeg;
        }
        out.images.push_back(std::move(img));
    }

    const JsonValue materials = root["materials"];
    for (std::uint32_t i = 0; i < materials.size(); ++i) {
        const JsonValue m = materials[i];
        GlbMaterial mat;
        mat.name = std::string(m["name"].string());
        const JsonValue pbr = m["pbrMetallicRoughness"];
        for (int c = 0; c < 4; ++c) mat.baseColor[c] = num(pbr["baseColorFactor"][c], 1.0);
        mat.metallic = num(pbr["metallicFactor"], 1.0);
        mat.roughness = num(pbr["roughnessFactor"], 1.0);
        for (int c = 0; c < 3; ++c) mat.emissiveFactor[c] = num(m["emissiveFactor"][c], 0.0);
        mat.emissiveStrength = num(m["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"], 1.0);
        mat.texture[static_cast<int>(TextureSlot::BaseColor)] = imageOf(root, pbr["baseColorTexture"]);
        mat.texture[static_cast<int>(TextureSlot::MetallicRoughness)] = imageOf(root, pbr["metallicRoughnessTexture"]);
        mat.texture[static_cast<int>(TextureSlot::Normal)] = imageOf(root, m["normalTexture"]);
        mat.texture[static_cast<int>(TextureSlot::Occlusion)] = imageOf(root, m["occlusionTexture"]);
        mat.texture[static_cast<int>(TextureSlot::Emissive)] = imageOf(root, m["emissiveTexture"]);
        out.materials.push_back(std::move(mat));
    }

    const JsonValue meshes = root["meshes"];
    out.meshes = meshes.size();
    for (std::uint32_t i = 0; i < meshes.size(); ++i) {
        const JsonValue prims = meshes[i]["primitives"];
        out.primitives += prims.size();
        for (std::uint32_t p = 0; p < prims.size(); ++p) {
            const JsonValue attrs = prims[p]["attributes"];
            std::uint32_t uv = 0;
            while (attrs.has("TEXCOORD_" + std::to_string(uv))) ++uv;
            out.uvSets = std::max(out.uvSets, uv);
            if (attrs.has("COLOR_0")) out.vertexColor = true;
            out.morphTargets = std::max(out.morphTargets, prims[p]["targets"].size());
        }
    }
    out.skins = root["skins"].size();
    return true;
}

AssetPack assetPackFromName(std::string_view name) {
    if (equalsNoCase(name, "solhaven")) return AssetPack::Solhaven;
    if (equalsNoCase(name, "lattice")) return AssetPack::Lattice;
    if (equalsNoCase(name, "unwritten")) return AssetPack::Unwritten;
    if (equalsNoCase(name, "restored")) return AssetPack::Restored;
    if (equalsNoCase(name, "inksolhaven") || equalsNoCase(name, "ink solhaven") || equalsNoCase(name, "ink-solhaven"))
        return AssetPack::InkSolhaven;
    return AssetPack::None;
}

AssetRole assetRoleFromName(std::string_view name) {
    if (equalsNoCase(name, "lead")) return AssetRole::Lead;
    if (equalsNoCase(name, "prop")) return AssetRole::Prop;
    return AssetRole::Supporting;
}

AssetRules characterRules(AssetPack pack, AssetRole role) {
    AssetRules r;
    r.pack = pack;
    r.maxTextureSize = role == AssetRole::Lead ? 2048 : 1024;
    r.emissiveRule = pack == AssetPack::Lattice;
    r.requireSkin = role != AssetRole::Prop;
    return r;
}

AssetRules propRules(AssetPack pack) {
    AssetRules r = characterRules(pack, AssetRole::Prop);
    r.requireSkin = false;
    return r;
}

bool isGlowMaterialName(std::string_view name) {
    return containsNoCase(name, "glow") || containsNoCase(name, "eye") || containsNoCase(name, "skill") ||
           containsNoCase(name, "rule");
}

std::vector<AssetIssue> checkGlb(const GlbInfo& info, const AssetRules& rules) {
    std::vector<AssetIssue> out;
    if (info.meshes == 0) add(out, AssetSeverity::Error, "no-mesh", "the file has no mesh");
    if (rules.requireSkin && info.skins == 0) add(out, AssetSeverity::Error, "no-skin", "a character needs a skin (armature)");
    if (info.uvSets > 1)
        add(out, AssetSeverity::Error, "uv-sets", "uses " + std::to_string(info.uvSets) + " UV sets; only TEXCOORD_0 is read");
    if (info.vertexColor)
        add(out, AssetSeverity::Error, "vertex-color", "has vertex colour (COLOR_0); pockets and load maps go in textures");

    for (std::size_t i = 0; i < info.materials.size(); ++i) {
        const GlbMaterial& m = info.materials[i];
        const float level = m.emissiveLevel();
        const bool emits = level > 0.f || m.hasTexture(TextureSlot::Emissive);
        if (!rules.emissiveRule || !emits) continue;
        if (isGlowMaterialName(m.name)) {
            if (level < AssetRules::kGlowMin)
                add(out, AssetSeverity::Error, "emissive-glow-low",
                    label("material", i, m.name) + " is a glow at " + fixed2(level) + "; skills, eyes and rules need 3.00 or more");
        } else if (level >= AssetRules::kPlainMax) {
            add(out, AssetSeverity::Error, "emissive-plain-high",
                label("material", i, m.name) + " glows at " + fixed2(level) +
                    "; only glow materials (Glow, Eye, Skill or Rule in the name) may reach 2.50");
        }
    }

    for (std::size_t i = 0; i < info.images.size(); ++i) {
        const GlbImage& img = info.images[i];
        const std::string who = label("image", i, img.name);
        if (!img.embedded) {
            add(out, AssetSeverity::Error, "image-external", who + " is not embedded in the glb");
            continue;
        }
        if (img.format == ImageFormat::Unknown) {
            add(out, AssetSeverity::Error, "image-format", who + " is not PNG or JPEG");
            continue;
        }
        if (img.width == 0 || img.height == 0) {
            add(out, AssetSeverity::Warning, "image-size-unknown", who + ": couldn't read its size");
            continue;
        }
        const std::string size = std::to_string(img.width) + "x" + std::to_string(img.height);
        if (img.width != img.height || !isPowerOfTwo(img.width))
            add(out, AssetSeverity::Warning, "image-not-square-pot", who + " is " + size + "; use a square power of two");
        if (img.width > rules.maxTextureSize || img.height > rules.maxTextureSize)
            add(out, AssetSeverity::Error, "image-too-big",
                who + " is " + size + "; the limit for this role is " + std::to_string(rules.maxTextureSize));
    }
    return out;
}

std::uint32_t countErrors(const std::vector<AssetIssue>& issues) {
    std::uint32_t n = 0;
    for (const AssetIssue& i : issues)
        if (i.severity == AssetSeverity::Error) ++n;
    return n;
}

} // namespace ftcore
