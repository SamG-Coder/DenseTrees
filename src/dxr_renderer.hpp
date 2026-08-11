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

class DxrRenderer {
public:
    DxrRenderer();~DxrRenderer();DxrRenderer(const DxrRenderer&)=delete;DxrRenderer& operator=(const DxrRenderer&)=delete;
    bool initialize(HWND window,int width,int height);
    void resize(int width,int height);
    void setTree(const TreeMesh& tree);
    void render(float yaw,float pitch,float distance,const DebugRenderSettings& settings,
                const EnvironmentCB& environment);
    const wchar_t* error()const;
    bool ready()const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
