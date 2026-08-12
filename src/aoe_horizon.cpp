#include "aoe_horizon.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dense {
namespace {

constexpr float waterSurfaceHeight=.012f;

bool isWater(AoeBiome biome) {
    return biome==AoeBiome::DeepWater||biome==AoeBiome::ShallowWater||
           biome==AoeBiome::RiverWater||biome==AoeBiome::MangroveShallows;
}

std::uint32_t packColor(float red,float green,float blue) {
    const auto channel=[](float value) {
        return static_cast<std::uint32_t>(
            std::clamp(value,0.0f,1.0f)*255.0f+.5f);
    };
    return channel(red)|(channel(green)<<8)|(channel(blue)<<16)|0xff000000u;
}

std::uint32_t biomeColor(AoeBiome biome) {
    switch(biome) {
    case AoeBiome::DeepWater:return packColor(.14f,.18f,.15f);
    case AoeBiome::ShallowWater:return packColor(.29f,.34f,.22f);
    case AoeBiome::RiverWater:return packColor(.22f,.27f,.18f);
    case AoeBiome::MangroveShallows:return packColor(.26f,.31f,.16f);
    case AoeBiome::Beach:return packColor(.66f,.57f,.36f);
    case AoeBiome::Grassland:return packColor(.31f,.49f,.13f);
    case AoeBiome::DryGrass:return packColor(.54f,.48f,.20f);
    case AoeBiome::Mud:return packColor(.24f,.18f,.10f);
    case AoeBiome::Forest:return packColor(.16f,.31f,.09f);
    case AoeBiome::JungleFloor:return packColor(.10f,.25f,.075f);
    case AoeBiome::Highland:return packColor(.34f,.40f,.20f);
    case AoeBiome::Rock:return packColor(.36f,.36f,.34f);
    case AoeBiome::Tundra:return packColor(.43f,.47f,.36f);
    case AoeBiome::Snow:return packColor(.82f,.87f,.89f);
    case AoeBiome::DesertSand:return packColor(.68f,.48f,.23f);
    case AoeBiome::CrackedEarth:return packColor(.39f,.25f,.12f);
    default:return packColor(.4f,.4f,.4f);
    }
}

float nominalWaterDepth(std::int64_t seed,AoeBiome biome,int x,int z) {
    const float base=AoeWorldGenerator::baseElevationAt(seed,x,z);
    switch(biome) {
    case AoeBiome::DeepWater:
        return std::clamp(1.9f+std::max(0.0f,-base)*.55f,1.9f,3.2f);
    case AoeBiome::ShallowWater:
        return std::clamp(.42f+std::max(0.0f,.9f-base)*.55f,.42f,1.15f);
    case AoeBiome::RiverWater: {
        const AoeHydrologySample water=AoeWorldGenerator::hydrologyAt(
            seed,static_cast<float>(x),static_cast<float>(z));
        return std::clamp(.72f+water.river*1.75f+water.lake*.55f,.72f,2.7f);
    }
    case AoeBiome::MangroveShallows:return .34f;
    default:return 0.0f;
    }
}

struct RingData {
    AoeHorizonRing ring{};
    int cellsPerAxis{};
    int verticesPerAxis{};
    std::vector<float> heights;
    std::vector<Vec3> normals;
    std::vector<std::uint32_t> colors;
    std::vector<float> waterCoverage;
    std::vector<float> waterDepths;
    std::vector<AoeBiome> cellBiomes;

