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
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> Accumulation : register(u1);
ConstantBuffer<Camera> camera : register(b0);

float3 srgbToLinear(float3 c) { return pow(c, 2.2); }
float3 linearToSrgb(float3 c) { return pow(saturate(c), 1.0 / 2.2); }
float3 tonemap(float3 x) { return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14)); }
float hash(float2 p) { float3 p3=frac(float3(p.xyx)*.1031);p3+=dot(p3,p3.yzx+33.33);return frac((p3.x+p3.y)*p3.z); }
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
    Vertex a=Vertices[i0],b=Vertices[i1],c=Vertices[i2];float3 geometricNormal=normalize(a.normal*bary.x+b.normal*bary.y+c.normal*bary.z);bool upperFace=dot(geometricNormal,WorldRayDirection())<0;float3 n=upperFace?geometricNormal:-geometricNormal;float2 uv=a.uv*bary.x+b.uv*bary.y+c.uv*bary.z;
    float3 hit=WorldRayOrigin()+WorldRayDirection()*RayTCurrent();uint packed=a.color;float3 albedo=srgbToLinear(float3(packed&255,(packed>>8)&255,(packed>>16)&255)/255.0);
    float material=a.material;float kind=floor(material+.001);uint2 pixel=DispatchRaysIndex().xy;float2 random=float2(hash(pixel+camera.frameIndex*13),hash(pixel.yx+camera.frameIndex*29));
    float3 baseSun=-normalize(camera.sunDirection),sunTangent=normalize(cross(abs(baseSun.y)<.9?float3(0,1,0):float3(1,0,0),baseSun)),sunBitangent=cross(baseSun,sunTangent);float diskRadius=sqrt(random.x)*.018,angle=random.y*6.2831853;float3 sunDir=normalize(baseSun+sunTangent*cos(angle)*diskRadius+sunBitangent*sin(angle)*diskRadius);
    VisibilityPayload shadow;shadow.visible=0;RayDesc s;s.Origin=hit+(kind>.5&&kind<1.5?sunDir:n)*.012;s.Direction=sunDir;s.TMin=.01;s.TMax=1000;TraceRay(Scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,0xff,1,0,1,s,shadow);
    VisibilityPayload ao;ao.visible=1;if(payload.depth==0){ao.visible=0;RayDesc ar;ar.Origin=hit+n*.015;ar.Direction=cosineHemisphere(n,float2(hash(pixel+camera.frameIndex*47),hash(pixel.yx+camera.frameIndex*71)));ar.TMin=.01;ar.TMax=1.35;TraceRay(Scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,0xff,1,0,1,ar,ao);}
    float visibility=shadow.visible;float ndl=saturate(dot(n,sunDir));float occlusion=lerp(.76,1.0,float(ao.visible));float3 ambient=sky(n)*(.43+.20*saturate(n.y))*occlusion;ambient+=float3(.20,.17,.12)*(.08+.14*saturate(-n.y));float3 direct=float3(1.08,.96,.80)*ndl*visibility*1.28;
    if(kind<.5){
        // Procedural oak bark: broad vertical plates, narrow dark fissures and
        // fine-scale roughness.  Lighting the crevices provides depth without a
        // repeating image texture.
        float az=atan2(hit.z,hit.x);float plates=abs(sin(az*11.0+hit.y*.43+sin(hit.y*1.7)*.7));
        float verticalCrack=1-smoothstep(.035,.16,plates);float crossCrack=1-smoothstep(.018,.075,abs(sin(hit.y*3.8+az*2.3)));
        float fine=hash(floor(hit.xz*31)+floor(hit.y*19));float crevice=saturate(verticalCrack*.82+crossCrack*.32);
        albedo*=lerp(.91+fine*.14,.25,crevice);ambient*=lerp(1,.66,crevice);
    }
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
