struct Vertex { float3 position; float3 normal; uint color; float material; float2 uv; };
struct Camera {
    float3 eye; float tanHalfFov;
    float3 forward; float aspect;
    float3 right; uint frameIndex;
    float3 up; uint maxFrames;
    float3 sunDirection; float exposure;
    uint2 resolution; uint2 padding;
};
struct RadiancePayload { float3 color; uint depth; };
struct VisibilityPayload { uint visible; };

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<Vertex> Vertices : register(t1);
StructuredBuffer<uint> Indices : register(t2);
Texture2D<float4> BarkNormal : register(t3);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> Accumulation : register(u1);
ConstantBuffer<Camera> camera : register(b0);

float3 srgbToLinear(float3 c) { return pow(c, 2.2); }
float3 unpackColor(uint packed) { return float3(packed&255,(packed>>8)&255,(packed>>16)&255)/255.0; }
float3 linearToSrgb(float3 c) { return pow(saturate(c), 1.0 / 2.2); }
float3 tonemap(float3 x) { return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14)); }
float hash(float2 p) { float3 p3=frac(float3(p.xyx)*.1031);p3+=dot(p3,p3.yzx+33.33);return frac((p3.x+p3.y)*p3.z); }
int wrappedIndex(int value,int size) { int result=value%size;return result<0?result+size:result; }
float4 sampleBarkMip(float2 uv,uint mip) {
    uint width,height,levels;BarkNormal.GetDimensions(mip,width,height,levels);float2 texel=frac(uv)*float2(width,height)-.5;
    int2 base=int2(floor(texel));float2 blend=frac(texel);int2 size=int2(width,height);
    int2 p00=int2(wrappedIndex(base.x,size.x),wrappedIndex(base.y,size.y));
    int2 p10=int2(wrappedIndex(base.x+1,size.x),p00.y);
    int2 p01=int2(p00.x,wrappedIndex(base.y+1,size.y));
    int2 p11=int2(p10.x,p01.y);
    float4 low=lerp(BarkNormal.Load(int3(p00,mip)),BarkNormal.Load(int3(p10,mip)),blend.x);
    float4 high=lerp(BarkNormal.Load(int3(p01,mip)),BarkNormal.Load(int3(p11,mip)),blend.x);
    return lerp(low,high,blend.y);
}
float4 sampleBarkNormal(float2 uv,float requestedMip) { float lod=clamp(requestedMip,0.0,11.0);uint low=(uint)floor(lod),high=min(low+1,11u);return lerp(sampleBarkMip(uv,low),sampleBarkMip(uv,high),frac(lod)); }
float3 cosineHemisphere(float3 n,float2 random) {
    float phi=6.2831853*random.x,r=sqrt(random.y);float3 helper=abs(n.y)<.9?float3(0,1,0):float3(1,0,0);float3 tangent=normalize(cross(helper,n)),bitangent=cross(n,tangent);
    return normalize(tangent*(r*cos(phi))+bitangent*(r*sin(phi))+n*sqrt(1-random.y));
}

float3 sky(float3 d) {
    float h=saturate(d.y*.5+.5);float3 horizon=float3(.52,.68,.84),zenith=float3(.12,.30,.58);
    float sunDisk=pow(saturate(dot(d,-normalize(camera.sunDirection))),900);
    return lerp(horizon,zenith,h)+sunDisk*float3(8.0,6.4,4.4);
}

[shader("raygeneration")]
void RayGen() {
    uint2 pixel=DispatchRaysIndex().xy;float2 jitter=float2(hash(pixel+camera.frameIndex*17),hash(pixel.yx+camera.frameIndex*31))-.5;
    float2 uv=((float2(pixel)+.5+jitter)/float2(camera.resolution))*2-1;uv.y=-uv.y;
    RayDesc ray;ray.Origin=camera.eye;ray.Direction=normalize(camera.forward+camera.right*uv.x*camera.aspect*camera.tanHalfFov+camera.up*uv.y*camera.tanHalfFov);ray.TMin=.02;ray.TMax=1000;
    RadiancePayload payload;payload.color=0;payload.depth=0;TraceRay(Scene,RAY_FLAG_NONE,0xff,0,0,0,ray,payload);
    float4 previous=Accumulation[pixel];float frame=float(camera.frameIndex);float3 accumulated=(previous.rgb*frame+payload.color)/(frame+1);
    Accumulation[pixel]=float4(accumulated,1);Output[pixel]=float4(linearToSrgb(tonemap(accumulated*camera.exposure)),1);
}

