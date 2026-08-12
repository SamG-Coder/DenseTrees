#include "ground_texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace dense {
namespace {

float saturate(float value) {
    return std::clamp(value,0.0f,1.0f);
}

float mix(float a,float b,float amount) {
    return a+(b-a)*amount;
}

float smoothStep(float low,float high,float value) {
    if(high<=low)return value>=high?1.0f:0.0f;
    const float t=saturate((value-low)/(high-low));
    return t*t*(3.0f-2.0f*t);
}

float fract(float value) {
    return value-std::floor(value);
}

uint32_t mixBits(uint32_t value) {
    value^=value>>16;
    value*=0x7feb352du;
    value^=value>>15;
    value*=0x846ca68bu;
    value^=value>>16;
    return value;
}

int wrapCell(int value,int period) {
    const int result=value%period;
    return result<0?result+period:result;
}

uint32_t latticeHash(int x,int y,uint32_t salt) {
    uint32_t value=static_cast<uint32_t>(x)*0x8da6b343u;
    value^=static_cast<uint32_t>(y)*0xd8163841u;
    value^=salt*0x9e3779b9u+0x68bc21ebu;
    return mixBits(value);
}

float unitFloat(uint32_t value) {
    return static_cast<float>(value>>8)*(1.0f/16777216.0f);
}

float periodicValueNoise(float u,float v,int cells,uint32_t salt) {
    const float gx=u*static_cast<float>(cells),gy=v*static_cast<float>(cells);
    const int ix=static_cast<int>(std::floor(gx)),iy=static_cast<int>(std::floor(gy));
    float fx=gx-std::floor(gx),fy=gy-std::floor(gy);
    fx=fx*fx*(3.0f-2.0f*fx);fy=fy*fy*(3.0f-2.0f*fy);
    const auto sample=[&](int x,int y) {
        return unitFloat(latticeHash(wrapCell(x,cells),wrapCell(y,cells),salt));
    };
    const float a=sample(ix,iy),b=sample(ix+1,iy);
    const float c=sample(ix,iy+1),d=sample(ix+1,iy+1);
    return mix(mix(a,b,fx),mix(c,d,fx),fy);
}

float periodicFbm(float u,float v,int baseCells,int octaves,uint32_t salt) {
    float result=0.0f,totalWeight=0.0f,weight=.56f;
    int cells=baseCells;
    for(int octave=0;octave<octaves;++octave) {
        result+=periodicValueNoise(u,v,cells,salt+static_cast<uint32_t>(octave)*0x9e3779b9u)*weight;
        totalWeight+=weight;weight*=.48f;cells*=2;
    }
    return result/std::max(totalWeight,1.0e-6f);
}

struct CellularSample {
    float nearest{};
    float edge{};
    float localX{};
    float localY{};
    float identity{};
};

CellularSample periodicCellular(float u,float v,int cells,uint32_t salt) {
    const float gx=u*cells,gy=v*cells;
    const int ix=static_cast<int>(std::floor(gx)),iy=static_cast<int>(std::floor(gy));
    float nearestSq=1.0e9f,secondSq=1.0e9f,bestX=0,bestY=0,bestIdentity=0;
    for(int oy=-1;oy<=1;++oy)for(int ox=-1;ox<=1;++ox) {
        const int cellX=ix+ox,cellY=iy+oy;
        const int wrappedX=wrapCell(cellX,cells),wrappedY=wrapCell(cellY,cells);
        const uint32_t h=latticeHash(wrappedX,wrappedY,salt);
        const float jitterX=.13f+.74f*unitFloat(h);
        const float jitterY=.13f+.74f*unitFloat(mixBits(h^0xa511e9b3u));
        const float dx=static_cast<float>(cellX)+jitterX-gx;
        const float dy=static_cast<float>(cellY)+jitterY-gy;
        const float distanceSq=dx*dx+dy*dy;
        if(distanceSq<nearestSq) {
            secondSq=nearestSq;nearestSq=distanceSq;
            bestX=dx;bestY=dy;bestIdentity=unitFloat(mixBits(h^0x63d83595u));
        } else if(distanceSq<secondSq) {
            secondSq=distanceSq;
        }
    }
    return {std::sqrt(nearestSq),std::sqrt(secondSq)-std::sqrt(nearestSq),
            bestX,bestY,bestIdentity};
}

float periodicStripe(float u,float v,int frequencyU,int frequencyV,float phase,
                     float halfWidth,float softness) {
    const float coordinate=fract(u*frequencyU+v*frequencyV+phase);
    const float distance=std::abs(coordinate-.5f);
    return 1.0f-smoothStep(halfWidth,halfWidth+softness,distance);
}

struct MaterialSample {
    float red{},green{},blue{},roughness{},height{},cavity{};
};

// A cut stem occupies only its own cellular neighbourhood.  Unlike a global
// periodic stripe it cannot continue for metres across adjacent texels, so it
// reads as scattered thatch rather than a woven or brushed normal pattern.
float cellularStemFragment(const CellularSample& stem,float length,float width,
                           float presenceThreshold) {
    const float angle=stem.identity*6.28318530718f;
    const float cosine=std::cos(angle),sine=std::sin(angle);
    const float along=stem.localX*cosine+stem.localY*sine;
    const float across=-stem.localX*sine+stem.localY*cosine;
    const float longitudinal=1-smoothStep(length*.58f,length,std::abs(along));
    const float lateral=1-smoothStep(width*.45f,width,std::abs(across));
    return longitudinal*lateral*smoothStep(presenceThreshold,
                                            presenceThreshold+.18f,
                                            stem.identity);
}

MaterialSample denseTurf(float u,float v,uint32_t seed) {
    const float macro=periodicFbm(u,v,4,5,seed^0x71b52a91u);
    const float fine=periodicFbm(u,v,32,4,seed^0x36d17ab5u);
    // A mown lawn is not one uniformly bright green mat.  Fertility and
    // drying form overlapping colonies at roughly 15--50 cm, while the cut
    // leaf litter remains a separate centimetre-scale layer below them.
    const float oliveField=periodicFbm(u,v,7,4,seed^0x6c8e9cf5u);
    const float feltField=periodicFbm(u,v,18,3,seed^0xb74f0a31u);
    const CellularSample mat=periodicCellular(u,v,37,seed^0xad90777du);
    const CellularSample cutA=periodicCellular(u,v,103,seed^0x3142c7a1u);
    const CellularSample cutB=periodicCellular(u,v,71,seed^0xf0ad239bu);
    const float turfClump=(1-smoothStep(.12f,.48f,mat.nearest))*(.72f+.28f*mat.identity);
    const float fragments=saturate(cellularStemFragment(cutA,.39f,.060f,.40f)+
                                   cellularStemFragment(cutB,.34f,.052f,.61f)*.70f);
    const float olive=smoothStep(.54f,.78f,oliveField+.10f*(.5f-macro));
    const float felt=smoothStep(.56f,.82f,feltField+.12f*olive);
    const float thatch=saturate(fragments*.72f+felt*olive*.24f);
    const float lush=smoothStep(.48f,.76f,macro+.08f*turfClump)*(1-.58f*olive);

    const float greenR=mix(.022f,.052f,macro)+.009f*turfClump;
    const float greenG=mix(.064f,.142f,macro)+.018f*turfClump;
    const float greenB=mix(.006f,.018f,fine)+.002f*turfClump;
    const float oliveR=mix(.066f,.098f,feltField);
    const float oliveG=mix(.088f,.120f,feltField);
    const float oliveB=mix(.010f,.022f,fine);
    MaterialSample sample;
    sample.red=mix(greenR,oliveR,olive*.52f);
    sample.green=mix(greenG,oliveG,olive*.52f);
    sample.blue=mix(greenB,oliveB,olive*.52f);
    // Cut fragments are straw-coloured rather than brighter green.  Keeping
    // the blend partial leaves a dark, living sward visible between pieces.
    sample.red=mix(sample.red,.148f,thatch*.58f);
    sample.green=mix(sample.green,.120f,thatch*.58f);
    sample.blue=mix(sample.blue,.032f,thatch*.58f);
    sample.red+=.008f*lush;sample.green+=.018f*lush;sample.blue+=.001f*lush;
    sample.roughness=saturate(.925f+.030f*felt-.035f*turfClump-
                              .025f*lush+.018f*(1-fine));
    sample.height=saturate(.465f+.070f*(macro-.5f)+.050f*(fine-.5f)+
                           .052f*turfClump+.026f*fragments-.020f*felt);
    sample.cavity=saturate(.105f+.16f*(1-fine)+.19f*(1-turfClump)*
                           smoothStep(.32f,.72f,macro)+.055f*felt-
                           .035f*fragments);
    return sample;
}

MaterialSample coarseMeadow(float u,float v,uint32_t seed) {
    const float macro=periodicFbm(u,v,2,6,seed^0xe8a72b6du);
    const float dryPatch=periodicFbm(u,v,7,4,seed^0x94c3d8f1u);
    const float felt=periodicFbm(u,v,20,3,seed^0x4bb8615du);
    const float grit=periodicFbm(u,v,47,3,seed^0x09d31efbu);
    const CellularSample tuft=periodicCellular(u,v,23,seed^0x30b4a6c9u);
    const CellularSample stone=periodicCellular(u,v,71,seed^0xc075b31du);
    const float tuftMask=(1-smoothStep(.15f,.49f,tuft.nearest))*(.60f+.40f*tuft.identity);
    const CellularSample cutA=periodicCellular(u,v,67,seed^0x4f1bbcddu);
    const CellularSample cutB=periodicCellular(u,v,49,seed^0x75dd81a3u);
    const float stems=saturate((cellularStemFragment(cutA,.43f,.055f,.34f)*.68f+
                                cellularStemFragment(cutB,.38f,.050f,.57f)*.48f)*
                               (.34f+.82f*tuftMask));
    const float exposedGrit=(1-smoothStep(.10f,.25f,stone.nearest))*
                            smoothStep(.70f,.94f,stone.identity)*smoothStep(.50f,.76f,dryPatch);
    const float dryness=smoothStep(.48f,.80f,dryPatch+.13f*(1-macro));
    const float thatch=saturate(stems*.68f+
        smoothStep(.61f,.84f,felt)*dryness*.26f);
    const float greenR=mix(.030f,.066f,macro),greenG=mix(.072f,.148f,macro),greenB=mix(.008f,.023f,grit);
    const float dryR=mix(.088f,.162f,dryPatch),dryG=mix(.088f,.146f,dryPatch),dryB=mix(.018f,.041f,dryPatch);
    MaterialSample sample;
    sample.red=mix(greenR,dryR,dryness)+.045f*exposedGrit;
    sample.green=mix(greenG,dryG,dryness)+.041f*exposedGrit;
    sample.blue=mix(greenB,dryB,dryness)+.034f*exposedGrit;
    sample.red=mix(sample.red,.155f,thatch*.46f);
    sample.green=mix(sample.green,.126f,thatch*.46f);
    sample.blue=mix(sample.blue,.034f,thatch*.46f);
    sample.roughness=saturate(.925f+.035f*dryness+.018f*felt-
                              .045f*stems-.090f*exposedGrit);
    sample.height=saturate(.46f+.085f*(macro-.5f)+.070f*(grit-.5f)+
                           .070f*tuftMask+.038f*stems+.10f*exposedGrit-
                           .018f*felt);
    sample.cavity=saturate(.13f+.24f*(1-tuftMask)+.17f*(1-stems)*
                           smoothStep(.42f,.78f,dryPatch)+.045f*felt);
    return sample;
}

MaterialSample wornSoil(float u,float v,uint32_t seed) {
    const float macro=periodicFbm(u,v,3,6,seed^0x7c2dd1a9u);
    const float granules=periodicFbm(u,v,61,4,seed^0xc1f57a3bu);
    const float moisture=periodicFbm(u,v,2,5,seed^0x21d469efu);
    const CellularSample slabs=periodicCellular(u,v,8,seed^0x991b35c7u);
    const CellularSample fragments=periodicCellular(u,v,29,seed^0x446af283u);
    const CellularSample grit=periodicCellular(u,v,89,seed^0x14e8b6ddu);
    // Broad mineral plates, with a second discontinuous fracture network.
    // Their scale is tens of centimetres; high-frequency granules stay a few
    // millimetres and therefore vanish naturally through the mip chain.
    const float fissureA=1-smoothStep(.018f,.082f,slabs.edge);
    const float fissureB=(1-smoothStep(.010f,.044f,fragments.edge))*(1-.66f*fissureA);
    const float fissures=saturate(fissureA+.48f*fissureB);
    const float slabCrown=(1-smoothStep(.18f,.52f,slabs.nearest))*
                          (.68f+.32f*slabs.identity);
    const float fragment=(1-smoothStep(.09f,.24f,fragments.nearest))*
                         smoothStep(.54f,.90f,fragments.identity);
    const float paleGrit=(1-smoothStep(.08f,.19f,grit.nearest))*
                         smoothStep(.73f,.94f,grit.identity);
    const float damp=smoothStep(.38f,.74f,moisture);
    const float dryR=mix(.125f,.220f,macro),dryG=mix(.082f,.145f,macro),dryB=mix(.046f,.085f,macro);
    const float wetR=mix(.050f,.096f,macro),wetG=mix(.038f,.069f,macro),wetB=mix(.025f,.045f,macro);
    MaterialSample sample;
    sample.red=mix(dryR,wetR,damp)+.036f*slabCrown+.050f*fragment+.080f*paleGrit;
    sample.green=mix(dryG,wetG,damp)+.033f*slabCrown+.046f*fragment+.074f*paleGrit;
    sample.blue=mix(dryB,wetB,damp)+.029f*slabCrown+.041f*fragment+.066f*paleGrit;
    sample.red=mix(sample.red,.020f,fissures*.78f);
    sample.green=mix(sample.green,.015f,fissures*.78f);
    sample.blue=mix(sample.blue,.011f,fissures*.78f);
    sample.roughness=saturate(.91f+.045f*(1-damp)-.12f*slabCrown-
                              .10f*fragment-.16f*paleGrit-.025f*granules);
    sample.height=saturate(.47f+.18f*(macro-.5f)+.09f*(granules-.5f)+
                           .21f*slabCrown+.15f*fragment+.12f*paleGrit-.30f*fissures);
    sample.cavity=saturate(.08f+.78f*fissures+.12f*(1-granules));
    return sample;
}

MaterialSample cloverMoss(float u,float v,uint32_t seed) {
    const float silt=periodicFbm(u,v,2,6,seed^0x771f2ae9u);
    const float moss=periodicFbm(u,v,7,5,seed^0xa7ef1531u);
    const float velvet=periodicFbm(u,v,52,4,seed^0x2b91c4d7u);
    const CellularSample plant=periodicCellular(u,v,23,seed^0x5a7e0b93u);
    const CellularSample gravel=periodicCellular(u,v,67,seed^0xc36947a5u);
    static constexpr std::array<float,8> directionsX{1.0f,.70710678f,0,-.70710678f,-1.0f,-.70710678f,0,.70710678f};
    static constexpr std::array<float,8> directionsY{0,.70710678f,1.0f,.70710678f,0,-.70710678f,-1.0f,-.70710678f};
    const int orientation=std::min(7,static_cast<int>(plant.identity*8.0f));
    const float pointX=-plant.localX,pointY=-plant.localY;
    float clover=0;
    for(int leaf=0;leaf<3;++leaf) {
        const int direction=(orientation+leaf*3)&7;
        const float cx=directionsX[direction]*.16f,cy=directionsY[direction]*.16f;
        const float dx=pointX-cx,dy=pointY-cy;
        clover=std::max(clover,1-smoothStep(.105f,.245f,std::sqrt(dx*dx+dy*dy)));
    }
    clover*=smoothStep(.23f,.48f,plant.identity);
    const float leafVein=clover*periodicStripe(u,v,46,-23,plant.identity,.010f,.026f);
    const float mossLight=smoothStep(.34f,.78f,moss+.10f*(silt-.5f));
    const float wetSilt=smoothStep(.52f,.82f,silt)*(1-mossLight*.62f);
    const float riverGrit=(1-smoothStep(.09f,.22f,gravel.nearest))*
                          smoothStep(.68f,.93f,gravel.identity)*(1-clover);
    MaterialSample sample;
    sample.red=mix(.018f,.052f,mossLight)+.018f*clover+.010f*leafVein+
               .050f*riverGrit-.010f*wetSilt;
    sample.green=mix(.048f,.142f,mossLight)+.040f*clover+.012f*leafVein+
                 .047f*riverGrit-.012f*wetSilt;
    sample.blue=mix(.012f,.036f,velvet)+.007f*clover+.043f*riverGrit+
                .004f*wetSilt;
    sample.roughness=saturate(.89f-.075f*mossLight-.095f*clover-
                              .16f*wetSilt-.11f*riverGrit+.018f*(1-velvet));
    sample.height=saturate(.43f+.13f*(silt-.5f)+.13f*(moss-.5f)+
                           .07f*(velvet-.5f)+.22f*clover+.050f*leafVein+
                           .14f*riverGrit-.07f*wetSilt);
    sample.cavity=saturate(.12f+.24f*(1-velvet)+.22f*(1-clover)*
                           smoothStep(.42f,.72f,moss)+.12f*wetSilt);
    return sample;
}

MaterialSample evaluateMaterial(GroundMaterialTile material,float u,float v,uint32_t seed) {
    switch(material) {
    case GroundMaterialTile::MeadowTurf:return denseTurf(u,v,seed);
    case GroundMaterialTile::UplandShortTurf:return coarseMeadow(u,v,seed);
    case GroundMaterialTile::ExposedRockSoil:return wornSoil(u,v,seed);
    case GroundMaterialTile::RiparianMoss:return cloverMoss(u,v,seed);
    }
    return {};
}

uint32_t encodeChannel(float value) {
    return static_cast<uint32_t>(saturate(value)*255.0f+.5f);
}

uint32_t packRgba(float r,float g,float b,float a) {
    return encodeChannel(r)|(encodeChannel(g)<<8)|(encodeChannel(b)<<16)|(encodeChannel(a)<<24);
}

float decodeChannel(uint32_t pixel,int shift) {
    return static_cast<float>((pixel>>shift)&255u)*(1.0f/255.0f);
}

GroundTextureMip makeTopAlbedoMip(std::array<std::vector<float>,2>& tileFields,
                                  const GroundTextureAtlas& atlas,uint32_t seed,
                                  GroundTextureMip& normalMip) {
    GroundTextureMip albedoMip{GroundTextureAtlas::atlasWidth,GroundTextureAtlas::atlasHeight,
        std::vector<uint32_t>(static_cast<size_t>(GroundTextureAtlas::atlasWidth)*GroundTextureAtlas::atlasHeight)};
    normalMip={GroundTextureAtlas::atlasWidth,GroundTextureAtlas::atlasHeight,
        std::vector<uint32_t>(static_cast<size_t>(GroundTextureAtlas::atlasWidth)*GroundTextureAtlas::atlasHeight)};
    constexpr uint32_t size=GroundTextureAtlas::tileSize;
    const float texelWorld=GroundTextureAtlas::tileWorldSizeMetres/static_cast<float>(size);
    for(uint32_t tile=0;tile<GroundTextureAtlas::tileCount;++tile) {
        auto& heights=tileFields[0];auto& cavities=tileFields[1];
        heights.resize(static_cast<size_t>(size)*size);cavities.resize(static_cast<size_t>(size)*size);
        const uint32_t tileSeed=mixBits(seed^(0x9e3779b9u*(tile+1)));
        const auto material=static_cast<GroundMaterialTile>(tile);
        const uint32_t originX=(tile&1u)*size,originY=(tile>>1u)*size;
        for(uint32_t y=0;y<size;++y)for(uint32_t x=0;x<size;++x) {
            const float u=(static_cast<float>(x)+.5f)/size;
            const float v=(static_cast<float>(y)+.5f)/size;
            const MaterialSample sample=evaluateMaterial(material,u,v,tileSeed);
            const size_t local=static_cast<size_t>(y)*size+x;
            heights[local]=saturate(sample.height);cavities[local]=saturate(sample.cavity);
            const size_t atlasIndex=static_cast<size_t>(originY+y)*GroundTextureAtlas::atlasWidth+originX+x;
            albedoMip.pixels[atlasIndex]=packRgba(sample.red,sample.green,sample.blue,sample.roughness);
        }
        const float heightScale=atlas.heightAmplitudeMetres[tile]*2.0f;
        for(uint32_t y=0;y<size;++y)for(uint32_t x=0;x<size;++x) {
            const uint32_t left=(x+size-1)%size,right=(x+1)%size;
            const uint32_t down=(y+size-1)%size,up=(y+1)%size;
            const float dhdx=(heights[static_cast<size_t>(y)*size+right]-
                              heights[static_cast<size_t>(y)*size+left])*heightScale/(2*texelWorld);
            const float dhdy=(heights[static_cast<size_t>(up)*size+x]-
                              heights[static_cast<size_t>(down)*size+x])*heightScale/(2*texelWorld);
            const float inverse=1/std::sqrt(dhdx*dhdx+dhdy*dhdy+1);
            const float nx=-dhdx*inverse,ny=-dhdy*inverse;
            const size_t local=static_cast<size_t>(y)*size+x;
            const size_t atlasIndex=static_cast<size_t>(originY+y)*GroundTextureAtlas::atlasWidth+originX+x;
            normalMip.pixels[atlasIndex]=packRgba(nx*.5f+.5f,ny*.5f+.5f,
                                                   heights[local],cavities[local]);
        }
    }
    return albedoMip;
}

GroundTextureMip downsampleAlbedoIsolated(const GroundTextureMip& source) {
    const uint32_t width=source.width/2,height=source.height/2;
    GroundTextureMip result{width,height,std::vector<uint32_t>(static_cast<size_t>(width)*height)};
    const uint32_t sourceTile=source.width/2,destinationTile=width/2;
    for(uint32_t tile=0;tile<GroundTextureAtlas::tileCount;++tile) {
        const uint32_t sx0=(tile&1u)*sourceTile,sy0=(tile>>1u)*sourceTile;
        const uint32_t dx0=(tile&1u)*destinationTile,dy0=(tile>>1u)*destinationTile;
        for(uint32_t y=0;y<destinationTile;++y)for(uint32_t x=0;x<destinationTile;++x) {
            uint32_t sums[4]{};
            for(uint32_t oy=0;oy<2;++oy)for(uint32_t ox=0;ox<2;++ox) {
                const uint32_t pixel=source.pixels[static_cast<size_t>(sy0+y*2+oy)*source.width+sx0+x*2+ox];
                sums[0]+=pixel&255u;sums[1]+=(pixel>>8)&255u;
                sums[2]+=(pixel>>16)&255u;sums[3]+=(pixel>>24)&255u;
            }
            result.pixels[static_cast<size_t>(dy0+y)*width+dx0+x]=
                ((sums[0]+2)/4)|(((sums[1]+2)/4)<<8)|(((sums[2]+2)/4)<<16)|(((sums[3]+2)/4)<<24);
        }
    }
    return result;
}

GroundTextureMip downsampleNormalIsolated(const GroundTextureMip& source) {
    const uint32_t width=source.width/2,height=source.height/2;
    GroundTextureMip result{width,height,std::vector<uint32_t>(static_cast<size_t>(width)*height)};
    const uint32_t sourceTile=source.width/2,destinationTile=width/2;
    for(uint32_t tile=0;tile<GroundTextureAtlas::tileCount;++tile) {
        const uint32_t sx0=(tile&1u)*sourceTile,sy0=(tile>>1u)*sourceTile;
        const uint32_t dx0=(tile&1u)*destinationTile,dy0=(tile>>1u)*destinationTile;
        for(uint32_t y=0;y<destinationTile;++y)for(uint32_t x=0;x<destinationTile;++x) {
            float nx=0,ny=0,nz=0,heightSum=0,cavitySum=0,cavityMaximum=0;
            for(uint32_t oy=0;oy<2;++oy)for(uint32_t ox=0;ox<2;++ox) {
                const uint32_t pixel=source.pixels[static_cast<size_t>(sy0+y*2+oy)*source.width+sx0+x*2+ox];
                const float xNormal=decodeChannel(pixel,0)*2-1,yNormal=decodeChannel(pixel,8)*2-1;
                nx+=xNormal;ny+=yNormal;nz+=std::sqrt(std::max(0.0f,1-xNormal*xNormal-yNormal*yNormal));
                heightSum+=decodeChannel(pixel,16);
                const float cavity=decodeChannel(pixel,24);cavitySum+=cavity;cavityMaximum=std::max(cavityMaximum,cavity);
            }
            const float inverse=1/std::sqrt(std::max(nx*nx+ny*ny+nz*nz,1.0e-12f));
            nx*=inverse;ny*=inverse;
            const float cavity=(cavitySum*.75f+cavityMaximum)*.25f;
            result.pixels[static_cast<size_t>(dy0+y)*width+dx0+x]=
                packRgba(nx*.5f+.5f,ny*.5f+.5f,heightSum*.25f,cavity);
        }
    }
    return result;
}

} // namespace

