#include "aoe_world.hpp"

// This file is a C++ 3D port of the deterministic world-generation
// algorithms in the MIT-licensed C:\AI RPG AOE project at revision 3e76dcd.
// Copyright (c) 2026 SamG-Coder. No Age of Empires game assets are used.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace dense {
namespace {

constexpr int islandCellSize=192;
constexpr int hydrologyCellSize=8;
constexpr int hydrologyRegionCells=64;
constexpr int hydrologyRegionSpan=hydrologyCellSize*hydrologyRegionCells;
constexpr int hydrologyHaloCells=24;
constexpr int hydrologyGridSize=hydrologyRegionCells+hydrologyHaloCells*2;
constexpr int hydrologyBlendTiles=hydrologyHaloCells*hydrologyCellSize/2;

float mix(float a,float b,float amount) {
    return a+(b-a)*amount;
}

float smoothStep(float edge0,float edge1,float value) {
    const float t=clamp((value-edge0)/(edge1-edge0),0.0f,1.0f);
    return t*t*(3.0f-2.0f*t);
}

int floorDiv(int value,int divisor) {
    const int quotient=value/divisor;
    return value<0&&value%divisor!=0?quotient-1:quotient;
}

std::uint64_t seedBits(std::int64_t seed) {
    return static_cast<std::uint64_t>(seed);
}

float unitHash(std::uint64_t seed,int x,int z,int salt) {
    std::uint64_t value=seed^
        (static_cast<std::uint64_t>(static_cast<std::int64_t>(x))*
         0x9e3779b185ebca87ull)^
        (static_cast<std::uint64_t>(static_cast<std::int64_t>(z))*
         0xc2b2ae3d27d4eb4full)^
        static_cast<std::uint32_t>(salt);
    value^=value>>30;
    value*=0xbf58476d1ce4e5b9ull;
    value^=value>>27;
    value*=0x94d049bb133111ebull;
    value^=value>>31;
    return static_cast<float>(value>>40)*(1.0f/16777216.0f);
}

float valueNoise(std::uint64_t seed,float x,float z) {
    const int x0=static_cast<int>(std::floor(x));
    const int z0=static_cast<int>(std::floor(z));
    float fx=x-static_cast<float>(x0);
    float fz=z-static_cast<float>(z0);
    fx=fx*fx*(3.0f-2.0f*fx);
    fz=fz*fz*(3.0f-2.0f*fz);
    const float a=unitHash(seed,x0,z0,0);
    const float b=unitHash(seed,x0+1,z0,0);
    const float c=unitHash(seed,x0,z0+1,0);
    const float d=unitHash(seed,x0+1,z0+1,0);
    return mix(mix(a,b,fx),mix(c,d,fx),fz);
}

float fractalNoise(std::uint64_t seed,float x,float z,int octaves) {
    float value=0.0f,amplitude=1.0f,total=0.0f;
    for(int octave=0;octave<octaves;++octave) {
        value+=valueNoise(seed+static_cast<std::uint64_t>(octave*1013),x,z)*
               amplitude;
        total+=amplitude;
        amplitude*=.5f;x*=2.03f;z*=2.03f;
    }
    return value/total*2.0f-1.0f;
}

std::pair<float,float> mountainProfile(std::uint64_t seed,int x,int z) {
    constexpr int rangeCellSize=768;
    const float warpedX=static_cast<float>(x)+
        fractalNoise(seed^0x3c6ef372fe94f82bull,x/310.0f,z/310.0f,3)*42.0f;
    const float warpedZ=static_cast<float>(z)+
        fractalNoise(seed^0x428a2f98d728ae22ull,x/310.0f,z/310.0f,3)*42.0f;
    const int cellX=floorDiv(x,rangeCellSize);
    const int cellZ=floorDiv(z,rangeCellSize);
    float ramp=0.0f,core=0.0f;
    for(int cz=cellZ-1;cz<=cellZ+1;++cz) {
        for(int cx=cellX-1;cx<=cellX+1;++cx) {
            const float centerX=(static_cast<float>(cx)+.5f+
                (unitHash(seed,cx,cz,401)-.5f)*.34f)*rangeCellSize;
            const float centerZ=(static_cast<float>(cz)+.5f+
                (unitHash(seed,cx,cz,409)-.5f)*.34f)*rangeCellSize;
            const float angle=unitHash(seed,cx,cz,419)*pi;
            const float halfLength=300.0f+unitHash(seed,cx,cz,421)*250.0f;
            const float halfWidth=125.0f+unitHash(seed,cx,cz,431)*105.0f;
            const float axisX=std::cos(angle),axisZ=std::sin(angle);
            const float relativeX=warpedX-centerX,relativeZ=warpedZ-centerZ;
            const float along=clamp(relativeX*axisX+relativeZ*axisZ,
                                    -halfLength,halfLength);
            const float nearestX=centerX+axisX*along;
            const float nearestZ=centerZ+axisZ*along;
            const float dx=warpedX-nearestX,dz=warpedZ-nearestZ;
            const float normalized=std::sqrt(dx*dx+dz*dz)/halfWidth;
            ramp=std::max(ramp,1.0f-smoothStep(.15f,1.0f,normalized));
            core=std::max(core,1.0f-smoothStep(.05f,.34f,normalized));
        }
    }
    return {ramp,core};
}

float baseElevation(std::uint64_t seed,int x,int z) {
    const float continental=fractalNoise(
        seed^0x6a09e667f3bcc909ull,x/720.0f,z/720.0f,4);
    const float continentalDetail=fractalNoise(
        seed^0xbb67ae8584caa73bull,x/280.0f,z/280.0f,3);
    const float continentHeight=(continental+continentalDetail*.22f+.12f)*5.4f;

    const int cellX=floorDiv(x,islandCellSize);
    const int cellZ=floorDiv(z,islandCellSize);
    float island=-1.0f;
    for(int cz=cellZ-1;cz<=cellZ+1;++cz) {
        for(int cx=cellX-1;cx<=cellX+1;++cx) {
            const float centerX=(static_cast<float>(cx)+.18f+
                unitHash(seed,cx,cz,11)*.64f)*islandCellSize;
            const float centerZ=(static_cast<float>(cz)+.18f+
                unitHash(seed,cx,cz,17)*.64f)*islandCellSize;
            const float radiusX=islandCellSize*(.25f+
                unitHash(seed,cx,cz,23)*.20f);
            const float radiusZ=islandCellSize*(.23f+
                unitHash(seed,cx,cz,29)*.19f);
            const float dx=(static_cast<float>(x)-centerX)/radiusX;
            const float dz=(static_cast<float>(z)-centerZ)/radiusZ;
            const float distance=std::sqrt(dx*dx+dz*dz);
            const float warp=fractalNoise(seed^0x243f6a8885a308d3ull,
                                           x/48.0f,z/48.0f,3)*.28f;
            island=std::max(island,1.0f-distance+warp);
        }
    }
    const float islandHeight=(island-.08f)*7.2f;
    const auto [rangeRamp,mountainCore]=mountainProfile(seed,x,z);
    const float mountainGate=clamp((continental+.15f)*1.7f,0.0f,1.0f);
    const float passNoise=fractalNoise(seed^0x428a2f98d728ae22ull,
                                       x/115.0f,z/115.0f,2);
    const float passCut=clamp((passNoise-.42f)*2.3f,0.0f,.72f);
    const float mountains=mountainCore*mountainGate*12.5f*(1.0f-passCut);
    const float foothills=rangeRamp*mountainGate*6.0f*(1.0f-passCut*.55f);
    const float hillNoise=std::max(0.0f,fractalNoise(
        seed^0x7137449123ef65cdull,x/92.0f,z/92.0f,3));
    const float hills=hillNoise*hillNoise*clamp((continental+.3f)*1.25f,
                                                0.0f,1.0f)*2.6f;
    const float detail=fractalNoise(seed^0x13198a2e03707344ull,
                                    x/22.0f,z/22.0f,3)*.8f;
    return std::max(continentHeight,islandHeight)+mountains+foothills+hills+detail;
}

float rainfall(std::uint64_t seed,int x,int z) {
    const float broad=fractalNoise(seed^0x5deece66dull,x/430.0f,z/430.0f,4);
    const float detail=fractalNoise(seed^0xa54ff53a5f1d36f1ull,
                                    x/105.0f,z/105.0f,2);
    const float windAngle=unitHash(seed,0,0,557)*pi*2.0f;
    const float windX=std::cos(windAngle),windZ=std::sin(windAngle);
    const float localElevation=baseElevation(seed,x,z);
    const float upwindNear=baseElevation(seed,
        static_cast<int>(x-windX*72.0f),static_cast<int>(z-windZ*72.0f));
    const float upwindFar=baseElevation(seed,
        static_cast<int>(x-windX*152.0f),static_cast<int>(z-windZ*152.0f));
    const float barrier=std::max(upwindNear,upwindFar)-localElevation;
    const float rainShadow=clamp(barrier*.045f,0.0f,.48f);
    const float oceanMoisture=upwindFar<.5f?.16f:0.0f;
    return clamp(.65f+broad*.28f+detail*.12f+oceanMoisture-rainShadow,
                 .10f,1.2f);
}

struct HydrologyRegion {
    int originX{},originZ{};
    std::vector<float> river,lake,flow;

