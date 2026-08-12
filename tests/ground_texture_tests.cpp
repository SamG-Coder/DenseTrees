#include "ground_texture.hpp"
#include "environment.hpp"

#include <algorithm>
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
    const uint32_t bladeCount=patch.packed&255u;
    const uint32_t shortCode=(patch.packed>>8)&255u;
    const uint32_t coarseCount=(patch.packed>>16)&255u;
    const uint32_t coarseCode=(patch.packed>>24)&255u;
    require(bladeCount>=28&&bladeCount<=34&&shortCode>=13&&shortCode<=20,
            "streamed grass left the dense mown-turf population range");
    require(coarseCount?(coarseCount<=3&&coarseCode>=32&&coarseCode<=40&&
                         coarseCode>shortCode):(coarseCode==0),
            "streamed grass emitted an invalid coarse-blade population");
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
    require(atlas.heightAmplitudeMetres[static_cast<size_t>(GroundMaterialTile::MeadowTurf)]<=.004f,
            "meadow normal relief exceeds close-cropped turf scale");
    require(atlas.heightAmplitudeMetres[static_cast<size_t>(GroundMaterialTile::UplandShortTurf)]<=.0055f,
            "upland turf relief is large enough to read as folded fabric");
    require(dense::GroundTextureAtlas::biomeMaterialCount==4&&
                dense::GroundTextureAtlas::tileCount==5,
            "root loam must remain a detail-only fifth material slice");
    const float rootLoamRelief=atlas.heightAmplitudeMetres[
        static_cast<size_t>(GroundMaterialTile::RootLoam)];
    require(rootLoamRelief>=.004f&&rootLoamRelief<=.006f&&
                rootLoamRelief<atlas.heightAmplitudeMetres[
                    static_cast<size_t>(GroundMaterialTile::ExposedRockSoil)]*.25f,
            "root loam relief no longer represents compact organic crumbs");

    const auto generatedAtlas=dense::makeGroundTextureAtlas(0x4c6f616du);
    const auto&loamAlbedo=generatedAtlas.albedoRoughness.front();
    const auto&loamNormal=generatedAtlas.normalHeightCavity.front();
    const size_t loamX=static_cast<size_t>(GroundMaterialTile::RootLoam)*
                       dense::GroundTextureAtlas::tileSize;
    double roughnessSum=0,cavitySum=0,heightSum=0;
    uint32_t heightMinimum=255,heightMaximum=0;
    constexpr uint32_t surveyStep=17;
    size_t surveyCount=0;
    for(uint32_t y=0;y<dense::GroundTextureAtlas::tileSize;y+=surveyStep)
        for(uint32_t x=0;x<dense::GroundTextureAtlas::tileSize;x+=surveyStep){
            const size_t index=static_cast<size_t>(y)*loamAlbedo.width+loamX+x;
            const uint32_t albedo=loamAlbedo.pixels[index];
            const uint32_t normal=loamNormal.pixels[index];
            roughnessSum+=(albedo>>24)&255u;
            const uint32_t height=(normal>>16)&255u;
            heightSum+=height;cavitySum+=(normal>>24)&255u;
            heightMinimum=std::min(heightMinimum,height);
            heightMaximum=std::max(heightMaximum,height);
            ++surveyCount;
        }
    require(roughnessSum/(255.0*surveyCount)>.88,
            "root loam lost its dry, highly diffuse surface");
    require(heightMaximum-heightMinimum>=15&&
                heightSum/(255.0*surveyCount)>.35&&
                heightSum/(255.0*surveyCount)<.65,
            "root loam height field is flat or physically biased");
    require(cavitySum/(255.0*surveyCount)>.12&&
                cavitySum/(255.0*surveyCount)<.55,
            "root loam no longer contains bounded crumb-scale porosity");

    // Grass is now generated from absolute world-grid cells around the camera.
    // This fixed far-field survey is deliberately well beyond the historical
    // origin-centred grass disc and must still contain healthy pasture.
    constexpr uint32_t grassSeed=0x6f616b31u;
    const int farBaseX=grassCell(880.0f),farBaseZ=grassCell(-760.0f);
    int farAccepted=0,deterministicAccepted=0,farCoarse=0;
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
        farCoarse+=((first.packed>>16)&255u)!=0;
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
    require(farCoarse>0&&farCoarse*8<farAccepted,
            "far-field coarse blades are absent or no longer rare");

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

    // Root loam and grass use one analytic annulus, so the origin clearing
    // cannot end as a circular texture ring with an independently hard grass
    // edge. Survey complete rays: the mask remains monotonic, retains the old
    // 1.05 m clear core, and finishes its broad blend by 2.10 m.
    constexpr float tau=6.2831853071795864769f;
    float minimumHalfRadius=10.0f,maximumHalfRadius=0;
    for(int sector=0;sector<144;++sector) {
        const float angle=tau*static_cast<float>(sector)/144.0f;
        const float directionX=std::cos(angle),directionZ=std::sin(angle);
        require(dense::EnvironmentGenerator::rootLoamWeight(
                    directionX*1.05f,directionZ*1.05f)>=1.0f-1.0e-6f,
                "hero-root loam no longer preserves the grass-free buttress core");
        require(dense::EnvironmentGenerator::rootLoamWeight(
                    directionX*2.10f,directionZ*2.10f)<=1.0e-6f,
                "hero-root loam transition extends into uninterrupted meadow");
        float previous=1.0f,halfRadius=0;
        bool sawTransition=false;
        for(int radialStep=0;radialStep<=160;++radialStep) {
            const float radius=2.20f*static_cast<float>(radialStep)/160.0f;
            const float loam=dense::EnvironmentGenerator::rootLoamWeight(
                directionX*radius,directionZ*radius);
            require(std::isfinite(loam)&&loam>=0&&loam<=1,
                    "hero-root loam mask escaped its normalized range");
            require(loam<=previous+1.0e-6f,
                    "hero-root loam mask is not monotonic along a radial ray");
            if(loam>1.0e-4f&&loam<1.0f-1.0e-4f)sawTransition=true;
            if(halfRadius==0&&loam<=.5f)halfRadius=radius;
            previous=loam;
        }
        require(sawTransition&&halfRadius>1.35f&&halfRadius<1.75f,
                "hero-root loam lost its broad annular material transition");
        minimumHalfRadius=std::min(minimumHalfRadius,halfRadius);
        maximumHalfRadius=std::max(maximumHalfRadius,halfRadius);
    }
    require(maximumHalfRadius-minimumHalfRadius>.10f&&
                maximumHalfRadius-minimumHalfRadius<.30f,
            "hero-root loam boundary is circular or implausibly ragged");

    // Probe matched world-grid cells over many deterministic seeds. Core cells
    // never emit a blade, transition cells retain sparse grass, and fully
    // meadow cells must be materially denser than the annulus.
    constexpr int transitionCells[][2]{{2,0},{-3,0},{0,2},{0,-3}};
    constexpr int meadowCells[][2]{{4,0},{-5,0},{0,4},{0,-5}};
    int transitionAccepted=0,meadowAccepted=0;
    for(uint32_t sampleSeed=1;sampleSeed<=512;++sampleSeed) {
        dense::GrassPatchGpu core{};
        require(!dense::EnvironmentGenerator::makeGrassPatch(
                    0,0,grassSeed^sampleSeed,core),
                "streamed grass survived inside the guaranteed root core");
        for(const auto& cell:transitionCells) {
            dense::GrassPatchGpu patch{};
            if(dense::EnvironmentGenerator::makeGrassPatch(
                    cell[0],cell[1],grassSeed^sampleSeed,patch)) {
                const float x=(patch.minX+patch.maxX)*.5f;
                const float z=(patch.minZ+patch.maxZ)*.5f;
                const float loam=dense::EnvironmentGenerator::rootLoamWeight(x,z);
                require(loam>0&&loam<1,
                        "transition-cell grass escaped the shared loam annulus");
                ++transitionAccepted;
            }
        }
        for(const auto& cell:meadowCells) {
            dense::GrassPatchGpu patch{};
            if(dense::EnvironmentGenerator::makeGrassPatch(
                    cell[0],cell[1],grassSeed^sampleSeed,patch)) {
                const float x=(patch.minX+patch.maxX)*.5f;
                const float z=(patch.minZ+patch.maxZ)*.5f;
                require(dense::EnvironmentGenerator::rootLoamWeight(x,z)<=1.0e-6f,
                        "meadow survey cell still overlaps the root-loam annulus");
                ++meadowAccepted;
            }
        }
    }
    require(transitionAccepted>100&&meadowAccepted>transitionAccepted*2,
            "streamed grass no longer fades from sparse root fringe to full meadow");

    std::cout << "grass survey: far=" << farAccepted
              << "/4096 coarse=" << farCoarse
              << " main-water=" << mainWaterCells
              << " tributary-water=" << tributaryWaterCells << '\n';
    return EXIT_SUCCESS;
}
