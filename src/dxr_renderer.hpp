#pragma once
#include "environment.hpp"
#include "environment_simulation.hpp"
#include "tree.hpp"
#include <windows.h>
#include <functional>
#include <memory>

namespace dense {

struct DebugRenderSettings {
    // The default sward is close-mown park turf.  Density supplies the fine
    // overlapping canopy while the authored blade heights remain physical.
    float grassDensity = 3.75f;
    float bladeHeightScale = 1.00f;
    float groundNormalStrength = 1.00f;
    float groundDetailStrength = 1.00f;
    float shortGrassDrawDistance = 26.0f;
    float tallGrassDrawDistance = 52.0f;
};

struct CameraView {
    Vec3 eye{};
    Vec3 forward{0.0f,0.0f,1.0f};
    // First-person locomotion can expose a lightweight world-space grass
    // collider.  The raster grass shader evaluates it per blade; no grass
    // geometry is read back or rewritten on the CPU.
    Vec3 grassInteractionPosition{};
    Vec3 grassInteractionVelocity{};
    bool grassInteractionEnabled = false;
};

struct PlayerLocalLight {
    bool enabled = false;
    // The player light is an omnidirectional warm point light by default.
    // Spotlight mode remains available to callers without changing the b0 ABI.
    bool spotlight = false;
    // A small hand-carried lamp.  The shaders use inverse-square falloff, so
    // the previous value of 60 bleached nearby bark and fluorescently lit the
    // ground at arm's length.  Four renderer-candela units retain useful
    // visibility several metres out without driving diffuse albedo into the
    // tone-mapper's white shoulder.
    float intensity = 4.0f;
    float range = 16.0f;
    float innerConeRadians = 0.19198622f; // 11 degrees
    float outerConeRadians = 0.31415927f; // 18 degrees
};

class DxrRenderer {
public:
    using WaterSampler=std::function<PersistentWaterSample(float,float)>;
    DxrRenderer();~DxrRenderer();DxrRenderer(const DxrRenderer&)=delete;DxrRenderer& operator=(const DxrRenderer&)=delete;
    bool initialize(HWND window,int width,int height);
    void resize(int width,int height);
    // Must be called before setTree(). The visual-test scene may omit this and
    // retain the original EnvironmentGenerator defaults.
    void setWorld(EnvironmentMesh world,WaterSampler waterSampler);
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
