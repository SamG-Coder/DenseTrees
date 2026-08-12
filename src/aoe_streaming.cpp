#include "aoe_streaming.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dense {
namespace {

bool contains(std::span<const AoeChunkCoordinate> values,
              AoeChunkCoordinate value) {
    return std::find(values.begin(),values.end(),value)!=values.end();
}

int sanitizedLoadRadius(const AoeChunkStreamSettings& settings) {
    return std::clamp(settings.loadRadius,0,256);
}

int sanitizedRetentionPadding(const AoeChunkStreamSettings& settings) {
    return std::clamp(settings.retentionPadding,0,256);
}

double sanitizedRecenterDistance(const AoeChunkStreamSettings& settings) {
    return std::isfinite(settings.sceneRecenterDistance)&&
           settings.sceneRecenterDistance>=
               static_cast<double>(AoeChunkStreamingPolicy::chunkSize)?
        settings.sceneRecenterDistance:96.0;
}

} // namespace

std::optional<AoeChunkCoordinate>
AoeChunkStreamingPolicy::coordinateForWorld(double worldX,double worldZ) {
    if(!std::isfinite(worldX)||!std::isfinite(worldZ))return std::nullopt;
    constexpr double minimum=static_cast<double>(
        std::numeric_limits<std::int64_t>::min())*chunkSize;
    constexpr double maximum=static_cast<double>(
        std::numeric_limits<std::int64_t>::max())*chunkSize;
    if(worldX<minimum||worldX>=maximum||worldZ<minimum||worldZ>=maximum)
        return std::nullopt;
    return AoeChunkCoordinate{
        static_cast<std::int64_t>(std::floor(worldX/chunkSize)),
        static_cast<std::int64_t>(std::floor(worldZ/chunkSize))};
}

double AoeChunkStreamingPolicy::chunkOrigin(std::int64_t coordinate) {
    return static_cast<double>(coordinate)*chunkSize;
}

bool AoeChunkStreamingPolicy::outsideRetentionRadius(
    AoeChunkCoordinate coordinate,AoeChunkCoordinate center,
    const AoeChunkStreamSettings& settings) {
    const std::int64_t radius=static_cast<std::int64_t>(
        sanitizedLoadRadius(settings)+sanitizedRetentionPadding(settings));
    // Difference through a wider floating representation avoids signed
    // overflow for malformed or fuzzed extreme coordinates.
    const long double deltaX=static_cast<long double>(coordinate.x)-center.x;
    const long double deltaZ=static_cast<long double>(coordinate.z)-center.z;
    return std::abs(deltaX)>radius||std::abs(deltaZ)>radius;
}

std::vector<AoeChunkCoordinate> AoeChunkStreamingPolicy::wantedChunks(
    AoeChunkCoordinate center,std::span<const AoeChunkCoordinate> resident,
    std::optional<AoeChunkCoordinate> pending,
    const AoeChunkStreamSettings& settings) {
    const int radius=sanitizedLoadRadius(settings);
    const std::int64_t radiusSquared=static_cast<std::int64_t>(radius)*radius;
    struct Candidate {
        AoeChunkCoordinate coordinate;
        std::int64_t distanceSquared;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>((radius*2+1)*(radius*2+1)));
    for(int z=-radius;z<=radius;++z) {
        for(int x=-radius;x<=radius;++x) {
            const std::int64_t distanceSquared=
                static_cast<std::int64_t>(x)*x+static_cast<std::int64_t>(z)*z;
            if(distanceSquared>radiusSquared)continue;
            // Radius is bounded, but the absolute coordinate may be fuzzed at
            // int64 limits. Skip an unrepresentable neighbour deterministically.
            if((x<0&&center.x<std::numeric_limits<std::int64_t>::min()-x)||
               (x>0&&center.x>std::numeric_limits<std::int64_t>::max()-x)||
               (z<0&&center.z<std::numeric_limits<std::int64_t>::min()-z)||
               (z>0&&center.z>std::numeric_limits<std::int64_t>::max()-z))
                continue;
            const AoeChunkCoordinate coordinate{center.x+x,center.z+z};
            if(contains(resident,coordinate)||
               (pending&&*pending==coordinate))continue;
            candidates.push_back({coordinate,distanceSquared});
        }
    }
    std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,
                                                     const Candidate& b) {
        if(a.distanceSquared!=b.distanceSquared)
            return a.distanceSquared<b.distanceSquared;
        if(a.coordinate.z!=b.coordinate.z)return a.coordinate.z<b.coordinate.z;
        return a.coordinate.x<b.coordinate.x;
    });
    std::vector<AoeChunkCoordinate> result;
    result.reserve(candidates.size());
    for(const Candidate& candidate:candidates)result.push_back(candidate.coordinate);
    return result;
}

