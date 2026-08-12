#pragma once

#include "environment.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dense {

// Numeric order is part of the AI RPG world/material contract.  The renderer
// stores it in the fractional portion of material kind 7.
enum class AoeBiome : std::uint8_t {
    DeepWater,
    ShallowWater,
    RiverWater,
    MangroveShallows,
    Beach,
    Grassland,
    DryGrass,
    Mud,
    Forest,
    JungleFloor,
    Highland,
    Rock,
    Tundra,
    Snow,
    DesertSand,
    CrackedEarth,
    Count
};

enum class AoeWorldBiome : std::uint8_t {
    Ocean,
    Coast,
    River,
    Wetland,
    TemperateGrassland,
    TemperateForest,
    Rainforest,
    Savanna,
    Desert,
    Taiga,
    Tundra,
    Alpine,
    Count
};

struct AoeHydrologySample {
    float river{};
    float lake{};
    float flow{};
};

struct AoeWorldStats {
    std::array<std::uint32_t,static_cast<std::size_t>(AoeBiome::Count)>
        biomeTileCounts{};
    std::uint32_t terrainTiles{};
    std::uint32_t waterTiles{};
    std::uint32_t trees{};
    std::uint32_t shrubs{};
    std::uint32_t rocks{};
    std::uint32_t grassPatches{};
    float minimumHeight{};
    float maximumHeight{};
};

class AoeWorldGenerator;

// A finite, movable 3D view of the otherwise infinite deterministic world.
// CPU sampling grids deliberately remain resident after takeMesh(), allowing
// camera collision and underwater queries to use the exact geometry source.
class AoeWorldScene {
public:
    static constexpr int gridResolution = 513;
    static constexpr int tileResolution = gridResolution - 1;
    static constexpr float halfExtent = tileResolution * .5f;
    static constexpr float heightScale = .80f;
    static constexpr float waterSurfaceHeight = .012f;

    AoeWorldScene() = default;
    AoeWorldScene(const AoeWorldScene&) = delete;
    AoeWorldScene& operator=(const AoeWorldScene&) = delete;
    AoeWorldScene(AoeWorldScene&&) noexcept = default;
    AoeWorldScene& operator=(AoeWorldScene&&) noexcept = default;

    [[nodiscard]] EnvironmentMesh takeMesh() { return std::move(mesh_); }
    [[nodiscard]] TerrainSurfaceSample sampleTerrain(float x,float z) const;
    [[nodiscard]] PersistentWaterSample sampleWater(float x,float z) const;
    [[nodiscard]] AoeBiome sampleBiome(float x,float z) const;

    [[nodiscard]] std::int64_t seed() const { return seed_; }
    [[nodiscard]] Vec3 spawn() const { return spawn_; }
    [[nodiscard]] float traversalHalfExtent() const { return halfExtent-1.0f; }
    [[nodiscard]] float minimumBound() const { return -halfExtent; }
    [[nodiscard]] float maximumBound() const { return halfExtent; }
    [[nodiscard]] std::uint32_t terrainTileCount() const {
        return stats_.terrainTiles;
    }
    [[nodiscard]] std::uint32_t waterTileCount() const {
        return stats_.waterTiles;
    }
    [[nodiscard]] std::uint32_t treeCount() const { return stats_.trees; }
    [[nodiscard]] const AoeWorldStats& stats() const { return stats_; }

private:
    friend class AoeWorldGenerator;

    [[nodiscard]] std::size_t vertexIndex(int x,int z) const {
        return static_cast<std::size_t>(z)*gridResolution+
               static_cast<std::size_t>(x);
    }
    [[nodiscard]] std::size_t tileIndex(int x,int z) const {
        return static_cast<std::size_t>(z)*tileResolution+
               static_cast<std::size_t>(x);
    }

    std::int64_t seed_{};
    float sourceOriginX_{};
    float sourceOriginZ_{};
    Vec3 spawn_{};
    AoeWorldStats stats_{};
    EnvironmentMesh mesh_{};
    std::vector<float> terrainHeights_;
    std::vector<Vec3> terrainNormals_;
    std::vector<AoeBiome> tileBiomes_;
    std::vector<float> tileWaterDepths_;
};

class AoeWorldGenerator {
public:
    [[nodiscard]] static AoeWorldScene generate(std::int64_t seed = 8675309);

    // Public deterministic probes are intentionally small.  They make the
    // translated C++ implementation comparable with C# golden samples without
    // requiring the source repository or any game assets at runtime.
    [[nodiscard]] static float baseElevationAt(
        std::int64_t seed,int x,int z);
    [[nodiscard]] static float rainfallAt(std::int64_t seed,int x,int z);
    [[nodiscard]] static AoeHydrologySample hydrologyAt(
        std::int64_t seed,float x,float z);
    [[nodiscard]] static AoeBiome biomeAt(std::int64_t seed,int x,int z);
    [[nodiscard]] static float renderedHeightAt(
        std::int64_t seed,float x,float z);
    [[nodiscard]] static Vec3 nearestPlayableSpawn(std::int64_t seed);
};

} // namespace dense
