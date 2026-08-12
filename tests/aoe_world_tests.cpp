#include "aoe_world.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(bool condition,std::string_view message) {
    if(!condition)fail(message);
}

bool near(float actual,float expected,float tolerance=6.0e-4f) {
    return std::isfinite(actual)&&std::abs(actual-expected)<=tolerance;
}

bool finite(dense::Vec3 value) {
    return std::isfinite(value.x)&&std::isfinite(value.y)&&
           std::isfinite(value.z);
}

void requireGolden(float actual,float expected,std::string_view label,
                   float tolerance=6.0e-4f) {
    if(!near(actual,expected,tolerance)) {
        std::cerr << "FAIL: " << label << " expected " << expected
                  << " but got " << actual << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void checkAuthoritativeGoldens() {
    using dense::AoeBiome;
    using dense::AoeWorldGenerator;

    // These values were evaluated by the C# implementation at source revision
    // 3e76dcd.  They cover negative coordinates, a chunk boundary and active
    // macro-hydrology rather than merely comparing the C++ port with itself.
    requireGolden(AoeWorldGenerator::baseElevationAt(2187,0,0),-1.6780317f,
                  "seed 2187 origin base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(2187,0,0),1.0237592f,
                  "seed 2187 origin rainfall");
    require(AoeWorldGenerator::biomeAt(2187,0,0)==AoeBiome::DeepWater,
            "seed 2187 origin biome differs from the C# world");
    requireGolden(AoeWorldGenerator::renderedHeightAt(2187,.375f,.625f),0.0f,
                  "seed 2187 flattened ocean height");

    requireGolden(AoeWorldGenerator::baseElevationAt(2187,-62,-62),.9356617f,
                  "seed 2187 playable-coast base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(2187,-62,-62),.93709123f,
                  "seed 2187 playable-coast rainfall");
    require(AoeWorldGenerator::biomeAt(2187,-62,-62)==AoeBiome::Beach,
            "seed 2187 nearest playable tile is not beach");
    requireGolden(AoeWorldGenerator::renderedHeightAt(
                      2187,-61.5f,-61.5f),0.0f,
                  "seed 2187 beach rendered height");

    requireGolden(AoeWorldGenerator::baseElevationAt(2187,75,-153),3.766281f,
                  "seed 2187 inland base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(2187,75,-153),.8858372f,
                  "seed 2187 inland rainfall");
    require(AoeWorldGenerator::biomeAt(2187,75,-153)==AoeBiome::Forest,
            "seed 2187 inland biome differs from the C# world");
    requireGolden(AoeWorldGenerator::renderedHeightAt(
                      2187,75.375f,-152.375f),3.0f,
                  "seed 2187 smoothed inland height");

    requireGolden(AoeWorldGenerator::baseElevationAt(
                      2187,-257,-193),-.5101283f,
                  "negative-cell base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(
                      2187,-257,-193),.80464303f,
                  "negative-cell rainfall");
    require(AoeWorldGenerator::biomeAt(2187,-257,-193)==AoeBiome::DeepWater,
            "negative-cell biome differs from the C# world");
    requireGolden(AoeWorldGenerator::renderedHeightAt(
                      2187,-256.625f,-192.375f),0.0f,
                  "negative-cell rendered height");

    requireGolden(AoeWorldGenerator::baseElevationAt(2187,511,97),2.0546308f,
                  "seed 2187 pre-boundary base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(2187,511,97),.49369124f,
                  "seed 2187 pre-boundary rainfall");
    require(AoeWorldGenerator::biomeAt(2187,511,97)==AoeBiome::DesertSand,
            "seed 2187 pre-boundary biome differs from the C# world");
    requireGolden(AoeWorldGenerator::baseElevationAt(2187,512,97),2.0288506f,
                  "seed 2187 post-boundary base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(2187,512,97),.49934345f,
                  "seed 2187 post-boundary rainfall");
    require(AoeWorldGenerator::biomeAt(2187,512,97)==AoeBiome::DesertSand,
            "seed 2187 post-boundary biome differs from the C# world");
    requireGolden(AoeWorldGenerator::renderedHeightAt(
                      2187,512.375f,97.625f),0.0f,
                  "seed 2187 post-boundary rendered height");
    requireGolden(AoeWorldGenerator::baseElevationAt(2187,512,512),-.9183657f,
                  "seed 2187 diagonal-boundary base elevation");
    require(AoeWorldGenerator::biomeAt(2187,512,512)==AoeBiome::DeepWater,
            "seed 2187 diagonal-boundary biome differs from the C# world");

    requireGolden(AoeWorldGenerator::baseElevationAt(8675309,0,0),4.586389f,
                  "showcase origin base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(8675309,0,0),.7095554f,
                  "showcase origin rainfall");
    require(AoeWorldGenerator::biomeAt(8675309,0,0)==AoeBiome::Forest,
            "showcase origin is not temperate forest");
    requireGolden(AoeWorldGenerator::renderedHeightAt(
                      8675309,.375f,.625f),4.0f,
                  "showcase origin rendered height");

    requireGolden(AoeWorldGenerator::baseElevationAt(
                      8675309,137,-91),2.8385766f,
                  "showcase inland-river base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(
                      8675309,137,-91),.6840575f,
                  "showcase inland-river rainfall");
    require(AoeWorldGenerator::biomeAt(8675309,137,-91)==AoeBiome::Forest,
            "showcase inland-river tile biome differs from the C# world");
    requireGolden(AoeWorldGenerator::renderedHeightAt(
                      8675309,137.375f,-90.375f),0.0f,
                  "showcase inland-river flattened height");

    requireGolden(AoeWorldGenerator::baseElevationAt(
                      8675309,-257,-193),.6880931f,
                  "showcase negative-cell base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(
                      8675309,-257,-193),.710155f,
                  "showcase negative-cell rainfall");
    require(AoeWorldGenerator::biomeAt(8675309,-257,-193)==
                AoeBiome::ShallowWater,
            "showcase negative-cell biome differs from the C# world");

    requireGolden(AoeWorldGenerator::baseElevationAt(8675309,511,97),8.70583f,
                  "showcase pre-boundary base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(8675309,511,97),.7276571f,
                  "showcase pre-boundary rainfall");
    require(AoeWorldGenerator::biomeAt(8675309,511,97)==AoeBiome::Highland,
            "showcase pre-boundary biome differs from the C# world");
    requireGolden(AoeWorldGenerator::renderedHeightAt(
                      8675309,511.375f,97.625f),8.0f,
                  "showcase pre-boundary rendered height");

    requireGolden(AoeWorldGenerator::baseElevationAt(
                      8675309,512,97),8.593157f,
                  "positive chunk-boundary base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(
                      8675309,512,97),.727929f,
                  "positive chunk-boundary rainfall");
    require(AoeWorldGenerator::biomeAt(8675309,512,97)==AoeBiome::Highland,
            "positive chunk-boundary biome differs from the C# world");
    requireGolden(AoeWorldGenerator::renderedHeightAt(
                      8675309,512.375f,97.625f),7.915039f,
                  "positive chunk-boundary rendered height",1.2e-3f);

    requireGolden(AoeWorldGenerator::baseElevationAt(
                      8675309,512,512),-.61055386f,
                  "showcase diagonal-boundary base elevation");
    requireGolden(AoeWorldGenerator::rainfallAt(
                      8675309,512,512),.5423747f,
                  "showcase diagonal-boundary rainfall");
    require(AoeWorldGenerator::biomeAt(8675309,512,512)==AoeBiome::DeepWater,
            "showcase diagonal-boundary biome differs from the C# world");

    const dense::AoeHydrologySample hydrology=
        AoeWorldGenerator::hydrologyAt(8675309,224.0f,-232.0f);
    requireGolden(hydrology.river,1.0f,"active river mask",2.0e-3f);
    requireGolden(hydrology.lake,.017627973f,"active lake mask",2.0e-3f);
    requireGolden(hydrology.flow,105.26262f,"active flow accumulation",3.0e-2f);
    require(AoeWorldGenerator::biomeAt(8675309,224,-232)==
                AoeBiome::ShallowWater,
            "active-hydrology tile biome differs from the C# world");

    const dense::Vec3 showcaseSpawn=
        AoeWorldGenerator::nearestPlayableSpawn(8675309);
    const dense::Vec3 coastSpawn=AoeWorldGenerator::nearestPlayableSpawn(2187);
    require(near(showcaseSpawn.x,.5f,1.0e-5f)&&
                near(showcaseSpawn.z,.5f,1.0e-5f),
            "showcase nearest-playable search order changed");
    require(near(coastSpawn.x,-61.5f,1.0e-5f)&&
                near(coastSpawn.z,-61.5f,1.0e-5f),
            "seed 2187 nearest-playable search order changed");
}

