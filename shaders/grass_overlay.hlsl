#include "environment_cb.hlsli"

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
    // xz: player foot position, zw: horizontal velocity. groundSettings.w is
    // the explicit first-person interaction gate.
    float4 grassInteraction;
};

struct GrassPatch {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
    uint seed, packed;
    float baseY, normalX, normalZ, moisture;
    float colourFertility, colourDryColony, colourLushColony, colourWarmCool;
};

Texture2D<float4> SceneDepth : register(t0);
Texture2D<float4> SceneColor : register(t1);
StructuredBuffer<GrassPatch> GrassPatches : register(t2);
RaytracingAccelerationStructure Scene : register(t3);
ConstantBuffer<Camera> camera : register(b0);
cbuffer GrassDraw : register(b2) {
    uint drawPatchOffset;
    uint drawInstanceStride;
};

float3 srgbToLinear(float3 c) {
    c=saturate(c);
    return lerp(c/12.92,pow((c+.055)/1.055,2.4),step(.04045,c));
}

float3 linearToSrgb(float3 c) {
    c=max(c,0);
    return lerp(12.92*c,1.055*pow(c,1.0/2.4)-.055,step(.0031308,c));
}

float3 tonemap(float3 x) {
    return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));
}

float3 colorGrade(float3 c) {
    float luminance=dot(c,float3(.2126,.7152,.0722));
    c=lerp(luminance.xxx,c,1.055);
    c=(c-.18)*1.015+.18;
    c=pow(saturate(c),.985)*float3(1.006,1.0,.992);
    return saturate(c);
}

uint hashUint(uint x) {
    x^=x>>16;
    x*=0x7feb352du;
    x^=x>>15;
    x*=0x846ca68bu;
    x^=x>>16;
    return x;
}

float randomUint(uint x) {
    return float(hashUint(x)&0x00ffffffu)*(1.0/16777216.0);
}

float3 directionToSun() {
    return normalize(g_SunDirection);
}

float3 directionToMoon() {
    return normalize(g_MoonDirection);
}

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
    RayDesc ray;ray.Origin=hit+direction*.012;ray.Direction=direction;
    ray.TMin=.003;ray.TMax=max(distanceToLight-.025,.004);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(Scene,RAY_FLAG_NONE,0x1,ray);
    while(query.Proceed()){}
    return query.CommittedStatus()==COMMITTED_NOTHING?1:0;
}

float3 skyIrradiance(float3 normal) {
    float up=saturate(normal.y*.5+.5);
    float daylight=smoothstep(-.12,.08,directionToSun().y);
    float3 horizon=lerp(float3(.008,.013,.030),float3(.38,.48,.58),daylight);
    float3 zenith=lerp(float3(.0015,.004,.016),float3(.075,.22,.48),daylight);
    float3 sky=lerp(horizon,zenith,pow(up,.58))*lerp(1.0,.38,g_StormIntensity);
    sky+=g_MoonColor*g_MoonIntensity*(.16+.22*up);
    sky+=lightningRadiance()*(.12+.16*up);
    return max(sky,0);
}

float3 clearSkyAirlight(float3 direction) {
    float3 d=normalize(direction);
    float up=saturate(d.y),daylight=smoothstep(-.12,.08,directionToSun().y);
    float3 horizon=lerp(float3(.009,.014,.030),float3(.34,.52,.69),daylight);
    float3 zenith=lerp(float3(.002,.005,.018),float3(.035,.16,.43),daylight);
    float3 airlight=lerp(horizon,zenith,pow(up,.58))*lerp(1.0,.52,g_StormIntensity);
    float forwardScatter=pow(saturate(dot(d,directionToKeyLight())),12);
    airlight+=keyLightRadiance()*forwardScatter*.22+lightningRadiance()*.20;
    return max(airlight,0);
}

float3 applyAerialPerspective(float3 radiance,float3 hit,float3 rayDirection) {
    float distanceToHit=distance(camera.eye,hit);
    float falloff=max(g_FogHeightFalloff,1e-5);
    float eyeDensity=exp(-max(camera.eye.y,0.0)*falloff);
    float middleDensity=exp(-max((camera.eye.y+hit.y)*.5,0.0)*falloff);
    float hitDensity=exp(-max(hit.y,0.0)*falloff);
    float opticalDepth=g_FogDensity*distanceToHit*(eyeDensity+4*middleDensity+hitDensity)/6.0;
    float transmittance=exp(-max(opticalDepth,0.0));
    return radiance*transmittance+clearSkyAirlight(rayDirection)*(1-transmittance);
}

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
    float leanStrength;
    float verticalScale;
    float waterCoverage;
    float waterlineAlong;
};

