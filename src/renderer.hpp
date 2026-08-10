#pragma once

#include "tree.hpp"
#include <windows.h>
#include <memory>

namespace dense {

class Renderer {
public:
    Renderer(); ~Renderer();
    Renderer(const Renderer&)=delete; Renderer& operator=(const Renderer&)=delete;
    bool initialize(HWND window, int width, int height);
    void resize(int width, int height);
    void setTree(const TreeMesh& tree);
    void render(float yaw, float pitch, float distance, float sunAzimuth);
    const wchar_t* error() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

}
