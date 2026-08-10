#pragma once
#include "tree.hpp"
#include <windows.h>
#include <memory>

namespace dense {
class DxrRenderer {
public:
    DxrRenderer();~DxrRenderer();DxrRenderer(const DxrRenderer&)=delete;DxrRenderer& operator=(const DxrRenderer&)=delete;
    bool initialize(HWND window,int width,int height);
    void resize(int width,int height);
    void setTree(const TreeMesh& tree);
    void render(float yaw,float pitch,float distance,float sunAzimuth,float windStrength=.72f);
    const wchar_t* error()const;
    bool ready()const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