    AoeHydrologySample sample(float worldX,float worldZ) const {
        const float x=clamp((worldX-originX)/hydrologyCellSize,0.0f,
                            hydrologyGridSize-1.001f);
        const float z=clamp((worldZ-originZ)/hydrologyCellSize,0.0f,
                            hydrologyGridSize-1.001f);
        const int x0=static_cast<int>(x),z0=static_cast<int>(z);
        const int x1=std::min(x0+1,hydrologyGridSize-1);
        const int z1=std::min(z0+1,hydrologyGridSize-1);
        const float tx=x-x0,tz=z-z0;
        const auto bilinear=[&](const std::vector<float>& values) {
            const float north=mix(values[static_cast<std::size_t>(z0)*
                                         hydrologyGridSize+x0],
                                  values[static_cast<std::size_t>(z0)*
                                         hydrologyGridSize+x1],tx);
            const float south=mix(values[static_cast<std::size_t>(z1)*
                                         hydrologyGridSize+x0],
                                  values[static_cast<std::size_t>(z1)*
                                         hydrologyGridSize+x1],tx);
            return mix(north,south,tz);
        };
        return {bilinear(river),bilinear(lake),bilinear(flow)};
    }
};

struct HydrologyKey {
    std::uint64_t seed{};int x{},z{};
    bool operator==(const HydrologyKey&) const = default;
};

struct HydrologyKeyHash {
    std::size_t operator()(const HydrologyKey& key) const {
        std::uint64_t value=key.seed^
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.x))<<32)^
            static_cast<std::uint32_t>(key.z);
        value^=value>>30;value*=0xbf58476d1ce4e5b9ull;value^=value>>27;
        return static_cast<std::size_t>(value^(value>>31));
    }
};

std::unordered_map<HydrologyKey,std::shared_ptr<const HydrologyRegion>,
                   HydrologyKeyHash> hydrologyCache;
std::mutex hydrologyMutex;

