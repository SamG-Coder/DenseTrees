#include "environment_cb.hlsli"

struct Vertex { float3 position; float3 normal; uint color; float material; float2 uv; };
struct Camera {
    float3 eye; float tanHalfFov;
    float3 forward; float aspect;
    float3 right; uint frameIndex;
    float3 up; uint maxFrames;
    float exposure; float localLightIntensity; float localLightRange;
    float localLightInnerCos;
    uint2 resolution; uint environmentIndexOffset; float localLightOuterCos;
    float4 grassSettings;
    float4 groundSettings;
};
struct RadiancePayload {
    float3 color;
    uint depth;
    float primaryT;
    float primaryKeyVisibility;
};
struct VisibilityPayload { uint visible; };
struct GrassPatch {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
    uint seed, packed;
    float baseY, normalX, normalZ, moisture;
    float colourFertility, colourDryColony, colourLushColony, colourWarmCool;
};
struct GrassAttributes { float2 encoded; };

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<Vertex> Vertices : register(t1);
StructuredBuffer<uint> Indices : register(t2);
Texture2D<float4> BarkNormal : register(t3);
StructuredBuffer<GrassPatch> GrassPatches : register(t4);
Texture2DArray<float4> GroundAlbedoRoughness : register(t5);
Texture2DArray<float4> GroundNormalHeightCavity : register(t6);
SamplerState GroundSampler : register(s0);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> Accumulation : register(u1);
ConstantBuffer<Camera> camera : register(b0);

float3 srgbToLinear(float3 c) { c=saturate(c);return lerp(c/12.92,pow((c+.055)/1.055,2.4),step(.04045,c)); }
float3 unpackColor(uint packed) { return float3(packed&255,(packed>>8)&255,(packed>>16)&255)/255.0; }
float unpackAlpha(uint packed) { return float((packed>>24)&255)/255.0; }
float3 linearToSrgb(float3 c) { c=max(c,0);return lerp(12.92*c,1.055*pow(c,1.0/2.4)-.055,step(.0031308,c)); }
float3 tonemap(float3 x) { return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14)); }
float3 colorGrade(float3 c) {
    float luminance=dot(c,float3(.2126,.7152,.0722));
    c=lerp(luminance.xxx,c,1.055);
    c=(c-.18)*1.015+.18;
    c=pow(saturate(c),.985)*float3(1.006,1.0,.992);
    return saturate(c);
}
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
float4 sampleGroundAlbedo(float2 uv,uint tile,float requestedMip) {
    return GroundAlbedoRoughness.SampleLevel(GroundSampler,float3(uv,float(tile)),
                                             clamp(requestedMip,0.0,10.0));
}
float4 sampleGroundNormal(float2 uv,uint tile,float requestedMip) {
    return GroundNormalHeightCavity.SampleLevel(GroundSampler,float3(uv,float(tile)),
                                                clamp(requestedMip,0.0,10.0));
}
float3 cosineHemisphere(float3 n,float2 random) {
    float phi=6.2831853*random.x,r=sqrt(random.y);float3 helper=abs(n.y)<.9?float3(0,1,0):float3(1,0,0);float3 tangent=normalize(cross(helper,n)),bitangent=cross(n,tangent);
    return normalize(tangent*(r*cos(phi))+bitangent*(r*sin(phi))+n*sqrt(1-random.y));
}

float3 directionToSun() { return normalize(g_SunDirection); }
float3 directionToMoon() { return normalize(g_MoonDirection); }
bool sunIsKeyLight() {
    const float3 luminance=float3(.2126,.7152,.0722);
    return dot(g_SunColor*g_SunIntensity,luminance)>=
           dot(g_MoonColor*g_MoonIntensity,luminance);
}
float3 directionToKeyLight() {
    return sunIsKeyLight()?directionToSun():directionToMoon();
}
float3 keyLightRadiance() {
    return sunIsKeyLight()?
        g_SunColor*g_SunIntensity:g_MoonColor*g_MoonIntensity;
}
float3 lightningRadiance() {
    return float3(.52,.66,1.0)*g_LightningFlash;
}
float daylightAmount() { return smoothstep(-.12,.08,directionToSun().y); }

struct LocalLightSample {
    float3 direction;
    float distance;
    float3 radiance;
    float active;
};

LocalLightSample samplePlayerLocalLight(float3 hit) {
    LocalLightSample sample;
    sample.direction=float3(0,1,0);sample.distance=0;
    sample.radiance=0;sample.active=0;
    if(camera.localLightIntensity<=.001||camera.localLightRange<=.05)return sample;

    // Keep the source player-local but slightly below/right of the eye. Exact
    // eye coincidence would make every primary shadow ray retrace the view ray
    // and therefore hide all cast-shadow parallax from a first-person camera.
    float3 position=camera.eye+camera.forward*.08+camera.right*.15-camera.up*.18;
    float3 toLight=position-hit;
    float distanceSquared=dot(toLight,toLight);
    float rangeSquared=camera.localLightRange*camera.localLightRange;
    if(distanceSquared<=1e-6||distanceSquared>=rangeSquared)return sample;

    float distanceToLight=sqrt(distanceSquared);
    float3 direction=toLight/distanceToLight;
    float cone=1;
    if(camera.localLightOuterCos>-.5){
        float coneCosine=dot(-direction,camera.forward);
        cone=smoothstep(camera.localLightOuterCos,
                        max(camera.localLightInnerCos,
                            camera.localLightOuterCos+1e-4),coneCosine);
    }
    if(cone<=1e-4)return sample;

    float normalizedDistanceSquared=distanceSquared/rangeSquared;
    float rangeWindow=saturate(1-normalizedDistanceSquared*normalizedDistanceSquared);
    float attenuation=rangeWindow*rangeWindow/max(distanceSquared,.25);
    sample.direction=direction;sample.distance=distanceToLight;
    sample.radiance=float3(1.0,.71,.48)*camera.localLightIntensity*attenuation*cone;
    sample.active=1;
    return sample;
}

float playerLocalLightVisibility(float3 hit,float3 direction,float distanceToLight) {
    if(distanceToLight<=.04)return 1;
    VisibilityPayload shadow;shadow.visible=0;
    RayDesc ray;ray.Origin=hit+direction*.012;ray.Direction=direction;
    ray.TMin=.003;ray.TMax=max(distanceToLight-.025,.004);
    TraceRay(Scene,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0x1,1,0,1,ray,shadow);
    return float(shadow.visible);
}

