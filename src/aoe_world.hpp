#pragma once

#include "environment.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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

struct AoeBiomeWeights {
    std::array<float,static_cast<std::size_t>(AoeBiome::Count)> values{};

    [[nodiscard]] float operator[](AoeBiome biome) const {
        return values[static_cast<std::size_t>(biome)];
    }
    [[nodiscard]] float waterCoverage() const {
        return values[0]+values[1]+values[2]+values[3];
    }
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
    std::uint32_t trails{};
    std::uint32_t worldFeatures{};
    std::uint32_t gameplayMarkers{};
    float minimumHeight{};
    float maximumHeight{};
};

class AoeWorldGenerator;
struct AoeDressingResult;

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
    [[nodiscard]] AoeBiomeWeights sampleBiomeWeights(float x,float z) const;
    [[nodiscard]] float sampleShoreDistance(float x,float z) const;

    [[nodiscard]] std::int64_t seed() const { return seed_; }
    [[nodiscard]] float sourceOriginX() const { return sourceOriginX_; }
    [[nodiscard]] float sourceOriginZ() const { return sourceOriginZ_; }
    [[nodiscard]] float sourceCenterX() const {
        return sourceOriginX_+halfExtent;
    }
    [[nodiscard]] float sourceCenterZ() const {
        return sourceOriginZ_+halfExtent;
    }
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
    // Source tree metadata and the native 3D trails/sites/interaction markers
    // remain available after the render mesh is moved into the renderer.
    [[nodiscard]] const AoeDressingResult* dressing() const {
        return dressing_.get();
    }

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
    std::vector<AoeBiomeWeights> vertexBiomeWeights_;
    std::vector<float> vertexShoreDistances_;
    std::shared_ptr<AoeDressingResult> dressing_;
};

class AoeWorldGenerator {
public:
    [[nodiscard]] static AoeWorldScene generate(std::int64_t seed = 8675309);
    // Builds the same 512 m render window around an arbitrary absolute source
    // coordinate. Local mesh positions stay camera-friendly while generation
    // hashes, climate and hydrology continue in global world space.
    [[nodiscard]] static AoeWorldScene generateWindow(
        std::int64_t seed,int centerX,int centerZ);

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