struct LegacySignature {
    std::array<float,8> values{};
    bool waterInside{};
};

LegacySignature legacySignature() {
    const float riverX=dense::EnvironmentGenerator::riverCenterX(0.0f);
    const dense::PersistentWaterSample water=
        dense::EnvironmentGenerator::persistentWater(riverX,0.0f);
    return {{{dense::EnvironmentGenerator::terrainHeight(0.0f,0.0f),
              dense::EnvironmentGenerator::terrainHeight(311.0f,-227.0f),
              riverX,
              dense::EnvironmentGenerator::riverWaterHalfWidth(0.0f),
              water.surfaceHeight,water.depth,water.shoreCoordinate,
              dense::EnvironmentGenerator::tributaryCenterZ(-400.0f)}},
            water.inside};
}

void validateLegacySignature(const LegacySignature& signature) {
    for(float value:signature.values)
        require(std::isfinite(value),
                "retained Dense Trees generator emitted a non-finite sample");
    require(near(signature.values[0],0.0f,1.0e-6f)&&signature.waterInside&&
                signature.values[5]>.5f,
            "retained Dense Trees terrain/river contract changed");
}

void validateTriangles(const std::vector<dense::MeshVertex>& vertices,
                       const std::vector<std::uint32_t>& indices,
                       std::string_view label) {
    require(!vertices.empty(),"generated mesh part has no vertices");
    require(!indices.empty()&&indices.size()%3==0,
            "generated mesh part has an invalid triangle inventory");
    for(std::size_t index=0;index<indices.size();index+=3) {
        const std::uint32_t ia=indices[index];
        const std::uint32_t ib=indices[index+1];
        const std::uint32_t ic=indices[index+2];
        if(ia>=vertices.size()||ib>=vertices.size()||ic>=vertices.size()) {
            std::cerr << "FAIL: " << label << " triangle " << index/3
                      << " has an out-of-range index\n";
            std::exit(EXIT_FAILURE);
        }
        const dense::MeshVertex& a=vertices[ia];
        const dense::MeshVertex& b=vertices[ib];
        const dense::MeshVertex& c=vertices[ic];
        const dense::Vec3 area=dense::cross(b.position-a.position,
                                            c.position-a.position);
        if(!finite(a.position)||!finite(b.position)||!finite(c.position)||
           !finite(a.normal)||!finite(b.normal)||!finite(c.normal)||
           !finite(area)||dense::lengthSq(area)<=1.0e-16f) {
            std::cerr << "FAIL: " << label << " triangle " << index/3
                      << " is non-finite or degenerate\n";
            std::exit(EXIT_FAILURE);
        }
    }
}

