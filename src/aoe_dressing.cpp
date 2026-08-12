#include "aoe_dressing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace dense {
namespace {

constexpr std::array<AoeTreeFamilyMetadata,19> familyMetadata{{
    {AoeTreeFamily::Palm,"FPAL_NN",13,NativeTreeArchetype::Palm},
    {AoeTreeFamily::Pine,"FPIN_NN",9,NativeTreeArchetype::Conifer},
    {AoeTreeFamily::Oak,"FOAK_NN",14,NativeTreeArchetype::Broadleaf},
    {AoeTreeFamily::Jungle,"FJUN_NN",13,NativeTreeArchetype::JungleBroadleaf},
    {AoeTreeFamily::SnowConifer,"FSNO_NN",9,NativeTreeArchetype::SnowConifer},
    {AoeTreeFamily::Bamboo,"FBAM_NN",4,NativeTreeArchetype::BambooClump},
    {AoeTreeFamily::Cactus,"FCAC_NN",6,NativeTreeArchetype::Cactus},
    {AoeTreeFamily::GenericA,"TREEA_NN",1,NativeTreeArchetype::Broadleaf},
    {AoeTreeFamily::GenericB,"TREEB_NN",1,NativeTreeArchetype::Broadleaf},
    {AoeTreeFamily::GenericC,"TREEC_NN",1,NativeTreeArchetype::Conifer},
    {AoeTreeFamily::GenericD,"TREED_NN",1,NativeTreeArchetype::Broadleaf},
    {AoeTreeFamily::GenericE,"TREEE_NN",1,NativeTreeArchetype::Broadleaf},
    {AoeTreeFamily::GenericF,"TREEF_NN",1,NativeTreeArchetype::Conifer},
    {AoeTreeFamily::GenericG,"TREEG_NN",1,NativeTreeArchetype::Broadleaf},
    {AoeTreeFamily::GenericH,"TREEH_NN",1,NativeTreeArchetype::JungleBroadleaf},
    {AoeTreeFamily::GenericI,"TREEI_NN",1,NativeTreeArchetype::Broadleaf},
    {AoeTreeFamily::GenericJ,"TREEJ_NN",1,NativeTreeArchetype::Palm},
    {AoeTreeFamily::GenericK,"TREEK_NN",1,NativeTreeArchetype::Broadleaf},
    {AoeTreeFamily::GenericL,"TREEL_NN",1,NativeTreeArchetype::Conifer}
}};

std::uint64_t mixedHash(std::int64_t seed,int x,int z,int salt) {
    std::uint64_t value=static_cast<std::uint64_t>(seed)^
        static_cast<std::uint64_t>(static_cast<std::int64_t>(x))*
            0x9e3779b185ebca87ull^
        static_cast<std::uint64_t>(static_cast<std::int64_t>(z))*
            0xc2b2ae3d27d4eb4full^
        static_cast<std::uint32_t>(salt);
    value^=value>>30;value*=0xbf58476d1ce4e5b9ull;
    value^=value>>27;value*=0x94d049bb133111ebull;
    value^=value>>31;
    return value;
}

float unitHash(std::int64_t seed,int x,int z,int salt) {
    return static_cast<float>(mixedHash(seed,x,z,salt)>>40)*
           (1.0f/16777216.0f);
}

float smoothstep(float low,float high,float value) {
    const float t=clamp((value-low)/(high-low),0.0f,1.0f);
    return t*t*(3.0f-2.0f*t);
}

bool wooded(AoeWorldBiome region) {
    return region==AoeWorldBiome::TemperateForest||
           region==AoeWorldBiome::Rainforest||
           region==AoeWorldBiome::Taiga||
           region==AoeWorldBiome::Wetland;
}

AoeTreeFamily genericFamily(float roll) {
    const int variant=std::min(static_cast<int>(roll*12.0f),11);
    return static_cast<AoeTreeFamily>(
        static_cast<int>(AoeTreeFamily::GenericA)+variant);
}

AoeTreeFamily sourceFamily(std::int64_t seed,int x,int z,
                           AoeWorldBiome region,AoeBiome biome) {
    const float roll=unitHash(seed,x,z,137);
    const AoeTreeFamily generic=genericFamily(unitHash(seed,x,z,149));
    switch(region) {
    case AoeWorldBiome::Coast:return AoeTreeFamily::Palm;
    case AoeWorldBiome::Savanna:
        return roll<.62f?AoeTreeFamily::Palm:generic;
    case AoeWorldBiome::Rainforest:
        return roll<.72f?AoeTreeFamily::Jungle:
               (roll<.88f?AoeTreeFamily::Bamboo:generic);
    case AoeWorldBiome::TemperateForest:
        return roll<.72f?AoeTreeFamily::Oak:generic;
    case AoeWorldBiome::Wetland:
        return roll<.55f?AoeTreeFamily::Bamboo:
               (roll<.82f?AoeTreeFamily::Jungle:generic);
    case AoeWorldBiome::Taiga:return AoeTreeFamily::Pine;
    case AoeWorldBiome::Tundra:return AoeTreeFamily::SnowConifer;
    case AoeWorldBiome::Alpine:
        return biome==AoeBiome::Snow?AoeTreeFamily::SnowConifer:
                                     AoeTreeFamily::Pine;
    case AoeWorldBiome::Desert:return AoeTreeFamily::Cactus;
    default:return generic;
    }
}

std::uint32_t packColor(float r,float g,float b) {
    const auto byte=[](float value) {return static_cast<std::uint32_t>(
        clamp(value,0.0f,1.0f)*255.0f+.5f);};
    return 0xff000000u|(byte(b)<<16)|(byte(g)<<8)|byte(r);
}

struct GeometryWriter {
    std::vector<MeshVertex>& vertices;
    std::vector<std::uint32_t>& indices;