std::shared_ptr<const HydrologyRegion> generateHydrologyRegion(
    std::uint64_t seed,int regionX,int regionZ) {
    auto region=std::make_shared<HydrologyRegion>();
    region->originX=regionX*hydrologyRegionSpan-
                    hydrologyHaloCells*hydrologyCellSize;
    region->originZ=regionZ*hydrologyRegionSpan-
                    hydrologyHaloCells*hydrologyCellSize;
    constexpr int count=hydrologyGridSize*hydrologyGridSize;
    std::vector<float> original(count),filled(count),accumulation(count);
    std::vector<int> receiver(count,-1);
    region->lake.resize(count);
    for(int z=0;z<hydrologyGridSize;++z) {
        for(int x=0;x<hydrologyGridSize;++x) {
            const int worldX=region->originX+x*hydrologyCellSize;
            const int worldZ=region->originZ+z*hydrologyCellSize;
            const int index=z*hydrologyGridSize+x;
            original[index]=baseElevation(seed,worldX,worldZ);
            filled[index]=original[index];
            accumulation[index]=rainfall(seed,worldX,worldZ);
        }
    }
    for(int pass=0;pass<20;++pass) {
        for(int z=1;z<hydrologyGridSize-1;++z) {
            for(int x=1;x<hydrologyGridSize-1;++x) {
                const int index=z*hydrologyGridSize+x;
                float lowest=std::numeric_limits<float>::max();
                for(int oz=-1;oz<=1;++oz)for(int ox=-1;ox<=1;++ox) {
                    if(ox==0&&oz==0)continue;
                    lowest=std::min(lowest,filled[(z+oz)*hydrologyGridSize+x+ox]);
                }
                if(filled[index]<lowest)
                    filled[index]=std::min(lowest+.002f,original[index]+3.0f);
            }
        }
    }
    for(int z=1;z<hydrologyGridSize-1;++z) {
        for(int x=1;x<hydrologyGridSize-1;++x) {
            const int index=z*hydrologyGridSize+x;
            float best=filled[index];int bestIndex=-1;
            for(int oz=-1;oz<=1;++oz)for(int ox=-1;ox<=1;++ox) {
                if(ox==0&&oz==0)continue;
                const int candidate=(z+oz)*hydrologyGridSize+x+ox;
                const float penalty=ox!=0&&oz!=0?.0002f:0.0f;
                if(filled[candidate]+penalty>=best)continue;
                best=filled[candidate]+penalty;bestIndex=candidate;
            }
            receiver[index]=bestIndex;
            region->lake[index]=clamp((filled[index]-original[index])/1.2f,
                                      0.0f,1.0f);
        }
    }
    std::vector<int> order(count);std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](int left,int right) {
        return filled[left]>filled[right];
    });
    for(int index:order) {
        const int target=receiver[index];
        if(target>=0)accumulation[target]+=accumulation[index];
    }
    region->river.assign(count,0.0f);
    for(int index=0;index<count;++index) {
        if(original[index]<.55f)continue;
        const float flow=std::log2(1.0f+accumulation[index]);
        region->river[index]=smoothStep(2.7f,6.4f,flow);
        region->lake[index]*=smoothStep(.8f,2.2f,accumulation[index]);
    }
    region->flow=std::move(accumulation);
    return region;
}

std::shared_ptr<const HydrologyRegion> getHydrologyRegion(
    std::uint64_t seed,int x,int z) {
    const HydrologyKey key{seed,x,z};
    std::lock_guard lock(hydrologyMutex);
    const auto found=hydrologyCache.find(key);
    if(found!=hydrologyCache.end())return found->second;
    auto region=generateHydrologyRegion(seed,x,z);
    hydrologyCache.emplace(key,region);
    return region;
}

AoeHydrologySample mixHydrology(AoeHydrologySample a,AoeHydrologySample b,
                                 float amount) {
    return {mix(a.river,b.river,amount),mix(a.lake,b.lake,amount),
            mix(a.flow,b.flow,amount)};
}

AoeHydrologySample hydrology(std::uint64_t seed,float worldX,float worldZ) {
    const int regionX=floorDiv(static_cast<int>(std::floor(worldX)),
                               hydrologyRegionSpan);
    const int regionZ=floorDiv(static_cast<int>(std::floor(worldZ)),
                               hydrologyRegionSpan);
    const float localX=worldX-regionX*hydrologyRegionSpan;
    const float localZ=worldZ-regionZ*hydrologyRegionSpan;
    const int xNeighbor=localX<hydrologyBlendTiles?-1:
        (localX>hydrologyRegionSpan-hydrologyBlendTiles?1:0);
    const int zNeighbor=localZ<hydrologyBlendTiles?-1:
        (localZ>hydrologyRegionSpan-hydrologyBlendTiles?1:0);
    const float xBlend=xNeighbor<0?1.0f-localX/hydrologyBlendTiles:
        (xNeighbor>0?(localX-(hydrologyRegionSpan-hydrologyBlendTiles))/
                     hydrologyBlendTiles:0.0f);
    const float zBlend=zNeighbor<0?1.0f-localZ/hydrologyBlendTiles:
        (zNeighbor>0?(localZ-(hydrologyRegionSpan-hydrologyBlendTiles))/
                     hydrologyBlendTiles:0.0f);
    const AoeHydrologySample center=
        getHydrologyRegion(seed,regionX,regionZ)->sample(worldX,worldZ);
    if(xNeighbor==0&&zNeighbor==0)return center;
    const AoeHydrologySample horizontal=xNeighbor==0?center:mixHydrology(
        center,getHydrologyRegion(seed,regionX+xNeighbor,regionZ)->
            sample(worldX,worldZ),xBlend);
    if(zNeighbor==0)return horizontal;
    const AoeHydrologySample vertical=mixHydrology(center,
        getHydrologyRegion(seed,regionX,regionZ+zNeighbor)->
            sample(worldX,worldZ),zBlend);
    if(xNeighbor==0)return vertical;
    const AoeHydrologySample diagonal=
        getHydrologyRegion(seed,regionX+xNeighbor,regionZ+zNeighbor)->
            sample(worldX,worldZ);
    return mixHydrology(horizontal,mixHydrology(vertical,diagonal,xBlend),
                        zBlend);
}

float carvedElevation(std::uint64_t seed,int x,int z) {
    float elevation=baseElevation(seed,x,z);
    const AoeHydrologySample drainage=hydrology(seed,x,z);
    if(elevation>.35f) {
        const float channelCarve=drainage.river*
            std::min(6.5f,elevation-.25f);
        const float lakeCarve=drainage.lake*
            std::min(3.2f,elevation-.2f);
        elevation-=std::max(channelCarve,lakeCarve);
    }
    return elevation;
}

std::uint8_t heightAt(std::uint64_t seed,int x,int z) {
    return static_cast<std::uint8_t>(clamp(
        static_cast<float>(std::floor(carvedElevation(seed,x,z))),0.0f,22.0f));
}

std::uint8_t surface(std::uint8_t height) {
    return height<=2?0:height;
}

struct Classification {
    AoeBiome biome{AoeBiome::DeepWater};
    AoeWorldBiome region{AoeWorldBiome::Ocean};
};

