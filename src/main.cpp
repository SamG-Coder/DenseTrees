#include "dxr_renderer.hpp"
#include "tree.hpp"
#include "rtx_caps.hpp"
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>

namespace {
struct App {
    dense::DxrRenderer renderer; dense::TreeGenerator generator; dense::TreeParameters params=dense::TreeGenerator::parametersFor(dense::TreeSpecies::EnglishOak);dense::GpuCapabilities gpu;
    float yaw=.55f,pitch=.18f,distance=14.0f,sunAzimuth=.55f,windStrength=.72f;
    bool dragging=false; POINT last{}; uint32_t generation=0;
    void regenerate(HWND window,bool nextSeed=true) {
        if(nextSeed) params.seed=5080+generation++;
        const auto start=std::chrono::steady_clock::now();
        auto nodes=generator.grow(params);auto mesh=generator.buildMesh(nodes,params);renderer.setTree(mesh);
        const auto milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        const auto traits=dense::TreeGenerator::traits(params.species);std::wstringstream title;
        title<<L"Dense Trees — "<<traits.name<<L" — "<<nodes.size()-1<<L" branch segments / "<<mesh.leafCount<<L" leaves / "<<static_cast<int>(mesh.totalLeafAreaM2)<<L" m² leaf area / "<<milliseconds<<L" ms — "<<gpu.adapter<<L" / DXR "<<gpu.rayTracingTier/10<<L'.'<<gpu.rayTracingTier%10;
        SetWindowTextW(window,title.str().c_str());
    }
    void setSpecies(HWND window,dense::TreeSpecies species) {
        const uint32_t seed=params.seed;params=dense::TreeGenerator::parametersFor(species,seed);regenerate(window,false);
    }
};
std::unique_ptr<App> app;

LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    switch(message) {
    case WM_DESTROY:PostQuitMessage(0);return 0;
    case WM_SIZE:if(app)app->renderer.resize(LOWORD(lParam),HIWORD(lParam));return 0;
    case WM_LBUTTONDOWN:if(app){app->dragging=true;app->last={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};SetCapture(window);}return 0;
    case WM_LBUTTONUP:if(app){app->dragging=false;ReleaseCapture();}return 0;
    case WM_MOUSEMOVE:if(app&&app->dragging){POINT p{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};app->yaw+=(p.x-app->last.x)*.008f;app->pitch=std::clamp(app->pitch+(p.y-app->last.y)*.006f,-.35f,1.15f);app->last=p;}return 0;
    case WM_MOUSEWHEEL:if(app)app->distance=std::clamp(app->distance-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam))/120.0f,7.0f,30.0f);return 0;
    case WM_KEYDOWN:
        if(wParam==VK_ESCAPE){DestroyWindow(window);return 0;}
        if(app&&(wParam=='R'||wParam==VK_SPACE)){app->regenerate(window);return 0;}
        if(app&&wParam>='1'&&wParam<='5'){app->setSpecies(window,static_cast<dense::TreeSpecies>(wParam-'1'));return 0;}
        if(app&&wParam==VK_LEFT){app->sunAzimuth-=.12f;return 0;}
        if(app&&wParam==VK_RIGHT){app->sunAzimuth+=.12f;return 0;}
        if(app&&wParam=='W'){app->windStrength=app->windStrength>.01f?0.0f:.72f;return 0;}
        break;
    default:break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    SetProcessDPIAware();WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.style=CS_HREDRAW|CS_VREDRAW;wc.lpfnWndProc=windowProc;wc.hInstance=instance;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);wc.lpszClassName=L"DenseTreesWindow";
    if(!RegisterClassExW(&wc))return 1;
    RECT rect{0,0,1440,900};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"Dense Trees",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,instance,nullptr);if(!window)return 2;
    app=std::make_unique<App>();app->gpu=dense::queryGpuCapabilities();RECT client{};GetClientRect(window,&client);if(!app->renderer.initialize(window,client.right,client.bottom)){MessageBoxW(window,app->renderer.error(),L"Dense Trees DXR renderer error",MB_ICONERROR);return 3;}
    app->regenerate(window);ShowWindow(window,show);UpdateWindow(window);
    MSG msg{};bool running=true;while(running){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){running=false;break;}TranslateMessage(&msg);DispatchMessageW(&msg);}if(running)app->renderer.render(app->yaw,app->pitch,app->distance,app->sunAzimuth,app->windStrength);}
    app.reset();return static_cast<int>(msg.wParam);
}
