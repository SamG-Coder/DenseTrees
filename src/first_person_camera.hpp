#pragma once

#include "environment.hpp"

#include <cstdint>
#include <functional>

namespace dense {

struct FirstPersonCameraSettings {
    float eyeHeight{1.68f};
    float minimumEyeClearance{1.50f};
    float capsuleRadius{.28f};
    float walkSpeed{2.8f};
    float sprintSpeed{5.5f};
    float minimumGroundNormalY{.819152f}; // cos(35 degrees)
    float maximumStepHeight{.35f};
    float maximumSweepDistance{.18f};
    float fixedTimeStep{1.0f/120.0f};
    float maximumFrameDelta{.10f};
    uint32_t maximumPhysicsSteps{12};
    float accelerationResponse{12.0f};
    float brakingResponse{18.0f};
    float heightHalfLife{.075f};
    // The camera may traverse the complete authored map. The controller still
    // subtracts its capsule radius so every support sample remains inside the
    // terrain's explicit two-metre edge margin.
    float horizontalHalfExtent{EnvironmentGenerator::traversalHalfExtent};
};

struct FirstPersonCameraInput {
    bool forward{};
    bool backward{};
    bool left{};
    bool right{};
    bool sprint{};
    // Radians accumulated since the prior update. Positive pitch looks down,
    // matching the existing orbit-camera mouse convention.
    float yawDelta{};
    float pitchDelta{};
};

struct FirstPersonCameraState {
    Vec3 footPosition{};
    Vec3 horizontalVelocity{};
    float yaw{};
    float pitch{};
    float cameraEyeY{};
    bool grounded{};
};

struct FirstPersonCameraPose {
    Vec3 eye{};
    Vec3 forward{0,0,1};
    Vec3 right{1,0,0};
    Vec3 up{0,1,0};
};

using TerrainSurfaceSampler=std::function<TerrainSurfaceSample(float,float)>;

class FirstPersonCameraController {
public:
    static constexpr float defaultDropX=7.199395f;
    static constexpr float defaultDropZ=-11.742512f;
    static constexpr float defaultDropYaw=-.55f;
    static constexpr float defaultDropPitch=-.159f;

    explicit FirstPersonCameraController(
        FirstPersonCameraSettings settings={},TerrainSurfaceSampler terrainSampler={});

    void resetToDefaultDrop();
    void reset(float x,float z,float yaw,float pitch);
    void update(float elapsedSeconds,const FirstPersonCameraInput& input);
    void setHorizontalHalfExtent(float halfExtent);

    const FirstPersonCameraSettings& settings() const { return settings_; }
    const FirstPersonCameraState& state() const { return state_; }
    FirstPersonCameraPose pose() const;

private:
    struct GroundContact {
        float supportHeight{};
        float minimumNormalY{1.0f};
        bool valid{};
    };

    GroundContact groundContact(float x,float z) const;
    bool tryMove(float deltaX,float deltaZ);
    void simulateStep(float deltaTime,const FirstPersonCameraInput& input);
    float movementLimit() const;

    FirstPersonCameraSettings settings_;
    TerrainSurfaceSampler terrainSampler_;
    FirstPersonCameraState state_;
    float accumulator_{};
};

} // namespace dense
