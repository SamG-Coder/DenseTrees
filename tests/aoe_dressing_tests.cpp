#include "aoe_dressing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string_view>
#include <tuple>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr<<"FAIL: "<<message<<'\n';
    std::exit(EXIT_FAILURE);
}

void require(bool condition,std::string_view message) {
    if(!condition)fail(message);
}

bool near(float a,float b,float tolerance=1.0e-7f) {
    return std::isfinite(a)&&std::abs(a-b)<=tolerance;
}

float pointSegmentDistanceSquared(dense::Vec3 point,dense::Vec3 start,
                                  dense::Vec3 end) {
    const float dx=end.x-start.x,dz=end.z-start.z;
    const float denominator=dx*dx+dz*dz;
    const float t=denominator<1.0e-6f?0.0f:std::clamp(
        ((point.x-start.x)*dx+(point.z-start.z)*dz)/denominator,0.0f,1.0f);
    const float px=point.x-(start.x+dx*t),pz=point.z-(start.z+dz*t);
    return px*px+pz*pz;
}

void checkSourceParity() {
    using namespace dense;
    require(near(AoeWorldDressing::sourceSpawnChance(
                     AoeWorldBiome::TemperateForest,4),.23f),
            "temperate-forest chance differs from source");
    require(near(AoeWorldDressing::sourceSpawnChance(
                     AoeWorldBiome::Rainforest,4),.31f),
            "rainforest chance differs from source");
    require(near(AoeWorldDressing::sourceSpawnChance(
                     AoeWorldBiome::Alpine,10),.0225f),
            "alpine elevation attenuation differs from source");
    require(AoeWorldDressing::sourceSpawnChance(
                AoeWorldBiome::TemperateGrassland,4)==0,
            "grassland must retain source zero tree chance");

    const auto temperate=AoeWorldDressing::sourceTreeAt(8675309,-20,-30,
        AoeWorldBiome::TemperateForest,AoeBiome::Forest,4);
    require(temperate.spawned&&near(temperate.spawnRoll,.123268306f)&&
            temperate.family==AoeTreeFamily::Oak&&temperate.sourceFrame==5,
            "spawn/family/frame salts differ from C# temperate oracle");
    const auto rainforest=AoeWorldDressing::sourceTreeAt(8675309,-24,-30,
        AoeWorldBiome::Rainforest,AoeBiome::JungleFloor,4);
    require(rainforest.spawned&&rainforest.family==AoeTreeFamily::Jungle&&
            rainforest.sourceFrame==3,
            "rainforest family/frame differ from C# oracle");
    const auto wetland=AoeWorldDressing::sourceTreeAt(8675309,-20,-30,
        AoeWorldBiome::Wetland,AoeBiome::Mud,4);
    require(wetland.spawned&&wetland.family==AoeTreeFamily::Bamboo&&
            wetland.sourceFrame==1,
            "wetland family/frame differ from C# oracle");
    const auto savanna=AoeWorldDressing::sourceTreeAt(8675309,20,-30,
        AoeWorldBiome::Savanna,AoeBiome::DryGrass,4);
    require(savanna.spawned&&savanna.family==AoeTreeFamily::GenericJ&&
            savanna.sourceFrame==0,
            "savanna generic family salt differs from C# oracle");
    const auto desert=AoeWorldDressing::sourceTreeAt(8675309,26,-30,
        AoeWorldBiome::Desert,AoeBiome::DesertSand,4);
    require(desert.spawned&&desert.family==AoeTreeFamily::Cactus&&
            desert.sourceFrame==3,
            "desert family/frame differ from C# oracle");
    const auto snow=AoeWorldDressing::sourceTreeAt(8675309,5,-30,
        AoeWorldBiome::Alpine,AoeBiome::Snow,4);
    require(snow.spawned&&snow.family==AoeTreeFamily::SnowConifer&&
            snow.sourceFrame==4,
            "snow-alpine family selection differs from source");
    require(AoeWorldDressing::metadata(AoeTreeFamily::Oak).sourceVariantCount==14&&
            AoeWorldDressing::metadata(AoeTreeFamily::Bamboo).sourceVariantCount==4&&
            AoeWorldDressing::metadata(AoeTreeFamily::GenericL).sourceVariantCount==1,
            "source frame-count metadata differs from catalogue");
    require(temperate.provenance==AoeDressingProvenance::SourceParity&&
            AoeWorldDressing::metadata(AoeTreeFamily::Oak).geometryProvenance==
                AoeDressingProvenance::Native3DExtension,
            "source selection and native geometry provenance are conflated");
}

