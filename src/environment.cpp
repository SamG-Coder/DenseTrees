#include "environment.hpp"
#include "ground_texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dense {
namespace {

constexpr float terrainGridExponent=2.05f;
constexpr float tributaryConfluenceZ=520.0f;
constexpr float wettedChannelFraction=.90f;
constexpr float waterSurfaceLift=.012f;
constexpr float mainChannelDepth=2.40f;
constexpr float tributaryChannelDepth=1.35f;
constexpr float hiddenWaterOverlap=1.00f;

float smoothStep(float low,float high,float value) {
    const float t=clamp((value-low)/(high-low),0.0f,1.0f);
    return t*t*(3.0f-2.0f*t);
}

// One cross-section owns the submerged bed and exposed bank.  Smoothstep's
// zero derivative at both ends makes the centre bed and exact shoreline free
// of creases.  Outside q=1 the bank rises immediately; there is no submerged
// shelf extending tens of metres beyond the visible water.
float channelBankTarget(float surface,float centreDepth,float lateral,
                        float waterHalfWidth) {
    const float q=std::abs(lateral)/std::max(waterHalfWidth,.001f);
    if(q<=1.0f) {
        const float rise=smoothStep(.02f,1.0f,std::pow(q,1.12f));
        return surface-centreDepth*(1.0f-rise);
    }
    constexpr float exposedBankWidth=6.0f;
    const float bank=smoothStep(0.0f,1.0f,
        (std::abs(lateral)-waterHalfWidth)/exposedBankWidth);
    return surface+.68f*bank;
}

float terrainGridCoordinate(int coordinate) {
    constexpr int centre=(EnvironmentGenerator::terrainResolution-1)/2;
    const float centred=static_cast<float>(coordinate-centre)/centre;
    return std::copysign(EnvironmentGenerator::terrainHalfExtent*
                         std::pow(std::abs(centred),terrainGridExponent),centred);
}

bool riverCorridorPoint(float x,float z) {
    const float mainDistance=std::abs(x-EnvironmentGenerator::riverCenterX(z));
    const float joinX=EnvironmentGenerator::riverCenterX(tributaryConfluenceZ);
    const float tributaryDistance=std::abs(z-EnvironmentGenerator::tributaryCenterZ(x));
    return mainDistance<=EnvironmentGenerator::riverWaterHalfWidth(z)+8.0f||
        (x<=joinX+18.0f&&tributaryDistance<=
            EnvironmentGenerator::tributaryWaterHalfWidth(x)+8.0f);
}

bool riverRefinedCell(int cellX,int cellZ) {
    cellX=std::clamp(cellX,0,EnvironmentGenerator::terrainResolution-2);
    cellZ=std::clamp(cellZ,0,EnvironmentGenerator::terrainResolution-2);
    const float x0=terrainGridCoordinate(cellX),x1=terrainGridCoordinate(cellX+1);
    const float z0=terrainGridCoordinate(cellZ),z1=terrainGridCoordinate(cellZ+1);
    for(int zProbe=0;zProbe<3;++zProbe)for(int xProbe=0;xProbe<3;++xProbe) {
        const float x=x0+(x1-x0)*(.5f*xProbe);
        const float z=z0+(z1-z0)*(.5f*zProbe);
        if(riverCorridorPoint(x,z))return true;
    }
    return false;
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

float mapFbm(float x,float z) {
    float sum=0,amplitude=.56f,normalization=0;
    for(int octave=0;octave<5;++octave) {
        sum+=amplitude*meadowValueNoise(x,z);
        normalization+=amplitude;
        const float nextX=x*1.91f-z*.37f+19.7f;
        z=x*.31f+z*1.83f-27.1f;
        x=nextX;
        amplitude*=.51f;
    }
    return sum/normalization;
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

// Both the legacy origin field and the camera-streamed clipmap use this one
// population contract.  At the normal 1.0 height scale, the visible overlay
// scatters blades below each encoded maximum, so codes 13--20 read as roughly
// 3--8 cm mown turf and codes 32--40 as roughly 8--16 cm coarse stems.  The
// packed byte layout remains unchanged.
struct MownTurfPopulation {
    uint32_t packed{};
    float maximumHeight{};
};

MownTurfPopulation makeMownTurfPopulation(Rng& rng,bool coarse) {
    constexpr float heightCodeStep=.004f;
    const float shortHeight=rng.range(.053f,.084f);
    const float coarseHeight=coarse?rng.range(.129f,.164f):0.0f;
    const uint32_t shortCode=static_cast<uint32_t>(
        clamp(shortHeight/heightCodeStep,1,255));
    const uint32_t coarseCode=coarse?static_cast<uint32_t>(
        clamp(coarseHeight/heightCodeStep,1,255)):0u;
    const uint32_t bladeCount=28u+static_cast<uint32_t>(rng.unit()*7.0f);
    const uint32_t coarseCount=coarse?
        1u+static_cast<uint32_t>(rng.unit()*3.0f):0u;
    return {(bladeCount&255u)|(shortCode<<8)|(coarseCount<<16)|(coarseCode<<24),
            coarse?coarseHeight:shortHeight};
}

float mownTurfDensity(const MeadowColourFields& colour) {
    // Broad fertility keeps the lawn full, while the existing coherent dry
    // colonies open a few soft-edged thin areas instead of salt-and-pepper
    // holes chosen independently in every cell.
    return clamp(.88f+.12f*colour.fertility-.28f*colour.dryColony,
                 .58f,1.0f);
}

float coarseTurfProbability(const MeadowColourFields& colour,float habitat) {
    // Coarse stems occur as occasional local accents in lush or sheltered
    // colonies.  They are deliberately rare enough never to form a canopy.
    return clamp(.025f+.035f*colour.fertility+.020f*colour.lushColony+
                 .035f*clamp(habitat,0.0f,1.0f),.025f,.11f);
}

uint32_t worldGrassHash(int cellX,int cellZ,uint32_t seed) {
    uint32_t value=seed^static_cast<uint32_t>(cellX)*0x8da6b343u^
                   static_cast<uint32_t>(cellZ)*0xd8163841u;
    value^=value>>16;value*=0x7feb352du;
    value^=value>>15;value*=0x846ca68bu;
    value^=value>>16;
    return value?value:1u;
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
                                        crownRadius*rng.range(.14f,.20f));
        const float vertical=type==1?treeHeight*rng.range(.035f,.060f):
                             (type==2?treeHeight*rng.range(.055f,.085f):
                                      treeHeight*rng.range(.045f,.075f));
        const Vec3 helper=std::abs(tangent.y)<.9f?Vec3{0,1,0}:Vec3{1,0,0};
        const Vec3 side=normalize(cross(helper,tangent)),around=normalize(cross(tangent,side));
        const float phase=rng.range(0,2*pi);
        for(int pad=0;pad<terminalPadCount;++pad) {
            const float padAngle=phase+2*pi*pad/std::max(1,terminalPadCount)+rng.range(-.32f,.32f);
            // Oak foliage occupies the last several growth units, not a ball
            // pinned to the end of a naked stick.  Trail overlapping pads
            // backwards along the axis and stagger them around it.  This
            // preserves crown windows while removing the proxy "lollipop"
            // silhouette at medium and distant LODs.
            const float padProgress=terminalPadCount>1?
                static_cast<float>(pad)/(terminalPadCount-1):0.0f;
            const float offsetRadius=pad==0?horizontal*rng.range(.05f,.18f):
                                            horizontal*rng.range(.30f,.62f);
            const float along=pad==0?horizontal*rng.range(.05f,.18f):
                                     -horizontal*(.22f+.58f*padProgress);
            const Vec3 offset=side*(std::cos(padAngle)*offsetRadius)
                             +around*(std::sin(padAngle)*offsetRadius)
                             +tangent*along;
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

// Map-scale foliage is intentionally not a reduced copy of the hero tree.
// Five overlapping, branch-aligned pads preserve the broken oak silhouette at
// kilometre distances for a fraction of the geometry of the medium LOD.  The
// elongated pads trail back over their supporting limbs, avoiding both round
// lollipop crowns and the radial spoke pattern of generic billboard trees.
void appendFarCrownPad(EnvironmentMesh& mesh,Vec3 center,Vec3 majorDirection,
                       float majorRadius,float minorRadius,float verticalRadius,
                       int type,Rng& rng) {
    const Vec3 flatDirection{majorDirection.x,0,majorDirection.z};
    const Vec3 major=lengthSq(flatDirection)>.0001f?normalize(flatDirection):Vec3{1,0,0};
    const Vec3 minor{-major.z,0,major.x};
    constexpr int sides=6;
    const uint32_t foliage=type==1?packColor(.105f,.255f,.075f):
                             (type==2?packColor(.165f,.285f,.070f):
                                      packColor(.120f,.285f,.072f));
    const float material=4.0f+type*.1f;
    const uint32_t bottom=static_cast<uint32_t>(mesh.detailVertices.size());
    mesh.detailVertices.push_back({center+Vec3{0,-verticalRadius,0},{0,-1,0},
                                   foliage,material,.5f,0});
    constexpr std::array<float,2> latitude{-.34f,.38f};
    const uint32_t firstRing=static_cast<uint32_t>(mesh.detailVertices.size());
    const float phase=rng.range(-.26f,.26f);
    for(size_t ring=0;ring<latitude.size();++ring)for(int side=0;side<sides;++side) {
        const float angle=2*pi*side/sides+phase;
        const float y=latitude[ring];
        const float profile=std::sqrt(std::max(0.0f,1-y*y));
        const float irregular=.87f+rng.range(-.055f,.075f)+
            .075f*std::sin(angle*3.0f+ring*1.7f+type*.8f);
        const Vec3 local=major*(std::cos(angle)*majorRadius*profile*irregular)+
                         minor*(std::sin(angle)*minorRadius*profile*irregular)+
                         Vec3{0,y*verticalRadius,0};
        const Vec3 normal=normalize(major*(std::cos(angle)/std::max(majorRadius,.01f))+
                                    minor*(std::sin(angle)/std::max(minorRadius,.01f))+
                                    Vec3{0,y/std::max(verticalRadius,.01f),0});
        const float tint=rng.range(.82f,1.11f);
        const float red=(foliage&255)/255.0f,green=((foliage>>8)&255)/255.0f;
        const float blue=((foliage>>16)&255)/255.0f;
        mesh.detailVertices.push_back({center+local,normal,
            packColor(red*tint,green*tint,blue*tint),material,
            static_cast<float>(side)/sides,(y+1)*.5f});
    }
    const uint32_t top=static_cast<uint32_t>(mesh.detailVertices.size());
    mesh.detailVertices.push_back({center+Vec3{0,verticalRadius,0},{0,1,0},foliage,
                                   material,.5f,1});
    for(int side=0;side<sides;++side) {
        const uint32_t next=static_cast<uint32_t>((side+1)%sides);
        mesh.detailIndices.insert(mesh.detailIndices.end(),
            {bottom,firstRing+next,firstRing+static_cast<uint32_t>(side)});
        const uint32_t a=firstRing+static_cast<uint32_t>(side),b=firstRing+next;
        const uint32_t c=a+sides,d=b+sides;
        mesh.detailIndices.insert(mesh.detailIndices.end(),{a,c,d,a,d,b});
        mesh.detailIndices.insert(mesh.detailIndices.end(),{c,d,top});
    }
}

void appendFarTree(EnvironmentMesh& mesh,Vec3 base,Vec3 gradeNormal,float height,
                   float crownRadius,int type,Rng& rng) {
    const uint32_t wood=packColor(.085f,.052f,.028f);
    const float dominant=rng.range(0,2*pi);
    const Vec3 leanDirection{std::cos(dominant),0,std::sin(dominant)};
    const Vec3 gradeBias{gradeNormal.x*.10f,0,gradeNormal.z*.10f};
    const Vec3 lower=base+Vec3{0,height*.34f,0}+leanDirection*(height*rng.range(.015f,.045f));
    const Vec3 fork=base+Vec3{0,height*.61f,0}+
                    leanDirection*(height*rng.range(.045f,.095f))+gradeBias*height;
    appendTube(mesh,base,lower,height*.041f,height*.031f,4,wood,5.0f);
    appendTube(mesh,lower,fork,height*.031f,height*.021f,4,wood,5.0f);

    std::array<Vec3,4> padCenters{};
    std::array<Vec3,4> padDirections{};
    for(int limb=0;limb<4;++limb) {
        // Golden-angle progression plus a dominant-side bias produces unequal
        // reiterations instead of evenly spaced radial arms.
        const float angle=dominant+limb*2.3999632f+rng.range(-.32f,.32f);
        const float broadness=type==1?.74f:(type==2?.86f:1.0f);
        const float limbLength=crownRadius*broadness*rng.range(.67f,1.02f)*
            (limb==0?1.13f:(limb==3?.82f:1.0f));
        const float rise=type==1?rng.range(.34f,.60f):rng.range(.16f,.42f);
        const Vec3 direction=normalize({std::cos(angle),rise,std::sin(angle)});
        const Vec3 sideways{-direction.z,0,direction.x};
        const Vec3 origin=(limb<2?lower:fork)+Vec3{0,height*(.025f*limb),0};
        const Vec3 middle=origin+direction*(limbLength*.48f)+
                          sideways*(limbLength*rng.range(-.075f,.075f));
        const Vec3 tip=middle+direction*(limbLength*.52f)+
                       sideways*(limbLength*rng.range(-.09f,.09f));
        const float radius=height*rng.range(.0085f,.0125f)*(limb<2?1.12f:.92f);
        appendTube(mesh,origin,middle,radius,radius*.64f,3,wood,5.0f);
        appendTube(mesh,middle,tip,radius*.64f,radius*.24f,3,wood,5.0f);
        padCenters[limb]=middle+(tip-middle)*rng.range(.47f,.68f);
        padDirections[limb]=tip-origin;
    }

    const float verticalScale=type==1?.115f:(type==2?.135f:.125f);
    for(int limb=0;limb<4;++limb) {
        const float size=(limb==0?1.08f:(limb==3?.87f:1.0f));
        appendFarCrownPad(mesh,padCenters[limb],padDirections[limb],
            crownRadius*rng.range(.28f,.38f)*size,
            crownRadius*rng.range(.17f,.25f)*size,
            height*rng.range(verticalScale*.76f,verticalScale*1.08f)*size,
            type,rng);
    }
    // An off-centre interior mass fuses the four scaffold reiterations while
    // retaining one deliberate crown window on the non-dominant side.
    const Vec3 crownDirection{std::cos(dominant+.42f),0,std::sin(dominant+.42f)};
    const Vec3 crownCenter=fork+Vec3{0,height*rng.range(.105f,.19f),0}+
                           crownDirection*(crownRadius*rng.range(.04f,.14f));
    appendFarCrownPad(mesh,crownCenter,crownDirection,
        crownRadius*rng.range(.30f,.42f),crownRadius*rng.range(.20f,.29f),
        height*rng.range(.10f,.15f),type,rng);
}

void appendProxyBush(EnvironmentMesh& mesh,Vec3 base,float radius,float height,int variant,
                     Rng& rng) {
    const uint32_t wood=packColor(.105f,.068f,.038f);
    const uint32_t foliage=variant==1?packColor(.115f,.285f,.075f):
                             (variant==2?packColor(.19f,.315f,.072f):
                                         packColor(.125f,.305f,.080f));
    const int stems=(variant==1?7:5)+static_cast<int>(rng.next()%3u);
    for(int stem=0;stem<stems;++stem) {
        const float angle=2*pi*stem/stems+rng.range(-.22f,.22f);
        const float lean=rng.range(.22f,.38f);
        const Vec3 middle=base+Vec3{std::cos(angle)*radius*lean,height*rng.range(.35f,.49f),
                                    std::sin(angle)*radius*lean};
        const Vec3 tip=base+Vec3{std::cos(angle)*radius*rng.range(.68f,1.0f),
                                 height*rng.range(.68f,1.0f),
                                 std::sin(angle)*radius*rng.range(.68f,1.0f)};
        appendTube(mesh,base,middle,height*.018f,height*.010f,4,wood,5.0f);
        appendTube(mesh,middle,tip,height*.010f,.003f,4,wood,5.0f);
        if((stem&1)==0||variant==1) {
            const Vec3 inner=middle+(tip-middle)*rng.range(.18f,.42f);
            appendFoliageClump(mesh,inner,
                               {radius*rng.range(.21f,.34f),height*rng.range(.12f,.19f),
                                radius*rng.range(.19f,.32f)},
                               foliage,4.2f,rng.next(),5);
        }
        appendFoliageClump(mesh,tip,{radius*rng.range(.18f,.28f),height*rng.range(.14f,.22f),
                                    radius*rng.range(.17f,.27f)},
                           foliage,4.2f,rng.next(),5);
    }
}

struct GrassIsland { float x,z,rx,rz,phase; };

struct PopulationSite {
    float height{};
    Vec3 normal{0,1,0};
    float slope{};
    float riverBankDistance{10000.0f};
    float floodplainInfluence{};
    float exposure{};
    GroundBiomeWeights biome{};
    bool channelClear{true};
};

PopulationSite populationSite(float x,float z) {
    PopulationSite site;
    site.height=EnvironmentGenerator::terrainHeight(x,z);
    site.normal=EnvironmentGenerator::terrainNormal(x,z);
    site.slope=std::sqrt(site.normal.x*site.normal.x+site.normal.z*site.normal.z)/
               std::max(site.normal.y,.08f);

    const float mainLateral=std::abs(x-EnvironmentGenerator::riverCenterX(z));
    const float mainWidth=EnvironmentGenerator::riverHalfWidth(z);
    const float mainBank=std::max(0.0f,mainLateral-mainWidth);

    const float joinX=EnvironmentGenerator::riverCenterX(tributaryConfluenceZ);
    const float tributaryActive=
        1.0f-smoothStep(joinX-45.0f,joinX+18.0f,x);
    const float tributaryLateral=std::abs(z-EnvironmentGenerator::tributaryCenterZ(x));
    const float tributaryWidth=EnvironmentGenerator::tributaryHalfWidth(x);
    const float tributaryBank=tributaryActive>.001f?
        std::max(0.0f,tributaryLateral-tributaryWidth):10000.0f;

    site.riverBankDistance=std::min(mainBank,tributaryBank);
    const float broadWet=mapFbm(x*.0017f+113.0f,z*.0017f-67.0f);
    const float riverMoisture=1.0f-smoothStep(18.0f,285.0f,site.riverBankDistance);
    const float moisture=clamp(.27f+.58f*riverMoisture+.24f*(broadWet-.35f)-
                               .12f*smoothStep(.22f,.75f,site.slope),0.0f,1.0f);
    site.floodplainInfluence=riverMoisture;
    site.exposure=clamp(.55f*mapFbm(x*.0028f-79.0f,z*.0028f+37.0f)+
                        .45f*mapFbm(x*.0091f+17.0f,z*.0091f-131.0f),0.0f,1.0f);
    site.biome=groundBiomeWeights({site.height,site.slope,site.riverBankDistance,
                                    moisture,site.exposure});
    site.channelClear=mainLateral>mainWidth+20.0f&&
        (tributaryActive<.05f||tributaryLateral>tributaryWidth+20.0f);
    return site;
}

} // namespace

float EnvironmentGenerator::riverCenterX(float z) {
    return 330.0f+110.0f*std::sin(z/610.0f+.40f)
                 +45.0f*std::sin(z/185.0f-1.0f);
}

float EnvironmentGenerator::riverHalfWidth(float z) {
    return 34.0f+5.0f*std::sin(z/430.0f+.8f)
                +3.0f*std::sin(z/137.0f-1.3f);
}

float EnvironmentGenerator::riverBedHeight(float z) {
    return riverSurfaceHeight(z)-mainChannelDepth;
}

float EnvironmentGenerator::riverSurfaceHeight(float z) {
    // 2.4 metres of fall per kilometre.  This remains strictly downhill even
    // through the mountain pass and is deliberately independent of noise.
    return -5.68f-.00240f*z;
}

float EnvironmentGenerator::riverWaterHalfWidth(float z) {
    return riverHalfWidth(z)*wettedChannelFraction;
}

float EnvironmentGenerator::tributaryCenterZ(float x) {
    const float joinX=riverCenterX(tributaryConfluenceZ);
    const float upstream=joinX-x;
    const float developed=smoothStep(0.0f,360.0f,std::max(0.0f,upstream));
    return tributaryConfluenceZ+.105f*upstream+
           82.0f*std::sin(upstream*.0021f)*developed;
}

float EnvironmentGenerator::tributaryHalfWidth(float x) {
    const float joinX=riverCenterX(tributaryConfluenceZ);
    const float upstream=joinX-x;
    return 18.0f+2.8f*std::sin(upstream/310.0f+.45f)
                +1.8f*std::sin(upstream/103.0f-1.1f);
}

float EnvironmentGenerator::tributarySurfaceHeight(float x) {
    const float joinX=riverCenterX(tributaryConfluenceZ);
    const float confluenceReach=riverWaterHalfWidth(tributaryConfluenceZ);
    // The visible tributary reaches the main river at its near shoreline,
    // then remains on the main plane beneath the final hidden overlap.  This
    // avoids both a vertical water step and a second sheet below refracted
    // main-river rays.
    const float visibleUpstream=std::max(0.0f,joinX-x-confluenceReach);
    return riverSurfaceHeight(tributaryConfluenceZ)+.00310f*visibleUpstream;
}

float EnvironmentGenerator::tributaryBedHeight(float x) {
    return tributarySurfaceHeight(x)-tributaryChannelDepth;
}

float EnvironmentGenerator::tributaryWaterHalfWidth(float x) {
    return tributaryHalfWidth(x)*wettedChannelFraction;
}

PersistentWaterSample EnvironmentGenerator::persistentWater(float x,float z) {
    PersistentWaterSample result;
    if(!std::isfinite(x)||!std::isfinite(z))return result;

    const bool mainDomain=z>=-terrainHalfExtent&&z<=terrainHalfExtent;
    const float mainShore=riverWaterHalfWidth(z);
    const float mainCoordinate=std::abs(x-riverCenterX(z))/
                               std::max(mainShore,.001f);

    const float joinX=riverCenterX(tributaryConfluenceZ);
    const bool tributaryDomain=x>=-terrainHalfExtent&&x<=joinX;
    const float tributaryShore=tributaryWaterHalfWidth(x);
    const float tributaryCoordinate=std::abs(z-tributaryCenterZ(x))/
                                    std::max(tributaryShore,.001f);

    // One ulp of coordinate arithmetic at kilometre-scale positions must not
    // flicker a point authored exactly on the shoreline in and out of water.
    constexpr float shorelineTolerance=1.0e-4f;
    const bool inMain=mainDomain&&mainCoordinate<=1.0f+shorelineTolerance;
    const bool inTributary=tributaryDomain&&
                           tributaryCoordinate<=1.0f+shorelineTolerance;
    result.inside=inMain||inTributary;
    if(!result.inside) {
        result.shoreCoordinate=std::min(mainCoordinate,tributaryCoordinate);
        return result;
    }

    // The main sheet is the upper, authoritative surface throughout the
    // confluence overlap; the tributary mesh is deliberately tucked beneath
    // it there to avoid a coplanar T-junction.
    const bool useTributary=inTributary&&!inMain;
    result.shoreCoordinate=clamp(
        useTributary?tributaryCoordinate:mainCoordinate,0.0f,1.0f);
    result.surfaceHeight=(useTributary?tributarySurfaceHeight(x):
                                         riverSurfaceHeight(z))+waterSurfaceLift;
    result.depth=std::max(result.surfaceHeight-terrainHeight(x,z),0.0f);
    return result;
}

float EnvironmentGenerator::terrainHeight(float x,float z) {
    const float radius=std::sqrt(x*x+z*z);
    const float rootMask=smoothStep(1.65f,4.6f,radius);
    const float ellipticalRadiusSq=x*x*.84f+z*z*1.12f;
    const float heroKnoll=-2.75f*(1.0f-std::exp(-ellipticalRadiusSq/275.0f));

    // Continental-scale fields establish broad watersheds.  Frequencies are
    // in hundreds of metres, eliminating the repeating test-map ripples.
    const float continental=(mapFbm(x*.00043f+17.1f,z*.00043f-9.7f)-.5f)*8.5f;
    const float rolling=(mapFbm(x*.00165f-31.4f,z*.00165f+24.8f)-.5f)*2.2f;
    const float meadowMicro=(mapFbm(x*.032f+7.0f,z*.032f-43.0f)-.5f)*.42f+
                            (mapFbm(x*.085f-63.0f,z*.085f+11.0f)-.5f)*.12f;
    const float pastureMask=1.0f-smoothStep(1350.0f,2050.0f,radius);
    float terrain=heroKnoll+(continental*.34f+rolling)*smoothStep(7.0f,34.0f,radius)
                 *(.55f+.45f*pastureMask)+
                 meadowMicro*(1.0f-smoothStep(620.0f,940.0f,radius));

    // A rock escarpment breaks the western middle distance and creates a real
    // ridge/gorge relationship with the tributary instead of decorative
    // radial hills.  Its broad Gaussian profile remains heightfield-safe.
    const float escarpmentAxis=x+1320.0f-.20f*z;
    const float escarpmentAlong=z+180.0f;
    const float escarpmentEnvelope=smoothStep(-1550.0f,-1120.0f,escarpmentAlong)*
                                    (1.0f-smoothStep(1150.0f,1530.0f,escarpmentAlong));
    const float escarpmentProfile=std::exp(-(escarpmentAxis*escarpmentAxis)/(185.0f*185.0f));
    const float rockyBands=.88f+.10f*std::sin((x+z*.24f)*.015f)+
                                  .05f*std::sin((x-z*.41f)*.041f);
    terrain+=38.0f*escarpmentEnvelope*escarpmentProfile*rockyBands;

    // The map is bounded by a distant, irregular horseshoe range with an open
    // southern river outlet.  These chains sit 2.2--3.0 km from the oak,
    // roughly twice the former test-map distance.
    const float northCenter=2520.0f+175.0f*std::sin(x/690.0f+.25f)
                                   +70.0f*std::sin(x/247.0f-1.1f);
    const float northD=z-northCenter;
    const float northProfile=std::exp(-(northD*northD)/(340.0f*340.0f));
    const float northEnvelope=1.0f-smoothStep(2750.0f,3200.0f,std::abs(x));
    const float northPeakField=mapFbm(x*.00105f+73.0f,z*.00105f-41.0f);
    const float northRange=northProfile*northEnvelope*(150.0f+155.0f*northPeakField)*
                           (.91f+.09f*std::sin(x*.0085f+z*.0017f));

    const float westCenter=-2670.0f+145.0f*std::sin(z/610.0f-.5f);
    const float westD=x-westCenter;
    const float westProfile=std::exp(-(westD*westD)/(315.0f*315.0f));
    const float westEnvelope=smoothStep(-2550.0f,-2050.0f,z)*
                             (1.0f-smoothStep(2450.0f,2940.0f,z));
    const float westRange=westProfile*westEnvelope*
        (125.0f+135.0f*mapFbm(x*.00114f-16.0f,z*.00114f+88.0f));

    const float eastCenter=2780.0f+120.0f*std::sin(z/720.0f+.9f);
    const float eastD=x-eastCenter;
    const float eastProfile=std::exp(-(eastD*eastD)/(360.0f*360.0f));
    const float eastEnvelope=smoothStep(-2200.0f,-1660.0f,z)*
                             (1.0f-smoothStep(2180.0f,2760.0f,z));
    const float eastRange=eastProfile*eastEnvelope*
        (100.0f+115.0f*mapFbm(x*.00102f+137.0f,z*.00102f-62.0f));
    terrain+=std::max({northRange,westRange,eastRange});

    const float northFoothillD=z-(northCenter-480.0f);
    terrain+=28.0f*std::exp(-(northFoothillD*northFoothillD)/(610.0f*610.0f))*
             northEnvelope*(.55f+.45f*mapFbm(x*.0017f+9.0f,z*.0017f+117.0f));

    // Tributary valley first, then the main valley.  Applying the main river
    // last guarantees a clean, deeper confluence.  Valley and floodplain
    // targets only lower terrain; channel centres are authored elevations.
    const float joinX=riverCenterX(tributaryConfluenceZ);
    const float tributaryLateral=std::abs(z-tributaryCenterZ(x));
    const float tributaryWidth=tributaryHalfWidth(x);
    const float tributaryActive=
        1.0f-smoothStep(joinX-45.0f,joinX+18.0f,x);
    const float tributaryFloodWidth=115.0f+22.0f*std::sin((joinX-x)/470.0f);
    const float tributaryValley=tributaryActive*
        (1.0f-smoothStep(tributaryFloodWidth,tributaryFloodWidth+300.0f,
                         tributaryLateral));
    const float tributaryValleyTarget=tributarySurfaceHeight(x)+2.0f+
        3.4f*std::pow(clamp(tributaryLateral/(tributaryFloodWidth+1.0f),0.0f,1.0f),1.45f);
    terrain=terrain+(std::min(terrain,tributaryValleyTarget)-terrain)*tributaryValley*.78f;
    const float tributaryFlood=tributaryActive*
        (1.0f-smoothStep(tributaryFloodWidth*.72f,tributaryFloodWidth,
                         tributaryLateral));
    const float tributaryFloodTarget=tributarySurfaceHeight(x)+.72f+
        1.25f*std::pow(tributaryLateral/(tributaryFloodWidth+1.0f),1.7f);
    terrain=terrain+(std::min(terrain,tributaryFloodTarget)-terrain)*tributaryFlood;
    const float tributaryChannel=tributaryActive*
        (1.0f-smoothStep(tributaryWidth*1.75f,tributaryWidth*1.95f,
                         tributaryLateral));
    const float tributaryChannelTarget=channelBankTarget(
        tributarySurfaceHeight(x),tributaryChannelDepth,tributaryLateral,
        tributaryWaterHalfWidth(x));
    terrain=terrain+(tributaryChannelTarget-terrain)*tributaryChannel;

    const float mainLateral=std::abs(x-riverCenterX(z));
    const float mainWidth=riverHalfWidth(z);
    const float floodWidth=205.0f+42.0f*std::sin(z/760.0f+.3f)
                                  +20.0f*std::sin(z/233.0f-1.0f);
    const float mainValley=1.0f-smoothStep(floodWidth,floodWidth+430.0f,mainLateral);
    const float mainValleyTarget=riverSurfaceHeight(z)+2.7f+
        4.6f*std::pow(clamp(mainLateral/(floodWidth+1.0f),0.0f,1.0f),1.5f);
    terrain=terrain+(std::min(terrain,mainValleyTarget)-terrain)*mainValley*.80f;
    const float mainFloodplain=1.0f-smoothStep(floodWidth*.72f,floodWidth,mainLateral);
    const float floodplainTexture=(mapFbm(x*.008f+53.0f,z*.008f-71.0f)-.5f)*.14f;
    const float mainFloodTarget=riverSurfaceHeight(z)+1.05f+floodplainTexture+
        1.45f*std::pow(mainLateral/(floodWidth+1.0f),1.8f);
    terrain=terrain+(std::min(terrain,mainFloodTarget)-terrain)*mainFloodplain;
    const float mainChannel=1.0f-smoothStep(mainWidth*1.75f,mainWidth*1.95f,
                                            mainLateral);
    const float mainChannelTarget=channelBankTarget(
        riverSurfaceHeight(z),mainChannelDepth,mainLateral,
        riverWaterHalfWidth(z));
    terrain=terrain+(mainChannelTarget-terrain)*mainChannel;

    // Treat the two hydraulic interiors as a union at the confluence. A bank
    // target from either channel may never raise solid terrain through the
    // other channel's water sheet.
    const bool insideMain=mainLateral<=riverWaterHalfWidth(z);
    const bool insideTributary=tributaryActive>.001f&&
        tributaryLateral<=tributaryWaterHalfWidth(x);
    if(insideMain||insideTributary) {
        float hydraulicBed=std::numeric_limits<float>::max();
        if(insideMain)hydraulicBed=std::min(hydraulicBed,mainChannelTarget);
        if(insideTributary)hydraulicBed=std::min(hydraulicBed,tributaryChannelTarget);
        terrain=std::min(terrain,hydraulicBed);
    }

    return rootMask*terrain;
}

Vec3 EnvironmentGenerator::terrainNormal(float x,float z) {
    constexpr float epsilon=.12f;
    const float dx=(terrainHeight(x+epsilon,z)-terrainHeight(x-epsilon,z))/(2*epsilon);
    const float dz=(terrainHeight(x,z+epsilon)-terrainHeight(x,z-epsilon))/(2*epsilon);
    return normalize({-dx,1.0f,-dz});
}

TerrainSurfaceSample EnvironmentGenerator::sampleTerrainSurface(float x,float z) {
    if(!std::isfinite(x)||!std::isfinite(z))return {};

    constexpr int centre=(terrainResolution-1)/2;
    const bool inside=x>=-terrainHalfExtent&&x<=terrainHalfExtent&&
                      z>=-terrainHalfExtent&&z<=terrainHalfExtent;
    const float worldX=clamp(x,-terrainHalfExtent,terrainHalfExtent);
    const float worldZ=clamp(z,-terrainHalfExtent,terrainHalfExtent);
    const auto gridPosition=[](float world) {
        const float normalized=clamp(world/terrainHalfExtent,-1.0f,1.0f);
        const float uniform=std::copysign(
            std::pow(std::abs(normalized),1.0f/terrainGridExponent),normalized);
        return centre+uniform*centre;
    };
    const float gridX=gridPosition(worldX),gridZ=gridPosition(worldZ);
    const int cellX=std::clamp(static_cast<int>(std::floor(gridX)),0,
                               terrainResolution-2);
    const int cellZ=std::clamp(static_cast<int>(std::floor(gridZ)),0,
                               terrainResolution-2);
    if(riverRefinedCell(cellX,cellZ))
        return {{worldX,terrainHeight(worldX,worldZ),worldZ},
                terrainNormal(worldX,worldZ),inside};
    const float x0=terrainGridCoordinate(cellX),x1=terrainGridCoordinate(cellX+1);
    const float z0=terrainGridCoordinate(cellZ),z1=terrainGridCoordinate(cellZ+1);
    const float u=clamp((worldX-x0)/(x1-x0),0.0f,1.0f);
    const float v=clamp((worldZ-z0)/(z1-z0),0.0f,1.0f);

    const Vec3 a{x0,terrainHeight(x0,z0),z0};
    const Vec3 b{x1,terrainHeight(x1,z0),z0};
    const Vec3 c{x0,terrainHeight(x0,z1),z1};
    const Vec3 d{x1,terrainHeight(x1,z1),z1};
    Vec3 p0,p1,p2;float w0,w1,w2;
    if(((cellX+cellZ)&1)==0) {
        if(v>=u) {
            p0=a;p1=c;p2=d;w0=1-v;w1=v-u;w2=u;
        } else {
            p0=a;p1=d;p2=b;w0=1-u;w1=v;w2=u-v;
        }
    } else if(u+v<=1.0f) {
        p0=a;p1=c;p2=b;w0=1-u-v;w1=v;w2=u;
    } else {
        p0=b;p1=c;p2=d;w0=1-v;w1=1-u;w2=u+v-1;
    }
    const float height=p0.y*w0+p1.y*w1+p2.y*w2;
    return {{worldX,height,worldZ},normalize(cross(p1-p0,p2-p0)),inside};
}

float EnvironmentGenerator::rootLoamWeight(float x,float z) {
    const float radius=std::sqrt(x*x+z*z);
    const float azimuth=std::atan2(z,x);
    // This formula is intentionally limited to elementary HLSL operations.
    // The common 5/9-lobe offset breaks the circular boundary while keeping a
    // guaranteed 1.05 m grass-free core. A separate three-lobe term varies the
    // transition width between .75 and .87 m without moving that core.
    const float sharedOffset=.055f*std::sin(5*azimuth+.60f)+
                             .035f*std::sin(9*azimuth-1.20f);
    const float coreRadius=1.14f+sharedOffset;
    const float meadowRadius=1.95f+sharedOffset+
                             .060f*std::sin(3*azimuth+1.70f);
    return 1.0f-smoothStep(coreRadius,meadowRadius,radius);
}

bool EnvironmentGenerator::makeGrassPatch(int cellX,int cellZ,uint32_t seed,
                                           GrassPatchGpu& patch) {
    Rng rng(worldGrassHash(cellX,cellZ,seed));
    const float x=(static_cast<float>(cellX)+rng.range(.13f,.87f))*grassCellSize;
    const float z=(static_cast<float>(cellZ)+rng.range(.13f,.87f))*grassCellSize;
    const TerrainSurfaceSample surface=sampleTerrainSurface(x,z);
    if(!surface.insideBounds)return false;

    const float heroRadius=std::sqrt(x*x+z*z);
    const float rootGrassSuitability=1.0f-rootLoamWeight(x,z);
    if(rootGrassSuitability<=0)return false;
    const Vec3 normal=surface.normal;
    const float slope=std::sqrt(normal.x*normal.x+normal.z*normal.z)/
                      std::max(normal.y,.20f);
    // Permanent water and its exposed gravel lip remain blade-free.  Grass
    // resumes through a soft riparian transition rather than drawing a hard
    // green line exactly on the hydraulic shoreline.
    const float mainLateral=std::abs(x-riverCenterX(z));
    const float mainBank=mainLateral-riverWaterHalfWidth(z);
    const float joinX=riverCenterX(tributaryConfluenceZ);
    const float tributaryActive=
        1.0f-smoothStep(joinX-45.0f,joinX+18.0f,x);
    const float tributaryLateral=std::abs(z-tributaryCenterZ(x));
    const float tributaryBank=tributaryActive>.05f?
        tributaryLateral-tributaryWaterHalfWidth(x):10000.0f;
    if(mainBank<=0||tributaryBank<=0||normal.y<.80f)return false;
    const float bankDistance=std::min(mainBank,tributaryBank);

    const MeadowColourFields colour=meadowColourFields(x,z);
    const float broadWet=mapFbm(x*.0038f+47.0f,z*.0038f-29.0f);
    const float riverMoisture=1.0f-smoothStep(18.0f,285.0f,bankDistance);
    const float moisture=clamp(.25f+.58f*riverMoisture+
        .25f*(broadWet-.35f)-.22f*smoothStep(18.0f,105.0f,surface.position.y),0,1);
    const float exposure=mapFbm(x*.0061f-83.0f,z*.0061f+51.0f);
    const GroundBiomeWeights biome=groundBiomeWeights(
        {surface.position.y,slope,bankDistance,moisture,exposure});
    const float meadow=biome.material[static_cast<size_t>(GroundMaterialTile::MeadowTurf)];
    const float upland=biome.material[static_cast<size_t>(GroundMaterialTile::UplandShortTurf)];
    const float mineral=biome.material[static_cast<size_t>(GroundMaterialTile::ExposedRockSoil)];
    const float riparian=biome.material[static_cast<size_t>(GroundMaterialTile::RiparianMoss)];
    const float bareDriver=exposure*.58f+(1-moisture)*.42f;
    const float coherentBare=smoothStep(.58f,.82f,bareDriver);
    // Mineral cliffs, exposed gravel/sand and coherent dry-soil colonies are
    // not sparse meadow.  Reject them before stochastic thinning so the
    // streamed clipmap cannot sprinkle isolated green blades across a dirt or
    // rock biome merely because normalized biome weights retain a tiny meadow
    // transition component.
    const bool mineralDominant=mineral>.38f&&mineral>meadow*1.05f&&
                               mineral>riparian*1.35f;
    const bool exposedBare=coherentBare>.64f&&
                           (mineral>.14f||meadow<.52f||moisture<.28f);
    if(mineralDominant||exposedBare)return false;
    float suitability=meadow*.98f+upland*.30f+riparian*.78f;
    suitability*=1-.94f*coherentBare;
    suitability*=smoothStep(.80f,.93f,normal.y);
    suitability*=smoothStep(1.8f,9.0f,bankDistance);
    suitability*=mownTurfDensity(colour);
    suitability*=rootGrassSuitability;
    if(rng.unit()>clamp(suitability,0.0f,1.0f))return false;

    const float coarseHabitat=clamp(.25f*meadow+.65f*riparian+
                                    .10f*upland-.45f*mineral,0.0f,1.0f);
    const bool coarse=rng.unit()<coarseTurfProbability(colour,coarseHabitat);
    const float canopyShade=1.0f-.26f*(1.0f-smoothStep(5.0f,11.0f,heroRadius));
    if(rng.unit()>canopyShade)return false;
    const MownTurfPopulation population=makeMownTurfPopulation(rng,coarse);
    const float maximumHeight=population.maximumHeight*2.5f;
    const float lateralRatio=coarse?.66f:.52f;
    const float maximumWidth=coarse?.043f:.018f;
    constexpr float boundsSafety=.012f;
    const float horizontalReach=.245f+maximumHeight*(slope+lateralRatio)+
                                maximumWidth+boundsSafety;
    const float surfaceRise=.245f*slope;
    const float lowerReach=surfaceRise+maximumWidth*slope+boundsSafety;
    const float upperReach=surfaceRise+maximumHeight*(normal.y+lateralRatio*slope)+
                           maximumWidth*slope+boundsSafety;
    const float baseY=surface.position.y+.006f;
    // Camera-streamed grass intentionally does not invent local standing
    // water. Global flood height still submerges these blades; retained
    // micro-puddles remain driven by the terrain's baked runoff map.
    const uint32_t packedSeed=rng.next()&0x00ffffffu;
    patch={x-horizontalReach,baseY-lowerReach,z-horizontalReach,
           x+horizontalReach,baseY+upperReach,z+horizontalReach,
           packedSeed,population.packed,baseY,normal.x,normal.z,moisture,
           colour.fertility,colour.dryColony,colour.lushColony,
           colour.warmCool};
    return true;
}

EnvironmentMesh EnvironmentGenerator::build(uint32_t seed) const {
    EnvironmentMesh mesh;
    mesh.grassSeed=seed;
    constexpr int resolution=terrainResolution;
    mesh.terrainVertices.reserve(static_cast<size_t>(resolution)*resolution);
    mesh.terrainIndices.reserve(static_cast<size_t>(resolution-1)*(resolution-1)*6);
    mesh.minimumHeight=std::numeric_limits<float>::max();
    mesh.maximumHeight=std::numeric_limits<float>::lowest();

    // Hydrology is solved once on the CPU.  A location can retain standing
    // water only when every sampled escape direction crosses a higher rim.
    // This spill-depth test rejects convex hilltops even when their immediate
    // surface is flat, while multi-scale concavity captures shallow meadow
    // depressions.  Suitability is stored in vertex-colour alpha (RGB remains
    // visible colour), adding no vertex stride or per-ray terrain probes.
    const size_t terrainVertexCount=static_cast<size_t>(resolution)*resolution;
    std::vector<float> gridCoordinates(resolution),heightGrid(terrainVertexCount),
                       waterRetention(terrainVertexCount);
    std::vector<Vec3> normalGrid(terrainVertexCount);
    for(int coordinate=0;coordinate<resolution;++coordinate)
        gridCoordinates[coordinate]=terrainGridCoordinate(coordinate);
    const auto gridIndex=[&](int x,int z){return static_cast<size_t>(z)*resolution+x;};
    for(int z=0;z<resolution;++z)for(int x=0;x<resolution;++x) {
        const size_t index=gridIndex(x,z);
        const float worldX=gridCoordinates[x],worldZ=gridCoordinates[z];
        heightGrid[index]=terrainHeight(worldX,worldZ);
        normalGrid[index]=terrainNormal(worldX,worldZ);
    }
    for(int z=0;z<resolution;++z)for(int x=0;x<resolution;++x) {
        const size_t index=gridIndex(x,z);const Vec3 normal=normalGrid[index];
        const float worldX=gridCoordinates[x],worldZ=gridCoordinates[z];
        const float mainDistance=std::abs(worldX-riverCenterX(worldZ));
        const float mainChannel=1.0f-smoothStep(riverHalfWidth(worldZ)*.72f,
                                                riverHalfWidth(worldZ)*1.10f,
                                                mainDistance);
        const float tributaryJoinX=riverCenterX(tributaryConfluenceZ);
        const float tributaryActive=
            1.0f-smoothStep(tributaryJoinX-45.0f,tributaryJoinX+18.0f,worldX);
        const float tributaryDistance=std::abs(worldZ-tributaryCenterZ(worldX));
        const float tributaryChannel=tributaryActive*
            (1.0f-smoothStep(tributaryHalfWidth(worldX)*.70f,
                             tributaryHalfWidth(worldX)*1.10f,tributaryDistance));
        const float mappedChannelRetention=std::max(mainChannel,tributaryChannel)*
            smoothStep(.990268f,.997564f,normal.y);
        if(std::sqrt(worldX*worldX+worldZ*worldZ)>grassHalfExtent){
            waterRetention[index]=mappedChannelRetention;continue;
        }
        const float height=heightGrid[index];float minimumRim=std::numeric_limits<float>::max();
        float meanSix=0,meanSixteen=0;
        for(int direction=0;direction<8;++direction) {
            const float angle=2*pi*direction/8.0f,dx=std::cos(angle),dz=std::sin(angle);
            const float heightThree=terrainHeight(worldX+dx*3.0f,worldZ+dz*3.0f);
            const float heightSix=terrainHeight(worldX+dx*6.0f,worldZ+dz*6.0f);
            const float heightTen=terrainHeight(worldX+dx*10.0f,worldZ+dz*10.0f);
            const float heightSixteen=terrainHeight(worldX+dx*16.0f,worldZ+dz*16.0f);
            minimumRim=std::min(minimumRim,std::max({height,heightThree,heightSix,
                                                     heightTen,heightSixteen}));
            meanSix+=heightSix;meanSixteen+=heightSixteen;
        }
        meanSix*=.125f;meanSixteen*=.125f;
        const float spillDepth=std::max(0.0f,minimumRim-height);
        const float sixMetrePosition=meanSix-height;
        const float sixteenMetrePosition=meanSixteen-height;
        const float flat=smoothStep(.990268f,.997564f,normal.y);
        const float depth=smoothStep(.008f,.12f,spillDepth);
        const float concavity=std::max(smoothStep(.01f,.10f,sixMetrePosition),
            .75f*smoothStep(.04f,.32f,sixteenMetrePosition));
        const float notRidge=1-smoothStep(.02f,.18f,
                                          std::max(0.0f,-sixteenMetrePosition));
        waterRetention[index]=clamp(std::max(flat*depth*(.65f+.35f*concavity)*notRidge,
                                              mappedChannelRetention),0.0f,1.0f);
    }
    for(int z=0;z<resolution;++z) {
        const float worldZ=gridCoordinates[z];
        for(int x=0;x<resolution;++x) {
            const float worldX=gridCoordinates[x];const size_t index=gridIndex(x,z);
            const float y=heightGrid[index];const Vec3 normal=normalGrid[index];
            const float broadColour=mapFbm(worldX*.0042f+12.0f,worldZ*.0042f-37.0f);
            const float fineColour=mapFbm(worldX*.013f-81.0f,worldZ*.013f+26.0f);
            const float mainDistance=std::abs(worldX-riverCenterX(worldZ));
            const float floodplain=1.0f-smoothStep(120.0f,285.0f,mainDistance);
            const float altitudeRock=smoothStep(32.0f,128.0f,y);
            const float slopeRock=smoothStep(.08f,.32f,1.0f-normal.y);
            const float rock=clamp(std::max(altitudeRock*.72f,slopeRock),0.0f,1.0f);
            const Vec3 meadow{.175f+.050f*broadColour+.018f*fineColour,
                              .285f+.072f*broadColour+.025f*fineColour,
                              .115f+.030f*broadColour};
            const Vec3 wetMeadow{meadow.x*.72f,meadow.y*.84f,meadow.z*.78f};
            const Vec3 pasture=meadow+(wetMeadow-meadow)*floodplain*.58f;
            const float rockTone=.72f+.24f*mapFbm(worldX*.008f+91.0f,
                                                  worldZ*.008f-13.0f);
            const Vec3 stone{.31f*rockTone,.295f*rockTone,.265f*rockTone};
            const Vec3 vertexColour=pasture+(stone-pasture)*rock;
            uint32_t color=packColor(vertexColour.x,vertexColour.y,vertexColour.z);
            const uint32_t retentionByte=static_cast<uint32_t>(
                clamp(waterRetention[index]*255.0f+0.5f,0.0f,255.0f));
            color=(color&0x00ffffffu)|(retentionByte<<24);
            mesh.terrainVertices.push_back({{worldX,y,worldZ},normal,color,2.0f,
                                            worldX*.08f,worldZ*.08f});
            mesh.minimumHeight=std::min(mesh.minimumHeight,y);
            mesh.maximumHeight=std::max(mesh.maximumHeight,y);
        }
    }
    struct RefinedTerrainCell { int x{},z{}; std::array<Vec3,4> corner; };
    std::vector<RefinedTerrainCell> refinedTerrainCells;
    for(int z=0;z<resolution-1;++z)for(int x=0;x<resolution-1;++x) {
        const uint32_t a=static_cast<uint32_t>(z*resolution+x),b=a+1;
        const uint32_t c=a+static_cast<uint32_t>(resolution),d=c+1;
        // Replace any coarse cell touching a channel corridor.  Keeping a
        // global-grid triangle whose distant corners straddle the river would
        // bridge over the explicit bed and make the player walk on water.
        const Vec3&pa=mesh.terrainVertices[a].position;
        const Vec3&pb=mesh.terrainVertices[b].position;
        const Vec3&pc=mesh.terrainVertices[c].position;
        const Vec3&pd=mesh.terrainVertices[d].position;
        if(riverRefinedCell(x,z)) {
            refinedTerrainCells.push_back({x,z,{pa,pb,pc,pd}});continue;
        }
        if(((x+z)&1)==0)mesh.terrainIndices.insert(mesh.terrainIndices.end(),{a,c,d,a,d,b});
        else mesh.terrainIndices.insert(mesh.terrainIndices.end(),{a,c,b,b,c,d});
    }

    // Exactly replace each removed coarse cell with an adaptive four-metre
    // grid.  The outer vertices coincide with the retained heightfield cell,
    // proving full coverage without either gaps or coplanar overlap.
    for(const auto&refined:refinedTerrainCells) {
        const auto&cell=refined.corner;
        const float x0=cell[0].x,x1=cell[1].x;
        const float z0=cell[0].z,z1=cell[2].z;
        const int xSegments=std::max(1,static_cast<int>(std::ceil((x1-x0)/4.0f)));
        const int zSegments=std::max(1,static_cast<int>(std::ceil((z1-z0)/4.0f)));
        const bool clampWest=refined.x>0&&!riverRefinedCell(refined.x-1,refined.z);
        const bool clampEast=refined.x+1<resolution-1&&
                             !riverRefinedCell(refined.x+1,refined.z);
        const bool clampSouth=refined.z>0&&!riverRefinedCell(refined.x,refined.z-1);
        const bool clampNorth=refined.z+1<resolution-1&&
                             !riverRefinedCell(refined.x,refined.z+1);
        const uint32_t base=static_cast<uint32_t>(mesh.terrainVertices.size());
        const uint32_t stride=static_cast<uint32_t>(xSegments+1);
        for(int zi=0;zi<=zSegments;++zi) {
            const float worldZ=z0+(z1-z0)*static_cast<float>(zi)/zSegments;
            for(int xi=0;xi<=xSegments;++xi) {
                const float worldX=x0+(x1-x0)*static_cast<float>(xi)/xSegments;
                const float u=static_cast<float>(xi)/xSegments;
                const float v=static_cast<float>(zi)/zSegments;
                const bool boundary=(xi==0&&clampWest)||(xi==xSegments&&clampEast)||
                                    (zi==0&&clampSouth)||(zi==zSegments&&clampNorth);
                // A retained neighbour sees this edge as one straight coarse
                // triangle edge.  Boundary vertices must lie on that exact
                // segment; only replacement interiors sample the analytic bed.
                const float coarseY=(1-u)*(1-v)*cell[0].y+u*(1-v)*cell[1].y+
                                    (1-u)*v*cell[2].y+u*v*cell[3].y;
                const float y=boundary?coarseY:terrainHeight(worldX,worldZ);
                const Vec3 normal=terrainNormal(worldX,worldZ);
                const PersistentWaterSample water=persistentWater(worldX,worldZ);
                const float wet=water.inside?clamp(water.depth/2.4f,0.0f,1.0f):0.0f;
                const Vec3 colour=lerp(Vec3{.17f,.25f,.12f},
                                       Vec3{.115f,.105f,.080f},wet);
                mesh.terrainVertices.push_back({{worldX,y,worldZ},normal,
                    packColor(colour.x,colour.y,colour.z),2.0f,worldX*.08f,worldZ*.08f});
                mesh.minimumHeight=std::min(mesh.minimumHeight,y);
                mesh.maximumHeight=std::max(mesh.maximumHeight,y);
            }
        }
        for(int zi=0;zi<zSegments;++zi)
            for(int xi=0;xi<xSegments;++xi) {
                const uint32_t a=base+static_cast<uint32_t>(zi)*stride+xi;
                const uint32_t b=a+1,c=a+stride,d=c+1;
                mesh.terrainIndices.insert(mesh.terrainIndices.end(),{a,c,d,a,d,b});
            }
    }

    // The water strip is an analytic plane across each section.  Its final
    // metre is hidden beneath the immediately rising bank, so the opaque DXR
    // geometry has no unsupported slab edge.  No per-vertex terrain clamp is
    // permitted: that old correction made adjacent triangles different
    // planes, exposing the tessellation in reflected light.
    const uint32_t riverColor=packColor(.075f,.19f,.235f);
    const auto appendStrip=[&](int longitudinalSegments,int crossSegments,
                               auto crossSection) {
        const uint32_t base=static_cast<uint32_t>(mesh.riverVertices.size());
        const uint32_t stride=static_cast<uint32_t>(crossSegments+1);
        for(int segment=0;segment<=longitudinalSegments;++segment) {
            const auto section=crossSection(static_cast<float>(segment)/longitudinalSegments);
            const Vec3 lateral=normalize(section[2]);
            Vec3 normal=normalize(cross(section[1],lateral));
            if(normal.y<0)normal=normal*-1.0f;
            for(int across=0;across<=crossSegments;++across) {
                const float lane=static_cast<float>(across)/crossSegments;
                const float offset=(lane*2.0f-1.0f)*section[3].x;
                // Shader U describes the true hydraulic cross-section.  The
                // extra metre of hidden overlap therefore clamps to the edge
                // instead of making the visible shoreline appear deep.
                const float u=clamp(.5f+.5f*offset/
                    std::max(section[3].z,.001f),0.0f,1.0f);
                const Vec3 position=section[0]+lateral*offset;
                mesh.riverVertices.push_back({position,normal,riverColor,
                                               6.0f,u,section[3].y});
            }
        }

        for(int segment=0;segment<longitudinalSegments;++segment)
            for(int across=0;across<crossSegments;++across) {
                const uint32_t a=base+static_cast<uint32_t>(segment)*stride+across;
                const uint32_t b=a+1,c=a+stride,d=c+1;
                mesh.riverIndices.insert(mesh.riverIndices.end(),{a,c,d,a,d,b});
            }
    };

    // Four-metre longitudinal rows and a dozen lanes keep silhouettes and
    // specular highlights stable at first-person viewing distance.  The old
    // ~30 m rows visibly stepped and aliased as a repeated texture.
    constexpr int mainSegments=1600,mainCrossSegments=12;
    appendStrip(mainSegments,mainCrossSegments,[&](float t) {
        const float z=-terrainHalfExtent+t*(terrainHalfExtent*2.0f);
        constexpr float derivativeStep=1.0f;
        const float dx=(riverCenterX(z+derivativeStep)-riverCenterX(z-derivativeStep))*.5f;
        const float dy=(riverSurfaceHeight(z+derivativeStep)-
                        riverSurfaceHeight(z-derivativeStep))*.5f;
        const Vec3 tangent{dx,dy,1.0f};
        // Across-river positions stay at this exact Z.  That makes
        // abs(x-riverCenterX(z)) identical to the terrain carve's coordinate.
        const Vec3 lateral{1.0f,0.0f,0.0f};
        return std::array<Vec3,4>{{
            {riverCenterX(z),riverSurfaceHeight(z)+waterSurfaceLift,z},tangent,lateral,
            {riverWaterHalfWidth(z)+hiddenWaterOverlap,t,riverWaterHalfWidth(z)}
        }};
    });

    constexpr int tributarySegments=880,tributaryCrossSegments=10;
    constexpr float tributaryWaterStartX=-terrainHalfExtent;
    const float joinX=riverCenterX(tributaryConfluenceZ);
    const float tributaryJoinShoreX=
        joinX-riverWaterHalfWidth(tributaryConfluenceZ);
    const float tributaryWaterEndX=tributaryJoinShoreX+hiddenWaterOverlap;
    appendStrip(tributarySegments,tributaryCrossSegments,[&](float t) {
        // Begin only after the authored tributary activation ramp is fully
        // carved.  The former strip started in its half-strength valley and
        // had to be lifted more than sixteen metres above solid ground.
        const float x=tributaryWaterStartX+t*(tributaryWaterEndX-tributaryWaterStartX);
        constexpr float derivativeStep=1.0f;
        const float dz=(tributaryCenterZ(x+derivativeStep)-
                        tributaryCenterZ(x-derivativeStep))*.5f;
        const float dy=(tributarySurfaceHeight(x+derivativeStep)-
                        tributarySurfaceHeight(x-derivativeStep))*.5f;
        const Vec3 tangent{1.0f,dy,dz};
        // Likewise the tributary crosses at fixed X, sharing
        // abs(z-tributaryCenterZ(x)) with its authored bank.
        const Vec3 lateral{0.0f,0.0f,-1.0f};
        const float mainIntrusion=smoothStep(
            tributaryJoinShoreX,tributaryWaterEndX,x);
        return std::array<Vec3,4>{{
            {x,tributarySurfaceHeight(x)+waterSurfaceLift-.025f*mainIntrusion,
             tributaryCenterZ(x)},
            tangent,lateral,{tributaryWaterHalfWidth(x)+hiddenWaterOverlap,t,
                             tributaryWaterHalfWidth(x)}
        }};
    });

    // Sample the exact emitted terrain triangle rather than bilinearly
    // filtering across its alternating diagonals.  Grass and the DXR terrain
    // shader therefore agree at puddle boundaries.
    const auto sampleWaterRetention=[&](float worldX,float worldZ) {
        constexpr int centre=(resolution-1)/2;
        const auto gridPosition=[&](float world) {
            const float normalized=clamp(world/terrainHalfExtent,-1.0f,1.0f);
            const float uniform=std::copysign(
                std::pow(std::abs(normalized),1.0f/terrainGridExponent),normalized);
            return centre+uniform*centre;
        };
        const float gridX=gridPosition(worldX),gridZ=gridPosition(worldZ);
        const int cellX=std::clamp(static_cast<int>(std::floor(gridX)),0,resolution-2);
        const int cellZ=std::clamp(static_cast<int>(std::floor(gridZ)),0,resolution-2);
        const float x0=gridCoordinates[cellX],x1=gridCoordinates[cellX+1];
        const float z0=gridCoordinates[cellZ],z1=gridCoordinates[cellZ+1];
        const float u=clamp((worldX-x0)/(x1-x0),0.0f,1.0f);
        const float v=clamp((worldZ-z0)/(z1-z0),0.0f,1.0f);
        const float a=waterRetention[gridIndex(cellX,cellZ)];
        const float b=waterRetention[gridIndex(cellX+1,cellZ)];
        const float c=waterRetention[gridIndex(cellX,cellZ+1)];
        const float d=waterRetention[gridIndex(cellX+1,cellZ+1)];
        if(((cellX+cellZ)&1)==0) {
            if(v>=u)return a*(1-v)+c*(v-u)+d*u;
            return a*(1-u)+d*v+b*(u-v);
        }
        if(u+v<=1.0f)return a*(1-u-v)+c*v+b*u;
        return b*(1-v)+c*(1-u)+d*(u+v-1);
    };

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
        const float rootGrassSuitability=1.0f-rootLoamWeight(x,z);
        if(radius>grassHalfExtent-.25f||rootGrassSuitability<=0)continue;
        const MeadowColourFields colour=meadowColourFields(x,z);

        float islandStrength=0;
        for(const auto& island:islands) {
            const float dx=(x-island.x)/island.rx,dz=(z-island.z)/island.rz;
            const float q=std::sqrt(dx*dx+dz*dz);
            const float ragged=1.0f+.13f*std::sin(dx*4.1f+dz*2.7f+island.phase)
                                    +.07f*std::sin(dx*8.3f-dz*5.2f-island.phase);
            islandStrength=std::max(islandStrength,clamp((ragged-q)*1.7f,0.0f,1.0f));
        }
        // A mown lawn is overwhelmingly short.  Existing island fields now
        // localize the occasional coarse stem instead of creating a second,
        // knee-high meadow canopy.
        const bool coarse=grassRng.unit()<
            coarseTurfProbability(colour,islandStrength);
        const float canopyShade=1.0f-.26f*(1.0f-smoothStep(5.0f,11.0f,radius));
        const float density=canopyShade*mownTurfDensity(colour)*
                            rootGrassSuitability;
        if(grassRng.unit()>density)continue;

        const float baseY=terrainHeight(x,z)+.006f;
        const Vec3 normal=terrainNormal(x,z);
        const float moisture=clamp(.58f+(colour.fertility-.5f)*.34f+
                                   grassRng.range(-.018f,.018f),0,1);
        const MownTurfPopulation population=makeMownTurfPopulation(grassRng,coarse);
        const float maximumHeight=population.maximumHeight*2.5f;
        const float slope=std::sqrt(normal.x*normal.x+normal.z*normal.z)/
                          std::max(normal.y,.25f);
        const float lateralRatio=coarse?.66f:.52f;
        const float maximumWidth=coarse?.043f:.018f;
        constexpr float grassBoundsSafety=.012f;
        const float horizontalReach=.245f+maximumHeight*(slope+lateralRatio)+
                                    maximumWidth+grassBoundsSafety;
        const float surfaceRise=.245f*slope;
        const float lowerReach=surfaceRise+maximumWidth*slope+grassBoundsSafety;
        const float upperReach=surfaceRise+maximumHeight*(normal.y+lateralRatio*slope)+
                               maximumWidth*slope+grassBoundsSafety;
        const uint32_t retentionByte=static_cast<uint32_t>(clamp(
            sampleWaterRetention(x,z)*255.0f+0.5f,0.0f,255.0f));
        const uint32_t packedSeed=(grassRng.next()&0x00ffffffu)|(retentionByte<<24);
        mesh.grassPatches.push_back({x-horizontalReach,baseY-lowerReach,
                                     z-horizontalReach,x+horizontalReach,
                                     baseY+upperReach,z+horizontalReach,
                                     packedSeed,population.packed,baseY,normal.x,normal.z,moisture,
                                     colour.fertility,colour.dryColony,colour.lushColony,
                                     colour.warmCool});
        if(coarse)++mesh.tallGrassPatchCount;
    }

    Rng detailRng(seed^0xc8013ea4u);
    // Rock families follow exposed geology.  Three modest foreground groups
    // establish material scale; larger fractured groups then pick mineral or
    // short-upland sites, especially along the authored western escarpment.
    // The hero trunk and both water corridors remain physically clear.
    constexpr int rockGroupTarget=10;
    for(int group=0;group<rockGroupTarget;++group) {
        Vec3 groupCenter{};bool foundCenter=false;
        for(int attempt=0;attempt<420&&!foundCenter;++attempt) {
            float x=0,z=0;
            if(group<3) {
                const float angle=detailRng.range(0,2*pi);
                const float distance=detailRng.range(10.0f,47.0f);
                x=std::cos(angle)*distance;z=std::sin(angle)*distance;
            } else if(group<8) {
                x=detailRng.range(-1510.0f,-1120.0f);
                z=detailRng.range(-1080.0f,1180.0f);
            } else {
                x=detailRng.range(-1450.0f,1450.0f);
                z=detailRng.range(-1250.0f,1350.0f);
            }
            const float radius=std::sqrt(x*x+z*z);
            if(radius<6.0f)continue;
            const PopulationSite site=populationSite(x,z);
            const float mineral=site.biome.material[static_cast<size_t>(
                GroundMaterialTile::ExposedRockSoil)];
            const float upland=site.biome.material[static_cast<size_t>(
                GroundMaterialTile::UplandShortTurf)];
            const float suitability=group<3?.72f:clamp(.05f+mineral+upland*.34f,0.0f,1.0f);
            if(!site.channelClear||site.normal.y<(group<3?.78f:.44f)||
               detailRng.unit()>suitability)continue;
            groupCenter={x,site.height,z};foundCenter=true;
        }
        if(!foundCenter)continue;

        const int members=group<3?5+static_cast<int>(detailRng.next()%4u):
                                    6+static_cast<int>(detailRng.next()%4u);
        const float familySpread=group<3?detailRng.range(2.2f,5.2f):
                                         detailRng.range(5.5f,13.0f);
        for(int member=0;member<members;++member) {
            const float angle=detailRng.range(0,2*pi);
            const float spread=familySpread*std::sqrt(detailRng.unit());
            const float x=groupCenter.x+std::cos(angle)*spread;
            const float z=groupCenter.z+std::sin(angle)*spread;
            const float radius=std::sqrt(x*x+z*z);const PopulationSite site=populationSite(x,z);
            if(radius<6.0f||!site.channelClear||site.normal.y<(group<3?.74f:.40f))continue;
            const uint32_t rockRoll=detailRng.next()%7u;
            const int type=(group>=3&&rockRoll>=4u)?2:static_cast<int>(rockRoll&1u);
            const float scale=group<3?
                (type==2?detailRng.range(.48f,1.05f):detailRng.range(.15f,.58f)):
                (type==2?detailRng.range(.85f,2.15f):detailRng.range(.38f,1.25f));
            Vec3 radii{scale*detailRng.range(.78f,1.42f),
                       scale*(type==1?detailRng.range(.20f,.43f):
                                      detailRng.range(.47f,.88f)),
                       scale*detailRng.range(.66f,1.28f)};
            appendRock(mesh,{x,site.height,z},radii,detailRng.range(0,2*pi),
                       detailRng.next(),type);
            ++mesh.rockCount;
        }
    }

    // The remaining dressing is assembled as unequal habitat groves instead
    // of radial bands.  Type 0 repeats the hero oak's low, spreading scaffold
    // with a much cheaper crown representation; its real-world dimensions do
    // not shrink at distance.  Banks support damp broadleaf pockets, while an
    // exposed western grove admits a small secondary-species component.
    enum class GroveHabitat { Meadow,Riparian,Upland };
    struct Grove { float x,z,rx,rz;int target;GroveHabitat habitat; };
    const float southRiverZ=-820.0f,northRiverZ=790.0f,tributaryX=-930.0f;
    const std::array<Grove,7> groves{{
        {-510.0f,-520.0f,92.0f,68.0f,3,GroveHabitat::Meadow},
        {735.0f,-430.0f,108.0f,72.0f,3,GroveHabitat::Meadow},
        {riverCenterX(southRiverZ)-riverHalfWidth(southRiverZ)-82.0f,southRiverZ,
             62.0f,96.0f,3,GroveHabitat::Riparian},
        {riverCenterX(northRiverZ)+riverHalfWidth(northRiverZ)+88.0f,northRiverZ,
             70.0f,105.0f,3,GroveHabitat::Riparian},
        {tributaryX,tributaryCenterZ(tributaryX)-tributaryHalfWidth(tributaryX)-62.0f,
             112.0f,55.0f,3,GroveHabitat::Riparian},
        {-1260.0f,160.0f,105.0f,155.0f,3,GroveHabitat::Upland},
        {1010.0f,1010.0f,135.0f,105.0f,3,GroveHabitat::Meadow}
    }};
    struct TreeCandidate { Vec3 base;GroveHabitat habitat;uint32_t seed; };
    std::vector<TreeCandidate> treeCandidates;
    treeCandidates.reserve(21);
    auto treeCrowded=[&](float x,float z,float spacing) {
        for(const auto& existing:treeCandidates) {
            const float dx=x-existing.base.x,dz=z-existing.base.z;
            if(dx*dx+dz*dz<spacing*spacing)return true;
        }
        return false;
    };
    for(const Grove& grove:groves) {
        int placed=0,attempts=0;
        while(placed<grove.target&&attempts++<520) {
            const float angle=detailRng.range(0,2*pi);
            const float radial=std::sqrt(detailRng.unit());
            const float x=grove.x+std::cos(angle)*grove.rx*radial;
            const float z=grove.z+std::sin(angle)*grove.rz*radial;
            const float heroRadius=std::sqrt(x*x+z*z);
            if(heroRadius<90.0f||treeCrowded(x,z,29.0f))continue;
            const PopulationSite site=populationSite(x,z);
            if(!site.channelClear||site.normal.y<.61f||site.height>125.0f)continue;
            const float mineral=site.biome.material[static_cast<size_t>(
                GroundMaterialTile::ExposedRockSoil)];
            const float upland=site.biome.material[static_cast<size_t>(
                GroundMaterialTile::UplandShortTurf)];
            if(grove.habitat==GroveHabitat::Riparian&&site.floodplainInfluence<.30f)continue;
            if(grove.habitat==GroveHabitat::Upland&&upland+mineral<.10f)continue;
            // Low-frequency canopy opportunities leave deliberate open gaps
            // inside a grove instead of producing plantation-like spacing.
            const float opportunity=.46f+.38f*mapFbm(x*.0041f+51.0f,z*.0041f-29.0f);
            if(detailRng.unit()>opportunity)continue;
            treeCandidates.push_back({{x,site.height,z},grove.habitat,detailRng.next()});
            ++placed;
        }
    }
    // A permissive deterministic fallback protects the proxy inventory if a
    // future terrain seed turns one authored grove into a steep or wet site.
    for(int attempts=0;treeCandidates.size()<21&&attempts<1600;++attempts) {
        const float x=detailRng.range(-1420.0f,1420.0f);
        const float z=detailRng.range(-1280.0f,1380.0f);
        if(std::sqrt(x*x+z*z)<150.0f||treeCrowded(x,z,34.0f))continue;
        const PopulationSite site=populationSite(x,z);
        if(!site.channelClear||site.normal.y<.68f||site.height>92.0f)continue;
        const GroveHabitat habitat=site.floodplainInfluence>.34f?
            GroveHabitat::Riparian:GroveHabitat::Meadow;
        treeCandidates.push_back({{x,site.height,z},habitat,detailRng.next()});
    }
    std::sort(treeCandidates.begin(),treeCandidates.end(),[](const auto& a,const auto& b) {
        return a.base.x*a.base.x+a.base.z*a.base.z<b.base.x*b.base.x+b.base.z*b.base.z;
    });
    for(size_t index=0;index<treeCandidates.size();++index) {
        const TreeCandidate& candidate=treeCandidates[index];
        Rng objectRng(candidate.seed^0x9e3779b9u);
        const uint32_t typeRoll=objectRng.next()%100u;
        int type=0;
        if(candidate.habitat==GroveHabitat::Upland)
            type=typeRoll<67u?0:(typeRoll<88u?2:1);
        else if(candidate.habitat==GroveHabitat::Riparian)
            type=typeRoll<76u?0:2;
        else type=typeRoll<84u?0:(typeRoll<97u?2:1);
        const float treeHeight=type==0?objectRng.range(13.5f,20.0f):
                               (type==1?objectRng.range(15.0f,22.0f):
                                        objectRng.range(10.5f,16.5f));
        const float crownRadius=treeHeight*(type==0?objectRng.range(.48f,.62f):
                                  (type==1?objectRng.range(.20f,.28f):
                                           objectRng.range(.38f,.52f)));
        const int detail=index<4?2:(index<10?1:0);
        appendProxyTree(mesh,candidate.base,treeHeight,crownRadius,type,detail,objectRng);
        ++mesh.backgroundTreeCount;
    }

    // The landmark groves above intentionally stay sparse and readable near
    // the oak.  A separate map-scale LOD establishes wooded river corridors
    // and lower foothills.  Authored, unequal clusters follow landscape axes;
    // they are not a radial ring around the camera and leave the central
    // meadow open.  Cluster RNG and object RNG are independent from detailRng
    // so changing this inventory cannot reshuffle rocks or shrubs.
    struct DistantCluster {
        float x,z,rx,rz,rotation;
        int target;
        GroveHabitat habitat;
    };
    const auto mainBankX=[&](float z,float side,float offset) {
        return riverCenterX(z)+side*(riverHalfWidth(z)+offset);
    };
    const auto tributaryBankZ=[&](float x,float side,float offset) {
        return tributaryCenterZ(x)+side*(tributaryHalfWidth(x)+offset);
    };
    const std::array<DistantCluster,22> distantClusters{{
        {mainBankX(-2400,-1,132),-2400,118,205,-.12f,12,GroveHabitat::Riparian},
        {mainBankX(-2000, 1,145),-2000,145,185, .18f,13,GroveHabitat::Riparian},
        {mainBankX(-1460,-1,150),-1460,132,190,-.20f,12,GroveHabitat::Riparian},
        {mainBankX(-1050, 1,155),-1050,148,178, .16f,12,GroveHabitat::Riparian},
        {mainBankX( 1050,-1,145), 1050,138,190,-.14f,13,GroveHabitat::Riparian},
        {mainBankX( 1450, 1,158), 1450,145,205, .12f,13,GroveHabitat::Riparian},
        {mainBankX( 2000,-1,148), 2000,135,180,-.18f,12,GroveHabitat::Riparian},
        {mainBankX( 2380, 1,138), 2380,128,165, .14f,11,GroveHabitat::Riparian},

        {-2470,tributaryBankZ(-2470,-1,86),190,105,.08f,11,GroveHabitat::Riparian},
        {-2110,tributaryBankZ(-2110, 1,92),185,112,-.06f,11,GroveHabitat::Riparian},
        {-1730,tributaryBankZ(-1730,-1,88),180,102,.10f,12,GroveHabitat::Riparian},
        {-1350,tributaryBankZ(-1350, 1,96),172,108,-.08f,11,GroveHabitat::Riparian},
        { -930,tributaryBankZ( -930,-1,90),155,96,.12f,10,GroveHabitat::Riparian},

        {-1900,1540,250,180, .24f,15,GroveHabitat::Upland},
        {-1260,1840,270,170,-.12f,15,GroveHabitat::Upland},
        { -500,1990,250,155, .18f,14,GroveHabitat::Upland},
        {  300,1940,235,165,-.16f,13,GroveHabitat::Upland},
        { 1080,1830,275,175, .22f,15,GroveHabitat::Upland},
        { 1780,1550,250,170,-.20f,13,GroveHabitat::Upland},
        { 2290,1150,215,155, .16f,10,GroveHabitat::Upland},
        {-2020,-650,175,245,-.28f, 6,GroveHabitat::Upland},
        {-2140, 120,185,225, .31f, 6,GroveHabitat::Upland}
    }};
    struct DistantTreeCandidate {
        Vec3 base;
        Vec3 normal;
        GroveHabitat habitat;
        uint32_t seed;
    };
    constexpr size_t distantTreeTarget=260;
    std::vector<DistantTreeCandidate> distantTrees;
    distantTrees.reserve(distantTreeTarget);
    auto distantCrowded=[&](float x,float z,float spacing) {
        for(const auto& existing:distantTrees) {
            const float dx=x-existing.base.x,dz=z-existing.base.z;
            if(dx*dx+dz*dz<spacing*spacing)return true;
        }
        for(const auto& existing:treeCandidates) {
            const float dx=x-existing.base.x,dz=z-existing.base.z;
            if(dx*dx+dz*dz<spacing*spacing*2.25f)return true;
        }
        return false;
    };
    const auto acceptDistantTree=[&](float x,float z,GroveHabitat habitat,
                                     uint32_t objectSeed) {
        const float radius=std::sqrt(x*x+z*z);
        if(radius<620.0f||std::abs(x)>2990.0f||std::abs(z)>2990.0f||
           distantCrowded(x,z,17.5f))return false;
        const PopulationSite site=populationSite(x,z);
        const TerrainSurfaceSample grade=sampleTerrainSurface(x,z);
        if(!grade.insideBounds||!site.channelClear||grade.normal.y<.57f||site.height>175.0f)
            return false;
        const float upland=site.biome.material[static_cast<size_t>(
            GroundMaterialTile::UplandShortTurf)];
        const float mineral=site.biome.material[static_cast<size_t>(
            GroundMaterialTile::ExposedRockSoil)];
        if(habitat==GroveHabitat::Riparian) {
            if(site.floodplainInfluence<.16f||site.riverBankDistance>285.0f)return false;
        } else if(site.height<3.0f&&upland+mineral<.12f) return false;
        distantTrees.push_back({{x,grade.position.y,z},grade.normal,habitat,objectSeed});
        return true;
    };
    for(size_t clusterIndex=0;clusterIndex<distantClusters.size();++clusterIndex) {
        const DistantCluster& cluster=distantClusters[clusterIndex];
        Rng clusterRng(seed^0xd1b54a35u^
                       static_cast<uint32_t>(clusterIndex*0x9e3779b9u));
        int placed=0,attempts=0;
        while(placed<cluster.target&&attempts++<1800) {
            const float localX=clusterRng.range(-1.0f,1.0f);
            const float localZ=clusterRng.range(-1.0f,1.0f);
            const float q=localX*localX+localZ*localZ;
            if(q>1.0f)continue;
            const float c=std::cos(cluster.rotation),s=std::sin(cluster.rotation);
            const float warpedX=localX*cluster.rx+
                std::sin(localZ*4.2f+clusterIndex*.71f)*cluster.rx*.08f;
            const float warpedZ=localZ*cluster.rz+
                std::sin(localX*3.1f-clusterIndex*.53f)*cluster.rz*.07f;
            const float x=cluster.x+warpedX*c-warpedZ*s;
            const float z=cluster.z+warpedX*s+warpedZ*c;
            // Coherent recruitment gaps split each woodland into copses and
            // irregular windows instead of filling the ellipse uniformly.
            const float opportunity=.33f+.67f*mapFbm(x*.0051f+clusterIndex*3.7f,
                                                      z*.0051f-clusterIndex*2.1f);
            if(clusterRng.unit()>opportunity)continue;
            const uint32_t objectSeed=clusterRng.next();
            if(acceptDistantTree(x,z,cluster.habitat,objectSeed))++placed;
        }
    }
    // Terrain edits should not silently erase the forest contract.  Fill any
    // rejected cluster slots from the same riparian/foothill habitats, never
    // from the protected central meadow.
    Rng distantFallback(seed^0xa24baed5u);
    for(int attempts=0;distantTrees.size()<distantTreeTarget&&attempts<24000;++attempts) {
        const bool riparian=(attempts&1)==0;
        float x,z;
        GroveHabitat habitat;
        if(riparian) {
            z=distantFallback.range(-2720.0f,2720.0f);
            const float side=(distantFallback.next()&1u)?1.0f:-1.0f;
            x=mainBankX(z,side,distantFallback.range(70.0f,245.0f));
            habitat=GroveHabitat::Riparian;
        } else {
            x=distantFallback.range(-2450.0f,2450.0f);
            z=distantFallback.range(1380.0f,2150.0f);
            habitat=GroveHabitat::Upland;
        }
        acceptDistantTree(x,z,habitat,distantFallback.next());
    }
    std::sort(distantTrees.begin(),distantTrees.end(),[](const auto& a,const auto& b) {
        if(a.base.z!=b.base.z)return a.base.z<b.base.z;
        return a.base.x<b.base.x;
    });
    const size_t distantTriangleStart=mesh.detailIndices.size()/3;
    mesh.distantTreeBases.reserve(distantTrees.size());
    for(const DistantTreeCandidate& candidate:distantTrees) {
        Rng objectRng(candidate.seed^0x94d049bbu);
        const uint32_t typeRoll=objectRng.next()%100u;
        const int type=candidate.habitat==GroveHabitat::Riparian?
            (typeRoll<74u?0:(typeRoll<91u?1:2)):(typeRoll<87u?0:2);
        const float treeHeight=type==0?objectRng.range(12.0f,19.5f):
                               (type==1?objectRng.range(11.5f,18.5f):
                                        objectRng.range(8.5f,14.5f));
        const float crownRadius=treeHeight*(type==0?objectRng.range(.49f,.68f):
                                  (type==1?objectRng.range(.31f,.45f):
                                           objectRng.range(.43f,.60f)));
        appendFarTree(mesh,candidate.base,candidate.normal,treeHeight,crownRadius,type,
                      objectRng);
        mesh.distantTreeBases.push_back(candidate.base);
    }
    mesh.distantTreeCount=static_cast<uint32_t>(distantTrees.size());
    mesh.distantTreeTriangleCount=static_cast<uint32_t>(mesh.detailIndices.size()/3-
                                                        distantTriangleStart);

    // Riparian shrubs grow in broken colonies just beyond the bank, not as a
    // global scatter.  Overlapping foliage along each multi-stem axis makes a
    // distant hedge read as a porous plant rather than a row of green spheres.
    struct ShrubColony { float x,z,rx,rz;int target; };
    const float shrubSouthZ=-470.0f,shrubNorthZ=420.0f,tributaryShrubX=-1180.0f;
    const std::array<ShrubColony,5> shrubColonies{{
        {riverCenterX(shrubSouthZ)-riverHalfWidth(shrubSouthZ)-54.0f,shrubSouthZ,
             46.0f,80.0f,4},
        {riverCenterX(shrubNorthZ)+riverHalfWidth(shrubNorthZ)+58.0f,shrubNorthZ,
             48.0f,74.0f,4},
        {tributaryShrubX,tributaryCenterZ(tributaryShrubX)+
             tributaryHalfWidth(tributaryShrubX)+43.0f,78.0f,40.0f,4},
        {riverCenterX(1080.0f)-riverHalfWidth(1080.0f)-68.0f,1080.0f,
             58.0f,90.0f,3},
        {-660.0f,tributaryCenterZ(-660.0f)-tributaryHalfWidth(-660.0f)-48.0f,
             67.0f,40.0f,3}
    }};
    std::vector<Vec3> acceptedShrubs;acceptedShrubs.reserve(18);
    for(const ShrubColony& colony:shrubColonies) {
        int placed=0,attempts=0;
        while(placed<colony.target&&attempts++<460) {
            const float angle=detailRng.range(0,2*pi),radial=std::sqrt(detailRng.unit());
            const float x=colony.x+std::cos(angle)*colony.rx*radial;
            const float z=colony.z+std::sin(angle)*colony.rz*radial;
            if(std::sqrt(x*x+z*z)<90.0f)continue;
            const PopulationSite site=populationSite(x,z);
            if(!site.channelClear||site.normal.y<.70f||site.floodplainInfluence<.28f)continue;
            bool crowded=false;
            for(const Vec3& existing:acceptedShrubs) {
                const float dx=x-existing.x,dz=z-existing.z;
                if(dx*dx+dz*dz<12.0f){crowded=true;break;}
            }
            if(crowded)continue;
            for(const auto& tree:treeCandidates) {
                const float dx=x-tree.base.x,dz=z-tree.base.z;
                if(dx*dx+dz*dz<30.0f){crowded=true;break;}
            }
            if(crowded)continue;
            const uint32_t objectSeed=detailRng.next();Rng objectRng(objectSeed^0x85ebca6bu);
            const int variant=site.riverBankDistance<32.0f?1:
                              (objectRng.next()%5u==0u?2:0);
            appendProxyBush(mesh,{x,site.height,z},objectRng.range(.75f,1.65f),
                            objectRng.range(.72f,1.58f),variant,objectRng);
            acceptedShrubs.push_back({x,site.height,z});
            ++placed;++mesh.shrubCount;
        }
    }
    return mesh;
}

} // namespace dense
