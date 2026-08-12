#include "aoe_horizon.hpp"

#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <tuple>

namespace {

void require(bool condition,const char* message) {
    if(!condition) { std::cerr<<"FAIL: "<<message<<'\n';std::exit(1); }
}

bool finite(dense::Vec3 value) {
    return std::isfinite(value.x)&&std::isfinite(value.y)&&
           std::isfinite(value.z);
}

std::uint64_t mix(std::uint64_t hash,std::uint32_t value) {
    return (hash^value)*1099511628211ull;
}

std::uint64_t signature(const dense::EnvironmentMesh& mesh) {
    std::uint64_t hash=1469598103934665603ull;
    for(const dense::MeshVertex& vertex:mesh.terrainVertices) {
        hash=mix(hash,std::bit_cast<std::uint32_t>(vertex.position.x));
        hash=mix(hash,std::bit_cast<std::uint32_t>(vertex.position.y));
        hash=mix(hash,std::bit_cast<std::uint32_t>(vertex.position.z));
        hash=mix(hash,vertex.color);
        hash=mix(hash,std::bit_cast<std::uint32_t>(vertex.material));
    }
    for(std::uint32_t index:mesh.terrainIndices)hash=mix(hash,index);
    for(const dense::MeshVertex& vertex:mesh.riverVertices) {
        hash=mix(hash,std::bit_cast<std::uint32_t>(vertex.position.x));
        hash=mix(hash,std::bit_cast<std::uint32_t>(vertex.position.y));
        hash=mix(hash,std::bit_cast<std::uint32_t>(vertex.position.z));
        hash=mix(hash,std::bit_cast<std::uint32_t>(vertex.u));
    }
    for(std::uint32_t index:mesh.riverIndices)hash=mix(hash,index);
    return hash;
}

float nearHeight(float x,float z) {
    return 1.75f+x*.0007f-z*.0004f+
           std::sin(x*.013f)*.035f+std::cos(z*.017f)*.025f;
}

dense::TerrainSurfaceSample nearSample(float x,float z) {
    const float epsilon=.05f;
    const float dx=(nearHeight(x+epsilon,z)-nearHeight(x-epsilon,z))/(2*epsilon);
    const float dz=(nearHeight(x,z+epsilon)-nearHeight(x,z-epsilon))/(2*epsilon);
    return {{x,nearHeight(x,z),z},dense::normalize({-dx,1,-dz}),true};
}

using PositionKey=std::pair<int,int>;

std::map<PositionKey,std::vector<float>> seamHeights(
    const dense::EnvironmentMesh& mesh,int extent) {
    std::map<PositionKey,std::vector<float>> result;
    for(const dense::MeshVertex& vertex:mesh.terrainVertices) {
        const int x=static_cast<int>(std::lround(vertex.position.x));
        const int z=static_cast<int>(std::lround(vertex.position.z));
        if((std::abs(x)==extent&&std::abs(z)<=extent)||
           (std::abs(z)==extent&&std::abs(x)<=extent))
            result[{x,z}].push_back(vertex.position.y);
    }
    return result;
}

void validateTriangles(const std::vector<dense::MeshVertex>& vertices,
                       const std::vector<std::uint32_t>& indices) {
    require(!vertices.empty()&&!indices.empty()&&indices.size()%3==0,
            "horizon mesh inventory is empty or malformed");
    for(std::size_t index=0;index<indices.size();index+=3) {
        require(indices[index]<vertices.size()&&indices[index+1]<vertices.size()&&
                indices[index+2]<vertices.size(),
                "horizon triangle index escaped its vertex stream");
        const dense::MeshVertex& a=vertices[indices[index]];
        const dense::MeshVertex& b=vertices[indices[index+1]];
        const dense::MeshVertex& c=vertices[indices[index+2]];
        const dense::Vec3 area=dense::cross(b.position-a.position,
                                            c.position-a.position);
        require(finite(a.position)&&finite(b.position)&&finite(c.position)&&
                finite(a.normal)&&finite(b.normal)&&finite(c.normal)&&
                dense::lengthSq(area)>1.0e-8f,
                "horizon contains a non-finite or degenerate triangle");
    }
}

float maximumHorizontalTriangleEdge(
    const std::vector<dense::MeshVertex>& vertices,
    const std::vector<std::uint32_t>& indices) {
    float maximum=0.0f;
    for(std::size_t index=0;index<indices.size();index+=3) {
        for(int edge=0;edge<3;++edge) {
            const dense::Vec3& a=vertices[indices[index+edge]].position;
            const dense::Vec3& b=vertices[indices[index+(edge+1)%3]].position;
            maximum=std::max(maximum,std::hypot(a.x-b.x,a.z-b.z));
        }
    }
    return maximum;
}

} // namespace