    void cylinder(Vec3 base,float height,float bottomRadius,float topRadius,
                  int sides,std::uint32_t color,float material) {
        const std::uint32_t first=static_cast<std::uint32_t>(vertices.size());
        for(int layer=0;layer<2;++layer)for(int side=0;side<sides;++side) {
            const float angle=2.0f*pi*static_cast<float>(side)/sides;
            const Vec3 radial{std::cos(angle),0,std::sin(angle)};
            const float radius=layer==0?bottomRadius:topRadius;
            vertices.push_back({base+Vec3{radial.x*radius,
                layer==0?0.0f:height,radial.z*radius},radial,color,material,
                static_cast<float>(side)/sides,static_cast<float>(layer)});
        }
        for(int side=0;side<sides;++side) {
            const std::uint32_t next=static_cast<std::uint32_t>((side+1)%sides);
            const std::uint32_t current=static_cast<std::uint32_t>(side);
            indices.insert(indices.end(),{first+current,first+next,
                first+sides+current,first+next,first+sides+next,
                first+sides+current});
        }
    }

    void branch(Vec3 start,Vec3 end,float radius,int sides,
                std::uint32_t color,float material) {
        const Vec3 axis=normalize(end-start);
        const Vec3 helper=std::abs(axis.y)<.88f?Vec3{0,1,0}:Vec3{1,0,0};
        const Vec3 tangent=normalize(cross(helper,axis));
        const Vec3 bitangent=normalize(cross(axis,tangent));
        const std::uint32_t first=static_cast<std::uint32_t>(vertices.size());
        for(int layer=0;layer<2;++layer)for(int side=0;side<sides;++side) {
            const float angle=2*pi*static_cast<float>(side)/sides;
            const Vec3 radial=tangent*std::cos(angle)+bitangent*std::sin(angle);
            vertices.push_back({(layer==0?start:end)+radial*radius,radial,
                                color,material,static_cast<float>(side)/sides,
                                static_cast<float>(layer)});
        }
        for(int side=0;side<sides;++side) {
            const std::uint32_t next=static_cast<std::uint32_t>((side+1)%sides);
            const std::uint32_t current=static_cast<std::uint32_t>(side);
            indices.insert(indices.end(),{first+current,first+next,
                first+sides+current,first+next,first+sides+next,
                first+sides+current});
        }
    }

    void crown(Vec3 center,float radiusX,float radiusZ,float halfHeight,
               std::uint32_t color,float material,int sides=6,float yaw=0) {
        const std::uint32_t first=static_cast<std::uint32_t>(vertices.size());
        vertices.push_back({center+Vec3{0,halfHeight,0},{0,1,0},color,
                            material,.5f,1.0f});
        vertices.push_back({center-Vec3{0,halfHeight*.82f,0},{0,-1,0},color,
                            material,.5f,0.0f});
        constexpr std::array<float,3> ringY{{-.50f,.02f,.56f}};
        constexpr std::array<float,3> ringRadius{{.79f,1.0f,.77f}};
        for(int ring=0;ring<3;++ring)for(int side=0;side<sides;++side) {
            const float angle=yaw+2.0f*pi*static_cast<float>(side)/sides;
            const Vec3 local{std::cos(angle)*radiusX*ringRadius[ring],
                halfHeight*ringY[ring],std::sin(angle)*radiusZ*ringRadius[ring]};
            const Vec3 normal=normalize(Vec3{local.x/(radiusX*radiusX),
                local.y/(halfHeight*halfHeight),local.z/(radiusZ*radiusZ)});
            vertices.push_back({center+local,normal,color,material,
                .5f+.5f*std::cos(angle),.5f+.5f*std::sin(angle)});
        }
        for(int side=0;side<sides;++side) {
            const std::uint32_t next=static_cast<std::uint32_t>((side+1)%sides);
            const std::uint32_t lower=first+2+static_cast<std::uint32_t>(side);
            const std::uint32_t lowerNext=first+2+next;
            indices.insert(indices.end(),{first+1,lowerNext,lower});
            for(int ring=0;ring<2;++ring) {
                const std::uint32_t a=first+2+ring*sides+
                    static_cast<std::uint32_t>(side);
                const std::uint32_t b=first+2+ring*sides+next;
                const std::uint32_t c=a+static_cast<std::uint32_t>(sides);
                const std::uint32_t d=b+static_cast<std::uint32_t>(sides);
                indices.insert(indices.end(),{a,b,c,b,d,c});
            }
            const std::uint32_t upper=first+2+2*sides+
                static_cast<std::uint32_t>(side);
            const std::uint32_t upperNext=first+2+2*sides+next;
            indices.insert(indices.end(),{first,upper,upperNext});
        }
    }