    [[nodiscard]] std::size_t vertexIndex(int x,int z) const {
        return static_cast<std::size_t>(z)*verticesPerAxis+x;
    }
    [[nodiscard]] std::size_t cellIndex(int x,int z) const {
        return static_cast<std::size_t>(z)*cellsPerAxis+x;
    }
    [[nodiscard]] int localCoordinate(int index) const {
        return -ring.outerExtent+index*ring.cellSize;
    }
    [[nodiscard]] bool includedCell(int x,int z) const {
        const int localX=localCoordinate(x),localZ=localCoordinate(z);
        return !(localX>=-ring.innerExtent&&
                 localX+ring.cellSize<=ring.innerExtent&&
                 localZ>=-ring.innerExtent&&
                 localZ+ring.cellSize<=ring.innerExtent);
    }
};

struct ColorChannels {
    float red{},green{},blue{};
};

ColorChannels colorChannels(std::uint32_t color) {
    return {static_cast<float>(color&255u)/255.0f,
            static_cast<float>((color>>8)&255u)/255.0f,
            static_cast<float>((color>>16)&255u)/255.0f};
}

ColorChannels mixColor(ColorChannels a,ColorChannels b,float amount) {
    return {std::lerp(a.red,b.red,amount),std::lerp(a.green,b.green,amount),
            std::lerp(a.blue,b.blue,amount)};
}

void validateCenter(int center) {
    constexpr int margin=AoeHorizonBuilder::rings.back().outerExtent+1024;
    if(center<std::numeric_limits<int>::min()+margin||
       center>std::numeric_limits<int>::max()-margin)
        throw std::out_of_range("AOE horizon source center is outside the supported integer range");
}

RingData sampleRing(std::int64_t seed,int centerX,int centerZ,
                    AoeHorizonRing ring) {
    RingData data;data.ring=ring;
    data.cellsPerAxis=ring.outerExtent*2/ring.cellSize;
    data.verticesPerAxis=data.cellsPerAxis+1;
    const std::size_t cellCount=static_cast<std::size_t>(data.cellsPerAxis)*
                                data.cellsPerAxis;
    const std::size_t vertexCount=static_cast<std::size_t>(data.verticesPerAxis)*
                                  data.verticesPerAxis;
    data.cellBiomes.resize(cellCount);
    data.heights.resize(vertexCount);
    data.normals.resize(vertexCount,{0,1,0});
    data.colors.resize(vertexCount);
    data.waterCoverage.resize(vertexCount);
    data.waterDepths.resize(vertexCount);

    // Geometry resolution and hydrology resolution are deliberately separate.
    // The previous 512 m outer cells were directly visible as disconnected
    // rectangular slabs. 64/128 m cells now provide a sub-pixel horizon mesh,
    // while the expensive hydrology-bearing oracle retains its former sparse
    // 2/8 km world spacing (five controls per axis in each far ring). Cheap
    // base-elevation samples restore relief and coastline detail between them.
    const int oracleWorldSpacing=ring.cellSize==32?32:
        (ring.outerExtent==4096?2048:8192);
    const int oracleStride=std::max(1,oracleWorldSpacing/ring.cellSize);
    const int controlsPerAxis=data.cellsPerAxis/oracleStride+1;
    std::vector<float> controlHeights(
        static_cast<std::size_t>(controlsPerAxis)*controlsPerAxis);
    std::vector<float> controlBase(controlHeights.size());
    std::vector<float> rawWater(
        static_cast<std::size_t>(controlsPerAxis)*controlsPerAxis);
    std::vector<float> controlWater(rawWater.size());
    std::vector<float> rawDepth(rawWater.size());
    std::vector<float> controlDepth(rawWater.size());
    std::vector<ColorChannels> controlColors(rawWater.size());
    std::vector<AoeBiome> controlBiomes(rawWater.size());
    const auto controlIndex=[controlsPerAxis](int x,int z) {
        return static_cast<std::size_t>(z)*controlsPerAxis+x;
    };
    for(int z=0;z<controlsPerAxis;++z) {
        for(int x=0;x<controlsPerAxis;++x) {
            const int vertexX=std::min(x*oracleStride,data.cellsPerAxis);
            const int vertexZ=std::min(z*oracleStride,data.cellsPerAxis);
            const int worldX=centerX+data.localCoordinate(vertexX);
            const int worldZ=centerZ+data.localCoordinate(vertexZ);
            const AoeBiome biome=AoeWorldGenerator::biomeAt(
                seed,worldX,worldZ);
            const std::size_t index=controlIndex(x,z);
            controlBiomes[index]=biome;
            controlColors[index]=colorChannels(biomeColor(biome));
            rawWater[index]=isWater(biome)?1.0f:0.0f;
            rawDepth[index]=isWater(biome)?
                nominalWaterDepth(seed,biome,worldX,worldZ):0.0f;
            controlBase[index]=AoeWorldGenerator::baseElevationAt(
                seed,worldX,worldZ);
            controlHeights[controlIndex(x,z)]=
                AoeWorldGenerator::renderedHeightAt(
                    seed,static_cast<float>(worldX),static_cast<float>(worldZ))*
                ring.heightScale;
        }
    }
    // A weighted cross filter turns the binary water oracle into a stable
    // coverage field. Its asymmetric 6:1 weights deliberately put the .5
    // contour inside cells rather than on kilometre-aligned control edges.
    for(int z=0;z<controlsPerAxis;++z) {
        for(int x=0;x<controlsPerAxis;++x) {
            float coverage=rawWater[controlIndex(x,z)]*6.0f;
            float depth=rawDepth[controlIndex(x,z)]*6.0f;
            float weight=6.0f;
            constexpr std::array<std::pair<int,int>,4> offsets{{
                {-1,0},{1,0},{0,-1},{0,1}}};
            for(const auto [offsetX,offsetZ]:offsets) {
                const int neighborX=x+offsetX,neighborZ=z+offsetZ;
                if(neighborX<0||neighborZ<0||
                   neighborX>=controlsPerAxis||neighborZ>=controlsPerAxis)
                    continue;
                coverage+=rawWater[controlIndex(neighborX,neighborZ)];
                depth+=rawDepth[controlIndex(neighborX,neighborZ)];
                weight+=1.0f;
            }
            const std::size_t index=controlIndex(x,z);
            controlWater[index]=coverage/weight;
            controlDepth[index]=depth/weight;
        }
    }

    for(int z=0;z<data.cellsPerAxis;++z) {
        for(int x=0;x<data.cellsPerAxis;++x) {
            const int controlX=std::clamp(static_cast<int>(std::lround(
                static_cast<float>(x)+.5f))/oracleStride,0,controlsPerAxis-1);
            const int controlZ=std::clamp(static_cast<int>(std::lround(
                static_cast<float>(z)+.5f))/oracleStride,0,controlsPerAxis-1);
            data.cellBiomes[data.cellIndex(x,z)]=
                controlBiomes[controlIndex(controlX,controlZ)];
        }
    }
    for(int z=0;z<data.verticesPerAxis;++z) {
        for(int x=0;x<data.verticesPerAxis;++x) {
            const int controlX=std::min(x/oracleStride,controlsPerAxis-2);
            const int controlZ=std::min(z/oracleStride,controlsPerAxis-2);
            const float tx=static_cast<float>(x-controlX*oracleStride)/
                           oracleStride;
            const float tz=static_cast<float>(z-controlZ*oracleStride)/
                           oracleStride;
            const float north=std::lerp(
                controlHeights[controlIndex(controlX,controlZ)],
                controlHeights[controlIndex(controlX+1,controlZ)],tx);
            const float south=std::lerp(
                controlHeights[controlIndex(controlX,controlZ+1)],
                controlHeights[controlIndex(controlX+1,controlZ+1)],tx);
            float height=std::lerp(north,south,tz);
            const auto bilerpScalar=[&](const std::vector<float>& values) {
                const float top=std::lerp(values[controlIndex(controlX,controlZ)],
                    values[controlIndex(controlX+1,controlZ)],tx);
                const float bottom=std::lerp(
                    values[controlIndex(controlX,controlZ+1)],
                    values[controlIndex(controlX+1,controlZ+1)],tx);
                return std::lerp(top,bottom,tz);
            };
            const int worldX=centerX+data.localCoordinate(x);
            const int worldZ=centerZ+data.localCoordinate(z);
            const float base=AoeWorldGenerator::baseElevationAt(
                seed,worldX,worldZ);
            // Preserve the sparse authoritative rendered-height profile, then
            // restore bounded high-frequency continental relief from the cheap
            // base field. This stops each 8 km control span reading as one
            // planar shelf without causing additional hydrology generation.
            height+=std::clamp(base-bilerpScalar(controlBase),-4.0f,4.0f)*
                    ring.heightScale;
            // The source classifies ocean below base elevation .9. Feathering
            // that threshold over one elevation unit creates a contourable
            // far shoreline without requiring another hydrology region.
            const float oceanCoverage=std::clamp((1.35f-base)/.9f,0.0f,1.0f);
            float coverage=std::clamp(std::max(
                oceanCoverage,bilerpScalar(controlWater)),0.0f,1.0f);
            // At the outer visual shell the generated world must close into a
            // continuous ocean horizon. Sparse continental islands there are
            // only a few pixels high and read as disconnected rectangular
            // plates; fade them beneath water between 8 km and the 16 km cap.
            // The authoritative near world and the first two LOD rings remain
            // unchanged, while source coordinates continue through streaming.
            const float radialDistance=std::hypot(
                static_cast<float>(data.localCoordinate(x)),
                static_cast<float>(data.localCoordinate(z)));
            const float closureT=std::clamp(
                (radialDistance-7800.0f)/(13800.0f-7800.0f),0.0f,1.0f);
            const float oceanClosure=closureT*closureT*(3.0f-2.0f*closureT);
            coverage=std::lerp(coverage,1.0f,oceanClosure);
            const float premultipliedDepth=bilerpScalar(controlDepth);
            float depth=coverage>1.0e-4f?
                premultipliedDepth/coverage:0.0f;
            if(oceanCoverage>.5f)
                depth=std::max(depth,std::clamp(
                    .42f+std::max(0.0f,.9f-base)*.55f,.42f,1.9f));
            const float submerged=std::clamp((coverage-.35f)/.30f,0.0f,1.0f);
            const float bed=std::min(height,waterSurfaceHeight-depth);
            height=std::lerp(height,bed,submerged);
            const ColorChannels northColor=mixColor(
                controlColors[controlIndex(controlX,controlZ)],
                controlColors[controlIndex(controlX+1,controlZ)],tx);
            const ColorChannels southColor=mixColor(
                controlColors[controlIndex(controlX,controlZ+1)],
                controlColors[controlIndex(controlX+1,controlZ+1)],tx);
            const ColorChannels color=mixColor(northColor,southColor,tz);
            const std::size_t index=data.vertexIndex(x,z);
            data.heights[index]=height;
            data.colors[index]=packColor(color.red,color.green,color.blue);
            data.waterCoverage[index]=coverage;
            data.waterDepths[index]=depth;
        }
    }
    return data;
}

float bilinearHeight(const RingData& data,float localX,float localZ) {
    const float gridX=(localX+data.ring.outerExtent)/data.ring.cellSize;
    const float gridZ=(localZ+data.ring.outerExtent)/data.ring.cellSize;
    const int x0=std::clamp(static_cast<int>(std::floor(gridX)),
                            0,data.cellsPerAxis-1);
    const int z0=std::clamp(static_cast<int>(std::floor(gridZ)),
                            0,data.cellsPerAxis-1);
    const int x1=x0+1,z1=z0+1;
    const float tx=std::clamp(gridX-x0,0.0f,1.0f);
    const float tz=std::clamp(gridZ-z0,0.0f,1.0f);
    const float north=std::lerp(data.heights[data.vertexIndex(x0,z0)],
                                data.heights[data.vertexIndex(x1,z0)],tx);
    const float south=std::lerp(data.heights[data.vertexIndex(x0,z1)],
                                data.heights[data.vertexIndex(x1,z1)],tx);
    return std::lerp(north,south,tz);
}

void constrainSeams(std::array<RingData,3>& data,
                    const AoeNearTerrainSampler& nearTerrain) {
    for(std::size_t ringIndex=0;ringIndex+1<data.size();++ringIndex) {
        RingData& fine=data[ringIndex];
        const RingData& coarse=data[ringIndex+1];
        const int edge=fine.ring.outerExtent;
        for(int z=0;z<fine.verticesPerAxis;++z) {
            for(int x=0;x<fine.verticesPerAxis;++x) {
                const int localX=fine.localCoordinate(x);
                const int localZ=fine.localCoordinate(z);
                if(std::abs(localX)!=edge&&std::abs(localZ)!=edge)continue;
                fine.heights[fine.vertexIndex(x,z)]=bilinearHeight(
                    coarse,static_cast<float>(localX),static_cast<float>(localZ));
            }
        }
    }
    if(!nearTerrain)return;
    RingData& nearRing=data.front();
    const int edge=nearRing.ring.innerExtent;
    for(int z=0;z<nearRing.verticesPerAxis;++z) {
        for(int x=0;x<nearRing.verticesPerAxis;++x) {
            const int localX=nearRing.localCoordinate(x);
            const int localZ=nearRing.localCoordinate(z);
            const bool innerBoundary=(std::abs(localX)==edge&&
                                      std::abs(localZ)<=edge)||
                                     (std::abs(localZ)==edge&&
                                      std::abs(localX)<=edge);
            if(!innerBoundary)continue;
            const TerrainSurfaceSample sample=nearTerrain(
                static_cast<float>(localX),static_cast<float>(localZ));
            if(!std::isfinite(sample.position.y))continue;
            nearRing.heights[nearRing.vertexIndex(x,z)]=sample.position.y;
            if(std::isfinite(sample.normal.x)&&std::isfinite(sample.normal.y)&&
               std::isfinite(sample.normal.z)&&lengthSq(sample.normal)>1.0e-8f)
                nearRing.normals[nearRing.vertexIndex(x,z)]=normalize(sample.normal);
        }
    }
}

void calculateNormals(std::array<RingData,3>& data,
                      const AoeNearTerrainSampler& nearTerrain) {
    for(std::size_t ringIndex=0;ringIndex<data.size();++ringIndex) {
        RingData& ring=data[ringIndex];
        for(int z=0;z<ring.verticesPerAxis;++z) {
            for(int x=0;x<ring.verticesPerAxis;++x) {
                const int localX=ring.localCoordinate(x);
                const int localZ=ring.localCoordinate(z);
                if(ringIndex==0&&nearTerrain&&
                   (((std::abs(localX)==ring.ring.innerExtent)&&
                     std::abs(localZ)<=ring.ring.innerExtent)||
                    ((std::abs(localZ)==ring.ring.innerExtent)&&
                     std::abs(localX)<=ring.ring.innerExtent)))
                    continue;
                const int left=std::max(0,x-1),right=std::min(
                    ring.verticesPerAxis-1,x+1);
                const int north=std::max(0,z-1),south=std::min(
                    ring.verticesPerAxis-1,z+1);
                const float deltaX=ring.heights[ring.vertexIndex(right,z)]-
                                   ring.heights[ring.vertexIndex(left,z)];
                const float deltaZ=ring.heights[ring.vertexIndex(x,south)]-
                                   ring.heights[ring.vertexIndex(x,north)];
                const float runX=static_cast<float>((right-left)*ring.ring.cellSize);
                const float runZ=static_cast<float>((south-north)*ring.ring.cellSize);
                ring.normals[ring.vertexIndex(x,z)]=normalize(
                    Vec3{-deltaX/runX,1.0f,-deltaZ/runZ});
            }
        }
    }
    // Match normals at common coarse vertices. Fine-only edge vertices retain
    // their locally filtered normal while sharing the exact coarse edge line.
    for(std::size_t ringIndex=0;ringIndex+1<data.size();++ringIndex) {
        RingData& fine=data[ringIndex];
        const RingData& coarse=data[ringIndex+1];
        const int edge=fine.ring.outerExtent;
        for(int z=0;z<fine.verticesPerAxis;++z) {
            for(int x=0;x<fine.verticesPerAxis;++x) {
                const int localX=fine.localCoordinate(x);
                const int localZ=fine.localCoordinate(z);
                if((std::abs(localX)!=edge&&std::abs(localZ)!=edge)||
                   (localX+coarse.ring.outerExtent)%coarse.ring.cellSize!=0||
                   (localZ+coarse.ring.outerExtent)%coarse.ring.cellSize!=0)
                    continue;
                const int coarseX=(localX+coarse.ring.outerExtent)/
                                  coarse.ring.cellSize;
                const int coarseZ=(localZ+coarse.ring.outerExtent)/
                                  coarse.ring.cellSize;
                fine.normals[fine.vertexIndex(x,z)]=
                    coarse.normals[coarse.vertexIndex(coarseX,coarseZ)];
            }
        }
    }
}

void ensureIndexCapacity(std::size_t current,std::size_t addition) {
    if(current>std::numeric_limits<std::uint32_t>::max()-addition)
        throw std::length_error("AOE horizon exceeds the uint32 mesh index range");
}

struct WaterClipVertex {
    Vec3 position{};
    float coverage{};
    float depth{};
};

WaterClipVertex interpolateWater(WaterClipVertex a,WaterClipVertex b,
                                 float amount) {
    return {a.position+(b.position-a.position)*amount,
            std::lerp(a.coverage,b.coverage,amount),
            std::lerp(a.depth,b.depth,amount)};
}

bool appendWaterTriangle(EnvironmentMesh& mesh,
                         std::array<WaterClipVertex,3> triangle) {
    constexpr float threshold=.5f;
    std::array<WaterClipVertex,5> clipped{};
    int clippedCount=0;
    WaterClipVertex previous=triangle.back();
    bool previousInside=previous.coverage>=threshold;
    for(const WaterClipVertex current:triangle) {
        const bool currentInside=current.coverage>=threshold;
        if(currentInside!=previousInside) {
            const float denominator=current.coverage-previous.coverage;
            const float amount=std::abs(denominator)>1.0e-6f?
                (threshold-previous.coverage)/denominator:.5f;
            clipped[clippedCount++]=interpolateWater(
                previous,current,std::clamp(amount,0.0f,1.0f));
        }
        if(currentInside)clipped[clippedCount++]=current;
        previous=current;previousInside=currentInside;
    }
    int uniqueCount=0;
    for(int index=0;index<clippedCount;++index) {
        if(uniqueCount>0&&lengthSq(clipped[index].position-
                                  clipped[uniqueCount-1].position)<1.0e-6f)
            continue;
        clipped[uniqueCount++]=clipped[index];
    }
    clippedCount=uniqueCount;
    if(clippedCount>2&&lengthSq(clipped.front().position-
                               clipped[clippedCount-1].position)<1.0e-6f)
        --clippedCount;
    if(clippedCount<3)return false;
    ensureIndexCapacity(mesh.riverVertices.size(),
                        static_cast<std::size_t>(clippedCount));
    const std::uint32_t first=static_cast<std::uint32_t>(
        mesh.riverVertices.size());
    for(int index=0;index<clippedCount;++index) {
        const WaterClipVertex& source=clipped[index];
        mesh.riverVertices.push_back({
            {source.position.x,waterSurfaceHeight,source.position.z},
            {0,1,0},packColor(.34f,.55f,.64f),6.1f,
            std::clamp(source.depth/3.2f,0.0f,1.0f),0});
    }
    bool emitted=false;
    for(int index=1;index+1<clippedCount;++index) {
        if(lengthSq(cross(clipped[index+1].position-clipped[0].position,
                          clipped[index].position-clipped[0].position))<1.0e-6f)
            continue;
        mesh.riverIndices.insert(mesh.riverIndices.end(),{
            first,first+static_cast<std::uint32_t>(index+1),
            first+static_cast<std::uint32_t>(index)});
        emitted=true;
    }
    return emitted;
}

void appendRing(EnvironmentMesh& mesh,const RingData& data,std::size_t ringIndex,
                const AoeNearTerrainSampler& nearTerrain,
                AoeHorizonStats& stats) {
    constexpr float waterCoverageThreshold=.5f;
    for(int z=0;z<data.cellsPerAxis;++z) {
        for(int x=0;x<data.cellsPerAxis;++x) {
            if(!data.includedCell(x,z))continue;
            const std::size_t cellIndex=data.cellIndex(x,z);
            const AoeBiome biome=data.cellBiomes[cellIndex];
            // Far-field colour is already continuously blended. Keeping the
            // categorical biome id there would reintroduce large square
            // changes through the shader's roughness/detail branch.
            const float material=data.ring.cellSize==32?
                7.0f+static_cast<float>(biome)*.01f:7.05f;
            ensureIndexCapacity(mesh.terrainVertices.size(),4);
            const std::uint32_t first=static_cast<std::uint32_t>(
                mesh.terrainVertices.size());
            const auto vertex=[&](int vertexX,int vertexZ) {
                const std::size_t index=data.vertexIndex(vertexX,vertexZ);
                const int localX=data.localCoordinate(vertexX);
                const int localZ=data.localCoordinate(vertexZ);
                float height=data.heights[index];
                Vec3 normal=data.normals[index];
                if(ringIndex==0&&nearTerrain&&
                   (((std::abs(localX)==data.ring.innerExtent)&&
                     std::abs(localZ)<=data.ring.innerExtent)||
                    ((std::abs(localZ)==data.ring.innerExtent)&&
                     std::abs(localX)<=data.ring.innerExtent))) {
                    const TerrainSurfaceSample sample=nearTerrain(
                        static_cast<float>(localX),static_cast<float>(localZ));
                    if(std::isfinite(sample.position.y))height=sample.position.y;
                    if(std::isfinite(sample.normal.x)&&
                       std::isfinite(sample.normal.y)&&
                       std::isfinite(sample.normal.z)&&
                       lengthSq(sample.normal)>1.0e-8f)normal=normalize(sample.normal);
                }
                return MeshVertex{{static_cast<float>(data.localCoordinate(vertexX)),
                                   height,
                                   static_cast<float>(data.localCoordinate(vertexZ))},
                                  normal,data.colors[index],material,0,0};
            };
            mesh.terrainVertices.push_back(vertex(x,z));
            mesh.terrainVertices.push_back(vertex(x+1,z));
            mesh.terrainVertices.push_back(vertex(x+1,z+1));
            mesh.terrainVertices.push_back(vertex(x,z+1));
            if(((x+z)&1)==0)mesh.terrainIndices.insert(mesh.terrainIndices.end(),
                {first,first+3,first+2,first,first+2,first+1});
            else mesh.terrainIndices.insert(mesh.terrainIndices.end(),
                {first,first+3,first+1,first+1,first+3,first+2});
            ++stats.terrainCells[ringIndex];
            const std::array<std::pair<int,int>,4> corners{{
                {x,z},{x+1,z},{x+1,z+1},{x,z+1}}};
            std::array<WaterClipVertex,4> water{};
            float minimumCoverage=1.0f,maximumCoverage=0.0f;
            std::uint32_t firstColor=0;
            bool colorsDiffer=false;
            for(std::size_t corner=0;corner<corners.size();++corner) {
                const auto [vertexX,vertexZ]=corners[corner];
                const std::size_t vertexIndex=data.vertexIndex(vertexX,vertexZ);
                water[corner]={{
                    static_cast<float>(data.localCoordinate(vertexX)),
                    data.heights[vertexIndex],
                    static_cast<float>(data.localCoordinate(vertexZ))},
                    data.waterCoverage[vertexIndex],
                    data.waterDepths[vertexIndex]};
                minimumCoverage=std::min(minimumCoverage,water[corner].coverage);
                maximumCoverage=std::max(maximumCoverage,water[corner].coverage);
                if(corner==0)firstColor=data.colors[vertexIndex];
                else colorsDiffer|=data.colors[vertexIndex]!=firstColor;
            }
            if(colorsDiffer)++stats.blendedTerrainCells[ringIndex];
            if(minimumCoverage<waterCoverageThreshold&&
               maximumCoverage>=waterCoverageThreshold)
                ++stats.waterBoundaryCells[ringIndex];
            bool emitted=false;
            if(((x+z)&1)==0) {
                emitted|=appendWaterTriangle(mesh,{water[0],water[3],water[2]});
                emitted|=appendWaterTriangle(mesh,{water[0],water[2],water[1]});
            } else {
                emitted|=appendWaterTriangle(mesh,{water[0],water[3],water[1]});
                emitted|=appendWaterTriangle(mesh,{water[1],water[3],water[2]});
            }
            if(emitted)++stats.waterCells[ringIndex];
        }
    }
}

} // namespace