float3 skyIrradiance(float3 normal) {
    float up=saturate(normal.y*.5+.5),daylight=daylightAmount();
    float3 nightHorizon=float3(.008,.013,.030),nightZenith=float3(.0015,.004,.016);
    float3 dayHorizon=float3(.38,.48,.58),dayZenith=float3(.075,.22,.48);
    float3 horizon=lerp(nightHorizon,dayHorizon,daylight);
    float3 zenith=lerp(nightZenith,dayZenith,daylight);
    float3 sky=lerp(horizon,zenith,pow(up,.58));
    sky*=lerp(1.0,.38,g_StormIntensity);
    sky+=g_MoonColor*g_MoonIntensity*(.16+.22*up);
    sky+=lightningRadiance()*(.12+.16*up);
    return max(sky,0);
}

float proceduralStars(float3 direction) {
    float3 d=normalize(direction);
    float2 spherical=float2(atan2(d.z,d.x)*(1.0/6.2831853)+.5,
                            asin(clamp(d.y,-1.0,1.0))*(1.0/3.14159265)+.5);
    float2 grid=spherical*float2(960,480);
    float2 cell=floor(grid),local=frac(grid);
    float seed=hash(cell+float2(17,43));
    float2 starPoint=float2(hash(cell+float2(71,19)),hash(cell+float2(11,97)));
    float radius=lerp(.028,.075,hash(cell+float2(131,7)));
    float sparkle=1-smoothstep(radius*.35,radius,distance(local,starPoint));
    float enabled=smoothstep(.9962,1.0,seed);
    return sparkle*enabled*lerp(.55,2.4,hash(cell+float2(5,211)));
}

float3 environmentRadiance(float3 direction) {
    float3 d=normalize(direction),sun=directionToSun(),moon=directionToMoon();
    float up=saturate(d.y),daylight=daylightAmount();
    float horizonHaze=exp(-up*7.5);
    float3 horizon=lerp(float3(.007,.012,.028),float3(.43,.64,.82),daylight);
    float3 zenith=lerp(float3(.001,.004,.016),float3(.035,.16,.43),daylight);
    float3 color=lerp(horizon,zenith,pow(up,.58));
    color+=horizonHaze*lerp(float3(.004,.007,.015),float3(.12,.16,.18),daylight);

    float sunMu=saturate(dot(d,sun));
    float sunDisk=smoothstep(cos(.0058),cos(.0042),sunMu)*g_SunIntensity;
    float sunAureole=pow(sunMu,48)*(.16+.30*g_SunIntensity);
    color+=sunDisk*g_SunColor*13.0+sunAureole*g_SunColor*1.4;
    float moonMu=saturate(dot(d,moon));
    float moonDisk=smoothstep(cos(.0052),cos(.0040),moonMu)*g_MoonPhase;
    color+=moonDisk*float3(.42,.52,.82)+pow(moonMu,96)*g_MoonColor*g_MoonIntensity*.8;

    float horizonStars=smoothstep(.035,.25,d.y);
    if(g_StarVisibility>.001)
        color+=proceduralStars(d)*g_StarVisibility*horizonStars*
               lerp(float3(.55,.68,1.0),float3(1.0,.72,.48),hash(floor(d.xy*317)));

    if(d.y>.018){
        float2 cloudPoint=d.xz/max(d.y,.075)*.52;
        cloudPoint+=g_WindDirection*(g_Time*g_WindSpeed*.006);
        // Two coherent octaves are enough at sky scale and avoid eight full
        // noise octaves for every miss ray.
        float cloudNoise=.68*valueNoise(cloudPoint)+
                         .32*valueNoise(cloudPoint*2.11+31.4);
        float cloudThreshold=lerp(.73,.51,g_StormIntensity);
        float coverage=smoothstep(cloudThreshold,cloudThreshold+.105,cloudNoise);
        coverage*=smoothstep(.025,.20,d.y);
        float silver=pow(saturate(dot(d,sun)),10)*g_SunIntensity;
        float3 clearCloud=lerp(float3(.10,.12,.15),float3(.96,.98,.94),daylight);
        float3 stormCloud=float3(.055,.065,.078);
        float3 cloudColor=lerp(clearCloud,stormCloud,g_StormIntensity*.88)+silver*.28;
        color=lerp(color,cloudColor,coverage*lerp(.62,.94,g_StormIntensity));
    }
    color=lerp(color,color*.44,g_StormIntensity*.62);
    color+=lightningRadiance()*(.18+.48*horizonHaze);
    if(d.y<0)color=lerp(float3(.010,.014,.012),color,saturate(1+d.y*7.0));
    return max(color,0);
}

float3 clearSkyAirlight(float3 direction) {
    float3 d=normalize(direction);float up=saturate(d.y),daylight=daylightAmount();
    float3 horizon=lerp(float3(.009,.014,.030),float3(.34,.52,.69),daylight);
    float3 zenith=lerp(float3(.002,.005,.018),float3(.035,.16,.43),daylight);
    float3 airlight=lerp(horizon,zenith,pow(up,.58))*lerp(1.0,.52,g_StormIntensity);
    float forwardScatter=pow(saturate(dot(d,directionToKeyLight())),12);
    airlight+=keyLightRadiance()*forwardScatter*.22+lightningRadiance()*.20;
    return max(airlight,0);
}

float3 applyAerialPerspective(float3 radiance,float3 rayOrigin,float3 hit,
                              float3 rayDirection) {
    float distanceToHit=distance(rayOrigin,hit);
    float falloff=max(g_FogHeightFalloff,1e-5);
    float originDensity=exp(-max(rayOrigin.y,0.0)*falloff);
    float middleDensity=exp(-max((rayOrigin.y+hit.y)*.5,0.0)*falloff);
    float hitDensity=exp(-max(hit.y,0.0)*falloff);
    float opticalDepth=g_FogDensity*distanceToHit*
                       (originDensity+4*middleDensity+hitDensity)/6.0;
    float transmittance=exp(-max(opticalDepth,0.0));
    return radiance*transmittance+clearSkyAirlight(rayDirection)*(1-transmittance);
}

float rainStreakMask(uint2 pixel) {
    // Build precipitation velocity in world space first.  Its vertical
    // component is explicitly negative, so horizontal wind can never make
    // rain rise.  Project that velocity into the camera plane; pixel Y grows
    // downwards, hence the minus sign on camera.up.
    float fallSpeed=lerp(7.0,10.0,saturate(g_RainIntensity));
    float driftSpeed=min(g_WindSpeed*g_WindStrength,12.0);
    float3 velocityWorld=float3(g_WindDirection.x*driftSpeed,-fallSpeed,
                                g_WindDirection.y*driftSpeed);
    float2 velocityPixels=float2(dot(velocityWorld,camera.right),
                                -dot(velocityWorld,camera.up));
    // Keep rainfall visibly downward even at extreme orbit angles.  The
    // horizontal component still carries the full projected wind drift.
    velocityPixels.y=max(velocityPixels.y,fallSpeed*.18);
    float pixelsPerMetre=camera.resolution.y/
        (2.0*max(camera.tanHalfFov,1e-3)*8.0);
    velocityPixels*=pixelsPerMetre;
    float speed=max(length(velocityPixels),1.0);
    float2 along=velocityPixels/speed;
    float2 across=float2(along.y,-along.x);
    float2 pixelCenter=float2(pixel)+.5;
    float2 p=float2(dot(pixelCenter,across),dot(pixelCenter,along));
    p.y-=g_Time*speed;
    float2 cell=floor(p/float2(7.0,46.0));
    float2 local=frac(p/float2(7.0,46.0));
    float seed=hash(cell+float2(37,91));
    float center=hash(cell+float2(11,173));
    float width=lerp(.045,.14,hash(cell+float2(67,5)));
    float streakLine=1-smoothstep(width,width*2.2,abs(local.x-center));
    float segment=smoothstep(.04,.20,local.y)*(1-smoothstep(.72,.98,local.y));
    return streakLine*segment*smoothstep(.55,1.0,seed);
}