std::uint64_t mix(std::uint64_t hash,std::uint32_t value) {
    hash^=value;
    return hash*1099511628211ull;
}

std::uint64_t meshSignature(const dense::EnvironmentMesh& mesh) {
    std::uint64_t hash=1469598103934665603ull;
    const auto vertices=[&](const std::vector<dense::MeshVertex>& values) {
        hash=mix(hash,static_cast<std::uint32_t>(values.size()));
        if(values.empty())return;
        const std::size_t stride=std::max<std::size_t>(1,values.size()/4096);
        for(std::size_t index=0;index<values.size();index+=stride) {
            const dense::MeshVertex& value=values[index];
            hash=mix(hash,std::bit_cast<std::uint32_t>(value.position.x));
            hash=mix(hash,std::bit_cast<std::uint32_t>(value.position.y));
            hash=mix(hash,std::bit_cast<std::uint32_t>(value.position.z));
            hash=mix(hash,value.color);
            hash=mix(hash,std::bit_cast<std::uint32_t>(value.material));
            hash=mix(hash,std::bit_cast<std::uint32_t>(value.u));
        }
    };
    const auto indices=[&](const std::vector<std::uint32_t>& values) {
        hash=mix(hash,static_cast<std::uint32_t>(values.size()));
        if(values.empty())return;
        const std::size_t stride=std::max<std::size_t>(1,values.size()/4096);
        for(std::size_t index=0;index<values.size();index+=stride)
            hash=mix(hash,values[index]);
    };
    vertices(mesh.terrainVertices);indices(mesh.terrainIndices);
    vertices(mesh.riverVertices);indices(mesh.riverIndices);
    vertices(mesh.detailVertices);indices(mesh.detailIndices);
    hash=mix(hash,static_cast<std::uint32_t>(mesh.grassPatches.size()));
    for(std::size_t index=0;index<mesh.grassPatches.size();
        index+=std::max<std::size_t>(1,mesh.grassPatches.size()/1024)) {
        const dense::GrassPatchGpu& patch=mesh.grassPatches[index];
        hash=mix(hash,std::bit_cast<std::uint32_t>(patch.minX));
        hash=mix(hash,std::bit_cast<std::uint32_t>(patch.baseY));
        hash=mix(hash,patch.seed);hash=mix(hash,patch.packed);
    }
    return hash;
}

