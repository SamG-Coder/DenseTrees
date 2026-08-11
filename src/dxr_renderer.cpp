#include "dxr_renderer.hpp"
#include "environment.hpp"
#include "ground_texture.hpp"
#include "math.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace dense {
namespace {
template<class T>void release(T*&p){if(p){p->Release();p=nullptr;}}
constexpr UINT64 alignUp(UINT64 value,UINT64 alignment){return(value+alignment-1)&~(alignment-1);}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_UNKNOWN;h.MemoryPoolPreference=D3D12_MEMORY_POOL_UNKNOWN;h.CreationNodeMask=1;h.VisibleNodeMask=1;return h;}
D3D12_RESOURCE_DESC bufferDesc(UINT64 bytes,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=DXGI_FORMAT_UNKNOWN;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;return d;}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource*r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=r;b.Transition.StateBefore=before;b.Transition.StateAfter=after;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;return b;}
float fade(float t){return t*t*t*(t*(t*6-15)+10);}
float smoothRange(float low,float high,float value){const float t=clamp((value-low)/(high-low),0,1);return t*t*(3-2*t);}
uint32_t gridHash(int x,int y){uint32_t h=static_cast<uint32_t>(x)*0x8da6b343u^static_cast<uint32_t>(y)*0xd8163841u^0xcb1ab31fu;h^=h>>13;h*=0x85ebca6bu;h^=h>>16;return h;}
float tileNoise(float u,float v,int cells){const float x=u*cells,y=v*cells;const int ix=static_cast<int>(std::floor(x)),iy=static_cast<int>(std::floor(y));const float fx=fade(x-ix),fy=fade(y-iy);auto sample=[&](int px,int py){px=(px%cells+cells)%cells;py=(py%cells+cells)%cells;return static_cast<float>(gridHash(px,py)>>8)*(1.0f/16777216.0f);};const float a=sample(ix,iy),b=sample(ix+1,iy),c=sample(ix,iy+1),d=sample(ix+1,iy+1);return(a+(b-a)*fx)+((c+(d-c)*fx)-(a+(b-a)*fx))*fy;}
struct CellularSample{float edge{},value{};};
CellularSample cellularPlate(float u,float v,int cellsX,int cellsY,uint32_t salt){
    u-=std::floor(u);v-=std::floor(v);const float gx=u*cellsX,gy=v*cellsY;const int ix=static_cast<int>(std::floor(gx)),iy=static_cast<int>(std::floor(gy));float nearest=1e9f,second=1e9f,bestValue=.5f;
    for(int oy=-1;oy<=1;++oy)for(int ox=-1;ox<=1;++ox){const int cellX=ix+ox,cellY=iy+oy,wrappedX=(cellX%cellsX+cellsX)%cellsX,wrappedY=(cellY%cellsY+cellsY)%cellsY;const uint32_t h0=gridHash(wrappedX+static_cast<int>(salt*17u),wrappedY-static_cast<int>(salt*11u)),h1=gridHash(wrappedX-static_cast<int>(salt*7u),wrappedY+static_cast<int>(salt*23u));const float jitterX=.12f+.76f*static_cast<float>((h0>>8)&0xffffu)/65535.0f,jitterY=.10f+.80f*static_cast<float>((h1>>8)&0xffffu)/65535.0f;const float dx=cellX+jitterX-gx,dy=cellY+jitterY-gy,distance=dx*dx+dy*dy;if(distance<nearest){second=nearest;nearest=distance;bestValue=static_cast<float>((h0>>24)&255u)/255.0f;}else if(distance<second)second=distance;}
    return {std::sqrt(second)-std::sqrt(nearest),bestValue};
}
std::vector<uint32_t> makeOakBarkNormal(UINT width,UINT height){
    const size_t count=static_cast<size_t>(width)*height;std::vector<float> heightField(count),cavityField(count),toneField(count);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
        const float u=(x+.5f)/width,v=(y+.5f)/height,broad=tileNoise(u,v,5),medium=tileNoise(u,v,19),fine=tileNoise(u,v,93),grain=tileNoise(u,v,191);const float warpedU=u+(broad-.5f)*.034f+(medium-.5f)*.009f,warpedV=v+(broad-.5f)*.026f+(medium-.5f)*.007f;const CellularSample primary=cellularPlate(warpedU,warpedV,42,5,3),secondary=cellularPlate(warpedU+(fine-.5f)*.006f,warpedV,14,30,19);const float primaryFissure=std::pow(1-smoothRange(.035f,.145f,primary.edge),1.18f),secondaryFissure=std::pow(1-smoothRange(.018f,.082f,secondary.edge),1.30f)*(1-primaryFissure*.72f),plate=std::pow(smoothRange(.028f,.29f,primary.edge),.62f),fiber=std::abs(std::sin(2*pi*(u*167+(medium-.5f)*2.4f+(fine-.5f)*.55f)));const size_t index=static_cast<size_t>(y)*width+x;
        heightField[index]=.0025f*(broad-.5f)+.0045f*plate-.0155f*primaryFissure-.0048f*secondaryFissure*plate+.00115f*(fine-.5f)+.00038f*(grain-.5f)+.00028f*(fiber-.5f);
        cavityField[index]=clamp(.90f*primaryFissure+.42f*secondaryFissure+.08f*std::max(0.0f,.5f-grain),0,1);
        toneField[index]=clamp(.50f+.22f*(primary.value-.5f)+.14f*(broad-.5f)+.10f*(medium-.5f)+.06f*(fine-.5f)-.12f*secondaryFissure,0,1);
    }
    std::vector<uint32_t> pixels(count);auto encode=[](float value){return static_cast<uint32_t>(clamp((value*.5f+.5f)*255.0f,0,255));};const float texelWidth=4.25f/width,texelHeight=2.40f/height;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const size_t left=static_cast<size_t>(y)*width+(x+width-1)%width,right=static_cast<size_t>(y)*width+(x+1)%width,down=static_cast<size_t>((y+height-1)%height)*width+x,up=static_cast<size_t>((y+1)%height)*width+x,index=static_cast<size_t>(y)*width+x;const float dhdx=(heightField[right]-heightField[left])/(2*texelWidth),dhdy=(heightField[up]-heightField[down])/(2*texelHeight);const float inverse=1/std::sqrt(dhdx*dhdx*.82f*.82f+dhdy*dhdy*.95f*.95f+1);const float nx=-dhdx*.82f*inverse,ny=-dhdy*.95f*inverse;const uint32_t r=encode(nx),g=encode(ny),b=static_cast<uint32_t>(clamp(cavityField[index]*255.0f,0,255)),a=static_cast<uint32_t>(clamp(toneField[index]*255.0f,0,255));pixels[index]=r|(g<<8)|(b<<16)|(a<<24);}
    return pixels;
}
std::vector<std::vector<uint32_t>> makeNormalMipChain(std::vector<uint32_t> top,UINT width,UINT height,UINT levels){
    std::vector<std::vector<uint32_t>> result;result.reserve(levels);result.push_back(std::move(top));
    UINT previousWidth=width,previousHeight=height;
    for(UINT level=1;level<levels;++level){
        const UINT currentWidth=std::max(1u,previousWidth/2),currentHeight=std::max(1u,previousHeight/2);std::vector<uint32_t> current(static_cast<size_t>(currentWidth)*currentHeight);const auto&previous=result.back();
        for(UINT y=0;y<currentHeight;++y)for(UINT x=0;x<currentWidth;++x){
            float nx=0,ny=0,nz=0,cavity=0,maxCavity=0,tone=0;
            for(UINT oy=0;oy<2;++oy)for(UINT ox=0;ox<2;++ox){const uint32_t packed=previous[static_cast<size_t>(std::min(previousHeight-1,y*2+oy))*previousWidth+std::min(previousWidth-1,x*2+ox)];const float sampleX=((packed&255)/255.0f)*2-1,sampleY=(((packed>>8)&255)/255.0f)*2-1,sampleZ=std::sqrt(std::max(0.0f,1-sampleX*sampleX-sampleY*sampleY)),sampleCavity=((packed>>16)&255)/255.0f;nx+=sampleX;ny+=sampleY;nz+=sampleZ;cavity+=sampleCavity;maxCavity=std::max(maxCavity,sampleCavity);tone+=((packed>>24)&255)/255.0f;}
            const float inverse=1/std::sqrt(nx*nx+ny*ny+nz*nz);nx*=inverse;ny*=inverse;cavity=level<=5?cavity*.1625f+maxCavity*.35f:cavity*.25f;tone*=.25f;auto encode=[](float value){return static_cast<uint32_t>(clamp((value*.5f+.5f)*255.0f,0,255));};const uint32_t r=encode(nx),g=encode(ny),b=static_cast<uint32_t>(clamp(cavity*255.0f,0,255)),a=static_cast<uint32_t>(clamp(tone*255.0f,0,255));current[static_cast<size_t>(y)*currentWidth+x]=r|(g<<8)|(b<<16)|(a<<24);
        }
        result.push_back(std::move(current));previousWidth=currentWidth;previousHeight=currentHeight;
    }
    return result;
}
}

