#include "environment.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dense {
namespace {

float smoothStep(float low,float high,float value) {
    const float t=clamp((value-low)/(high-low),0.0f,1.0f);
    return t*t*(3.0f-2.0f*t);
}

float meadowHash(float x,float z) {
    const float value=std::sin(x*127.1f+z*311.7f)*43758.5453123f;
    return value-std::floor(value);
}

float meadowValueNoise(float x,float z) {
    const float cellX=std::floor(x),cellZ=std::floor(z);
    const float localX=x-cellX,localZ=z-cellZ;
    const float blendX=localX*localX*(3.0f-2.0f*localX);
    const float blendZ=localZ*localZ*(3.0f-2.0f*localZ);
    const float a=meadowHash(cellX,cellZ),b=meadowHash(cellX+1,cellZ);
    const float c=meadowHash(cellX,cellZ+1),d=meadowHash(cellX+1,cellZ+1);
    return (a+(b-a)*blendX)+((c+(d-c)*blendX)-(a+(b-a)*blendX))*blendZ;
}

struct MeadowColourFields {
    float fertility;
    float dryColony;
    float lushColony;
    float warmCool;
};

MeadowColourFields meadowColourFields(float x,float z) {
    const float rotatedX=.819f*x+.574f*z,rotatedZ=-.574f*x+.819f*z;
    const float broad=meadowValueNoise(x*.019f+17.3f,z*.019f-9.1f);
    const float colony=meadowValueNoise(rotatedX*.064f-31.7f,rotatedZ*.064f+22.4f);
    const float subclump=meadowValueNoise((x+rotatedX*.37f)*.17f+8.6f,
                                          (z+rotatedZ*.37f)*.17f+41.2f);
    const float fertility=clamp(.52f*broad+.33f*colony+.15f*subclump,0.0f,1.0f);
    const float dry=smoothStep(.68f,.88f,.62f*colony+.38f*(1-broad));
    const float lush=smoothStep(.60f,.84f,.58f*broad+.42f*subclump);
    const float warmCool=clamp((colony-.5f)*1.4f+(broad-.5f)*.6f,-1.0f,1.0f);
    return {fertility,dry,lush,warmCool};
}

float gaussian(float x,float z,float cx,float cz,float rx,float rz) {
    const float dx=(x-cx)/rx,dz=(z-cz)/rz;
    return std::exp(-(dx*dx+dz*dz));
}

uint32_t packColor(float r,float g,float b) {
    const auto channel=[](float value) {
        return static_cast<uint32_t>(clamp(value,0.0f,1.0f)*255.0f+0.5f);
    };
    return channel(r)|(channel(g)<<8)|(channel(b)<<16)|0xff000000u;
}

Vec3 rotateY(Vec3 value,float angle) {
    const float c=std::cos(angle),s=std::sin(angle);
    return {value.x*c-value.z*s,value.y,value.x*s+value.z*c};
}

void appendTube(EnvironmentMesh& mesh,Vec3 start,Vec3 end,float startRadius,float endRadius,
                int sides,uint32_t color,float material) {
    const Vec3 axis=normalize(end-start);
    const Vec3 helper=std::abs(axis.y)<.92f?Vec3{0,1,0}:Vec3{1,0,0};
    const Vec3 side=normalize(cross(helper,axis)),up=normalize(cross(axis,side));
    const uint32_t base=static_cast<uint32_t>(mesh.detailVertices.size());
    for(int ring=0;ring<2;++ring) {
        const Vec3 center=ring?end:start;
        const float radius=ring?endRadius:startRadius;
        for(int k=0;k<=sides;++k) {
            const float angle=2*pi*k/sides;
            const Vec3 radial=side*std::cos(angle)+up*std::sin(angle);
            mesh.detailVertices.push_back({center+radial*radius,radial,color,material,
                                           static_cast<float>(k)/sides,
                                           static_cast<float>(ring)});
        }
    }
    for(int k=0;k<sides;++k) {
        const uint32_t a=base+k,b=a+1,c=base+(sides+1)+k,d=c+1;
        mesh.detailIndices.insert(mesh.detailIndices.end(),{a,c,d,a,d,b});
    }
}

void appendRock(EnvironmentMesh& mesh,Vec3 grade,Vec3 radii,float yaw,uint32_t seed,int type) {
    const int sides=type==2?18:16;
    const int rings=type==2?8:7;
    const uint32_t base=static_cast<uint32_t>(mesh.detailVertices.size());
    for(int ring=0;ring<rings;++ring)for(int k=0;k<=sides;++k) {
        const float t=static_cast<float>(ring)/(rings-1);
        const float level=-.36f+.96f*t;
        const float profile=clamp(1.03f-std::pow(std::abs((level-.04f)/.82f),1.65f),.24f,1.0f);
        const float ringPhase=.035f*std::sin(seed*.00011f+ring*1.73f)+(ring&1? .018f:-.018f);
        const float angle=2*pi*k/sides+ringPhase;
        const float angularNoise=1.0f+(type==2?.17f:.09f)*std::sin(angle*3.0f+seed*.000071f)
                                     +.055f*std::sin(angle*5.0f-seed*.000037f)
                                     +.035f*std::sin(angle*9.0f+ring*.83f+seed*.000019f);
        const float layerOffset=(type==1?.035f:0.0f)*std::sin(angle*4+ring*.9f);
        Vec3 local{std::cos(angle)*radii.x*profile*angularNoise,
                   (level+layerOffset)*radii.y,
                   std::sin(angle)*radii.z*profile*angularNoise};
        const Vec3 world=grade+rotateY(local,yaw);
        Vec3 localNormal{local.x/std::max(radii.x*radii.x,.001f),
                         local.y/std::max(radii.y*radii.y,.003f),
                         local.z/std::max(radii.z*radii.z,.001f)};
        const Vec3 normal=normalize(rotateY(localNormal,yaw));
        const float tone=.82f+.16f*std::sin(angle*2.0f+ring+seed*.000013f);
        const uint32_t color=type==1?packColor(.48f*tone,.46f*tone,.40f*tone)
                            :packColor(.43f*tone,.42f*tone,.37f*tone);
        mesh.detailVertices.push_back({world,normal,color,3.0f+type*.1f,
                                       static_cast<float>(k)/sides,t});
    }
    for(int ring=0;ring<rings-1;++ring)for(int k=0;k<sides;++k) {
        const uint32_t a=base+ring*(sides+1)+k,b=a+1,c=a+sides+1,d=c+1;
        mesh.detailIndices.insert(mesh.detailIndices.end(),{a,c,d,a,d,b});
    }
    const uint32_t top=static_cast<uint32_t>(mesh.detailVertices.size());
    const Vec3 topLocal{.06f*radii.x*std::sin(seed*.001f),.68f*radii.y,
                        .05f*radii.z*std::cos(seed*.0013f)};
    mesh.detailVertices.push_back({grade+rotateY(topLocal,yaw),{0,1,0},
                                   packColor(.39f,.385f,.34f),3.0f+type*.1f,.5f,1.0f});
    const uint32_t last=base+(rings-1)*(sides+1);
    for(int k=0;k<sides;++k)mesh.detailIndices.insert(mesh.detailIndices.end(),
                                                       {last+static_cast<uint32_t>(k),top,
                                                        last+static_cast<uint32_t>(k+1)});
}

void appendFoliageClump(EnvironmentMesh& mesh,Vec3 center,Vec3 radii,uint32_t baseColor,
                        float material,uint32_t seed,int sides=8) {
    const uint32_t bottom=static_cast<uint32_t>(mesh.detailVertices.size());
    mesh.detailVertices.push_back({center+Vec3{0,-radii.y,0},{0,-1,0},baseColor,material,.5f,0});
    const std::array<float,3> latitude{-0.46f,0.02f,.48f};
    const uint32_t rings=static_cast<uint32_t>(latitude.size());
    const uint32_t firstRing=static_cast<uint32_t>(mesh.detailVertices.size());
    for(uint32_t ring=0;ring<rings;++ring)for(int k=0;k<sides;++k) {
        const float angle=2*pi*k/sides+.13f*std::sin(seed*.0001f+ring);
        const float y=latitude[ring],latitudeRadius=std::sqrt(std::max(0.0f,1-y*y));
        const float irregular=.84f+.13f*std::sin(angle*3+seed*.00017f)
                                    +.08f*std::sin(angle*5-ring*.7f);
        Vec3 local{std::cos(angle)*radii.x*latitudeRadius*irregular,
                   y*radii.y*(.94f+.08f*std::sin(angle*2+seed*.00003f)),
                   std::sin(angle)*radii.z*latitudeRadius*irregular};
        Vec3 normal=normalize({local.x/std::max(radii.x*radii.x,.001f),
                               local.y/std::max(radii.y*radii.y,.001f),
                               local.z/std::max(radii.z*radii.z,.001f)});
        const float tint=.88f+.15f*std::sin(angle*4+ring+seed*.00007f);
        const float r=(baseColor&255)/255.0f,g=((baseColor>>8)&255)/255.0f,
                    b=((baseColor>>16)&255)/255.0f;
        mesh.detailVertices.push_back({center+local,normal,packColor(r*tint,g*tint,b*tint),
                                       material,static_cast<float>(k)/sides,(y+1)*.5f});
    }
    const uint32_t top=static_cast<uint32_t>(mesh.detailVertices.size());
    mesh.detailVertices.push_back({center+Vec3{0,radii.y,0},{0,1,0},baseColor,material,.5f,1});
    for(int k=0;k<sides;++k) {
        const uint32_t next=static_cast<uint32_t>((k+1)%sides);
        mesh.detailIndices.insert(mesh.detailIndices.end(),{bottom,firstRing+next,firstRing+static_cast<uint32_t>(k)});
    }
    for(uint32_t ring=0;ring+1<rings;++ring)for(int k=0;k<sides;++k) {
        const uint32_t next=static_cast<uint32_t>((k+1)%sides);
        const uint32_t a=firstRing+ring*sides+k,b=firstRing+ring*sides+next;
        const uint32_t c=a+sides,d=b+sides;
        mesh.detailIndices.insert(mesh.detailIndices.end(),{a,c,d,a,d,b});
    }
    const uint32_t last=firstRing+(rings-1)*sides;
    for(int k=0;k<sides;++k) {
        const uint32_t next=static_cast<uint32_t>((k+1)%sides);
        mesh.detailIndices.insert(mesh.detailIndices.end(),{last+static_cast<uint32_t>(k),
                                                            last+next,top});
    }
}

void appendProxyAxis(EnvironmentMesh& mesh,Vec3 start,Vec3 direction,float axisLength,
                     float radius,int depth,int type,float crownRadius,float treeHeight,
                     uint32_t foliage,float foliageMaterial,int terminalPadCount,Rng& rng) {
    Vec3 position=start,tangent=normalize(direction);
    constexpr int segments=3;
    for(int segment=0;segment<segments;++segment) {
        const float angle=rng.range(0,2*pi);
        const Vec3 wander{std::cos(angle),type==1?rng.range(-.06f,.025f):rng.range(-.015f,.08f),
                          std::sin(angle)};
        tangent=normalize(tangent*.93f+wander*.07f);
        const Vec3 next=position+tangent*(axisLength/segments*rng.range(.92f,1.08f));
        const float t0=static_cast<float>(segment)/segments,t1=static_cast<float>(segment+1)/segments;
        const float r0=radius*std::pow(1-.72f*t0,1.18f),r1=radius*std::pow(1-.72f*t1,1.18f);
        appendTube(mesh,position,next,r0,r1,5,packColor(.105f,.069f,.038f),5.0f);
        position=next;
    }
    if(depth==0) {
        const float horizontal=type==1?crownRadius*rng.range(.10f,.17f):
                               (type==2?crownRadius*rng.range(.14f,.21f):
                                        crownRadius*rng.range(.18f,.25f));
        const float vertical=type==1?treeHeight*rng.range(.035f,.060f):
                             (type==2?treeHeight*rng.range(.055f,.085f):
                                      treeHeight*rng.range(.070f,.105f));
        const Vec3 helper=std::abs(tangent.y)<.9f?Vec3{0,1,0}:Vec3{1,0,0};
        const Vec3 side=normalize(cross(helper,tangent)),around=normalize(cross(tangent,side));
        const float phase=rng.range(0,2*pi);
        for(int pad=0;pad<terminalPadCount;++pad) {
            const float padAngle=phase+2*pi*pad/std::max(1,terminalPadCount)+rng.range(-.32f,.32f);
            const float offsetRadius=pad==0?0.0f:horizontal*rng.range(.38f,.68f);
            const Vec3 offset=side*(std::cos(padAngle)*offsetRadius)
                             +around*(std::sin(padAngle)*offsetRadius)
                             +tangent*horizontal*rng.range(.04f,.24f);
            appendFoliageClump(mesh,position+offset,
                               {horizontal*rng.range(.84f,1.12f),
                                vertical*rng.range(.82f,1.10f),
                                horizontal*rng.range(.72f,1.10f)},
                               foliage,foliageMaterial,rng.next(),7);
        }
        return;
    }

    const Vec3 helper=std::abs(tangent.y)<.9f?Vec3{0,1,0}:Vec3{1,0,0};
    const Vec3 side=normalize(cross(helper,tangent)),around=normalize(cross(tangent,side));
    const float phase=rng.range(0,2*pi);
    const Vec3 radial=side*std::cos(phase)+around*std::sin(phase);
    Vec3 continuation=normalize(tangent*.88f+radial*.28f+Vec3{0,type==1?.01f:.08f,0});
    Vec3 lateral=normalize(tangent*.58f+radial*(-.78f)+Vec3{0,type==1?-.035f:.12f,0});
    appendProxyAxis(mesh,position,continuation,axisLength*.58f,radius*.60f,depth-1,type,
                    crownRadius,treeHeight,foliage,foliageMaterial,terminalPadCount,rng);
    appendProxyAxis(mesh,position,lateral,axisLength*.50f,radius*.52f,depth-1,type,
                    crownRadius,treeHeight,foliage,foliageMaterial,terminalPadCount,rng);
}

void appendProxyTree(EnvironmentMesh& mesh,Vec3 base,float height,float crownRadius,
                     int type,int detailLevel,Rng& rng) {
    const uint32_t foliage=type==1?packColor(.13f,.31f,.095f):
                             (type==2?packColor(.16f,.34f,.095f):packColor(.145f,.325f,.085f));
    const float foliageMaterial=4.0f+type*.1f;
    const int terminalPadCount=detailLevel==0?(type==0?3:2):1;
    const float trunkFraction=type==1?.94f:(type==2?.76f:.58f);
    const int trunkSegments=type==1?7:5;
    std::vector<Vec3> trunkNodes;trunkNodes.reserve(static_cast<size_t>(trunkSegments)+1);
    trunkNodes.push_back(base);
    Vec3 position=base,tangent{0,1,0};
    for(int segment=0;segment<trunkSegments;++segment) {
        const float angle=rng.range(0,2*pi);
        tangent=normalize(tangent*.97f+Vec3{std::cos(angle)*.025f,0,std::sin(angle)*.025f});
        const Vec3 next=position+tangent*(height*trunkFraction/trunkSegments);
        const float t0=static_cast<float>(segment)/trunkSegments,
                    t1=static_cast<float>(segment+1)/trunkSegments;
        appendTube(mesh,position,next,height*.034f*std::pow(1-.74f*t0,1.1f),
                   height*.034f*std::pow(1-.74f*t1,1.1f),7,
                   packColor(.105f,.068f,.038f),5.0f);
        position=next;trunkNodes.push_back(position);
    }

    if(type==1) {
        const int tiers=5;
        for(int tier=0;tier<tiers;++tier) {
            const int node=1+tier*(trunkSegments-2)/(tiers-1);
            const float taper=1.0f-.12f*tier;
            const float phase=tier*2.39996f+rng.range(-.24f,.24f);
            for(int branch=0;branch<3;++branch) {
                const float angle=phase+2*pi*branch/3+rng.range(-.15f,.15f);
                const float tierProgress=static_cast<float>(tier)/(tiers-1);
                const float elevation=-.10f+.32f*tierProgress;
                const Vec3 direction=normalize({std::cos(angle),elevation,std::sin(angle)});
                appendProxyAxis(mesh,trunkNodes[node],direction,crownRadius*.72f*taper,
                                height*.011f*taper,detailLevel>0?1:0,type,crownRadius,height,foliage,
                                foliageMaterial,terminalPadCount,rng);
            }
        }
        appendFoliageClump(mesh,trunkNodes.back()+Vec3{0,height*.025f,0},
                           {crownRadius*.10f,height*.045f,crownRadius*.10f},
                           foliage,foliageMaterial,rng.next(),6);
    } else {
        const int limbs=type==2?7:6;
        const int depth=std::min(type==2?1:2,detailLevel);
        const float dominant=rng.range(0,2*pi);
        for(int limb=0;limb<limbs;++limb) {
            const int node=1+(limb*(trunkSegments-2)+limb/2)%std::max(2,trunkSegments-1);
            const float angle=limb*2.39996f+rng.range(-.28f,.28f);
            const float elevation=type==2?rng.range(.40f,.72f):rng.range(.18f,.48f);
            const float dominance=1.0f+.18f*std::max(0.0f,std::cos(angle-dominant));
            const Vec3 direction=normalize({std::cos(angle)*std::cos(elevation),std::sin(elevation),
                                            std::sin(angle)*std::cos(elevation)});
            appendProxyAxis(mesh,trunkNodes[node],direction,
                            crownRadius*rng.range(.55f,.76f)*dominance,
                            height*rng.range(.011f,.017f),depth,type,crownRadius,height,
                            foliage,foliageMaterial,terminalPadCount,rng);
        }
    }
}

void appendProxyBush(EnvironmentMesh& mesh,Vec3 base,float radius,float height,Rng& rng) {
    const uint32_t wood=packColor(.105f,.068f,.038f),foliage=packColor(.14f,.315f,.08f);
    const int stems=5+static_cast<int>(rng.next()%3u);
    for(int stem=0;stem<stems;++stem) {
        const float angle=2*pi*stem/stems+rng.range(-.22f,.22f);
        const Vec3 middle=base+Vec3{std::cos(angle)*radius*.28f,height*.43f,
                                    std::sin(angle)*radius*.28f};
        const Vec3 tip=base+Vec3{std::cos(angle)*radius*rng.range(.68f,1.0f),
                                 height*rng.range(.68f,1.0f),
                                 std::sin(angle)*radius*rng.range(.68f,1.0f)};
        appendTube(mesh,base,middle,height*.018f,height*.010f,4,wood,5.0f);
        appendTube(mesh,middle,tip,height*.010f,.003f,4,wood,5.0f);
        appendFoliageClump(mesh,tip,{radius*rng.range(.18f,.28f),height*rng.range(.14f,.22f),
                                    radius*rng.range(.17f,.27f)},
                           foliage,4.2f,rng.next(),6);
    }
}

struct GrassIsland { float x,z,rx,rz,phase; };

} // namespace

