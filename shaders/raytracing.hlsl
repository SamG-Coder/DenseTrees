struct Vertex { float3 position; float3 normal; uint color; float material; float2 uv; };
struct Camera {
    float3 eye; float tanHalfFov;
    float3 forward; float aspect;
    float3 right; uint frameIndex;
    float3 up; uint maxFrames;
    float3 sunDirection; float exposure;
    uint2 resolution; float timeSeconds; float windStrength;
    float4 atmosphere;
};
struct RadiancePayload { float3 color; uint depth; };
struct VisibilityPayload { uint visible; };
struct GrassPatch {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
    uint seed, packed;
    float baseY, normalX, normalZ, moisture;
};
struct GrassAttributes { float2 encoded; };

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<Vertex> Vertices : register(t1);
StructuredBuffer<uint> Indices : register(t2);
Texture2D<float4> BarkNormal : register(t3);
StructuredBuffer<GrassPatch> GrassPatches : register(t4);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> Accumulation : register(u1);
ConstantBuffer<Camera> camera : register(b0);

float3 srgbToLinear(float3 c) { return pow(c, 2.2); }
float3 unpackColor(uint packed) { return float3(packed&255,(packed>>8)&255,(packed>>16)&255)/255.0; }
float3 linearToSrgb(float3 c) { return pow(saturate(c), 1.0 / 2.2); }
float3 tonemap(float3 x) { return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14)); }
float hash(float2 p) { float3 p3=frac(float3(p.xyx)*.1031);p3+=dot(p3,p3.yzx+33.33);return frac((p3.x+p3.y)*p3.z); }
uint hashUint(uint x) { x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x; }
float randomUint(uint x) { return float(hashUint(x)&0x00ffffffu)*(1.0/16777216.0); }
float valueNoise(float2 p) {
    int2 cell=int2(floor(p));float2 f=frac(p);f=f*f*(3-2*f);
    float a=hash(float2(cell)),b=hash(float2(cell+int2(1,0)));
    float c=hash(float2(cell+int2(0,1))),d=hash(float2(cell+int2(1,1)));
    return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
float fbm(float2 p) {
    float result=0,weight=.54;
    [unroll] for(uint octave=0;octave<4;++octave){result+=valueNoise(p)*weight;p=p*2.03+17.17;weight*=.48;}
    return result;
}
float filteredValueNoise(float2 world,float frequency,float footprint) {
    float filter=1-smoothstep(.25,.75,frequency*footprint);
    return lerp(.5,valueNoise(world*frequency),filter);
}
float filteredFbmWorld(float2 world,float baseFrequency,float footprint) {
    float2 p=world*baseFrequency;float frequency=baseFrequency,result=0,weight=.54;
    [unroll] for(uint octave=0;octave<4;++octave){float filter=1-smoothstep(.25,.75,frequency*footprint);result+=lerp(.5,valueNoise(p),filter)*weight;p=p*2.03+17.17;frequency*=2.03;weight*=.48;}
    return result;
}
float hash3(float3 p) {
    p=frac(p*.1031);p+=dot(p,p.yzx+33.33);return frac((p.x+p.y)*p.z);
}
float valueNoise3(float3 p) {
    int3 cell=int3(floor(p));float3 f=frac(p);f=f*f*(3-2*f);
    float n000=hash3(float3(cell)),n100=hash3(float3(cell+int3(1,0,0)));
    float n010=hash3(float3(cell+int3(0,1,0))),n110=hash3(float3(cell+int3(1,1,0)));
    float n001=hash3(float3(cell+int3(0,0,1))),n101=hash3(float3(cell+int3(1,0,1)));
    float n011=hash3(float3(cell+int3(0,1,1))),n111=hash3(float3(cell+int3(1,1,1)));
    return lerp(lerp(lerp(n000,n100,f.x),lerp(n010,n110,f.x),f.y),
                lerp(lerp(n001,n101,f.x),lerp(n011,n111,f.x),f.y),f.z);
}
float fbm3(float3 p) {
    float result=0,weight=.53;
    [unroll] for(uint octave=0;octave<4;++octave){result+=valueNoise3(p)*weight;p=p*2.07+19.31;weight*=.47;}
    return result;
}
float rockRelief(float3 p,float variant) {
    float coarse=fbm3(p*2.7+variant*17.0),fine=fbm3(p*13.5+31.0+variant*7.0);
    float layered=1-abs(valueNoise3(p*5.2+variant*11.0)*2-1);
    return coarse*.68+fine*.22-pow(saturate(layered),10)*.18;
}
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

float3 directionToSun() { return -normalize(camera.sunDirection); }
float3 skyIrradiance(float3 normal) {
    float up=saturate(normal.y*.5+.5),sunHeight=saturate(directionToSun().y);
    float3 horizon=lerp(float3(.38,.46,.53),float3(.58,.69,.78),sunHeight);
    float3 zenith=lerp(float3(.075,.13,.22),float3(.16,.31,.52),sunHeight);
    return lerp(horizon,zenith,pow(up,.58))*(.72+.18*sunHeight);
}
float3 environmentRadiance(float3 direction) {
    float3 d=normalize(direction),sun=directionToSun();float up=saturate(d.y);
    float sunHeight=saturate(sun.y),horizonHaze=exp(-up*7.5);
    float3 horizon=lerp(float3(.54,.38,.29),float3(.43,.64,.82),sunHeight);
    float3 zenith=lerp(float3(.055,.075,.16),float3(.035,.16,.43),sunHeight);
    float3 color=lerp(horizon,zenith,pow(up,.58));
    color+=horizonHaze*lerp(float3(.20,.13,.08),float3(.12,.16,.18),sunHeight);
    float mu=saturate(dot(d,sun));
    float disk=smoothstep(cos(.0058),cos(.0042),mu);
    float aureole=pow(mu,48)*(.20+.42*camera.atmosphere.y);
    color+=disk*float3(14.0,11.2,7.2)+aureole*float3(1.6,1.18,.72);

    if(d.y>.018){
        float2 cloudPoint=d.xz/max(d.y,.075)*.52;
        cloudPoint+=float2(camera.timeSeconds*.006,-camera.timeSeconds*.0025);
        float cloudNoise=.74*fbm(cloudPoint)+.26*fbm(cloudPoint*2.7+31.4);
        float cloudThreshold=lerp(.71,.57,camera.atmosphere.z);
        float coverage=smoothstep(cloudThreshold,cloudThreshold+.105,cloudNoise);
        coverage*=smoothstep(.025,.20,d.y);
        float silver=pow(saturate(dot(d,sun)),10);
        float3 cloudColor=lerp(float3(.50,.55,.59),float3(1.06,1.03,.95),
                               .56+.34*sunHeight)+silver*.34;
        color=lerp(color,cloudColor,coverage*.78);
    }
    if(d.y<0)color=lerp(float3(.13,.17,.14),color,saturate(1+d.y*7.0));
    return max(color,0);
}

[shader("raygeneration")]
void RayGen() {
    uint2 pixel=DispatchRaysIndex().xy;float2 jitter=0;
    if(camera.maxFrames>1u)jitter=float2(hash(pixel+camera.frameIndex*17),hash(pixel.yx+camera.frameIndex*31))-.5;
    float2 uv=((float2(pixel)+.5+jitter)/float2(camera.resolution))*2-1;uv.y=-uv.y;
    RayDesc ray;ray.Origin=camera.eye;ray.Direction=normalize(camera.forward+camera.right*uv.x*camera.aspect*camera.tanHalfFov+camera.up*uv.y*camera.tanHalfFov);ray.TMin=.02;ray.TMax=1000;
    RadiancePayload payload;payload.color=0;payload.depth=0;TraceRay(Scene,RAY_FLAG_NONE,0x3,0,0,0,ray,payload);
    float4 previous=Accumulation[pixel];float history=min(float(camera.frameIndex),float(max(camera.maxFrames,1u)-1u));float3 accumulated=(previous.rgb*history+payload.color)/(history+1);
    Accumulation[pixel]=float4(accumulated,1);Output[pixel]=float4(linearToSrgb(tonemap(accumulated*camera.exposure)),1);
}

[shader("miss")]
void RadianceMiss(inout RadiancePayload payload) { payload.color=environmentRadiance(WorldRayDirection()); }
[shader("miss")]
void VisibilityMiss(inout VisibilityPayload payload) { payload.visible=1; }
[shader("closesthit")]
void VisibilityHit(inout VisibilityPayload payload,in BuiltInTriangleIntersectionAttributes attr) { payload.visible=0; }

struct BladeData {
    float3 base;
    float3 normal;
    float3 side;
    float3 naturalLean;
    float height;
    float halfWidth;
    float phase;
    float stiffness;
    float dryness;
    float tall;
    float species;
};

BladeData makeBlade(GrassPatch patch,uint bladeIndex) {
    BladeData blade;
    blade.normal=normalize(float3(patch.normalX,
        sqrt(saturate(1-patch.normalX*patch.normalX-patch.normalZ*patch.normalZ)),
        patch.normalZ));
    float3 axisX=normalize(float3(1,-blade.normal.x/max(blade.normal.y,.25),0));
    float3 axisZ=normalize(cross(axisX,blade.normal));
    uint seed=hashUint(patch.seed^((bladeIndex+1u)*0x9e3779b9u));
    uint tallCount=(patch.packed>>16)&255u;
    blade.tall=bladeIndex<tallCount?1.0:0.0;
    float radius=sqrt(randomUint(seed))*lerp(.245,.065,blade.tall);
    float offsetAngle=randomUint(seed^0x68bc21ebu)*6.2831853;
    float clusterAngle=randomUint(patch.seed^0x91e10da5u)*6.2831853;
    float clusterRadius=randomUint(patch.seed^0x243f6a88u)*.095*blade.tall;
    blade.base=float3((patch.minX+patch.maxX)*.5,patch.baseY,
                      (patch.minZ+patch.maxZ)*.5)
              +axisX*(cos(offsetAngle)*radius+cos(clusterAngle)*clusterRadius)
              +axisZ*(sin(offsetAngle)*radius+sin(clusterAngle)*clusterRadius);
    float bladeAngle=randomUint(seed^0x02e5be93u)*6.2831853;
    blade.side=normalize(axisX*cos(bladeAngle)+axisZ*sin(bladeAngle));
    blade.naturalLean=normalize(cross(blade.side,blade.normal));
    blade.species=float(patch.seed%3u);
    float shortMaximum=float((patch.packed>>8)&255u)*.004;
    float tallMaximum=float((patch.packed>>24)&255u)*.004;
    float maximumHeight=lerp(shortMaximum,tallMaximum,blade.tall);
    blade.height=maximumHeight*lerp(.50,1.0,randomUint(seed^0xa511e9b3u));
    blade.halfWidth=lerp(lerp(.0032,.0085,randomUint(seed^0x63d83595u)),
                         lerp(.0058,.0145,randomUint(seed^0x63d83595u)),blade.tall)
                   *lerp(.88,1.16,patch.moisture);
    float individualPhase=randomUint(seed^0xb5297a4du)*6.2831853;
    float coherentPhase=randomUint(patch.seed^0xd1b54a35u)*6.2831853;
    blade.phase=lerp(individualPhase,coherentPhase,.78*blade.tall);
    blade.stiffness=lerp(lerp(.36,.88,randomUint(seed^0x1b56c4e9u)),
                         lerp(.24,.62,randomUint(seed^0x1b56c4e9u)),blade.tall);
    blade.dryness=randomUint(seed^0xc2b2ae35u);
    return blade;
}

float3 grassWindDirection(float3 normal) {
    float directionWave=.18*sin(camera.timeSeconds*.19);
    float3 wind=normalize(float3(.82+directionWave,0,.57-directionWave));
    return normalize(wind-normal*dot(wind,normal));
}

float grassGust(BladeData blade) {
    float traveling=camera.timeSeconds*1.36+dot(blade.base.xz,float2(.23,.17))+blade.phase;
    float gust=.56+.25*sin(traveling)+.14*sin(traveling*2.31+1.7)
              +.05*sin(camera.timeSeconds*7.2+blade.phase*3.0);
    return saturate(gust);
}

float3 bladeCenter(BladeData blade,float along) {
    float s=saturate(along),shape=s*s*(2-s),gust=grassGust(blade);
    float compliance=lerp(.43,.17,blade.stiffness)*lerp(1.0,1.18,blade.tall);
    float bend=blade.height*camera.windStrength*compliance*gust;
    float flutter=sin(camera.timeSeconds*(6.5+2.5*(1-blade.stiffness))+blade.phase+s*5.0)
                 *blade.height*.013*camera.windStrength*s*s;
    return blade.base+blade.normal*(blade.height*s)
         +blade.naturalLean*(blade.height*lerp(.055,.10,blade.tall)*shape)
         +grassWindDirection(blade.normal)*(bend*shape)+blade.side*flutter;
}

bool rayTriangle(float3 origin,float3 direction,float3 a,float3 b,float3 c,
                 float minimumT,inout float maximumT,out float2 barycentric) {
    float3 e1=b-a,e2=c-a,p=cross(direction,e2);float determinant=dot(e1,p);
    if(abs(determinant)<1e-7)return false;
    float inverse=1.0/determinant;float3 tvec=origin-a;
    float u=dot(tvec,p)*inverse;if(u<0||u>1)return false;
    float3 q=cross(tvec,e1);float v=dot(direction,q)*inverse;
    if(v<0||u+v>1)return false;
    float t=dot(e2,q)*inverse;if(t<minimumT||t>=maximumT)return false;
    maximumT=t;barycentric=float2(u,v);return true;
}

[shader("intersection")]
void GrassIntersection() {
    GrassPatch patch=GrassPatches[PrimitiveIndex()];
    uint candidateCount=min(patch.packed&255u,16u);
    uint tallCount=min((patch.packed>>16)&255u,candidateCount);
    float patchDistance=distance(camera.eye,float3((patch.minX+patch.maxX)*.5,
                                                   patch.baseY,
                                                   (patch.minZ+patch.maxZ)*.5));
    if(patchDistance>=36.0||(tallCount==0&&patchDistance>=26.0))return;
    uint activeCount=patchDistance>=26.0?tallCount:candidateCount;
    float pixelFootprint=2*patchDistance*camera.tanHalfFov/max(1.0,(float)camera.resolution.y);
    float targetHalfWidth=.55*pixelFootprint;
    bool visibilityRay=(RayFlags()&RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH)!=0;
    float bestT=RayTCurrent();GrassAttributes bestAttribute;bool found=false;
    float3 rayOrigin=ObjectRayOrigin(),rayDirection=ObjectRayDirection();
    [loop] for(uint bladeIndex=0;bladeIndex<activeCount;++bladeIndex) {
        BladeData blade=makeBlade(patch,bladeIndex);
        float distanceCoverage=1-smoothstep(blade.tall>.5?20.0:14.0,
                                            blade.tall>.5?36.0:26.0,patchDistance);
        if(distanceCoverage<=0)continue;
        float lodDensity=lerp(blade.tall>.5?.46:.27,1.0,
                              1-smoothstep(8.0,blade.tall>.5?38.0:31.0,patchDistance));
        uint selection=patch.seed^((bladeIndex+19u)*0x27d4eb2du);
        if(!(blade.tall>.5&&bladeIndex==0)&&randomUint(selection)>lodDensity)continue;
        uint2 rayPixel=DispatchRaysIndex().xy;
        uint coverageSeed=selection^(rayPixel.x*0x85ebca6bu)^(rayPixel.y*0xc2b2ae35u);
        float coverageThreshold=randomUint(coverageSeed);
        uint segments=blade.tall>.5?(patchDistance<16.0?3u:2u):
                                      (patchDistance<10.0?3u:(patchDistance<18.0?2u:1u));
        [loop] for(uint segment=0;segment<segments;++segment) {
            float s0=float(segment)/segments,s1=float(segment+1u)/segments;
            float3 p0=bladeCenter(blade,s0),p1=bladeCenter(blade,s1);
            float seed0=blade.tall*step(.68,blade.dryness)*smoothstep(.68,.76,s0)*(1-smoothstep(.94,1.0,s0));
            float seed1=blade.tall*step(.68,blade.dryness)*smoothstep(.68,.76,s1)*(1-smoothstep(.94,1.0,s1));
            float physicalW0=blade.halfWidth*(pow(max(1-s0,.015),.72)+seed0*1.65)+.00015;
            float physicalW1=blade.halfWidth*(pow(max(1-s1,.015),.72)+seed1*1.65)+.00015;
            float widthCap=blade.tall>.5?.043:.018;
            float renderW0=min(max(physicalW0,targetHalfWidth),widthCap);
            float renderW1=min(max(physicalW1,targetHalfWidth),widthCap);
            float3 left0=p0-blade.side*renderW0,right0=p0+blade.side*renderW0;
            float3 left1=p1-blade.side*renderW1,right1=p1+blade.side*renderW1;
            float2 triangleBary;
            float candidateT=bestT;
            if(rayTriangle(rayOrigin,rayDirection,left0,right0,left1,RayTMin(),candidateT,
                           triangleBary)){
                float across=triangleBary.x;
                float along=lerp(s0,s1,triangleBary.y);
                float local=saturate((along-s0)*segments);
                float coverage=distanceCoverage*saturate(lerp(physicalW0,physicalW1,local)/
                                                           max(lerp(renderW0,renderW1,local),1e-5));
                if(coverageThreshold<coverage){bestT=candidateT;bestAttribute.encoded=float2(float(bladeIndex)+.10+.80*across,along);found=true;}
            }
            candidateT=bestT;
            if(rayTriangle(rayOrigin,rayDirection,right0,right1,left1,RayTMin(),candidateT,
                           triangleBary)){
                float across=1-triangleBary.y;
                float along=s0*(1-triangleBary.x-triangleBary.y)
                           +s1*(triangleBary.x+triangleBary.y);
                float local=saturate((along-s0)*segments);
                float coverage=distanceCoverage*saturate(lerp(physicalW0,physicalW1,local)/
                                                           max(lerp(renderW0,renderW1,local),1e-5));
                if(coverageThreshold<coverage){bestT=candidateT;bestAttribute.encoded=float2(float(bladeIndex)+.10+.80*across,along);found=true;}
            }
        }
        if(found&&visibilityRay&&ReportHit(bestT,0,bestAttribute))return;
    }
    if(found)ReportHit(bestT,0,bestAttribute);
}

[shader("closesthit")]
void GrassRadianceHit(inout RadiancePayload payload,in GrassAttributes attr) {
    GrassPatch patch=GrassPatches[PrimitiveIndex()];uint bladeIndex=(uint)floor(attr.encoded.x);
    BladeData blade=makeBlade(patch,bladeIndex);float along=saturate(attr.encoded.y);
    float epsilon=.012;float3 tangent=normalize(bladeCenter(blade,min(1.0,along+epsilon))
                                              -bladeCenter(blade,max(0.0,along-epsilon)));
    float3 geometricNormal=normalize(cross(blade.side,tangent));
    bool front=dot(geometricNormal,WorldRayDirection())<0;
    float3 n=front?geometricNormal:-geometricNormal;
    float3 hit=WorldRayOrigin()+WorldRayDirection()*RayTCurrent();
    float dryThreshold=lerp(.16,.055,patch.moisture);
    float dry=1-smoothstep(dryThreshold-.025,dryThreshold+.025,blade.dryness);
    float3 green=lerp(float3(.070,.175,.025),float3(.24,.47,.075),
                      .44+.56*patch.moisture);
    green*=blade.species<.5?float3(.94,1.07,.82):
           (blade.species<1.5?float3(1.10,.98,.72):float3(.82,1.02,1.05));
    green*=lerp(.55,1.12,smoothstep(0,.72,along));
    float3 straw=float3(.34,.29,.105)*lerp(.72,1.15,along);
    float3 albedo=lerp(green,straw,dry);
    float3 sun=directionToSun();uint2 pixel=DispatchRaysIndex().xy;
    float2 random=float2(hash(pixel+camera.frameIndex*131),
                         hash(pixel.yx+camera.frameIndex*173));
    float3 sunTangent=normalize(cross(abs(sun.y)<.9?float3(0,1,0):float3(1,0,0),sun));
    float3 sunBitangent=cross(sun,sunTangent);float angle=random.y*6.2831853;
    sun=normalize(sun+sunTangent*cos(angle)*sqrt(random.x)*.0065
                     +sunBitangent*sin(angle)*sqrt(random.x)*.0065);
    VisibilityPayload shadow;shadow.visible=0;RayDesc ray;ray.Origin=hit+n*.004;
    ray.Direction=sun;ray.TMin=.003;ray.TMax=1000;
    TraceRay(Scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0x3,1,0,1,ray,shadow);
    float frontLight=saturate(dot(n,sun)),backLight=saturate(dot(-n,sun));
    float3 ambient=skyIrradiance(n)*(.50+.18*along);
    float3 direct=float3(1.02,.94,.73)*frontLight*shadow.visible*1.18;
    direct+=albedo*float3(.42,.74,.20)*backLight*shadow.visible*.72;
    float3 view=normalize(camera.eye-hit),halfVector=normalize(sun+view);
    direct+=pow(saturate(dot(n,halfVector)),22)*.08*shadow.visible;
    float seedHead=blade.tall*smoothstep(.70,.79,along)*(1-smoothstep(.92,1.0,along))
                  *step(.68,blade.dryness);
    albedo=lerp(albedo,float3(.30,.27,.10),seedHead*.46);
    float3 result=albedo*(ambient+direct)*lerp(.58,1.0,smoothstep(0,.22,along));
    float haze=(1-exp(-distance(camera.eye,hit)*.0045))*
               lerp(.55,1.0,1-saturate(WorldRayDirection().y));
    payload.color=lerp(result,environmentRadiance(WorldRayDirection()),haze*.45);
}

[shader("closesthit")]
void RadianceHit(inout RadiancePayload payload,in BuiltInTriangleIntersectionAttributes attr) {
    uint primitive=PrimitiveIndex();uint i0=Indices[primitive*3],i1=Indices[primitive*3+1],i2=Indices[primitive*3+2];
    float3 bary=float3(1-attr.barycentrics.x-attr.barycentrics.y,attr.barycentrics.x,attr.barycentrics.y);
    Vertex a=Vertices[i0],b=Vertices[i1],c=Vertices[i2];float3 geometricNormal=normalize(a.normal*bary.x+b.normal*bary.y+c.normal*bary.z);bool upperFace=dot(geometricNormal,WorldRayDirection())<0;float3 surfaceNormal=upperFace?geometricNormal:-geometricNormal;float3 n=surfaceNormal;float2 uv=a.uv*bary.x+b.uv*bary.y+c.uv*bary.z;
    float3 hit=WorldRayOrigin()+WorldRayDirection()*RayTCurrent();float3 albedo=srgbToLinear(unpackColor(a.color)*bary.x+unpackColor(b.color)*bary.y+unpackColor(c.color)*bary.z);
    float material=a.material;float kind=floor(material+.001);bool thinFoliage=(kind>.5&&kind<1.5)||(kind>3.5&&kind<4.5);uint2 pixel=DispatchRaysIndex().xy;float2 random=float2(hash(pixel+camera.frameIndex*13),hash(pixel.yx+camera.frameIndex*29));
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
    if(kind>1.5&&kind<2.5){
        float viewDistance=distance(camera.eye,hit);
        float frequency=lerp(2.0,.14,smoothstep(8.0,170.0,viewDistance));
        float sampleStep=.18/frequency;
        float dx=(fbm((hit.xz+float2(sampleStep,0))*frequency)-
                  fbm((hit.xz-float2(sampleStep,0))*frequency))/(2*sampleStep);
        float dz=(fbm((hit.xz+float2(0,sampleStep))*frequency)-
                  fbm((hit.xz-float2(0,sampleStep))*frequency))/(2*sampleStep);
        n=normalize(n+float3(-dx*.045,0,-dz*.045)*(1-saturate(1-surfaceNormal.y)));
    }
    if(kind>2.5&&kind<3.5){
        float rockVariant=round(frac(material)*10);float epsilon=.014;
        float hx0=rockRelief(hit-float3(epsilon,0,0),rockVariant),hx1=rockRelief(hit+float3(epsilon,0,0),rockVariant);
        float hy0=rockRelief(hit-float3(0,epsilon,0),rockVariant),hy1=rockRelief(hit+float3(0,epsilon,0),rockVariant);
        float hz0=rockRelief(hit-float3(0,0,epsilon),rockVariant),hz1=rockRelief(hit+float3(0,0,epsilon),rockVariant);
        float3 gradient=float3(hx1-hx0,hy1-hy0,hz1-hz0)/(2*epsilon);
        gradient-=surfaceNormal*dot(gradient,surfaceNormal);
        n=normalize(surfaceNormal-gradient*lerp(.030,.052,saturate(rockVariant*.5)));
        float2 stoneCoordinates=float2(dot(hit,float3(.73,.27,.19)),dot(hit,float3(-.21,.46,.81)));
        float mineral=valueNoise(stoneCoordinates*4.2);float fissure=pow(saturate(1-abs(valueNoise3(hit*5.4+rockVariant*9.0)*2-1)),13);
        float lichen=smoothstep(.56,.90,geometricNormal.y)*smoothstep(.60,.84,fbm(hit.xz*.23+11.7));
        float3 speciesTone=rockVariant<.5?float3(1.02,.98,.91):(rockVariant<1.5?float3(.94,.97,1.03):float3(.88,.91,.86));
        albedo*=speciesTone*lerp(.76,1.14,mineral);albedo=lerp(albedo,srgbToLinear(float3(.055,.048,.039)),fissure*.70);albedo=lerp(albedo,srgbToLinear(float3(.18,.22,.075)),lichen*.34);
    }
    if(kind>4.5&&kind<5.5){
        float grain=valueNoise(float2(uv.x*7.0+uv.y*.15,uv.y*2.1));albedo*=lerp(.72,1.10,grain);
    }
    float3 baseSun=directionToSun(),sunTangent=normalize(cross(abs(baseSun.y)<.9?float3(0,1,0):float3(1,0,0),baseSun)),sunBitangent=cross(baseSun,sunTangent);float diskRadius=sqrt(random.x)*.0065,angle=random.y*6.2831853;float3 sunDir=normalize(baseSun+sunTangent*cos(angle)*diskRadius+sunBitangent*sin(angle)*diskRadius);
    VisibilityPayload shadow;shadow.visible=0;RayDesc s;s.Origin=hit+(thinFoliage?sunDir:surfaceNormal)*.012;s.Direction=sunDir;s.TMin=.01;s.TMax=1000;TraceRay(Scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,0x3,1,0,1,s,shadow);
    VisibilityPayload ao;ao.visible=1;if(payload.depth==0){ao.visible=0;RayDesc ar;ar.Origin=hit+surfaceNormal*.015;ar.Direction=cosineHemisphere(n,float2(hash(pixel+camera.frameIndex*47),hash(pixel.yx+camera.frameIndex*71)));ar.TMin=.01;ar.TMax=1.35;TraceRay(Scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,0x1,1,0,1,ar,ao);}
    float visibility=shadow.visible;float ndl=saturate(dot(n,sunDir));float occlusion=lerp(.76,1.0,float(ao.visible));float3 ambient=skyIrradiance(n)*(.48+.16*saturate(n.y))*occlusion;ambient+=float3(.20,.17,.12)*(.08+.14*saturate(-n.y));float3 direct=float3(1.08,.96,.80)*ndl*visibility*1.28;
    if(kind<.5)ambient*=lerp(1,.68,barkCavity);
    if(kind>1.5&&kind<2.5){
        float terrainDistance=distance(camera.eye,hit);
        float pixelWorld=2*terrainDistance*camera.tanHalfFov/max(1.0,(float)camera.resolution.y);
        float grazing=rcp(max(abs(dot(surfaceNormal,-WorldRayDirection())),.22));
        float footprint=min(pixelWorld*grazing,2.0);
        float macro=filteredFbmWorld(hit.xz,.075,footprint),detail=filteredValueNoise(hit.xz,.72,footprint);
        float slope=1-saturate(surfaceNormal.y),rootDistance=length(hit.xz);
        float exposedRoots=1-smoothstep(.75,1.65,rootDistance);
        float soilMask=saturate(smoothstep(.09,.31,slope)+exposedRoots*.82
                                +smoothstep(.18,.76,detail)*.18);
        float3 meadow=lerp(float3(.038,.074,.018),float3(.096,.155,.039),
                           saturate(.28+.62*macro));
        float3 dryGrass=float3(.20,.19,.075);
        meadow=lerp(meadow,dryGrass,smoothstep(.74,.95,macro)*.18);
        float meadowFine=filteredValueNoise(hit.xz,2.8,footprint);
        meadow*=lerp(.82,1.13,meadowFine);
        float2 grassAlong=normalize(float2(.82,.57)),grassAcross=float2(-grassAlong.y,grassAlong.x);
        float2 swardUv=float2(dot(hit.xz,grassAcross)*3.1,dot(hit.xz,grassAlong)*.62);
        float swardFilter=1-smoothstep(.25,.75,3.1*footprint);
        float sward=lerp(.5,valueNoise(swardUv),swardFilter);
        float grassDomain=smoothstep(1.05,1.45,rootDistance);
        float farWeight=grassDomain*(1-soilMask)*smoothstep(14.0,26.0,terrainDistance);
        meadow=lerp(meadow,meadow*lerp(.90,1.08,sward),farWeight);
        float3 soil=lerp(float3(.105,.071,.038),float3(.21,.145,.075),detail);
        albedo=lerp(meadow,soil,soilMask);
        float highGround=smoothstep(8.0,34.0,hit.y)*smoothstep(190,285,rootDistance);
        float mountainStone=saturate(highGround*(.34+.90*slope)+smoothstep(.28,.62,slope)*.38);
        float3 distantStone=lerp(float3(.16,.17,.145),float3(.28,.265,.22),detail);
        albedo=lerp(albedo,distantStone,mountainStone*.78);ambient*=lerp(.79,.91,macro);
    }
    if(kind>.5&&kind<1.5){
        float midrib=exp(-abs(uv.x-.5)*150);float secondary=exp(-abs(frac((uv.y+abs(uv.x-.5)*.72)*6)-.5)*34);float veins=saturate(midrib*.82+secondary*.28);float edge=saturate(length((uv-.5)*float2(1.25,1.0))*2);
        float chlorophyll=lerp(1.08,.76,edge)*lerp(1,.58,veins);albedo*=upperFace?float3(.82,1.0,.76):float3(1.05,1.16,.90);albedo=lerp(albedo,float3(.25,.38,.13),veins*.42);
        float pathLength=(.24+.28*chlorophyll)/max(abs(dot(geometricNormal,sunDir)),.16);float3 absorption=float3(2.55,.72,3.25);float3 transmittance=exp(-absorption*pathLength);float back=saturate(dot(-n,sunDir));direct+=transmittance*float3(1.02,.96,.76)*back*visibility*.68;
        float roughness=upperFace?.34:.58;float3 viewDirection=normalize(camera.eye-hit),halfVector=normalize(sunDir+viewDirection);float ndh=saturate(dot(n,halfVector));float fresnel=.022+.978*pow(1-saturate(dot(n,viewDirection)),5);direct+=fresnel*pow(ndh,lerp(70,18,roughness))*(upperFace?.42:.12);
    }
    if(kind>3.5&&kind<4.5){
        float species=round(frac(material)*10);float mottling=valueNoise(float2(hit.x*1.7+hit.y*.53,hit.z*1.9-hit.y*.37));
        albedo*=lerp(.80,1.16,mottling);albedo*=upperFace?1.0:.78;
        albedo*=species<.5?float3(.96,1.05,.88):(species<1.5?float3(.84,1.03,.92):float3(1.04,.98,.77));
        float back=saturate(dot(-n,sunDir));float3 transmission=species<1.5?float3(.18,.36,.075):float3(.27,.32,.07);
        direct+=transmission*back*visibility*.48;ambient*=.94;
    }
    if(kind>2.5&&kind<3.5)ambient*=.86;
    if(kind>4.5&&kind<5.5)ambient*=.84;
    float3 result=albedo*(ambient+direct);
    if(payload.depth==0){RadiancePayload bounce;bounce.color=0;bounce.depth=1;RayDesc br;br.Origin=hit+n*.018;br.Direction=cosineHemisphere(n,float2(hash(pixel+camera.frameIndex*89),hash(pixel.yx+camera.frameIndex*113)));br.TMin=.01;br.TMax=6;TraceRay(Scene,RAY_FLAG_NONE,0x3,0,0,0,br,bounce);result+=albedo*bounce.color*.075;}
    float haze=(1-exp(-distance(camera.eye,hit)*.0038))*
               lerp(.52,1.0,1-saturate(WorldRayDirection().y));
    payload.color=lerp(result,environmentRadiance(WorldRayDirection()),haze*.48);
}
