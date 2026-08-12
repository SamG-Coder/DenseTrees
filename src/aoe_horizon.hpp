#pragma once

#include "aoe_world.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace dense {

struct AoeHorizonRing {
    int innerExtent{};
    int outerExtent{};
    int cellSize{};
    float heightScale{.80f};
};

struct AoeHorizonStats {
    std::array<std::uint32_t,3> terrainCells{};
    std::array<std::uint32_t,3> waterCells{};
    std::array<std::uint32_t,3> blendedTerrainCells{};
    std::array<std::uint32_t,3> waterBoundaryCells{};
    std::uint32_t terrainVertices{};
    std::uint32_t terrainIndices{};
    std::uint32_t waterVertices{};
    std::uint32_t waterIndices{};
    float outerExtent{};
};

using AoeNearTerrainSampler=
    std::function<TerrainSurfaceSample(float localX,float localZ)>;

// Appends three deterministic, camera-local LOD annuli around the 512 m near
// scene. Source coordinates remain absolute, so moving/rebasing the near mesh
// never changes the generated world's seed contract.
class AoeHorizonBuilder {
public:
    static constexpr std::array<AoeHorizonRing,3> rings{{
        {256,1024,32,.80f},
        {1024,4096,64,.52f},
        {4096,16384,128,.10f}
    }};

    [[nodiscard]] static AoeHorizonStats append(
        EnvironmentMesh& mesh,std::int64_t seed,
        int sourceCenterX,int sourceCenterZ,
        AoeNearTerrainSampler nearTerrain={});
};

} // namespace dense