float2 grassWaterState(GrassPatch patch,float3 patchNormal) {
    float puddleFill=saturate(g_PuddleCoverage);
    // The high seed byte carries the CPU terrain sampler's exact hydrology
    // retention.  The low 24 bits remain the patch's procedural random seed.
    float flatSurface=smoothstep(.990268,.997564,patchNormal.y);
    float retention=float(patch.seed>>24)*(1.0/255.0);
    float threshold=lerp(1.08,.20,puddleFill);
    float basin=smoothstep(threshold-.04,threshold+.04,retention);
    float retainedMask=basin*flatSurface*smoothstep(.025,.13,retention)*
                       smoothstep(.002,.03,puddleFill);

    // Regional flooding uses the same absolute water level and global gate as
    // the terrain shader.  Existing coherent colour fields provide a modest
    // sub-patch shoreline offset without adding noise work to every vertex.
    float floodGate=smoothstep(.002,.070,saturate(g_FloodCoverage));
    float boundaryOffset=(patch.colourFertility-.5)*.070+
                         patch.colourWarmCool*.012;
    float waterHead=g_WaterTableHeight+boundaryOffset-patch.baseY;
    float lowlandMask=smoothstep(-.035,.035,waterHead)*flatSurface*floodGate;
    float coverage=saturate(retainedMask+lowlandMask-retainedMask*lowlandMask);
    float retainedDepth=lerp(.008,.035,puddleFill)*retainedMask;
    float floodDepth=max(waterHead+.010,0.0)*lowlandMask;
    return float2(coverage,max(retainedDepth,floodDepth));
}

BladeData makeBlade(GrassPatch patch,uint bladeIndex,float3 patchNormal,
                    float3 axisX,float3 axisZ,uint tallCount) {
    BladeData blade;
    blade.normal=patchNormal;
    uint patchRandomSeed=patch.seed&0x00ffffffu;
    uint seed=hashUint(patchRandomSeed^((bladeIndex+1u)*0x9e3779b9u));
    blade.tall=bladeIndex<tallCount?1.0:0.0;
    float radius=sqrt(randomUint(seed))*lerp(.265,.072,blade.tall);
    float offsetAngle=randomUint(seed^0x68bc21ebu)*6.2831853;
    uint subclump=bladeIndex%3u;
    float clusterAngle=randomUint(patchRandomSeed^0x91e10da5u)*6.2831853+
                       float(subclump)*2.0943951+
                       (randomUint(seed^0x243f6a88u)-.5)*.34;
    float clusterRadius=lerp(.10,.15,randomUint(patchRandomSeed^(subclump*0x9e3779b9u)))*blade.tall;
    blade.base=float3((patch.minX+patch.maxX)*.5,patch.baseY,
                      (patch.minZ+patch.maxZ)*.5)
              +axisX*(cos(offsetAngle)*radius+cos(clusterAngle)*clusterRadius)
              +axisZ*(sin(offsetAngle)*radius+sin(clusterAngle)*clusterRadius);
    float patchAngle=randomUint(patchRandomSeed^0x02e5be93u)*6.2831853;
    float bladeAngle=patchAngle+float(bladeIndex)*2.39996323+
                     (randomUint(seed^0x68bc21ebu)-.5)*.42;
    blade.side=normalize(axisX*cos(bladeAngle)+axisZ*sin(bladeAngle));
    blade.naturalLean=normalize(cross(blade.side,blade.normal));
    blade.species=float(patchRandomSeed%3u);
    float shortMaximum=float((patch.packed>>8)&255u)*.004;
    float tallMaximum=float((patch.packed>>24)&255u)*.004;
    float maximumHeight=lerp(shortMaximum,tallMaximum,blade.tall);
    blade.height=maximumHeight*lerp(.50,1.0,randomUint(seed^0xa511e9b3u))*
                 clamp(camera.grassSettings.y,.35,2.5);
    blade.halfWidth=lerp(lerp(.0022,.0048,randomUint(seed^0x63d83595u)),
                         lerp(.0035,.0085,randomUint(seed^0x63d83595u)),blade.tall)
                   *lerp(.88,1.16,patch.moisture);
    float individualPhase=randomUint(seed^0xb5297a4du)*6.2831853;
    float coherentPhase=randomUint(patchRandomSeed^0xd1b54a35u)*6.2831853;
    blade.phase=lerp(individualPhase,coherentPhase,.78*blade.tall);
    blade.stiffness=lerp(lerp(.36,.88,randomUint(seed^0x1b56c4e9u)),
                         lerp(.24,.62,randomUint(seed^0x1b56c4e9u)),blade.tall);
    blade.dryness=randomUint(seed^0xc2b2ae35u);
    blade.leanStrength=lerp(blade.tall>.5?.22:.025,blade.tall>.5?.45:.14,
                            randomUint(seed^0x94d049bbu));
    float2 water=grassWaterState(patch,patchNormal);
    float uncompressedWaterline=saturate(water.y/max(blade.height,.02));
    float depthResponse=smoothstep(.08,.86,uncompressedWaterline);
    // A shallow film wets every species but only bends short blades.  Tall
    // stems remain upright until the physical water depth reaches a useful
    // fraction of their height.
    blade.waterCoverage=max(water.x*.15,depthResponse);
    blade.verticalScale=lerp(1.0,.38,depthResponse);
    blade.waterlineAlong=saturate(water.y/
        max(blade.height*blade.verticalScale,.02));
    // Stay inside the patch AABB's existing short/tall lateral budgets.
    float flattenedLean=lerp(.42,.58,blade.tall);
    blade.leanStrength=lerp(blade.leanStrength,flattenedLean,
                            blade.waterCoverage*.88);
    return blade;
}