    void coniferTier(Vec3 base,float radius,float height,std::uint32_t color,
                     float material,int sides=6,float yaw=0) {
        const std::uint32_t first=static_cast<std::uint32_t>(vertices.size());
        vertices.push_back({base+Vec3{0,height,0},{0,1,0},color,material,.5f,1});
        vertices.push_back({base,{0,-1,0},color,material,.5f,0});
        constexpr std::array<float,2> ringY{{.10f,.56f}};
        constexpr std::array<float,2> ringRadius{{1.0f,.54f}};
        for(int ring=0;ring<2;++ring)for(int side=0;side<sides;++side) {
            const float angle=yaw+2*pi*static_cast<float>(side)/sides;
            const Vec3 radial{std::cos(angle),.34f,std::sin(angle)};
            vertices.push_back({base+Vec3{radial.x*radius*ringRadius[ring],
                height*ringY[ring],radial.z*radius*ringRadius[ring]},
                normalize(radial),color,material,.5f+.5f*radial.x,
                .5f+.5f*radial.z});
        }
        for(int side=0;side<sides;++side) {
            const std::uint32_t next=static_cast<std::uint32_t>((side+1)%sides);
            const std::uint32_t lower=first+2+static_cast<std::uint32_t>(side);
            const std::uint32_t lowerNext=first+2+next;
            const std::uint32_t upper=lower+static_cast<std::uint32_t>(sides);
            const std::uint32_t upperNext=lowerNext+static_cast<std::uint32_t>(sides);
            indices.insert(indices.end(),{first+1,lowerNext,lower,
                lower,lowerNext,upper,lowerNext,upperNext,upper,
                first,upper,upperNext});
        }
    }

    void frond(Vec3 root,float angle,float length,float width,float drop,
               std::uint32_t color,float material) {
        const Vec3 forward{std::cos(angle),0,std::sin(angle)};
        const Vec3 lateral{-forward.z,0,forward.x};
        const std::array<Vec3,3> centers{{root,
            root+forward*(length*.56f)+Vec3{0,length*.08f,0},
            root+forward*length+Vec3{0,-drop,0}}};
        const std::array<float,2> widths{{width*.30f,width}};
        const std::array<float,2> thickness{{std::max(width*.16f,.035f),
                                             std::max(width*.11f,.025f)}};
        const std::uint32_t first=static_cast<std::uint32_t>(vertices.size());
        for(int section=0;section<2;++section) {
            const Vec3 center=centers[section];
            const float halfWidth=widths[section]*.5f;
            const float halfThickness=thickness[section]*.5f;
            vertices.push_back({center+lateral*halfWidth+Vec3{0,halfThickness,0},
                normalize(lateral+Vec3{0,.45f,0}),color,material,0,
                static_cast<float>(section)});
            vertices.push_back({center-lateral*halfWidth+Vec3{0,halfThickness,0},
                normalize(lateral*-1.0f+Vec3{0,.45f,0}),color,material,1,
                static_cast<float>(section)});
            vertices.push_back({center+Vec3{0,-halfThickness,0},{0,-1,0},
                color,material,.5f,static_cast<float>(section)});
        }
        vertices.push_back({centers[2],normalize(forward+Vec3{0,-.25f,0}),
                            color,material,.5f,1});
        for(int side=0;side<3;++side) {
            const std::uint32_t next=static_cast<std::uint32_t>((side+1)%3);
            const std::uint32_t a=first+static_cast<std::uint32_t>(side);
            const std::uint32_t b=first+next;
            const std::uint32_t c=first+3+static_cast<std::uint32_t>(side);
            const std::uint32_t d=first+3+next;
            indices.insert(indices.end(),{a,b,c,b,d,c,
                                           c,d,first+6});
        }
        indices.insert(indices.end(),{first,first+2,first+1});
    }

