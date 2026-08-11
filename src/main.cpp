#include "dxr_renderer.hpp"
#include "rtx_caps.hpp"
#include "tree.hpp"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

namespace {

constexpr wchar_t mainWindowClass[] = L"DenseTreesWindow";
constexpr wchar_t debugWindowClass[] = L"DenseTreesDebugPanel";
constexpr int sliderPositionMaximum = 1000;
constexpr int resetButtonId = 2100;
constexpr int environmentSectionButtonId = 2190;
constexpr int environmentResetButtonId = 2290;
constexpr int environmentPauseButtonId = 2291;
constexpr int environmentLightningButtonId = 2292;

struct SliderSpec {
    int controlId;
    const wchar_t* label;
    float minimum;
    float maximum;
    float dense::DebugRenderSettings::* value;
    int decimals;
    const wchar_t* suffix;
};

constexpr std::array<SliderSpec,6> sliderSpecs{{
    {2000,L"Grass density",0.0f,6.00f,&dense::DebugRenderSettings::grassDensity,2,L"x"},
    {2001,L"Blade height",.35f,2.50f,&dense::DebugRenderSettings::bladeHeightScale,2,L"x"},
    {2002,L"Ground normal",0.0f,2.00f,&dense::DebugRenderSettings::groundNormalStrength,2,L""},
    {2003,L"Ground detail",0.0f,2.00f,&dense::DebugRenderSettings::groundDetailStrength,2,L""},
    {2004,L"Short grass range",2.0f,128.0f,&dense::DebugRenderSettings::shortGrassDrawDistance,1,L" m"},
    {2005,L"Tall grass range",4.0f,192.0f,&dense::DebugRenderSettings::tallGrassDrawDistance,1,L" m"},
}};

enum class EnvironmentSliderBinding {
    Control,
    TimeOfDay,
    Wetness,
    WindDirection
};

struct EnvironmentSliderSpec {
    int controlId;
    const wchar_t* label;
    float minimum;
    float maximum;
    float dense::EnvironmentControls::* value;
    EnvironmentSliderBinding binding;
    int decimals;
    const wchar_t* suffix;
};

constexpr std::array<EnvironmentSliderSpec,13> environmentSliderSpecs{{
    {2200,L"Time of day",0.0f,24.0f,nullptr,EnvironmentSliderBinding::TimeOfDay,1,L" hrs"},
    {2201,L"Wind speed",0.0f,15.0f,&dense::EnvironmentControls::windSpeed,EnvironmentSliderBinding::Control,2,L""},
    {2202,L"Wind strength",0.0f,3.0f,&dense::EnvironmentControls::windStrength,EnvironmentSliderBinding::Control,2,L""},
    {2203,L"Wind direction",0.0f,360.0f,nullptr,EnvironmentSliderBinding::WindDirection,0,L" deg"},
    {2204,L"Gust frequency",0.0f,8.0f,&dense::EnvironmentControls::windGustFrequency,EnvironmentSliderBinding::Control,2,L""},
    {2205,L"Rain intensity",0.0f,1.0f,&dense::EnvironmentControls::rainIntensity,EnvironmentSliderBinding::Control,2,L""},
    {2206,L"Wetness",0.0f,1.0f,nullptr,EnvironmentSliderBinding::Wetness,2,L""},
    {2207,L"Max puddle coverage",0.0f,1.0f,&dense::EnvironmentControls::maximumPuddleCoverage,EnvironmentSliderBinding::Control,2,L""},
    {2208,L"Fog density",0.0f,.005f,&dense::EnvironmentControls::baseFogDensity,EnvironmentSliderBinding::Control,5,L""},
    {2209,L"Fog height falloff",0.0f,.20f,&dense::EnvironmentControls::fogHeightFalloff,EnvironmentSliderBinding::Control,4,L""},
    {2210,L"Moon phase",0.0f,1.0f,&dense::EnvironmentControls::moonPhase,EnvironmentSliderBinding::Control,2,L""},
    {2211,L"Sun intensity",0.0f,3.0f,&dense::EnvironmentControls::sunIntensityScale,EnvironmentSliderBinding::Control,2,L"x"},
    {2212,L"Moon intensity",0.0f,3.0f,&dense::EnvironmentControls::moonIntensityScale,EnvironmentSliderBinding::Control,2,L"x"},
}};

constexpr size_t environmentTimeOfDayIndex = 0;
constexpr size_t environmentWindStrengthIndex = 2;
constexpr size_t environmentWetnessIndex = 6;

struct App {
    dense::DxrRenderer renderer;
    dense::TreeGenerator generator;
    dense::TreeParameters params =
        dense::TreeGenerator::parametersFor(dense::TreeSpecies::EnglishOak);
    dense::GpuCapabilities gpu;
    dense::DebugRenderSettings debugSettings;
    dense::EnvironmentSimulation environment;

