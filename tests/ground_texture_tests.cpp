#include "ground_texture.hpp"

#include <cmath>
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
    return EXIT_SUCCESS;
}