    void rock(Vec3 base,float radius,std::uint32_t color) {
        const std::uint32_t first=static_cast<std::uint32_t>(vertices.size());
        const std::array<Vec3,5> points{{{-radius,0,-radius*.65f},
            {radius*.9f,0,-radius*.55f},{radius*.72f,0,radius*.8f},
            {-radius*.8f,0,radius*.72f},{0,radius*.85f,0}}};
        for(const Vec3 point:points)vertices.push_back({base+point,
            normalize(Vec3{point.x,radius*.8f,point.z}),color,3.0f,0,0});
        indices.insert(indices.end(),{first,first+1,first+4,
            first+1,first+2,first+4,first+2,first+3,first+4,
            first+3,first,first+4,first,first+3,first+2,
            first,first+2,first+1});
    }
};

void appendTreeGeometry(GeometryWriter& writer,const AoeTreeInstance3D& tree,
                        std::int64_t seed) {
    const float tint=.86f+.20f*unitHash(seed,tree.sourceX,tree.sourceZ,5177);
    const float yaw=2*pi*unitHash(seed,tree.sourceX,tree.sourceZ,5191);
    const std::uint32_t bark=packColor(.22f*tint,.13f*tint,.065f*tint);
    const float trunkRadius=.075f+tree.height*.018f;
    switch(tree.archetype) {
    case NativeTreeArchetype::Broadleaf:
        writer.cylinder(tree.position,tree.height*.60f,trunkRadius,
                        trunkRadius*.62f,5,bark,5.0f);
        writer.crown(tree.position+Vec3{-tree.crownRadius*.18f,
            tree.height*.69f,0},tree.crownRadius,tree.crownRadius*.88f,
            tree.height*.20f,packColor(.18f*tint,.46f*tint,.12f*tint),4.0f,7,yaw);
        writer.crown(tree.position+Vec3{tree.crownRadius*.34f,
            tree.height*.77f,tree.crownRadius*.12f},tree.crownRadius*.72f,
            tree.crownRadius*.62f,tree.height*.16f,
            packColor(.22f*tint,.52f*tint,.14f*tint),4.0f,6,yaw+.47f);
        break;
    case NativeTreeArchetype::Conifer:
    case NativeTreeArchetype::SnowConifer: {
        writer.cylinder(tree.position,tree.height*.72f,trunkRadius,
                        trunkRadius*.42f,5,bark,5.0f);
        const bool snow=tree.archetype==NativeTreeArchetype::SnowConifer;
        const std::uint32_t color=snow?packColor(.38f*tint,.53f*tint,.43f*tint):
            packColor(.14f*tint,.34f*tint,.16f*tint);
        for(int level=0;level<3;++level) {
            const float t=static_cast<float>(level)/2.0f;
            writer.coniferTier(tree.position+Vec3{0,tree.height*(.25f+.20f*t),0},
                tree.crownRadius*(1.0f-.26f*t),tree.height*(.31f-.025f*t),
                color,4.1f,6,yaw+level*.31f);
        }
        break;
    }
    case NativeTreeArchetype::Palm: {
        writer.cylinder(tree.position,tree.height*.80f,trunkRadius*.75f,
                        trunkRadius*.48f,6,packColor(.36f*tint,.25f*tint,.10f*tint),5.1f);
        const Vec3 crown=tree.position+Vec3{0,tree.height*.80f,0};
        const std::uint32_t color=packColor(.25f*tint,.55f*tint,.10f*tint);
        writer.crown(crown+Vec3{0,tree.height*.035f,0},tree.crownRadius*.31f,
            tree.crownRadius*.31f,tree.height*.075f,color,4.2f,6,yaw);
        for(int leaf=0;leaf<8;++leaf)
            writer.frond(crown,yaw+2*pi*static_cast<float>(leaf)/8.0f,
                tree.crownRadius*(.87f+.15f*((leaf+tree.sourceFrame)%3)),
                tree.crownRadius*.30f,tree.height*(.08f+.025f*(leaf&1)),
                color,4.2f);
        break;
    }
    case NativeTreeArchetype::JungleBroadleaf:
        writer.cylinder(tree.position,tree.height*.57f,trunkRadius*1.18f,
                        trunkRadius*.64f,6,bark,5.0f);
        for(int cluster=0;cluster<3;++cluster) {
            const float angle=2*pi*static_cast<float>(cluster)/3.0f+
                unitHash(seed,tree.sourceX,tree.sourceZ,5219);
            writer.crown(tree.position+Vec3{std::cos(angle)*tree.crownRadius*.32f,
                tree.height*(.68f+.07f*cluster),std::sin(angle)*tree.crownRadius*.32f},
                tree.crownRadius*.82f,tree.crownRadius*.70f,tree.height*.19f,
                packColor(.13f*tint,(.43f+.025f*cluster)*tint,.10f*tint),
                4.0f,7,yaw+cluster*.61f);
        }
        break;
    case NativeTreeArchetype::BambooClump:
        for(int stem=0;stem<4;++stem) {
            const float angle=2*pi*static_cast<float>(stem)/4.0f;
            const Vec3 offset{std::cos(angle)*tree.crownRadius*.20f,0,
                              std::sin(angle)*tree.crownRadius*.20f};
            writer.cylinder(tree.position+offset,tree.height*(.78f+.05f*stem),
                trunkRadius*.30f,trunkRadius*.24f,5,
                packColor(.25f*tint,.43f*tint,.08f*tint),5.1f);
            writer.crown(tree.position+offset+Vec3{0,tree.height*(.72f+.05f*stem),0},
                tree.crownRadius*.55f,tree.crownRadius*.40f,tree.height*.13f,
                packColor(.17f*tint,.49f*tint,.10f*tint),4.1f,5,yaw+stem*.77f);
        }
        break;
    case NativeTreeArchetype::Cactus: {
        const std::uint32_t cactus=packColor(.25f,.46f,.16f);
        writer.cylinder(tree.position,tree.height*.78f,trunkRadius*1.25f,
                        trunkRadius*1.05f,7,cactus,4.2f);
        const Vec3 side{std::cos(yaw),0,std::sin(yaw)};
        const Vec3 branchBase=tree.position+Vec3{0,tree.height*.35f,0};
        const Vec3 elbow=branchBase+side*trunkRadius*2.8f;
        writer.branch(branchBase,elbow,trunkRadius*.72f,6,cactus,4.2f);
        writer.cylinder(elbow,tree.height*.27f,trunkRadius*.76f,
                        trunkRadius*.61f,6,cactus,4.2f);
        if(tree.sourceFrame&1) {
            const Vec3 otherBase=tree.position+Vec3{0,tree.height*.48f,0};
            const Vec3 otherElbow=otherBase-side*trunkRadius*2.2f;
            writer.branch(otherBase,otherElbow,trunkRadius*.62f,6,cactus,4.2f);
            writer.cylinder(otherElbow,tree.height*.18f,trunkRadius*.65f,
                            trunkRadius*.54f,6,cactus,4.2f);
        }
        break;
    }
    }
}

float minimumSpacing(NativeTreeArchetype archetype) {
    switch(archetype) {
    case NativeTreeArchetype::Palm:return 1.65f;
    case NativeTreeArchetype::BambooClump:return 1.45f;
    case NativeTreeArchetype::Cactus:return 2.25f;
    case NativeTreeArchetype::JungleBroadleaf:return 1.30f;
    default:return 1.18f;
    }
}

struct Candidate {
    int sourceX{};
    int sourceZ{};
    AoeDressingSample sample{};
    AoeSourceTreeSelection source{};
    float worldX{};
    float worldZ{};
    float priority{};
};

std::uint64_t coordinateKey(int x,int z) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(x))<<32|
           static_cast<std::uint32_t>(z);
}