[shader("raygeneration")]
void RayGen() {
    uint2 pixel=DispatchRaysIndex().xy;
    // Grass is composited by the instanced raster pass.  A single primary
    // sample keeps the path tracer from paying twice for animated geometry;
    // static frames still converge through the accumulation buffer.
    uint spatialSamples=1u;float3 frameColor=0;float sceneDepth=2200.0;
    float frameKeyVisibility=0;
    [loop] for(uint sampleIndex=0;sampleIndex<spatialSamples;++sampleIndex){
        float2 jitter;
        if(camera.maxFrames>1u)jitter=float2(hash(pixel+camera.frameIndex*17),hash(pixel.yx+camera.frameIndex*31))-.5;
        else jitter=0;
        float2 uv=((float2(pixel)+.5+jitter)/float2(camera.resolution))*2-1;uv.y=-uv.y;
        RayDesc ray;ray.Origin=camera.eye;ray.Direction=normalize(camera.forward+camera.right*uv.x*camera.aspect*camera.tanHalfFov+camera.up*uv.y*camera.tanHalfFov);ray.TMin=.02;ray.TMax=2200;
        RadiancePayload payload;payload.color=0;payload.depth=0;payload.primaryT=2200.0;
        payload.primaryKeyVisibility=1;
        TraceRay(Scene,RAY_FLAG_NONE,0x1,0,0,0,ray,payload);
        frameColor+=payload.color;
        frameKeyVisibility+=payload.primaryKeyVisibility;
        sceneDepth=min(sceneDepth,payload.primaryT*max(dot(ray.Direction,camera.forward),.001));
    }
    frameColor/=float(spatialSamples);
    float rainStreak=0;
    if(g_RainIntensity>.001)rainStreak=rainStreakMask(pixel)*g_RainIntensity;
    frameColor+=rainStreak*(float3(.24,.32,.42)+lightningRadiance()*.16)*.38;
    frameKeyVisibility/=float(spatialSamples);
    float4 previous=Accumulation[pixel];float history=min(float(camera.frameIndex),float(max(camera.maxFrames,1u)-1u));float3 accumulated=(previous.rgb*history+frameColor)/(history+1);
    float accumulatedKeyVisibility=camera.frameIndex==0u?frameKeyVisibility:
        (camera.frameIndex<camera.maxFrames?
            (Output[pixel].a*history+frameKeyVisibility)/(history+1):Output[pixel].a);
    Accumulation[pixel]=float4(accumulated,sceneDepth);
    Output[pixel]=float4(linearToSrgb(colorGrade(tonemap(accumulated*camera.exposure))),
                         accumulatedKeyVisibility);
}

[shader("miss")]
void RadianceMiss(inout RadiancePayload payload) {
    payload.color=environmentRadiance(WorldRayDirection());
    if(payload.depth==0){payload.primaryT=2200.0;payload.primaryKeyVisibility=1;}
}
[shader("miss")]
void VisibilityMiss(inout VisibilityPayload payload) { payload.visible=1; }
[shader("closesthit")]
void VisibilityHit(inout VisibilityPayload payload,in BuiltInTriangleIntersectionAttributes attr) { payload.visible=0; }

struct BladeData {
    float3 base;
    float3 normal;
    float3 side;
    float3 crossSide;
    float3 naturalLean;
    float height;
    float halfWidth;
    float phase;
    float stiffness;
    float dryness;
    float tall;
    float species;
    float leanStrength;
};

BladeData makeBlade(GrassPatch patch,uint bladeIndex) {
    BladeData blade;
    blade.normal=normalize(float3(patch.normalX,
        sqrt(saturate(1-patch.normalX*patch.normalX-patch.normalZ*patch.normalZ)),
        patch.normalZ));
    float3 axisX=normalize(float3(1,-blade.normal.x/max(blade.normal.y,.25),0));
    float3 axisZ=normalize(cross(axisX,blade.normal));
    uint seed=hashUint(patch.seed^((bladeIndex+1u)*0x9e3779b9u));
    uint baseCandidateCount=min(patch.packed&255u,34u);
    uint baseTallCount=min((patch.packed>>16)&255u,baseCandidateCount);
    float densityScale=clamp(camera.grassSettings.x,0.0,6.0);
    uint tallCount=min((uint)ceil(baseTallCount*min(densityScale,1.8)),
                       (uint)ceil(baseCandidateCount*densityScale));
    blade.tall=bladeIndex<tallCount?1.0:0.0;
    float radius=sqrt(randomUint(seed))*lerp(.245,.065,blade.tall);
    float offsetAngle=randomUint(seed^0x68bc21ebu)*6.2831853;
    float clusterAngle=randomUint(patch.seed^0x91e10da5u)*6.2831853;
    float clusterRadius=randomUint(patch.seed^0x243f6a88u)*.095*blade.tall;
    blade.base=float3((patch.minX+patch.maxX)*.5,patch.baseY,
                      (patch.minZ+patch.maxZ)*.5)
              +axisX*(cos(offsetAngle)*radius+cos(clusterAngle)*clusterRadius)
              +axisZ*(sin(offsetAngle)*radius+sin(clusterAngle)*clusterRadius);
    float patchAngle=randomUint(patch.seed^0x02e5be93u)*6.2831853;
    float bladeAngle=patchAngle+float(bladeIndex)*2.39996323+
                     (randomUint(seed^0x68bc21ebu)-.5)*.42;
    blade.side=normalize(axisX*cos(bladeAngle)+axisZ*sin(bladeAngle));
    blade.naturalLean=normalize(cross(blade.side,blade.normal));
    float crossAngle=lerp(1.20,1.94,randomUint(seed^0x7f4a7c15u));
    blade.crossSide=normalize(blade.side*cos(crossAngle)+blade.naturalLean*sin(crossAngle));
    blade.species=float(patch.seed%3u);
    float shortMaximum=float((patch.packed>>8)&255u)*.004;
    float tallMaximum=float((patch.packed>>24)&255u)*.004;
    float maximumHeight=lerp(shortMaximum,tallMaximum,blade.tall);
    blade.height=maximumHeight*lerp(.50,1.0,randomUint(seed^0xa511e9b3u))*
                 clamp(camera.grassSettings.y,.35,2.5);
    blade.halfWidth=lerp(lerp(.0022,.0048,randomUint(seed^0x63d83595u)),
                         lerp(.0035,.0085,randomUint(seed^0x63d83595u)),blade.tall)
                   *lerp(.88,1.16,patch.moisture);
    float individualPhase=randomUint(seed^0xb5297a4du)*6.2831853;
    float coherentPhase=randomUint(patch.seed^0xd1b54a35u)*6.2831853;
    blade.phase=lerp(individualPhase,coherentPhase,.78*blade.tall);
    blade.stiffness=lerp(lerp(.36,.88,randomUint(seed^0x1b56c4e9u)),
                          lerp(.24,.62,randomUint(seed^0x1b56c4e9u)),blade.tall);
    blade.dryness=randomUint(seed^0xc2b2ae35u);
    blade.leanStrength=lerp(blade.tall>.5?.07:.025,blade.tall>.5?.18:.13,
                            randomUint(seed^0x94d049bbu));
    return blade;
}