    float yaw = .55f;
    float pitch = .18f;
    float distance = 14.0f;
    bool dragging = false;
    POINT last{};
    uint32_t generation = 0;

    HWND mainWindow{};
    HWND debugPanel{};
    std::array<HWND,sliderSpecs.size()> sliders{};
    std::array<HWND,sliderSpecs.size()> valueLabels{};
    std::array<HWND,environmentSliderSpecs.size()> environmentSliders{};
    std::array<HWND,environmentSliderSpecs.size()> environmentValueLabels{};
    std::vector<HWND> environmentSectionControls;
    HWND environmentSectionButton{};
    HWND environmentPauseButton{};
    HFONT uiFont{};
    HFONT uiFontBold{};
    float uiScale = 1.0f;
    bool debugPanelVisible = false;
    bool environmentSectionExpanded = false;
    float lastNonZeroWindStrength = .72f;
    std::wstring sceneTitle;
    double smoothedFrameMilliseconds = 0.0;
    std::chrono::steady_clock::time_point nextPerformanceTitleUpdate{};
    std::chrono::steady_clock::time_point lastEnvironmentUpdate{};

    ~App() {
        if(debugPanel && IsWindow(debugPanel))DestroyWindow(debugPanel);
        if(uiFontBold)DeleteObject(uiFontBold);
        if(uiFont)DeleteObject(uiFont);
    }

    int scaled(int value) const {
        return static_cast<int>(std::lround(static_cast<float>(value)*uiScale));
    }

    void regenerate(HWND window,bool nextSeed = true) {
        if(nextSeed)params.seed = 5080+generation++;
        const auto start = std::chrono::steady_clock::now();
        auto nodes = generator.grow(params);
        auto mesh = generator.buildMesh(nodes,params);
        renderer.setTree(mesh);
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-start).count();
        const auto traits = dense::TreeGenerator::traits(params.species);
        std::wstringstream title;
        title << L"Dense Trees \u2014 " << traits.name << L" \u2014 " << nodes.size()-1
              << L" branch segments / " << mesh.leafCount << L" leaves / "
              << static_cast<int>(mesh.totalLeafAreaM2) << L" m\u00B2 leaf area / "
              << L"build " << milliseconds << L" ms \u2014 " << gpu.adapter << L" / DXR "
              << gpu.rayTracingTier/10 << L'.' << gpu.rayTracingTier%10;
        sceneTitle=title.str();SetWindowTextW(window,sceneTitle.c_str());
        smoothedFrameMilliseconds=0.0;nextPerformanceTitleUpdate={};
    }

    void updatePerformanceTitle(HWND window,double frameMilliseconds) {
        smoothedFrameMilliseconds=smoothedFrameMilliseconds==0.0?frameMilliseconds:
            smoothedFrameMilliseconds*.88+frameMilliseconds*.12;
        const auto now=std::chrono::steady_clock::now();
        if(now<nextPerformanceTitleUpdate)return;
        nextPerformanceTitleUpdate=now+std::chrono::milliseconds(500);
        const double framesPerSecond=1000.0/std::max(smoothedFrameMilliseconds,.001);
        std::wstringstream title;title.setf(std::ios::fixed);
        title << sceneTitle << L" \u2014 " << std::setprecision(1) << smoothedFrameMilliseconds
              << L" ms frame / " << std::setprecision(0) << framesPerSecond << L" FPS";
        SetWindowTextW(window,title.str().c_str());
    }