bool validTreeCandidate(const AoeDressingConfig& config,
                        const AoeDressingSample& sample,
                        const AoeSourceTreeSelection& source,int x,int z) {
    if(!source.spawned||!sample.traversable||sample.normal.y<.66f)return false;
    if(!wooded(sample.region))return true;
    // A nonzero fringe remains at the boundary, then smoothly approaches the
    // exact source density in the forest interior. This is a 3D readability
    // extension; family selection and the pre-thinning candidate stay exact.
    const float keep=.12f+.88f*smoothstep(.06f,.78f,sample.forestInterior);
    return unitHash(config.seed,x,z,4001)<keep;
}

bool featureLand(const AoeDressingSample& sample) {
    return sample.traversable&&sample.normal.y>.78f&&
           sample.biome!=AoeBiome::DeepWater&&
           sample.biome!=AoeBiome::ShallowWater&&
           sample.biome!=AoeBiome::RiverWater&&
           sample.biome!=AoeBiome::MangroveShallows;
}

float distanceSquared(int ax,int az,int bx,int bz) {
    const float dx=static_cast<float>(ax-bx),dz=static_cast<float>(az-bz);
    return dx*dx+dz*dz;
}

float pointSegmentDistanceSquared(Vec3 point,Vec3 start,Vec3 end) {
    const float dx=end.x-start.x,dz=end.z-start.z;
    const float lengthSquared=dx*dx+dz*dz;
    if(lengthSquared<1.0e-6f) {
        const float px=point.x-start.x,pz=point.z-start.z;
        return px*px+pz*pz;
    }
    const float t=clamp(((point.x-start.x)*dx+(point.z-start.z)*dz)/
                        lengthSquared,0.0f,1.0f);
    const float px=point.x-(start.x+dx*t),pz=point.z-(start.z+dz*t);
    return px*px+pz*pz;
}

Vec3 localPosition(const AoeDressingConfig& config,int sourceX,int sourceZ,
                   const AoeDressingSample& sample,float lift=0.0f) {
    return {sourceX-config.localOriginX+.5f,sample.position.y+lift,
            sourceZ-config.localOriginZ+.5f};
}

bool inRange(const AoeDressingConfig& config,int x,int z) {
    return x>=config.minimumSourceX&&x<config.maximumSourceX&&
           z>=config.minimumSourceZ&&z<config.maximumSourceZ;
}

void addFeatureGeometry(GeometryWriter& writer,const AoeWorldFeature3D& feature) {
    const std::uint32_t warmStone=packColor(.37f,.30f,.22f);
    switch(feature.kind) {
    case AoeWorldFeatureKind::StarterCamp:
        for(int rock=0;rock<8;++rock) {
            const float angle=2*pi*static_cast<float>(rock)/8.0f;
            writer.rock(feature.position+Vec3{std::cos(angle)*.48f,0,
                        std::sin(angle)*.48f},.14f,warmStone);
        }
        writer.cylinder(feature.position+Vec3{-1.1f,0,.8f},1.65f,.065f,.05f,
                        5,packColor(.30f,.17f,.07f),5.0f);
        writer.cylinder(feature.position+Vec3{1.1f,0,.8f},1.65f,.065f,.05f,
                        5,packColor(.30f,.17f,.07f),5.0f);
        break;
    case AoeWorldFeatureKind::ForestGrove:
        writer.cylinder(feature.position,3.4f,.25f,.15f,7,
                        packColor(.23f,.135f,.06f),5.0f);
        writer.crown(feature.position+Vec3{0,3.25f,0},1.55f,1.32f,.70f,
                     packColor(.10f,.31f,.075f),4.0f,8,.18f);
        writer.crown(feature.position+Vec3{.72f,3.70f,.18f},1.02f,.86f,.48f,
                     packColor(.14f,.36f,.085f),4.0f,7,.55f);
        break;
    case AoeWorldFeatureKind::StandingStones:
        writer.rock(feature.position+Vec3{-.45f,0,0},1.05f,packColor(.37f,.38f,.36f));
        writer.rock(feature.position+Vec3{.48f,0,.22f},.82f,packColor(.33f,.34f,.32f));
        writer.rock(feature.position+Vec3{0,0,-.58f},.64f,packColor(.40f,.39f,.35f));
        break;
    case AoeWorldFeatureKind::CoastalBeacon:
        writer.cylinder(feature.position,2.4f,.18f,.11f,6,
                        packColor(.30f,.18f,.07f),5.0f);
        writer.crown(feature.position+Vec3{0,2.5f,0},.34f,.30f,.36f,
                     packColor(.92f,.43f,.08f),4.2f,6,.10f);
        break;
    case AoeWorldFeatureKind::WetlandTotem:
        writer.cylinder(feature.position,2.0f,.17f,.12f,5,
                        packColor(.20f,.12f,.055f),5.1f);
        writer.crown(feature.position+Vec3{0,1.75f,0},.38f,.32f,.22f,
                     packColor(.10f,.28f,.075f),4.1f,5,.32f);
        break;
    default:break;
    }
}