GroundTextureAtlas makeGroundTextureAtlas(uint32_t seed) {
    GroundTextureAtlas atlas;
    atlas.albedoRoughness.reserve(GroundTextureAtlas::tileSafeMipCount);
    atlas.normalHeightCavity.reserve(GroundTextureAtlas::tileSafeMipCount);
    std::array<std::vector<float>,2> tileFields;
    GroundTextureMip normalTop;
    atlas.albedoRoughness.push_back(makeTopAlbedoMip(tileFields,atlas,seed,normalTop));
    atlas.normalHeightCavity.push_back(std::move(normalTop));
    while(atlas.albedoRoughness.back().width>2) {
        atlas.albedoRoughness.push_back(downsampleAlbedoIsolated(atlas.albedoRoughness.back()));
        atlas.normalHeightCavity.push_back(downsampleNormalIsolated(atlas.normalHeightCavity.back()));
    }
    return atlas;
}

GroundBiomeWeights groundBiomeWeights(const GroundBiomeInput& input) {
    const float elevation=std::max(input.elevationMetres,0.0f);
    const float slope=std::max(input.slopeGradient,0.0f);
    const float riverDistance=std::max(input.riverDistanceMetres,0.0f);
    const float moisture=saturate(input.moisture);
    const float exposure=saturate(input.exposure);

    // A river influences more than its visible water ribbon: damp alluvium and
    // moss extend onto the low bank, then recede smoothly with slope.  Steep
    // cut banks remain mineral rather than being painted uniformly green.
    const float riverInfluence=1-smoothStep(4.0f,22.0f,riverDistance);
    float riparian=riverInfluence*smoothStep(.38f,.76f,moisture)*
                    (1-smoothStep(.34f,.72f,slope));

    const float highland=smoothStep(20.0f,78.0f,elevation);
    const float cliff=smoothStep(.34f,.82f,slope);
    float mineral=saturate(cliff*mix(.58f,1.0f,exposure)+
                           highland*smoothStep(.46f,.82f,exposure)*.58f);
    mineral*=1-riparian*.62f;

    float upland=saturate(highland*(1-cliff*.78f)*(.66f+.34f*(1-moisture))+
                          smoothStep(.14f,.38f,slope)*(1-cliff)*.30f);
    upland*=1-riparian*.84f;
    upland*=1-mineral*.78f;

    float meadow=(.72f+.28f*moisture)*(1-riparian*.88f)*(1-mineral*.92f)*
                 (1-upland*.72f);
    // Preserve a little turf in transition zones; it breaks up mathematically
    // perfect rings when broad erosion noise is supplied by the world map.
    meadow*=mix(.88f,1.08f,1-exposure);

    GroundBiomeWeights result{{std::max(meadow,.001f),std::max(upland,0.0f),
                                std::max(mineral,0.0f),std::max(riparian,0.0f)}};
    float total=0;
    for(float weight:result.material)total+=weight;
    const float inverse=1/std::max(total,1.0e-6f);
    for(float& weight:result.material)weight*=inverse;
    return result;
}

} // namespace dense