float3 grassWindDirection(BladeData blade) {
    float2 baseDirection=normalize(g_WindDirection);
    float2 windUV=blade.base.xz*.05+baseDirection*(g_Time*g_WindSpeed*.20);
    float directionWave=.16*sin(dot(windUV,float2(1.31,-.87))+g_Time*.19);
    float2 rotated=float2(baseDirection.x-directionWave*baseDirection.y,
                          baseDirection.y+directionWave*baseDirection.x);
    float3 wind=normalize(float3(rotated.x,0,rotated.y));
    float3 normal=blade.normal;
    return normalize(wind-normal*dot(wind,normal));
}

float grassGust(BladeData blade) {
    float2 windUV=blade.base.xz*.05+g_WindDirection*(g_Time*g_WindSpeed*.20);
    float turbulence=.5+.30*sin(dot(windUV,float2(2.17,1.31)))+
                     .20*sin(dot(windUV,float2(-4.13,3.27))+1.7);
    float traveling=g_Time*g_WindSpeed*max(g_WindGustFrequency,.05)+
                     dot(blade.base.xz,float2(.23,.17))+blade.phase;
    float gust=.56+.25*sin(traveling)+.14*sin(traveling*2.31+1.7)
              +.05*sin(g_Time*g_WindSpeed*7.2+blade.phase*3.0);
    return saturate(gust*lerp(.76,1.24,saturate(turbulence)));
}

float3 bladeCenter(BladeData blade,float along) {
    float s=saturate(along),shape=s*s*(2-s),gust=grassGust(blade);
    float compliance=lerp(.43,.17,blade.stiffness)*lerp(1.0,1.18,blade.tall);
    float bend=blade.height*g_WindStrength*compliance*gust;
    float flutter=sin(g_Time*g_WindSpeed*(6.5+2.5*(1-blade.stiffness))+blade.phase+s*5.0)
                 *blade.height*.013*g_WindStrength*s*s;
    return blade.base+blade.normal*(blade.height*s)
         +blade.naturalLean*(blade.height*blade.leanStrength*shape)
         +grassWindDirection(blade)*(bend*shape)+blade.side*flutter;
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
    uint baseCandidateCount=min(patch.packed&255u,34u);
    uint baseTallCount=min((patch.packed>>16)&255u,baseCandidateCount);
    float densityScale=clamp(camera.grassSettings.x,0.0,6.0);
    uint candidateCount=min((uint)ceil(baseCandidateCount*densityScale),128u);
    uint tallCount=min((uint)ceil(baseTallCount*min(densityScale,1.8)),candidateCount);
    float shortDistance=clamp(camera.grassSettings.z,2.0,128.0);
    float tallDistance=max(shortDistance,clamp(camera.grassSettings.w,4.0,192.0));
    float patchDistance=distance(camera.eye,float3((patch.minX+patch.maxX)*.5,
                                                   patch.baseY,
                                                   (patch.minZ+patch.maxZ)*.5));
    if(patchDistance>=tallDistance||(tallCount==0&&patchDistance>=shortDistance))return;
    uint activeCount=patchDistance>=shortDistance?tallCount:candidateCount;
    float pixelFootprint=2*patchDistance*camera.tanHalfFov/max(1.0,(float)camera.resolution.y);
    float targetHalfWidth=.55*pixelFootprint;
    bool visibilityRay=(RayFlags()&RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH)!=0;
    float bestT=RayTCurrent();GrassAttributes bestAttribute;bool found=false;
    float3 rayOrigin=ObjectRayOrigin(),rayDirection=ObjectRayDirection();
    [loop] for(uint bladeIndex=0;bladeIndex<activeCount;++bladeIndex) {
        BladeData blade=makeBlade(patch,bladeIndex);
        float drawDistance=blade.tall>.5?tallDistance:shortDistance;
        float distanceCoverage=1-smoothstep(drawDistance*.70,drawDistance,patchDistance);
        if(distanceCoverage<=0)continue;
        float lodDensity=lerp(blade.tall>.5?.62:.68,1.0,
                              1-smoothstep(3.0,drawDistance,patchDistance));
        uint selection=patch.seed^((bladeIndex+19u)*0x27d4eb2du);
        if(bladeIndex>=2u&&randomUint(selection)>lodDensity)continue;
        float coverageThreshold=randomUint(selection^0x165667b1u);
        bool crossed=blade.tall>.5||((hashUint(selection)&1u)==0u);
        uint segments=blade.tall>.5?(patchDistance<tallDistance*.56?3u:2u):
                                      (patchDistance<shortDistance*.46?3u:
                                       (patchDistance<shortDistance*.72?2u:1u));
        [loop] for(uint segment=0;segment<segments;++segment) {
            float s0=float(segment)/segments,s1=float(segment+1u)/segments;
            float3 p0=bladeCenter(blade,s0),p1=bladeCenter(blade,s1);
            float seed0=blade.tall*step(.84,blade.dryness)*smoothstep(.58,.66,s0)*(1-smoothstep(.90,1.0,s0));
            float seed1=blade.tall*step(.84,blade.dryness)*smoothstep(.58,.66,s1)*(1-smoothstep(.90,1.0,s1));
            float physicalW0=blade.halfWidth*(pow(max(1-s0,.015),.72)+seed0*1.65)+.00015;
            float physicalW1=blade.halfWidth*(pow(max(1-s1,.015),.72)+seed1*1.65)+.00015;
            float widthCap=blade.tall>.5?.043:.018;
            float renderW0=min(max(physicalW0,targetHalfWidth),widthCap);
            float renderW1=min(max(physicalW1,targetHalfWidth),widthCap);
            uint selectedPlane=0;
            if(visibilityRay&&crossed){
                float3 segmentTangent=normalize(p1-p0);
                float facing0=abs(dot(normalize(cross(blade.side,segmentTangent)),rayDirection));
                float facing1=abs(dot(normalize(cross(blade.crossSide,segmentTangent)),rayDirection));
                selectedPlane=facing1>facing0?1u:0u;
            }
            uint planeCount=visibilityRay?1u:(crossed?2u:1u);
            [loop] for(uint planeStep=0;planeStep<planeCount;++planeStep){
                uint planeIndex=visibilityRay?selectedPlane:planeStep;
                float3 ribbonSide=planeIndex==0u?blade.side:blade.crossSide;
                float3 left0=p0-ribbonSide*renderW0,right0=p0+ribbonSide*renderW0;
                float3 left1=p1-ribbonSide*renderW1,right1=p1+ribbonSide*renderW1;
                float2 triangleBary;float candidateT=bestT;
                if(rayTriangle(rayOrigin,rayDirection,left0,right0,left1,RayTMin(),candidateT,
                               triangleBary)){
                    float across=triangleBary.x;
                    float along=lerp(s0,s1,triangleBary.y);
                    float local=saturate((along-s0)*segments);
                    float coverage=distanceCoverage*saturate(lerp(physicalW0,physicalW1,local)/
                                                               max(lerp(renderW0,renderW1,local),1e-5));
                    if(coverageThreshold<coverage){bestT=candidateT;bestAttribute.encoded=float2(float(bladeIndex)+.10+.80*across,along+2.0*planeIndex);found=true;}
                }
                candidateT=bestT;
                if(rayTriangle(rayOrigin,rayDirection,right0,right1,left1,RayTMin(),candidateT,
                               triangleBary)){
                    float across=1-triangleBary.y;
                    float along=s0*(1-triangleBary.x-triangleBary.y)+s1*(triangleBary.x+triangleBary.y);
                    float local=saturate((along-s0)*segments);
                    float coverage=distanceCoverage*saturate(lerp(physicalW0,physicalW1,local)/
                                                               max(lerp(renderW0,renderW1,local),1e-5));
                    if(coverageThreshold<coverage){bestT=candidateT;bestAttribute.encoded=float2(float(bladeIndex)+.10+.80*across,along+2.0*planeIndex);found=true;}
                }
            }
        }
        if(found&&visibilityRay&&ReportHit(bestT,0,bestAttribute))return;
    }
    if(found)ReportHit(bestT,0,bestAttribute);
}

