struct Camera {
    float3 eye; float tanHalfFov;
    float3 forward; float aspect;
    float3 right; uint frameIndex;
    float3 up; uint maxFrames;
    float3 sunDirection; float exposure;
    uint2 resolution; float timeSeconds; float windStrength;
    float4 atmosphere;
    float4 grassSettings;
    float4 groundSettings;
};

struct GrassPatch {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
    uint seed, packed;
    float baseY, normalX, normalZ, moisture;
};

Texture2D<float4> SceneDepth : register(t0);
Texture2D<float4> SceneColor : register(t1);
StructuredBuffer<GrassPatch> GrassPatches : register(t2);
ConstantBuffer<Camera> camera : register(b0);
cbuffer GrassDraw : register(b1) {
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
    return -normalize(camera.sunDirection);
}

float3 skyIrradiance(float3 normal) {
    float up=saturate(normal.y*.5+.5);
    float sunHeight=saturate(directionToSun().y);
    float3 horizon=lerp(float3(.38,.46,.53),float3(.58,.69,.78),sunHeight);
    float3 zenith=lerp(float3(.075,.13,.22),float3(.16,.31,.52),sunHeight);
    return lerp(horizon,zenith,pow(up,.58))*(.72+.18*sunHeight);
}

float3 clearSkyAirlight(float3 direction) {
    float3 d=normalize(direction);
    float up=saturate(d.y),sunHeight=saturate(directionToSun().y);
    float3 horizon=lerp(float3(.32,.39,.47),float3(.34,.52,.69),sunHeight);
    float3 zenith=lerp(float3(.055,.075,.16),float3(.035,.16,.43),sunHeight);
    return lerp(horizon,zenith,pow(up,.58));
}

float3 applyAerialPerspective(float3 radiance,float3 hit,float3 rayDirection) {
    float distanceToHit=distance(camera.eye,hit);
    float eyeDensity=exp(-max(camera.eye.y,0.0)/120.0);
    float hitDensity=exp(-max(hit.y,0.0)/120.0);
    float transmittance=exp(-camera.atmosphere.w*distanceToHit*.5*(eyeDensity+hitDensity));
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
};

