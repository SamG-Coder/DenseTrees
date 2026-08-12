#include "aoe_streaming.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

void require(bool condition,const char* message) {
    if(!condition) { std::cerr<<"FAIL: "<<message<<'\n';std::exit(1); }
}

} // namespace

int main() {
    using dense::AoeChunkCoordinate;
    using dense::AoeChunkStreamingPolicy;

    const auto zero=AoeChunkStreamingPolicy::coordinateForWorld(0.0,31.999);
    const auto positive=AoeChunkStreamingPolicy::coordinateForWorld(32.0,64.0);
    const auto negative=AoeChunkStreamingPolicy::coordinateForWorld(-.001,-32.0);
    const auto lower=AoeChunkStreamingPolicy::coordinateForWorld(-32.001,-64.001);
    require(zero&&*zero==AoeChunkCoordinate{0,0}&&
            positive&&*positive==AoeChunkCoordinate{1,2}&&
            negative&&*negative==AoeChunkCoordinate{-1,-1}&&
            lower&&*lower==AoeChunkCoordinate{-2,-3},
            "world-to-chunk floor division is wrong at signed boundaries");
    require(!AoeChunkStreamingPolicy::coordinateForWorld(
                std::numeric_limits<double>::infinity(),0.0),
            "non-finite world coordinate produced a chunk address");

    dense::AoeChunkStreamSettings twoRing;
    twoRing.loadRadius=2;
    const auto all=AoeChunkStreamingPolicy::wantedChunks({7,-4},{},std::nullopt,
                                                          twoRing);
    require(all.size()==13&&all[0]==AoeChunkCoordinate{7,-4}&&
            all[1]==AoeChunkCoordinate{7,-5}&&
            all[2]==AoeChunkCoordinate{6,-4}&&
            all[3]==AoeChunkCoordinate{8,-4}&&
            all[4]==AoeChunkCoordinate{7,-3},
            "circular wanted chunks are not deterministic nearest-first");
    const std::vector<AoeChunkCoordinate> resident{{7,-4},{7,-5}};
    const auto missing=AoeChunkStreamingPolicy::wantedChunks(
        {7,-4},resident,AoeChunkCoordinate{6,-4},twoRing);
    require(missing.size()==10&&missing.front()==AoeChunkCoordinate{8,-4},
            "resident or pending chunks remained in the wanted list");

    dense::AoeChunkStreamSettings sourcePolicy;
    require(!AoeChunkStreamingPolicy::outsideRetentionRadius(
                {10,2},{4,-4},sourcePolicy)&&
            !AoeChunkStreamingPolicy::outsideRetentionRadius(
                {-2,-10},{4,-4},sourcePolicy)&&
            AoeChunkStreamingPolicy::outsideRetentionRadius(
                {11,-4},{4,-4},sourcePolicy),
            "one-ring square retention hysteresis differs from the source game");

    dense::AoeSceneStreamState stream(0.0,0.0);
    require(!stream.requestForCamera(96.0,0.0),
            "scene recentered on the hysteresis boundary");
    const auto first=stream.requestForCamera(96.01,0.0);
    require(first&&first->id!=0&&first->centerChunk==AoeChunkCoordinate{3,0}&&
            first->centerWorldX==96.0&&first->centerWorldZ==0.0&&
            stream.isCurrent(first->id),
            "scene recenter request was not snapped to a stable chunk origin");
    require(!stream.requestForCamera(100.0,1.0),
            "same async destination was requested twice");
    const auto newer=stream.requestForCamera(160.1,0.0);
    require(newer&&newer->id>first->id&&
            newer->centerChunk==AoeChunkCoordinate{5,0}&&
            !stream.isCurrent(first->id)&&!stream.commit(first->id),
            "stale asynchronous scene request was accepted");
    require(stream.commit(newer->id)&&stream.centerWorldX()==160.0&&
            stream.centerWorldZ()==0.0,
            "latest asynchronous scene did not commit its snapped center");

    const auto returnTrip=stream.requestForCamera(40.0,0.0);
    require(returnTrip&&stream.isCurrent(returnTrip->id),
            "return trip did not request a replacement scene");
    require(!stream.requestForCamera(159.0,0.0)&&
            !stream.isCurrent(returnTrip->id)&&!stream.commit(returnTrip->id),
            "returning inside hysteresis did not invalidate the worker result");

    const auto abandoned=stream.requestForCamera(-1.0,-100.0);
    require(abandoned&&stream.isCurrent(abandoned->id),
            "negative scene destination did not produce a request");
    stream.abandon(abandoned->id);
    require(!stream.isCurrent(abandoned->id),
            "abandoned scene request remained current");

    std::cout<<"AI RPG AOE streaming policy checks passed\n";
}