AoeHorizonStats AoeHorizonBuilder::append(
    EnvironmentMesh& mesh,std::int64_t seed,int sourceCenterX,int sourceCenterZ,
    AoeNearTerrainSampler nearTerrain) {
    validateCenter(sourceCenterX);validateCenter(sourceCenterZ);
    const std::size_t terrainVertexStart=mesh.terrainVertices.size();
    const std::size_t terrainIndexStart=mesh.terrainIndices.size();
    const std::size_t waterVertexStart=mesh.riverVertices.size();
    const std::size_t waterIndexStart=mesh.riverIndices.size();

    std::array<RingData,rings.size()> data;
    for(std::size_t index=0;index<rings.size();++index)
        data[index]=sampleRing(seed,sourceCenterX,sourceCenterZ,rings[index]);
    constrainSeams(data,nearTerrain);
    calculateNormals(data,nearTerrain);

    AoeHorizonStats stats;stats.outerExtent=static_cast<float>(
        rings.back().outerExtent);
    for(std::size_t index=0;index<data.size();++index)
        appendRing(mesh,data[index],index,nearTerrain,stats);
    stats.terrainVertices=static_cast<std::uint32_t>(
        mesh.terrainVertices.size()-terrainVertexStart);
    stats.terrainIndices=static_cast<std::uint32_t>(
        mesh.terrainIndices.size()-terrainIndexStart);
    stats.waterVertices=static_cast<std::uint32_t>(
        mesh.riverVertices.size()-waterVertexStart);
    stats.waterIndices=static_cast<std::uint32_t>(
        mesh.riverIndices.size()-waterIndexStart);
    for(const MeshVertex& vertex:mesh.terrainVertices) {
        mesh.minimumHeight=std::min(mesh.minimumHeight,vertex.position.y);
        mesh.maximumHeight=std::max(mesh.maximumHeight,vertex.position.y);
    }
    return stats;
}

} // namespace dense
