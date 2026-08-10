#include "renderer.hpp"
#include "math.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <string>
#include <vector>

namespace dense {
namespace {
template<class T> void release(T*& p) { if(p) { p->Release(); p=nullptr; } }

constexpr const char* shaderSource=R"(
cbuffer Scene : register(b0) { row_major float4x4 viewProjection; float4 sunDirection; float4 cameraAndTime; };
struct VSIn { float3 position:POSITION; float3 normal:NORMAL; float4 color:COLOR; float material:TEXCOORD0; };
struct PSIn { float4 position:SV_POSITION; float3 world:TEXCOORD0; float3 normal:NORMAL; float4 color:COLOR; float fog:TEXCOORD1; float material:TEXCOORD2; };
PSIn VSMain(VSIn v) {
    PSIn o; float3 world=v.position;
    if(v.material>.5 && v.material<1.5) world.xz += sin(cameraAndTime.w*1.7+world.y*2.3+world.xz)*(.018+.004*world.y);
    o.position=mul(float4(world,1),viewProjection); o.world=world; o.normal=v.normal; o.color=v.color;o.material=v.material;
    o.fog=saturate(o.position.w/45.0); return o;
}
float hash31(float3 p){p=frac(p*.1031);p+=dot(p,p.yzx+33.33);return frac((p.x+p.y)*p.z);}
float3 tonemap(float3 x){return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));}
float4 PSMain(PSIn p,bool front:SV_IsFrontFace):SV_TARGET {
    float kind=floor(p.material+.001);float species=round(frac(p.material)*10);float3 n=normalize(p.normal);if(!front)n=-n;
    float3 sun=normalize(-sunDirection.xyz),view=normalize(cameraAndTime.xyz-p.world);float ndl=dot(n,sun);float3 albedo=pow(p.color.rgb,2.2);
    float skyLight=.34+.18*saturate(n.y);float direct=.78*saturate(ndl);float3 light=float3(1.08,.97,.82)*direct+float3(.48,.61,.74)*skyLight;
    if(kind<.5){
        float noise=hash31(floor(p.world*18));float fissure=1;
        if(species<.5){float grooves=abs(sin(p.world.y*5+p.world.x*7+sin(p.world.z*11)));fissure=lerp(.68,1.06,smoothstep(.14,.52,grooves));}
        else if(species<1.5){float scales=abs(sin(p.world.y*11+p.world.x*6)*sin(p.world.z*8-p.world.y*3));fissure=lerp(.72,1.06,smoothstep(.16,.58,scales));}
        else if(species<2.5){float lenticel=(1-smoothstep(.025,.075,abs(sin(p.world.y*19+noise))))*smoothstep(.45,.78,abs(sin(p.world.x*8+p.world.z*8)));float basal=saturate(1-p.world.y/2.0);albedo=lerp(albedo,float3(.10,.085,.07),saturate(lenticel*.88+basal*.72));fissure=.92+.12*noise;}
        else if(species<3.5){float grooves=abs(sin(p.world.y*11+p.world.x*7+sin(p.world.z*13)));fissure=lerp(.58,1.06,smoothstep(.10,.40,grooves));}
        else {float plates=abs(sin(p.world.y*8+p.world.x*9)*sin(p.world.z*10));fissure=lerp(.62,1.08,smoothstep(.12,.44,plates));}
        albedo*=fissure*(.92+.16*noise);float rim=pow(1-saturate(dot(n,view)),4)*.10;light+=rim;
    } else if(kind<1.5){
        float veins=.94+.06*sin((p.world.x+p.world.z)*115);albedo*=veins*(front?1.0:.72);float transmission=pow(saturate(-ndl),1.35);light+=albedo*float3(.55,.86,.28)*transmission*1.35;float gloss=pow(saturate(dot(reflect(-sun,n),view)),36)*(front?.20:.04);light+=gloss;
    } else {float mottled=.88+.12*sin(p.world.x*.72)*sin(p.world.z*.61);float canopyShadow=.34*exp(-dot(p.world.xz,p.world.xz)*.045);albedo*=mottled*(1-canopyShadow);}
    float3 color=albedo*light+float3(.018,.022,.014);color=tonemap(color*1.35);color=pow(color,1.0/2.2);float3 sky=float3(.58,.72,.86);
    return float4(lerp(color,sky,p.fog*.20),p.color.a);
})";
}