struct DxrRenderer::Impl{
    HWND window{};int width=1,height=1;std::wstring lastError;bool initialized=false;UINT frameIndex=0;
    CameraView lastView{};PlayerLocalLight lastLocalLight{};
    bool haveLastView=false,haveLastLocalLight=false;
    DebugRenderSettings lastDebugSettings{};bool haveLastDebugSettings=false;
    EnvironmentCB lastEnvironment{};bool haveLastEnvironment=false;
    EnvironmentMesh environment{};
    IDXGIFactory6*factory{};IDXGISwapChain3*swap{};ID3D12Device5*device{};ID3D12CommandQueue*queue{};ID3D12CommandAllocator*allocator{};ID3D12GraphicsCommandList4*list{};
    ID3D12Fence*fence{};HANDLE fenceEvent{};UINT64 fenceValue{};ID3D12DescriptorHeap*rtvHeap{};ID3D12DescriptorHeap*dsvHeap{};ID3D12DescriptorHeap*gpuHeap{};UINT rtvSize{},srvSize{};ID3D12Resource*backBuffers[2]{};
    ID3D12RootSignature*root{};ID3D12StateObject*state{};ID3D12StateObjectProperties*stateProps{};
    ID3D12RootSignature*grassRoot{};ID3D12PipelineState*grassPipeline{};
    ID3D12RootSignature*treeWindRoot{};ID3D12PipelineState*treeWindPipeline{};
    ID3D12Resource*output{};ID3D12Resource*accumulation{};ID3D12Resource*grassDepth{};ID3D12Resource*barkNormal{};ID3D12Resource*groundAlbedo{};ID3D12Resource*groundNormal{};ID3D12Resource*cameraBuffer{};void*cameraMapped{};ID3D12Resource*environmentBuffer{};void*environmentMapped{};
    ID3D12Resource*vertexBuffer{};ID3D12Resource*baseTreeVertexBuffer{};
    ID3D12Resource*indexBuffer{};ID3D12Resource*blas{};ID3D12Resource*staticBlas{};
    ID3D12Resource*tlas{};ID3D12Resource*blasScratch{};ID3D12Resource*staticBlasScratch{};
    ID3D12Resource*tlasScratch{};ID3D12Resource*instanceBuffer{};
    ID3D12Resource*grassBuffer{};ID3D12Resource*visibleGrassBuffer{};
    ID3D12Resource*grassBlas{};ID3D12Resource*grassBlasScratch{};
    void*visibleGrassMapped{};
    ID3D12Resource*raygenTable{};ID3D12Resource*missTable{};ID3D12Resource*hitTable{};
    UINT vertexCount{},indexCount{},treeVertexCount{},treeIndexCount{},grassPatchCount{};
    float treeHeight=1.0f;bool treeWindWasActive=false;
    UINT visibleNearGrassPatchCount{},visibleFarGrassPatchCount{};