struct MeshCounts {
    std::size_t terrainVertices{},terrainIndices{};
    std::size_t waterVertices{},waterIndices{};
    std::size_t detailVertices{},detailIndices{},grassPatches{};
    std::uint64_t signature{};
};

MeshCounts countsOf(const dense::EnvironmentMesh& mesh) {
    return {mesh.terrainVertices.size(),mesh.terrainIndices.size(),
            mesh.riverVertices.size(),mesh.riverIndices.size(),
            mesh.detailVertices.size(),mesh.detailIndices.size(),
            mesh.grassPatches.size(),meshSignature(mesh)};
}

bool sameCounts(const MeshCounts& left,const MeshCounts& right) {
    return left.terrainVertices==right.terrainVertices&&
           left.terrainIndices==right.terrainIndices&&
           left.waterVertices==right.waterVertices&&
           left.waterIndices==right.waterIndices&&
           left.detailVertices==right.detailVertices&&
           left.detailIndices==right.detailIndices&&
           left.grassPatches==right.grassPatches&&
           left.signature==right.signature;
}

struct SceneSnapshot {
    dense::AoeWorldStats stats{};
    dense::TerrainSurfaceSample terrain{};
    dense::PersistentWaterSample water{};
    dense::AoeBiome biome{};
    dense::Vec3 spawn{};
    MeshCounts mesh{};
};