    void setSpecies(HWND window,dense::TreeSpecies species) {
        const uint32_t seed = params.seed;
        params = dense::TreeGenerator::parametersFor(species,seed);
        regenerate(window,false);
    }

    bool handleSceneKey(HWND window,WPARAM key) {
        if(key==VK_ESCAPE){DestroyWindow(window);return true;}
        if(key=='R'||key==VK_SPACE){regenerate(window);return true;}
        if(key>='1'&&key<='5'){
            setSpecies(window,static_cast<dense::TreeSpecies>(key-'1'));
            return true;
        }
        if(key==VK_LEFT){environment.controls.sunAzimuthRadians-=.12f;return true;}
        if(key==VK_RIGHT){environment.controls.sunAzimuthRadians+=.12f;return true;}
        if(key=='W'){
            if(environment.controls.windStrength>.01f) {
                lastNonZeroWindStrength=environment.controls.windStrength;
                environment.controls.windStrength=0.0f;
            } else {
                environment.controls.windStrength=lastNonZeroWindStrength;
            }
            synchronizeEnvironmentSlider(environmentWindStrengthIndex);
            return true;
        }
        return false;
    }

    void setControlFont(HWND control,bool bold = false) const {
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(bold?uiFontBold:uiFont),TRUE);
    }

    int sliderPosition(size_t index) const {
        const SliderSpec& spec = sliderSpecs[index];
        const float value = debugSettings.*(spec.value);
        const float normalized = std::clamp((value-spec.minimum)/(spec.maximum-spec.minimum),0.0f,1.0f);
        return static_cast<int>(std::lround(normalized*sliderPositionMaximum));
    }

    void updateValueLabel(size_t index) const {
        if(!valueLabels[index])return;
        const SliderSpec& spec = sliderSpecs[index];
        std::wstringstream value;
        value << std::fixed << std::setprecision(spec.decimals)
              << debugSettings.*(spec.value) << spec.suffix;
        SetWindowTextW(valueLabels[index],value.str().c_str());
    }

    void synchronizeSlider(size_t index) const {
        if(sliders[index])SendMessageW(sliders[index],TBM_SETPOS,TRUE,sliderPosition(index));
        updateValueLabel(index);
    }

    void synchronizeDebugControls() const {
        for(size_t index=0;index<sliderSpecs.size();++index)synchronizeSlider(index);
    }

    float environmentValue(size_t index) const {
        const EnvironmentSliderSpec& spec=environmentSliderSpecs[index];
        switch(spec.binding) {
        case EnvironmentSliderBinding::Control:
            return environment.controls.*(spec.value);
        case EnvironmentSliderBinding::TimeOfDay:
            return environment.state.timeOfDay;
        case EnvironmentSliderBinding::Wetness:
            return environment.state.wetnessFactor;
        case EnvironmentSliderBinding::WindDirection: {
            constexpr float radiansToDegrees=57.29577951308232f;
            float degrees=std::atan2(environment.controls.windDirection.y,
                                     environment.controls.windDirection.x)*radiansToDegrees;
            return degrees<0.0f?degrees+360.0f:degrees;
        }
        }
        return 0.0f;
    }

    void setEnvironmentValue(size_t index,float value) {
        const EnvironmentSliderSpec& spec=environmentSliderSpecs[index];
        value=std::clamp(value,spec.minimum,spec.maximum);
        switch(spec.binding) {
        case EnvironmentSliderBinding::Control:
            environment.controls.*(spec.value)=value;
            break;
        case EnvironmentSliderBinding::TimeOfDay:
            environment.state.timeOfDay=value;
            break;
        case EnvironmentSliderBinding::Wetness:
            environment.state.wetnessFactor=value;
            break;
        case EnvironmentSliderBinding::WindDirection: {
            constexpr float degreesToRadians=.017453292519943295f;
            const float radians=value*degreesToRadians;
            environment.controls.windDirection={std::cos(radians),std::sin(radians)};
            break;
        }
        }
    }

    int environmentSliderPosition(size_t index) const {
        const EnvironmentSliderSpec& spec=environmentSliderSpecs[index];
        const float normalized=std::clamp(
            (environmentValue(index)-spec.minimum)/(spec.maximum-spec.minimum),0.0f,1.0f);
        return static_cast<int>(std::lround(normalized*sliderPositionMaximum));
    }

    void updateEnvironmentValueLabel(size_t index) const {
        if(!environmentValueLabels[index])return;
        const EnvironmentSliderSpec& spec=environmentSliderSpecs[index];
        std::wstringstream value;
        value << std::fixed << std::setprecision(spec.decimals)
              << environmentValue(index) << spec.suffix;
        SetWindowTextW(environmentValueLabels[index],value.str().c_str());
    }

    void synchronizeEnvironmentSlider(size_t index) const {
        if(environmentSliders[index]&&GetCapture()!=environmentSliders[index]) {
            SendMessageW(environmentSliders[index],TBM_SETPOS,TRUE,
                         environmentSliderPosition(index));
        }
        updateEnvironmentValueLabel(index);
    }

    void updateEnvironmentPauseButton() const {
        if(environmentPauseButton)SetWindowTextW(environmentPauseButton,
            environment.controls.advanceTime?L"Pause time":L"Resume time");
    }

    void synchronizeEnvironmentControls() const {
        for(size_t index=0;index<environmentSliderSpecs.size();++index)
            synchronizeEnvironmentSlider(index);
        updateEnvironmentPauseButton();
    }

    void updateEnvironment() {
        const auto now=std::chrono::steady_clock::now();
        const float deltaTime=lastEnvironmentUpdate.time_since_epoch().count()==0?0.0f:
            std::chrono::duration<float>(now-lastEnvironmentUpdate).count();
        lastEnvironmentUpdate=now;
        environment.update(deltaTime);
        if(debugPanelVisible&&environmentSectionExpanded) {
            synchronizeEnvironmentSlider(environmentTimeOfDayIndex);
            synchronizeEnvironmentSlider(environmentWetnessIndex);
        }
    }

    void buildDebugControls(HWND panel) {
        const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(panel,GWLP_HINSTANCE));
        const int width = scaled(344);
        HWND note = CreateWindowExW(0,L"STATIC",
            L"Live renderer controls. Press F1 to hide this window.",
            WS_CHILD|WS_VISIBLE|SS_LEFT,scaled(12),scaled(10),width-scaled(24),scaled(20),
            panel,nullptr,instance,nullptr);
        setControlFont(note,true);

        const int firstRow = scaled(39);
        const int rowHeight = scaled(45);
        for(size_t index=0;index<sliderSpecs.size();++index) {
            const SliderSpec& spec = sliderSpecs[index];
            const int rowY = firstRow+static_cast<int>(index)*rowHeight;
            HWND label = CreateWindowExW(0,L"STATIC",spec.label,WS_CHILD|WS_VISIBLE|SS_LEFT,
                scaled(12),rowY,width-scaled(112),scaled(18),panel,nullptr,instance,nullptr);
            setControlFont(label);
            valueLabels[index] = CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_RIGHT,
                width-scaled(100),rowY,scaled(86),scaled(18),panel,nullptr,instance,nullptr);
            setControlFont(valueLabels[index],true);
            sliders[index] = CreateWindowExW(0,TRACKBAR_CLASSW,L"",
                WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_AUTOTICKS|TBS_TOOLTIPS,
                scaled(8),rowY+scaled(17),width-scaled(16),scaled(27),panel,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(spec.controlId)),instance,nullptr);
            SendMessageW(sliders[index],TBM_SETRANGE,TRUE,MAKELONG(0,sliderPositionMaximum));
            SendMessageW(sliders[index],TBM_SETTICFREQ,250,0);
            SendMessageW(sliders[index],TBM_SETPAGESIZE,0,50);
        }

        const int buttonY = firstRow+static_cast<int>(sliderSpecs.size())*rowHeight+scaled(4);
        HWND reset = CreateWindowExW(0,L"BUTTON",L"Reset defaults",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,scaled(12),buttonY,
            scaled(122),scaled(27),panel,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(resetButtonId)),instance,nullptr);
        setControlFont(reset);
        HWND rangeNote = CreateWindowExW(0,L"STATIC",L"Short range is kept within tall range.",
            WS_CHILD|WS_VISIBLE|SS_RIGHT,width-scaled(202),buttonY+scaled(5),scaled(188),scaled(18),
            panel,nullptr,instance,nullptr);
        setControlFont(rangeNote);

        environmentSectionButton=CreateWindowExW(0,L"BUTTON",L"Environment & Atmosphere  >",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,scaled(12),buttonY+scaled(36),
            width-scaled(24),scaled(28),panel,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(environmentSectionButtonId)),instance,nullptr);
        setControlFont(environmentSectionButton,true);

        const int environmentX=scaled(356);
        const int environmentWidth=scaled(344);
        for(size_t index=0;index<environmentSliderSpecs.size();++index) {
            const EnvironmentSliderSpec& spec=environmentSliderSpecs[index];
            const int rowY=firstRow+static_cast<int>(index)*rowHeight;
            HWND label=CreateWindowExW(0,L"STATIC",spec.label,WS_CHILD|SS_LEFT,
                environmentX+scaled(12),rowY,environmentWidth-scaled(112),scaled(18),
                panel,nullptr,instance,nullptr);
            setControlFont(label);
            environmentSectionControls.push_back(label);

            environmentValueLabels[index]=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|SS_RIGHT,
                environmentX+environmentWidth-scaled(100),rowY,scaled(86),scaled(18),
                panel,nullptr,instance,nullptr);
            setControlFont(environmentValueLabels[index],true);
            environmentSectionControls.push_back(environmentValueLabels[index]);

            environmentSliders[index]=CreateWindowExW(0,TRACKBAR_CLASSW,L"",
                WS_CHILD|TBS_HORZ|TBS_AUTOTICKS|TBS_TOOLTIPS,
                environmentX+scaled(8),rowY+scaled(17),environmentWidth-scaled(16),scaled(27),
                panel,reinterpret_cast<HMENU>(static_cast<INT_PTR>(spec.controlId)),instance,nullptr);
            SendMessageW(environmentSliders[index],TBM_SETRANGE,TRUE,
                         MAKELONG(0,sliderPositionMaximum));
            SendMessageW(environmentSliders[index],TBM_SETTICFREQ,250,0);
            SendMessageW(environmentSliders[index],TBM_SETPAGESIZE,0,50);
            environmentSectionControls.push_back(environmentSliders[index]);
        }

        const int environmentButtonY=firstRow+
            static_cast<int>(environmentSliderSpecs.size())*rowHeight+scaled(4);
        HWND environmentReset=CreateWindowExW(0,L"BUTTON",L"Reset environment",
            WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,environmentX+scaled(12),environmentButtonY,
            scaled(112),scaled(27),panel,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(environmentResetButtonId)),instance,nullptr);
        setControlFont(environmentReset);
        environmentSectionControls.push_back(environmentReset);

        environmentPauseButton=CreateWindowExW(0,L"BUTTON",L"Pause time",
            WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,environmentX+scaled(130),environmentButtonY,
            scaled(92),scaled(27),panel,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(environmentPauseButtonId)),instance,nullptr);
        setControlFont(environmentPauseButton);
        environmentSectionControls.push_back(environmentPauseButton);

        HWND lightning=CreateWindowExW(0,L"BUTTON",L"Trigger lightning",
            WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,environmentX+scaled(228),environmentButtonY,
            scaled(104),scaled(27),panel,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(environmentLightningButtonId)),instance,nullptr);
        setControlFont(lightning);
        environmentSectionControls.push_back(lightning);

        synchronizeDebugControls();
        synchronizeEnvironmentControls();
    }

    bool createDebugPanelWindow(HWND owner,HINSTANCE instance) {
        mainWindow = owner;
        HDC deviceContext = GetDC(owner);
        const int dpi = deviceContext?GetDeviceCaps(deviceContext,LOGPIXELSY):96;
        if(deviceContext)ReleaseDC(owner,deviceContext);
        uiScale = std::max(.75f,static_cast<float>(dpi)/96.0f);
        const int fontHeight = -MulDiv(9,dpi,72);
        uiFont = CreateFontW(fontHeight,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        uiFontBold = CreateFontW(fontHeight,0,0,0,600,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");

        constexpr DWORD style = WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU;
        constexpr DWORD extendedStyle = WS_EX_TOOLWINDOW|WS_EX_DLGMODALFRAME;
        RECT bounds{0,0,scaled(344),scaled(390)};
        AdjustWindowRectEx(&bounds,style,FALSE,extendedStyle);
        debugPanel = CreateWindowExW(extendedStyle,debugWindowClass,L"Renderer controls - F1",
            style,CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,
            owner,nullptr,instance,this);
        if(!debugPanel)return false;
        positionDebugPanel();
        return true;
    }

    void setEnvironmentSectionExpanded(bool expanded) {
        environmentSectionExpanded=expanded;
        for(HWND control:environmentSectionControls)
            if(control)ShowWindow(control,expanded?SW_SHOW:SW_HIDE);
        if(environmentSectionButton)SetWindowTextW(environmentSectionButton,
            expanded?L"Environment & Atmosphere  <":L"Environment & Atmosphere  >");
        if(!debugPanel||!IsWindow(debugPanel))return;

        const int clientWidth=scaled(expanded?700:344);
        const int clientHeight=scaled(expanded?668:390);
        RECT bounds{0,0,clientWidth,clientHeight};
        const DWORD style=static_cast<DWORD>(GetWindowLongPtrW(debugPanel,GWL_STYLE));
        const DWORD extendedStyle=static_cast<DWORD>(GetWindowLongPtrW(debugPanel,GWL_EXSTYLE));
        AdjustWindowRectEx(&bounds,style,FALSE,extendedStyle);
        SetWindowPos(debugPanel,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        if(expanded)synchronizeEnvironmentControls();
        positionDebugPanel();
    }

    void toggleEnvironmentSection() {
        setEnvironmentSectionExpanded(!environmentSectionExpanded);
    }

    void positionDebugPanel() const {
        if(!mainWindow||!debugPanel||!IsWindow(debugPanel))return;
        RECT ownerBounds{},panelBounds{};
        GetWindowRect(mainWindow,&ownerBounds);
        GetWindowRect(debugPanel,&panelBounds);
        const int width = panelBounds.right-panelBounds.left;
        const int height = panelBounds.bottom-panelBounds.top;
        MONITORINFO monitor{};
        monitor.cbSize=sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(mainWindow,MONITOR_DEFAULTTONEAREST),&monitor);
        const int desiredX = ownerBounds.right-width-scaled(18);
        const int desiredY = ownerBounds.top+scaled(54);
        const int workLeft=static_cast<int>(monitor.rcWork.left);
        const int workTop=static_cast<int>(monitor.rcWork.top);
        const int workRight=static_cast<int>(monitor.rcWork.right);
        const int workBottom=static_cast<int>(monitor.rcWork.bottom);
        const int x = std::max(workLeft,std::min(desiredX,workRight-width));
        const int y = std::max(workTop,std::min(desiredY,workBottom-height));
        SetWindowPos(debugPanel,HWND_TOP,x,y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
    }

    void showDebugPanel(bool visible) {
        if(!debugPanel||!IsWindow(debugPanel))return;
        debugPanelVisible = visible;
        if(visible) {
            positionDebugPanel();
            synchronizeDebugControls();
            synchronizeEnvironmentControls();
            ShowWindow(debugPanel,SW_SHOWNOACTIVATE);
        } else {
            ShowWindow(debugPanel,SW_HIDE);
            if(mainWindow)SetFocus(mainWindow);
        }
    }

    void toggleDebugPanel() {
        showDebugPanel(!debugPanelVisible);
    }

    void updateSettingFromSlider(HWND slider) {
        for(size_t index=0;index<sliders.size();++index) {
            if(sliders[index]!=slider)continue;
            const SliderSpec& spec = sliderSpecs[index];
            const int position = static_cast<int>(SendMessageW(slider,TBM_GETPOS,0,0));
            const float normalized = static_cast<float>(position)/sliderPositionMaximum;
            debugSettings.*(spec.value) = spec.minimum+(spec.maximum-spec.minimum)*normalized;

            constexpr size_t shortRangeIndex = 4;
            constexpr size_t tallRangeIndex = 5;
            if(index==shortRangeIndex &&
               debugSettings.shortGrassDrawDistance>debugSettings.tallGrassDrawDistance) {
                debugSettings.tallGrassDrawDistance = debugSettings.shortGrassDrawDistance;
                synchronizeSlider(tallRangeIndex);
            } else if(index==tallRangeIndex &&
                      debugSettings.tallGrassDrawDistance<debugSettings.shortGrassDrawDistance) {
                debugSettings.shortGrassDrawDistance = debugSettings.tallGrassDrawDistance;
                synchronizeSlider(shortRangeIndex);
            }
            updateValueLabel(index);
            return;
        }
        for(size_t index=0;index<environmentSliders.size();++index) {
            if(environmentSliders[index]!=slider)continue;
            const EnvironmentSliderSpec& spec=environmentSliderSpecs[index];
            const int position=static_cast<int>(SendMessageW(slider,TBM_GETPOS,0,0));
            const float normalized=static_cast<float>(position)/sliderPositionMaximum;
            setEnvironmentValue(index,
                spec.minimum+(spec.maximum-spec.minimum)*normalized);
            updateEnvironmentValueLabel(index);
            return;
        }
    }

    void resetDebugSettings() {
        debugSettings = {};
        synchronizeDebugControls();
    }

    void resetEnvironment() {
        environment.controls={};
        environment.reset(dense::EnvironmentState{});
        lastNonZeroWindStrength=environment.controls.windStrength;
        lastEnvironmentUpdate={};
        synchronizeEnvironmentControls();
    }

    void toggleEnvironmentTime() {
        environment.controls.advanceTime=!environment.controls.advanceTime;
        updateEnvironmentPauseButton();
    }

    void triggerEnvironmentLightning() {
        const auto now=std::chrono::steady_clock::now();
        const float elapsed=lastEnvironmentUpdate.time_since_epoch().count()==0?0.0f:
            std::chrono::duration<float>(now-lastEnvironmentUpdate).count();
        // First account for the render interval that elapsed before this UI
        // event; it predates the lightning and must not decay the new flash.
        environment.update(elapsed);
        environment.triggerLightning();
        // At the current path-tracing workload a frame can exceed one second.
        // Publish the peak before real-time decay so a click always produces at
        // least one visible frame instead of expiring between UI polls.
        environment.update(0.0f);
        lastEnvironmentUpdate=now;
    }
};

std::unique_ptr<App> app;

LRESULT CALLBACK debugPanelProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    App* owner = reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        const auto* created = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        owner = static_cast<App*>(created->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(owner));
    }
    switch(message) {
    case WM_CREATE:
        if(owner)owner->buildDebugControls(window);
        return 0;
    case WM_HSCROLL:
        if(owner&&lParam)owner->updateSettingFromSlider(reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_COMMAND:
        if(owner) {
            switch(LOWORD(wParam)) {
            case resetButtonId:owner->resetDebugSettings();return 0;
            case environmentSectionButtonId:owner->toggleEnvironmentSection();return 0;
            case environmentResetButtonId:owner->resetEnvironment();return 0;
            case environmentPauseButtonId:owner->toggleEnvironmentTime();return 0;
            case environmentLightningButtonId:owner->triggerEnvironmentLightning();return 0;
            default:break;
            }
        }
        break;
    case WM_CLOSE:
        if(owner)owner->showDebugPanel(false);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_NCDESTROY:
        if(owner){owner->debugPanel=nullptr;owner->debugPanelVisible=false;}
        SetWindowLongPtrW(window,GWLP_USERDATA,0);
        break;
    default:break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    switch(message) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if(app&&wParam!=SIZE_MINIMIZED) {
            app->renderer.resize(LOWORD(lParam),HIWORD(lParam));
            app->positionDebugPanel();
        }
        return 0;
    case WM_MOVE:
        if(app)app->positionDebugPanel();
        return 0;
    case WM_LBUTTONDOWN:
        if(app) {
            SetFocus(window);
            app->dragging=true;
            app->last={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            SetCapture(window);
        }
        return 0;
    case WM_LBUTTONUP:
        if(app){app->dragging=false;ReleaseCapture();}
        return 0;
    case WM_MOUSEMOVE:
        if(app&&app->dragging) {
            POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            app->yaw+=(point.x-app->last.x)*.008f;
            app->pitch=std::clamp(app->pitch+(point.y-app->last.y)*.006f,-.35f,1.15f);
            app->last=point;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if(app)app->distance=std::clamp(app->distance-
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam))/120.0f,7.0f,30.0f);
        return 0;
    case WM_KEYDOWN:
        if(app&&wParam==VK_F1){
            if((lParam&(static_cast<LPARAM>(1)<<30))==0)app->toggleDebugPanel();
            return 0;
        }
        if(app&&app->handleSceneKey(window,wParam))return 0;
        break;
    default:break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls),ICC_BAR_CLASSES|ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&commonControls);

    WNDCLASSEXW mainClass{};
    mainClass.cbSize=sizeof(mainClass);
    mainClass.style=CS_HREDRAW|CS_VREDRAW;
    mainClass.lpfnWndProc=windowProc;
    mainClass.hInstance=instance;
    mainClass.hCursor=LoadCursor(nullptr,IDC_ARROW);
    mainClass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    mainClass.lpszClassName=mainWindowClass;
    if(!RegisterClassExW(&mainClass))return 1;

    WNDCLASSEXW debugClass{};
    debugClass.cbSize=sizeof(debugClass);
    debugClass.style=CS_HREDRAW|CS_VREDRAW;
    debugClass.lpfnWndProc=debugPanelProc;
    debugClass.hInstance=instance;
    debugClass.hCursor=LoadCursor(nullptr,IDC_ARROW);
    debugClass.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);
    debugClass.lpszClassName=debugWindowClass;
    if(!RegisterClassExW(&debugClass))return 1;

    SetProcessDPIAware();
    RECT rectangle{0,0,1440,900};
    AdjustWindowRect(&rectangle,WS_OVERLAPPEDWINDOW,FALSE);
    HWND window = CreateWindowExW(0,mainClass.lpszClassName,L"Dense Trees",
        WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,
        rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,
        nullptr,nullptr,instance,nullptr);
    if(!window)return 2;

    app=std::make_unique<App>();
    app->gpu=dense::queryGpuCapabilities();
    RECT client{};
    GetClientRect(window,&client);
    if(!app->renderer.initialize(window,client.right,client.bottom)) {
        MessageBoxW(window,app->renderer.error(),L"Dense Trees DXR renderer error",MB_ICONERROR);
        return 3;
    }
    if(!app->createDebugPanelWindow(window,instance)) {
        MessageBoxW(window,L"The renderer controls could not be created.",
            L"Dense Trees debug controls",MB_ICONWARNING);
    }
    app->regenerate(window);
    ShowWindow(window,show);
    UpdateWindow(window);
    app->showDebugPanel(true);

    MSG message{};
    bool running=true;
    while(running) {
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT){running=false;break;}
            if(app&&message.message==WM_KEYDOWN&&message.wParam==VK_F1) {
                if((message.lParam&(static_cast<LPARAM>(1)<<30))==0)app->toggleDebugPanel();
                continue;
            }
            if(app&&message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE&&
               app->debugPanel&&GetAncestor(message.hwnd,GA_ROOT)==app->debugPanel) {
                app->showDebugPanel(false);
                continue;
            }
            if(app&&app->debugPanel&&IsWindowVisible(app->debugPanel)&&
               GetAncestor(message.hwnd,GA_ROOT)==app->debugPanel&&
               IsDialogMessageW(app->debugPanel,&message))continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if(running) {
            const auto frameStart=std::chrono::steady_clock::now();
            app->updateEnvironment();
            app->renderer.render(app->yaw,app->pitch,app->distance,app->debugSettings,
                                 app->environment.constants());
            const double frameMilliseconds=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-frameStart).count();
            app->updatePerformanceTitle(window,frameMilliseconds);
        }
    }
    app.reset();
    return static_cast<int>(message.wParam);
}