void addTrailGeometry(GeometryWriter& writer,const AoeTrail3D& trail) {
    if(trail.points.size()<2)return;
    const std::uint32_t first=static_cast<std::uint32_t>(writer.vertices.size());
    const std::uint32_t color=packColor(.34f,.25f,.13f);
    for(std::size_t index=0;index<trail.points.size();++index) {
        const Vec3 previous=trail.points[index==0?0:index-1];
        const Vec3 next=trail.points[std::min(index+1,trail.points.size()-1)];
        const Vec3 tangent=normalize(Vec3{next.x-previous.x,0,next.z-previous.z});
        const Vec3 side{-tangent.z,0,tangent.x};
        writer.vertices.push_back({trail.points[index]+side*trail.halfWidth,
            {0,1,0},color,7.15f,0,static_cast<float>(index)});
        writer.vertices.push_back({trail.points[index]-side*trail.halfWidth,
            {0,1,0},color,7.15f,1,static_cast<float>(index)});
    }
    for(std::size_t index=0;index+1<trail.points.size();++index) {
        const std::uint32_t a=first+static_cast<std::uint32_t>(index*2);
        writer.indices.insert(writer.indices.end(),{a,a+2,a+3,a,a+3,a+1});
    }
}

} // namespace

const AoeTreeFamilyMetadata& AoeWorldDressing::metadata(AoeTreeFamily family) {
    const std::size_t index=static_cast<std::size_t>(family);
    if(index>=familyMetadata.size())throw std::out_of_range("invalid tree family");
    return familyMetadata[index];
}

