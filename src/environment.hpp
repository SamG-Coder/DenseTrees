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
    std::vector<GrassPatchGpu> grassPatches;
    float minimumHeight{};
    float maximumHeight{};
};

class EnvironmentGenerator {
public:
    static constexpr int terrainResolution = 193;
    static constexpr float terrainHalfExtent = 120.0f;
    static constexpr float grassHalfExtent = 24.0f;

    static float terrainHeight(float x, float z);
    static Vec3 terrainNormal(float x, float z);
    EnvironmentMesh build(uint32_t seed = 0x6f616b31u) const;
};

}
