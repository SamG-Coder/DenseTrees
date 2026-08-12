#pragma once

#include "tree.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dense {

// The first 24 bytes deliberately match D3D12_RAYTRACING_AABB.  The same GPU
// buffer can therefore feed both the grass BLAS and the intersection shader.
struct GrassPatchGpu {
    float minX{}, minY{}, minZ{};
    float maxX{}, maxY{}, maxZ{};
    // Low 24 bits are the deterministic random seed.  The high byte carries
    // the terrain's baked standing-water retention at this patch.  Packing
    // both keeps the hot grass structured-buffer stride at 64 bytes.
    uint32_t seed{};
    uint32_t packed{};
    float baseY{};
    float normalX{};
    float normalZ{};
    float moisture{};
    // Coherent, world-space meadow colour fields generated once per patch.
    // Keeping a 64-byte stride makes GPU structured-buffer reads efficient
    // while avoiding billions of repeated noise evaluations in the VS.
    float colourFertility{};
    float colourDryColony{};
    float colourLushColony{};
    float colourWarmCool{};
};
static_assert(sizeof(GrassPatchGpu) == 64);
static_assert(offsetof(GrassPatchGpu,maxX) == 12);
static_assert(offsetof(GrassPatchGpu,seed) == 24);

constexpr uint32_t grassPatchRandomSeed(uint32_t packedSeed) {
    return packedSeed&0x00ffffffu;
}

constexpr float grassPatchWaterRetention(uint32_t packedSeed) {
    return static_cast<float>(packedSeed>>24)*(1.0f/255.0f);
}

struct EnvironmentMesh {
    std::vector<MeshVertex> terrainVertices;
    std::vector<uint32_t> terrainIndices;
    // Persistent sloping water surfaces for the authored main river and its
    // western tributary.  Indices are local to riverVertices and material 6
    // is reserved for the renderer's reflective river-water path.
    std::vector<MeshVertex> riverVertices;
    std::vector<uint32_t> riverIndices;
    // Static scene dressing shares the triangle BLAS with the oak and terrain.
    // Materials 3, 4 and 5 route rocks, foliage and secondary wood in HLSL.
    std::vector<MeshVertex> detailVertices;
    std::vector<uint32_t> detailIndices;
    std::vector<GrassPatchGpu> grassPatches;
    // Seed used by the camera-relative grass clipmap.  Grass patches are
    // regenerated from absolute world-grid coordinates, so they stay anchored
    // while the player traverses the full map instead of ending at a fixed
    // world-origin disc.
    uint32_t grassSeed{};
    uint32_t tallGrassPatchCount{};
    uint32_t rockCount{};
    uint32_t shrubCount{};
    uint32_t backgroundTreeCount{};
    // The 21 background trees above are medium-detail grove landmarks.  This
    // second inventory is a deliberately cheaper, map-scale woodland LOD used
    // beyond the open hero meadow.  Keeping its bases and triangle count makes
    // deterministic habitat and performance contracts directly testable.
    std::vector<Vec3> distantTreeBases;
    uint32_t distantTreeCount{};
    uint32_t distantTreeTriangleCount{};
    float minimumHeight{};
    float maximumHeight{};
};

// Exact sample of the triangle surface emitted by EnvironmentGenerator::build.
// Queries outside the finite terrain square are clamped to its edge and marked
// invalid so callers can keep collision state finite without treating the
// clamped point as traversable ground.
struct TerrainSurfaceSample {
    Vec3 position{};
    Vec3 normal{0,1,0};
    bool insideBounds{};
};

class EnvironmentGenerator {
public:
    // The squared-ish coordinate distribution keeps hero-tree sampling as
    // dense as the former test field while carrying a complete 6.4 km map.
    // 321^2 vertices / 204,800 triangles remains a modest static BLAS.
    static constexpr int terrainResolution = 321;
    static constexpr float terrainHalfExtent = 3200.0f;
    // Keep the first-person capsule away from the finite heightfield edge so
    // all of its support samples remain on authored terrain.
    static constexpr float traversalEdgeMargin = 2.0f;
    static constexpr float traversalHalfExtent =
        terrainHalfExtent - traversalEdgeMargin;
    // Legacy origin-field radius retained for deterministic generation tests.
    // Rendering uses makeGrassPatch() as a camera-relative world-anchored
    // clipmap, so this is no longer a visible map boundary.
    static constexpr float grassHalfExtent = 224.0f;
    static constexpr float grassCellSize = 0.55f;

    static float terrainHeight(float x, float z);
    static Vec3 terrainNormal(float x, float z);
    static TerrainSurfaceSample sampleTerrainSurface(float x, float z);

    // World-stable material mask around the origin-centred hero oak. One is
    // exposed root loam and zero is uninterrupted meadow. The exact analytic
    // profile is shared with the terrain shader so grass density and material
    // blending cannot form two different rings around the same buttresses.
    static float rootLoamWeight(float x, float z);

    // Deterministic map contract shared by terrain tests and the renderer's
    // persistent river pass.  The main river flows toward increasing Z.
    static float riverCenterX(float z);
    static float riverHalfWidth(float z);
    static float riverBedHeight(float z);
    static float riverSurfaceHeight(float z);
    // The wetted shoreline is shared by the terrain carve, persistent-water
    // mesh, grass biome mask, and tests.  Keeping it as an explicit world
    // contract prevents a water strip from ending over a different bank
    // profile and exposing a dark suspended edge.
    static float riverWaterHalfWidth(float z);
    static float tributaryCenterZ(float x);
    static float tributaryHalfWidth(float x);
    static float tributaryBedHeight(float x);
    static float tributarySurfaceHeight(float x);
    static float tributaryWaterHalfWidth(float x);
    // Deterministically materializes one absolute world-grid grass patch.
    // Returns false for permanent water, exposed dirt/mineral ground, steep
    // slopes and other biomes that should not carry meadow grass.
    static bool makeGrassPatch(int cellX,int cellZ,uint32_t seed,
                               GrassPatchGpu& patch);
    EnvironmentMesh build(uint32_t seed = 0x6f616b31u) const;
};

}