Classification classify(std::uint64_t seed,int x,int z,float elevation) {
    const float base=baseElevation(seed,x,z);
    if(base<-.35f)return {AoeBiome::DeepWater,AoeWorldBiome::Ocean};
    if(base<.9f)return {AoeBiome::ShallowWater,AoeWorldBiome::Ocean};
    const AoeHydrologySample drainage=hydrology(seed,x,z);
    const float river=drainage.river;
    const float continental=fractalNoise(seed^0x6a09e667f3bcc909ull,
                                          x/720.0f,z/720.0f,4);
    if(drainage.lake>.48f&&elevation<5.5f) {
        const auto signedSeed=static_cast<std::int64_t>(seed);
        const bool warmBand=std::sin((z+signedSeed%10000)/1450.0f)>-.05f;
        const bool mangrove=base<1.7f&&warmBand&&rainfall(seed,x,z)>.72f;
        return {mangrove?AoeBiome::MangroveShallows:AoeBiome::RiverWater,
                AoeWorldBiome::Wetland};
    }
    if(river>.48f&&continental>-.18f)
        return {AoeBiome::RiverWater,AoeWorldBiome::River};
    if(elevation<1.45f)return {AoeBiome::Beach,AoeWorldBiome::Coast};
    const float moisture=clamp(.5f+
        fractalNoise(seed^0x5deece66dull,x/430.0f,z/430.0f,4)*.34f+
        fractalNoise(seed^0xa54ff53a5f1d36f1ull,x/105.0f,z/105.0f,2)*.16f+
        river*.24f,0.0f,1.0f);
    const auto signedSeed=static_cast<std::int64_t>(seed);
    const float climateBand=std::sin((z+signedSeed%10000)/1450.0f);
    const float temperature=clamp(.55f+climateBand*.24f+
        fractalNoise(seed^0x510e527fade682d1ull,x/610.0f,z/610.0f,3)*.22f-
        std::max(0.0f,elevation-3.0f)*.032f,0.0f,1.0f);
    if(elevation>13.0f)return temperature<.43f&&moisture>.34f?
        Classification{AoeBiome::Snow,AoeWorldBiome::Alpine}:
        Classification{AoeBiome::Rock,AoeWorldBiome::Alpine};
    if(elevation>9.0f)return temperature<.30f&&moisture>.42f?
        Classification{AoeBiome::Snow,AoeWorldBiome::Alpine}:
        Classification{AoeBiome::Rock,AoeWorldBiome::Alpine};
    if(elevation>6.0f)return temperature<.24f&&moisture>.48f?
        Classification{AoeBiome::Snow,AoeWorldBiome::Alpine}:
        Classification{AoeBiome::Highland,AoeWorldBiome::TemperateGrassland};
    if(temperature<.20f)return {AoeBiome::Tundra,AoeWorldBiome::Tundra};
    if(temperature<.36f)return moisture>.43f?
        Classification{AoeBiome::Forest,AoeWorldBiome::Taiga}:
        Classification{AoeBiome::Tundra,AoeWorldBiome::Tundra};
    if(moisture<.18f&&temperature>.58f)
        return {AoeBiome::CrackedEarth,AoeWorldBiome::Desert};
    if(moisture<.30f&&temperature>.5f)
        return {AoeBiome::DesertSand,AoeWorldBiome::Desert};
    if(moisture<.43f&&temperature>.55f)
        return {AoeBiome::DryGrass,AoeWorldBiome::Savanna};
    if(river>.24f&&moisture>.62f)
        return {AoeBiome::Mud,AoeWorldBiome::Wetland};
    if(moisture>.72f&&temperature>.58f)
        return {AoeBiome::JungleFloor,AoeWorldBiome::Rainforest};
    if(moisture>.53f)
        return {AoeBiome::Forest,AoeWorldBiome::TemperateForest};
    return {AoeBiome::Grassland,AoeWorldBiome::TemperateGrassland};
}

bool waterBiome(AoeBiome biome) {
    return biome==AoeBiome::DeepWater||biome==AoeBiome::ShallowWater||
           biome==AoeBiome::RiverWater||biome==AoeBiome::MangroveShallows;
}

float nominalWaterDepth(AoeBiome biome,AoeHydrologySample drainage,
                        float base) {
    switch(biome) {
    case AoeBiome::DeepWater:
        return clamp(1.9f+std::max(0.0f,-base)*.55f,1.9f,3.2f);
    case AoeBiome::ShallowWater:
        return clamp(.42f+std::max(0.0f,.9f-base)*.55f,.42f,1.15f);
    case AoeBiome::RiverWater:
        return clamp(.72f+drainage.river*1.75f+drainage.lake*.55f,.72f,2.7f);
    case AoeBiome::MangroveShallows:return .34f;
    default:return 0.0f;
    }
}

