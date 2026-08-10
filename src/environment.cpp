#include "environment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dense {
namespace {

float smoothStep(float low, float high, float value) {
    const float t = clamp((value-low)/(high-low), 0.0f, 1.0f);
    return t*t*(3.0f-2.0f*t);
}

uint32_t packColor(float r, float g, float b) {
    const auto channel=[](float value) {
        return static_cast<uint32_t>(clamp(value,0.0f,1.0f)*255.0f+0.5f);
    };
    return channel(r)|(channel(g)<<8)|(channel(b)<<16)|0xff000000u;
}

}

float EnvironmentGenerator::terrainHeight(float x,float z) {
    const float radius=std::sqrt(x*x+z*z);
    const float rootMask=smoothStep(1.65f,4.6f,radius);
    const float ellipticalRadiusSq=x*x*.82f+z*z*1.16f;
    const float hill=-3.25f*(1.0f-std::exp(-ellipticalRadiusSq/235.0f));

    // Broad, low-frequency landforms keep the silhouette natural.  They are
    // masked away around the roots, preserving the oak's exact y=0 grade.
    const float broad=.24f*std::sin(x*.105f+.45f)*std::cos(z*.083f-.80f)
                     +.13f*std::sin((x+z)*.047f+1.70f)
                     +.08f*std::cos((x-z)*.071f-.20f);
    const float shoulder=.38f*std::exp(-((x+8.0f)*(x+8.0f)/120.0f+
                                         (z-5.0f)*(z-5.0f)/210.0f));
    const float shoulderAtOrigin=.38f*std::exp(-(64.0f/120.0f+25.0f/210.0f));
    return rootMask*(hill+broad+shoulder-shoulderAtOrigin);
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
    const float spacing=2.0f*extent/static_cast<float>(resolution-1);
    mesh.terrainVertices.reserve(static_cast<size_t>(resolution)*resolution);
    mesh.terrainIndices.reserve(static_cast<size_t>(resolution-1)*(resolution-1)*6);
    mesh.minimumHeight=std::numeric_limits<float>::max();
    mesh.maximumHeight=std::numeric_limits<float>::lowest();

    for(int z=0;z<resolution;++z) {
        const float worldZ=-extent+z*spacing;
        for(int x=0;x<resolution;++x) {
            const float worldX=-extent+x*spacing;
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
        // Alternating diagonals prevent a directional triangulation pattern.
        if(((x+z)&1)==0)mesh.terrainIndices.insert(mesh.terrainIndices.end(),{a,c,d,a,d,b});
        else mesh.terrainIndices.insert(mesh.terrainIndices.end(),{a,c,b,b,c,d});
    }

    // One clump per jittered 44 cm cell.  Each clump reconstructs several
    // blades in the intersection shader and owns a conservative wind AABB.
    constexpr float cell=.36f;
    const int cells=static_cast<int>(std::floor(2*grassHalfExtent/cell));
    Rng rng(seed);
    mesh.grassPatches.reserve(static_cast<size_t>(cells)*cells);
    for(int iz=0;iz<cells;++iz)for(int ix=0;ix<cells;++ix) {
        const float x=-grassHalfExtent+(ix+rng.range(.13f,.87f))*cell;
        const float z=-grassHalfExtent+(iz+rng.range(.13f,.87f))*cell;
        const float radius=std::sqrt(x*x+z*z);
        if(radius>grassHalfExtent-.25f||radius<1.05f)continue;

        // Oak shade thins the sward without making a sterile circular hole.
        const float canopyShade=1.0f-.26f*(1.0f-smoothStep(5.0f,11.0f,radius));
        const float meadowVariation=.72f+.28f*(.5f+.5f*std::sin(x*.31f+z*.19f));
        if(rng.unit()>canopyShade*meadowVariation)continue;

        const float baseY=terrainHeight(x,z)+.006f;
        const Vec3 normal=terrainNormal(x,z);
        const float moisture=clamp(.58f+.20f*std::sin(x*.12f-z*.09f)+rng.range(-.13f,.13f),0,1);
        const float maximumHeight=rng.range(.16f,.31f)*(.82f+.25f*moisture);
        const uint32_t heightCode=static_cast<uint32_t>(clamp(maximumHeight/.0025f,1,255));
        const uint32_t bladeCount=static_cast<uint32_t>(rng.range(11.0f,16.0f));
        const uint32_t packed=(heightCode<<8)|(bladeCount&255u);
        const float patchRadius=cell*.66f;
        const float windReach=maximumHeight*.44f+.025f;
        const float slopeReach=patchRadius*std::sqrt(normal.x*normal.x+normal.z*normal.z)
                              /std::max(normal.y,.25f);
        mesh.grassPatches.push_back({x-patchRadius-windReach,baseY-slopeReach-.025f,
                                     z-patchRadius-windReach,x+patchRadius+windReach,
                                     baseY+slopeReach+maximumHeight+.035f,z+patchRadius+windReach,
                                     rng.next(),packed,baseY,normal.x,normal.z,moisture});
    }
    return mesh;
}

}