float3 grassWindDirection(BladeData blade) {
    float2 baseDirection=normalize(g_WindDirection);
    float2 windUV=blade.base.xz*.05+baseDirection*(g_Time*g_WindSpeed*.20);
    float directionWave=.16*sin(dot(windUV,float2(1.31,-.87))+
                                g_Time*.19*saturate(g_WindSpeed));
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

struct BladeMotion {
    float3 windDirection;
    float bend;
    float flutterPhase;
    float flutterAmplitude;
    float3 interactionDirection;
    float interactionBend;
};

float3 grassInteractionDirection(BladeData blade,out float response) {
    response=0;
    if(camera.groundSettings.w<.5)return float3(0,0,0);

    float2 player=camera.grassInteraction.xy;
    float2 velocity=camera.grassInteraction.zw;
    float2 delta=blade.base.xz-player;
    float distanceToCapsule=length(delta);
    float speed=length(velocity);
    float2 travel=speed>.06?velocity/speed:float2(0,0);
    float2 radial=distanceToCapsule>.025?delta/distanceToCapsule:
                  (speed>.06?-travel:float2(1,0));

    // A soft capsule around the legs parts nearby blades.  A short swept wake
    // follows the actual controller velocity, extending recovery over roughly
    // a quarter second without retaining or updating individual blades.
    float capsule=1-smoothstep(.30,.82,distanceToCapsule);
    float wake=0;
    if(speed>.06) {
        float longitudinal=dot(delta,travel);
        float lateral=abs(delta.x*travel.y-delta.y*travel.x);
        float wakeLength=lerp(.45,1.55,saturate(speed/5.5));
        float behind=saturate(-longitudinal/wakeLength);
        float longitudinalWindow=step(longitudinal,0.0)*(1-smoothstep(.62,1.0,behind));
        float lateralWindow=1-smoothstep(.24,.68,lateral);
        wake=longitudinalWindow*lateralWindow*saturate(speed/.65)*.72;
    }
    response=saturate(max(capsule,wake));
    float2 push=normalize(radial*max(capsule,.08)+travel*wake*.72+float2(1e-5,0));
    float3 tangentPush=float3(push.x,0,push.y);
    tangentPush-=blade.normal*dot(tangentPush,blade.normal);
    return normalize(tangentPush+float3(1e-6,0,0));
}

BladeMotion prepareBladeMotion(BladeData blade) {
    BladeMotion motion;
    float compliance=lerp(.43,.17,blade.stiffness)*lerp(1.0,1.18,blade.tall);
    motion.windDirection=grassWindDirection(blade);
    float waterPinning=lerp(1.0,.06,blade.waterCoverage);
    motion.bend=blade.height*g_WindStrength*compliance*grassGust(blade)*waterPinning;
    motion.flutterPhase=g_Time*g_WindSpeed*(6.5+2.5*(1-blade.stiffness))+blade.phase;
    motion.flutterAmplitude=blade.height*.013*g_WindStrength*waterPinning;
    float interactionResponse;
    motion.interactionDirection=grassInteractionDirection(blade,interactionResponse);
    float contactCompliance=lerp(1.10,.82,blade.stiffness)*lerp(.88,1.08,blade.tall);
    motion.interactionBend=blade.height*interactionResponse*contactCompliance*
                           waterPinning;
    return motion;
}

float3 bladeCenter(BladeData blade,BladeMotion motion,float along) {
    float s=saturate(along),shape=s*s*(2-s);
    float flutter=sin(motion.flutterPhase+s*5.0)*motion.flutterAmplitude*s*s;
    return blade.base+blade.normal*(blade.height*s*blade.verticalScale)
         +blade.naturalLean*(blade.height*blade.leanStrength*shape)
         +motion.windDirection*(motion.bend*shape)
         +motion.interactionDirection*(motion.interactionBend*shape)
         +blade.side*flutter;
}

float3 bladeTangent(BladeData blade,BladeMotion motion,float along) {
    float s=saturate(along);
    float shapeDerivative=4*s-3*s*s;
    float phase=motion.flutterPhase+s*5.0;
    float flutterDerivative=motion.flutterAmplitude*
                            (5*cos(phase)*s*s+2*sin(phase)*s);
    return normalize(blade.normal*(blade.height*blade.verticalScale)
         +blade.naturalLean*(blade.height*blade.leanStrength*shapeDerivative)
         +motion.windDirection*(motion.bend*shapeDerivative)
         +motion.interactionDirection*(motion.interactionBend*shapeDerivative)
         +blade.side*flutterDerivative);
}

float3 cameraFacingSide(float3 center,float3 tangent,float3 fallbackSide) {
    float3 viewDirection=normalize(camera.eye-center);
    float3 side=cross(tangent,viewDirection);
    if(dot(side,side)<1e-6)side=fallbackSide-tangent*dot(fallbackSide,tangent);
    side=normalize(side);
    // Keep the billboard in the same hemisphere as the blade's persistent
    // biological plane. Without this sign convention a 180-degree yaw flips
    // triangle winding and the raster top-left rule can make thin blades pop,
    // even with back-face culling disabled.
    return dot(side,fallbackSide)<0?-side:side;
}

float bladePhysicalHalfWidth(BladeData blade,float along) {
    float seedHead=blade.tall*step(.84,blade.dryness)*smoothstep(.58,.66,along)
                  *(1-smoothstep(.90,1.0,along));
    return blade.halfWidth*(pow(max(1-along,.015),.72)+seedHead*1.65)+.00015;
}

float4 projectWorld(float3 worldPosition) {
    float3 delta=worldPosition-camera.eye;
    float viewX=dot(delta,camera.right);
    float viewY=dot(delta,camera.up);
    float viewZ=dot(delta,camera.forward);
    const float nearPlane=.02;
    const float farPlane=2200.0;
    float projectionScale=max(camera.tanHalfFov,1e-4);
    float projectionAspect=max(camera.aspect,1e-4);
    float clipZ=(viewZ*farPlane-nearPlane*farPlane)/(farPlane-nearPlane);
    return float4(viewX/(projectionAspect*projectionScale),
                  viewY/projectionScale,clipZ,viewZ);
}

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 bladeCoordinates : TEXCOORD2;
    float4 bladeParameters : TEXCOORD3;
    float coverage : TEXCOORD4;
    nointerpolation uint ditherSeed : TEXCOORD5;
    nointerpolation float4 colourFields : TEXCOORD6;
};

