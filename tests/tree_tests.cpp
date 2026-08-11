#include "environment.hpp"
#include "ground_texture.hpp"
#include "tree.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

bool finite(dense::Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void validateTriangles(const std::vector<dense::MeshVertex>& vertices,
                       const std::vector<uint32_t>& indices,
                       std::string_view label) {
    if (indices.size() % 3 != 0) {
        std::cerr << "FAIL: " << label << " index count is not divisible by three\n";
        std::exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < indices.size(); i += 3) {
        const uint32_t ia = indices[i];
        const uint32_t ib = indices[i + 1];
        const uint32_t ic = indices[i + 2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) {
            std::cerr << "FAIL: " << label << " triangle " << i / 3
                      << " contains an out-of-range vertex index\n";
            std::exit(EXIT_FAILURE);
        }
        const auto& a = vertices[ia].position;
        const auto& b = vertices[ib].position;
        const auto& c = vertices[ic].position;
        const dense::Vec3 doubledArea = dense::cross(b - a, c - a);
        if (!finite(a) || !finite(b) || !finite(c) || !finite(doubledArea) ||
            dense::lengthSq(doubledArea) <= 1.0e-18f) {
            std::cerr << "FAIL: " << label << " triangle " << i / 3
                      << " is non-finite or has zero area\n";
            std::exit(EXIT_FAILURE);
        }
    }
}

} // namespace

