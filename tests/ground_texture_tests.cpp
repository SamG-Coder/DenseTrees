#include "ground_texture.hpp"
#include "environment.hpp"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(bool condition,std::string_view message) {
    if(!condition)fail(message);
}

size_t dominant(const dense::GroundBiomeWeights& weights) {
    size_t result=0;
    for(size_t index=1;index<weights.material.size();++index)
        if(weights.material[index]>weights.material[result])result=index;
    return result;
}

void validateWeights(const dense::GroundBiomeWeights& weights) {
    float total=0;
    for(float weight:weights.material) {
        require(std::isfinite(weight)&&weight>=0&&weight<=1,
                "biome classifier emitted an invalid material weight");
        total+=weight;
    }
    require(std::abs(total-1.0f)<1.0e-5f,
            "biome material weights are not normalized");
}

int grassCell(float worldCoordinate) {
    return static_cast<int>(std::floor(
        worldCoordinate/dense::EnvironmentGenerator::grassCellSize));
}

void validatePatch(const dense::GrassPatchGpu& patch) {
    const float values[]{patch.minX,patch.minY,patch.minZ,
                         patch.maxX,patch.maxY,patch.maxZ,
                         patch.baseY,patch.normalX,patch.normalZ,
                         patch.moisture,patch.colourFertility,
                         patch.colourDryColony,patch.colourLushColony,
                         patch.colourWarmCool};
    for(float value:values)
        require(std::isfinite(value),
                "grass generator emitted non-finite patch data");
    require(patch.minX<patch.maxX&&patch.minY<patch.maxY&&
            patch.minZ<patch.maxZ,
            "grass generator emitted an empty or inverted AABB");
    require(patch.baseY>=patch.minY&&patch.baseY<=patch.maxY,
            "grass patch base lies outside its AABB");
    require(patch.moisture>=0&&patch.moisture<=1,
            "grass moisture escaped its normalized range");
}

} // namespace

int main() {
    using dense::GroundMaterialTile;
    const auto meadow=dense::groundBiomeWeights({4.0f,.03f,180.0f,.58f,.24f});
    const auto upland=dense::groundBiomeWeights({72.0f,.12f,300.0f,.34f,.30f});
    const auto rock=dense::groundBiomeWeights({88.0f,.92f,260.0f,.22f,.91f});
    const auto riverbank=dense::groundBiomeWeights({3.0f,.05f,2.0f,.93f,.18f});
    validateWeights(meadow);validateWeights(upland);validateWeights(rock);
    validateWeights(riverbank);
    require(dominant(meadow)==static_cast<size_t>(GroundMaterialTile::MeadowTurf),
            "flat low pasture does not select meadow turf");
    require(dominant(upland)==static_cast<size_t>(GroundMaterialTile::UplandShortTurf),
            "high gentle ground does not select upland turf");
    require(dominant(rock)==static_cast<size_t>(GroundMaterialTile::ExposedRockSoil),
            "steep exposed ground does not select mineral material");
    require(dominant(riverbank)==static_cast<size_t>(GroundMaterialTile::RiparianMoss),
            "wet low river bank does not select riparian material");

    const dense::GroundTextureAtlas atlas;
    require(atlas.heightAmplitudeMetres[static_cast<size_t>(GroundMaterialTile::ExposedRockSoil)]>
            atlas.heightAmplitudeMetres[static_cast<size_t>(GroundMaterialTile::MeadowTurf)]*3.0f,
            "rock relief no longer has a physically distinct height scale");

    // Grass is now generated from absolute world-grid cells around the camera.
    // This fixed far-field survey is deliberately well beyond the historical
    // origin-centred grass disc and must still contain healthy pasture.
    constexpr uint32_t grassSeed=0x6f616b31u;
    const int farBaseX=grassCell(880.0f),farBaseZ=grassCell(-760.0f);
    int farAccepted=0,deterministicAccepted=0;
    for(int z=0;z<64;++z)for(int x=0;x<64;++x) {
        dense::GrassPatchGpu first{},second{};
        const bool accepted=dense::EnvironmentGenerator::makeGrassPatch(
            farBaseX+x,farBaseZ+z,grassSeed,first);
        const bool repeated=dense::EnvironmentGenerator::makeGrassPatch(
            farBaseX+x,farBaseZ+z,grassSeed,second);
        require(accepted==repeated,
                "absolute grass cell changed its acceptance decision");
        if(!accepted)continue;
        ++farAccepted;
        require(std::memcmp(&first,&second,sizeof(first))==0,
                "absolute grass cell did not reproduce byte-identical data");
        ++deterministicAccepted;
        validatePatch(first);
        const float patchX=(first.minX+first.maxX)*.5f;
        const float patchZ=(first.minZ+first.maxZ)*.5f;
        require(std::sqrt(patchX*patchX+patchZ*patchZ)>
                    dense::EnvironmentGenerator::grassHalfExtent+500.0f,
                "far-field grass survey unexpectedly fell inside legacy disc");
    }
    require(farAccepted>=256,
            "far meadow produced too little grass outside the legacy radius");
    require(deterministicAccepted==farAccepted,
            "not every accepted far patch was checked deterministically");

    // The persistent water contract must win before stochastic biome thinning:
    // no seed or camera position may put meadow blades in either wetted channel.
    int mainWaterCells=0;
    for(float z=-2700.0f;z<=2700.0f;z+=91.0f) {
        const float centre=dense::EnvironmentGenerator::riverCenterX(z);
        const float safeHalf=dense::EnvironmentGenerator::riverWaterHalfWidth(z)*.72f;
        const int cellZ=grassCell(z);
        for(float offset=-safeHalf;offset<=safeHalf;offset+=3.5f) {
            dense::GrassPatchGpu patch{};
            require(!dense::EnvironmentGenerator::makeGrassPatch(
                        grassCell(centre+offset),cellZ,grassSeed,patch),
                    "grass patch survived inside permanent main-river water");
            ++mainWaterCells;
        }
    }
    require(mainWaterCells>=600,
            "main-river exclusion survey did not cover enough cells");

    int tributaryWaterCells=0;
    for(float x=-2500.0f;x<=180.0f;x+=67.0f) {
        const float centre=dense::EnvironmentGenerator::tributaryCenterZ(x);
        const float safeHalf=
            dense::EnvironmentGenerator::tributaryWaterHalfWidth(x)*.68f;
        const int cellX=grassCell(x);
        for(float offset=-safeHalf;offset<=safeHalf;offset+=2.4f) {
            dense::GrassPatchGpu patch{};
            require(!dense::EnvironmentGenerator::makeGrassPatch(
                        cellX,grassCell(centre+offset),grassSeed,patch),
                    "grass patch survived inside permanent tributary water");
            ++tributaryWaterCells;
        }
    }
    require(tributaryWaterCells>=350,
            "tributary exclusion survey did not cover enough cells");

    std::cout << "grass survey: far=" << farAccepted
              << "/4096 main-water=" << mainWaterCells
              << " tributary-water=" << tributaryWaterCells << '\n';
    return EXIT_SUCCESS;
}