VSOutput inactiveVertex() {
    VSOutput output=(VSOutput)0;
    output.position=float4(2,2,1,1);
    return output;
}

VSOutput VSMain(uint vertexId : SV_VertexID,uint instanceId : SV_InstanceID) {
    uint instanceStride=clamp(drawInstanceStride,1u,128u);
    uint patchIndex=drawPatchOffset+instanceId/instanceStride;
    uint bladeIndex=instanceId%instanceStride;
    GrassPatch patch=GrassPatches[patchIndex];
    uint baseCandidateCount=min(patch.packed&255u,34u);
    uint baseTallCount=min((patch.packed>>16)&255u,baseCandidateCount);
    float densityScale=clamp(camera.grassSettings.x,0.0,6.0);
    uint candidateCount=min((uint)ceil(baseCandidateCount*densityScale),128u);
    uint tallCount=min((uint)ceil(baseTallCount*min(densityScale,1.8)),candidateCount);
    float shortDistance=clamp(camera.grassSettings.z,2.0,128.0);
    float tallDistance=max(shortDistance,clamp(camera.grassSettings.w,4.0,192.0));
    // Short grass must not switch as one camera-centred ring.  A broad
    // transition gives each world-stable blade room to leave independently as
    // the player moves, instead of removing a whole band behind the camera and
    // introducing the matching band in front.
    float shortTransitionDistance=min(tallDistance,
        max(shortDistance+12.0,shortDistance*1.45));
    float3 patchCenter=float3((patch.minX+patch.maxX)*.5,patch.baseY,
                              (patch.minZ+patch.maxZ)*.5);
    float patchDistance=distance(camera.eye,patchCenter);
    if(patchDistance>=tallDistance||
       (tallCount==0u&&patchDistance>=shortTransitionDistance))
        return inactiveVertex();

    if(bladeIndex>=candidateCount)return inactiveVertex();
    bool tallBlade=bladeIndex<tallCount;
    uint selection=(patch.seed&0x00ffffffu)^((bladeIndex+19u)*0x27d4eb2du);
    // Give every blade a deterministic exit radius derived only from its
    // absolute patch/blade identity.  The population remains anchored in world
    // space while the fade shell travels past it.
    float shortSpan=max(shortTransitionDistance-shortDistance,.01);
    float shortExit=lerp(shortDistance,shortTransitionDistance,
                         randomUint(selection^0x68bc21ebu));
    float shortFadeWidth=min(shortSpan,max(1.5,shortSpan*.30));
    if(!tallBlade&&patchDistance>=shortExit)return inactiveVertex();
    float shortCoverage=1-smoothstep(shortExit-shortFadeWidth,shortExit,
                                     patchDistance);
    float tallCoverage=1-smoothstep(tallDistance*.70,tallDistance,patchDistance);
    float distanceCoverage=tallBlade?tallCoverage:shortCoverage;
    float tallLodDensity=lerp(.62,1.0,1-smoothstep(3.0,tallDistance,patchDistance));
    if(distanceCoverage<=0||
       (tallBlade&&bladeIndex>=2u&&randomUint(selection)>tallLodDensity))
        return inactiveVertex();

    float3 patchNormal=normalize(float3(patch.normalX,
        sqrt(saturate(1-patch.normalX*patch.normalX-patch.normalZ*patch.normalZ)),
        patch.normalZ));
    float3 axisX=normalize(float3(1,-patchNormal.x/max(patchNormal.y,.25),0));
    float3 axisZ=normalize(cross(axisX,patchNormal));
    BladeData blade=makeBlade(patch,bladeIndex,patchNormal,axisX,axisZ,tallCount);
    BladeMotion motion=prepareBladeMotion(blade);

    uint segment=min(vertexId/6u,1u);
    uint corner=vertexId%6u;
    float along0=float(segment)*.5;
    float along1=float(segment+1u)*.5;
    float3 center0=bladeCenter(blade,motion,along0);
    float3 center1=bladeCenter(blade,motion,along1);
    float3 tangent0=bladeTangent(blade,motion,along0);
    float3 tangent1=bladeTangent(blade,motion,along1);
    float3 ribbonSide0=cameraFacingSide(center0,tangent0,blade.side);
    float3 ribbonSide1=cameraFacingSide(center1,tangent1,blade.side);
    // The ribbon turns toward the camera for robust sub-pixel coverage, but
    // lighting follows the blade's persistent biological plane.  Otherwise
    // rotating the camera also rotates every grass normal and its shadow tone.
    float3 shadingNormal0=normalize(cross(blade.side,tangent0));
    float3 shadingNormal1=normalize(cross(blade.side,tangent1));

    float physicalWidth0=bladePhysicalHalfWidth(blade,along0);
    float physicalWidth1=bladePhysicalHalfWidth(blade,along1);
    float viewDepth0=max(dot(center0-camera.eye,camera.forward),.02);
    float viewDepth1=max(dot(center1-camera.eye,camera.forward),.02);
    float pixelScale=2*camera.tanHalfFov/max(1.0,(float)camera.resolution.y);
    float minimumHalfWidth0=.60*viewDepth0*pixelScale;
    float minimumHalfWidth1=.60*viewDepth1*pixelScale;
    float widthCap=blade.tall>.5?.043:.018;
    float renderWidth0=min(max(physicalWidth0,minimumHalfWidth0),
                           max(widthCap,minimumHalfWidth0));
    float renderWidth1=min(max(physicalWidth1,minimumHalfWidth1),
                           max(widthCap,minimumHalfWidth1));

    bool upper=(corner==2u||corner==3u||corner==5u);
    bool right=(corner==1u||corner==4u||corner==5u);
    float along=upper?along1:along0;
    float sideSign=right?1.0:-1.0;
    float3 center=upper?center1:center0;
    float3 ribbonSide=upper?ribbonSide1:ribbonSide0;
    float3 shadingNormal=upper?shadingNormal1:shadingNormal0;
    float physicalWidth=upper?physicalWidth1:physicalWidth0;
    float renderWidth=upper?renderWidth1:renderWidth0;
    float3 worldPosition=center+ribbonSide*(renderWidth*sideSign);

    VSOutput output;
    output.position=projectWorld(worldPosition);
    output.worldPosition=worldPosition;
    output.normal=shadingNormal;
    output.bladeCoordinates=float2(sideSign,along);
    // Species was unused by the pixel shader; reuse that interpolant for the
    // patch-coherent water response without increasing varying bandwidth.
    output.bladeParameters=float4(blade.dryness,blade.tall,
                                  blade.waterlineAlong,patch.moisture);
    // Fully submerged blades disappear through the same stochastic coverage
    // path as distant grass; emergent tips remain visible above the waterline.
    float submergedVisibility=lerp(1.0,.06,
        smoothstep(.88,1.0,blade.waterlineAlong));
    output.coverage=distanceCoverage*saturate(physicalWidth/max(renderWidth,1e-5))*
                    submergedVisibility;
    output.ditherSeed=selection;
    output.colourFields=float4(patch.colourFertility,patch.colourDryColony,
                               patch.colourLushColony,patch.colourWarmCool);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
    uint2 pixel=min(uint2(input.position.xy),camera.resolution-1u);
    float sceneViewDepth=SceneDepth.Load(int3(int2(pixel),0)).a;
    float grassViewDepth=dot(input.worldPosition-camera.eye,camera.forward);
    float depthBias=max(.012,min(.035,sceneViewDepth*.00075));
    clip(sceneViewDepth+depthBias-grassViewDepth);

    float edgeDistance=1-abs(input.bladeCoordinates.x);
    float edgeWidth=max(fwidth(input.bladeCoordinates.x),1e-4);
    float edgeCoverage=saturate(edgeDistance/edgeWidth+.5);
    // Let the physical gaps between ribbons provide the meadow transparency.
    // Per-pixel stochastic rejection turned shadowed grass into black/yellow
    // salt-and-pepper noise.  A conservative analytic edge keeps narrow tips
    // anti-aliased without punching random holes through every blade.
    float densityScale=max(camera.grassSettings.x,.001);
    float densityCompensation=pow(min(1.0,3.0/densityScale),.36);
    float coverage=saturate(input.coverage*edgeCoverage*densityCompensation);
    clip(coverage-.16);

    float along=saturate(input.bladeCoordinates.y);
    float dryness=input.bladeParameters.x;
    float tall=input.bladeParameters.y;
    float waterlineAlong=saturate(input.bladeParameters.z);
    float moisture=input.bladeParameters.w;
    float fertile=input.colourFields.x;
    float dryColony=input.colourFields.y;
    float lushColony=input.colourFields.z;
    float warmCool=input.colourFields.w;
    dryness=saturate(dryness+dryColony*.15-lushColony*.08);
    float dryThreshold=lerp(.82,.94,moisture);
    float dry=smoothstep(dryThreshold-.03,dryThreshold+.03,dryness);
    float3 green=lerp(float3(.035,.056,.022),float3(.070,.112,.040),
                      saturate(.28+.55*moisture));
    green*=1.0+warmCool*float3(.035,.006,-.045);
    green*=lerp(float3(.86,.93,.83),float3(1.10,1.08,.91),fertile);
    green=lerp(green,green*float3(1.10,1.01,.76),dryColony*.22);
    green*=lerp(.72,1.04,smoothstep(0,.70,along));
    float3 straw=float3(.118,.103,.052)*lerp(.82,1.06,along)*
                  lerp(.90,1.10,dryColony);
    float3 albedo=lerp(green,straw,dry);
    float albedoLuminance=dot(albedo,float3(.2126,.7152,.0722));
    albedo=lerp(albedoLuminance.xxx,albedo,.78);
    // Puddles weigh down the blade and soak its lower stem.  The tip keeps the
    // atmospheric wetness only, so flattened grass still has readable green
    // detail above the thin water film.
    float waterlineWidth=max(fwidth(along)*1.5,.018);
    float submergedStem=smoothstep(.003,.025,waterlineAlong)*
        (1-smoothstep(waterlineAlong-waterlineWidth,
                      waterlineAlong+waterlineWidth,along));
    float wetness=max(saturate(g_WetnessFactor*.78),submergedStem);
    albedo*=lerp(1.0,.61,wetness);
    albedo*=lerp(1.0,.78,submergedStem);
    float seedHead=tall*smoothstep(.70,.79,along)*(1-smoothstep(.92,1.0,along))
                  *step(.84,dryness);
    albedo=lerp(albedo,float3(.30,.27,.10),seedHead*.46);

    float3 view=normalize(camera.eye-input.worldPosition);
    float3 n=normalize(input.normal);
    if(dot(n,view)<0)n=-n;
    float3 sun=directionToKeyLight();
    float3 keyRadiance=keyLightRadiance();
    // DXR stores the primary surface's actual key-light visibility in alpha.
    // Inferring it from display luminance double-shadowed grass over the tree
    // shadow and incorrectly treated dark bark/soil as occlusion.
    float sunVisibility=SceneColor.Load(int3(int2(pixel),0)).a;
    float frontLight=saturate(dot(n,sun));
    float backLight=saturate(dot(-n,sun));
    float3 ambient=.5*(skyIrradiance(n)+skyIrradiance(-n))*(.56+.16*along);
    ambient+=keyRadiance*float3(.020,.026,.012)*(.35+.65*sunVisibility);
    float3 direct=keyRadiance*frontLight*sunVisibility*1.28;
    // Coverage and scene-depth rejection have already run, so inline rays are
    // issued only for surviving grass fragments inside the local-light range.
    // The raster blades themselves are intentionally not in the
    // TLAS; this tests tree, terrain, and environment occlusion without costly
    // blade-to-blade shadowing.
    LocalLightSample localLight=samplePlayerLocalLight(input.worldPosition);
    float localNdotL=localLight.active*saturate(dot(n,localLight.direction));
    if(localNdotL>1e-4){
        float localVisibility=playerLocalLightVisibility(
            input.worldPosition,localLight.direction,localLight.distance);
        direct+=localLight.radiance*localNdotL*localVisibility*.68;
    }
    float3 transmittedTint=lerp(albedo,float3(.10,.14,.035),.32);
    float3 unmodulated=keyRadiance*transmittedTint*backLight*
                        sunVisibility*.46;
    float3 halfVector=normalize(sun+view);
    float wetExponent=lerp(22.0,110.0,wetness);
    unmodulated+=keyRadiance*pow(saturate(dot(n,halfVector)),wetExponent)*
                 lerp(.025,.13,wetness)*sunVisibility;
    ambient+=lightningRadiance()*(.18+.10*along);
    float fade=lerp(.72,1.0,smoothstep(0,.22,along));
    float3 result=(albedo*(ambient+direct)+unmodulated)*fade;
    float3 rayDirection=normalize(input.worldPosition-camera.eye);
    result=applyAerialPerspective(result,input.worldPosition,rayDirection);
    float3 displayColor=linearToSrgb(colorGrade(tonemap(result*camera.exposure)));
    // Conventional alpha blending preserves the original airy meadow without
    // random black pinholes. Depth writing deliberately keeps this to a
    // single physically nearest ribbon rather than accumulating an opaque
    // stack of dozens of transparent blades.
    return float4(displayColor,saturate(coverage*.58));
}