    ~Impl(){wait();if(cameraBuffer&&cameraMapped)cameraBuffer->Unmap(0,nullptr);if(environmentBuffer&&environmentMapped)environmentBuffer->Unmap(0,nullptr);if(visibleGrassBuffer&&visibleGrassMapped)visibleGrassBuffer->Unmap(0,nullptr);release(hitTable);release(missTable);release(raygenTable);release(instanceBuffer);release(tlasScratch);release(grassBlasScratch);release(staticBlasScratch);release(blasScratch);release(tlas);release(grassBlas);release(staticBlas);release(blas);release(visibleGrassBuffer);release(grassBuffer);release(indexBuffer);release(baseTreeVertexBuffer);release(vertexBuffer);release(environmentBuffer);release(cameraBuffer);release(groundNormal);release(groundAlbedo);release(barkNormal);release(grassDepth);release(accumulation);release(output);release(treeWindPipeline);release(treeWindRoot);release(grassPipeline);release(grassRoot);release(stateProps);release(state);release(root);for(auto&b:backBuffers)release(b);release(gpuHeap);release(dsvHeap);release(rtvHeap);release(list);release(allocator);release(fence);release(queue);release(swap);release(device);release(factory);if(fenceEvent)CloseHandle(fenceEvent);}
    bool fail(HRESULT hr,const wchar_t*message){wchar_t text[320];wsprintfW(text,L"%s (HRESULT 0x%08X)",message,static_cast<unsigned>(hr));lastError=text;return false;}
    void wait(){if(!queue||!fence)return;const UINT64 value=++fenceValue;if(SUCCEEDED(queue->Signal(fence,value))&&fence->GetCompletedValue()<value){fence->SetEventOnCompletion(value,fenceEvent);WaitForSingleObject(fenceEvent,INFINITE);}}
    bool begin(){wait();if(FAILED(allocator->Reset()))return false;if(FAILED(list->Reset(allocator,nullptr)))return false;return true;}
    bool execute(){if(FAILED(list->Close()))return false;ID3D12CommandList*commands[]={list};queue->ExecuteCommandLists(1,commands);wait();return true;}
    ID3D12Resource*makeBuffer(UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){ID3D12Resource*r{};auto h=heap(type);auto d=bufferDesc(std::max<UINT64>(bytes,256),flags);if(FAILED(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&r))))return nullptr;return r;}
    template<class T>ID3D12Resource*upload(const std::vector<T>&data){ID3D12Resource*r=makeBuffer(data.size()*sizeof(T),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!r)return nullptr;void*mapped{};if(FAILED(r->Map(0,nullptr,&mapped))){release(r);return nullptr;}std::memcpy(mapped,data.data(),data.size()*sizeof(T));r->Unmap(0,nullptr);return r;}
    template<class T>ID3D12Resource*uploadDefault(const std::vector<T>&data){const UINT64 bytes=std::max<UINT64>(data.size()*sizeof(T),256);ID3D12Resource*destination=makeBuffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST),*staging=makeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!destination||!staging){release(destination);release(staging);return nullptr;}void*mapped{};if(FAILED(staging->Map(0,nullptr,&mapped))){release(destination);release(staging);return nullptr;}std::memcpy(mapped,data.data(),data.size()*sizeof(T));staging->Unmap(0,nullptr);if(!begin()){release(destination);release(staging);return nullptr;}list->CopyBufferRegion(destination,0,staging,0,data.size()*sizeof(T));auto barrier=transition(destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_GENERIC_READ);list->ResourceBarrier(1,&barrier);if(!execute()){release(destination);release(staging);return nullptr;}release(staging);return destination;}
    template<class T>ID3D12Resource*uploadDefaultUav(const std::vector<T>&data){const UINT64 bytes=std::max<UINT64>(data.size()*sizeof(T),256);ID3D12Resource*destination=makeBuffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),*staging=makeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!destination||!staging){release(destination);release(staging);return nullptr;}void*mapped{};if(FAILED(staging->Map(0,nullptr,&mapped))){release(destination);release(staging);return nullptr;}std::memcpy(mapped,data.data(),data.size()*sizeof(T));staging->Unmap(0,nullptr);if(!begin()){release(destination);release(staging);return nullptr;}list->CopyBufferRegion(destination,0,staging,0,data.size()*sizeof(T));auto barrier=transition(destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&barrier);if(!execute()){release(destination);release(staging);return nullptr;}release(staging);return destination;}
    std::pair<UINT,UINT> compactVisibleGrass(
        const Vec3&eye,const Vec3&forward,const Vec3&right,const Vec3&up,
        float tanHalf,float aspect,const DebugRenderSettings&settings){
        if(!visibleGrassMapped)return {};
        auto*visible=static_cast<GrassPatchGpu*>(visibleGrassMapped);
        UINT nearCount=0,farCount=0;
        const float shortDistance=clamp(settings.shortGrassDrawDistance,2.0f,128.0f);
        const float tallDistance=std::max(shortDistance,
            clamp(settings.tallGrassDrawDistance,4.0f,192.0f));
        for(const GrassPatchGpu&patch:environment.grassPatches){
            const bool tall=((patch.packed>>16)&255u)!=0;
            const float drawDistance=tall?tallDistance:shortDistance;
            const Vec3 center{(patch.minX+patch.maxX)*.5f,patch.baseY,
                              (patch.minZ+patch.maxZ)*.5f};
            const Vec3 delta=center-eye;
            if(dot(delta,delta)>=drawDistance*drawDistance)continue;
            const float radiusX=(patch.maxX-patch.minX)*.5f;
            const float radiusY=std::max(patch.maxY-patch.baseY,
                                         patch.baseY-patch.minY);
            const float radiusZ=(patch.maxZ-patch.minZ)*.5f;
            const float radius=std::sqrt(radiusX*radiusX+radiusY*radiusY+radiusZ*radiusZ);
            const float viewDepth=dot(delta,forward);
            if(viewDepth+radius<=.02f)continue;
            const float projectedDepth=std::max(viewDepth,.02f);
            if(std::abs(dot(delta,right))>projectedDepth*tanHalf*aspect+radius)continue;
            if(std::abs(dot(delta,up))>projectedDepth*tanHalf+radius)continue;
            if(dot(delta,delta)<shortDistance*shortDistance){
                visible[nearCount++]=patch;
            }else if(tall){
                visible[grassPatchCount-1u-farCount++]=patch;
            }
        }
        return {nearCount,farCount};
    }
    bool createBackBuffers(){D3D12_CPU_DESCRIPTOR_HANDLE handle=rtvHeap->GetCPUDescriptorHandleForHeapStart();for(UINT n=0;n<2;++n){HRESULT hr=swap->GetBuffer(n,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&backBuffers[n]));if(FAILED(hr))return fail(hr,L"DXR swap-chain buffer creation failed");device->CreateRenderTargetView(backBuffers[n],nullptr,handle);handle.ptr+=rtvSize;}return true;}
    bool createOutputs(){release(output);release(accumulation);release(grassDepth);D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;auto h=heap(D3D12_HEAP_TYPE_DEFAULT);
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;HRESULT hr=device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&output));if(FAILED(hr))return fail(hr,L"DXR output texture creation failed");
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;hr=device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&accumulation));if(FAILED(hr))return fail(hr,L"DXR accumulation texture creation failed");
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;d.Format=DXGI_FORMAT_D32_FLOAT;D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_D32_FLOAT;clear.DepthStencil.Depth=1.0f;hr=device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&grassDepth));if(FAILED(hr))return fail(hr,L"Grass depth texture creation failed");
        D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};depthView.Format=DXGI_FORMAT_D32_FLOAT;depthView.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;device->CreateDepthStencilView(grassDepth,&depthView,dsvHeap->GetCPUDescriptorHandleForHeapStart());
        auto cpu=gpuHeap->GetCPUDescriptorHandleForHeapStart();D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;u.Format=DXGI_FORMAT_R8G8B8A8_UNORM;device->CreateUnorderedAccessView(output,nullptr,&u,cpu);cpu.ptr+=srvSize;u.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;device->CreateUnorderedAccessView(accumulation,nullptr,&u,cpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;cpu=gpuHeap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=5ull*srvSize;view.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;device->CreateShaderResourceView(accumulation,&view,cpu);cpu.ptr+=srvSize;view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;device->CreateShaderResourceView(output,&view,cpu);frameIndex=0;return true;}
    bool createBarkNormal(){
        constexpr UINT textureWidth=2048,textureHeight=2048,mipLevels=12;const auto pixels=makeNormalMipChain(makeOakBarkNormal(textureWidth,textureHeight),textureWidth,textureHeight,mipLevels);D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=textureWidth;d.Height=textureHeight;d.DepthOrArraySize=1;d.MipLevels=mipLevels;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;auto defaultHeap=heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr=device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&barkNormal));if(FAILED(hr))return fail(hr,L"Runtime oak bark normal texture creation failed");
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipLevels);std::vector<UINT> rows(mipLevels);std::vector<UINT64> rowBytes(mipLevels);UINT64 uploadBytes{};device->GetCopyableFootprints(&d,0,mipLevels,0,footprints.data(),rows.data(),rowBytes.data(),&uploadBytes);ID3D12Resource*uploadBuffer=makeBuffer(uploadBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!uploadBuffer)return fail(E_OUTOFMEMORY,L"Runtime oak bark upload allocation failed");
        void*mapped{};if(FAILED(uploadBuffer->Map(0,nullptr,&mapped))){release(uploadBuffer);return fail(E_FAIL,L"Runtime oak bark upload mapping failed");}for(UINT level=0;level<mipLevels;++level){const UINT levelWidth=std::max(1u,textureWidth>>level),levelHeight=std::max(1u,textureHeight>>level);for(UINT y=0;y<levelHeight;++y)std::memcpy(static_cast<char*>(mapped)+footprints[level].Offset+static_cast<size_t>(y)*footprints[level].Footprint.RowPitch,pixels[level].data()+static_cast<size_t>(y)*levelWidth,levelWidth*sizeof(uint32_t));}uploadBuffer->Unmap(0,nullptr);
        if(!begin()){release(uploadBuffer);return false;}for(UINT level=0;level<mipLevels;++level){D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=barkNormal;destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;destination.SubresourceIndex=level;D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=uploadBuffer;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprints[level];list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);}auto barrier=transition(barkNormal,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&barrier);if(!execute()){release(uploadBuffer);return false;}release(uploadBuffer);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Format=d.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=mipLevels;auto cpu=gpuHeap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=2*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);device->CreateShaderResourceView(barkNormal,&view,cpu);return true;
    }
    bool createGroundTextureArray(const std::vector<GroundTextureMip>&mips,
                                  ID3D12Resource*&resource,UINT descriptorIndex,
                                  const wchar_t*failureMessage){
        if(mips.empty()||mips.front().width!=GroundTextureAtlas::atlasWidth||
           mips.front().height!=GroundTextureAtlas::atlasHeight)return fail(E_INVALIDARG,failureMessage);
        const UINT mipLevels=static_cast<UINT>(mips.size()),arraySize=GroundTextureAtlas::tileCount;
        const UINT topTileWidth=mips.front().width/2,topTileHeight=mips.front().height/2;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=topTileWidth;d.Height=topTileHeight;d.DepthOrArraySize=static_cast<UINT16>(arraySize);d.MipLevels=static_cast<UINT16>(mipLevels);d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;auto defaultHeap=heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr=device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&resource));if(FAILED(hr))return fail(hr,failureMessage);
        const UINT subresourceCount=mipLevels*arraySize;std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);std::vector<UINT> rows(subresourceCount);std::vector<UINT64> rowBytes(subresourceCount);UINT64 uploadBytes{};device->GetCopyableFootprints(&d,0,subresourceCount,0,footprints.data(),rows.data(),rowBytes.data(),&uploadBytes);ID3D12Resource*uploadBuffer=makeBuffer(uploadBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!uploadBuffer){release(resource);return fail(E_OUTOFMEMORY,failureMessage);}
        void*mapped{};if(FAILED(uploadBuffer->Map(0,nullptr,&mapped))){release(uploadBuffer);release(resource);return fail(E_FAIL,failureMessage);}for(UINT tile=0;tile<arraySize;++tile)for(UINT level=0;level<mipLevels;++level){const auto&mip=mips[level];const UINT tileWidth=mip.width/2,tileHeight=mip.height/2,originX=(tile&1u)*tileWidth,originY=(tile>>1u)*tileHeight,subresource=level+tile*mipLevels;if(tileWidth==0||tileHeight==0||mip.pixels.size()!=static_cast<size_t>(mip.width)*mip.height){uploadBuffer->Unmap(0,nullptr);release(uploadBuffer);release(resource);return fail(E_INVALIDARG,failureMessage);}for(UINT y=0;y<tileHeight;++y)std::memcpy(static_cast<char*>(mapped)+footprints[subresource].Offset+static_cast<size_t>(y)*footprints[subresource].Footprint.RowPitch,mip.pixels.data()+static_cast<size_t>(originY+y)*mip.width+originX,static_cast<size_t>(tileWidth)*sizeof(uint32_t));}uploadBuffer->Unmap(0,nullptr);
        if(!begin()){release(uploadBuffer);release(resource);return false;}for(UINT subresource=0;subresource<subresourceCount;++subresource){D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=resource;destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;destination.SubresourceIndex=subresource;D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=uploadBuffer;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprints[subresource];list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);}auto barrier=transition(resource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&barrier);if(!execute()){release(uploadBuffer);release(resource);return false;}release(uploadBuffer);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Format=d.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;view.Texture2DArray.MipLevels=mipLevels;view.Texture2DArray.ArraySize=arraySize;auto cpu=gpuHeap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=descriptorIndex*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);device->CreateShaderResourceView(resource,&view,cpu);return true;
    }
    bool createGroundMaterials(){
        const GroundTextureAtlas atlas=makeGroundTextureAtlas();
        if(!createGroundTextureArray(atlas.albedoRoughness,groundAlbedo,3,L"Runtime ground albedo texture creation failed"))return false;
        if(!createGroundTextureArray(atlas.normalHeightCavity,groundNormal,4,L"Runtime ground normal texture creation failed")){release(groundAlbedo);return false;}
        return true;
    }
    std::vector<char>loadShader(const wchar_t*name){wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);auto path=std::filesystem::path(exe).parent_path().parent_path()/L"shaders"/name;std::ifstream stream(path,std::ios::binary|std::ios::ate);if(!stream)return{};const auto size=stream.tellg();stream.seekg(0);std::vector<char>data(static_cast<size_t>(size));stream.read(data.data(),size);return data;}
    std::vector<char>loadDxil(){return loadShader(L"raytracing.dxil");}
    bool createPipeline(){D3D12_DESCRIPTOR_RANGE ranges[3]{};ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[0].NumDescriptors=2;ranges[0].BaseShaderRegister=0;ranges[0].OffsetInDescriptorsFromTableStart=0;ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[1].NumDescriptors=1;ranges[1].BaseShaderRegister=3;ranges[1].OffsetInDescriptorsFromTableStart=2;ranges[2].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[2].NumDescriptors=2;ranges[2].BaseShaderRegister=5;ranges[2].OffsetInDescriptorsFromTableStart=3;
        D3D12_ROOT_PARAMETER params[7]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable.NumDescriptorRanges=3;params[0].DescriptorTable.pDescriptorRanges=ranges;for(int p=1;p<=3;++p){params[p].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[p].Descriptor.ShaderRegister=static_cast<UINT>(p-1);}params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[4].Descriptor.ShaderRegister=0;params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[5].Descriptor.ShaderRegister=4;params[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[6].Descriptor.ShaderRegister=1;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_ANISOTROPIC;sampler.AddressU=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.AddressV=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.MaxAnisotropy=8;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MinLOD=0;sampler.MaxLOD=10;sampler.ShaderRegister=0;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=7;rs.pParameters=params;rs.NumStaticSamplers=1;rs.pStaticSamplers=&sampler;ID3DBlob*blob{},*errors{};HRESULT hr=D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);if(FAILED(hr)){release(errors);return fail(hr,L"DXR root-signature serialization failed");}hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),__uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&root));release(blob);release(errors);if(FAILED(hr))return fail(hr,L"DXR root-signature creation failed");
        auto dxil=loadDxil();if(dxil.empty()){lastError=L"Compiled raytracing.dxil was not found beside the build output.";return false;}
        const wchar_t*exports[]={L"RayGen",L"RadianceMiss",L"VisibilityMiss",L"RadianceHit",L"VisibilityHit",L"GrassIntersection",L"GrassRadianceHit"};D3D12_EXPORT_DESC exportDescs[7]{};for(int n=0;n<7;++n)exportDescs[n].Name=exports[n];D3D12_DXIL_LIBRARY_DESC library{};library.DXILLibrary={dxil.data(),dxil.size()};library.NumExports=7;library.pExports=exportDescs;
        D3D12_HIT_GROUP_DESC hit0{};hit0.HitGroupExport=L"RadianceHitGroup";hit0.ClosestHitShaderImport=L"RadianceHit";hit0.Type=D3D12_HIT_GROUP_TYPE_TRIANGLES;D3D12_HIT_GROUP_DESC hit1{};hit1.HitGroupExport=L"VisibilityHitGroup";hit1.ClosestHitShaderImport=L"VisibilityHit";hit1.Type=D3D12_HIT_GROUP_TYPE_TRIANGLES;
        D3D12_HIT_GROUP_DESC hit2{};hit2.HitGroupExport=L"GrassRadianceHitGroup";hit2.IntersectionShaderImport=L"GrassIntersection";hit2.ClosestHitShaderImport=L"GrassRadianceHit";hit2.Type=D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;D3D12_HIT_GROUP_DESC hit3{};hit3.HitGroupExport=L"GrassVisibilityHitGroup";hit3.IntersectionShaderImport=L"GrassIntersection";hit3.Type=D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{24,8};D3D12_GLOBAL_ROOT_SIGNATURE global{root};D3D12_RAYTRACING_PIPELINE_CONFIG pipeline{3};D3D12_STATE_SUBOBJECT subs[9]{};subs[0]={D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library};subs[1]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit0};subs[2]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit1};subs[3]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit2};subs[4]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit3};subs[5]={D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&shaderConfig};const wchar_t*associations[]={L"RayGen",L"RadianceMiss",L"VisibilityMiss",L"RadianceHitGroup",L"VisibilityHitGroup",L"GrassRadianceHitGroup",L"GrassVisibilityHitGroup"};D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&subs[5],7,associations};subs[6]={D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,&association};subs[7]={D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global};subs[8]={D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&pipeline};D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,9,subs};hr=device->CreateStateObject(&desc,__uuidof(ID3D12StateObject),reinterpret_cast<void**>(&state));if(FAILED(hr))return fail(hr,L"DXR state-object creation failed");hr=state->QueryInterface(__uuidof(ID3D12StateObjectProperties),reinterpret_cast<void**>(&stateProps));if(FAILED(hr))return fail(hr,L"DXR state-object properties unavailable");return createShaderTables();}

    bool createGrassPipeline(){
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=2;range.BaseShaderRegister=0;range.OffsetInDescriptorsFromTableStart=0;
        D3D12_ROOT_PARAMETER params[6]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[0].Descriptor.ShaderRegister=0;params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[1].Descriptor.ShaderRegister=2;params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[2].DescriptorTable.NumDescriptorRanges=1;params[2].DescriptorTable.pDescriptorRanges=&range;params[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].Constants.ShaderRegister=2;params[3].Constants.Num32BitValues=2;
        params[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
        params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[4].Descriptor.ShaderRegister=1;params[4].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[5].Descriptor.ShaderRegister=3;
        params[5].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=6;signature.pParameters=params;signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob*blob{},*errors{};HRESULT hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);if(FAILED(hr)){release(errors);return fail(hr,L"Grass root-signature serialization failed");}
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),__uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&grassRoot));release(blob);release(errors);if(FAILED(hr))return fail(hr,L"Grass root-signature creation failed");
        const auto vertexShader=loadShader(L"grass_overlay_vs.dxil"),pixelShader=loadShader(L"grass_overlay_ps.dxil");if(vertexShader.empty()||pixelShader.empty()){lastError=L"Compiled grass overlay shaders were not found beside the build output.";return false;}
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=grassRoot;desc.VS={vertexShader.data(),vertexShader.size()};desc.PS={pixelShader.data(),pixelShader.size()};
        auto&target=desc.BlendState.RenderTarget[0];target.BlendEnable=FALSE;target.LogicOpEnable=FALSE;target.SrcBlend=D3D12_BLEND_ONE;target.DestBlend=D3D12_BLEND_ZERO;target.BlendOp=D3D12_BLEND_OP_ADD;target.SrcBlendAlpha=D3D12_BLEND_ONE;target.DestBlendAlpha=D3D12_BLEND_ZERO;target.BlendOpAlpha=D3D12_BLEND_OP_ADD;target.LogicOp=D3D12_LOGIC_OP_NOOP;target.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask=UINT_MAX;desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;desc.RasterizerState.DepthClipEnable=TRUE;
        desc.DepthStencilState.DepthEnable=TRUE;desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;desc.DepthStencilState.StencilEnable=FALSE;
        desc.InputLayout={nullptr,0};desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;desc.NumRenderTargets=1;desc.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;desc.DSVFormat=DXGI_FORMAT_D32_FLOAT;desc.SampleDesc.Count=1;
        hr=device->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),reinterpret_cast<void**>(&grassPipeline));if(FAILED(hr))return fail(hr,L"Grass graphics pipeline creation failed");return true;
    }
    bool createTreeWindPipeline(){
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[0].Descriptor.ShaderRegister=0;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor.ShaderRegister=0;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants.ShaderRegister=0;params[2].Constants.Num32BitValues=4;
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[3].Descriptor.ShaderRegister=1;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=4;
        signature.pParameters=params;
        ID3DBlob*blob{},*errors{};
        HRESULT hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,
                                               &blob,&errors);
        if(FAILED(hr)){release(errors);return fail(hr,L"Tree-wind root-signature serialization failed");}
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
            __uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&treeWindRoot));
        release(blob);release(errors);
        if(FAILED(hr))return fail(hr,L"Tree-wind root-signature creation failed");
        const auto shader=loadShader(L"tree_wind.dxil");
        if(shader.empty()){lastError=L"Compiled tree_wind.dxil was not found beside the build output.";return false;}
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=treeWindRoot;
        desc.CS={shader.data(),shader.size()};
        hr=device->CreateComputePipelineState(&desc,__uuidof(ID3D12PipelineState),
                                              reinterpret_cast<void**>(&treeWindPipeline));
        if(FAILED(hr))return fail(hr,L"Tree-wind compute pipeline creation failed");
        return true;
    }
    ID3D12Resource*shaderTable(const std::vector<const void*>&ids){const UINT stride=D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;ID3D12Resource*r=makeBuffer(alignUp(ids.size()*stride,D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!r)return nullptr;void*m{};r->Map(0,nullptr,&m);for(size_t i=0;i<ids.size();++i)std::memcpy(static_cast<char*>(m)+i*stride,ids[i],stride);r->Unmap(0,nullptr);return r;}
    bool createShaderTables(){const void*rg=stateProps->GetShaderIdentifier(L"RayGen"),*rm=stateProps->GetShaderIdentifier(L"RadianceMiss"),*vm=stateProps->GetShaderIdentifier(L"VisibilityMiss"),*rh=stateProps->GetShaderIdentifier(L"RadianceHitGroup"),*vh=stateProps->GetShaderIdentifier(L"VisibilityHitGroup"),*grh=stateProps->GetShaderIdentifier(L"GrassRadianceHitGroup"),*gvh=stateProps->GetShaderIdentifier(L"GrassVisibilityHitGroup");if(!rg||!rm||!vm||!rh||!vh||!grh||!gvh){lastError=L"DXR shader identifier lookup failed.";return false;}raygenTable=shaderTable({rg});missTable=shaderTable({rm,vm});hitTable=shaderTable({rh,vh,grh,gvh});return raygenTable&&missTable&&hitTable;}
    bool buildBottomLevel(const D3D12_RAYTRACING_GEOMETRY_DESC&geometry,
                          ID3D12Resource*&scratch,ID3D12Resource*&result,
                          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags=
                              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE){
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        input.Flags=flags;
        input.NumDescs=1;input.pGeometryDescs=&geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
        scratch=makeBuffer(std::max(info.ScratchDataSizeInBytes,info.UpdateScratchDataSizeInBytes),D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        result=makeBuffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if(!scratch||!result)return false;
        if(!begin())return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=input;
        build.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
        build.DestAccelerationStructureData=result->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource=result;list->ResourceBarrier(1,&uav);
        return execute();
    }
    bool buildAcceleration(const TreeMesh&tree){
        wait();release(instanceBuffer);release(tlasScratch);release(grassBlasScratch);
        release(staticBlasScratch);release(blasScratch);release(tlas);release(grassBlas);
        release(staticBlas);release(blas);
        if(visibleGrassBuffer&&visibleGrassMapped)visibleGrassBuffer->Unmap(0,nullptr);
        visibleGrassMapped=nullptr;release(visibleGrassBuffer);
        release(grassBuffer);release(indexBuffer);release(baseTreeVertexBuffer);release(vertexBuffer);
        if(environment.terrainVertices.empty())environment=EnvironmentGenerator{}.build();

        std::vector<MeshVertex>treeVertices=tree.branchVertices;
        treeVertices.insert(treeVertices.end(),tree.leafVertices.begin(),tree.leafVertices.end());
        std::vector<MeshVertex>vertices=treeVertices;std::vector<uint32_t>indices;
        vertices.reserve(tree.branchVertices.size()+tree.leafVertices.size()+
                         environment.terrainVertices.size()+environment.detailVertices.size());
        indices.reserve(tree.branchIndices.size()+tree.leafIndices.size()+
                        environment.terrainIndices.size()+environment.detailIndices.size());
        indices=tree.branchIndices;
        const uint32_t leafBase=static_cast<uint32_t>(tree.branchVertices.size());
        for(uint32_t index:tree.leafIndices)indices.push_back(leafBase+index);
        treeVertexCount=static_cast<UINT>(treeVertices.size());
        treeIndexCount=static_cast<UINT>(indices.size());treeHeight=.5f;
        for(const MeshVertex&vertex:treeVertices)treeHeight=std::max(treeHeight,vertex.position.y);
        const uint32_t terrainBase=static_cast<uint32_t>(vertices.size());
        vertices.insert(vertices.end(),environment.terrainVertices.begin(),
                        environment.terrainVertices.end());
        for(uint32_t index:environment.terrainIndices)indices.push_back(terrainBase+index);
        const uint32_t detailBase=static_cast<uint32_t>(vertices.size());
        vertices.insert(vertices.end(),environment.detailVertices.begin(),
                        environment.detailVertices.end());
        for(uint32_t index:environment.detailIndices)indices.push_back(detailBase+index);
        vertexCount=static_cast<UINT>(vertices.size());indexCount=static_cast<UINT>(indices.size());
        baseTreeVertexBuffer=uploadDefault(treeVertices);
        vertexBuffer=uploadDefaultUav(vertices);indexBuffer=uploadDefault(indices);
        grassPatchCount=static_cast<UINT>(environment.grassPatches.size());
        grassBuffer=uploadDefault(environment.grassPatches);
        visibleGrassBuffer=makeBuffer(
            std::max<UINT64>(environment.grassPatches.size()*sizeof(GrassPatchGpu),256),
            D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        if(visibleGrassBuffer&&
           FAILED(visibleGrassBuffer->Map(0,nullptr,&visibleGrassMapped))){
            release(visibleGrassBuffer);visibleGrassMapped=nullptr;
        }
        if(!baseTreeVertexBuffer||!vertexBuffer||!indexBuffer||!grassBuffer||!visibleGrassBuffer){
            lastError=L"DXR scene geometry upload to GPU-local memory failed.";return false;
        }

        D3D12_RAYTRACING_GEOMETRY_DESC treeGeometry{};
        treeGeometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        treeGeometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        treeGeometry.Triangles.VertexBuffer.StartAddress=vertexBuffer->GetGPUVirtualAddress();
        treeGeometry.Triangles.VertexBuffer.StrideInBytes=sizeof(MeshVertex);
        treeGeometry.Triangles.VertexCount=treeVertexCount;
        treeGeometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        treeGeometry.Triangles.IndexBuffer=indexBuffer->GetGPUVirtualAddress();
        treeGeometry.Triangles.IndexCount=treeIndexCount;
        treeGeometry.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
        constexpr auto updateFlags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
        if(!buildBottomLevel(treeGeometry,blasScratch,blas,updateFlags))return false;

        D3D12_RAYTRACING_GEOMETRY_DESC staticGeometry=treeGeometry;
        staticGeometry.Triangles.VertexCount=vertexCount;
        staticGeometry.Triangles.IndexBuffer=indexBuffer->GetGPUVirtualAddress()+
                                               static_cast<UINT64>(treeIndexCount)*sizeof(uint32_t);
        staticGeometry.Triangles.IndexCount=indexCount-treeIndexCount;
        if(!buildBottomLevel(staticGeometry,staticBlasScratch,staticBlas))return false;

        D3D12_RAYTRACING_INSTANCE_DESC triangleInstance{};
        triangleInstance.Transform[0][0]=triangleInstance.Transform[1][1]=
            triangleInstance.Transform[2][2]=1;
        triangleInstance.InstanceMask=0x1;
        triangleInstance.InstanceID=0;
        triangleInstance.InstanceContributionToHitGroupIndex=0;
        triangleInstance.AccelerationStructure=blas->GetGPUVirtualAddress();
        D3D12_RAYTRACING_INSTANCE_DESC staticInstance=triangleInstance;
        staticInstance.InstanceID=1;
        staticInstance.AccelerationStructure=staticBlas->GetGPUVirtualAddress();
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC>instances{triangleInstance,staticInstance};
        instanceBuffer=upload(instances);
        if(!instanceBuffer)return false;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        input.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
        input.NumDescs=static_cast<UINT>(instances.size());
        input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        input.InstanceDescs=instanceBuffer->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
        tlasScratch=makeBuffer(std::max(info.ScratchDataSizeInBytes,info.UpdateScratchDataSizeInBytes),D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        tlas=makeBuffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if(!tlasScratch||!tlas)return false;
        if(!begin())return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=input;
        build.ScratchAccelerationStructureData=tlasScratch->GetGPUVirtualAddress();
        build.DestAccelerationStructureData=tlas->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource=tlas;list->ResourceBarrier(1,&uav);
        if(!execute())return false;
        frameIndex=0;treeWindWasActive=false;return true;
    }
    bool recordTreeWind(const EnvironmentCB&environmentConstants){
        const bool active=environmentConstants.windSpeed>.001f&&
                          environmentConstants.windStrength>.001f;
        if(!active&&!treeWindWasActive)return true;
        if(!treeWindPipeline||!treeWindRoot||!baseTreeVertexBuffer||!vertexBuffer||
           !blas||!tlas||!instanceBuffer||treeVertexCount==0)return false;

        auto toUav=transition(vertexBuffer,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1,&toUav);
        list->SetComputeRootSignature(treeWindRoot);
        list->SetPipelineState(treeWindPipeline);
        list->SetComputeRootShaderResourceView(0,baseTreeVertexBuffer->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1,vertexBuffer->GetGPUVirtualAddress());
        struct WindConstants{UINT count;float height;UINT padding[2];}
            constants{treeVertexCount,treeHeight,{0,0}};
        static_assert(sizeof(WindConstants)==16);
        list->SetComputeRoot32BitConstants(2,4,&constants,0);
        list->SetComputeRootConstantBufferView(3,environmentBuffer->GetGPUVirtualAddress());
        list->Dispatch((treeVertexCount+255u)/256u,1,1);
        D3D12_RESOURCE_BARRIER vertexUav{};vertexUav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        vertexUav.UAV.pResource=vertexBuffer;list->ResourceBarrier(1,&vertexUav);
        auto toAcceleration=transition(vertexBuffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1,&toAcceleration);

        D3D12_RAYTRACING_GEOMETRY_DESC treeGeometry{};
        treeGeometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        treeGeometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        treeGeometry.Triangles.VertexBuffer.StartAddress=vertexBuffer->GetGPUVirtualAddress();
        treeGeometry.Triangles.VertexBuffer.StrideInBytes=sizeof(MeshVertex);
        treeGeometry.Triangles.VertexCount=treeVertexCount;
        treeGeometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        treeGeometry.Triangles.IndexBuffer=indexBuffer->GetGPUVirtualAddress();
        treeGeometry.Triangles.IndexCount=treeIndexCount;
        treeGeometry.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottomInput{};
        bottomInput.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottomInput.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
        bottomInput.NumDescs=1;bottomInput.pGeometryDescs=&treeGeometry;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomBuild{};
        bottomBuild.Inputs=bottomInput;
        bottomBuild.SourceAccelerationStructureData=blas->GetGPUVirtualAddress();
        bottomBuild.DestAccelerationStructureData=blas->GetGPUVirtualAddress();
        bottomBuild.ScratchAccelerationStructureData=blasScratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&bottomBuild,0,nullptr);
        D3D12_RESOURCE_BARRIER blasUav{};blasUav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        blasUav.UAV.pResource=blas;list->ResourceBarrier(1,&blasUav);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topInput{};
        topInput.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        topInput.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
        topInput.NumDescs=2;topInput.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        topInput.InstanceDescs=instanceBuffer->GetGPUVirtualAddress();
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuild{};
        topBuild.Inputs=topInput;
        topBuild.SourceAccelerationStructureData=tlas->GetGPUVirtualAddress();
        topBuild.DestAccelerationStructureData=tlas->GetGPUVirtualAddress();
        topBuild.ScratchAccelerationStructureData=tlasScratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&topBuild,0,nullptr);
        D3D12_RESOURCE_BARRIER tlasUav{};tlasUav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        tlasUav.UAV.pResource=tlas;list->ResourceBarrier(1,&tlasUav);
        treeWindWasActive=active;
        return true;
    }
};

DxrRenderer::DxrRenderer():impl_(std::make_unique<Impl>()){}DxrRenderer::~DxrRenderer()=default;
bool DxrRenderer::initialize(HWND window,int width,int height){auto&i=*impl_;i.window=window;i.width=std::max(1,width);i.height=std::max(1,height);HRESULT hr=CreateDXGIFactory1(__uuidof(IDXGIFactory6),reinterpret_cast<void**>(&i.factory));if(FAILED(hr))return i.fail(hr,L"DXGI factory creation failed");IDXGIAdapter1*adapter{};for(UINT n=0;i.factory->EnumAdapterByGpuPreference(n,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,__uuidof(IDXGIAdapter1),reinterpret_cast<void**>(&adapter))!=DXGI_ERROR_NOT_FOUND;++n){DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_12_1,__uuidof(ID3D12Device5),reinterpret_cast<void**>(&i.device))))break;release(adapter);}release(adapter);if(!i.device){i.lastError=L"No DXR-capable DirectX 12 device was found.";return false;}D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};if(FAILED(i.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))||options.RaytracingTier<D3D12_RAYTRACING_TIER_1_1){i.lastError=L"The selected GPU does not expose DXR 1.1 inline ray queries.";return false;}
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr=i.device->CreateCommandQueue(&q,__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(&i.queue));if(FAILED(hr))return i.fail(hr,L"DXR command queue creation failed");hr=i.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,__uuidof(ID3D12CommandAllocator),reinterpret_cast<void**>(&i.allocator));if(FAILED(hr))return i.fail(hr,L"DXR command allocator creation failed");hr=i.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,i.allocator,nullptr,__uuidof(ID3D12GraphicsCommandList4),reinterpret_cast<void**>(&i.list));if(FAILED(hr))return i.fail(hr,L"DXR command-list creation failed");i.list->Close();
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=i.width;sd.Height=i.height;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SampleDesc.Count=1;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;IDXGISwapChain1*base{};hr=i.factory->CreateSwapChainForHwnd(i.queue,window,&sd,nullptr,nullptr,&base);if(FAILED(hr))return i.fail(hr,L"DXR swap chain creation failed");hr=base->QueryInterface(__uuidof(IDXGISwapChain3),reinterpret_cast<void**>(&i.swap));release(base);if(FAILED(hr))return i.fail(hr,L"DXR swap-chain interface unavailable");
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=2;i.device->CreateDescriptorHeap(&rh,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(&i.rtvHeap));i.rtvSize=i.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);D3D12_DESCRIPTOR_HEAP_DESC dh{};dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;dh.NumDescriptors=1;i.device->CreateDescriptorHeap(&dh,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(&i.dsvHeap));D3D12_DESCRIPTOR_HEAP_DESC gh{};gh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;gh.NumDescriptors=7;gh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;i.device->CreateDescriptorHeap(&gh,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(&i.gpuHeap));i.srvSize=i.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);i.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),reinterpret_cast<void**>(&i.fence));i.fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!i.createBackBuffers()||!i.createOutputs()||!i.createBarkNormal()||!i.createGroundMaterials()||!i.createPipeline()||!i.createGrassPipeline()||!i.createTreeWindPipeline())return false;i.cameraBuffer=i.makeBuffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);i.environmentBuffer=i.makeBuffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!i.cameraBuffer||!i.environmentBuffer||FAILED(i.cameraBuffer->Map(0,nullptr,&i.cameraMapped))||FAILED(i.environmentBuffer->Map(0,nullptr,&i.environmentMapped)))return false;i.initialized=true;return true;}
void DxrRenderer::resize(int width,int height){auto&i=*impl_;if(!i.initialized||width<=0||height<=0)return;i.wait();i.width=width;i.height=height;for(auto&b:i.backBuffers)release(b);if(SUCCEEDED(i.swap->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0))){i.createBackBuffers();i.createOutputs();}}
void DxrRenderer::setTree(const TreeMesh&tree){if(impl_->initialized&&!impl_->buildAcceleration(tree))MessageBoxW(impl_->window,impl_->lastError.c_str(),L"Dense Trees DXR geometry error",MB_ICONERROR);}
void DxrRenderer::render(const CameraView&requestedView,
                         const DebugRenderSettings&settings,
                         const EnvironmentCB&environment,
                         const PlayerLocalLight&requestedLocalLight){
    auto&i=*impl_;if(!i.initialized||!i.tlas)return;
    const auto finiteVec=[](const Vec3&value){
        return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);
    };
    Vec3 eye=finiteVec(requestedView.eye)?requestedView.eye:Vec3{0.0f,4.1f,-14.0f};
    Vec3 forward=requestedView.forward;
    if(!finiteVec(forward)||lengthSq(forward)<1e-8f)forward={0.0f,0.0f,1.0f};
    else forward=normalize(forward);
    Vec3 right=cross({0.0f,1.0f,0.0f},forward);
    if(lengthSq(right)<1e-8f)right=cross({0.0f,0.0f,1.0f},forward);
    if(lengthSq(right)<1e-8f)right={1.0f,0.0f,0.0f};
    else right=normalize(right);
    const Vec3 up=normalize(cross(forward,right));

    PlayerLocalLight localLight=requestedLocalLight;
    localLight.intensity=std::isfinite(localLight.intensity)?
        clamp(localLight.intensity,0.0f,2048.0f):60.0f;
    localLight.range=std::isfinite(localLight.range)?
        clamp(localLight.range,.25f,128.0f):22.0f;
    localLight.innerConeRadians=std::isfinite(localLight.innerConeRadians)?
        clamp(localLight.innerConeRadians,0.0f,1.54f):.19198622f;
    localLight.outerConeRadians=std::isfinite(localLight.outerConeRadians)?
        clamp(localLight.outerConeRadians,localLight.innerConeRadians+.001f,1.56f):
        std::max(localLight.innerConeRadians+.001f,.31415927f);
    const CameraView view{eye,forward};
    const auto changed=[](float a,float b){return std::abs(a-b)>.0001f;};
    const bool viewChanged=!i.haveLastView||
        changed(view.eye.x,i.lastView.eye.x)||changed(view.eye.y,i.lastView.eye.y)||
        changed(view.eye.z,i.lastView.eye.z)||
        changed(view.forward.x,i.lastView.forward.x)||
        changed(view.forward.y,i.lastView.forward.y)||
        changed(view.forward.z,i.lastView.forward.z);
    const bool localLightChanged=!i.haveLastLocalLight||
        localLight.enabled!=i.lastLocalLight.enabled||
        localLight.spotlight!=i.lastLocalLight.spotlight||
        changed(localLight.intensity,i.lastLocalLight.intensity)||
        changed(localLight.range,i.lastLocalLight.range)||
        changed(localLight.innerConeRadians,i.lastLocalLight.innerConeRadians)||
        changed(localLight.outerConeRadians,i.lastLocalLight.outerConeRadians);
    const bool debugChanged=!i.haveLastDebugSettings||
        std::abs(settings.grassDensity-i.lastDebugSettings.grassDensity)>.0001f||
        std::abs(settings.bladeHeightScale-i.lastDebugSettings.bladeHeightScale)>.0001f||
        std::abs(settings.groundNormalStrength-i.lastDebugSettings.groundNormalStrength)>.0001f||
        std::abs(settings.groundDetailStrength-i.lastDebugSettings.groundDetailStrength)>.0001f||
        std::abs(settings.shortGrassDrawDistance-i.lastDebugSettings.shortGrassDrawDistance)>.0001f||
        std::abs(settings.tallGrassDrawDistance-i.lastDebugSettings.tallGrassDrawDistance)>.0001f;
    EnvironmentCB visualEnvironment=environment;
    EnvironmentCB previousVisualEnvironment=i.lastEnvironment;
    // Total time and dt are not visible when every time-driven effect is
    // disabled. Ignoring those two clock fields lets a paused scene converge.
    visualEnvironment.time=previousVisualEnvironment.time=0.0f;
    visualEnvironment.deltaTime=previousVisualEnvironment.deltaTime=0.0f;
    const bool environmentChanged=!i.haveLastEnvironment||
        std::memcmp(&visualEnvironment,&previousVisualEnvironment,
                    sizeof(visualEnvironment))!=0;
    if(viewChanged||localLightChanged||debugChanged||environmentChanged){
        i.frameIndex=0;
    }
    i.lastView=view;i.lastLocalLight=localLight;
    i.lastDebugSettings=settings;i.lastEnvironment=environment;
    i.haveLastView=i.haveLastLocalLight=true;
    i.haveLastDebugSettings=i.haveLastEnvironment=true;
    const bool animatedEnvironment=environment.windSpeed>.001f||
        environment.rainIntensity>.001f||environment.lightningFlash>.001f||
        environmentChanged;
    const UINT temporalFrames=animatedEnvironment?1u:8u;
    const UINT shaderFrame=temporalFrames>1u?i.frameIndex:0u;
    const float tanHalf=std::tan(52*pi/360);
    const float grassDensity=clamp(settings.grassDensity,0.0f,6.0f);
    const UINT nearGrassStride=static_cast<UINT>(clamp(
        std::ceil(grassDensity*34.0f),1.0f,128.0f));
    const UINT farGrassStride=static_cast<UINT>(clamp(
        std::ceil(std::min(grassDensity,1.8f)*24.0f),1.0f,44.0f));
    const auto visibleGrass=i.compactVisibleGrass(
        eye,forward,right,up,tanHalf,static_cast<float>(i.width)/i.height,settings);
    i.visibleNearGrassPatchCount=visibleGrass.first;
    i.visibleFarGrassPatchCount=visibleGrass.second;
    struct Camera{
        float eye[3],tanHalf;float forward[3],aspect;float right[3];UINT frame;
        float up[3];UINT maxFrames;
        float exposure,localLightIntensity,localLightRange,localLightInnerCos;
        UINT resolution[2];UINT environmentIndexOffset;float localLightOuterCos;
        float grassSettings[4];float groundSettings[4];
    }c{{eye.x,eye.y,eye.z},tanHalf,
       {forward.x,forward.y,forward.z},static_cast<float>(i.width)/i.height,
       {right.x,right.y,right.z},shaderFrame,{up.x,up.y,up.z},temporalFrames,
        1.0f,localLight.enabled?localLight.intensity:0.0f,localLight.range,
        localLight.spotlight?std::cos(localLight.innerConeRadians):-1.0f,
        {static_cast<UINT>(i.width),static_cast<UINT>(i.height)},
        i.treeIndexCount,
        localLight.spotlight?std::cos(localLight.outerConeRadians):-1.0f,
        {settings.grassDensity,settings.bladeHeightScale,settings.shortGrassDrawDistance,
         settings.tallGrassDrawDistance},
         {settings.groundNormalStrength,settings.groundDetailStrength,
          static_cast<float>(nearGrassStride),0}};
    static_assert(sizeof(Camera)==128);
    static_assert(offsetof(Camera,localLightIntensity)==68);
    static_assert(offsetof(Camera,resolution)==80);
    static_assert(offsetof(Camera,grassSettings)==96);
    // The mapped constants are single-buffered.  begin() waits for the prior
    // submission before we overwrite them, preventing the previous frame's
    // ray/compute work from observing partially updated camera or wind data.
    if(!i.begin())return;
    std::memcpy(i.cameraMapped,&c,sizeof(c));
    std::memcpy(i.environmentMapped,&environment,sizeof(environment));
    if(!i.recordTreeWind(environment)){
        i.lastError=L"GPU tree-wind deformation or acceleration refit failed.";
        i.list->Close();return;
    }
    ID3D12DescriptorHeap*heaps[]={i.gpuHeap};i.list->SetDescriptorHeaps(1,heaps);
    i.list->SetComputeRootSignature(i.root);i.list->SetPipelineState1(i.state);
    i.list->SetComputeRootDescriptorTable(0,i.gpuHeap->GetGPUDescriptorHandleForHeapStart());
    i.list->SetComputeRootShaderResourceView(1,i.tlas->GetGPUVirtualAddress());
    i.list->SetComputeRootShaderResourceView(2,i.vertexBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootShaderResourceView(3,i.indexBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootConstantBufferView(4,i.cameraBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootShaderResourceView(5,i.grassBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootConstantBufferView(6,i.environmentBuffer->GetGPUVirtualAddress());
    D3D12_DISPATCH_RAYS_DESC rays{};
    rays.RayGenerationShaderRecord={i.raygenTable->GetGPUVirtualAddress(),
                                    D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
    rays.MissShaderTable={i.missTable->GetGPUVirtualAddress(),
                          D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES*2,
                          D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
    rays.HitGroupTable={i.hitTable->GetGPUVirtualAddress(),
                        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES*4,
                        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
    rays.Width=i.width;rays.Height=i.height;rays.Depth=1;i.list->DispatchRays(&rays);
    const UINT back=i.swap->GetCurrentBackBufferIndex();
    const D3D12_RESOURCE_STATES outputRead=static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_COPY_SOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER barriers[]={
        transition(i.output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   outputRead),
        transition(i.accumulation,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        transition(i.backBuffers[back],D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_COPY_DEST)};
    i.list->ResourceBarrier(3,barriers);i.list->CopyResource(i.backBuffers[back],i.output);
    auto renderBarrier=transition(i.backBuffers[back],D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET);
    i.list->ResourceBarrier(1,&renderBarrier);

    D3D12_CPU_DESCRIPTOR_HANDLE renderTarget=i.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    renderTarget.ptr+=static_cast<SIZE_T>(back)*i.rtvSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE depthTarget=i.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    i.list->OMSetRenderTargets(1,&renderTarget,FALSE,&depthTarget);
    i.list->ClearDepthStencilView(depthTarget,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
    D3D12_VIEWPORT viewport{0,0,static_cast<float>(i.width),static_cast<float>(i.height),0,1};
    D3D12_RECT scissor{0,0,i.width,i.height};i.list->RSSetViewports(1,&viewport);i.list->RSSetScissorRects(1,&scissor);
    i.list->SetGraphicsRootSignature(i.grassRoot);i.list->SetPipelineState(i.grassPipeline);
    i.list->SetGraphicsRootConstantBufferView(0,i.cameraBuffer->GetGPUVirtualAddress());
    i.list->SetGraphicsRootShaderResourceView(1,i.visibleGrassBuffer->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE grassTextures=i.gpuHeap->GetGPUDescriptorHandleForHeapStart();
    grassTextures.ptr+=5ull*i.srvSize;i.list->SetGraphicsRootDescriptorTable(2,grassTextures);
    i.list->SetGraphicsRootConstantBufferView(4,i.environmentBuffer->GetGPUVirtualAddress());
    i.list->SetGraphicsRootShaderResourceView(5,i.tlas->GetGPUVirtualAddress());
    i.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if(settings.grassDensity>.001f){
        if(i.visibleNearGrassPatchCount){
            const UINT drawConstants[]={0u,nearGrassStride};
            i.list->SetGraphicsRoot32BitConstants(3,2,drawConstants,0);
            i.list->DrawInstanced(12,i.visibleNearGrassPatchCount*nearGrassStride,0,0);
        }
        if(i.visibleFarGrassPatchCount){
            const UINT drawConstants[]={i.grassPatchCount-i.visibleFarGrassPatchCount,
                                        farGrassStride};
            i.list->SetGraphicsRoot32BitConstants(3,2,drawConstants,0);
            i.list->DrawInstanced(12,i.visibleFarGrassPatchCount*farGrassStride,0,0);
        }
    }

    barriers[0]=transition(i.output,outputRead,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barriers[1]=transition(i.accumulation,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barriers[2]=transition(i.backBuffers[back],D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_PRESENT);
    i.list->ResourceBarrier(3,barriers);
    if(i.execute()){i.swap->Present(1,0);i.frameIndex=std::min<UINT>(i.frameIndex+1,1023);}
}

void DxrRenderer::render(float yaw,float pitch,float distance,
                         const DebugRenderSettings&settings,
                         const EnvironmentCB&environment){
    const Vec3 target{0.0f,4.1f,0.0f};
    Vec3 eye=target+Vec3{std::sin(yaw)*std::cos(pitch)*distance,
                         std::sin(pitch)*distance,
                         -std::cos(yaw)*std::cos(pitch)*distance};
    eye.y=std::max(eye.y,EnvironmentGenerator::terrainHeight(eye.x,eye.z)+.34f);
    render(CameraView{eye,normalize(target-eye)},settings,environment,PlayerLocalLight{});
}

const wchar_t*DxrRenderer::error()const{return impl_->lastError.c_str();}bool DxrRenderer::ready()const{return impl_->initialized;}
}