std::uint32_t packColor(float r,float g,float b) {
    const auto channel=[](float value) {
        return static_cast<std::uint32_t>(clamp(value,0.0f,1.0f)*255.0f+.5f);
    };
    return channel(r)|(channel(g)<<8)|(channel(b)<<16)|0xff000000u;
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

float treeChance(AoeWorldBiome region,float elevation) {
    float chance=0.0f;
    switch(region) {
    case AoeWorldBiome::Rainforest:chance=.31f;break;
    case AoeWorldBiome::TemperateForest:chance=.23f;break;
    case AoeWorldBiome::Taiga:chance=.19f;break;
    case AoeWorldBiome::Wetland:chance=.13f;break;
    case AoeWorldBiome::Savanna:chance=.065f;break;
    case AoeWorldBiome::Alpine:chance=.045f;break;
    case AoeWorldBiome::Coast:chance=.012f;break;
    case AoeWorldBiome::Tundra:chance=.025f;break;
    case AoeWorldBiome::Desert:chance=.009f;break;
    default:break;
    }
    return region==AoeWorldBiome::Alpine?
        chance*clamp((12.0f-elevation)/4.0f,0.0f,1.0f):chance;
}

void appendTree(EnvironmentMesh& mesh,Vec3 base,float height,float radius,
                std::uint32_t seed,AoeWorldBiome region) {
    constexpr int sides=5;
    const std::uint32_t trunkBase=static_cast<std::uint32_t>(
        mesh.detailVertices.size());
    const std::uint32_t wood=packColor(.22f,.13f,.065f);
    for(int layer=0;layer<2;++layer)for(int side=0;side<sides;++side) {
        const float angle=2.0f*pi*side/sides;
        const float r=layer==0?radius:radius*.68f;
        const Vec3 normal{std::cos(angle),0,std::sin(angle)};
        mesh.detailVertices.push_back({base+Vec3{normal.x*r,
            layer==0?0.0f:height*.62f,normal.z*r},normal,wood,5.0f,
            static_cast<float>(side)/sides,static_cast<float>(layer)});
    }
    for(int side=0;side<sides;++side) {
        const std::uint32_t next=(side+1)%sides;
        mesh.detailIndices.insert(mesh.detailIndices.end(),{
            trunkBase+static_cast<std::uint32_t>(side),trunkBase+next,
            trunkBase+sides+static_cast<std::uint32_t>(side),
            trunkBase+next,trunkBase+sides+next,
            trunkBase+sides+static_cast<std::uint32_t>(side)});
    }
    const float conifer=(region==AoeWorldBiome::Taiga||
                         region==AoeWorldBiome::Tundra||
                         region==AoeWorldBiome::Alpine)?1.0f:0.0f;
    const float warm=(region==AoeWorldBiome::Savanna||
                      region==AoeWorldBiome::Desert)?1.0f:0.0f;
    const float tint=.88f+.18f*unitHash(seed,0,0,631);
    const std::uint32_t foliage=conifer>.5f?
        packColor(.095f*tint,.235f*tint,.095f*tint):
        (warm>.5f?packColor(.24f*tint,.38f*tint,.075f*tint):
                    packColor(.13f*tint,.36f*tint,.095f*tint));
    const int crownCount=conifer>.5f?3:2;
    for(int crown=0;crown<crownCount;++crown) {
        const float t=static_cast<float>(crown)/(std::max(1,crownCount-1));
        const float crownY=height*(.58f+.18f*t);
        const float crownRadius=height*(conifer>.5f?mix(.24f,.11f,t):
                                                    mix(.25f,.20f,t));
        const float crownHeight=height*(conifer>.5f?.30f:.22f);
        const std::uint32_t first=static_cast<std::uint32_t>(
            mesh.detailVertices.size());
        mesh.detailVertices.push_back({base+Vec3{0,crownY+crownHeight,0},
            {0,1,0},foliage,4.0f,.5f,1.0f});
        mesh.detailVertices.push_back({base+Vec3{0,crownY-crownHeight*.72f,0},
            {0,-1,0},foliage,4.0f,.5f,0.0f});
        for(int side=0;side<6;++side) {
            const float angle=2*pi*side/6.0f;
            const Vec3 radial{std::cos(angle),.08f,std::sin(angle)};
            mesh.detailVertices.push_back({base+Vec3{radial.x*crownRadius,
                crownY+radial.y*crownRadius,radial.z*crownRadius},
                normalize(radial),foliage,4.0f,
                .5f+.5f*radial.x,.5f+.5f*radial.z});
        }
        for(int side=0;side<6;++side) {
            const std::uint32_t a=first+2+side,b=first+2+(side+1)%6;
            mesh.detailIndices.insert(mesh.detailIndices.end(),
                {first,a,b,first+1,b,a});
        }
    }
}

void appendRock(EnvironmentMesh& mesh,Vec3 base,float radius,std::uint32_t color) {
    const std::uint32_t first=static_cast<std::uint32_t>(mesh.detailVertices.size());
    const std::array<Vec3,5> points{{{-radius,0,-radius*.65f},
        {radius*.9f,0,-radius*.55f},{radius*.72f,0,radius*.8f},
        {-radius*.8f,0,radius*.72f},{0,radius*.85f,0}}};
    for(Vec3 point:points)mesh.detailVertices.push_back(
        {base+point,normalize(Vec3{point.x,radius*.8f,point.z}),color,3.0f,0,0});
    mesh.detailIndices.insert(mesh.detailIndices.end(),{
        first,first+1,first+4,first+1,first+2,first+4,
        first+2,first+3,first+4,first+3,first,first+4,
        first,first+3,first+2,first,first+2,first+1});
}

float grassSuitability(AoeBiome biome) {
    switch(biome) {
    case AoeBiome::Grassland:return .90f;
    case AoeBiome::DryGrass:return .62f;
    case AoeBiome::Forest:return .46f;
    case AoeBiome::JungleFloor:return .54f;
    case AoeBiome::Mud:return .10f;
    case AoeBiome::Tundra:return .22f;
    case AoeBiome::Highland:return .31f;
    default:return 0.0f;
    }
}

float biomeMoisture(AoeBiome biome) {
    switch(biome) {
    case AoeBiome::JungleFloor:return .90f;
    case AoeBiome::Forest:return .72f;
    case AoeBiome::Mud:return .85f;
    case AoeBiome::Grassland:return .58f;
    case AoeBiome::Highland:return .42f;
    case AoeBiome::Tundra:return .48f;
    case AoeBiome::DryGrass:return .26f;
    default:return .4f;
    }
}

} // namespace

float AoeWorldGenerator::baseElevationAt(std::int64_t seed,int x,int z) {
    return baseElevation(seedBits(seed),x,z);
}

float AoeWorldGenerator::rainfallAt(std::int64_t seed,int x,int z) {
    return rainfall(seedBits(seed),x,z);
}

AoeHydrologySample AoeWorldGenerator::hydrologyAt(
    std::int64_t seed,float x,float z) {
    return hydrology(seedBits(seed),x,z);
}

AoeBiome AoeWorldGenerator::biomeAt(std::int64_t seed,int x,int z) {
    const std::uint64_t bits=seedBits(seed);
    const float average=(heightAt(bits,x,z)+heightAt(bits,x+1,z)+
                         heightAt(bits,x+1,z+1)+heightAt(bits,x,z+1))/4.0f;
    return classify(bits,x,z,average).biome;
}

float AoeWorldGenerator::renderedHeightAt(
    std::int64_t seed,float x,float z) {
    const std::uint64_t bits=seedBits(seed);
    const int tileX=static_cast<int>(std::floor(x));
    const int tileZ=static_cast<int>(std::floor(z));
    const float fractionX=x-tileX,fractionZ=z-tileZ;
    const auto smoothed=[&](int vertexX,int vertexZ) {
        float weighted=0,total=0;
        for(int offsetZ=-1;offsetZ<=1;++offsetZ) {
            for(int offsetX=-1;offsetX<=1;++offsetX) {
                const float weight=(offsetX==0?2.0f:1.0f)*
                                   (offsetZ==0?2.0f:1.0f);
                weighted+=surface(heightAt(bits,vertexX+offsetX,
                                            vertexZ+offsetZ))*weight;
                total+=weight;
            }
        }
        return weighted/total;
    };
    const float northWest=smoothed(tileX,tileZ);
    const float northEast=smoothed(tileX+1,tileZ);
    const float southWest=smoothed(tileX,tileZ+1);
    const float southEast=smoothed(tileX+1,tileZ+1);
    return mix(mix(northWest,northEast,fractionX),
               mix(southWest,southEast,fractionX),fractionZ);
}