dense::AoeDressingSample syntheticSample(int x,int z) {
    using namespace dense;
    AoeDressingSample sample{};
    sample.position={x+.5f,.16f*std::sin(x*.09f)*std::cos(z*.07f),z+.5f};
    sample.normal={0,1,0};
    sample.elevation=4;
    sample.traversable=true;
    if(x<-20) {
        sample.biome=AoeBiome::JungleFloor;
        sample.region=AoeWorldBiome::Rainforest;
        sample.forestInterior=1;
    } else if(x>50) {
        sample.biome=AoeBiome::DesertSand;
        sample.region=AoeWorldBiome::Desert;
        sample.forestInterior=0;
    } else if(x>34) {
        sample.biome=AoeBiome::Highland;
        sample.region=AoeWorldBiome::Alpine;
        sample.forestInterior=0;
    } else if(z>35) {
        sample.biome=AoeBiome::Beach;
        sample.region=AoeWorldBiome::Coast;
        sample.forestInterior=0;
    } else if(z<-34) {
        sample.biome=AoeBiome::Mud;
        sample.region=AoeWorldBiome::Wetland;
        sample.forestInterior=.72f;
    } else {
        sample.biome=AoeBiome::Forest;
        sample.region=AoeWorldBiome::TemperateForest;
        sample.forestInterior=std::clamp((20.0f-std::abs(static_cast<float>(x)))/
                                         20.0f,0.0f,1.0f);
    }
    return sample;
}

