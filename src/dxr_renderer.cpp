#include "dxr_renderer.hpp"
#include "environment.hpp"
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
    HWND window{};int width=1,height=1;std::wstring lastError;bool initialized=false;UINT frameIndex=0;float lastYaw=99,lastPitch=99,lastDistance=0,lastSun=99,lastWind=-1;
    std::chrono::steady_clock::time_point lastSimulationUpdate=std::chrono::steady_clock::now();
    float simulationSeconds=0;
    EnvironmentMesh environment{};
    IDXGIFactory6*factory{};IDXGISwapChain3*swap{};ID3D12Device5*device{};ID3D12CommandQueue*queue{};ID3D12CommandAllocator*allocator{};ID3D12GraphicsCommandList4*list{};
    ID3D12Fence*fence{};HANDLE fenceEvent{};UINT64 fenceValue{};ID3D12DescriptorHeap*rtvHeap{};ID3D12DescriptorHeap*gpuHeap{};UINT rtvSize{};ID3D12Resource*backBuffers[2]{};
    ID3D12RootSignature*root{};ID3D12StateObject*state{};ID3D12StateObjectProperties*stateProps{};
    ID3D12Resource*output{};ID3D12Resource*accumulation{};ID3D12Resource*barkNormal{};ID3D12Resource*cameraBuffer{};void*cameraMapped{};
    ID3D12Resource*vertexBuffer{};ID3D12Resource*indexBuffer{};ID3D12Resource*blas{};ID3D12Resource*tlas{};ID3D12Resource*blasScratch{};ID3D12Resource*tlasScratch{};ID3D12Resource*instanceBuffer{};
    ID3D12Resource*grassBuffer{};ID3D12Resource*grassBlas{};ID3D12Resource*grassBlasScratch{};
    ID3D12Resource*raygenTable{};ID3D12Resource*missTable{};ID3D12Resource*hitTable{};UINT vertexCount{},indexCount{};

    ~Impl(){wait();if(cameraBuffer&&cameraMapped)cameraBuffer->Unmap(0,nullptr);release(hitTable);release(missTable);release(raygenTable);release(instanceBuffer);release(tlasScratch);release(grassBlasScratch);release(blasScratch);release(tlas);release(grassBlas);release(blas);release(grassBuffer);release(indexBuffer);release(vertexBuffer);release(cameraBuffer);release(barkNormal);release(accumulation);release(output);release(stateProps);release(state);release(root);for(auto&b:backBuffers)release(b);release(gpuHeap);release(rtvHeap);release(list);release(allocator);release(fence);release(queue);release(swap);release(device);release(factory);if(fenceEvent)CloseHandle(fenceEvent);}
    bool fail(HRESULT hr,const wchar_t*message){wchar_t text[320];wsprintfW(text,L"%s (HRESULT 0x%08X)",message,static_cast<unsigned>(hr));lastError=text;return false;}
    void wait(){if(!queue||!fence)return;const UINT64 value=++fenceValue;if(SUCCEEDED(queue->Signal(fence,value))&&fence->GetCompletedValue()<value){fence->SetEventOnCompletion(value,fenceEvent);WaitForSingleObject(fenceEvent,INFINITE);}}
    bool begin(){wait();if(FAILED(allocator->Reset()))return false;if(FAILED(list->Reset(allocator,nullptr)))return false;return true;}
    bool execute(){if(FAILED(list->Close()))return false;ID3D12CommandList*commands[]={list};queue->ExecuteCommandLists(1,commands);wait();return true;}
    ID3D12Resource*makeBuffer(UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){ID3D12Resource*r{};auto h=heap(type);auto d=bufferDesc(std::max<UINT64>(bytes,256),flags);if(FAILED(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&r))))return nullptr;return r;}
    template<class T>ID3D12Resource*upload(const std::vector<T>&data){ID3D12Resource*r=makeBuffer(data.size()*sizeof(T),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!r)return nullptr;void*mapped{};if(FAILED(r->Map(0,nullptr,&mapped))){release(r);return nullptr;}std::memcpy(mapped,data.data(),data.size()*sizeof(T));r->Unmap(0,nullptr);return r;}
    template<class T>ID3D12Resource*uploadDefault(const std::vector<T>&data){const UINT64 bytes=std::max<UINT64>(data.size()*sizeof(T),256);ID3D12Resource*destination=makeBuffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST),*staging=makeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!destination||!staging){release(destination);release(staging);return nullptr;}void*mapped{};if(FAILED(staging->Map(0,nullptr,&mapped))){release(destination);release(staging);return nullptr;}std::memcpy(mapped,data.data(),data.size()*sizeof(T));staging->Unmap(0,nullptr);if(!begin()){release(destination);release(staging);return nullptr;}list->CopyBufferRegion(destination,0,staging,0,data.size()*sizeof(T));auto barrier=transition(destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_GENERIC_READ);list->ResourceBarrier(1,&barrier);if(!execute()){release(destination);release(staging);return nullptr;}release(staging);return destination;}
    bool createBackBuffers(){D3D12_CPU_DESCRIPTOR_HANDLE handle=rtvHeap->GetCPUDescriptorHandleForHeapStart();for(UINT n=0;n<2;++n){HRESULT hr=swap->GetBuffer(n,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&backBuffers[n]));if(FAILED(hr))return fail(hr,L"DXR swap-chain buffer creation failed");device->CreateRenderTargetView(backBuffers[n],nullptr,handle);handle.ptr+=rtvSize;}return true;}
    bool createOutputs(){release(output);release(accumulation);D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;auto h=heap(D3D12_HEAP_TYPE_DEFAULT);
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;HRESULT hr=device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&output));if(FAILED(hr))return fail(hr,L"DXR output texture creation failed");
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;hr=device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&accumulation));if(FAILED(hr))return fail(hr,L"DXR accumulation texture creation failed");
        auto cpu=gpuHeap->GetCPUDescriptorHandleForHeapStart();D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;u.Format=DXGI_FORMAT_R8G8B8A8_UNORM;device->CreateUnorderedAccessView(output,nullptr,&u,cpu);cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);u.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;device->CreateUnorderedAccessView(accumulation,nullptr,&u,cpu);frameIndex=0;return true;}
    bool createBarkNormal(){
        constexpr UINT textureWidth=2048,textureHeight=2048,mipLevels=12;const auto pixels=makeNormalMipChain(makeOakBarkNormal(textureWidth,textureHeight),textureWidth,textureHeight,mipLevels);D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=textureWidth;d.Height=textureHeight;d.DepthOrArraySize=1;d.MipLevels=mipLevels;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;auto defaultHeap=heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr=device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&barkNormal));if(FAILED(hr))return fail(hr,L"Runtime oak bark normal texture creation failed");
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipLevels);std::vector<UINT> rows(mipLevels);std::vector<UINT64> rowBytes(mipLevels);UINT64 uploadBytes{};device->GetCopyableFootprints(&d,0,mipLevels,0,footprints.data(),rows.data(),rowBytes.data(),&uploadBytes);ID3D12Resource*uploadBuffer=makeBuffer(uploadBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!uploadBuffer)return fail(E_OUTOFMEMORY,L"Runtime oak bark upload allocation failed");
        void*mapped{};if(FAILED(uploadBuffer->Map(0,nullptr,&mapped))){release(uploadBuffer);return fail(E_FAIL,L"Runtime oak bark upload mapping failed");}for(UINT level=0;level<mipLevels;++level){const UINT levelWidth=std::max(1u,textureWidth>>level),levelHeight=std::max(1u,textureHeight>>level);for(UINT y=0;y<levelHeight;++y)std::memcpy(static_cast<char*>(mapped)+footprints[level].Offset+static_cast<size_t>(y)*footprints[level].Footprint.RowPitch,pixels[level].data()+static_cast<size_t>(y)*levelWidth,levelWidth*sizeof(uint32_t));}uploadBuffer->Unmap(0,nullptr);
        if(!begin()){release(uploadBuffer);return false;}for(UINT level=0;level<mipLevels;++level){D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=barkNormal;destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;destination.SubresourceIndex=level;D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=uploadBuffer;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprints[level];list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);}auto barrier=transition(barkNormal,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&barrier);if(!execute()){release(uploadBuffer);return false;}release(uploadBuffer);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Format=d.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=mipLevels;auto cpu=gpuHeap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=2*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);device->CreateShaderResourceView(barkNormal,&view,cpu);return true;
    }
    std::vector<char>loadDxil(){wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);auto path=std::filesystem::path(exe).parent_path().parent_path()/L"shaders"/L"raytracing.dxil";std::ifstream stream(path,std::ios::binary|std::ios::ate);if(!stream)return{};const auto size=stream.tellg();stream.seekg(0);std::vector<char>data(static_cast<size_t>(size));stream.read(data.data(),size);return data;}
    bool createPipeline(){D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[0].NumDescriptors=2;ranges[0].BaseShaderRegister=0;ranges[0].OffsetInDescriptorsFromTableStart=0;ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[1].NumDescriptors=1;ranges[1].BaseShaderRegister=3;ranges[1].OffsetInDescriptorsFromTableStart=2;
        D3D12_ROOT_PARAMETER params[6]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable.NumDescriptorRanges=2;params[0].DescriptorTable.pDescriptorRanges=ranges;for(int p=1;p<=3;++p){params[p].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[p].Descriptor.ShaderRegister=static_cast<UINT>(p-1);}params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[4].Descriptor.ShaderRegister=0;params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[5].Descriptor.ShaderRegister=4;
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=6;rs.pParameters=params;ID3DBlob*blob{},*errors{};HRESULT hr=D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);if(FAILED(hr)){release(errors);return fail(hr,L"DXR root-signature serialization failed");}hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),__uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&root));release(blob);release(errors);if(FAILED(hr))return fail(hr,L"DXR root-signature creation failed");
        auto dxil=loadDxil();if(dxil.empty()){lastError=L"Compiled raytracing.dxil was not found beside the build output.";return false;}
        const wchar_t*exports[]={L"RayGen",L"RadianceMiss",L"VisibilityMiss",L"RadianceHit",L"VisibilityHit",L"GrassIntersection",L"GrassRadianceHit"};D3D12_EXPORT_DESC exportDescs[7]{};for(int n=0;n<7;++n)exportDescs[n].Name=exports[n];D3D12_DXIL_LIBRARY_DESC library{};library.DXILLibrary={dxil.data(),dxil.size()};library.NumExports=7;library.pExports=exportDescs;
        D3D12_HIT_GROUP_DESC hit0{};hit0.HitGroupExport=L"RadianceHitGroup";hit0.ClosestHitShaderImport=L"RadianceHit";hit0.Type=D3D12_HIT_GROUP_TYPE_TRIANGLES;D3D12_HIT_GROUP_DESC hit1{};hit1.HitGroupExport=L"VisibilityHitGroup";hit1.ClosestHitShaderImport=L"VisibilityHit";hit1.Type=D3D12_HIT_GROUP_TYPE_TRIANGLES;
        D3D12_HIT_GROUP_DESC hit2{};hit2.HitGroupExport=L"GrassRadianceHitGroup";hit2.IntersectionShaderImport=L"GrassIntersection";hit2.ClosestHitShaderImport=L"GrassRadianceHit";hit2.Type=D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;D3D12_HIT_GROUP_DESC hit3{};hit3.HitGroupExport=L"GrassVisibilityHitGroup";hit3.IntersectionShaderImport=L"GrassIntersection";hit3.Type=D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{16,8};D3D12_GLOBAL_ROOT_SIGNATURE global{root};D3D12_RAYTRACING_PIPELINE_CONFIG pipeline{3};D3D12_STATE_SUBOBJECT subs[9]{};subs[0]={D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library};subs[1]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit0};subs[2]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit1};subs[3]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit2};subs[4]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit3};subs[5]={D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&shaderConfig};const wchar_t*associations[]={L"RayGen",L"RadianceMiss",L"VisibilityMiss",L"RadianceHitGroup",L"VisibilityHitGroup",L"GrassRadianceHitGroup",L"GrassVisibilityHitGroup"};D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&subs[5],7,associations};subs[6]={D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,&association};subs[7]={D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global};subs[8]={D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&pipeline};D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,9,subs};hr=device->CreateStateObject(&desc,__uuidof(ID3D12StateObject),reinterpret_cast<void**>(&state));if(FAILED(hr))return fail(hr,L"DXR state-object creation failed");hr=state->QueryInterface(__uuidof(ID3D12StateObjectProperties),reinterpret_cast<void**>(&stateProps));if(FAILED(hr))return fail(hr,L"DXR state-object properties unavailable");return createShaderTables();}
    ID3D12Resource*shaderTable(const std::vector<const void*>&ids){const UINT stride=D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;ID3D12Resource*r=makeBuffer(alignUp(ids.size()*stride,D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!r)return nullptr;void*m{};r->Map(0,nullptr,&m);for(size_t i=0;i<ids.size();++i)std::memcpy(static_cast<char*>(m)+i*stride,ids[i],stride);r->Unmap(0,nullptr);return r;}
    bool createShaderTables(){const void*rg=stateProps->GetShaderIdentifier(L"RayGen"),*rm=stateProps->GetShaderIdentifier(L"RadianceMiss"),*vm=stateProps->GetShaderIdentifier(L"VisibilityMiss"),*rh=stateProps->GetShaderIdentifier(L"RadianceHitGroup"),*vh=stateProps->GetShaderIdentifier(L"VisibilityHitGroup"),*grh=stateProps->GetShaderIdentifier(L"GrassRadianceHitGroup"),*gvh=stateProps->GetShaderIdentifier(L"GrassVisibilityHitGroup");if(!rg||!rm||!vm||!rh||!vh||!grh||!gvh){lastError=L"DXR shader identifier lookup failed.";return false;}raygenTable=shaderTable({rg});missTable=shaderTable({rm,vm});hitTable=shaderTable({rh,vh,grh,gvh});return raygenTable&&missTable&&hitTable;}
    bool buildBottomLevel(const D3D12_RAYTRACING_GEOMETRY_DESC&geometry,
                          ID3D12Resource*&scratch,ID3D12Resource*&result){
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        input.NumDescs=1;input.pGeometryDescs=&geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
        scratch=makeBuffer(info.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
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
        release(blasScratch);release(tlas);release(grassBlas);release(blas);
        release(grassBuffer);release(indexBuffer);release(vertexBuffer);
        if(environment.terrainVertices.empty())environment=EnvironmentGenerator{}.build();

        std::vector<MeshVertex>vertices;std::vector<uint32_t>indices;
        vertices.reserve(tree.branchVertices.size()+tree.leafVertices.size()+
                         environment.terrainVertices.size()+environment.detailVertices.size());
        indices.reserve(tree.branchIndices.size()+tree.leafIndices.size()+
                        environment.terrainIndices.size()+environment.detailIndices.size());
        vertices=tree.branchVertices;indices=tree.branchIndices;
        const uint32_t leafBase=static_cast<uint32_t>(vertices.size());
        vertices.insert(vertices.end(),tree.leafVertices.begin(),tree.leafVertices.end());
        for(uint32_t index:tree.leafIndices)indices.push_back(leafBase+index);
        const uint32_t terrainBase=static_cast<uint32_t>(vertices.size());
        vertices.insert(vertices.end(),environment.terrainVertices.begin(),
                        environment.terrainVertices.end());
        for(uint32_t index:environment.terrainIndices)indices.push_back(terrainBase+index);
        const uint32_t detailBase=static_cast<uint32_t>(vertices.size());
        vertices.insert(vertices.end(),environment.detailVertices.begin(),
                        environment.detailVertices.end());
        for(uint32_t index:environment.detailIndices)indices.push_back(detailBase+index);
        vertexCount=static_cast<UINT>(vertices.size());indexCount=static_cast<UINT>(indices.size());
        vertexBuffer=uploadDefault(vertices);indexBuffer=uploadDefault(indices);
        grassBuffer=uploadDefault(environment.grassPatches);
        if(!vertexBuffer||!indexBuffer||!grassBuffer){
            lastError=L"DXR scene geometry upload to GPU-local memory failed.";return false;
        }

        D3D12_RAYTRACING_GEOMETRY_DESC triangleGeometry{};
        triangleGeometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        triangleGeometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        triangleGeometry.Triangles.VertexBuffer.StartAddress=vertexBuffer->GetGPUVirtualAddress();
        triangleGeometry.Triangles.VertexBuffer.StrideInBytes=sizeof(MeshVertex);
        triangleGeometry.Triangles.VertexCount=vertexCount;
        triangleGeometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        triangleGeometry.Triangles.IndexBuffer=indexBuffer->GetGPUVirtualAddress();
        triangleGeometry.Triangles.IndexCount=indexCount;
        triangleGeometry.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
        if(!buildBottomLevel(triangleGeometry,blasScratch,blas))return false;

        D3D12_RAYTRACING_GEOMETRY_DESC grassGeometry{};
        grassGeometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        grassGeometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        grassGeometry.AABBs.AABBCount=environment.grassPatches.size();
        grassGeometry.AABBs.AABBs.StartAddress=grassBuffer->GetGPUVirtualAddress();
        grassGeometry.AABBs.AABBs.StrideInBytes=sizeof(GrassPatchGpu);
        if(!buildBottomLevel(grassGeometry,grassBlasScratch,grassBlas))return false;

        D3D12_RAYTRACING_INSTANCE_DESC triangleInstance{};
        triangleInstance.Transform[0][0]=triangleInstance.Transform[1][1]=
            triangleInstance.Transform[2][2]=1;
        triangleInstance.InstanceMask=0x1;
        triangleInstance.InstanceContributionToHitGroupIndex=0;
        triangleInstance.AccelerationStructure=blas->GetGPUVirtualAddress();
        D3D12_RAYTRACING_INSTANCE_DESC grassInstance=triangleInstance;
        grassInstance.InstanceID=1;grassInstance.InstanceMask=0x2;
        grassInstance.InstanceContributionToHitGroupIndex=2;
        grassInstance.AccelerationStructure=grassBlas->GetGPUVirtualAddress();
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC>instances{triangleInstance,grassInstance};
        instanceBuffer=upload(instances);
        if(!instanceBuffer)return false;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        input.NumDescs=static_cast<UINT>(instances.size());
        input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        input.InstanceDescs=instanceBuffer->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
        tlasScratch=makeBuffer(info.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
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
        frameIndex=0;return true;
    }
};

DxrRenderer::DxrRenderer():impl_(std::make_unique<Impl>()){}DxrRenderer::~DxrRenderer()=default;
bool DxrRenderer::initialize(HWND window,int width,int height){auto&i=*impl_;i.window=window;i.width=std::max(1,width);i.height=std::max(1,height);HRESULT hr=CreateDXGIFactory1(__uuidof(IDXGIFactory6),reinterpret_cast<void**>(&i.factory));if(FAILED(hr))return i.fail(hr,L"DXGI factory creation failed");IDXGIAdapter1*adapter{};for(UINT n=0;i.factory->EnumAdapterByGpuPreference(n,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,__uuidof(IDXGIAdapter1),reinterpret_cast<void**>(&adapter))!=DXGI_ERROR_NOT_FOUND;++n){DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_12_1,__uuidof(ID3D12Device5),reinterpret_cast<void**>(&i.device))))break;release(adapter);}release(adapter);if(!i.device){i.lastError=L"No DXR-capable DirectX 12 device was found.";return false;}D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};if(FAILED(i.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))||options.RaytracingTier==D3D12_RAYTRACING_TIER_NOT_SUPPORTED){i.lastError=L"The selected GPU does not expose DXR.";return false;}
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr=i.device->CreateCommandQueue(&q,__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(&i.queue));if(FAILED(hr))return i.fail(hr,L"DXR command queue creation failed");hr=i.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,__uuidof(ID3D12CommandAllocator),reinterpret_cast<void**>(&i.allocator));if(FAILED(hr))return i.fail(hr,L"DXR command allocator creation failed");hr=i.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,i.allocator,nullptr,__uuidof(ID3D12GraphicsCommandList4),reinterpret_cast<void**>(&i.list));if(FAILED(hr))return i.fail(hr,L"DXR command-list creation failed");i.list->Close();
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=i.width;sd.Height=i.height;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SampleDesc.Count=1;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;IDXGISwapChain1*base{};hr=i.factory->CreateSwapChainForHwnd(i.queue,window,&sd,nullptr,nullptr,&base);if(FAILED(hr))return i.fail(hr,L"DXR swap chain creation failed");hr=base->QueryInterface(__uuidof(IDXGISwapChain3),reinterpret_cast<void**>(&i.swap));release(base);if(FAILED(hr))return i.fail(hr,L"DXR swap-chain interface unavailable");
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=2;i.device->CreateDescriptorHeap(&rh,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(&i.rtvHeap));i.rtvSize=i.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);D3D12_DESCRIPTOR_HEAP_DESC gh{};gh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;gh.NumDescriptors=3;gh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;i.device->CreateDescriptorHeap(&gh,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(&i.gpuHeap));i.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),reinterpret_cast<void**>(&i.fence));i.fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!i.createBackBuffers()||!i.createOutputs()||!i.createBarkNormal()||!i.createPipeline())return false;i.cameraBuffer=i.makeBuffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!i.cameraBuffer||FAILED(i.cameraBuffer->Map(0,nullptr,&i.cameraMapped)))return false;i.initialized=true;return true;}
void DxrRenderer::resize(int width,int height){auto&i=*impl_;if(!i.initialized||width<=0||height<=0)return;i.wait();i.width=width;i.height=height;for(auto&b:i.backBuffers)release(b);if(SUCCEEDED(i.swap->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0))){i.createBackBuffers();i.createOutputs();}}
void DxrRenderer::setTree(const TreeMesh&tree){if(impl_->initialized&&!impl_->buildAcceleration(tree))MessageBoxW(impl_->window,impl_->lastError.c_str(),L"Dense Trees DXR geometry error",MB_ICONERROR);}
void DxrRenderer::render(float yaw,float pitch,float distance,float sunAzimuth,float windStrength){
    auto&i=*impl_;if(!i.initialized||!i.tlas)return;
    windStrength=clamp(windStrength,0.0f,1.0f);
    if(std::abs(yaw-i.lastYaw)>.0001f||std::abs(pitch-i.lastPitch)>.0001f||
       std::abs(distance-i.lastDistance)>.0001f||std::abs(sunAzimuth-i.lastSun)>.0001f||
       std::abs(windStrength-i.lastWind)>.0001f){
        i.frameIndex=0;i.lastYaw=yaw;i.lastPitch=pitch;i.lastDistance=distance;
        i.lastSun=sunAzimuth;i.lastWind=windStrength;
    }
    const Vec3 target{0,4.1f,0};
    Vec3 eye=target+Vec3{std::sin(yaw)*std::cos(pitch)*distance,
                         std::sin(pitch)*distance,
                         -std::cos(yaw)*std::cos(pitch)*distance};
    eye.y=std::max(eye.y,EnvironmentGenerator::terrainHeight(eye.x,eye.z)+.34f);
    const Vec3 forward=normalize(target-eye),right=normalize(cross({0,1,0},forward));
    const Vec3 up=cross(forward,right);
    const auto simulationNow=std::chrono::steady_clock::now();
    const float rawSimulationDelta=std::chrono::duration<float>(
        simulationNow-i.lastSimulationUpdate).count();
    i.lastSimulationUpdate=simulationNow;
    i.simulationSeconds+=std::min(rawSimulationDelta,1.0f/20.0f);
    struct Camera{
        float eye[3],tanHalf;float forward[3],aspect;float right[3];UINT frame;
        float up[3];UINT maxFrames;float sun[3],exposure;UINT resolution[2];
        float timeSeconds,windStrength;float atmosphere[4];
    }c{{eye.x,eye.y,eye.z},std::tan(52*pi/360),
       {forward.x,forward.y,forward.z},static_cast<float>(i.width)/i.height,
       {right.x,right.y,right.z},i.frameIndex,{up.x,up.y,up.z},windStrength>.03f?1u:8u,
       {-std::sin(sunAzimuth),-1.35f,-std::cos(sunAzimuth)},1.08f,
       {static_cast<UINT>(i.width),static_cast<UINT>(i.height)},
       i.simulationSeconds,windStrength,{2.15f,.20f,.52f,0}};
    std::memcpy(i.cameraMapped,&c,sizeof(c));if(!i.begin())return;
    ID3D12DescriptorHeap*heaps[]={i.gpuHeap};i.list->SetDescriptorHeaps(1,heaps);
    i.list->SetComputeRootSignature(i.root);i.list->SetPipelineState1(i.state);
    i.list->SetComputeRootDescriptorTable(0,i.gpuHeap->GetGPUDescriptorHandleForHeapStart());
    i.list->SetComputeRootShaderResourceView(1,i.tlas->GetGPUVirtualAddress());
    i.list->SetComputeRootShaderResourceView(2,i.vertexBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootShaderResourceView(3,i.indexBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootConstantBufferView(4,i.cameraBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootShaderResourceView(5,i.grassBuffer->GetGPUVirtualAddress());
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
    D3D12_RESOURCE_BARRIER barriers[]={
        transition(i.output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(i.backBuffers[back],D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_COPY_DEST)};
    i.list->ResourceBarrier(2,barriers);i.list->CopyResource(i.backBuffers[back],i.output);
    barriers[0]=transition(i.output,D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barriers[1]=transition(i.backBuffers[back],D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_PRESENT);
    i.list->ResourceBarrier(2,barriers);
    if(i.execute()){i.swap->Present(1,0);i.frameIndex=std::min<UINT>(i.frameIndex+1,1023);}
}
const wchar_t*DxrRenderer::error()const{return impl_->lastError.c_str();}bool DxrRenderer::ready()const{return impl_->initialized;}
}