[shader("miss")]
void RadianceMiss(inout RadiancePayload payload) { payload.color=sky(WorldRayDirection()); }
[shader("miss")]
void VisibilityMiss(inout VisibilityPayload payload) { payload.visible=1; }
[shader("closesthit")]
void VisibilityHit(inout VisibilityPayload payload,in BuiltInTriangleIntersectionAttributes attr) { payload.visible=0; }

[shader("closesthit")]
void RadianceHit(inout RadiancePayload payload,in BuiltInTriangleIntersectionAttributes attr) {
    uint primitive=PrimitiveIndex();uint i0=Indices[primitive*3],i1=Indices[primitive*3+1],i2=Indices[primitive*3+2];
    float3 bary=float3(1-attr.barycentrics.x-attr.barycentrics.y,attr.barycentrics.x,attr.barycentrics.y);
    Vertex a=Vertices[i0],b=Vertices[i1],c=Vertices[i2];float3 geometricNormal=normalize(a.normal*bary.x+b.normal*bary.y+c.normal*bary.z);bool upperFace=dot(geometricNormal,WorldRayDirection())<0;float3 surfaceNormal=upperFace?geometricNormal:-geometricNormal;float3 n=surfaceNormal;float2 uv=a.uv*bary.x+b.uv*bary.y+c.uv*bary.z;
    float3 hit=WorldRayOrigin()+WorldRayDirection()*RayTCurrent();float3 albedo=srgbToLinear(unpackColor(a.color)*bary.x+unpackColor(b.color)*bary.y+unpackColor(c.color)*bary.z);
    float material=a.material;float kind=floor(material+.001);uint2 pixel=DispatchRaysIndex().xy;float2 random=float2(hash(pixel+camera.frameIndex*13),hash(pixel.yx+camera.frameIndex*29));
    float barkCavity=0;
    if(kind<.5){
        float3 edge1=b.position-a.position,edge2=c.position-a.position;float2 delta1=b.uv-a.uv,delta2=c.uv-a.uv;float determinant=delta1.x*delta2.y-delta1.y*delta2.x;
        if(abs(determinant)>1e-7){
            float inverseDeterminant=1.0/determinant;float3 rawTangent=(edge1*delta2.y-edge2*delta1.y)*inverseDeterminant;float3 rawBitangent=(edge2*delta1.x-edge1*delta2.x)*inverseDeterminant;
            float3 tangent=normalize(rawTangent-surfaceNormal*dot(rawTangent,surfaceNormal));float handedness=dot(cross(tangent,rawBitangent),surfaceNormal)<0?-1.0:1.0;float3 bitangent=normalize(cross(surfaceNormal,tangent))*handedness;
            float distanceToCamera=distance(camera.eye,hit);float worldFootprint=2*distanceToCamera*camera.tanHalfFov/max(1.0,(float)camera.resolution.y);float grazing=rcp(max(abs(dot(surfaceNormal,-WorldRayDirection())),.20));float texelsPerMetre=max(2048.0/max(length(rawTangent),.01),2048.0*.42/max(length(rawBitangent),.01));float barkMip=clamp(log2(max(worldFootprint*texelsPerMetre*grazing,1.0))-.85+(payload.depth>0?.50:0.0),0.0,11.0);
            float4 bark=sampleBarkNormal(float2(uv.x,uv.y*.42),barkMip);float physicalRadius=length(rawTangent)*.15915494,ageStrength=smoothstep(.006,.12,physicalRadius);float2 normalXY=(bark.rg*2-1)*lerp(.10,1.0,ageStrength);float normalZ=sqrt(saturate(1-dot(normalXY,normalXY)));float3 tangentNormal=normalize(float3(normalXY,normalZ));
            n=normalize(tangent*tangentNormal.x+bitangent*tangentNormal.y+surfaceNormal*tangentNormal.z);if(dot(n,WorldRayDirection())>0)n=-n;
            barkCavity=bark.b*lerp(.10,1.0,ageStrength);float plateTone=bark.a,luminance=dot(albedo,float3(.2126,.7152,.0722));float3 oakPlate=lerp(albedo,luminance.xxx,.18+.12*plateTone)*lerp(.86,1.28,plateTone);float3 fissureColor=srgbToLinear(float3(.075,.060,.045));albedo=lerp(oakPlate,fissureColor,smoothstep(.14,.92,barkCavity)*.74);
        }
    }
    float3 baseSun=-normalize(camera.sunDirection),sunTangent=normalize(cross(abs(baseSun.y)<.9?float3(0,1,0):float3(1,0,0),baseSun)),sunBitangent=cross(baseSun,sunTangent);float diskRadius=sqrt(random.x)*.018,angle=random.y*6.2831853;float3 sunDir=normalize(baseSun+sunTangent*cos(angle)*diskRadius+sunBitangent*sin(angle)*diskRadius);
    VisibilityPayload shadow;shadow.visible=0;RayDesc s;s.Origin=hit+(kind>.5&&kind<1.5?sunDir:surfaceNormal)*.012;s.Direction=sunDir;s.TMin=.01;s.TMax=1000;TraceRay(Scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,0xff,1,0,1,s,shadow);
    VisibilityPayload ao;ao.visible=1;if(payload.depth==0){ao.visible=0;RayDesc ar;ar.Origin=hit+surfaceNormal*.015;ar.Direction=cosineHemisphere(n,float2(hash(pixel+camera.frameIndex*47),hash(pixel.yx+camera.frameIndex*71)));ar.TMin=.01;ar.TMax=1.35;TraceRay(Scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,0xff,1,0,1,ar,ao);}
    float visibility=shadow.visible;float ndl=saturate(dot(n,sunDir));float occlusion=lerp(.76,1.0,float(ao.visible));float3 ambient=sky(n)*(.43+.20*saturate(n.y))*occlusion;ambient+=float3(.20,.17,.12)*(.08+.14*saturate(-n.y));float3 direct=float3(1.08,.96,.80)*ndl*visibility*1.28;
    if(kind<.5)ambient*=lerp(1,.68,barkCavity);
    if(kind>1.5){albedo=float3(.13,.22,.095);ambient*=.86;}
    if(kind>.5&&kind<1.5){
        float midrib=exp(-abs(uv.x-.5)*150);float secondary=exp(-abs(frac((uv.y+abs(uv.x-.5)*.72)*6)-.5)*34);float veins=saturate(midrib*.82+secondary*.28);float edge=saturate(length((uv-.5)*float2(1.25,1.0))*2);
        float chlorophyll=lerp(1.08,.76,edge)*lerp(1,.58,veins);albedo*=upperFace?float3(.82,1.0,.76):float3(1.05,1.16,.90);albedo=lerp(albedo,float3(.25,.38,.13),veins*.42);
        float pathLength=(.24+.28*chlorophyll)/max(abs(dot(geometricNormal,sunDir)),.16);float3 absorption=float3(2.55,.72,3.25);float3 transmittance=exp(-absorption*pathLength);float back=saturate(dot(-n,sunDir));direct+=transmittance*float3(1.02,.96,.76)*back*visibility*.68;
        float roughness=upperFace?.34:.58;float3 viewDirection=normalize(camera.eye-hit),halfVector=normalize(sunDir+viewDirection);float ndh=saturate(dot(n,halfVector));float fresnel=.022+.978*pow(1-saturate(dot(n,viewDirection)),5);direct+=fresnel*pow(ndh,lerp(70,18,roughness))*(upperFace?.42:.12);
    }
    float3 result=albedo*(ambient+direct);
    if(payload.depth==0){RadiancePayload bounce;bounce.color=0;bounce.depth=1;RayDesc br;br.Origin=hit+n*.018;br.Direction=cosineHemisphere(n,float2(hash(pixel+camera.frameIndex*89),hash(pixel.yx+camera.frameIndex*113)));br.TMin=.01;br.TMax=6;TraceRay(Scene,RAY_FLAG_NONE,0xff,0,0,0,br,bounce);result+=albedo*bounce.color*.075;}
    payload.color=result;
}