Vec3 AoeWorldGenerator::nearestPlayableSpawn(std::int64_t seed) {
    constexpr int maximumSearchRadius=160;
    for(int radius=0;radius<=maximumSearchRadius;++radius) {
        for(int z=-radius;z<=radius;++z)for(int x=-radius;x<=radius;++x) {
            if(std::max(std::abs(x),std::abs(z))!=radius)continue;
            if(!waterBiome(biomeAt(seed,x,z)))
                return {x+.5f,0,z+.5f};
        }
    }
    throw std::runtime_error("No playable land was found near the world origin.");
}

AoeWorldScene AoeWorldGenerator::generate(std::int64_t seed) {
    AoeWorldScene scene;scene.seed_=seed;
    const std::uint64_t bits=seedBits(seed);
    const Vec3 sourceSpawn=nearestPlayableSpawn(seed);
    const int centerX=static_cast<int>(std::floor(sourceSpawn.x));
    const int centerZ=static_cast<int>(std::floor(sourceSpawn.z));
    scene.sourceOriginX_=static_cast<float>(centerX)-AoeWorldScene::halfExtent;
    scene.sourceOriginZ_=static_cast<float>(centerZ)-AoeWorldScene::halfExtent;
    const int firstSourceX=static_cast<int>(scene.sourceOriginX_);
    const int firstSourceZ=static_cast<int>(scene.sourceOriginZ_);

    constexpr int haloResolution=AoeWorldScene::gridResolution+2;
    std::vector<std::uint8_t> rawHeights(
        static_cast<std::size_t>(haloResolution)*haloResolution);
    const auto haloIndex=[](int x,int z) {
        return static_cast<std::size_t>(z)*haloResolution+x;
    };
    for(int z=0;z<haloResolution;++z)for(int x=0;x<haloResolution;++x)
        rawHeights[haloIndex(x,z)]=heightAt(bits,firstSourceX+x-1,
                                            firstSourceZ+z-1);

    std::vector<float> rendered(static_cast<std::size_t>(
        AoeWorldScene::gridResolution)*AoeWorldScene::gridResolution);
    for(int z=0;z<AoeWorldScene::gridResolution;++z) {
        for(int x=0;x<AoeWorldScene::gridResolution;++x) {
            float weighted=0,total=0;
            for(int oz=-1;oz<=1;++oz)for(int ox=-1;ox<=1;++ox) {
                const float weight=(ox==0?2.0f:1.0f)*(oz==0?2.0f:1.0f);
                weighted+=surface(rawHeights[haloIndex(x+1+ox,z+1+oz)])*weight;
                total+=weight;
            }
            rendered[scene.vertexIndex(x,z)]=weighted/total*
                AoeWorldScene::heightScale;
        }
    }

    scene.tileBiomes_.resize(static_cast<std::size_t>(
        AoeWorldScene::tileResolution)*AoeWorldScene::tileResolution);
    scene.tileWaterDepths_.resize(scene.tileBiomes_.size());
    std::vector<AoeWorldBiome> tileRegions(scene.tileBiomes_.size());
    for(int z=0;z<AoeWorldScene::tileResolution;++z) {
        for(int x=0;x<AoeWorldScene::tileResolution;++x) {
            const float average=(rawHeights[haloIndex(x+1,z+1)]+
                                 rawHeights[haloIndex(x+2,z+1)]+
                                 rawHeights[haloIndex(x+2,z+2)]+
                                 rawHeights[haloIndex(x+1,z+2)])/4.0f;
            const int sourceX=firstSourceX+x,sourceZ=firstSourceZ+z;
            const Classification classified=classify(bits,sourceX,sourceZ,average);
            const std::size_t index=scene.tileIndex(x,z);
            scene.tileBiomes_[index]=classified.biome;
            tileRegions[index]=classified.region;
            ++scene.stats_.biomeTileCounts[static_cast<std::size_t>(classified.biome)];
            ++scene.stats_.terrainTiles;
            if(waterBiome(classified.biome)) {
                scene.tileWaterDepths_[index]=nominalWaterDepth(
                    classified.biome,hydrology(bits,sourceX,sourceZ),
                    baseElevation(bits,sourceX,sourceZ));
                ++scene.stats_.waterTiles;
            }
        }
    }

    scene.terrainHeights_=rendered;
    for(int z=0;z<AoeWorldScene::gridResolution;++z) {
        for(int x=0;x<AoeWorldScene::gridResolution;++x) {
            float adjacentDepth=0.0f;
            for(int oz=-1;oz<=0;++oz)for(int ox=-1;ox<=0;++ox) {
                const int tx=x+ox,tz=z+oz;
                if(tx<0||tz<0||tx>=AoeWorldScene::tileResolution||
                   tz>=AoeWorldScene::tileResolution)continue;
                adjacentDepth=std::max(adjacentDepth,
                    scene.tileWaterDepths_[scene.tileIndex(tx,tz)]);
            }
            if(adjacentDepth>0.0f)scene.terrainHeights_[scene.vertexIndex(x,z)]=
                std::min(scene.terrainHeights_[scene.vertexIndex(x,z)],
                    AoeWorldScene::waterSurfaceHeight-adjacentDepth);
        }
    }
    scene.terrainNormals_.resize(scene.terrainHeights_.size());
    scene.stats_.minimumHeight=std::numeric_limits<float>::max();
    scene.stats_.maximumHeight=std::numeric_limits<float>::lowest();
    for(int z=0;z<AoeWorldScene::gridResolution;++z) {
        for(int x=0;x<AoeWorldScene::gridResolution;++x) {
            const int left=std::max(0,x-1),right=std::min(
                AoeWorldScene::gridResolution-1,x+1);
            const int north=std::max(0,z-1),south=std::min(
                AoeWorldScene::gridResolution-1,z+1);
            const float dx=scene.terrainHeights_[scene.vertexIndex(right,z)]-
                           scene.terrainHeights_[scene.vertexIndex(left,z)];
            const float dz=scene.terrainHeights_[scene.vertexIndex(x,south)]-
                           scene.terrainHeights_[scene.vertexIndex(x,north)];
            const Vec3 normal=normalize(Vec3{-dx/
                static_cast<float>(right-left),1.0f,-dz/
                static_cast<float>(south-north)});
            const std::size_t index=scene.vertexIndex(x,z);
            scene.terrainNormals_[index]=normal;
            scene.stats_.minimumHeight=std::min(scene.stats_.minimumHeight,
                                                scene.terrainHeights_[index]);
            scene.stats_.maximumHeight=std::max(scene.stats_.maximumHeight,
                                                scene.terrainHeights_[index]);
        }
    }

    EnvironmentMesh& mesh=scene.mesh_;
    mesh.terrainVertices.reserve(static_cast<std::size_t>(
        AoeWorldScene::tileResolution)*AoeWorldScene::tileResolution*4);
    mesh.terrainIndices.reserve(static_cast<std::size_t>(
        AoeWorldScene::tileResolution)*AoeWorldScene::tileResolution*6);
    for(int z=0;z<AoeWorldScene::tileResolution;++z) {
        for(int x=0;x<AoeWorldScene::tileResolution;++x) {
            const std::size_t tile=scene.tileIndex(x,z);
            const AoeBiome biome=scene.tileBiomes_[tile];
            const float material=7.0f+static_cast<float>(biome)*.01f;
            const std::uint32_t color=biomeColor(biome);
            const std::uint32_t first=static_cast<std::uint32_t>(
                mesh.terrainVertices.size());
            const auto vertex=[&](int gx,int gz,float u,float v) {
                const std::size_t index=scene.vertexIndex(gx,gz);
                return MeshVertex{{gx-AoeWorldScene::halfExtent,
                    scene.terrainHeights_[index],gz-AoeWorldScene::halfExtent},
                    scene.terrainNormals_[index],color,material,u,v};
            };
            mesh.terrainVertices.push_back(vertex(x,z,0,0));
            mesh.terrainVertices.push_back(vertex(x+1,z,1,0));
            mesh.terrainVertices.push_back(vertex(x+1,z+1,1,1));
            mesh.terrainVertices.push_back(vertex(x,z+1,0,1));
            if(((x+z)&1)==0)mesh.terrainIndices.insert(mesh.terrainIndices.end(),
                {first,first+3,first+2,first,first+2,first+1});
            else mesh.terrainIndices.insert(mesh.terrainIndices.end(),
                {first,first+3,first+1,first+1,first+3,first+2});

            const float waterDepth=scene.tileWaterDepths_[tile];
            if(waterDepth>0.0f) {
                const std::uint32_t waterFirst=static_cast<std::uint32_t>(
                    mesh.riverVertices.size());
                const auto waterVertex=[&](int gx,int gz) {
                    const float depth=AoeWorldScene::waterSurfaceHeight-
                        scene.terrainHeights_[scene.vertexIndex(gx,gz)];
                    const float normalizedDepth=clamp(depth/3.2f,0.0f,1.0f);
                    return MeshVertex{{gx-AoeWorldScene::halfExtent,
                        AoeWorldScene::waterSurfaceHeight,
                        gz-AoeWorldScene::halfExtent},{0,1,0},
                        packColor(.34f,.55f,.64f),6.1f,normalizedDepth,0};
                };
                mesh.riverVertices.push_back(waterVertex(x,z));
                mesh.riverVertices.push_back(waterVertex(x+1,z));
                mesh.riverVertices.push_back(waterVertex(x+1,z+1));
                mesh.riverVertices.push_back(waterVertex(x,z+1));
                mesh.riverIndices.insert(mesh.riverIndices.end(),{
                    waterFirst,waterFirst+3,waterFirst+2,
                    waterFirst,waterFirst+2,waterFirst+1});
            }
        }
    }

    // Translate the source world's deterministic tree catalogue into compact
    // 3D proxies. Distribution follows the original regional spawn chances;
    // geometry and materials are native procedural assets.
    for(int z=0;z<AoeWorldScene::tileResolution;++z) {
        for(int x=0;x<AoeWorldScene::tileResolution;++x) {
            const std::size_t tile=scene.tileIndex(x,z);
            if(waterBiome(scene.tileBiomes_[tile]))continue;
            const int sourceX=firstSourceX+x,sourceZ=firstSourceZ+z;
            const float elevation=(rawHeights[haloIndex(x+1,z+1)]+
                                   rawHeights[haloIndex(x+2,z+1)]+
                                   rawHeights[haloIndex(x+2,z+2)]+
                                   rawHeights[haloIndex(x+1,z+2)])/4.0f;
            const float localX=x-AoeWorldScene::halfExtent+.5f;
            const float localZ=z-AoeWorldScene::halfExtent+.5f;
            const TerrainSurfaceSample ground=scene.sampleTerrain(localX,localZ);
            if(unitHash(bits,sourceX,sourceZ,91)<
               treeChance(tileRegions[tile],elevation)) {
                const float jitterX=(unitHash(bits,sourceX,sourceZ,307)-.5f)*.54f;
                const float jitterZ=(unitHash(bits,sourceX,sourceZ,311)-.5f)*.54f;
                const float height=3.2f+unitHash(bits,sourceX,sourceZ,313)*4.8f;
                appendTree(mesh,{localX+jitterX,ground.position.y,localZ+jitterZ},
                    height,.11f+height*.018f,
                    static_cast<std::uint32_t>(unitHash(bits,sourceX,sourceZ,317)*
                                               4294967295.0f),tileRegions[tile]);
                ++scene.stats_.trees;
            } else {
                const float rockChance=(scene.tileBiomes_[tile]==AoeBiome::Rock||
                    scene.tileBiomes_[tile]==AoeBiome::Highland)?.024f:.0025f;
                if(unitHash(bits,sourceX,sourceZ,811)<rockChance) {
                    appendRock(mesh,ground.position+
                        Vec3{localX-ground.position.x,0,localZ-ground.position.z},
                        .18f+unitHash(bits,sourceX,sourceZ,823)*.34f,
                        packColor(.36f,.34f,.29f));
                    ++scene.stats_.rocks;
                }
            }

            const float suitability=grassSuitability(scene.tileBiomes_[tile]);
            if(suitability<=0.0f)continue;
            for(int subZ=0;subZ<2;++subZ)for(int subX=0;subX<2;++subX) {
                const int salt=901+subZ*17+subX*31;
                if(unitHash(bits,sourceX,sourceZ,salt)>=suitability)continue;
                const float patchX=x-AoeWorldScene::halfExtent+.25f+.5f*subX;
                const float patchZ=z-AoeWorldScene::halfExtent+.25f+.5f*subZ;
                const TerrainSurfaceSample patchGround=scene.sampleTerrain(patchX,patchZ);
                const float moisture=biomeMoisture(scene.tileBiomes_[tile]);
                const std::uint32_t baseCount=28u+static_cast<std::uint32_t>(
                    unitHash(bits,sourceX*2+subX,sourceZ*2+subZ,937)*7.0f);
                const std::uint32_t shortCode=13u+static_cast<std::uint32_t>(
                    unitHash(bits,sourceX*2+subX,sourceZ*2+subZ,941)*8.0f);
                const bool coarse=unitHash(bits,sourceX*2+subX,
                    sourceZ*2+subZ,947)<.07f;
                const std::uint32_t coarseCount=coarse?1u+
                    static_cast<std::uint32_t>(unitHash(bits,sourceX,sourceZ,953)*2):0u;
                const std::uint32_t coarseCode=32u+static_cast<std::uint32_t>(
                    unitHash(bits,sourceX*2+subX,sourceZ*2+subZ,967)*9.0f);
                GrassPatchGpu patch{};
                patch.minX=patchX-.275f;patch.maxX=patchX+.275f;
                patch.minZ=patchZ-.275f;patch.maxZ=patchZ+.275f;
                patch.baseY=patchGround.position.y;
                patch.minY=patch.baseY-.035f;patch.maxY=patch.baseY+.42f;
                patch.seed=static_cast<std::uint32_t>(
                    unitHash(bits,sourceX*2+subX,sourceZ*2+subZ,971)*16777215.0f)+1u;
                patch.packed=baseCount|(shortCode<<8)|(coarseCount<<16)|
                             (coarseCode<<24);
                patch.normalX=patchGround.normal.x;patch.normalZ=patchGround.normal.z;
                patch.moisture=moisture;
                patch.colourFertility=unitHash(bits,sourceX,sourceZ,977);
                patch.colourDryColony=unitHash(bits,sourceX,sourceZ,983);
                patch.colourLushColony=unitHash(bits,sourceX,sourceZ,991);
                patch.colourWarmCool=unitHash(bits,sourceX,sourceZ,997)*2.0f-1.0f;
                mesh.grassPatches.push_back(patch);
            }
        }
    }
    scene.stats_.grassPatches=static_cast<std::uint32_t>(mesh.grassPatches.size());
    mesh.grassSeed=static_cast<std::uint32_t>(bits);
    mesh.backgroundTreeCount=scene.stats_.trees;
    mesh.rockCount=scene.stats_.rocks;
    mesh.shrubCount=scene.stats_.shrubs;
    mesh.minimumHeight=scene.stats_.minimumHeight;
    mesh.maximumHeight=scene.stats_.maximumHeight;

    scene.spawn_={sourceSpawn.x-centerX,0,sourceSpawn.z-centerZ};
    scene.spawn_.y=scene.sampleTerrain(scene.spawn_.x,scene.spawn_.z).position.y;
    return scene;
}

