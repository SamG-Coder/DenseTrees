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
    uint32_t seed{};
    uint32_t packed{};
    float baseY{};
    float normalX{};
    float normalZ{};
    float moisture{};
};
static_assert(sizeof(GrassPatchGpu) == 48);
static_assert(offsetof(GrassPatchGpu,maxX) == 12);
static_assert(offsetof(GrassPatchGpu,seed) == 24);

struct EnvironmentMesh {
    std::vector<MeshVertex> terrainVertices;
    std::vector<uint32_t> terrainIndices;
    // Static scene dressing shares the triangle BLAS with the oak and terrain.
    // Materials 3, 4 and 5 route rocks, foliage and secondary wood in HLSL.
    std::vector<MeshVertex> detailVertices;
    std::vector<uint32_t> detailIndices;
    std::vector<GrassPatchGpu> grassPatches;
    uint32_t tallGrassPatchCount{};
    uint32_t rockCount{};
    uint32_t shrubCount{};
    uint32_t backgroundTreeCount{};
    float minimumHeight{};
    float maximumHeight{};
};

class EnvironmentGenerator {
public:
    static constexpr int terrainResolution = 257;
    static constexpr float terrainHalfExtent = 1600.0f;
    // Covers the complete 30 m camera orbit plus the 192 m debug range.
    // Visible blades are instanced by the raster overlay, so this broader
    // field no longer bloats the ray-tracing acceleration structure.
    static constexpr float grassHalfExtent = 224.0f;

    static float terrainHeight(float x, float z);
    static Vec3 terrainNormal(float x, float z);
    EnvironmentMesh build(uint32_t seed = 0x6f616b31u) const;
};

}