struct Renderer::Impl {
    HWND window{}; int width=1,height=1; std::wstring lastError;
    ID3D11Device* device{}; ID3D11DeviceContext* context{}; IDXGISwapChain* swap{};
    ID3D11RenderTargetView* target{}; ID3D11Texture2D* depthTexture{}; ID3D11DepthStencilView* depth{};
    ID3D11VertexShader* vs{}; ID3D11PixelShader* ps{}; ID3D11InputLayout* layout{};
    ID3D11Buffer* constants{}; ID3D11Buffer* branchVB{}; ID3D11Buffer* branchIB{}; ID3D11Buffer* leafVB{}; ID3D11Buffer* leafIB{};
    ID3D11RasterizerState* raster{}; ID3D11BlendState* blend{};
    UINT branchIndices{},leafIndices{};

    ~Impl() {
        if(context) context->ClearState();
        release(branchVB);release(branchIB);release(leafVB);release(leafIB);release(constants);release(layout);release(vs);release(ps);
        release(raster);release(blend);release(depth);release(depthTexture);release(target);release(swap);release(context);release(device);
    }
    bool fail(HRESULT hr,const wchar_t* what) { wchar_t b[256]; wsprintfW(b,L"%s (HRESULT 0x%08X)",what,static_cast<unsigned>(hr)); lastError=b; return false; }
    bool makeTargets() {
        ID3D11Texture2D* back{}; HRESULT hr=swap->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&back));
        if(FAILED(hr)) return fail(hr,L"Could not get swap-chain buffer");
        hr=device->CreateRenderTargetView(back,nullptr,&target); release(back); if(FAILED(hr)) return fail(hr,L"Could not create render target");
        D3D11_TEXTURE2D_DESC d{}; d.Width=static_cast<UINT>(width);d.Height=static_cast<UINT>(height);d.MipLevels=1;d.ArraySize=1;
        d.Format=DXGI_FORMAT_D32_FLOAT;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        hr=device->CreateTexture2D(&d,nullptr,&depthTexture);if(FAILED(hr)) return fail(hr,L"Could not create depth texture");
        hr=device->CreateDepthStencilView(depthTexture,nullptr,&depth);if(FAILED(hr)) return fail(hr,L"Could not create depth view"); return true;
    }
    template<class T> ID3D11Buffer* buffer(const std::vector<T>& data,UINT bind) {
        if(data.empty()) return nullptr;
        D3D11_BUFFER_DESC d{}; d.ByteWidth=static_cast<UINT>(data.size()*sizeof(T));d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data.data(); ID3D11Buffer* out{};
        if(FAILED(device->CreateBuffer(&d,&initial,&out))) return nullptr;
        return out;
    }
};

Renderer::Renderer():impl_(std::make_unique<Impl>()){} Renderer::~Renderer()=default;

bool Renderer::initialize(HWND window,int width,int height) {
    auto& i=*impl_;i.window=window;i.width=std::max(1,width);i.height=std::max(1,height);
    DXGI_SWAP_CHAIN_DESC sd{};sd.BufferDesc.Width=i.width;sd.BufferDesc.Height=i.height;sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.OutputWindow=window;sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL requested[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL acquired{};
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,requested,2,D3D11_SDK_VERSION,&sd,&i.swap,&i.device,&acquired,&i.context);
    if(FAILED(hr)) { hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,requested+1,1,D3D11_SDK_VERSION,&sd,&i.swap,&i.device,&acquired,&i.context); }
    if(FAILED(hr)) return i.fail(hr,L"Direct3D hardware device creation failed");
    if(!i.makeTargets()) return false;

    ID3DBlob *vsBlob{},*psBlob{},*errors{};
    hr=D3DCompile(shaderSource,strlen(shaderSource),"DenseTrees.hlsl",nullptr,nullptr,"VSMain","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vsBlob,&errors);
    if(FAILED(hr)) { if(errors)i.lastError=std::wstring(static_cast<const char*>(errors->GetBufferPointer()),static_cast<const char*>(errors->GetBufferPointer())+errors->GetBufferSize());release(errors);return false; }
    hr=i.device->CreateVertexShader(vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),nullptr,&i.vs);if(FAILED(hr)){release(vsBlob);return i.fail(hr,L"Vertex shader creation failed");}
    hr=D3DCompile(shaderSource,strlen(shaderSource),"DenseTrees.hlsl",nullptr,nullptr,"PSMain","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&psBlob,&errors);
    if(FAILED(hr)){release(vsBlob);release(errors);return i.fail(hr,L"Pixel shader compilation failed");}
    hr=i.device->CreatePixelShader(psBlob->GetBufferPointer(),psBlob->GetBufferSize(),nullptr,&i.ps);release(psBlob);if(FAILED(hr)){release(vsBlob);return i.fail(hr,L"Pixel shader creation failed");}
    D3D11_INPUT_ELEMENT_DESC elements[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32_FLOAT,0,28,D3D11_INPUT_PER_VERTEX_DATA,0}};
    hr=i.device->CreateInputLayout(elements,4,vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),&i.layout);release(vsBlob);if(FAILED(hr))return i.fail(hr,L"Input layout creation failed");
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=96;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    hr=i.device->CreateBuffer(&cb,nullptr,&i.constants);if(FAILED(hr))return i.fail(hr,L"Constant buffer creation failed");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    i.device->CreateRasterizerState(&rd,&i.raster);
    D3D11_BLEND_DESC bd{};bd.RenderTarget[0].BlendEnable=TRUE;bd.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;bd.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    i.device->CreateBlendState(&bd,&i.blend);return true;
}

