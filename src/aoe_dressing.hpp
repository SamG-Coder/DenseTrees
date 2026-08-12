#pragma once

#include "aoe_world.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dense {

// These names describe which source-world rule produced a tree.  They do not
// refer to copied art: DenseTrees builds every visible archetype procedurally.
enum class AoeTreeFamily : std::uint8_t {
    Palm,
    Pine,
    Oak,
    Jungle,
    SnowConifer,
    Bamboo,
    Cactus,
    GenericA,
    GenericB,
    GenericC,
    GenericD,
    GenericE,
    GenericF,
    GenericG,
    GenericH,
    GenericI,
    GenericJ,
    GenericK,
    GenericL
};

enum class NativeTreeArchetype : std::uint8_t {
    Broadleaf,
    Conifer,
    Palm,
    JungleBroadleaf,
    BambooClump,
    SnowConifer,
    Cactus
};

enum class AoeDressingProvenance : std::uint8_t {
    // Direct translation of WorldTreeCatalog at source revision 3e76dcd.
    SourceParity,
    // DenseTrees-only representation or placement rule designed for 3D.
    Native3DExtension
};

struct AoeTreeFamilyMetadata {
    AoeTreeFamily family{};
    const char* sourceGraphicName{};
    std::uint8_t sourceVariantCount{};
    NativeTreeArchetype nativeArchetype{};
    AoeDressingProvenance selectionProvenance{
        AoeDressingProvenance::SourceParity};
    AoeDressingProvenance geometryProvenance{
        AoeDressingProvenance::Native3DExtension};
};

struct AoeSourceTreeSelection {
    bool spawned{};
    AoeTreeFamily family{AoeTreeFamily::GenericA};
    std::uint8_t sourceFrame{};
    float spawnChance{};
    float spawnRoll{};
    AoeDressingProvenance provenance{AoeDressingProvenance::SourceParity};
};

struct AoeDressingSample {
    float elevation{};
    AoeBiome biome{AoeBiome::DeepWater};
    AoeWorldBiome region{AoeWorldBiome::Ocean};
    Vec3 position{};
    Vec3 normal{0,1,0};
    // Continuous woodland coverage supplied by the biome-weight field.  The
    // dressing layer uses it for gradual forest-edge thinning.
    float forestInterior{};
    bool traversable{};
};

using AoeDressingSampler =
    std::function<AoeDressingSample(int sourceX,int sourceZ)>;

struct AoeTreeInstance3D {
    int sourceX{};
    int sourceZ{};
    Vec3 position{};
    AoeTreeFamily family{AoeTreeFamily::GenericA};
    NativeTreeArchetype archetype{NativeTreeArchetype::Broadleaf};
    std::uint8_t sourceFrame{};
    float height{};
    float crownRadius{};
    float forestInterior{};
    AoeDressingProvenance familyProvenance{
        AoeDressingProvenance::SourceParity};
    AoeDressingProvenance placementProvenance{
        AoeDressingProvenance::Native3DExtension};
    // Exact subrange in AoeDressingResult's detail geometry. Besides making
    // streamed batches inspectable, this keeps canopy budget and silhouette
    // regressions testable per procedural tree.
    std::uint32_t geometryFirstVertex{};
    std::uint32_t geometryVertexCount{};
    std::uint32_t geometryFirstIndex{};
    std::uint32_t geometryIndexCount{};
};

enum class AoeWorldFeatureKind : std::uint8_t {
    StarterCamp,
    Trail,
    ForestGrove,
    StandingStones,
    CoastalBeacon,
    WetlandTotem,
    SpawnMarker,
    CampfireInteraction,
    ResourceInteraction,
    EncounterMarker,
    QuestMarker
};

struct AoeWorldFeature3D {
    AoeWorldFeatureKind kind{};
    Vec3 position{};
    float radius{};
    std::uint64_t stableId{};
    // Roads, camps, landmarks and gameplay markers are deliberately not
    // claimed as source parity; they are the first DenseTrees gameplay layer.
    AoeDressingProvenance provenance{
        AoeDressingProvenance::Native3DExtension};
};

struct AoeTrail3D {
    std::uint64_t stableId{};
    std::vector<Vec3> points;
    float halfWidth{.42f};
    AoeDressingProvenance provenance{
        AoeDressingProvenance::Native3DExtension};
};

struct AoeDressingConfig {
    std::int64_t seed{8675309};
    int minimumSourceX{};
    int maximumSourceX{}; // exclusive
    int minimumSourceZ{};
    int maximumSourceZ{}; // exclusive
    float localOriginX{};
    float localOriginZ{};
    int spawnSourceX{};
    int spawnSourceZ{};
    bool includeWorldFeatures{true};
};

struct AoeDressingResult {
    std::vector<AoeTreeInstance3D> trees;
    std::vector<AoeWorldFeature3D> features;
    std::vector<AoeTrail3D> trails;
    std::vector<MeshVertex> detailVertices;
    std::vector<std::uint32_t> detailIndices;

    void appendGeometryTo(EnvironmentMesh& mesh) const;
};

class AoeWorldDressing {
public:
    [[nodiscard]] static const AoeTreeFamilyMetadata& metadata(
        AoeTreeFamily family);
    [[nodiscard]] static float sourceSpawnChance(
        AoeWorldBiome region,float elevation);
    [[nodiscard]] static AoeSourceTreeSelection sourceTreeAt(
        std::int64_t seed,int sourceX,int sourceZ,AoeWorldBiome region,
        AoeBiome biome,float elevation);

    // Generates native low-poly dressing over [minimum, maximum).  The sampler
    // must also answer a small halo around that range so spacing remains stable
    // when adjacent streamed chunks are generated independently.
    [[nodiscard]] static AoeDressingResult generate(
        const AoeDressingConfig& config,const AoeDressingSampler& sampler);
};

} // namespace dense