[shader("closesthit")]
void GrassRadianceHit(inout RadiancePayload payload,in GrassAttributes attr) {
    if(payload.depth==0)payload.primaryT=RayTCurrent();
    GrassPatch patch=GrassPatches[PrimitiveIndex()];uint bladeIndex=(uint)floor(attr.encoded.x);
    BladeData blade=makeBlade(patch,bladeIndex);uint plane=attr.encoded.y>=1.5?1u:0u;
    float along=saturate(attr.encoded.y-2.0*plane);
    float epsilon=.012;float3 tangent=normalize(bladeCenter(blade,min(1.0,along+epsilon))
                                              -bladeCenter(blade,max(0.0,along-epsilon)));
    float3 ribbonSide=plane==0u?blade.side:blade.crossSide;
    float3 geometricNormal=normalize(cross(ribbonSide,tangent));
    bool front=dot(geometricNormal,WorldRayDirection())<0;
    float3 n=front?geometricNormal:-geometricNormal;
    float3 hit=WorldRayOrigin()+WorldRayDirection()*RayTCurrent();
    float clusteredDryness=saturate(blade.dryness+patch.colourDryColony*.15-
                                    patch.colourLushColony*.08);
    float dryThreshold=lerp(.82,.94,patch.moisture);
    float dry=smoothstep(dryThreshold-.03,dryThreshold+.03,clusteredDryness);
    float3 green=lerp(float3(.040,.068,.014),float3(.095,.155,.030),
                      saturate(.28+.55*patch.moisture));
    green*=1.0+patch.colourWarmCool*float3(.035,.006,-.045);
    green*=lerp(float3(.86,.93,.83),float3(1.10,1.08,.91),patch.colourFertility);
    green=lerp(green,green*float3(1.10,1.01,.76),patch.colourDryColony*.22);
    green*=lerp(.72,1.04,smoothstep(0,.70,along));
    float3 straw=float3(.145,.122,.042)*lerp(.82,1.06,along)*
                  lerp(.90,1.10,patch.colourDryColony);
    float3 albedo=lerp(green,straw,dry);
    float wetness=saturate(g_WetnessFactor*.78);
    albedo*=lerp(1.0,.61,wetness);
    float3 sun=directionToKeyLight();float3 keyRadiance=keyLightRadiance();
    uint2 pixel=DispatchRaysIndex().xy;
    float2 random=float2(hash(pixel+camera.frameIndex*131),
                         hash(pixel.yx+camera.frameIndex*173));
    float3 sunTangent=normalize(cross(abs(sun.y)<.9?float3(0,1,0):float3(1,0,0),sun));
    float3 sunBitangent=cross(sun,sunTangent);float angle=random.y*6.2831853;
    sun=normalize(sun+sunTangent*cos(angle)*sqrt(random.x)*.0065
                     +sunBitangent*sin(angle)*sqrt(random.x)*.0065);
    VisibilityPayload shadow;shadow.visible=0;RayDesc ray;ray.Origin=hit+n*.004;
    ray.Direction=sun;ray.TMin=.003;ray.TMax=1000;
    TraceRay(Scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0x1,1,0,1,ray,shadow);
    if(payload.depth==0)payload.primaryKeyVisibility=float(shadow.visible);
    float frontLight=saturate(dot(n,sun)),backLight=saturate(dot(-n,sun));
    float3 ambient=skyIrradiance(n)*(.50+.18*along);
    float3 direct=keyRadiance*frontLight*shadow.visible*1.28;
    float3 unmodulated=keyRadiance*float3(.42,.74,.20)*backLight*
                       shadow.visible*.72;
    float3 view=normalize(camera.eye-hit),halfVector=normalize(sun+view);
    float wetExponent=lerp(22.0,110.0,wetness);
    unmodulated+=keyRadiance*pow(saturate(dot(n,halfVector)),wetExponent)*
                 lerp(.08,.34,wetness)*shadow.visible;
    ambient+=lightningRadiance()*(.18+.10*along);
    float seedHead=blade.tall*smoothstep(.70,.79,along)*(1-smoothstep(.92,1.0,along))
                   *step(.84,clusteredDryness);
    albedo=lerp(albedo,float3(.30,.27,.10),seedHead*.46);
    float fade=lerp(.58,1.0,smoothstep(0,.22,along));
    float3 result=(albedo*(ambient+direct)+unmodulated)*fade;
    payload.color=applyAerialPerspective(result,WorldRayOrigin(),hit,
                                          WorldRayDirection());
}