TerrainSurfaceSample AoeWorldScene::sampleTerrain(float x,float z) const {
    const bool finiteInput=std::isfinite(x)&&std::isfinite(z);
    const bool inside=finiteInput&&x>=-halfExtent&&x<=halfExtent&&
                      z>=-halfExtent&&z<=halfExtent;
    x=clamp(finiteInput?x:0.0f,-halfExtent,halfExtent);
    z=clamp(finiteInput?z:0.0f,-halfExtent,halfExtent);
    const float gridX=x+halfExtent,gridZ=z+halfExtent;
    const int cellX=std::min(tileResolution-1,
        std::max(0,static_cast<int>(std::floor(gridX))));
    const int cellZ=std::min(tileResolution-1,
        std::max(0,static_cast<int>(std::floor(gridZ))));
    const float tx=gridX-cellX,tz=gridZ-cellZ;
    if(terrainHeights_.empty()||terrainNormals_.empty())
        return {{x,0,z},{0,1,0},false};
    const std::array<std::size_t,4> corners{
        vertexIndex(cellX,cellZ),vertexIndex(cellX+1,cellZ),
        vertexIndex(cellX+1,cellZ+1),vertexIndex(cellX,cellZ+1)};
    std::array<float,4> weights{};
    // Match the alternating diagonals emitted by generate(). Collision,
    // water depth, and the ray-traced surface then share the same piecewise
    // planar height instead of disagreeing across bilinear saddle cells.
    if(((cellX+cellZ)&1)==0) {
        if(tz>=tx)weights={1.0f-tz,0.0f,tx,tz-tx};
        else weights={1.0f-tx,tx-tz,tz,0.0f};
    } else if(tx+tz<=1.0f) {
        weights={1.0f-tx-tz,tx,0.0f,tz};
    } else {
        weights={0.0f,1.0f-tz,tx+tz-1.0f,1.0f-tx};
    }
    float height=0.0f;
    Vec3 normal{};
    for(std::size_t corner=0;corner<corners.size();++corner) {
        height+=terrainHeights_[corners[corner]]*weights[corner];
        normal+=terrainNormals_[corners[corner]]*weights[corner];
    }
    return {{x,height,z},normalize(normal),inside};
}

