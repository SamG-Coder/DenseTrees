#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace dense {

enum class GroundMaterialTile : uint32_t {
    // The numeric order is part of the CPU/HLSL ABI.  The more descriptive
    // names below retain aliases for older call sites while making the atlas
    // useful to the world-map biome pass.
    MeadowTurf = 0,
    UplandShortTurf = 1,
    ExposedRockSoil = 2,
    RiparianMoss = 3,
    // Local detail material beneath the hero oak.  This is deliberately not
    // part of the four-way biome mixture: roots disturb the existing biome
    // instead of defining a fifth landscape biome.
    RootLoam = 4,
    DenseShortTurf = MeadowTurf,
    CoarseMeadow = UplandShortTurf,
    WornSoil = ExposedRockSoil,
    CloverMoss = RiparianMoss,
};

struct GroundTextureMip {
    uint32_t width{};
    uint32_t height{};
    // Row-major RGBA8_UNORM pixels packed as R | (G << 8) | (B << 16) | (A << 24).
    std::vector<uint32_t> pixels;
};

struct GroundTextureAtlas {
    static constexpr uint32_t tileSize = 1024;
    static constexpr uint32_t biomeMaterialCount = 4;
    static constexpr uint32_t tileCount = 5;
    // CPU generation uses a horizontal staging strip.  Each material is
    // downsampled independently, then uploaded as one Texture2DArray slice.
    // At the coarsest 5x1 staging mip every slice still owns one isolated
    // texel, so no material can bleed into a neighbour.
    static constexpr uint32_t atlasWidth = tileSize * tileCount;
    static constexpr uint32_t atlasHeight = tileSize;
    static constexpr uint32_t tileSafeMipCount = 11;
    static constexpr float tileWorldSizeMetres = 2.0f;

    // R,G,B contain linear albedo; A contains perceptual roughness.
    std::vector<GroundTextureMip> albedoRoughness;
    // R,G encode tangent-space normal X,Y in [0,1], B is normalized height,
    // and A is cavity (0 = open surface, 1 = deep cavity).
    std::vector<GroundTextureMip> normalHeightCavity;

    // B=0 and B=1 map to -amplitude and +amplitude respectively.
    // Relief is physical displacement, not a visual-strength knob.  Turf is
    // a close-cropped millimetre-scale mat; large amplitudes turn its normal
    // map into a woven tarpaulin under grazing light.  Mineral plates retain
    // centimetre-scale relief, while moss/silt remains soft and shallow.
    std::array<float,tileCount> heightAmplitudeMetres{.0035f,.0048f,.028f,.0065f,.0050f};
};

struct GroundBiomeInput {
    float elevationMetres{};
    // Rise/run, rather than degrees, so terrain normals can supply this as
    // length(normal.xz) / normal.y without an atan in population loops.
    float slopeGradient{};
    float riverDistanceMetres{1000.0f};
    float moisture{0.5f};
    // Broad erosion/geology noise in [0,1].  This prevents a single contour
    // line from looking like a painted biome boundary.
    float exposure{};
};

struct GroundBiomeWeights {
    std::array<float,GroundTextureAtlas::biomeMaterialCount> material{};
};

// Atlas staging tiles run left-to-right in GroundMaterialTile numeric order.
// Every tile and every mip is generated independently from the supplied seed.
[[nodiscard]] GroundTextureAtlas makeGroundTextureAtlas(uint32_t seed = 0x67726f75u);

// Produces a normalized, smoothly blended biome mixture.  It deliberately
// depends only on sampled terrain/river values so EnvironmentGenerator and
// tests can share it without coupling the material system to terrainHeight().
[[nodiscard]] GroundBiomeWeights groundBiomeWeights(const GroundBiomeInput& input);

} // namespace dense