int main() {
    {
    const auto groundAtlas=dense::makeGroundTextureAtlas(0x1234abcdu);
    require(groundAtlas.albedoRoughness.size()==dense::GroundTextureAtlas::tileSafeMipCount&&
                groundAtlas.normalHeightCavity.size()==dense::GroundTextureAtlas::tileSafeMipCount,
            "runtime ground material mip inventory is incomplete");
    uint32_t expectedAtlasSize=dense::GroundTextureAtlas::atlasWidth;
    for(size_t level=0;level<groundAtlas.albedoRoughness.size();++level){
        const auto&albedoMip=groundAtlas.albedoRoughness[level];
        const auto&normalMip=groundAtlas.normalHeightCavity[level];
        require(albedoMip.width==expectedAtlasSize&&albedoMip.height==expectedAtlasSize&&
                    normalMip.width==expectedAtlasSize&&normalMip.height==expectedAtlasSize&&
                    albedoMip.pixels.size()==static_cast<size_t>(expectedAtlasSize)*expectedAtlasSize&&
                    normalMip.pixels.size()==static_cast<size_t>(expectedAtlasSize)*expectedAtlasSize,
                "runtime ground material mip dimensions are invalid");
        const size_t stride=std::max<size_t>(1,normalMip.pixels.size()/257);
        for(size_t sample=0;sample<normalMip.pixels.size();sample+=stride){
            const uint32_t packed=normalMip.pixels[sample];
            const float nx=((packed&255u)/255.0f)*2-1;
            const float ny=(((packed>>8)&255u)/255.0f)*2-1;
            require(nx*nx+ny*ny<=1.02f,
                    "runtime ground normal contains a non-reconstructable tangent vector");
        }
        expectedAtlasSize=std::max(2u,expectedAtlasSize/2);
    }
    const auto&coarsestGround=groundAtlas.albedoRoughness.back();
    require(coarsestGround.width==2&&coarsestGround.height==2&&
                coarsestGround.pixels[0]!=coarsestGround.pixels[1]&&
                coarsestGround.pixels[0]!=coarsestGround.pixels[2]&&
                coarsestGround.pixels[2]!=coarsestGround.pixels[3],
            "runtime ground atlas bled distinct materials together at its coarsest mip");
    const auto repeatedGroundAtlas=dense::makeGroundTextureAtlas(0x1234abcdu);
    for(size_t level=0;level<groundAtlas.albedoRoughness.size();++level){
        const auto&first=groundAtlas.albedoRoughness[level].pixels;
        const auto&second=repeatedGroundAtlas.albedoRoughness[level].pixels;
        require(first.front()==second.front()&&first[first.size()/2]==second[second.size()/2]&&
                    first.back()==second.back(),
                "runtime ground material generation is not deterministic");
    }
    }

    dense::EnvironmentGenerator environmentGenerator;
    const auto environment = environmentGenerator.build(0x1234abcdu);
    const auto environmentCopy = environmentGenerator.build(0x1234abcdu);
    constexpr size_t terrainSide=dense::EnvironmentGenerator::terrainResolution;
    require(environment.terrainVertices.size()==terrainSide*terrainSide,
            "hill terrain vertex inventory changed unexpectedly");
    require(environment.terrainIndices.size()==(terrainSide-1)*(terrainSide-1)*6,
            "hill terrain index inventory changed unexpectedly");
    require(environment.grassPatches.size()>=400000&&environment.grassPatches.size()<=520000,
            "procedural grass patch inventory escaped its performance budget");
    require(dense::EnvironmentGenerator::grassHalfExtent>=222.0f,
            "grass field no longer covers the camera orbit plus maximum long-grass range");
    require(environment.grassPatches.size()==environmentCopy.grassPatches.size(),
            "same environment seed did not reproduce its grass inventory");
    require(environment.detailVertices.size()==environmentCopy.detailVertices.size()&&
                environment.detailIndices.size()==environmentCopy.detailIndices.size(),
            "same environment seed did not reproduce its scene-dressing inventory");
    require(environment.distantTreeCount==environmentCopy.distantTreeCount&&
                environment.distantTreeTriangleCount==environmentCopy.distantTreeTriangleCount&&
                environment.distantTreeBases.size()==environmentCopy.distantTreeBases.size(),
            "same environment seed did not reproduce its distant woodland inventory");
    require(environment.riverVertices.size()==environmentCopy.riverVertices.size()&&
                environment.riverIndices==environmentCopy.riverIndices,
            "same environment seed did not reproduce its river inventory");
    require(std::abs(dense::EnvironmentGenerator::terrainHeight(0,0))<1.0e-6f,
            "oak root grade is no longer exactly y=0");
    for(float z=-1.5f;z<=1.5f;z+=.5f)for(float x=-1.5f;x<=1.5f;x+=.5f)
        if(std::sqrt(x*x+z*z)<=1.5f)
            require(std::abs(dense::EnvironmentGenerator::terrainHeight(x,z))<1.0e-5f,
                    "terrain intruded into the oak root-clearance zone");
    require(environment.minimumHeight<-11.0f&&environment.minimumHeight>-25.0f&&
                environment.maximumHeight>170.0f&&environment.maximumHeight<390.0f,
            "valley-and-mountain terrain height envelope is implausible");
    for(const auto& vertex:environment.terrainVertices){
        require(finite(vertex.position)&&finite(vertex.normal),
                "terrain emitted non-finite geometry");
        require(std::abs(dense::length(vertex.normal)-1.0f)<.002f&&vertex.normal.y>.12f&&
                    std::abs(vertex.position.x)<=dense::EnvironmentGenerator::terrainHalfExtent&&
                    std::abs(vertex.position.z)<=dense::EnvironmentGenerator::terrainHalfExtent,
                "terrain normal is not unit length or points below the landscape");
        require(vertex.material==2.0f,"terrain material routing changed");
    }

    // The authored river is a strict downhill path rather than disconnected
    // puddle decals.  Its tributary meets the main surface without a step.
    float previousRiverBed=std::numeric_limits<float>::max();
    for(float z=-2700.0f;z<=2700.0f;z+=45.0f){
        const float center=dense::EnvironmentGenerator::riverCenterX(z);
        const float bed=dense::EnvironmentGenerator::terrainHeight(center,z);
        require(std::isfinite(center)&&std::isfinite(bed)&&
                    bed<previousRiverBed-.04f,
                "main river bed is not continuously downhill");
        require(std::abs(bed-dense::EnvironmentGenerator::riverBedHeight(z))<2.0e-4f&&
                    dense::EnvironmentGenerator::riverSurfaceHeight(z)>bed+.65f,
                "main river carve and persistent surface contract diverged");
        previousRiverBed=bed;
    }
    const float confluenceZ=520.0f;
    const float confluenceX=dense::EnvironmentGenerator::riverCenterX(confluenceZ);
    require(std::abs(dense::EnvironmentGenerator::tributaryCenterZ(confluenceX)-
                     confluenceZ)<1.0e-4f&&
                std::abs(dense::EnvironmentGenerator::tributarySurfaceHeight(confluenceX)-
                         dense::EnvironmentGenerator::riverSurfaceHeight(confluenceZ))<1.0e-4f,
            "tributary does not join the main river continuously");
    float previousTributarySurface=std::numeric_limits<float>::max();
    for(float x=-2600.0f;x<=confluenceX;x+=35.0f){
        const float surface=dense::EnvironmentGenerator::tributarySurfaceHeight(x);
        require(surface<previousTributarySurface,
                "tributary surface does not descend toward its confluence");
        previousTributarySurface=surface;
    }
    require(dense::EnvironmentGenerator::riverCenterX(0.0f)>250.0f,
            "main river intruded into the hero-tree clearing");
    for(const float z:std::array<float,5>{-2400.0f,-1200.0f,0.0f,1200.0f,2400.0f}){
        const float center=dense::EnvironmentGenerator::riverCenterX(z);
        const float wetHalfWidth=dense::EnvironmentGenerator::riverWaterHalfWidth(z);
        require(std::abs(wetHalfWidth/
                         dense::EnvironmentGenerator::riverHalfWidth(z)-.90f)<1.0e-5f,
                "main river terrain and water use different wetted widths");
        for(const float side:std::array<float,2>{-1.0f,1.0f}){
            const float bank=dense::EnvironmentGenerator::terrainHeight(
                center+side*wetHalfWidth,z);
            const float depth=dense::EnvironmentGenerator::riverSurfaceHeight(z)-bank;
            require(depth>.075f&&depth<.085f,
                    "main river analytic shoreline is detached from its shallow bank");
        }
    }
    constexpr float tributaryBankTestX=-1600.0f;
    const float tributaryWetHalfWidth=
        dense::EnvironmentGenerator::tributaryWaterHalfWidth(tributaryBankTestX);
    require(std::abs(tributaryWetHalfWidth/
                     dense::EnvironmentGenerator::tributaryHalfWidth(tributaryBankTestX)-.90f)<
                1.0e-5f,
            "tributary terrain and water use different wetted widths");
    for(const float side:std::array<float,2>{-1.0f,1.0f}){
        const float z=dense::EnvironmentGenerator::tributaryCenterZ(tributaryBankTestX)+
                      side*tributaryWetHalfWidth;
        const float bank=dense::EnvironmentGenerator::terrainHeight(tributaryBankTestX,z);
        const float depth=
            dense::EnvironmentGenerator::tributarySurfaceHeight(tributaryBankTestX)-bank;
        require(depth>.075f&&depth<.085f,
                "tributary analytic shoreline is detached from its shallow bank");
    }
    const std::array<size_t,5> sampledTerrainTriangles{{
        0,1,1379,environment.terrainIndices.size()/6,
        environment.terrainIndices.size()/3-1
    }};
    for(const size_t triangle:sampledTerrainTriangles){
        const size_t first=triangle*3;
        const auto&p0=environment.terrainVertices[environment.terrainIndices[first]].position;
        const auto&p1=environment.terrainVertices[environment.terrainIndices[first+1]].position;
        const auto&p2=environment.terrainVertices[environment.terrainIndices[first+2]].position;
        constexpr float w0=.19f,w1=.34f,w2=.47f;
        const dense::Vec3 expected=p0*w0+p1*w1+p2*w2;
        const auto sampled=dense::EnvironmentGenerator::sampleTerrainSurface(expected.x,expected.z);
        const dense::Vec3 expectedNormal=dense::normalize(dense::cross(p1-p0,p2-p0));
        require(sampled.insideBounds&&std::abs(sampled.position.y-expected.y)<2.0e-4f&&
                    dense::dot(sampled.normal,expectedNormal)>.9999f,
                "rendered-terrain query disagrees with its emitted triangle");
    }
    const auto outsideTerrain=dense::EnvironmentGenerator::sampleTerrainSurface(
        dense::EnvironmentGenerator::terrainHalfExtent+10.0f,0);
    require(!outsideTerrain.insideBounds&&
                std::abs(outsideTerrain.position.x-
                         dense::EnvironmentGenerator::terrainHalfExtent)<1.0e-4f&&
                finite(outsideTerrain.position)&&finite(outsideTerrain.normal),
            "out-of-bounds terrain query did not return a finite clamped edge sample");
    size_t hydrologyRegion=0,hydrologyEligible=0,flatLocalMinima=0,
           selectedFlatLocalMinima=0;
    constexpr float cosEightDegrees=.990268f,cosFourDegrees=.997564f;
    const auto retention=[](const dense::MeshVertex& vertex){
        return static_cast<float>((vertex.color>>24)&255u)/255.0f;
    };
    for(size_t z=0;z<terrainSide;++z)for(size_t x=0;x<terrainSide;++x){
        const size_t index=z*terrainSide+x;
        const auto&vertex=environment.terrainVertices[index];
        require(((vertex.color>>24)&255u)==
                    ((environmentCopy.terrainVertices[index].color>>24)&255u),
                "terrain runoff bake is not deterministic");
        const float radius=std::sqrt(vertex.position.x*vertex.position.x+
                                     vertex.position.z*vertex.position.z);
        const float suitability=retention(vertex);
        if(radius<6.0f)require(suitability==0,
                              "oak-base hill retained standing puddle water");
        if(vertex.normal.y<=cosEightDegrees)
            require(suitability==0,"terrain runoff bake admitted an eight-degree slope");
        if(radius>dense::EnvironmentGenerator::grassHalfExtent)continue;
        ++hydrologyRegion;hydrologyEligible+=suitability>0;
        if(x==0||z==0||x+1==terrainSide||z+1==terrainSide)continue;
        bool strictMinimum=true,strictMaximum=true;
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx){
            if(dx==0&&dz==0)continue;
            const float neighbour=environment.terrainVertices[
                static_cast<size_t>(static_cast<int>(z)+dz)*terrainSide+
                static_cast<size_t>(static_cast<int>(x)+dx)].position.y;
            strictMinimum&=vertex.position.y<neighbour;
            strictMaximum&=vertex.position.y>neighbour;
        }
        if(strictMaximum)require(suitability==0,
                                "convex terrain maximum retained puddle water");
        if(strictMinimum&&vertex.normal.y>=cosFourDegrees){
            ++flatLocalMinima;selectedFlatLocalMinima+=suitability>.15f;
        }
    }
    require(hydrologyEligible>hydrologyRegion*2/100&&
                hydrologyEligible<hydrologyRegion*15/100,
            "runoff bake selected an implausible fraction of the near terrain");
    require(flatLocalMinima>20&&selectedFlatLocalMinima>=flatLocalMinima/20,
            "runoff bake failed to retain representative flat local depressions");
    validateTriangles(environment.terrainVertices,environment.terrainIndices,"hill terrain");
    require(environment.riverVertices.size()>25000&&
                environment.riverIndices.size()/3>45000&&
                environment.riverIndices.size()/3<60000,
            "persistent river mesh is absent or outside its close-view tessellation budget");
    float minimumRiverClearance=std::numeric_limits<float>::max();
    float maximumBankClearance=0;
    for(size_t i=0;i<environment.riverVertices.size();++i){
        const auto&vertex=environment.riverVertices[i];
        require(finite(vertex.position)&&finite(vertex.normal)&&
                    std::abs(dense::length(vertex.normal)-1.0f)<.002f&&
                    vertex.normal.y>.85f&&vertex.material==6.0f,
                "river mesh emitted an invalid material, position, or normal");
        const auto ground=dense::EnvironmentGenerator::sampleTerrainSurface(
            vertex.position.x,vertex.position.z);
        const float clearance=vertex.position.y-ground.position.y;
        minimumRiverClearance=std::min(minimumRiverClearance,clearance);
        if(vertex.u<1.0e-5f||vertex.u>1.0f-1.0e-5f)
            maximumBankClearance=std::max(maximumBankClearance,clearance);
        require(ground.insideBounds&&clearance>=.029f,
                "persistent river surface intersects its rendered terrain");
    }
    require(maximumBankClearance<.30f,
            "adaptive river correction left a visibly suspended bank edge");
    validateTriangles(environment.riverVertices,environment.riverIndices,"river water");
    uint32_t decodedTallPatches=0,waterRetainingGrassPatches=0;
    for(size_t i=0;i<environment.grassPatches.size();++i){
        const auto& patch=environment.grassPatches[i];
        require(std::isfinite(patch.minX)&&std::isfinite(patch.minY)&&
                    std::isfinite(patch.minZ)&&std::isfinite(patch.maxX)&&
                    std::isfinite(patch.maxY)&&std::isfinite(patch.maxZ),
                "grass emitted a non-finite AABB");
        require(patch.minX<patch.maxX&&patch.minY<patch.baseY&&patch.baseY<patch.maxY&&
                    patch.minZ<patch.maxZ,
                "grass AABB does not enclose its patch base and blade height");
        const uint32_t blades=patch.packed&255u,shortCode=(patch.packed>>8)&255u;
        const uint32_t tallCount=(patch.packed>>16)&255u,tallCode=(patch.packed>>24)&255u;
        const bool tall=tallCount!=0;
        if(tall)++decodedTallPatches;
        const float grassRetention=dense::grassPatchWaterRetention(patch.seed);
        if(grassRetention>1.0f/255.0f)++waterRetainingGrassPatches;
        require(std::isfinite(grassRetention)&&grassRetention>=0&&grassRetention<=1,
                "grass patch runoff retention left normalized range");
        require(blades>=28&&blades<=34&&shortCode>=9&&shortCode<=34,
                "grass candidate count or short height left the meadow range");
        require(tall?(tallCount>=18&&tallCount<=24&&tallCount<blades&&tallCode>=90&&
                       tallCode<=255&&tallCode>shortCode):(tallCode==0),
                "clustered long-grass encoding is invalid");
        const float normalY=std::sqrt(std::max(0.0f,1-patch.normalX*patch.normalX-
                                                    patch.normalZ*patch.normalZ));
        require(normalY>.70f&&patch.moisture>=0&&patch.moisture<=1,
                "grass patch has invalid terrain alignment or moisture");
        require(std::isfinite(patch.colourFertility)&&
                    std::isfinite(patch.colourDryColony)&&
                    std::isfinite(patch.colourLushColony)&&
                    std::isfinite(patch.colourWarmCool)&&
                    patch.colourFertility>=0&&patch.colourFertility<=1&&
                    patch.colourDryColony>=0&&patch.colourDryColony<=1&&
                    patch.colourLushColony>=0&&patch.colourLushColony<=1&&
                    patch.colourWarmCool>=-1&&patch.colourWarmCool<=1,
                "grass patch colour domains are invalid");
        if(i<64){
            const auto& copy=environmentCopy.grassPatches[i];
            require(patch.seed==copy.seed&&patch.packed==copy.packed&&patch.baseY==copy.baseY&&
                        patch.minX==copy.minX&&patch.maxZ==copy.maxZ,
                    "same environment seed did not reproduce grass patch data");
        }
    }
    require(decodedTallPatches==environment.tallGrassPatchCount,
            "long-grass summary does not match the packed patch data");
    require(waterRetainingGrassPatches>environment.grassPatches.size()/100&&
                waterRetainingGrassPatches<environment.grassPatches.size()/5,
            "grass did not inherit a plausible fraction of terrain basins");
    require(environment.tallGrassPatchCount>environment.grassPatches.size()/2&&
                environment.tallGrassPatchCount<environment.grassPatches.size()*3/4,
            "clustered long grass is absent or overwhelms the short meadow");
    require(environment.rockCount>=30&&environment.rockCount<=90,
            "rock families escaped their ecological inventory");
    require(environment.shrubCount>=14&&environment.shrubCount<=24,
            "near/background shrub inventory is implausible");
    require(environment.backgroundTreeCount==21,
            "medium-detail landmark groves escaped their fixed inventory");
    require(environment.distantTreeCount>=180&&environment.distantTreeCount<=350&&
                environment.distantTreeBases.size()==environment.distantTreeCount,
            "map-scale woodland inventory escaped its LOD population budget");
    require(environment.distantTreeTriangleCount>=35000&&
                environment.distantTreeTriangleCount<=60000,
            "map-scale woodland escaped its dedicated triangle budget");
    size_t riparianWoodland=0,northernFoothillWoodland=0;
    float minimumWoodlandX=std::numeric_limits<float>::max();
    float maximumWoodlandX=std::numeric_limits<float>::lowest();
    float minimumWoodlandZ=std::numeric_limits<float>::max();
    float maximumWoodlandZ=std::numeric_limits<float>::lowest();
    const float tributaryJoinX=dense::EnvironmentGenerator::riverCenterX(520.0f);
    for(size_t i=0;i<environment.distantTreeBases.size();++i){
        const auto&base=environment.distantTreeBases[i];
        const auto&copy=environmentCopy.distantTreeBases[i];
        require(base.x==copy.x&&base.y==copy.y&&base.z==copy.z,
                "distant woodland object-local seeds are not deterministic");
        const float heroDistance=std::sqrt(base.x*base.x+base.z*base.z);
        const auto grade=dense::EnvironmentGenerator::sampleTerrainSurface(base.x,base.z);
        require(heroDistance>=620.0f&&grade.insideBounds&&
                    std::abs(base.y-grade.position.y)<2.0e-4f,
                "distant woodland entered the open hero meadow or floated above grade");
        const float mainBank=std::abs(base.x-dense::EnvironmentGenerator::riverCenterX(base.z))-
                             dense::EnvironmentGenerator::riverHalfWidth(base.z);
        float tributaryBank=10000.0f;
        if(base.x<tributaryJoinX)
            tributaryBank=std::abs(base.z-dense::EnvironmentGenerator::tributaryCenterZ(base.x))-
                           dense::EnvironmentGenerator::tributaryHalfWidth(base.x);
        require(mainBank>=19.5f&&tributaryBank>=19.5f,
                "distant woodland entered a permanent river channel");
        riparianWoodland+=std::min(mainBank,tributaryBank)<285.0f;
        northernFoothillWoodland+=base.z>1350.0f&&std::min(mainBank,tributaryBank)>=285.0f;
        minimumWoodlandX=std::min(minimumWoodlandX,base.x);
        maximumWoodlandX=std::max(maximumWoodlandX,base.x);
        minimumWoodlandZ=std::min(minimumWoodlandZ,base.z);
        maximumWoodlandZ=std::max(maximumWoodlandZ,base.z);
    }
    require(riparianWoodland>=100&&northernFoothillWoodland>=70,
            "distant woodland lost its riparian or foothill biome structure");
    require(minimumWoodlandX<-1900.0f&&maximumWoodlandX>600.0f&&
                minimumWoodlandZ<-2000.0f&&maximumWoodlandZ>2000.0f,
            "distant woodland collapsed into a local or radial-band distribution");
    require(!environment.detailVertices.empty()&&environment.detailIndices.size()/3<120000,
            "scene dressing is empty or exceeded its static triangle budget");
    for(const auto& vertex:environment.detailVertices){
        require(finite(vertex.position)&&finite(vertex.normal)&&
                    std::abs(dense::length(vertex.normal)-1.0f)<.004f,
                "scene dressing emitted a non-finite or non-unit vertex");
        require((vertex.material>=3.0f&&vertex.material<3.3f)||
                    (vertex.material>=4.0f&&vertex.material<4.3f)||
                    vertex.material==5.0f,
                "scene dressing lost its rock, foliage, or wood material route");
    }
    validateTriangles(environment.detailVertices,environment.detailIndices,"scene dressing");
    float innerPeak=-1000.0f,outerPeak=-1000.0f,outerPeakRadius=0.0f;
    for(float z=-3100.0f;z<=3100.0f;z+=70.0f)for(float x=-3100.0f;x<=3100.0f;x+=70.0f){
        const float radius=std::sqrt(x*x+z*z);
        const float height=dense::EnvironmentGenerator::terrainHeight(x,z);
        if(radius<1700.0f)innerPeak=std::max(innerPeak,height);
        if(radius>2050.0f&&height>outerPeak){outerPeak=height;outerPeakRadius=radius;}
    }
    require(innerPeak<75.0f&&outerPeak>170.0f&&outerPeakRadius>2200.0f,
            "mountain relief moved back into the playable valley or became too low");
    int northernMassifs=0;
    for(float x=-2100.0f;x<=2100.0f;x+=700.0f){
        float peak=-1000.0f;
        for(float z=1900.0f;z<=3100.0f;z+=25.0f)
            peak=std::max(peak,dense::EnvironmentGenerator::terrainHeight(x,z));
        northernMassifs+=peak>125.0f;
    }
    require(northernMassifs>=5,
            "distant northern mountain chain has implausible continuity");

    dense::TreeGenerator generator;
    dense::TreeParameters p;
    p.attractionPoints = 360;
    p.growthIterations = 55;
    p.fullBiologicalInventory = false;
    auto a = generator.grow(p);
    auto b = generator.grow(p);
    require(a.size() > 40, "generic generator produced no useful branch hierarchy");
    require(a.size() == b.size(), "same seed did not reproduce the same node count");
    for (size_t i = 0; i < a.size(); ++i) {
        require(a[i].position.x == b[i].position.x && a[i].position.y == b[i].position.y &&
                    a[i].position.z == b[i].position.z,
                "same seed did not reproduce identical node positions");
        require(finite(a[i].position), "generic generator emitted a non-finite position");
        if (i) {
            require(a[i].parent >= 0 && static_cast<size_t>(a[i].parent) < i,
                    "generic branch parent must precede its child");
        }
    }

    auto mesh = generator.buildMesh(a, p);
    require(!mesh.branchVertices.empty() && mesh.branchIndices.size() % 3 == 0,
            "generic branch mesh is empty or has incomplete triangles");
    require(!mesh.leafVertices.empty() && mesh.leafIndices.size() % 3 == 0,
            "generic leaf mesh is empty or has incomplete triangles");

    for (auto species : {dense::TreeSpecies::EnglishOak, dense::TreeSpecies::NorwaySpruce,
                         dense::TreeSpecies::SilverBirch, dense::TreeSpecies::WeepingWillow,
                         dense::TreeSpecies::UmbrellaAcacia}) {
        auto preset = dense::TreeGenerator::parametersFor(species, 42);
        preset.attractionPoints = 240;
        preset.growthIterations = 45;
        preset.fullBiologicalInventory = false;
        auto nodes = generator.grow(preset);
        require(nodes.size() > 20, "species preset produced no useful branch hierarchy");
        auto speciesMesh = generator.buildMesh(nodes, preset);
        require(!speciesMesh.branchIndices.empty(), "species preset produced no branch triangles");
    }

    // Canonical mature, open-grown Q. robur. These checks constrain architecture,
    // not a frozen geometry budget: node and leaf totals may grow as detail improves.
    auto oak = dense::TreeGenerator::parametersFor(dense::TreeSpecies::EnglishOak, 5080);
    auto oakNodes = generator.grow(oak);
    require(!oakNodes.empty(), "mature oak generator returned no nodes");
    // A catastrophe guard, not a visual quota. It catches accidental exponential
    // runaway early while leaving well over an order of magnitude of headroom.
    require(oakNodes.size() < 2'000'000, "mature oak node population ran away catastrophically");

    const size_t nodeInventoryBeforeMesh = oakNodes.size();
    size_t currentYearInventoryBeforeMesh = 0;
    size_t livingInventoryBeforeMesh = 0;
    for (const auto& node : oakNodes) {
        currentYearInventoryBeforeMesh += node.currentYear;
        livingInventoryBeforeMesh += node.alive;
    }

    auto oakMesh = generator.buildMesh(oakNodes, oak);
    require(oakNodes.size() == nodeInventoryBeforeMesh,
            "root/mesh construction changed the biological node inventory");
    size_t currentYearInventoryAfterMesh = 0;
    size_t livingInventoryAfterMesh = 0;
    for (const auto& node : oakNodes) {
        currentYearInventoryAfterMesh += node.currentYear;
        livingInventoryAfterMesh += node.alive;
    }
    require(currentYearInventoryAfterMesh == currentYearInventoryBeforeMesh &&
                livingInventoryAfterMesh == livingInventoryBeforeMesh,
            "root/mesh construction changed the living or current-year shoot inventory");
    require(!oakMesh.branchVertices.empty() && oakMesh.branchIndices.size() % 3 == 0,
            "mature oak branch mesh is empty or malformed");
    require(!oakMesh.leafVertices.empty() && oakMesh.leafIndices.size() % 3 == 0,
            "mature oak leaf mesh is empty or malformed");
    require(oakMesh.branchVertices.size() <= std::numeric_limits<uint32_t>::max(),
            "mature oak branch vertices exceed the 32-bit mesh index range");
    require(oakMesh.leafVertices.size() <= std::numeric_limits<uint32_t>::max(),
            "mature oak leaf vertices exceed the 32-bit mesh index range");
    require(static_cast<uint64_t>(oakMesh.structuralSegments) + oakMesh.fineShootSegments ==
                oakNodes.size() - 1,
            "mesh-only root geometry leaked into the biological segment inventory");

    // The oak branch mesh uses extra rings on structural wood and collars.
    // Reconstruct continuation choices and segment offsets so UV checks remain local to the
    // branch that owns them, then identify the mesh-only root range exactly.
    std::vector<int> meshContinuation(oakNodes.size(),-1);
    std::vector<float> meshContinuationScore(oakNodes.size(),-2.0f);
    for(size_t i=1;i<oakNodes.size();++i){const size_t parent=static_cast<size_t>(oakNodes[i].parent);if(oakNodes[i].axisOrder!=oakNodes[parent].axisOrder)continue;const float score=dense::dot(dense::normalize(oakNodes[i].position-oakNodes[parent].position),oakNodes[parent].direction);if(score>meshContinuationScore[parent]){meshContinuationScore[parent]=score;meshContinuation[parent]=static_cast<int>(i);}}
    size_t biologicalBranchVertexCount = 0;
    size_t biologicalBranchIndexCount = 0;
    size_t sampledUvSegments = 0;
    for (size_t i = 1; i < oakNodes.size(); ++i) {
        const int sides = oakNodes[i].axisOrder == 0 ? 48 : (oakNodes[i].axisOrder == 1 ? 32 : (oakNodes[i].axisOrder == 2 ? 16 : 8));
        const size_t ringVertices = static_cast<size_t>(sides + 1);
        const size_t parent=static_cast<size_t>(oakNodes[i].parent);const bool continuation=meshContinuation[parent]==static_cast<int>(i);const float childRatio=oakNodes[i].radius/std::max(oakNodes[parent].radius,.0001f);const bool structuralCollar=!continuation&&oakNodes[i].axisOrder<=2&&childRatio<.72f;const int ringCount=structuralCollar?5:(oakNodes[i].axisOrder<=2?3:2);
        require(biologicalBranchVertexCount + ringVertices * ringCount <= oakMesh.branchVertices.size(),
                "oak branch vertex stream ended inside a biological segment");

        // Sample all coarse structure and a deterministic cross-section of fine
        // shoots. This checks local arc-length mapping without making the test
        // runtime proportional to every UV in a very high-detail crown.
        if (i < 512 || i % 509 == 0) {
            const auto* start = oakMesh.branchVertices.data() + biologicalBranchVertexCount;
            const auto* end = start + ringVertices*(ringCount-1);
            require(std::abs(start[0].u) <= 1.0e-6f && std::abs(start[sides].u - 1.0f) <= 1.0e-6f &&
                        std::abs(end[0].u) <= 1.0e-6f && std::abs(end[sides].u - 1.0f) <= 1.0e-6f,
                    "oak bark U coordinate does not cover one complete local circumference");
            for (int k = 0; k <= sides; ++k) {
                const float expectedU = static_cast<float>(k) / sides;
                require(std::isfinite(start[k].u) && std::isfinite(start[k].v) &&
                            std::isfinite(end[k].u) && std::isfinite(end[k].v),
                        "oak bark UV contains a non-finite value");
                require(std::abs(start[k].u - expectedU) <= 1.0e-6f &&
                            std::abs(end[k].u - expectedU) <= 1.0e-6f,
                        "oak bark U coordinate is not monotonic around its local ring");
                require(std::abs(start[k].v - start[0].v) <= 1.0e-5f &&
                            std::abs(end[k].v - end[0].v) <= 1.0e-5f,
                        "oak bark V coordinate varies around one cross-section");
            }
            const float localLength = dense::length(
                oakNodes[i].position - oakNodes[static_cast<size_t>(oakNodes[i].parent)].position);
            const float mappedLength = end[0].v - start[0].v;
            require(mappedLength >= localLength - std::max(2.0e-5f, localLength * 2.0e-4f) &&
                        mappedLength <= localLength + .0261f,
                    "oak bark V coordinate does not include the rendered collar length");
            require(dense::lengthSq(start[0].position - start[sides].position) <= 1.0e-8f &&
                        dense::lengthSq(end[0].position - end[sides].position) <= 1.0e-8f,
                    "oak bark seam does not close geometrically");
            ++sampledUvSegments;
        }
        biologicalBranchVertexCount += ringVertices * ringCount;
        biologicalBranchIndexCount += static_cast<size_t>(sides) * (ringCount-1) * 6;
    }
    require(sampledUvSegments > 500, "oak bark UV regression sample is unexpectedly small");

    constexpr size_t rootCount = 7;
    constexpr size_t rootSegments = 12;
    constexpr size_t rootSides = 16;
    constexpr size_t rootVertices = rootCount * (rootSegments + 1) * (rootSides + 1);
    constexpr size_t rootIndices = rootCount * rootSegments * rootSides * 6;
    require(oakMesh.branchVertices.size() == biologicalBranchVertexCount + rootVertices,
            "oak mesh no longer contains exactly seven mesh-only shallow roots");
    require(oakMesh.branchIndices.size() == biologicalBranchIndexCount + rootIndices,
            "oak root index inventory no longer matches its declared topology");

    for (const auto& vertex : oakMesh.branchVertices) {
        require(finite(vertex.position) && finite(vertex.normal),
                "oak branch/root vertex contains non-finite geometry");
        const float normalLength = dense::length(vertex.normal);
        require(std::isfinite(normalLength) && normalLength >= .995f && normalLength <= 1.005f,
                "oak branch/root normal is not unit length");
        require(std::isfinite(vertex.u) && std::isfinite(vertex.v),
                "oak branch/root material coordinates are non-finite");
        require(std::isfinite(vertex.material) && std::abs(vertex.material) <= 1.0e-6f,
                "oak branch/root vertex lost the English-oak bark material class");
    }
    for (const auto& vertex : oakMesh.leafVertices) {
        require(finite(vertex.position) && finite(vertex.normal) && std::isfinite(vertex.material),
                "oak leaf vertex contains non-finite geometry or material data");
        require(std::abs(vertex.material - 1.0f) <= 1.0e-6f,
                "oak leaf vertex lost the English-oak foliage material class");
    }

    const float trunkRadius = oakNodes.front().radius;
    const size_t firstSegmentSides = 48;
    double basalRadiusSum = 0;
    float basalRadiusMaximum = 0;
    for (size_t k = 0; k < firstSegmentSides; ++k) {
        const auto& position = oakMesh.branchVertices[k].position;
        const float radius = std::sqrt(position.x * position.x + position.z * position.z);
        basalRadiusSum += radius;
        basalRadiusMaximum = std::max(basalRadiusMaximum, radius);
    }
    const float basalRadiusMean = static_cast<float>(basalRadiusSum / firstSegmentSides);
    require(basalRadiusMean > trunkRadius * 1.30f && basalRadiusMaximum > trunkRadius * 1.44f,
            "oak root collar lost its broad, irregular basal flare");

    dense::Vec3 rootMinimum = oakMesh.branchVertices[biologicalBranchVertexCount].position;
    dense::Vec3 rootMaximum = rootMinimum;
    float maximumRootReach = 0;
    for (size_t i = biologicalBranchVertexCount; i < oakMesh.branchVertices.size(); ++i) {
        const auto& position = oakMesh.branchVertices[i].position;
        rootMinimum.x = std::min(rootMinimum.x, position.x);
        rootMinimum.y = std::min(rootMinimum.y, position.y);
        rootMinimum.z = std::min(rootMinimum.z, position.z);
        rootMaximum.x = std::max(rootMaximum.x, position.x);
        rootMaximum.y = std::max(rootMaximum.y, position.y);
        rootMaximum.z = std::max(rootMaximum.z, position.z);
        maximumRootReach = std::max(maximumRootReach,
                                    std::sqrt(position.x * position.x + position.z * position.z));
    }
    const float rootHorizontalSpan = std::max(rootMaximum.x - rootMinimum.x,
                                              rootMaximum.z - rootMinimum.z);
    const float rootVerticalSpan = rootMaximum.y - rootMinimum.y;
    require(maximumRootReach > trunkRadius * 2.15f && rootHorizontalSpan > trunkRadius * 4.1f,
            "mesh-only woody roots no longer spread beyond the root collar");
    require(rootMinimum.y > -.20f && rootMaximum.y < .45f &&
                rootVerticalSpan < rootHorizontalSpan * .24f,
            "mesh-only woody roots are no longer broad and shallow at the ground plane");

    validateTriangles(oakMesh.branchVertices, oakMesh.branchIndices, "oak branch/root mesh");
    validateTriangles(oakMesh.leafVertices, oakMesh.leafIndices, "oak leaf mesh");

    dense::Vec3 minimum = oakNodes.front().position;
    dense::Vec3 maximum = minimum;
    std::array<size_t, 5> orders{};
    std::array<size_t, 5> terminals{};
    std::array<size_t, 16> outerShootSectors{};
    std::vector<float> primaryAzimuths;
    std::vector<float> primaryAttachmentHeights;
    size_t currentYearShoots = 0;
    size_t leafBearingShoots = 0;

    for (size_t i = 0; i < oakNodes.size(); ++i) {
        const auto& node = oakNodes[i];
        require(finite(node.position) && finite(node.direction) && std::isfinite(node.radius),
                "mature oak contains non-finite node data");
        require(node.axisOrder >= 0 && node.axisOrder <= 4,
                "mature oak contains an invalid botanical axis order");
        if (i) {
            require(node.parent >= 0 && static_cast<size_t>(node.parent) < i,
                    "mature oak branch parent must precede its child");
        }

        minimum.x = std::min(minimum.x, node.position.x);
        minimum.y = std::min(minimum.y, node.position.y);
        minimum.z = std::min(minimum.z, node.position.z);
        maximum.x = std::max(maximum.x, node.position.x);
        maximum.y = std::max(maximum.y, node.position.y);
        maximum.z = std::max(maximum.z, node.position.z);
        orders[static_cast<size_t>(node.axisOrder)]++;
        if (node.children == 0) terminals[static_cast<size_t>(node.axisOrder)]++;

        if (i && node.axisOrder == 1 && oakNodes[static_cast<size_t>(node.parent)].axisOrder == 0) {
            primaryAzimuths.push_back(std::atan2(node.direction.z, node.direction.x));
            primaryAttachmentHeights.push_back(oakNodes[static_cast<size_t>(node.parent)].position.y);
        }

        if (node.currentYear) {
            ++currentYearShoots;
            require(node.alive, "current-year oak shoot was marked dead before leaf attachment");
            require(node.axisOrder == 4 && node.children == 0,
                    "current-year oak foliage must terminate a fourth-order shoot");
            require(node.growthUnitStart == node.parent && node.parent >= 0,
                    "current-year oak shoot has no real parent growth-unit segment");
            require(node.birthSeason > 0, "current-year oak shoot has no birth season");
            const auto& start = oakNodes[static_cast<size_t>(node.growthUnitStart)];
            require(start.axisOrder == 3,
                    "current-year oak shoot must arise from a surviving third-order twig");
            const float shootLength = dense::length(node.position - start.position);
            require(shootLength >= .020f && shootLength <= .085f,
                    "current-year fourth-order shoot falls outside the measured compact length envelope");

            const float horizontalRadius = std::sqrt(node.position.x * node.position.x +
                                                     node.position.z * node.position.z);
            if (horizontalRadius > oak.crownRadius * .45f) {
                float angle = std::atan2(node.position.z, node.position.x);
                if (angle < 0) angle += 2 * dense::pi;
                size_t sector = static_cast<size_t>(angle / (2 * dense::pi) * outerShootSectors.size());
                sector = std::min(sector, outerShootSectors.size() - 1);
                outerShootSectors[sector]++;
            }
        }

        // This exactly mirrors the foliage eligibility contract in buildMesh.
        if (node.alive && node.axisOrder == 4 && node.children == 0 && node.radius <= .0048f) {
            ++leafBearingShoots;
            require(node.currentYear && node.growthUnitStart == node.parent,
                    "leaf-bearing oak terminal is not a real current-year growth unit");
        }
    }

    const float spanX = maximum.x - minimum.x;
    const float treeHeight = maximum.y - minimum.y;
    const float spanZ = maximum.z - minimum.z;
    const float crownMajor = std::max(spanX, spanZ);
    const float crownMinor = std::min(spanX, spanZ);
    require(treeHeight >= (oak.trunkHeight + oak.crownHeight) * .84f &&
                treeHeight <= (oak.trunkHeight + oak.crownHeight) * 1.30f,
            "mature oak height escaped its biological envelope");
    require(crownMajor / treeHeight >= 1.40f && crownMajor / treeHeight <= 2.30f,
            "open-grown oak crown must be broad rather than columnar or implausibly flat");
    require(crownMinor / treeHeight >= 1.15f,
            "open-grown oak must spread substantially in both horizontal dimensions");
    require(crownMajor / crownMinor <= 1.55f,
            "open-grown oak crown became an implausibly narrow one-axis fan");

    require(primaryAttachmentHeights.size() >= 5 && primaryAttachmentHeights.size() <= 16,
            "mature oak should have a small population of major reiterating limbs");
    const float firstPrimaryHeight = *std::min_element(primaryAttachmentHeights.begin(),
                                                       primaryAttachmentHeights.end());
    require(firstPrimaryHeight >= 1.60f && firstPrimaryHeight <= 2.50f,
            "mature open-grown oak must fork low without becoming a ground-level starburst");
    std::sort(primaryAttachmentHeights.begin(), primaryAttachmentHeights.end());
    size_t distinctAttachmentLevels = 0;
    float lastAttachment = -std::numeric_limits<float>::infinity();
    for (float height : primaryAttachmentHeights) {
        if (height - lastAttachment > .15f) {
            ++distinctAttachmentLevels;
            lastAttachment = height;
        }
    }
    require(distinctAttachmentLevels >= 3,
            "major oak limbs must emerge sequentially, not as radial arms from one junction");

    require(primaryAzimuths.size() == primaryAttachmentHeights.size(),
            "major-limb direction and attachment accounting diverged");
    std::sort(primaryAzimuths.begin(), primaryAzimuths.end());
    float largestPrimaryGap = 0;
    float smallestPrimaryGap = 2 * dense::pi;
    for (size_t i = 0; i < primaryAzimuths.size(); ++i) {
        const float next = i + 1 < primaryAzimuths.size()
                               ? primaryAzimuths[i + 1]
                               : primaryAzimuths.front() + 2 * dense::pi;
        const float gap = next - primaryAzimuths[i];
        largestPrimaryGap = std::max(largestPrimaryGap, gap);
        smallestPrimaryGap = std::min(smallestPrimaryGap, gap);
    }
    const float meanPrimaryGap = 2 * dense::pi / static_cast<float>(primaryAzimuths.size());
    require(largestPrimaryGap > meanPrimaryGap * 1.35f && largestPrimaryGap < 2.45f,
            "major oak limbs reverted to an even radial whorl or a one-sided fan");
    require(smallestPrimaryGap > .10f,
            "multiple major oak limbs collapsed onto the same radial direction");

    require(currentYearShoots > 0 && currentYearShoots == leafBearingShoots,
            "oak foliage set and current-year growth-unit set must match exactly");
    const uint64_t minimumLeaves = static_cast<uint64_t>(leafBearingShoots) * 8;
    const uint64_t maximumLeaves = static_cast<uint64_t>(leafBearingShoots) * 14;
    require(oakMesh.leafCount >= minimumLeaves && oakMesh.leafCount <= maximumLeaves,
            "oak foliage must contain 8-14 leaves per surviving current-year shoot");
    const float meanLeafAreaCm2 = oakMesh.totalLeafAreaM2 * 10'000.0f /
                                  static_cast<float>(oakMesh.leafCount);
    require(std::isfinite(meanLeafAreaCm2) && meanLeafAreaCm2 >= 8.0f && meanLeafAreaCm2 <= 23.0f,
            "oak mean leaf area escaped the Q. robur lamina envelope");

    size_t occupiedSectors = 0;
    size_t sectorMaximum = 0;
    double sectorSum = 0;
    for (size_t count : outerShootSectors) {
        occupiedSectors += count != 0;
        sectorMaximum = std::max(sectorMaximum, count);
        sectorSum += static_cast<double>(count);
    }
    require(occupiedSectors >= 12,
            "mature oak foliage no longer occupies a broadly spreading crown");
    const double sectorMean = sectorSum / outerShootSectors.size();
    double sectorVariance = 0;
    for (size_t count : outerShootSectors) {
        const double delta = static_cast<double>(count) - sectorMean;
        sectorVariance += delta * delta;
    }
    sectorVariance /= outerShootSectors.size();
    const double sectorCv = std::sqrt(sectorVariance) / sectorMean;
    require(sectorCv >= .10 && sectorCv <= 1.20,
            "outer oak foliage must be irregularly occupied without collapsing into one sector");
    require(static_cast<double>(sectorMaximum) < sectorMean * 3.0,
            "one foliage sector monopolized the mature oak crown");

    for (size_t order = 0; order < orders.size(); ++order) {
        require(orders[order] > 0, "mature oak is missing a required axis order");
    }
    require(terminals[4] == currentYearShoots,
            "fourth-order oak terminals must correspond to explicit current-year shoots");

    std::cout << "oak nodes=" << oakNodes.size() << " leaves=" << oakMesh.leafCount
              << " leafArea=" << oakMesh.totalLeafAreaM2 << "m2 extent=" << spanX << 'x'
              << treeHeight << 'x' << spanZ << " width/height=" << crownMajor / treeHeight
              << " fork=" << firstPrimaryHeight << "m attachmentLevels=" << distinctAttachmentLevels
              << " sectorCV=" << sectorCv << " basalFlare=" << basalRadiusMean / trunkRadius
              << "x rootSpan=" << rootHorizontalSpan << 'x' << rootVerticalSpan
              << "m leaves/shoot="
              << static_cast<double>(oakMesh.leafCount) / leafBearingShoots << " orders="
              << orders[0] << ',' << orders[1] << ',' << orders[2] << ',' << orders[3] << ','
              << orders[4] << '\n';
    std::cout << "nodes=" << a.size() << " branchTriangles=" << mesh.branchIndices.size() / 3
              << " leafTriangles=" << mesh.leafIndices.size() / 3 << '\n';
}