BladeData makeBlade(GrassPatch patch,uint bladeIndex,float3 patchNormal,
                    float3 axisX,float3 axisZ,uint tallCount) {
    BladeData blade;
    blade.normal=patchNormal;
    uint seed=hashUint(patch.seed^((bladeIndex+1u)*0x9e3779b9u));
    blade.tall=bladeIndex<tallCount?1.0:0.0;
    float radius=sqrt(randomUint(seed))*lerp(.265,.072,blade.tall);
    float offsetAngle=randomUint(seed^0x68bc21ebu)*6.2831853;
    uint subclump=bladeIndex%3u;
    float clusterAngle=randomUint(patch.seed^0x91e10da5u)*6.2831853+
                       float(subclump)*2.0943951+
                       (randomUint(seed^0x243f6a88u)-.5)*.34;
    float clusterRadius=lerp(.10,.15,randomUint(patch.seed^(subclump*0x9e3779b9u)))*blade.tall;
    blade.base=float3((patch.minX+patch.maxX)*.5,patch.baseY,
                      (patch.minZ+patch.maxZ)*.5)
              +axisX*(cos(offsetAngle)*radius+cos(clusterAngle)*clusterRadius)
              +axisZ*(sin(offsetAngle)*radius+sin(clusterAngle)*clusterRadius);
    float patchAngle=randomUint(patch.seed^0x02e5be93u)*6.2831853;
    float bladeAngle=patchAngle+float(bladeIndex)*2.39996323+
                     (randomUint(seed^0x68bc21ebu)-.5)*.42;
    blade.side=normalize(axisX*cos(bladeAngle)+axisZ*sin(bladeAngle));
    blade.naturalLean=normalize(cross(blade.side,blade.normal));
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
    blade.leanStrength=lerp(blade.tall>.5?.22:.025,blade.tall>.5?.45:.14,
                            randomUint(seed^0x94d049bbu));
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

struct BladeMotion {
    float3 windDirection;
    float bend;
    float flutterPhase;
    float flutterAmplitude;
};

BladeMotion prepareBladeMotion(BladeData blade) {
    BladeMotion motion;
    float compliance=lerp(.43,.17,blade.stiffness)*lerp(1.0,1.18,blade.tall);
    motion.windDirection=grassWindDirection(blade.normal);
    motion.bend=blade.height*camera.windStrength*compliance*grassGust(blade);
    motion.flutterPhase=camera.timeSeconds*(6.5+2.5*(1-blade.stiffness))+blade.phase;
    motion.flutterAmplitude=blade.height*.013*camera.windStrength;
    return motion;
}

float3 bladeCenter(BladeData blade,BladeMotion motion,float along) {
    float s=saturate(along),shape=s*s*(2-s);
    float flutter=sin(motion.flutterPhase+s*5.0)*motion.flutterAmplitude*s*s;
    return blade.base+blade.normal*(blade.height*s)
         +blade.naturalLean*(blade.height*blade.leanStrength*shape)
         +motion.windDirection*(motion.bend*shape)+blade.side*flutter;
}

float3 bladeTangent(BladeData blade,BladeMotion motion,float along) {
    float s=saturate(along);
    float shapeDerivative=4*s-3*s*s;
    float phase=motion.flutterPhase+s*5.0;
    float flutterDerivative=motion.flutterAmplitude*
                            (5*cos(phase)*s*s+2*sin(phase)*s);
    return normalize(blade.normal*blade.height
         +blade.naturalLean*(blade.height*blade.leanStrength*shapeDerivative)
         +motion.windDirection*(motion.bend*shapeDerivative)
         +blade.side*flutterDerivative);
}

float3 cameraFacingSide(float3 center,float3 tangent,float3 fallbackSide) {
    float3 viewDirection=normalize(camera.eye-center);
    float3 side=cross(tangent,viewDirection);
    if(dot(side,side)<1e-6)side=fallbackSide-tangent*dot(fallbackSide,tangent);
    return normalize(side);
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
    float3 patchCenter=float3((patch.minX+patch.maxX)*.5,patch.baseY,
                              (patch.minZ+patch.maxZ)*.5);
    float patchDistance=distance(camera.eye,patchCenter);
    if(patchDistance>=tallDistance||(tallCount==0u&&patchDistance>=shortDistance))
        return inactiveVertex();

    uint activeCount=patchDistance>=shortDistance?tallCount:candidateCount;
    if(bladeIndex>=activeCount)return inactiveVertex();

    bool tallBlade=bladeIndex<tallCount;
    float shortCoverage=1-smoothstep(shortDistance*.70,shortDistance,patchDistance);
    float tallCoverage=1-smoothstep(tallDistance*.70,tallDistance,patchDistance);
    float distanceCoverage=tallBlade?tallCoverage:shortCoverage;
    float shortLodDensity=lerp(.68,1.0,1-smoothstep(3.0,shortDistance,patchDistance));
    float tallLodDensity=lerp(.62,1.0,1-smoothstep(3.0,tallDistance,patchDistance));
    float lodDensity=tallBlade?tallLodDensity:shortLodDensity;
    uint selection=patch.seed^((bladeIndex+19u)*0x27d4eb2du);
    if(distanceCoverage<=0||(bladeIndex>=2u&&randomUint(selection)>lodDensity))
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
    float3 ribbonNormal0=normalize(cross(ribbonSide0,tangent0));
    float3 ribbonNormal1=normalize(cross(ribbonSide1,tangent1));
    if(dot(ribbonNormal0,camera.eye-center0)<0)ribbonNormal0=-ribbonNormal0;
    if(dot(ribbonNormal1,camera.eye-center1)<0)ribbonNormal1=-ribbonNormal1;

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
    float3 ribbonNormal=upper?ribbonNormal1:ribbonNormal0;
    float physicalWidth=upper?physicalWidth1:physicalWidth0;
    float renderWidth=upper?renderWidth1:renderWidth0;
    float3 worldPosition=center+ribbonSide*(renderWidth*sideSign);

    VSOutput output;
    output.position=projectWorld(worldPosition);
    output.worldPosition=worldPosition;
    output.normal=ribbonNormal;
    output.bladeCoordinates=float2(sideSign,along);
    output.bladeParameters=float4(blade.dryness,blade.tall,blade.species,patch.moisture);
    output.coverage=distanceCoverage*saturate(physicalWidth/max(renderWidth,1e-5));
    output.ditherSeed=selection;
    return output;
}

uint bayer2(uint2 p) {
    return p.y==0u?(p.x==0u?0u:2u):(p.x==0u?3u:1u);
}

float orderedThreshold(uint2 pixel,uint seed) {
    uint2 offset=uint2(hashUint(seed)&3u,hashUint(seed^0x9e3779b9u)&3u);
    uint2 p=(pixel+offset)&3u;
    uint value=4u*bayer2(p&1u)+bayer2((p>>1u)&1u);
    return (float(value)+.5)*(1.0/16.0);
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
    float coverage=saturate(input.coverage*edgeCoverage);
    clip(coverage-orderedThreshold(pixel,input.ditherSeed));

    float along=saturate(input.bladeCoordinates.y);
    float dryness=input.bladeParameters.x;
    float tall=input.bladeParameters.y;
    float species=input.bladeParameters.z;
    float moisture=input.bladeParameters.w;
    float dryThreshold=lerp(.82,.94,moisture);
    float dry=smoothstep(dryThreshold-.03,dryThreshold+.03,dryness);
    float3 green=lerp(float3(.040,.068,.014),float3(.095,.155,.030),
                      saturate(.28+.55*moisture));
    green*=species<.5?float3(.97,1.03,.91):
           (species<1.5?float3(1.04,.98,.90):float3(.92,1.01,1.02));
    green*=lerp(.72,1.04,smoothstep(0,.70,along));
    float3 straw=float3(.145,.122,.042)*lerp(.82,1.06,along);
    float3 albedo=lerp(green,straw,dry);
    float seedHead=tall*smoothstep(.70,.79,along)*(1-smoothstep(.92,1.0,along))
                  *step(.84,dryness);
    albedo=lerp(albedo,float3(.30,.27,.10),seedHead*.46);

    float3 n=normalize(input.normal);
    float3 sun=directionToSun();
    float3 sceneLinear=srgbToLinear(SceneColor.Load(int3(int2(pixel),0)).rgb);
    float sceneLuminance=dot(sceneLinear,float3(.2126,.7152,.0722));
    float localVisibility=lerp(.30,1.0,smoothstep(.018,.16,sceneLuminance));
    float frontLight=saturate(dot(n,sun));
    float backLight=saturate(dot(-n,sun));
    float3 ambient=skyIrradiance(n)*(.50+.18*along)*lerp(.72,1.0,localVisibility);
    float3 direct=float3(1.02,.94,.73)*frontLight*localVisibility*1.18;
    direct+=float3(.42,.74,.20)*backLight*localVisibility*.72;
    float3 view=normalize(camera.eye-input.worldPosition);
    float3 halfVector=normalize(sun+view);
    direct+=pow(saturate(dot(n,halfVector)),22)*.08*localVisibility;
    float3 result=albedo*(ambient+direct)*lerp(.58,1.0,smoothstep(0,.22,along));
    float3 rayDirection=normalize(input.worldPosition-camera.eye);
    result=applyAerialPerspective(result,input.worldPosition,rayDirection);
    float3 displayColor=linearToSrgb(colorGrade(tonemap(result*camera.exposure)));
    return float4(displayColor,1);
}
