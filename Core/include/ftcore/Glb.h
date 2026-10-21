#pragma once

// Reads what the asset rules care about from a binary glTF 2.0 file (.glb), and checks it.
//
// GlbInfo is a load-time summary (parsing allocates): the materials with the texture slots they fill
// and their emissive level, the images with their pixel size (read from the PNG or JPEG header in the
// file's binary chunk), and the mesh-wide facts (UV sets, vertex colour, morph targets). Nothing is
// decoded beyond headers.
//
//   ftcore::GlbInfo info;
//   std::string error;
//   if (!ftcore::readGlb(bytes, info, &error)) ...;
//   ftcore::AssetRules rules = ftcore::characterRules(ftcore::AssetPack::Lattice, ftcore::AssetRole::Lead);
//   for (const ftcore::AssetIssue& i : ftcore::checkGlb(info, rules)) ...;
//
// The rules are the ones in docs/ASSET_INTAKE.md:
//   - Emissive level is max(emissiveFactor) x KHR_materials_emissive_strength. In the Lattice a
//     material named for a glow (its name contains "Glow", "Eye", "Skill" or "Rule", any case) must
//     reach kGlowMin (3.0); every other emissive material must stay below kPlainMax (2.5). Other
//     packs have no emissive rule.
//   - One UV set (TEXCOORD_0 only). No vertex colour (COLOR_0): pockets and load maps go in textures.
//   - Image sizes: square powers of two, at most the role's size (2048 lead, 1024 otherwise).
//   - Images are embedded (a bufferView), PNG or JPEG.
// Every issue names a material, image or mesh by index and name, never anything else.

#include "ftcore/Api.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class TextureSlot : std::uint8_t { BaseColor, MetallicRoughness, Normal, Occlusion, Emissive, Count };

struct GlbMaterial {
    std::string name;
    int texture[static_cast<int>(TextureSlot::Count)] = {-1, -1, -1, -1, -1}; // image index, or -1
    float baseColor[4] = {1.f, 1.f, 1.f, 1.f};
    float emissiveFactor[3] = {0.f, 0.f, 0.f};
    float emissiveStrength = 1.f; // KHR_materials_emissive_strength
    float metallic = 1.f, roughness = 1.f;

    bool hasTexture(TextureSlot s) const { return texture[static_cast<int>(s)] >= 0; }
    // max(emissiveFactor) x emissiveStrength.
    FTCORE_API float emissiveLevel() const;
};

enum class ImageFormat : std::uint8_t { Unknown, Png, Jpeg };

struct GlbImage {
    std::string name;
    ImageFormat format = ImageFormat::Unknown;
    bool embedded = false; // in the binary chunk (a bufferView), not a uri
    std::uint32_t width = 0, height = 0; // 0 when the header could not be read
};

struct GlbInfo {
    std::vector<GlbMaterial> materials;
    std::vector<GlbImage> images;
    std::uint32_t meshes = 0, primitives = 0, skins = 0;
    std::uint32_t uvSets = 0;        // the most TEXCOORD_n sets on any primitive
    bool vertexColor = false;        // any primitive has COLOR_0
    std::uint32_t morphTargets = 0;  // the most targets on any primitive
};

// False on a malformed file (bad magic, version, chunk layout or JSON); `*error` says which.
FTCORE_API bool readGlb(std::string_view bytes, GlbInfo& out, std::string* error = nullptr);

enum class AssetPack : std::uint8_t { None, Solhaven, Lattice, Unwritten, Restored, InkSolhaven };
enum class AssetRole : std::uint8_t { Lead, Supporting, Prop };

// "Solhaven", "Lattice", "Unwritten", "Restored", "InkSolhaven" (any case; "Ink Solhaven" and
// "Ink-Solhaven" too). Anything else is None.
FTCORE_API AssetPack assetPackFromName(std::string_view name);
// "lead", "supporting", "prop" (any case); anything else is Supporting.
FTCORE_API AssetRole assetRoleFromName(std::string_view name);

struct AssetRules {
    static constexpr float kGlowMin = 3.0f;
    static constexpr float kPlainMax = 2.5f;

    AssetPack pack = AssetPack::None;
    std::uint32_t maxTextureSize = 1024;
    bool emissiveRule = false;   // the Lattice 3.0 / 2.5 split
    bool requireSkin = false;    // characters are skinned
};

FTCORE_API AssetRules characterRules(AssetPack pack, AssetRole role);
FTCORE_API AssetRules propRules(AssetPack pack);

// A material name that asks for a glow: contains Glow, Eye, Skill or Rule (any case).
FTCORE_API bool isGlowMaterialName(std::string_view name);

enum class AssetSeverity : std::uint8_t { Warning, Error };

struct AssetIssue {
    AssetSeverity severity = AssetSeverity::Error;
    std::string code;    // stable, e.g. "emissive-glow-low"
    std::string message; // names the material/image/mesh by index and name
};

FTCORE_API std::vector<AssetIssue> checkGlb(const GlbInfo& info, const AssetRules& rules);
FTCORE_API std::uint32_t countErrors(const std::vector<AssetIssue>& issues);

} // namespace ftcore