void Renderer::resize(int width,int height) {
    auto& i=*impl_;if(!i.swap||width<=0||height<=0)return;i.width=width;i.height=height;
    i.context->OMSetRenderTargets(0,nullptr,nullptr);release(i.target);release(i.depth);release(i.depthTexture);
    if(SUCCEEDED(i.swap->ResizeBuffers(0,static_cast<UINT>(width),static_cast<UINT>(height),DXGI_FORMAT_UNKNOWN,0)))i.makeTargets();
}

void Renderer::setTree(const TreeMesh& tree) {
    auto& i=*impl_;release(i.branchVB);release(i.branchIB);release(i.leafVB);release(i.leafIB);
    std::vector<MeshVertex> branches=tree.branchVertices;std::vector<uint32_t> indices=tree.branchIndices;
    const uint32_t base=static_cast<uint32_t>(branches.size());const uint32_t ground=0xff59704au;
    branches.insert(branches.end(),{{{-18,0,-18},{0,1,0},ground,2},{{-18,0,18},{0,1,0},ground,2},{{18,0,18},{0,1,0},ground,2},{{18,0,-18},{0,1,0},ground,2}});
    indices.insert(indices.end(),{base,base+1,base+2,base,base+2,base+3});
    i.branchVB=i.buffer(branches,D3D11_BIND_VERTEX_BUFFER);i.branchIB=i.buffer(indices,D3D11_BIND_INDEX_BUFFER);i.branchIndices=static_cast<UINT>(indices.size());
    i.leafVB=i.buffer(tree.leafVertices,D3D11_BIND_VERTEX_BUFFER);i.leafIB=i.buffer(tree.leafIndices,D3D11_BIND_INDEX_BUFFER);i.leafIndices=static_cast<UINT>(tree.leafIndices.size());
}

void Renderer::render(float yaw,float pitch,float distance,float sunAzimuth) {
    auto& i=*impl_;if(!i.target)return;const float clear[4]={.58f,.72f,.86f,1};i.context->ClearRenderTargetView(i.target,clear);i.context->ClearDepthStencilView(i.depth,D3D11_CLEAR_DEPTH,1,0);
    const Vec3 target{0,4.1f,0},eye=target+Vec3{std::sin(yaw)*std::cos(pitch)*distance,std::sin(pitch)*distance,-std::cos(yaw)*std::cos(pitch)*distance};
    const float time=static_cast<float>(GetTickCount64()%1000000)*.001f;
    struct Constants { Mat4 vp; float sun[4]; float cameraTime[4]; } c{multiply(lookAt(eye,target,{0,1,0}),perspective(52*pi/180.0f,static_cast<float>(i.width)/i.height,.08f,100.0f)),{-std::sin(sunAzimuth),-1.35f,-std::cos(sunAzimuth),0},{eye.x,eye.y,eye.z,time}};
    i.context->UpdateSubresource(i.constants,0,nullptr,&c,0,0);D3D11_VIEWPORT viewport{0,0,static_cast<float>(i.width),static_cast<float>(i.height),0,1};
    i.context->RSSetViewports(1,&viewport);i.context->RSSetState(i.raster);i.context->OMSetRenderTargets(1,&i.target,i.depth);i.context->OMSetBlendState(i.blend,nullptr,0xffffffff);
    i.context->IASetInputLayout(i.layout);i.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);i.context->VSSetShader(i.vs,nullptr,0);i.context->VSSetConstantBuffers(0,1,&i.constants);i.context->PSSetShader(i.ps,nullptr,0);
    UINT stride=sizeof(MeshVertex),offset=0;
    if(i.branchVB&&i.branchIB){i.context->IASetVertexBuffers(0,1,&i.branchVB,&stride,&offset);i.context->IASetIndexBuffer(i.branchIB,DXGI_FORMAT_R32_UINT,0);i.context->DrawIndexed(i.branchIndices,0,0);}
    if(i.leafVB&&i.leafIB){i.context->IASetVertexBuffers(0,1,&i.leafVB,&stride,&offset);i.context->IASetIndexBuffer(i.leafIB,DXGI_FORMAT_R32_UINT,0);i.context->DrawIndexed(i.leafIndices,0,0);}
    i.swap->Present(1,0);
}

const wchar_t* Renderer::error() const{return impl_->lastError.c_str();}
}