[shader("closesthit")]
void RadianceHit(inout RadiancePayload payload,in BuiltInTriangleIntersectionAttributes attr) {
    if(payload.depth==0)payload.primaryT=RayTCurrent();
    uint primitive=PrimitiveIndex();
    uint indexBase=InstanceID()==0u?0u:camera.environmentIndexOffset;
    uint i0=Indices[indexBase+primitive*3],i1=Indices[indexBase+primitive*3+1],
         i2=Indices[indexBase+primitive*3+2];
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
    float terrainMoisture=.5,terrainCavity=0,terrainRoughness=.9,puddleMask=0;
    float terrainRetention=0,terrainSlope=0;
    float materialRoughness=kind<.5?.74:(kind<1.5?.40:(kind<2.5?.90:
                            (kind<3.5?.67:(kind<4.5?.48:.72))));
    if(kind>1.5&&kind<2.5){
        float terrainDistance=distance(camera.eye,hit);
        float pixelWorld=2*terrainDistance*camera.tanHalfFov/max(1.0,(float)camera.resolution.y);
        float grazing=rcp(max(abs(dot(surfaceNormal,-WorldRayDirection())),.22));
        float footprint=min(pixelWorld*grazing,3.0);
        float fineFootprint=min(pixelWorld*sqrt(grazing),.55);
        float broad=filteredFbmWorld(hit.xz+float2(37.1,-19.6),.030,footprint);
        float patch=filteredFbmWorld(hit.xz+float2(-11.4,63.2),.140,footprint);
        float fine=filteredValueNoise(hit.xz+float2(7.7,21.3),2.3,fineFootprint);
        float slope=1-saturate(surfaceNormal.y),rootDistance=length(hit.xz);
        terrainSlope=length(surfaceNormal.xz)/max(surfaceNormal.y,.05);
        terrainRetention=unpackAlpha(a.color)*bary.x+unpackAlpha(b.color)*bary.y+
                         unpackAlpha(c.color)*bary.z;
        terrainMoisture=saturate(.10+.58*broad+.32*patch-.45*slope);
        float lushMask=smoothstep(.42,.72,terrainMoisture);
        float dryDriver=.65*(1-broad)+.35*patch;
        float dryMask=smoothstep(.64,.82,dryDriver)*(1-.65*lushMask);
        float3 shadowSward=float3(.028,.050,.012),pasture=float3(.070,.110,.027);
        float3 lushSward=float3(.098,.154,.036),drySward=float3(.155,.134,.052);
        float3 meadow=lerp(shadowSward,pasture,smoothstep(.24,.52,terrainMoisture));
        meadow=lerp(meadow,lushSward,lushMask*.72);
        meadow=lerp(meadow,drySward,dryMask*.62);
        meadow*=lerp(.90,1.10,fine);

        float soilMacro=filteredFbmWorld(hit.xz+float2(83,-47),.090,footprint);
        float soilFine=filteredValueNoise(hit.xz+float2(-31,14),1.7,fineFootprint);
        float rootCore=1-smoothstep(.72,1.35,rootDistance);
        float rootFringe=(1-smoothstep(1.10,2.70,rootDistance))*smoothstep(.55,.72,soilMacro);
        float slopeBare=smoothstep(.075,.23,slope)*smoothstep(.60,.78,soilMacro);
        float flatBare=smoothstep(.78,.90,.65*soilMacro+.35*soilFine)*(1-smoothstep(.10,.22,slope));
        float soilStructure=max(rootCore,max(rootFringe*.82,max(slopeBare*.68,flatBare*.62)));
        float soilMask=smoothstep(.22,.72,soilStructure+(soilFine-.5)*.18);
        soilMask*=1-smoothstep(150.0,230.0,rootDistance);
        float soilDryness=smoothstep(.42,.72,1-terrainMoisture);
        float3 soil=lerp(float3(.042,.024,.010),float3(.105,.061,.024),.30+.48*soilFine);
        soil=lerp(soil,float3(.160,.101,.041),soilDryness*.42);
        albedo=lerp(meadow,soil,soilMask);

        float highGround=smoothstep(18.0,92.0,hit.y)*smoothstep(760,1040,rootDistance);
        float mountainStone=saturate(highGround*(.34+.90*slope)+smoothstep(.28,.62,slope)*.38);
        float3 distantStone=lerp(float3(.105,.115,.098),float3(.225,.205,.165),fine);
        albedo=lerp(albedo,distantStone,mountainStone*.80);

        float nearTextureWeight=(payload.depth==0?1.0:0.0)*(1-smoothstep(70.0,110.0,terrainDistance))*(1-mountainStone);
        if(nearTextureWeight>.001){
            float2 textureWarp=float2(filteredValueNoise(hit.xz+float2(12.7,-8.3),.047,footprint),
                                      filteredValueNoise(hit.xz+float2(-29.1,44.6),.061,footprint))-.5;
            float2 groundUvA=float2(dot(hit.xz,float2(.963,.269)),dot(hit.xz,float2(.269,-.963)))*.5+textureWarp*.48;
            float2 groundUvB=float2(dot(hit.xz,float2(.526,-.851)),dot(hit.xz,float2(.851,.526)))*.2681+float2(.37,.19)+textureWarp.yx*.31;
            float textureFootprint=min(pixelWorld*pow(grazing,.25),.38);
            float groundLodA=clamp(log2(max(textureFootprint*512.0,1.0))-.85,0.0,10.0);
            float groundLodB=clamp(log2(max(textureFootprint*274.5,1.0))-.85,0.0,10.0);
            float normalLodA=max(0.0,groundLodA-1.0),normalLodB=max(0.0,groundLodB-1.0);
            float grassBlend=.34+.32*smoothstep(.30,.78,terrainMoisture);
            float4 denseAlbedo=sampleGroundAlbedo(groundUvA,0u,groundLodA);
            float4 coarseAlbedo=sampleGroundAlbedo(groundUvB,1u,groundLodB);
            float4 denseNormal=sampleGroundNormal(groundUvA,0u,normalLodA);
            float4 coarseNormal=sampleGroundNormal(groundUvB,1u,normalLodB);
            float4 textureAlbedo=lerp(coarseAlbedo,denseAlbedo,grassBlend);
            float4 textureNormal=lerp(coarseNormal,denseNormal,grassBlend);
            float4 textureLow=lerp(sampleGroundAlbedo(groundUvB,1u,groundLodB+3.25),
                                   sampleGroundAlbedo(groundUvA,0u,groundLodA+3.25),grassBlend);
            float cloverDriver=filteredFbmWorld(hit.xz+float2(-54.2,16.8),.18,footprint);
            float cloverWeight=smoothstep(.76,.90,cloverDriver)*smoothstep(.48,.72,terrainMoisture)*(1-soilMask)*.58;
            if(cloverWeight>.01){
                float4 cloverAlbedo=sampleGroundAlbedo(groundUvB+float2(.31,.17),3u,groundLodB);
                float4 cloverNormal=sampleGroundNormal(groundUvB+float2(.31,.17),3u,normalLodB);
                float4 cloverLow=sampleGroundAlbedo(groundUvB+float2(.31,.17),3u,groundLodB+3.25);
                textureAlbedo=lerp(textureAlbedo,cloverAlbedo,cloverWeight);
                textureNormal=lerp(textureNormal,cloverNormal,cloverWeight);
                textureLow=lerp(textureLow,cloverLow,cloverWeight);
            }
            if(soilMask>.01){
                float4 soilAlbedo=sampleGroundAlbedo(groundUvB+float2(.13,.43),2u,groundLodB);
                float4 soilNormal=sampleGroundNormal(groundUvB+float2(.13,.43),2u,normalLodB);
                float4 soilLow=sampleGroundAlbedo(groundUvB+float2(.13,.43),2u,groundLodB+3.25);
                textureAlbedo=lerp(textureAlbedo,soilAlbedo,soilMask);
                textureNormal=lerp(textureNormal,soilNormal,soilMask);
                textureLow=lerp(textureLow,soilLow,soilMask);
            }
            float3 highFrequency=clamp(textureAlbedo.rgb/max(textureLow.rgb,.012),.68,1.42);
            highFrequency*=lerp(.94,1.06,textureNormal.b);
            float materialDetail=saturate(nearTextureWeight*clamp(camera.groundSettings.y,0.0,2.0)*.90);
            albedo*=lerp(float3(1,1,1),highFrequency,materialDetail);
            terrainRoughness=textureAlbedo.a;
            terrainCavity=textureNormal.a*nearTextureWeight*.55;
            float normalStrength=nearTextureWeight*clamp(camera.groundSettings.x,0.0,2.0)*.92;
            float2 mapXY=(textureNormal.rg*2-1)*normalStrength;
            float mapZ=sqrt(saturate(1-dot(mapXY,mapXY)));
            float3 tangent=normalize(float3(1,-surfaceNormal.x/max(surfaceNormal.y,.12),0));
            float3 bitangent=normalize(cross(surfaceNormal,tangent));
            n=normalize(tangent*mapXY.x+bitangent*mapXY.y+surfaceNormal*mapZ);
        }else{
            float frequency=lerp(1.4,.12,smoothstep(8.0,190.0,terrainDistance));
            float sampleStep=.18/frequency;
            float dx=(fbm((hit.xz+float2(sampleStep,0))*frequency)-fbm((hit.xz-float2(sampleStep,0))*frequency))/(2*sampleStep);
            float dz=(fbm((hit.xz+float2(0,sampleStep))*frequency)-fbm((hit.xz-float2(0,sampleStep))*frequency))/(2*sampleStep);
            n=normalize(n+float3(-dx*.035,0,-dz*.035));
        }
        if(g_PuddleCoverage>.001){
            float flatSurface=smoothstep(.990268,.997564,surfaceNormal.y);
            float drainageSuitability=saturate(terrainRetention)*flatSurface;
            if(drainageSuitability>.001){
                // Hydrology selects real depressions.  Noise only breaks up
                // the shoreline; it can no longer create water on a hill.
                float puddleNoise=filteredFbmWorld(hit.xz+float2(91.7,-53.4),
                                                    .052,footprint);
                float puddleThreshold=lerp(1.08,.20,saturate(g_PuddleCoverage));
                float puddleDriver=drainageSuitability+(puddleNoise-.49)*.12;
                float basin=smoothstep(puddleThreshold,puddleThreshold+.07,
                                       puddleDriver);
                // Coverage is already the CPU-derived result of accumulated
                // wetness; applying wetness again delayed puddles twice.
                puddleMask=basin*flatSurface;
                n=normalize(lerp(n,float3(0,1,0),puddleMask));
                terrainRoughness=lerp(terrainRoughness,.001,puddleMask);
            }
        }
        materialRoughness=terrainRoughness;
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
    float wetScale=kind<.5?.52:(kind<1.5?.60:(kind<2.5?1.0:
                   (kind<3.5?.88:(kind<4.5?.62:.46))));
    float rainExposure=lerp(.56,1.0,saturate(surfaceNormal.y));
    float wetness=saturate(g_WetnessFactor*wetScale*rainExposure);
    if(kind>1.5&&kind<2.5){
        // A thin film remains after rain, but exposed slopes drain faster and
        // concave catchments stay saturated longer.
        float slopeRunoff=1-smoothstep(.035,.32,terrainSlope);
        wetness=saturate(wetness*lerp(.58,1.14,
            saturate(.65*terrainRetention+.35*slopeRunoff)));
    }
    wetness=max(wetness,puddleMask);
    albedo*=lerp(1.0,.55,wetness);
    materialRoughness=lerp(materialRoughness,.02,wetness);
    materialRoughness=lerp(materialRoughness,.001,puddleMask);

    // Environment foliage remains in the immutable BLAS, so it receives a
    // small shading flutter.  Instance 0 is the physically deformed oak and
    // must not receive this second, unrelated normal bend.
    if(InstanceID()!=0u&&thinFoliage&&g_WindSpeed>.001&&g_WindStrength>.001){
        float2 windUV=hit.xz*.05+g_WindDirection*(g_Time*g_WindSpeed*.20);
        float gust=sin(dot(windUV,float2(2.17,1.31))+g_Time*g_WindGustFrequency)+
                   .45*sin(dot(windUV,float2(-4.13,3.27))+g_Time*g_WindSpeed*2.1);
        float3 windVector=normalize(float3(g_WindDirection.x,0,g_WindDirection.y));
        n=normalize(n+windVector*gust*g_WindStrength*.055);
    }

    float3 baseSun=directionToKeyLight(),sunTangent=normalize(cross(abs(baseSun.y)<.9?float3(0,1,0):float3(1,0,0),baseSun)),sunBitangent=cross(baseSun,sunTangent);float diskRadius=sqrt(random.x)*.0065,angle=random.y*6.2831853;float3 sunDir=normalize(baseSun+sunTangent*cos(angle)*diskRadius+sunBitangent*sin(angle)*diskRadius);
    VisibilityPayload shadow;shadow.visible=0;RayDesc s;s.Origin=hit+(thinFoliage?sunDir:surfaceNormal)*.012;s.Direction=sunDir;s.TMin=.01;s.TMax=2200;TraceRay(Scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,0x1,1,0,1,s,shadow);
    float visibility=float(shadow.visible);
    if(payload.depth==0)payload.primaryKeyVisibility=visibility;
    // The old ground path launched three additional soft-shadow rays, AO and
    // a bounce ray for nearly every screen pixel.  One stochastic sun sample
    // converges on static frames; terrain cavity/normal maps provide the local
    // ground occlusion without another traversal.
    bool terrainSurface=kind>1.5&&kind<2.5;
    VisibilityPayload ao;ao.visible=1;if(payload.depth==0&&!terrainSurface){ao.visible=0;RayDesc ar;ar.Origin=hit+surfaceNormal*.015;ar.Direction=cosineHemisphere(n,float2(hash(pixel+camera.frameIndex*47),hash(pixel.yx+camera.frameIndex*71)));ar.TMin=.01;ar.TMax=1.35;TraceRay(Scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,0x1,1,0,1,ar,ao);}
    float3 keyRadiance=keyLightRadiance();
    float ndl=saturate(dot(n,sunDir));float occlusion=lerp(.76,1.0,float(ao.visible));float3 ambient=skyIrradiance(n)*(.48+.16*saturate(n.y))*occlusion;ambient+=lerp(float3(.010,.014,.020),float3(.20,.17,.12),daylightAmount())*(.08+.14*saturate(-n.y));ambient+=lightningRadiance()*(.16+.14*saturate(n.y));float3 direct=keyRadiance*ndl*visibility*1.28;float3 unmodulated=0;
    // Restrict the additional traversal to visible primary surfaces inside
    // the finite local-light range. Secondary/bounce hits retain the current performance
    // budget, while direct local shadows remain exact for the displayed scene.
    if(payload.depth==0){
        LocalLightSample localLight=samplePlayerLocalLight(hit);
        float localNdotL=localLight.active*saturate(dot(n,localLight.direction));
        if(localNdotL>1e-4){
            float localVisibility=playerLocalLightVisibility(
                hit,localLight.direction,localLight.distance);
            direct+=localLight.radiance*localNdotL*localVisibility;
        }
    }
    if(kind<.5)ambient*=lerp(1,.68,barkCavity);
    if(kind>1.5&&kind<2.5){
        ambient*=lerp(.94,1.02,terrainMoisture)*lerp(1.0,.86,terrainCavity);
        ambient+=float3(.014,.019,.010);
        float3 viewDirection=normalize(camera.eye-hit),halfVector=normalize(sunDir+viewDirection);
        float groundSpecular=pow(saturate(dot(n,halfVector)),lerp(34.0,8.0,terrainRoughness));
        unmodulated+=keyRadiance*groundSpecular*(1-terrainRoughness)*.10*visibility;
    }
    if(kind>.5&&kind<1.5){
        float midrib=exp(-abs(uv.x-.5)*150);float secondary=exp(-abs(frac((uv.y+abs(uv.x-.5)*.72)*6)-.5)*34);float veins=saturate(midrib*.82+secondary*.28);float edge=saturate(length((uv-.5)*float2(1.25,1.0))*2);
        float chlorophyll=lerp(1.08,.76,edge)*lerp(1,.58,veins);albedo*=upperFace?float3(.82,1.0,.76):float3(1.05,1.16,.90);albedo=lerp(albedo,float3(.25,.38,.13),veins*.42);
        float pathLength=(.24+.28*chlorophyll)/max(abs(dot(geometricNormal,sunDir)),.16);float3 absorption=float3(2.55,.72,3.25);float3 transmittance=exp(-absorption*pathLength);float back=saturate(dot(-n,sunDir));unmodulated+=transmittance*keyRadiance*back*visibility*.68;
        float roughness=upperFace?.34:.58;float3 viewDirection=normalize(camera.eye-hit),halfVector=normalize(sunDir+viewDirection);float ndh=saturate(dot(n,halfVector));float fresnel=.022+.978*pow(1-saturate(dot(n,viewDirection)),5);unmodulated+=keyRadiance*fresnel*pow(ndh,lerp(70,18,roughness))*(upperFace?.42:.12);
    }
    if(kind>3.5&&kind<4.5){
        float species=round(frac(material)*10);float mottling=valueNoise(float2(hit.x*1.7+hit.y*.53,hit.z*1.9-hit.y*.37));
        albedo*=lerp(.80,1.16,mottling);albedo*=upperFace?1.0:.78;
        albedo*=species<.5?float3(.96,1.05,.88):(species<1.5?float3(.84,1.03,.92):float3(1.04,.98,.77));
        float back=saturate(dot(-n,sunDir));float3 transmission=species<1.5?float3(.18,.36,.075):float3(.27,.32,.07);
        unmodulated+=transmission*keyRadiance*back*visibility*.48;ambient*=.94;
    }
    if(kind>2.5&&kind<3.5)ambient*=.86;
    if(kind>4.5&&kind<5.5)ambient*=.84;
    if(wetness>.001){
        float3 viewDirection=normalize(camera.eye-hit),halfVector=normalize(sunDir+viewDirection);
        float fresnel=.025+.975*pow(1-saturate(dot(n,viewDirection)),5);
        float exponent=lerp(12.0,220.0,1-materialRoughness);
        float wetSpecular=pow(saturate(dot(n,halfVector)),exponent)*
                           lerp(.04,.52,wetness)*(1-materialRoughness);
        unmodulated+=keyRadiance*wetSpecular*visibility*(.35+.65*fresnel);
        unmodulated+=lightningRadiance()*fresnel*wetness*.18;
    }
    float3 result=albedo*(ambient+direct)+unmodulated;
    if(payload.depth==0&&puddleMask>.05){
        float3 viewDirection=normalize(camera.eye-hit);
        float waterFresnel=.02+.98*pow(1-saturate(dot(n,viewDirection)),5);
        RadiancePayload reflection;reflection.color=0;reflection.depth=1;
        reflection.primaryT=2200;reflection.primaryKeyVisibility=1;
        RayDesc reflectedRay;reflectedRay.Origin=hit+n*.018;
        reflectedRay.Direction=normalize(reflect(WorldRayDirection(),n));
        reflectedRay.TMin=.012;reflectedRay.TMax=2200;
        TraceRay(Scene,RAY_FLAG_NONE,0x1,0,0,0,reflectedRay,reflection);
        float reflectionWeight=puddleMask*waterFresnel;
        result=lerp(result,reflection.color,reflectionWeight);
    }
    if(payload.depth==0&&!terrainSurface){RadiancePayload bounce;bounce.color=0;bounce.depth=1;bounce.primaryT=6;bounce.primaryKeyVisibility=1;RayDesc br;br.Origin=hit+surfaceNormal*.018;br.Direction=cosineHemisphere(n,float2(hash(pixel+camera.frameIndex*89),hash(pixel.yx+camera.frameIndex*113)));br.TMin=.01;br.TMax=6;TraceRay(Scene,RAY_FLAG_NONE,0x1,0,0,0,br,bounce);result+=albedo*bounce.color*.075;}
    payload.color=applyAerialPerspective(result,WorldRayOrigin(),hit,
                                          WorldRayDirection());
}