float EnvironmentGenerator::terrainHeight(float x,float z) {
    const float radius=std::sqrt(x*x+z*z);
    const float rootMask=smoothStep(1.65f,4.6f,radius);
    const float ellipticalRadiusSq=x*x*.82f+z*z*1.16f;
    const float hill=-3.25f*(1.0f-std::exp(-ellipticalRadiusSq/235.0f));

    const float broad=.24f*std::sin(x*.105f+.45f)*std::cos(z*.083f-.80f)
                     +.13f*std::sin((x+z)*.047f+1.70f)
                     +.08f*std::cos((x-z)*.071f-.20f);
    const float shoulder=.38f*gaussian(x,z,-8,5,std::sqrt(120.0f),std::sqrt(210.0f));
    const float shoulderAtOrigin=.38f*gaussian(0,0,-8,5,std::sqrt(120.0f),std::sqrt(210.0f));

    // Unequal overlapping landforms form a readable middle distance instead
    // of a single mathematical dome.
    const float middleMask=smoothStep(10,18,radius)*(1-smoothStep(63,74,radius));
    const float middle=2.15f*gaussian(x,z,-23,27,19,13)
                      +1.55f*gaussian(x,z,25,32,23,15)
                      +2.55f*gaussian(x,z,7,48,25,16)
                      +1.75f*gaussian(x,z,-39,-18,17,24)
                      +1.35f*gaussian(x,z,37,-25,21,18)
                      +.95f*gaussian(x,z,3,-40,28,15);

    // A warped annular range remains visible while orbiting.  The non-uniform
    // grid spends its resolution near the oak while retaining a genuinely
    // distant, atmospherically softened mountain silhouette.
    const float angle=std::atan2(z,x);
    const float ridgeRadius=1220.0f+95.0f*std::sin(angle*3+.55f)
                                    +55.0f*std::sin(angle*5-1.15f)
                                    +28.0f*std::sin(angle*9+.30f);
    const float ridgeWidth=145.0f+25.0f*std::sin(angle*4-.40f);
    const float ridgeProfile=std::exp(-((radius-ridgeRadius)*(radius-ridgeRadius))/
                                      (ridgeWidth*ridgeWidth));
    const float peakVariation=clamp(.50f+.24f*std::sin(angle*2-.25f)
                                         +.15f*std::sin(angle*5+1.35f)
                                         +.08f*std::sin(angle*11-.70f),0.0f,1.0f);
    const float ridgePeaks=62.0f+68.0f*peakVariation;
    const float foothillDistance=(radius-(ridgeRadius-245.0f))/205.0f;
    const float foothills=13.0f*std::exp(-foothillDistance*foothillDistance);
    const float ridgeMask=smoothStep(760,900,radius)*(1-smoothStep(1510,1590,radius));
    const float mountains=ridgeMask*(ridgeProfile*ridgePeaks+foothills);
    return rootMask*(hill+broad+shoulder-shoulderAtOrigin+middleMask*middle+mountains);
}