SceneSnapshot inspectShowcaseScene(bool validateCompleteMesh) {
    dense::AoeWorldScene scene=dense::AoeWorldGenerator::generate(8675309);
    SceneSnapshot snapshot;
    snapshot.stats=scene.stats();
    snapshot.spawn=scene.spawn();
    snapshot.terrain=scene.sampleTerrain(snapshot.spawn.x,snapshot.spawn.z);
    snapshot.water=scene.sampleWater(snapshot.spawn.x,snapshot.spawn.z);
    snapshot.biome=scene.sampleBiome(snapshot.spawn.x,snapshot.spawn.z);

    require(scene.seed()==8675309&&
                near(scene.traversalHalfExtent(),255.0f,1.0e-6f),
            "generated scene seed or traversal bounds are wrong");
    require(snapshot.terrain.insideBounds&&finite(snapshot.terrain.position)&&
                finite(snapshot.terrain.normal)&&snapshot.terrain.normal.y>0&&
                dense::lengthSq(snapshot.terrain.normal)>.8f,
            "playable spawn has no valid terrain support");
    require(near(snapshot.spawn.y,snapshot.terrain.position.y,1.0e-4f),
            "spawn height does not match the retained collision grid");
    require(snapshot.biome==dense::AoeBiome::Forest&&!snapshot.water.inside,
            "showcase spawn is not dry forest terrain");

    const dense::TerrainSurfaceSample outside=scene.sampleTerrain(
        scene.maximumBound()+.25f,0.0f);
    const dense::PersistentWaterSample outsideWater=scene.sampleWater(
        0.0f,scene.minimumBound()-.25f);
    require(!outside.insideBounds&&finite(outside.position)&&finite(outside.normal),
            "terrain sampler did not reject a point outside the finite view");
    require(!outsideWater.inside,
            "water sampler reported water outside the finite view");

    const std::uint64_t biomeTotal=std::uint64_t(snapshot.stats.terrainTiles);
    std::uint64_t countedBiomes=0;
    for(std::uint32_t count:snapshot.stats.biomeTileCounts)countedBiomes+=count;
    require(snapshot.stats.terrainTiles==
                dense::AoeWorldScene::tileResolution*
                dense::AoeWorldScene::tileResolution&&
                countedBiomes==biomeTotal&&
                std::isfinite(snapshot.stats.minimumHeight)&&
                std::isfinite(snapshot.stats.maximumHeight)&&
                snapshot.stats.maximumHeight>=snapshot.stats.minimumHeight,
            "generated scene tile/biome statistics are inconsistent");
    require(snapshot.stats.waterTiles>1000&&
                snapshot.stats.waterTiles<snapshot.stats.terrainTiles&&
                snapshot.stats.trees>100&&snapshot.stats.grassPatches>1000,
            "showcase scene omitted water or vegetation populations");
    for(dense::AoeBiome required:{dense::AoeBiome::RiverWater,
                                  dense::AoeBiome::Beach,
                                  dense::AoeBiome::Forest,
                                  dense::AoeBiome::Highland,
                                  dense::AoeBiome::Rock,
                                  dense::AoeBiome::Tundra,
                                  dense::AoeBiome::Snow}) {
        require(snapshot.stats.biomeTileCounts[static_cast<std::size_t>(required)]>0,
                "showcase scene lost one of its reference biomes");
    }

    dense::EnvironmentMesh mesh=scene.takeMesh();
    snapshot.mesh=countsOf(mesh);
    require(mesh.terrainIndices.size()==
                static_cast<std::size_t>(snapshot.stats.terrainTiles)*6&&
                mesh.terrainVertices.size()>=
                    static_cast<std::size_t>(dense::AoeWorldScene::gridResolution)*
                    dense::AoeWorldScene::gridResolution,
            "generated terrain is not a complete two-triangle heightfield");
    require(mesh.grassPatches.size()==snapshot.stats.grassPatches&&
                !mesh.riverVertices.empty()&&!mesh.riverIndices.empty()&&
                !mesh.detailVertices.empty()&&!mesh.detailIndices.empty(),
            "generated render mesh inventory disagrees with scene statistics");

    float minimumWaterU=1.0f,maximumWaterU=0.0f;
    for(const dense::MeshVertex& vertex:mesh.terrainVertices) {
        require(finite(vertex.position)&&finite(vertex.normal)&&
                    std::isfinite(vertex.material)&&vertex.material>=7.0f&&
                    vertex.material<7.16f,
                "generated terrain escaped material kind 7.xx");
        const int encodedBiome=static_cast<int>(std::lround(
            (vertex.material-7.0f)*100.0f));
        require(encodedBiome>=0&&encodedBiome<
                    static_cast<int>(dense::AoeBiome::Count),
                "generated terrain contains an invalid fractional biome id");
    }
    for(const dense::MeshVertex& vertex:mesh.riverVertices) {
        require(finite(vertex.position)&&finite(vertex.normal)&&
                    near(vertex.material,6.1f,2.0e-4f)&&
                    std::isfinite(vertex.u)&&vertex.u>=0&&vertex.u<=1,
                "generated water did not use material 6.1 and normalized depth U");
        minimumWaterU=std::min(minimumWaterU,vertex.u);
        maximumWaterU=std::max(maximumWaterU,vertex.u);
    }
    require(maximumWaterU>.20f&&maximumWaterU-minimumWaterU>.10f,
            "generated water U does not carry meaningful physical depth");

    // Query a triangle centroid with the greatest encoded depth. A centroid
    // avoids the ambiguous ownership of a shoreline vertex shared by dry and
    // wet tiles.
    float deepest=-1.0f;
    dense::Vec3 deepPoint{};
    float deepSurface{};
    for(std::size_t index=0;index<mesh.riverIndices.size();index+=3) {
        const dense::MeshVertex& a=mesh.riverVertices[mesh.riverIndices[index]];
        const dense::MeshVertex& b=mesh.riverVertices[mesh.riverIndices[index+1]];
        const dense::MeshVertex& c=mesh.riverVertices[mesh.riverIndices[index+2]];
        const float encoded=(a.u+b.u+c.u)/3.0f;
        if(encoded>deepest) {
            deepest=encoded;
            deepPoint=(a.position+b.position+c.position)/3.0f;
            deepSurface=(a.position.y+b.position.y+c.position.y)/3.0f;
        }
    }
    const dense::PersistentWaterSample deepWater=
        scene.sampleWater(deepPoint.x,deepPoint.z);
    const dense::TerrainSurfaceSample deepTerrain=
        scene.sampleTerrain(deepPoint.x,deepPoint.z);
    require(deepWater.inside&&deepWater.depth>.05f&&deepTerrain.insideBounds&&
                near(deepWater.surfaceHeight,deepSurface,2.0e-3f)&&
                near(deepWater.depth,
                     deepWater.surfaceHeight-deepTerrain.position.y,2.0e-4f)&&
                near(deepWater.depth,deepest*3.2f,2.0e-4f),
            "water mesh, depth sampler and terrain bed disagree");

    for(const dense::GrassPatchGpu& patch:mesh.grassPatches) {
        require(std::isfinite(patch.minX)&&std::isfinite(patch.minY)&&
                    std::isfinite(patch.minZ)&&std::isfinite(patch.maxX)&&
                    std::isfinite(patch.maxY)&&std::isfinite(patch.maxZ)&&
                    std::isfinite(patch.baseY)&&patch.minX<patch.maxX&&
                    patch.minY<patch.maxY&&patch.minZ<patch.maxZ&&
                    patch.baseY>=patch.minY&&patch.baseY<=patch.maxY&&
                    (patch.packed&255u)>0,
                "generated grass contains an invalid patch or population");
    }

    if(validateCompleteMesh) {
        validateTriangles(mesh.terrainVertices,mesh.terrainIndices,"terrain");
        validateTriangles(mesh.riverVertices,mesh.riverIndices,"water");
        validateTriangles(mesh.detailVertices,mesh.detailIndices,"detail");
    }
    return snapshot;
}

