#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace dense {

enum class GroundMaterialTile : uint32_t {
    DenseShortTurf = 0,
    CoarseMeadow = 1,
    WornSoil = 2,
    CloverMoss = 3,
};

struct GroundTextureMip {
    uint32_t width{};
    uint32_t height{};
    // Row-major RGBA8_UNORM pixels packed as R | (G << 8) | (B << 16) | (A << 24).
    std::vector<uint32_t> pixels;
};

struct GroundTextureAtlas {
    static constexpr uint32_t atlasWidth = 2048;
    static constexpr uint32_t atlasHeight = 2048;
    static constexpr uint32_t tileSize = 1024;
    static constexpr uint32_t tileCount = 4;
    // A 2x2 atlas can retain four isolated materials down to a 2x2 mip, where
    // each material owns one texel. A 1x1 mip cannot represent four materials
    // without bleeding and is therefore deliberately omitted.
    static constexpr uint32_t tileSafeMipCount = 11;
    static constexpr float tileWorldSizeMetres = 2.0f;

    // R,G,B contain linear albedo; A contains perceptual roughness.
    std::vector<GroundTextureMip> albedoRoughness;
    // R,G encode tangent-space normal X,Y in [0,1], B is normalized height,
    // and A is cavity (0 = open surface, 1 = deep cavity).
    std::vector<GroundTextureMip> normalHeightCavity;

    // B=0 and B=1 map to -amplitude and +amplitude respectively.
    std::array<float,tileCount> heightAmplitudeMetres{.010f,.018f,.022f,.012f};
};

// Atlas quadrants are ordered top-left, top-right, bottom-left, bottom-right,
// matching GroundMaterialTile's numeric values. Every tile and every mip is
// generated independently from the supplied seed.
[[nodiscard]] GroundTextureAtlas makeGroundTextureAtlas(uint32_t seed = 0x67726f75u);

} // namespace dense
