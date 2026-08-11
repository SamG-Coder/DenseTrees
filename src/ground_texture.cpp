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

MaterialSample denseTurf(float u,float v,uint32_t seed) {
    const float macro=periodicFbm(u,v,4,5,seed^0x71b52a91u);
    const float fine=periodicFbm(u,v,32,4,seed^0x36d17ab5u);
    const float warp=(periodicValueNoise(u,v,13,seed^0xad90777du)-.5f)*.16f;
    const float bladeA=periodicStripe(u,v,113,37,warp,.014f,.032f);
    const float bladeB=periodicStripe(u,v,-67,149,warp*.73f+.17f,.011f,.027f);
    const float blades=saturate(bladeA*.72f+bladeB*.48f);
    const float pale=smoothStep(.70f,.94f,periodicValueNoise(u,v,19,seed^0x05f131b7u));
    MaterialSample sample;
    sample.red=mix(.030f,.070f,macro)+.016f*blades+.018f*pale;
    sample.green=mix(.088f,.170f,macro)+.034f*blades+.020f*pale;
    sample.blue=mix(.010f,.033f,fine)+.006f*blades+.006f*pale;
    sample.roughness=saturate(.94f-.075f*blades-.035f*macro+.025f*(1-fine));
    sample.height=saturate(.43f+.17f*(macro-.5f)+.16f*(fine-.5f)+.20f*blades);
    sample.cavity=saturate(.10f+.28f*(1-fine)+.32f*(1-blades)*smoothStep(.35f,.75f,macro));
    return sample;
}

MaterialSample coarseMeadow(float u,float v,uint32_t seed) {
    const float macro=periodicFbm(u,v,3,5,seed^0xe8a72b6du);
    const float dryPatch=periodicFbm(u,v,7,4,seed^0x94c3d8f1u);
    const CellularSample tuft=periodicCellular(u,v,19,seed^0x30b4a6c9u);
    const float tuftMask=(1-smoothStep(.16f,.54f,tuft.nearest))*(.62f+.38f*tuft.identity);
    const float warp=(periodicValueNoise(u,v,11,seed^0x4f1bbcddu)-.5f)*.20f;
    const float stemA=periodicStripe(u,v,59,17,warp,.010f,.024f);
    const float stemB=periodicStripe(u,v,-31,83,warp*.63f+.31f,.008f,.021f);
    const float stems=saturate((stemA*.66f+stemB*.48f)*(.42f+.78f*tuftMask));
    const float dryness=smoothStep(.42f,.78f,dryPatch+.16f*(1-macro));
    const float greenR=mix(.044f,.082f,macro),greenG=mix(.100f,.182f,macro),greenB=mix(.014f,.038f,macro);
    const float dryR=mix(.135f,.240f,dryPatch),dryG=mix(.115f,.205f,dryPatch),dryB=mix(.035f,.073f,dryPatch);
    MaterialSample sample;
    sample.red=mix(greenR,dryR,dryness)+.018f*stems;
    sample.green=mix(greenG,dryG,dryness)+.020f*stems;
    sample.blue=mix(greenB,dryB,dryness)+.006f*stems;
    sample.roughness=saturate(.91f+.045f*dryness-.075f*stems-.025f*tuftMask);
    sample.height=saturate(.40f+.17f*(macro-.5f)+.16f*tuftMask+.21f*stems);
    sample.cavity=saturate(.14f+.33f*(1-tuftMask)+.22f*(1-stems)*smoothStep(.40f,.76f,dryPatch));
    return sample;
}

MaterialSample wornSoil(float u,float v,uint32_t seed) {
    const float macro=periodicFbm(u,v,5,5,seed^0x7c2dd1a9u);
    const float granules=periodicFbm(u,v,43,4,seed^0xc1f57a3bu);
    const float moisture=periodicFbm(u,v,3,4,seed^0x21d469efu);
    const CellularSample primary=periodicCellular(u,v,12,seed^0x991b35c7u);
    const CellularSample secondary=periodicCellular(u,v,31,seed^0x446af283u);
    const float crackA=1-smoothStep(.018f,.075f,primary.edge);
    const float crackB=(1-smoothStep(.012f,.048f,secondary.edge))*(1-.58f*crackA);
    const float cracks=saturate(crackA+.46f*crackB);
    float pebble=smoothStep(.84f,.955f,periodicValueNoise(u,v,83,seed^0x14e8b6ddu));
    pebble*=smoothStep(.16f,.48f,secondary.nearest);
    const float damp=smoothStep(.38f,.72f,moisture);
    const float dryR=mix(.135f,.235f,macro),dryG=mix(.077f,.142f,macro),dryB=mix(.034f,.073f,macro);
    const float wetR=mix(.060f,.110f,macro),wetG=mix(.036f,.071f,macro),wetB=mix(.018f,.038f,macro);
    MaterialSample sample;
    sample.red=mix(dryR,wetR,damp)+.075f*pebble;
    sample.green=mix(dryG,wetG,damp)+.068f*pebble;
    sample.blue=mix(dryB,wetB,damp)+.058f*pebble;
    sample.red=mix(sample.red,.025f,cracks*.72f);
    sample.green=mix(sample.green,.016f,cracks*.72f);
    sample.blue=mix(sample.blue,.009f,cracks*.72f);
    sample.roughness=saturate(.90f+.055f*(1-damp)-.11f*pebble-.035f*granules);
    sample.height=saturate(.50f+.22f*(macro-.5f)+.11f*(granules-.5f)-.23f*cracks+.12f*pebble);
    sample.cavity=saturate(.10f+.72f*cracks+.14f*(1-granules));
    return sample;
}

MaterialSample cloverMoss(float u,float v,uint32_t seed) {
    const float moss=periodicFbm(u,v,8,5,seed^0xa7ef1531u);
    const float velvet=periodicFbm(u,v,52,4,seed^0x2b91c4d7u);
    const CellularSample plant=periodicCellular(u,v,23,seed^0x5a7e0b93u);
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
    const float mossLight=smoothStep(.30f,.78f,moss);
    MaterialSample sample;
    sample.red=mix(.022f,.060f,mossLight)+.020f*clover+.012f*leafVein;
    sample.green=mix(.073f,.164f,mossLight)+.046f*clover+.014f*leafVein;
    sample.blue=mix(.011f,.040f,velvet)+.008f*clover;
    sample.roughness=saturate(.93f-.065f*mossLight-.105f*clover+.018f*(1-velvet));
    sample.height=saturate(.45f+.14f*(moss-.5f)+.08f*(velvet-.5f)+.23f*clover+.055f*leafVein);
    sample.cavity=saturate(.10f+.27f*(1-velvet)+.26f*(1-clover)*smoothStep(.42f,.72f,moss));
    return sample;
}

MaterialSample evaluateMaterial(GroundMaterialTile material,float u,float v,uint32_t seed) {
    switch(material) {
    case GroundMaterialTile::DenseShortTurf:return denseTurf(u,v,seed);
    case GroundMaterialTile::CoarseMeadow:return coarseMeadow(u,v,seed);
    case GroundMaterialTile::WornSoil:return wornSoil(u,v,seed);
    case GroundMaterialTile::CloverMoss:return cloverMoss(u,v,seed);
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

} // namespace dense