int main() {
    dense::EnvironmentMesh first;
    const dense::AoeHorizonStats stats=dense::AoeHorizonBuilder::append(
        first,8675309,0,0,nearSample);
    require(stats.outerExtent==16384.0f&&stats.terrainVertices==322560&&
            stats.terrainIndices==483840&&
            stats.terrainCells[0]==3840&&
            stats.terrainCells[1]==15360&&
            stats.terrainCells[2]==61440,
            "default horizon does not contain the complete refined LOD annuli");
    require(stats.waterVertices>0&&stats.waterIndices>0,
            "horizon seed produced no distant water surface");
    require(stats.blendedTerrainCells[1]>0&&stats.blendedTerrainCells[2]>0,
            "far horizon fell back to categorical rectangular biome blocks");
    require(stats.waterBoundaryCells[1]>0&&stats.waterBoundaryCells[2]>0,
            "far water has no clipped shoreline contours");
    validateTriangles(first.terrainVertices,first.terrainIndices);
    validateTriangles(first.riverVertices,first.riverIndices);
    constexpr float maximumRefinedDiagonal=181.1f;
    require(maximumHorizontalTriangleEdge(first.terrainVertices,
                                           first.terrainIndices)<
                maximumRefinedDiagonal&&
            maximumHorizontalTriangleEdge(first.riverVertices,
                                           first.riverIndices)<
                maximumRefinedDiagonal,
            "far horizon contains a terrain or water slab larger than 128 m");

    float maximumExtent=0.0f;
    for(std::size_t cell=0;cell<first.terrainVertices.size();cell+=4) {
        float minimumX=first.terrainVertices[cell].position.x;
        float maximumX=minimumX,minimumZ=first.terrainVertices[cell].position.z;
        float maximumZ=minimumZ;
        for(std::size_t corner=0;corner<4;++corner) {
            const dense::MeshVertex& vertex=first.terrainVertices[cell+corner];
            minimumX=std::min(minimumX,vertex.position.x);
            maximumX=std::max(maximumX,vertex.position.x);
            minimumZ=std::min(minimumZ,vertex.position.z);
            maximumZ=std::max(maximumZ,vertex.position.z);
            maximumExtent=std::max(maximumExtent,std::max(
                std::abs(vertex.position.x),std::abs(vertex.position.z)));
            require(vertex.material>=7.0f&&vertex.material<7.16f,
                    "horizon terrain escaped the authoritative biome material range");
        }
        const float centerX=(minimumX+maximumX)*.5f;
        const float centerZ=(minimumZ+maximumZ)*.5f;
        require(maximumX-minimumX<=128.0f&&maximumZ-minimumZ<=128.0f,
                "far horizon emitted a giant rectangular terrain patch");
        require(std::max(std::abs(centerX),std::abs(centerZ))>256.0f,
                "horizon emitted a terrain cell inside the 512 m near scene");
    }
    require(maximumExtent==16384.0f,
            "outer horizon does not reach the 16 km square boundary");

    // The outer shell closes the visual horizon rather than projecting
    // kilometre-distant source elevations high into the sky as floating land
    // shelves. Its relief is intentionally compressed while the two nearer
    // rings retain progressively more of the authoritative heightfield.
    float outerMaximumHeight=-1.0e9f;
    for(const dense::MeshVertex& vertex:first.terrainVertices) {
        if(std::max(std::abs(vertex.position.x),std::abs(vertex.position.z))>=4096.0f)
            outerMaximumHeight=std::max(outerMaximumHeight,vertex.position.y);
    }
    require(outerMaximumHeight<2.5f,
            "outer horizon relief can form detached floating land shelves");

    for(const dense::MeshVertex& vertex:first.terrainVertices) {
        if(std::hypot(vertex.position.x,vertex.position.z)<14000.0f)continue;
        require(vertex.position.y<.05f,
                "outer visual shell did not close beneath the ocean horizon");
    }

    for(const dense::MeshVertex& vertex:first.riverVertices)
        require(std::abs(vertex.position.y-.012f)<1.0e-6f&&
                    std::abs(vertex.material-6.1f)<2.0e-4f&&
                    vertex.u>=0.0f&&vertex.u<=1.0f,
                "horizon water lost its level surface, depth or material contract");

    const auto nearSeam=seamHeights(first,256);
    require(!nearSeam.empty(),"horizon has no shared inner boundary");
    for(const auto& [position,heights]:nearSeam) {
        const float expected=nearHeight(static_cast<float>(position.first),
                                        static_cast<float>(position.second));
        for(float height:heights)
            require(std::abs(height-expected)<1.0e-5f,
                    "inner horizon boundary does not match the near terrain sampler");
    }
    for(int seam:{1024,4096}) {
        const auto values=seamHeights(first,seam);
        require(!values.empty(),"horizon LOD seam has no shared vertices");
        for(const auto& [position,heights]:values) {
            if((position.first+16384)%512!=0||
               (position.second+16384)%512!=0)continue;
            const float reference=heights.front();
            for(float height:heights)
                require(std::abs(height-reference)<1.0e-5f,
                        "common horizon LOD seam vertices disagree in height");
        }
    }

    dense::EnvironmentMesh second;
    const auto secondStats=dense::AoeHorizonBuilder::append(
        second,8675309,0,0,nearSample);
    require(stats.terrainCells==secondStats.terrainCells&&
                stats.waterCells==secondStats.waterCells&&
                signature(first)==signature(second),
            "same seed and source center produced a different horizon mesh");

    std::cout<<"AI RPG AOE blended horizon checks passed (outer blended="
             <<stats.blendedTerrainCells[2]<<", outer shoreline="
             <<stats.waterBoundaryCells[2]<<")\n";
}
