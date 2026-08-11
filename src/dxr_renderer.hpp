#pragma once
#include "environment_simulation.hpp"
#include "tree.hpp"
#include <windows.h>
#include <memory>

namespace dense {

struct DebugRenderSettings {
    float grassDensity = 3.00f;
    float bladeHeightScale = 1.25f;
    float groundNormalStrength = 1.00f;
    float groundDetailStrength = 1.00f;
    float shortGrassDrawDistance = 26.0f;
    float tallGrassDrawDistance = 52.0f;
};

struct CameraView {
    Vec3 eye{};
    Vec3 forward{0.0f,0.0f,1.0f};
};

struct PlayerLocalLight {
    bool enabled = false;
    // The player light is an omnidirectional warm point light by default.
    // Spotlight mode remains available to callers without changing the b0 ABI.
    bool spotlight = false;
    float intensity = 60.0f;
    float range = 22.0f;
    float innerConeRadians = 0.19198622f; // 11 degrees
    float outerConeRadians = 0.31415927f; // 18 degrees
};

class DxrRenderer {
public:
    DxrRenderer();~DxrRenderer();DxrRenderer(const DxrRenderer&)=delete;DxrRenderer& operator=(const DxrRenderer&)=delete;
    bool initialize(HWND window,int width,int height);
    void resize(int width,int height);
    void setTree(const TreeMesh& tree);
    void render(const CameraView& view,const DebugRenderSettings& settings,
                const EnvironmentCB& environment,const PlayerLocalLight& localLight);
    // Compatibility wrapper for the current orbit-camera caller. New callers
    // should provide CameraView explicitly so a player/camera transform can own
    // the local light without being reconstructed inside the renderer.
    void render(float yaw,float pitch,float distance,const DebugRenderSettings& settings,
                const EnvironmentCB& environment);
    const wchar_t* error()const;
    bool ready()const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