AoeBiome AoeWorldScene::sampleBiome(float x,float z) const {
    if(tileBiomes_.empty())return AoeBiome::DeepWater;
    const int tileX=std::clamp(static_cast<int>(std::floor(
        clamp(x,-halfExtent,halfExtent-.001f)+halfExtent)),0,tileResolution-1);
    const int tileZ=std::clamp(static_cast<int>(std::floor(
        clamp(z,-halfExtent,halfExtent-.001f)+halfExtent)),0,tileResolution-1);
    return tileBiomes_[tileIndex(tileX,tileZ)];
}

PersistentWaterSample AoeWorldScene::sampleWater(float x,float z) const {
    if(!std::isfinite(x)||!std::isfinite(z)||x<-halfExtent||x>halfExtent||
       z<-halfExtent||z>halfExtent||tileWaterDepths_.empty())return {};
    const int tileX=std::clamp(static_cast<int>(std::floor(
        clamp(x,-halfExtent,halfExtent-.001f)+halfExtent)),0,tileResolution-1);
    const int tileZ=std::clamp(static_cast<int>(std::floor(
        clamp(z,-halfExtent,halfExtent-.001f)+halfExtent)),0,tileResolution-1);
    if(tileWaterDepths_[tileIndex(tileX,tileZ)]<=0.0f)return {};
    const TerrainSurfaceSample bed=sampleTerrain(x,z);
    const float depth=std::max(0.0f,waterSurfaceHeight-bed.position.y);
    return {waterSurfaceHeight,depth,1.0f-clamp(depth/3.2f,0.0f,1.0f),true};
}

} // namespace dense