Vec3 EnvironmentGenerator::terrainNormal(float x,float z) {
    constexpr float epsilon=.12f;
    const float dx=(terrainHeight(x+epsilon,z)-terrainHeight(x-epsilon,z))/(2*epsilon);
    const float dz=(terrainHeight(x,z+epsilon)-terrainHeight(x,z-epsilon))/(2*epsilon);
    return normalize({-dx,1.0f,-dz});
}

EnvironmentMesh EnvironmentGenerator::build(uint32_t seed) const {
    EnvironmentMesh mesh;
    constexpr int resolution=terrainResolution;
    constexpr float extent=terrainHalfExtent;
    mesh.terrainVertices.reserve(static_cast<size_t>(resolution)*resolution);
    mesh.terrainIndices.reserve(static_cast<size_t>(resolution-1)*(resolution-1)*6);
    mesh.minimumHeight=std::numeric_limits<float>::max();
    mesh.maximumHeight=std::numeric_limits<float>::lowest();

    const auto gridCoordinate=[&](int coordinate) {
        const float centered=static_cast<float>(coordinate-(resolution-1)/2)/
                             static_cast<float>((resolution-1)/2);
        return std::copysign(extent*std::pow(std::abs(centered),1.78f),centered);
    };
    for(int z=0;z<resolution;++z) {
        const float worldZ=gridCoordinate(z);
        for(int x=0;x<resolution;++x) {
            const float worldX=gridCoordinate(x);
            const float y=terrainHeight(worldX,worldZ);
            const Vec3 normal=terrainNormal(worldX,worldZ);
            const float variation=.5f+.5f*std::sin(worldX*.071f+worldZ*.053f);
            const uint32_t color=packColor(.20f+.035f*variation,.315f+.045f*variation,
                                           .135f+.022f*variation);
            mesh.terrainVertices.push_back({{worldX,y,worldZ},normal,color,2.0f,
                                            worldX*.08f,worldZ*.08f});
            mesh.minimumHeight=std::min(mesh.minimumHeight,y);
            mesh.maximumHeight=std::max(mesh.maximumHeight,y);
        }
    }
    for(int z=0;z<resolution-1;++z)for(int x=0;x<resolution-1;++x) {
        const uint32_t a=static_cast<uint32_t>(z*resolution+x),b=a+1;
        const uint32_t c=a+static_cast<uint32_t>(resolution),d=c+1;
        if(((x+z)&1)==0)mesh.terrainIndices.insert(mesh.terrainIndices.end(),{a,c,d,a,d,b});
        else mesh.terrainIndices.insert(mesh.terrainIndices.end(),{a,c,b,b,c,d});
    }

    std::array<GrassIsland,16> islands{};
    islands[0]={7.8f,-7.0f,3.5f,2.1f,.4f};
    islands[1]={-3.5f,-7.0f,3.0f,1.9f,1.7f};
    islands[2]={12.5f,-1.0f,2.2f,3.4f,2.6f};
    islands[3]={-13.0f,6.5f,3.7f,2.1f,.9f};
    islands[4]={6.0f,13.0f,4.0f,2.4f,2.2f};
    Rng islandRng(seed^0xa341316cu);
    for(size_t i=5;i<islands.size();++i) {
        const float angle=islandRng.range(0,2*pi),distance=islandRng.range(5.0f,21.5f);
        islands[i]={std::cos(angle)*distance,std::sin(angle)*distance,
                    islandRng.range(1.4f,3.7f),islandRng.range(1.1f,2.8f),
                    islandRng.range(0,2*pi)};
    }

    constexpr float cell=.55f;
    const int cells=static_cast<int>(std::floor(2*grassHalfExtent/cell));
    Rng grassRng(seed);
    mesh.grassPatches.reserve(static_cast<size_t>(cells)*cells);
    for(int iz=0;iz<cells;++iz)for(int ix=0;ix<cells;++ix) {
        const float x=-grassHalfExtent+(ix+grassRng.range(.13f,.87f))*cell;
        const float z=-grassHalfExtent+(iz+grassRng.range(.13f,.87f))*cell;
        const float radius=std::sqrt(x*x+z*z);
        if(radius>grassHalfExtent-.25f||radius<1.05f)continue;
        const MeadowColourFields colour=meadowColourFields(x,z);

        float islandStrength=0;
        for(const auto& island:islands) {
            const float dx=(x-island.x)/island.rx,dz=(z-island.z)/island.rz;
            const float q=std::sqrt(dx*dx+dz*dz);
            const float ragged=1.0f+.13f*std::sin(dx*4.1f+dz*2.7f+island.phase)
                                    +.07f*std::sin(dx*8.3f-dz*5.2f-island.phase);
            islandStrength=std::max(islandStrength,clamp((ragged-q)*1.7f,0.0f,1.0f));
        }
        // Long grass is a continuous meadow with denser island-like colonies,
        // rather than a handful of isolated vertical tufts.
        const bool tall=grassRng.unit()<(.50f+.24f*islandStrength);
        const float canopyShade=1.0f-.26f*(1.0f-smoothStep(5.0f,11.0f,radius));
        const float meadowVariation=.78f+.25f*colour.fertility;
        const float density=std::min(1.0f,canopyShade*meadowVariation*(tall?1.18f:1.0f));
        if(grassRng.unit()>density)continue;

        const float baseY=terrainHeight(x,z)+.006f;
        const Vec3 normal=terrainNormal(x,z);
        const float moisture=clamp(.58f+(colour.fertility-.5f)*.34f+
                                   grassRng.range(-.018f,.018f),0,1);
        const float shortHeight=grassRng.range(.045f,.120f)*(.88f+.17f*moisture);
        const float tallHeight=tall?grassRng.range(.42f,.84f)*(.88f+.17f*moisture):0.0f;
        const uint32_t shortCode=static_cast<uint32_t>(clamp(shortHeight/.004f,1,255));
        const uint32_t tallCode=tall?
            static_cast<uint32_t>(clamp(tallHeight/.004f,1,255)):0u;
        const uint32_t bladeCount=28u+static_cast<uint32_t>(grassRng.unit()*7.0f);
        const uint32_t tallCount=tall?18u+static_cast<uint32_t>(grassRng.unit()*7.0f):0u;
        const uint32_t packed=(bladeCount&255u)|(shortCode<<8)|(tallCount<<16)|(tallCode<<24);
        const float maximumHeight=(tall?tallHeight:shortHeight)*2.5f;
        const float slope=std::sqrt(normal.x*normal.x+normal.z*normal.z)/
                          std::max(normal.y,.25f);
        const float lateralRatio=tall?.66f:.52f;
        const float maximumWidth=tall?.043f:.018f;
        constexpr float grassBoundsSafety=.012f;
        const float horizontalReach=.245f+maximumHeight*(slope+lateralRatio)+
                                    maximumWidth+grassBoundsSafety;
        const float surfaceRise=.245f*slope;
        const float lowerReach=surfaceRise+maximumWidth*slope+grassBoundsSafety;
        const float upperReach=surfaceRise+maximumHeight*(normal.y+lateralRatio*slope)+
                               maximumWidth*slope+grassBoundsSafety;
        mesh.grassPatches.push_back({x-horizontalReach,baseY-lowerReach,
                                     z-horizontalReach,x+horizontalReach,
                                     baseY+upperReach,z+horizontalReach,
                                     grassRng.next(),packed,baseY,normal.x,normal.z,moisture,
                                     colour.fertility,colour.dryColony,colour.lushColony,
                                     colour.warmCool});
        if(tall)++mesh.tallGrassPatchCount;
    }

    Rng detailRng(seed^0xc8013ea4u);
    // Stones occur in loose families rather than a uniform scatter.
    for(int group=0;group<9;++group) {
        const float groupAngle=detailRng.range(0,2*pi),groupDistance=detailRng.range(4.0f,29.0f);
        const Vec3 groupCenter{std::cos(groupAngle)*groupDistance,0,
                               std::sin(groupAngle)*groupDistance};
        const int members=4+static_cast<int>(detailRng.next()%6u);
        for(int member=0;member<members;++member) {
            const float angle=detailRng.range(0,2*pi),spread=detailRng.range(.2f,3.6f);
            const float x=groupCenter.x+std::cos(angle)*spread,z=groupCenter.z+std::sin(angle)*spread;
            const float r=std::sqrt(x*x+z*z);const Vec3 normal=terrainNormal(x,z);
            if(r<2.1f||r>32||normal.y<.80f)continue;
            const uint32_t rockRoll=detailRng.next()%5u;
            const int type=rockRoll==4u?2:static_cast<int>(rockRoll&1u);
            const float scale=type==2?detailRng.range(.48f,.98f):detailRng.range(.14f,.55f);
            Vec3 radii{scale*detailRng.range(.78f,1.35f),
                       scale*(type==1?detailRng.range(.22f,.42f):detailRng.range(.52f,.92f)),
                       scale*detailRng.range(.68f,1.24f)};
            appendRock(mesh,{x,terrainHeight(x,z),z},radii,detailRng.range(0,2*pi),
                       detailRng.next(),type);
            ++mesh.rockCount;
        }
    }

    // Grazing keeps the hero-oak field open.  Shrubs are reserved for the
    // distant broken hedgerow below, where their scale and silhouette belong.

    // Sparse pasture trees establish scale without turning the clearing into
    // a ring of lollipops.  Each distance band uses a cheaper biological LOD;
    // physical size is never reduced merely because an object is farther away.
    struct TreeBand { int target;float inner,outer,spacing;int detail; };
    constexpr std::array<TreeBand,3> treeBands{{
        {4,245.0f,335.0f,34.0f,2},
        {6,430.0f,580.0f,46.0f,1},
        {9,690.0f,880.0f,58.0f,0}
    }};
    std::vector<Vec3> acceptedTrees;
    acceptedTrees.reserve(19);
    for(const auto& band:treeBands) {
        int placed=0,attempts=0;
        while(placed<band.target&&attempts++<900) {
            const float angle=detailRng.range(0,2*pi),radius=detailRng.range(band.inner,band.outer);
            const float grove=.43f+.31f*std::sin(angle*3.0f+radius*.034f)
                                   +.23f*std::sin(angle*7.0f-radius*.021f);
            if(detailRng.unit()>clamp(grove,.10f,.88f))continue;
            const float x=std::cos(angle)*radius,z=std::sin(angle)*radius;
            bool crowded=false;
            for(const Vec3& existing:acceptedTrees) {
                const float dx=x-existing.x,dz=z-existing.z;
                if(dx*dx+dz*dz<band.spacing*band.spacing){crowded=true;break;}
            }
            if(crowded)continue;
            const Vec3 normal=terrainNormal(x,z);const float heightAtBase=terrainHeight(x,z);
            if(normal.y<(band.detail==2?.82f:(band.detail==1?.70f:.58f))||heightAtBase>14.0f)
                continue;

            const uint32_t objectSeed=detailRng.next();Rng objectRng(objectSeed^0x9e3779b9u);
            const uint32_t typeRoll=detailRng.next()%100u;
            const int type=typeRoll<58u?0:(typeRoll<90u?2:1);
            const float treeHeight=band.detail==2?objectRng.range(7.0f,10.5f):
                                   (band.detail==1?objectRng.range(6.2f,9.2f):
                                                   objectRng.range(5.5f,8.2f));
            const float crownRadius=treeHeight*(type==1?objectRng.range(.18f,.25f):
                                                objectRng.range(.29f,.43f));
            appendProxyTree(mesh,{x,heightAtBase,z},treeHeight,crownRadius,type,
                            band.detail,objectRng);
            acceptedTrees.push_back({x,heightAtBase,z});
            ++placed;++mesh.backgroundTreeCount;
        }
    }

    // Multi-stem bushes bridge isolated trees into a few broken hedgerow
    // fragments.  Large empty sectors are deliberate and match grazed pasture.
    int backgroundBushes=0,bushAttempts=0;
    while(backgroundBushes<14&&bushAttempts++<500) {
        const float angle=detailRng.range(0,2*pi),radius=detailRng.range(315.0f,650.0f);
        const float hedge=.50f+.34f*std::sin(angle*3.0f+radius*.045f)
                              +.18f*std::sin(angle*8.0f-radius*.026f);
        if(detailRng.unit()>clamp(hedge,.10f,.90f))continue;
        const float x=std::cos(angle)*radius,z=std::sin(angle)*radius;
        if(terrainNormal(x,z).y<(radius<300?.72f:.62f))continue;
        const uint32_t objectSeed=detailRng.next();Rng objectRng(objectSeed^0x85ebca6bu);
        appendProxyBush(mesh,{x,terrainHeight(x,z),z},objectRng.range(.48f,1.18f),
                        objectRng.range(.55f,1.28f),objectRng);
        ++backgroundBushes;++mesh.shrubCount;
    }
    return mesh;
}

} // namespace dense