float AoeWorldDressing::sourceSpawnChance(AoeWorldBiome region,float elevation) {
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

AoeSourceTreeSelection AoeWorldDressing::sourceTreeAt(
    std::int64_t seed,int sourceX,int sourceZ,AoeWorldBiome region,
    AoeBiome biome,float elevation) {
    AoeSourceTreeSelection result{};
    result.spawnChance=sourceSpawnChance(region,elevation);
    result.spawnRoll=unitHash(seed,sourceX,sourceZ,91);
    result.spawned=result.spawnRoll<result.spawnChance;
    result.family=sourceFamily(seed,sourceX,sourceZ,region,biome);
    const auto& family=metadata(result.family);
    result.sourceFrame=family.sourceVariantCount<=1?0:
        static_cast<std::uint8_t>(static_cast<int>(
            unitHash(seed,sourceX,sourceZ,3137)*family.sourceVariantCount)%
            family.sourceVariantCount);
    return result;
}

AoeDressingResult AoeWorldDressing::generate(
    const AoeDressingConfig& config,const AoeDressingSampler& sampler) {
    if(!sampler)throw std::invalid_argument("world dressing requires a sampler");
    if(config.minimumSourceX>=config.maximumSourceX||
       config.minimumSourceZ>=config.maximumSourceZ)
        throw std::invalid_argument("world dressing bounds are empty");

    constexpr int halo=3;
    std::vector<Candidate> candidates;
    for(int z=config.minimumSourceZ-halo;z<config.maximumSourceZ+halo;++z) {
        for(int x=config.minimumSourceX-halo;x<config.maximumSourceX+halo;++x) {
            const AoeDressingSample sample=sampler(x,z);
            const AoeSourceTreeSelection source=sourceTreeAt(
                config.seed,x,z,sample.region,sample.biome,sample.elevation);
            if(!validTreeCandidate(config,sample,source,x,z))continue;
            candidates.push_back({x,z,sample,source,
                x+.5f+(unitHash(config.seed,x,z,307)-.5f)*.54f,
                z+.5f+(unitHash(config.seed,x,z,311)-.5f)*.54f,
                unitHash(config.seed,x,z,4051)});
        }
    }

    AoeDressingResult result{};
    std::unordered_map<std::uint64_t,std::size_t> candidateAt;
    candidateAt.reserve(candidates.size()*2);
    for(std::size_t index=0;index<candidates.size();++index)
        candidateAt.emplace(coordinateKey(candidates[index].sourceX,
                                          candidates[index].sourceZ),index);
    for(const Candidate& candidate:candidates) {
        if(!inRange(config,candidate.sourceX,candidate.sourceZ))continue;
        const NativeTreeArchetype archetype=metadata(candidate.source.family).nativeArchetype;
        const float spacing=minimumSpacing(archetype);
        bool suppressed=false;
        for(int offsetZ=-3;offsetZ<=3&&!suppressed;++offsetZ) {
            for(int offsetX=-3;offsetX<=3;++offsetX) {
                if(offsetX==0&&offsetZ==0)continue;
                const auto found=candidateAt.find(coordinateKey(
                    candidate.sourceX+offsetX,candidate.sourceZ+offsetZ));
                if(found==candidateAt.end())continue;
                const Candidate& other=candidates[found->second];
                const float otherSpacing=minimumSpacing(
                    metadata(other.source.family).nativeArchetype);
                const float required=std::max(spacing,otherSpacing);
                const float dx=candidate.worldX-other.worldX;
                const float dz=candidate.worldZ-other.worldZ;
                if(dx*dx+dz*dz>=required*required)continue;
                if(other.priority>candidate.priority||
                   (other.priority==candidate.priority&&
                    std::tie(other.sourceZ,other.sourceX)<
                    std::tie(candidate.sourceZ,candidate.sourceX))) {
                    suppressed=true;
                    break;
                }
            }
        }
        if(suppressed)continue;
        const float scale=.86f+.28f*unitHash(config.seed,candidate.sourceX,
                                             candidate.sourceZ,4099);
        float baseHeight=5.6f;
        switch(archetype) {
        case NativeTreeArchetype::Palm:baseHeight=6.8f;break;
        case NativeTreeArchetype::Conifer:baseHeight=7.2f;break;
        case NativeTreeArchetype::SnowConifer:baseHeight=6.2f;break;
        case NativeTreeArchetype::JungleBroadleaf:baseHeight=7.8f;break;
        case NativeTreeArchetype::BambooClump:baseHeight=5.1f;break;
        case NativeTreeArchetype::Cactus:baseHeight=3.4f;break;
        default:break;
        }
        const float height=baseHeight*scale*(.91f+.018f*candidate.source.sourceFrame);
        const float crownRadius=height*(archetype==NativeTreeArchetype::Conifer||
            archetype==NativeTreeArchetype::SnowConifer?.20f:
            (archetype==NativeTreeArchetype::Palm?.29f:
             archetype==NativeTreeArchetype::BambooClump?.18f:.25f));
        result.trees.push_back({candidate.sourceX,candidate.sourceZ,
            {candidate.worldX-config.localOriginX,candidate.sample.position.y,
             candidate.worldZ-config.localOriginZ},candidate.source.family,
            archetype,candidate.source.sourceFrame,height,crownRadius,
            clamp(candidate.sample.forestInterior,0.0f,1.0f)});
    }

    if(!config.includeWorldFeatures) {
        GeometryWriter writer{result.detailVertices,result.detailIndices};
        for(AoeTreeInstance3D& tree:result.trees) {
            tree.geometryFirstVertex=static_cast<std::uint32_t>(
                result.detailVertices.size());
            tree.geometryFirstIndex=static_cast<std::uint32_t>(
                result.detailIndices.size());
            appendTreeGeometry(writer,tree,config.seed);
            tree.geometryVertexCount=static_cast<std::uint32_t>(
                result.detailVertices.size())-tree.geometryFirstVertex;
            tree.geometryIndexCount=static_cast<std::uint32_t>(
                result.detailIndices.size())-tree.geometryFirstIndex;
        }
        return result;
    }

    int campX=config.spawnSourceX,campZ=config.spawnSourceZ;
    float campScore=std::numeric_limits<float>::lowest();
    for(int dz=-18;dz<=18;++dz)for(int dx=-18;dx<=18;++dx) {
        const int x=config.spawnSourceX+dx,z=config.spawnSourceZ+dz;
        const AoeDressingSample sample=sampler(x,z);
        if(!featureLand(sample))continue;
        const bool pleasant=sample.biome==AoeBiome::Grassland||
            sample.biome==AoeBiome::Forest||sample.biome==AoeBiome::DryGrass;
        const float score=(pleasant?14.0f:0.0f)+sample.normal.y*5.0f-
            std::sqrt(distanceSquared(x,z,config.spawnSourceX,config.spawnSourceZ))*.38f-
            sample.forestInterior*4.0f+unitHash(config.seed,x,z,6007)*.25f;
        if(score>campScore) {campScore=score;campX=x;campZ=z;}
    }
    const AoeDressingSample campSample=sampler(campX,campZ);
    const Vec3 campPosition=localPosition(config,campX,campZ,campSample,.025f);
    if(inRange(config,campX,campZ)) {
        result.features.push_back({AoeWorldFeatureKind::StarterCamp,campPosition,
            3.2f,mixedHash(config.seed,campX,campZ,6101)});
        result.features.push_back({AoeWorldFeatureKind::CampfireInteraction,
            campPosition,.8f,mixedHash(config.seed,campX,campZ,6103)});
        result.features.push_back({AoeWorldFeatureKind::QuestMarker,
            campPosition,1.2f,mixedHash(config.seed,campX,campZ,6107)});
        const AoeDressingSample spawnSample=sampler(
            config.spawnSourceX,config.spawnSourceZ);
        result.features.push_back({AoeWorldFeatureKind::SpawnMarker,
            localPosition(config,config.spawnSourceX,config.spawnSourceZ,
                          spawnSample,.03f),.65f,
            mixedHash(config.seed,config.spawnSourceX,config.spawnSourceZ,6113)});
    }

    struct LandmarkCandidate {
        AoeWorldFeatureKind kind{};
        int x{},z{};
        float score{std::numeric_limits<float>::lowest()};
    };
    std::array<LandmarkCandidate,4> landmarks{{
        {AoeWorldFeatureKind::ForestGrove},
        {AoeWorldFeatureKind::StandingStones},
        {AoeWorldFeatureKind::CoastalBeacon},
        {AoeWorldFeatureKind::WetlandTotem}}};
    for(int z=config.minimumSourceZ;z<config.maximumSourceZ;z+=3) {
        for(int x=config.minimumSourceX;x<config.maximumSourceX;x+=3) {
            const float d2=distanceSquared(x,z,campX,campZ);
            if(d2<14.0f*14.0f||d2>92.0f*92.0f)continue;
            const AoeDressingSample sample=sampler(x,z);
            if(!featureLand(sample))continue;
            int kind=-1;
            if(sample.forestInterior>.62f&&wooded(sample.region))kind=0;
            else if(sample.biome==AoeBiome::Rock||sample.biome==AoeBiome::Highland||
                    sample.biome==AoeBiome::Tundra)kind=1;
            else if(sample.biome==AoeBiome::Beach||sample.region==AoeWorldBiome::Coast)kind=2;
            else if(sample.biome==AoeBiome::Mud||sample.region==AoeWorldBiome::Wetland)kind=3;
            if(kind<0)continue;
            const float distance=std::sqrt(d2);
            const float score=sample.normal.y*3.0f-std::abs(distance-42.0f)*.025f+
                unitHash(config.seed,x,z,6203+kind)*.5f;
            if(score>landmarks[static_cast<std::size_t>(kind)].score)
                landmarks[static_cast<std::size_t>(kind)]={
                    landmarks[static_cast<std::size_t>(kind)].kind,x,z,score};
        }
    }

    int landmarkOrdinal=0;
    for(const LandmarkCandidate& landmark:landmarks) {
        if(!std::isfinite(landmark.score))continue;
        const AoeDressingSample sample=sampler(landmark.x,landmark.z);
        const Vec3 position=localPosition(config,landmark.x,landmark.z,sample,.025f);
        result.features.push_back({landmark.kind,position,2.2f,
            mixedHash(config.seed,landmark.x,landmark.z,6301+landmarkOrdinal)});
        result.features.push_back({AoeWorldFeatureKind::ResourceInteraction,
            position,1.4f,mixedHash(config.seed,landmark.x,landmark.z,
                                    6351+landmarkOrdinal)});
        if(landmarkOrdinal==1)result.features.push_back({
            AoeWorldFeatureKind::EncounterMarker,position,3.4f,
            mixedHash(config.seed,landmark.x,landmark.z,6389)});

        AoeTrail3D trail{};
        trail.stableId=mixedHash(config.seed,landmark.x,landmark.z,6401);
        const float routeDx=static_cast<float>(landmark.x-campX);
        const float routeDz=static_cast<float>(landmark.z-campZ);
        const float routeLength=std::sqrt(routeDx*routeDx+routeDz*routeDz);
        const int trailSegments=std::max(10,static_cast<int>(
            std::ceil(routeLength/.70f)));
        bool validRoute=true;
        for(int point=0;point<=trailSegments;++point) {
            const float t=static_cast<float>(point)/trailSegments;
            const float bend=std::sin(t*pi)*
                (unitHash(config.seed,landmark.x,landmark.z,6413)-.5f)*6.0f;
            const float length=std::max(routeLength,.001f);
            const float sourceX=campX+routeDx*t-routeDz/length*bend;
            const float sourceZ=campZ+routeDz*t+routeDx/length*bend;
            const int sampleX=static_cast<int>(std::floor(sourceX));
            const int sampleZ=static_cast<int>(std::floor(sourceZ));
            const AoeDressingSample pathSample=sampler(sampleX,sampleZ);
            if(!featureLand(pathSample)) {
                validRoute=false;
                break;
            }
            trail.points.push_back({sourceX-config.localOriginX+.5f,
                pathSample.position.y+.035f,sourceZ-config.localOriginZ+.5f});
        }
        if(validRoute) {
            const Vec3 midpoint=trail.points[trail.points.size()/2];
            result.features.push_back({AoeWorldFeatureKind::Trail,midpoint,
                trail.halfWidth,trail.stableId});
            result.trails.push_back(std::move(trail));
        }
        ++landmarkOrdinal;
    }

    const auto physicalFeature=[](AoeWorldFeatureKind kind) {
        return kind==AoeWorldFeatureKind::StarterCamp||
               kind==AoeWorldFeatureKind::ForestGrove||
               kind==AoeWorldFeatureKind::StandingStones||
               kind==AoeWorldFeatureKind::CoastalBeacon||
               kind==AoeWorldFeatureKind::WetlandTotem;
    };
    result.trees.erase(std::remove_if(result.trees.begin(),result.trees.end(),
        [&](const AoeTreeInstance3D& tree) {
            for(const AoeWorldFeature3D& feature:result.features) {
                if(!physicalFeature(feature.kind))continue;
                const float dx=tree.position.x-feature.position.x;
                const float dz=tree.position.z-feature.position.z;
                const float clearance=feature.radius+.55f;
                if(dx*dx+dz*dz<clearance*clearance)return true;
            }
            for(const AoeTrail3D& trail:result.trails) {
                const float clearance=trail.halfWidth+.45f;
                for(std::size_t point=0;point+1<trail.points.size();++point)
                    if(pointSegmentDistanceSquared(tree.position,
                        trail.points[point],trail.points[point+1])<
                        clearance*clearance)return true;
            }
            return false;
        }),result.trees.end());

    GeometryWriter writer{result.detailVertices,result.detailIndices};
    for(AoeTreeInstance3D& tree:result.trees) {
        tree.geometryFirstVertex=static_cast<std::uint32_t>(
            result.detailVertices.size());
        tree.geometryFirstIndex=static_cast<std::uint32_t>(
            result.detailIndices.size());
        appendTreeGeometry(writer,tree,config.seed);
        tree.geometryVertexCount=static_cast<std::uint32_t>(
            result.detailVertices.size())-tree.geometryFirstVertex;
        tree.geometryIndexCount=static_cast<std::uint32_t>(
            result.detailIndices.size())-tree.geometryFirstIndex;
    }
    for(const AoeTrail3D& trail:result.trails)addTrailGeometry(writer,trail);
    for(const AoeWorldFeature3D& feature:result.features)
        addFeatureGeometry(writer,feature);
    return result;
}

void AoeDressingResult::appendGeometryTo(EnvironmentMesh& mesh) const {
    const std::uint32_t offset=static_cast<std::uint32_t>(mesh.detailVertices.size());
    mesh.detailVertices.insert(mesh.detailVertices.end(),detailVertices.begin(),
                               detailVertices.end());
    mesh.detailIndices.reserve(mesh.detailIndices.size()+detailIndices.size());
    for(const std::uint32_t index:detailIndices)mesh.detailIndices.push_back(offset+index);
    mesh.backgroundTreeCount+=static_cast<std::uint32_t>(trees.size());
}

} // namespace dense