bool sameSceneSnapshot(const SceneSnapshot& left,const SceneSnapshot& right) {
    return left.stats.biomeTileCounts==right.stats.biomeTileCounts&&
           left.stats.terrainTiles==right.stats.terrainTiles&&
           left.stats.waterTiles==right.stats.waterTiles&&
           left.stats.trees==right.stats.trees&&
           left.stats.shrubs==right.stats.shrubs&&
           left.stats.rocks==right.stats.rocks&&
           left.stats.grassPatches==right.stats.grassPatches&&
           left.stats.minimumHeight==right.stats.minimumHeight&&
           left.stats.maximumHeight==right.stats.maximumHeight&&
           left.terrain.position.x==right.terrain.position.x&&
           left.terrain.position.y==right.terrain.position.y&&
           left.terrain.position.z==right.terrain.position.z&&
           left.terrain.normal.x==right.terrain.normal.x&&
           left.terrain.normal.y==right.terrain.normal.y&&
           left.terrain.normal.z==right.terrain.normal.z&&
           left.terrain.insideBounds==right.terrain.insideBounds&&
           left.water.surfaceHeight==right.water.surfaceHeight&&
           left.water.depth==right.water.depth&&
           left.water.shoreCoordinate==right.water.shoreCoordinate&&
           left.water.inside==right.water.inside&&left.biome==right.biome&&
           left.spawn.x==right.spawn.x&&left.spawn.y==right.spawn.y&&
           left.spawn.z==right.spawn.z&&sameCounts(left.mesh,right.mesh);
}

} // namespace

int main() {
    checkAuthoritativeGoldens();
    const LegacySignature legacyBefore=legacySignature();
    validateLegacySignature(legacyBefore);

    // Construct sequentially so the determinism test does not retain two large
    // render meshes at once. Scene CPU samples deliberately outlive takeMesh().
    const SceneSnapshot first=inspectShowcaseScene(true);
    const SceneSnapshot second=inspectShowcaseScene(false);
    require(sameSceneSnapshot(first,second),
            "same-seed generated scenes differ in samples, counts or mesh data");

    const LegacySignature legacyAfter=legacySignature();
    require(legacyBefore.values==legacyAfter.values&&
                legacyBefore.waterInside==legacyAfter.waterInside,
            "AI RPG generation mutated the retained Dense Trees test world");

    std::cout << "AI RPG AOE world generation checks passed\n";
}