AoeSceneStreamState::AoeSceneStreamState(
    double initialCenterX,double initialCenterZ,AoeChunkStreamSettings settings)
    :settings_(settings) {
    settings_.loadRadius=sanitizedLoadRadius(settings_);
    settings_.retentionPadding=sanitizedRetentionPadding(settings_);
    settings_.sceneRecenterDistance=sanitizedRecenterDistance(settings_);
    centerWorldX_=std::isfinite(initialCenterX)?snapToChunk(initialCenterX):0.0;
    centerWorldZ_=std::isfinite(initialCenterZ)?snapToChunk(initialCenterZ):0.0;
}

double AoeSceneStreamState::snapToChunk(double coordinate) {
    // Round to the nearest chunk origin. floor(x + .5) gives symmetric cells
    // around an origin and deterministic behaviour at negative coordinates.
    return std::floor(coordinate/AoeChunkStreamingPolicy::chunkSize+.5)*
           AoeChunkStreamingPolicy::chunkSize;
}

void AoeSceneStreamState::invalidateActive() {
    if(active_) {
        active_.reset();
        if(nextRequestId_!=std::numeric_limits<std::uint64_t>::max())
            ++nextRequestId_;
    }
}

std::optional<AoeSceneStreamRequest> AoeSceneStreamState::requestForCamera(
    double cameraX,double cameraZ) {
    if(!std::isfinite(cameraX)||!std::isfinite(cameraZ))return std::nullopt;
    const double deltaX=cameraX-centerWorldX_;
    const double deltaZ=cameraZ-centerWorldZ_;
    const double threshold=settings_.sceneRecenterDistance;
    if(deltaX*deltaX+deltaZ*deltaZ<=threshold*threshold) {
        // If the player returns before a build completes, invalidate that
        // destination. Its eventual worker result will be rejected by id.
        invalidateActive();
        return std::nullopt;
    }
    const double targetX=snapToChunk(cameraX);
    const double targetZ=snapToChunk(cameraZ);
    const auto coordinate=AoeChunkStreamingPolicy::coordinateForWorld(
        targetX,targetZ);
    if(!coordinate)return std::nullopt;
    if(active_&&active_->centerChunk==*coordinate)return std::nullopt;
    if(nextRequestId_==0||nextRequestId_==
            std::numeric_limits<std::uint64_t>::max())
        nextRequestId_=1;
    active_=AoeSceneStreamRequest{
        nextRequestId_++,*coordinate,targetX,targetZ};
    return active_;
}

bool AoeSceneStreamState::isCurrent(std::uint64_t requestId) const {
    return active_&&requestId!=0&&active_->id==requestId;
}

bool AoeSceneStreamState::commit(std::uint64_t requestId) {
    if(!isCurrent(requestId))return false;
    centerWorldX_=active_->centerWorldX;
    centerWorldZ_=active_->centerWorldZ;
    active_.reset();
    return true;
}

void AoeSceneStreamState::abandon(std::uint64_t requestId) {
    if(isCurrent(requestId))active_.reset();
}

} // namespace dense
