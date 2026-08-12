#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dense {

// InfiniteWorld.cs uses 32x32-tile chunks.  Keep the address independent from
// render-local coordinates so the same seed/chunk pair always describes the
// same source-world content, even after the 3D scene recentres.
struct AoeChunkCoordinate {
    std::int64_t x{};
    std::int64_t z{};

    friend constexpr bool operator==(
        const AoeChunkCoordinate&,const AoeChunkCoordinate&) = default;
};

struct AoeChunkStreamSettings {
    // Matches ZoomScaledWorldLoadingPolicy.StandardRadius in the source game.
    int loadRadius{5};
    // Source chunks remain resident one ring beyond the circular load area.
    int retentionPadding{1};
    // A 512 m near-field therefore keeps at least 160 m of already-renderable
    // terrain in the direction of travel while its replacement is generated.
    double sceneRecenterDistance{96.0};
};

class AoeChunkStreamingPolicy {
public:
    static constexpr std::int64_t chunkSize=32;

    // floor division is intentional: world -0.01 belongs to chunk -1, not 0.
    [[nodiscard]] static std::optional<AoeChunkCoordinate> coordinateForWorld(
        double worldX,double worldZ);
    [[nodiscard]] static double chunkOrigin(std::int64_t coordinate);

    // The source project loads a circle but retains a square with one extra
    // ring.  That hysteresis prevents border chunks from unloading/reloading
    // while the camera moves along a chunk edge.
    [[nodiscard]] static bool outsideRetentionRadius(
        AoeChunkCoordinate coordinate,AoeChunkCoordinate center,
        const AoeChunkStreamSettings& settings={});

    // Missing chunks are returned nearest first. Equal-distance ties are
    // ordered z then x, reproducing the source scan order without relying on
    // container iteration order.
    [[nodiscard]] static std::vector<AoeChunkCoordinate> wantedChunks(
        AoeChunkCoordinate center,
        std::span<const AoeChunkCoordinate> resident={},
        std::optional<AoeChunkCoordinate> pending=std::nullopt,
        const AoeChunkStreamSettings& settings={});
};

struct AoeSceneStreamRequest {
    std::uint64_t id{};
    AoeChunkCoordinate centerChunk{};
    double centerWorldX{};
    double centerWorldZ{};
};

// Main-thread state for an asynchronous camera-centred scene build.  A worker
// receives the value-only request. On completion, commit() accepts only the
// newest id, so a slow result can never replace a newer camera destination.
class AoeSceneStreamState {
public:
    explicit AoeSceneStreamState(
        double initialCenterX=0.0,double initialCenterZ=0.0,
        AoeChunkStreamSettings settings={});

    [[nodiscard]] std::optional<AoeSceneStreamRequest> requestForCamera(
        double cameraX,double cameraZ);
    [[nodiscard]] bool isCurrent(std::uint64_t requestId) const;
    [[nodiscard]] bool commit(std::uint64_t requestId);
    void abandon(std::uint64_t requestId);

    [[nodiscard]] double centerWorldX() const { return centerWorldX_; }
    [[nodiscard]] double centerWorldZ() const { return centerWorldZ_; }
    [[nodiscard]] std::optional<AoeSceneStreamRequest> activeRequest() const {
        return active_;
    }
    [[nodiscard]] const AoeChunkStreamSettings& settings() const {
        return settings_;
    }

private:
    [[nodiscard]] static double snapToChunk(double coordinate);
    void invalidateActive();

    AoeChunkStreamSettings settings_{};
    double centerWorldX_{};
    double centerWorldZ_{};
    std::uint64_t nextRequestId_{1};
    std::optional<AoeSceneStreamRequest> active_;
};

} // namespace dense