void checkNativeDressing() {
    using namespace dense;
    AoeDressingConfig config{};
    config.seed=8675309;
    config.minimumSourceX=-64;config.maximumSourceX=64;
    config.minimumSourceZ=-64;config.maximumSourceZ=64;
    config.spawnSourceX=0;config.spawnSourceZ=0;
    const AoeDressingSampler sampler=[](int x,int z) {
        return syntheticSample(x,z);
    };
    const AoeDressingResult first=AoeWorldDressing::generate(config,sampler);
    const AoeDressingResult repeated=AoeWorldDressing::generate(config,sampler);
    require(!first.trees.empty(),"native dressing emitted no trees");
    require(!first.detailVertices.empty()&&!first.detailIndices.empty(),
            "native archetypes emitted no geometry");
    require(first.trees.size()==repeated.trees.size()&&
            first.features.size()==repeated.features.size()&&
            first.trails.size()==repeated.trails.size()&&
            first.detailVertices.size()==repeated.detailVertices.size()&&
            first.detailIndices==repeated.detailIndices,
            "same seed and bounds must reproduce dressing");
    for(std::size_t index=0;index<first.trees.size();++index) {
        const auto& a=first.trees[index];
        const auto& b=repeated.trees[index];
        require(a.sourceX==b.sourceX&&a.sourceZ==b.sourceZ&&
                a.family==b.family&&a.sourceFrame==b.sourceFrame&&
                near(a.position.x,b.position.x)&&near(a.position.z,b.position.z),
                "deterministic tree metadata or placement drifted");
    }
    for(std::uint32_t index:first.detailIndices)
        require(index<first.detailVertices.size(),"dressing index is out of range");
    for(const MeshVertex& vertex:first.detailVertices) {
        require(std::isfinite(vertex.position.x)&&
                std::isfinite(vertex.position.y)&&
                std::isfinite(vertex.position.z)&&
                vertex.position.x>-70.0f&&vertex.position.x<70.0f&&
                vertex.position.z>-70.0f&&vertex.position.z<70.0f&&
                vertex.position.y>-.5f&&vertex.position.y<18.0f,
                "native dressing emitted implausibly unbounded geometry");
    }

    std::set<NativeTreeArchetype> archetypes;
    for(const auto& tree:first.trees)archetypes.insert(tree.archetype);
    require(archetypes.size()>=6,
            "regional native dressing did not produce varied archetypes");

    for(const auto& tree:first.trees) {
        require(tree.geometryVertexCount>0&&tree.geometryIndexCount>0&&
                tree.geometryFirstVertex+tree.geometryVertexCount<=
                    first.detailVertices.size()&&
                tree.geometryFirstIndex+tree.geometryIndexCount<=
                    first.detailIndices.size(),
                "tree geometry range is missing or invalid");
        require(tree.geometryVertexCount<=128&&tree.geometryIndexCount<=800,
                "tree archetype exceeded its bounded low-poly budget");

        dense::Vec3 minimum{1.0e9f,1.0e9f,1.0e9f};
        dense::Vec3 maximum{-1.0e9f,-1.0e9f,-1.0e9f};
        dense::Vec3 foliageMinimum=minimum,foliageMaximum=maximum;
        std::size_t foliageVertices=0;
        for(std::uint32_t offset=0;offset<tree.geometryVertexCount;++offset) {
            const auto& vertex=first.detailVertices[
                tree.geometryFirstVertex+offset];
            minimum.x=std::min(minimum.x,vertex.position.x);
            minimum.y=std::min(minimum.y,vertex.position.y);
            minimum.z=std::min(minimum.z,vertex.position.z);
            maximum.x=std::max(maximum.x,vertex.position.x);
            maximum.y=std::max(maximum.y,vertex.position.y);
            maximum.z=std::max(maximum.z,vertex.position.z);
            if(vertex.material>=4.0f&&vertex.material<5.0f) {
                ++foliageVertices;
                foliageMinimum.x=std::min(foliageMinimum.x,vertex.position.x);
                foliageMinimum.y=std::min(foliageMinimum.y,vertex.position.y);
                foliageMinimum.z=std::min(foliageMinimum.z,vertex.position.z);
                foliageMaximum.x=std::max(foliageMaximum.x,vertex.position.x);
                foliageMaximum.y=std::max(foliageMaximum.y,vertex.position.y);
                foliageMaximum.z=std::max(foliageMaximum.z,vertex.position.z);
            }
        }
        const float width=maximum.x-minimum.x;
        const float depth=maximum.z-minimum.z;
        const float visualHeight=maximum.y-minimum.y;
        require(width>.12f&&depth>.12f&&visualHeight>tree.height*.55f,
                "tree silhouette collapsed to a pole or flat primitive");
        if(tree.archetype!=NativeTreeArchetype::Cactus) {
            require(foliageVertices>=12,
                    "foliated archetype has too little closed canopy geometry");
            const float crownWidth=foliageMaximum.x-foliageMinimum.x;
            const float crownDepth=foliageMaximum.z-foliageMinimum.z;
            const float crownHeight=foliageMaximum.y-foliageMinimum.y;
            require(crownWidth>tree.crownRadius*.80f&&
                    crownDepth>tree.crownRadius*.80f&&
                    crownHeight>tree.height*.10f,
                    "canopy lacks three-dimensional thickness or extent");
            if(tree.archetype==NativeTreeArchetype::Palm)
                require(crownWidth>tree.height*.32f&&
                        crownDepth>tree.height*.32f,
                        "palm fronds do not form a broad radial silhouette");
            if(tree.archetype==NativeTreeArchetype::Conifer||
               tree.archetype==NativeTreeArchetype::SnowConifer)
                require(crownHeight>tree.height*.48f&&
                        crownWidth<tree.height*.55f&&crownDepth<tree.height*.55f,
                        "conifer lost its tall, tapered silhouette");
        } else {
            require(width>tree.height*.10f&&depth>tree.height*.10f,
                    "cactus lacks a rotated volumetric side arm");
        }
    }

    for(std::size_t a=0;a<first.trees.size();++a) {
        for(std::size_t b=a+1;b<first.trees.size();++b) {
            const float dx=first.trees[a].position.x-first.trees[b].position.x;
            const float dz=first.trees[a].position.z-first.trees[b].position.z;
            require(dx*dx+dz*dz>=1.16f,
                    "deterministic spacing retained an overlapping tree pair");
        }
    }

    std::size_t fringeCandidates=0,fringeKept=0,interiorCandidates=0,
                interiorKept=0;
    for(int z=-32;z<32;++z)for(int x=-19;x<20;++x) {
        const AoeDressingSample sample=syntheticSample(x,z);
        const auto source=AoeWorldDressing::sourceTreeAt(config.seed,x,z,
            sample.region,sample.biome,sample.elevation);
        if(!source.spawned)continue;
        if(sample.forestInterior<.20f)++fringeCandidates;
        if(sample.forestInterior>.80f)++interiorCandidates;
    }
    for(const auto& tree:first.trees) {
        if(tree.sourceX<-19||tree.sourceX>=20||tree.sourceZ<-32||tree.sourceZ>=32)
            continue;
        if(tree.forestInterior<.20f)++fringeKept;
        if(tree.forestInterior>.80f)++interiorKept;
    }
    require(fringeCandidates>0&&interiorCandidates>0&&fringeKept*interiorCandidates<
            interiorKept*fringeCandidates,
            "forest fringe is not thinner than forest interior");

    const auto featureCount=[&](AoeWorldFeatureKind kind) {
        return std::count_if(first.features.begin(),first.features.end(),
            [kind](const AoeWorldFeature3D& feature) {return feature.kind==kind;});
    };
    require(featureCount(AoeWorldFeatureKind::StarterCamp)==1&&
            featureCount(AoeWorldFeatureKind::CampfireInteraction)==1&&
            featureCount(AoeWorldFeatureKind::SpawnMarker)==1&&
            featureCount(AoeWorldFeatureKind::QuestMarker)==1,
            "starter camp and gameplay marker contract is incomplete");
    require(featureCount(AoeWorldFeatureKind::ForestGrove)>=1&&
            featureCount(AoeWorldFeatureKind::StandingStones)>=1&&
            featureCount(AoeWorldFeatureKind::CoastalBeacon)>=1&&
            featureCount(AoeWorldFeatureKind::WetlandTotem)>=1,
            "biome-appropriate landmark set is incomplete");
    require(!first.trails.empty(),"landmarks were not linked by trails");
    require(std::all_of(first.features.begin(),first.features.end(),
        [](const AoeWorldFeature3D& value) {
            return value.provenance==AoeDressingProvenance::Native3DExtension;
        }),"extension feature was mislabeled as source parity");

    for(const AoeTrail3D& trail:first.trails) {
        require(trail.points.size()>10,"trail did not sample continuously");
        for(const Vec3 point:trail.points) {
            const auto sample=syntheticSample(static_cast<int>(std::floor(point.x)),
                                              static_cast<int>(std::floor(point.z)));
            require(sample.traversable&&sample.biome!=AoeBiome::DeepWater&&
                    sample.biome!=AoeBiome::ShallowWater&&
                    sample.biome!=AoeBiome::RiverWater,
                    "trail crossed water or non-traversable ground");
        }
    }

    for(const auto& tree:first.trees) {
        for(const auto& feature:first.features) {
            const bool physical=feature.kind==AoeWorldFeatureKind::StarterCamp||
                feature.kind==AoeWorldFeatureKind::ForestGrove||
                feature.kind==AoeWorldFeatureKind::StandingStones||
                feature.kind==AoeWorldFeatureKind::CoastalBeacon||
                feature.kind==AoeWorldFeatureKind::WetlandTotem;
            if(!physical)continue;
            const float dx=tree.position.x-feature.position.x;
            const float dz=tree.position.z-feature.position.z;
            const float clearance=feature.radius+.55f;
            require(dx*dx+dz*dz>=clearance*clearance,
                    "tree overlaps a camp or landmark clearance radius");
        }
        for(const auto& trail:first.trails)for(std::size_t point=0;
            point+1<trail.points.size();++point) {
            const float clearance=trail.halfWidth+.45f;
            require(pointSegmentDistanceSquared(tree.position,trail.points[point],
                    trail.points[point+1])>=clearance*clearance,
                    "tree overlaps a trail clearance radius");
        }
    }

    EnvironmentMesh mesh{};
    mesh.detailVertices.push_back({});
    mesh.detailIndices.push_back(0);
    mesh.backgroundTreeCount=7;
    first.appendGeometryTo(mesh);
    require(mesh.detailVertices.size()==first.detailVertices.size()+1&&
            mesh.detailIndices.size()==first.detailIndices.size()+1&&
            mesh.backgroundTreeCount==7+first.trees.size(),
            "environment-mesh append did not preserve offsets or inventory");
    for(std::size_t index=1;index<mesh.detailIndices.size();++index)
        require(mesh.detailIndices[index]>0&&
                mesh.detailIndices[index]<mesh.detailVertices.size(),
                "environment-mesh appended index was not rebased");
}

void checkUnsafeTrailOmission() {
    using namespace dense;
    AoeDressingConfig config{};
    config.seed=8675309;
    config.minimumSourceX=-64;config.maximumSourceX=64;
    config.minimumSourceZ=-64;config.maximumSourceZ=64;
    const AoeDressingSampler sampler=[](int x,int z) {
        AoeDressingSample sample=syntheticSample(x,z);
        if(x>=8&&x<=12) {
            sample.biome=AoeBiome::DeepWater;
            sample.region=AoeWorldBiome::Ocean;
            sample.traversable=false;
            sample.position.y=-1;
        }
        return sample;
    };
    const auto result=AoeWorldDressing::generate(config,sampler);
    for(const auto& trail:result.trails)for(const Vec3 point:trail.points)
        require(point.x<8||point.x>=13,
                "unsafe trail crossing was retained instead of omitted");
}

} // namespace

int main() {
    checkSourceParity();
    checkNativeDressing();
    checkUnsafeTrailOmission();
    std::cout<<"AOE world dressing tests passed\n";
    return EXIT_SUCCESS;
}
