#include "tree.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <utility>

namespace dense {
namespace {

uint32_t rgba(unsigned r,unsigned g,unsigned b,unsigned a=255) { return r|(g<<8)|(b<<16)|(a<<24); }
unsigned channel(uint32_t c,int shift) { return (c>>shift)&255u; }
uint32_t vary(uint32_t c,float amount) {
    auto v=[&](int shift){return static_cast<unsigned>(clamp(channel(c,shift)*amount,0,255));};
    return rgba(v(0),v(8),v(16),channel(c,24));
}

void basis(Vec3 axis,Vec3& side,Vec3& up) {
    axis=normalize(axis);const Vec3 helper=std::abs(axis.y)<.92f?Vec3{0,1,0}:Vec3{1,0,0};
    side=normalize(cross(helper,axis));up=normalize(cross(axis,side));
}

std::vector<Vec3> makeCrown(const TreeParameters& p,const SpeciesTraits& t,Rng& rng) {
    std::vector<Vec3> points;points.reserve(static_cast<size_t>(p.attractionPoints));
    while(points.size()<static_cast<size_t>(p.attractionPoints)) {
        Vec3 q{rng.range(-1,1),rng.range(-1,1),rng.range(-1,1)};
        if(lengthSq(q)>1)continue;
        const float y=(q.y+1)*.5f;
        const float azimuth=std::atan2(q.z,q.x);
        float width=std::pow(std::max(.04f,std::sin(y*pi)),t.crownTaper);
        width*=.90f+.10f*std::sin(azimuth*5+p.seed*.013f)+.06f*std::sin(azimuth*3-y*7);
        if(p.species==TreeSpecies::NorwaySpruce) width=std::pow(1-y,.72f)*(.82f+.18f*std::sin(y*pi*9));
        if(p.species==TreeSpecies::UmbrellaAcacia) width=.45f+.55f*std::sin(y*pi*.72f);
        q.y=(q.y/t.crownFlatness);
        points.push_back({q.x*p.crownRadius*width,p.trunkHeight+(q.y+1)*.5f*p.crownHeight,q.z*p.crownRadius*width});
    }
    return points;
}

float skyExposure(Vec3 p,const std::vector<BranchNode>& nodes,size_t count) {
    float blockers=0;
    for(size_t i=0;i<count;i+=2) {
        const Vec3 d=nodes[i].position-p;
        if(d.y>.08f) {
            const float horizontal=d.x*d.x+d.z*d.z;
            blockers+=std::exp(-horizontal*1.8f)*std::exp(-d.y*.18f);
        }
    }
    return std::exp(-blockers*.09f);
}

}

SpeciesTraits TreeGenerator::traits(TreeSpecies species) {
    switch(species) {
    case TreeSpecies::NorwaySpruce:return {L"Norway spruce",.94f,.58f,.42f,.01f,1.25f,1.0f,.003f,.010f,1.15f,2.0f,0xff3d5073u,0xff38592cu};
    case TreeSpecies::SilverBirch:return {L"Silver birch",.70f,.84f,.22f,.13f,.34f,1.12f,.024f,.042f,.82f,2.05f,0xffc4d1d5u,0xff459665u};
    case TreeSpecies::WeepingWillow:return {L"Weeping willow",.30f,.88f,-.10f,.62f,.24f,.86f,.012f,.055f,1.18f,2.18f,0xff53626bu,0xff51a27fu};
    case TreeSpecies::UmbrellaAcacia:return {L"Umbrella acacia",.38f,.92f,.02f,.08f,.12f,2.15f,.008f,.020f,.68f,2.25f,0xff536671u,0xff5a8271u};
    default:return {};
    }
}

TreeParameters TreeGenerator::parametersFor(TreeSpecies species,uint32_t seed) {
    TreeParameters p;p.species=species;p.seed=seed;
    switch(species) {
    case TreeSpecies::NorwaySpruce:p.crownHeight=8.8f;p.crownRadius=3.25f;p.trunkHeight=1.0f;p.attractionPoints=1750;p.growthIterations=155;p.upwardBias=.16f;break;
    case TreeSpecies::SilverBirch:p.crownHeight=8.1f;p.crownRadius=2.75f;p.trunkHeight=2.1f;p.attractionPoints=1150;p.growthIterations=125;p.upwardBias=.18f;break;
    case TreeSpecies::WeepingWillow:p.crownHeight=6.1f;p.crownRadius=4.3f;p.trunkHeight=2.7f;p.attractionPoints=1600;p.growthIterations=155;p.upwardBias=.03f;break;
    case TreeSpecies::UmbrellaAcacia:p.crownHeight=3.3f;p.crownRadius=4.9f;p.trunkHeight=3.4f;p.attractionPoints=1400;p.growthIterations=125;p.upwardBias=.04f;p.waterAvailability=.42f;break;
    default:p.crownHeight=7.1f;p.crownRadius=6.8f;p.trunkHeight=1.9f;p.attractionPoints=1500;p.growthIterations=140;break;
    }
    return p;
}

std::vector<BranchNode> TreeGenerator::grow(const TreeParameters& p) const {
    const SpeciesTraits t=traits(p.species);Rng rng(p.seed);auto attractors=makeCrown(p,t,rng);
    std::vector<BranchNode> nodes;nodes.reserve(5000);nodes.push_back({{0,0,0},{0,1,0},-1,.18f,0,0,0,1,1,true});
    float trunkTarget=p.trunkHeight;
    if(p.species==TreeSpecies::NorwaySpruce)trunkTarget=p.trunkHeight+p.crownHeight*.96f;
    else if(p.species==TreeSpecies::SilverBirch)trunkTarget=p.trunkHeight+p.crownHeight*.42f;
    while(nodes.back().position.y<trunkTarget) {
        const int parent=static_cast<int>(nodes.size())-1;
        const Vec3 lean{std::sin(p.sunlightAzimuth)*.008f,1,std::cos(p.sunlightAzimuth)*.008f};
        nodes.push_back({nodes.back().position+normalize(lean)*p.segmentLength,normalize(lean),parent,.15f,0,0,0,1,1,true});nodes[static_cast<size_t>(parent)].children++;
    }
    if(p.species==TreeSpecies::EnglishOak&&p.fullBiologicalInventory) {
        auto addNode=[&](int parent,Vec3 direction,int order,float scale=1.0f) {
            direction=normalize(direction);const int index=static_cast<int>(nodes.size());
            nodes.push_back({nodes[static_cast<size_t>(parent)].position+direction*p.segmentLength*scale,direction,parent,.01f,0,0,order,1,1,true});
            nodes[static_cast<size_t>(parent)].children++;return index;
        };

        {
            // Mature open-grown Q. robur.  The permanent scaffold is built first
            // from a crooked stem, staggered massive boughs and later partial
            // reiterations.  Foliage is then grown into overlapping peripheral
            // pads; branch counts never stand in for crown architecture.
            struct AxisPath {std::vector<int> nodes;Vec3 outward;Vec3 target;};
            auto limitForkDirection=[](Vec3 parentDirection,Vec3 desired,float minimumDot) {
                parentDirection=normalize(parentDirection);desired=normalize(desired);const float alignment=dot(parentDirection,desired);
                if(alignment>=minimumDot)return desired;
                Vec3 lateral=desired-parentDirection*alignment;
                if(lengthSq(lateral)<.0001f){Vec3 side,up;basis(parentDirection,side,up);lateral=side;}
                return normalize(parentDirection*minimumDot+normalize(lateral)*std::sqrt(std::max(0.0f,1-minimumDot*minimumDot)));
            };
            auto growPath=[&](int parent,Vec3 direction,Vec3 target,Vec3 territoryOutward,int order,float totalLength,float distalSag,bool constrainOutward=true) {
                AxisPath path;path.outward=territoryOutward;path.target=target;
                const float forkDot=order<=1?.42f:(order==2?.57f:.42f);direction=limitForkDirection(nodes[static_cast<size_t>(parent)].direction,direction,forkDot);
                const float unit=order==0?.15f:(order==1?.145f:(order==2?.105f:.080f));
                const int steps=std::max(2,static_cast<int>(std::ceil(totalLength/unit)));const float stepLength=totalLength/steps;
                Vec3 curvature=normalize(Vec3{rng.range(-1,1),rng.range(-.35f,.55f),rng.range(-1,1)});
                for(int step=0;step<steps;++step) {
                    const float q=static_cast<float>(step+1)/steps;const Vec3 position=nodes[static_cast<size_t>(parent)].position;
                    curvature=normalize(curvature*.91f+Vec3{rng.range(-1,1),rng.range(-.40f,.52f),rng.range(-1,1)}*.09f);
                    const float bendQ=clamp(q/.30f,0,1);const float bendIn=bendQ*bendQ*(3-2*bendQ);
                    const float sag=distalSag*clamp((q-.58f)/.42f,0,1);const float baseCurve=order<=1?.185f:(order==2?.135f:.078f);
                    const float baseTarget=order<=1?.235f:(order==2?.285f:.335f);const float curveWeight=baseCurve*(.30f+.70f*bendIn);
                    const float targetWeight=baseTarget*(.24f+.76f*bendIn);const float directionWeight=1-curveWeight-targetWeight;
                    Vec3 desired=normalize(direction*directionWeight+normalize(target-position)*targetWeight+curvature*curveWeight+Vec3{0,sag,0});
                    if(constrainOutward) {
                        const Vec3 horizontal{desired.x,0,desired.z};
                        if(lengthSq(horizontal)>.001f&&dot(normalize(horizontal),territoryOutward)<-.06f)desired=normalize(desired+territoryOutward*.46f);
                    }
                    const Vec3 localProgress=normalize(target-position);if(dot(desired,localProgress)<.12f)desired=normalize(desired+localProgress*.58f);
                    if(dot(desired,direction)<.38f)desired=normalize(direction*.72f+desired*.28f);
                    const float turn=order<=1?.185f:(order==2?.235f:.34f);direction=normalize(lerp(direction,desired,turn));parent=addNode(parent,direction,order,stepLength/p.segmentLength);path.nodes.push_back(parent);
                }
                return path;
            };

            const int fork=static_cast<int>(nodes.size())-1;
            const Vec3 stemTarget{.62f,5.18f,-.43f};
            AxisPath central=growPath(fork,normalize(Vec3{.14f,.98f,-.10f}),stemTarget,normalize(Vec3{.62f,0,-.43f}),0,length(stemTarget-nodes[static_cast<size_t>(fork)].position)*1.025f,.015f,false);
            std::vector<int> stemNodes;stemNodes.reserve(static_cast<size_t>(fork+1)+central.nodes.size());
            for(int i=0;i<=fork;++i)stemNodes.push_back(i);
            stemNodes.insert(stemNodes.end(),central.nodes.begin(),central.nodes.end());
            auto stemAtHeight=[&](float height) {
                int best=stemNodes.front();float error=std::abs(nodes[static_cast<size_t>(best)].position.y-height);
                for(int candidate:stemNodes){const float e=std::abs(nodes[static_cast<size_t>(candidate)].position.y-height);if(e<error){error=e;best=candidate;}}
                return best;
            };

            struct BoughSpec {float baseHeight,azimuth,radius,height,sag,rise;};
            const BoughSpec boughSpecs[]{
                {1.82f,.18f,6.90f,3.20f,-.14f,.30f},
                {2.28f,3.30f,6.48f,3.35f,-.12f,.32f},
                {2.88f,1.34f,6.02f,4.55f,-.070f,.42f},
                {3.48f,5.18f,5.65f,5.25f,-.045f,.48f},
                {4.08f,2.32f,5.10f,6.35f,-.015f,.58f}
            };
            std::vector<AxisPath> boughs;boughs.reserve(std::size(boughSpecs));
            for(const BoughSpec& spec:boughSpecs) {
                const int base=stemAtHeight(spec.baseHeight+rng.range(-.06f,.06f));const float az=spec.azimuth+rng.range(-.09f,.09f);
                const Vec3 outward{std::cos(az),0,std::sin(az)};const Vec3 tangent{-outward.z,0,outward.x};
                const Vec3 target=outward*(spec.radius*rng.range(.94f,1.045f))+Vec3{rng.range(-.18f,.18f),spec.height+rng.range(-.16f,.20f),rng.range(-.18f,.18f)};
                const Vec3 initial=normalize(nodes[static_cast<size_t>(base)].direction*.16f+outward*.79f+Vec3{0,spec.rise,0});
                const Vec3 basePosition=nodes[static_cast<size_t>(base)].position;const float middleRadius=spec.radius*rng.range(.39f,.48f);
                const float middleHeight=basePosition.y+(target.y-basePosition.y)*rng.range(.52f,.66f)+rng.range(.08f,.26f);
                const Vec3 middle=outward*middleRadius+tangent*rng.range(-.42f,.42f)+Vec3{0,middleHeight,0};
                AxisPath proximal=growPath(base,initial,middle,outward,1,length(middle-basePosition)*rng.range(1.015f,1.040f),0);
                const int elbow=proximal.nodes.back();const Vec3 distalDirection=normalize(nodes[static_cast<size_t>(elbow)].direction*.72f+normalize(target-nodes[static_cast<size_t>(elbow)].position)*.42f);
                AxisPath distal=growPath(elbow,distalDirection,target,outward,1,length(target-nodes[static_cast<size_t>(elbow)].position)*rng.range(1.018f,1.050f),spec.sag);
                proximal.nodes.insert(proximal.nodes.end(),distal.nodes.begin(),distal.nodes.end());proximal.target=target;boughs.push_back(std::move(proximal));
            }

            struct ReiterationSpec {int source;float fraction,azimuth,radius,height;};
            const ReiterationSpec reiterationSpecs[]{
                {0,.38f,.58f,3.85f,8.18f},
                {1,.46f,2.65f,3.42f,8.82f},
                {3,.42f,5.86f,3.55f,8.05f},
                {-1,.82f,1.62f,2.72f,8.55f}
            };
            std::vector<AxisPath> reiterations;reiterations.reserve(4);
            for(const ReiterationSpec& spec:reiterationSpecs) {
                const AxisPath& source=spec.source<0?central:boughs[static_cast<size_t>(spec.source)];
                const size_t at=std::min(source.nodes.size()-1,static_cast<size_t>(clamp(spec.fraction,0,1)*(source.nodes.size()-1)));
                const int base=source.nodes[at];const float az=spec.azimuth+rng.range(-.12f,.12f);const Vec3 outward{std::cos(az),0,std::sin(az)};
                const Vec3 target=outward*(spec.radius*rng.range(.92f,1.06f))+Vec3{rng.range(-.20f,.20f),spec.height+rng.range(-.22f,.22f),rng.range(-.20f,.20f)};
                const Vec3 initial=normalize(nodes[static_cast<size_t>(base)].direction*.24f+normalize(target-nodes[static_cast<size_t>(base)].position)*.58f+Vec3{0,.44f,0});
                reiterations.push_back(growPath(base,initial,target,outward,1,length(target-nodes[static_cast<size_t>(base)].position)*1.025f,.012f));
            }

            struct FoliagePad {AxisPath support;Vec3 center;Vec3 outward;float radius,vigor;bool peripheral;};
            std::vector<int> scaffoldNodes=stemNodes;
            std::vector<int> carrierNodes;
            for(const AxisPath& axis:boughs){scaffoldNodes.insert(scaffoldNodes.end(),axis.nodes.begin(),axis.nodes.end());carrierNodes.insert(carrierNodes.end(),axis.nodes.begin(),axis.nodes.end());}
            for(const AxisPath& axis:reiterations){scaffoldNodes.insert(scaffoldNodes.end(),axis.nodes.begin(),axis.nodes.end());carrierNodes.insert(carrierNodes.end(),axis.nodes.begin(),axis.nodes.end());}
            std::vector<unsigned short> attachmentUse(nodes.size());
            struct CrownLobe {float azimuth,radius,height,tangentSpread,radialSpread,verticalSpread;int padCount;bool peripheral;};
            const CrownLobe crownLobes[]{
                {.08f,4.78f,3.82f,2.22f,1.78f,1.38f,12,true},
                {.74f,4.52f,5.14f,2.36f,1.86f,1.64f,11,true},
                {1.46f,3.92f,6.92f,2.14f,1.68f,1.46f,10,true},
                {2.17f,4.58f,5.66f,2.42f,1.92f,1.72f,12,true},
                {2.91f,4.86f,3.94f,2.24f,1.80f,1.42f,11,true},
                {3.52f,4.20f,6.10f,2.12f,1.72f,1.54f,9,true},
                {4.79f,4.72f,4.30f,2.38f,1.90f,1.48f,11,true},
                {5.43f,4.54f,5.58f,2.28f,1.84f,1.66f,12,true},
                {6.03f,3.94f,7.06f,2.12f,1.68f,1.38f,10,true},
                {1.28f,1.72f,8.10f,1.85f,1.28f,.96f,12,false},
                {4.38f,2.05f,7.78f,1.90f,1.34f,1.02f,12,false},
                {.42f,2.72f,4.46f,1.28f,1.02f,1.10f,8,false},
                {1.94f,2.58f,6.24f,1.22f,1.04f,1.18f,8,false},
                {3.62f,2.86f,4.92f,1.34f,1.08f,1.16f,8,false},
                {5.28f,2.52f,6.34f,1.24f,1.02f,1.12f,8,false},
                {2.48f,.72f,5.52f,1.38f,1.26f,1.42f,8,false}
            };
            std::vector<FoliagePad> pads;pads.reserve(360);
            for(size_t lobeIndex=0;lobeIndex<std::size(crownLobes);++lobeIndex) {
                const CrownLobe& lobeSpec=crownLobes[lobeIndex];const float lobeAzimuth=lobeSpec.azimuth+rng.range(-.10f,.10f);
                const Vec3 outward{std::cos(lobeAzimuth),0,std::sin(lobeAzimuth)};const Vec3 tangent{-outward.z,0,outward.x};
                Vec3 lobeCenter=outward*(lobeSpec.radius*rng.range(.96f,1.04f))+Vec3{rng.range(-.12f,.12f),lobeSpec.height+rng.range(-.18f,.18f),rng.range(-.12f,.12f)};
                lobeCenter.y=clamp(lobeCenter.y,2.20f,p.trunkHeight+p.crownHeight*.985f);const float centerQ=clamp((lobeCenter.y-p.trunkHeight)/p.crownHeight,0,1);
                const float centerUpper=(centerQ-.55f)/.45f;const float centerDome=centerQ<.55f?.90f+.10f*(centerQ/.55f):.35f+.65f*std::sqrt(std::max(0.0f,1-centerUpper*centerUpper));
                const float centerPermitted=p.crownRadius*centerDome*(1+.065f*std::sin(lobeAzimuth*3+p.seed*.017f)+.040f*std::sin(lobeAzimuth*5-.8f));const float centerRadius=std::sqrt(lobeCenter.x*lobeCenter.x+lobeCenter.z*lobeCenter.z);
                if(centerRadius>centerPermitted){const float scale=centerPermitted/centerRadius;lobeCenter.x*=scale;lobeCenter.z*=scale;}

                int lobeParent=scaffoldNodes.front();float bestLobeScore=std::numeric_limits<float>::max();const float desiredRadius=std::sqrt(lobeCenter.x*lobeCenter.x+lobeCenter.z*lobeCenter.z);
                const float distalLimit=std::max(.40f,desiredRadius-(lobeSpec.peripheral?1.75f:.95f));
                const std::vector<int>& lobeCandidates=lobeSpec.peripheral?carrierNodes:scaffoldNodes;
                for(int candidate:lobeCandidates) {
                    const Vec3 position=nodes[static_cast<size_t>(candidate)].position;const float candidateRadius=std::sqrt(position.x*position.x+position.z*position.z);
                    const float inwardPenalty=std::max(0.0f,candidateRadius-desiredRadius);const float distalPenalty=std::max(0.0f,candidateRadius-distalLimit);const float highPenalty=std::max(0.0f,position.y-lobeCenter.y-.45f);
                    const float candidateDistance=length(lobeCenter-position);const float shortAxisPenalty=std::max(0.0f,(lobeSpec.peripheral?1.65f:.85f)-candidateDistance);
                    const float score=lengthSq(lobeCenter-position)+inwardPenalty*inwardPenalty*2.4f+distalPenalty*distalPenalty*5.8f+highPenalty*highPenalty*1.8f+shortAxisPenalty*shortAxisPenalty*4.5f+attachmentUse[static_cast<size_t>(candidate)]*.62f;
                    if(score<bestLobeScore){bestLobeScore=score;lobeParent=candidate;}
                }
                attachmentUse[static_cast<size_t>(lobeParent)]++;
                struct PadTarget {Vec3 position;float radius;};
                const float phase=rng.range(0,2*pi);const int padTotal=lobeSpec.peripheral?static_cast<int>(std::round(lobeSpec.padCount*2.25f)):lobeSpec.padCount*2;
                std::vector<PadTarget> targets;targets.reserve(static_cast<size_t>(padTotal));
                for(int padIndex=0;padIndex<padTotal;++padIndex) {
                    const float vertical=clamp(1-2*(padIndex+.5f)/padTotal+rng.range(-.10f,.10f),-1,1);const float horizontal=std::sqrt(std::max(0.0f,1-vertical*vertical));
                    const float theta=phase+padIndex*2.39996323f+rng.range(-.12f,.12f);const float scatter=.42f+.58f*std::pow(rng.unit(),.3333333f);
                    Vec3 target=lobeCenter+(tangent*(std::cos(theta)*horizontal*lobeSpec.tangentSpread)+outward*(std::sin(theta)*horizontal*lobeSpec.radialSpread)+Vec3{0,vertical*lobeSpec.verticalSpread,0})*scatter;
                    target.y=clamp(target.y+(lobeSpec.peripheral?rng.range(.06f,.34f):0),2.20f,p.trunkHeight+p.crownHeight*.985f);
                    const float q=clamp((target.y-p.trunkHeight)/p.crownHeight,0,1);const float azimuth=std::atan2(target.z,target.x);const float upper=(q-.55f)/.45f;
                    const float dome=q<.55f?.90f+.10f*(q/.55f):.35f+.65f*std::sqrt(std::max(0.0f,1-upper*upper));const float crownVariation=1+.065f*std::sin(azimuth*3+p.seed*.017f)+.040f*std::sin(azimuth*5-.8f);
                    const float permittedRadius=p.crownRadius*dome*crownVariation;float targetRadius=std::sqrt(target.x*target.x+target.z*target.z);
                    if(targetRadius>permittedRadius){const float scale=permittedRadius/targetRadius;target.x*=scale;target.z*=scale;targetRadius=permittedRadius;}
                    if(lobeSpec.peripheral&&rng.unit()<.77f) {
                        const float lightShell=permittedRadius*rng.range(.68f,.96f);
                        if(targetRadius<lightShell){const Vec3 radialDirection=targetRadius>.02f?normalize(Vec3{target.x,0,target.z}):outward;target.x=radialDirection.x*lightShell;target.z=radialDirection.z*lightShell;}
                    }

                    const bool smallPad=rng.unit()<(lobeSpec.peripheral?.13f:.18f);
                    const float padRadius=lobeSpec.peripheral
                        ?(smallPad?rng.range(.38f,.50f):rng.range(.56f,.80f))
                        :(smallPad?rng.range(.36f,.46f):rng.range(.46f,.68f));
                    targets.push_back({target,padRadius});
                }

                // A real oak does not connect every foliage mass independently to
                // a central axis.  Nearby masses inherit a shared secondary, then
                // divide repeatedly into unequal daughter territories.  Spatial
                // farthest-pair clustering gives those nested forks without the
                // conspicuous radial fan produced by one connector per pad.
                std::function<void(int,std::vector<int>,int,Vec3)> routeTargets;
                routeTargets=[&](int parent,std::vector<int> targetIds,int depth,Vec3 inheritedDirection) {
                    if(targetIds.empty())return;
                    const Vec3 parentPosition=nodes[static_cast<size_t>(parent)].position;
                    if(targetIds.size()==1||depth>=6) {
                        for(int id:targetIds) {
                            const PadTarget& destination=targets[static_cast<size_t>(id)];
                            Vec3 toTarget=destination.position-parentPosition;
                            if(lengthSq(toTarget)<.0064f)toTarget=tangent*rng.range(-.10f,.10f)+outward*.10f+Vec3{0,.08f,0};
                            const Vec3 direction=normalize(inheritedDirection*.62f+normalize(toTarget)*.56f+outward*.030f+Vec3{0,.018f,0});
                            AxisPath support=growPath(parent,direction,destination.position,outward,3,length(toTarget)*rng.range(1.006f,1.022f),lobeSpec.height<4.2f?-.028f:-.006f,false);
                            const Vec3 center=nodes[static_cast<size_t>(support.nodes.back())].position;
                            float vigor=rng.range(.84f,1.18f);if(rng.unit()<.15f)vigor*=rng.range(.58f,.76f);
                            pads.push_back({std::move(support),center,outward,destination.radius,vigor,lobeSpec.peripheral});
                        }
                        return;
                    }

                    Vec3 centroid{};
                    for(int id:targetIds)centroid=centroid+targets[static_cast<size_t>(id)].position;
                    centroid=centroid/static_cast<float>(targetIds.size());
                    const Vec3 toCentroid=centroid-parentPosition;const float remaining=length(toCentroid);

                    // Stop short of the territory centre so the daughters retain
                    // visible shared ancestry instead of bursting from one hub.
                    const float progress=depth==0?rng.range(.48f,.60f):(depth==1?rng.range(.38f,.51f):rng.range(.28f,.42f));
                    const float distanceCap=depth==0?1.55f:(depth==1?.90f:.56f);
                    const float sharedDistance=std::min(distanceCap,std::max(.10f,remaining*progress));
                    Vec3 toward=remaining>.001f?toCentroid/remaining:normalize(inheritedDirection+outward*.18f+Vec3{0,.08f,0});
                    Vec3 routeSide,routeUp;basis(toward,routeSide,routeUp);
                    Vec3 waypoint=parentPosition+toward*sharedDistance
                        +routeSide*(rng.range(-.075f,.075f)*sharedDistance)
                        +routeUp*(rng.range(-.025f,.065f)*sharedDistance);
                    const Vec3 sharedDirection=normalize(inheritedDirection*.76f+normalize(waypoint-parentPosition)*.48f+outward*.020f+Vec3{0,.015f,0});
                    const int sharedOrder=depth==0?2:3;
                    AxisPath shared=growPath(parent,sharedDirection,waypoint,outward,sharedOrder,length(waypoint-parentPosition)*1.015f,lobeSpec.height<4.2f?-.022f:-.004f,false);
                    const int branchPoint=shared.nodes.back();const Vec3 branchDirection=nodes[static_cast<size_t>(branchPoint)].direction;
                    const bool bridgePad=(depth==1&&rng.unit()<.68f)||(depth==2&&rng.unit()<.17f);
                    if(bridgePad) {
                        AxisPath bridge=shared;const size_t first=static_cast<size_t>(bridge.nodes.size()*.42f);
                        if(first>0&&first<bridge.nodes.size())bridge.nodes.erase(bridge.nodes.begin(),bridge.nodes.begin()+first);
                        const Vec3 bridgeCenter=nodes[static_cast<size_t>(bridge.nodes.back())].position+outward*rng.range(.05f,.18f)+Vec3{0,rng.range(.04f,.16f),0};
                        const float bridgeRadius=lobeSpec.peripheral?rng.range(.44f,.62f):rng.range(.38f,.54f);const float bridgeVigor=rng.range(.60f,.84f);
                        pads.push_back({std::move(bridge),bridgeCenter,outward,bridgeRadius,bridgeVigor,lobeSpec.peripheral});
                    }

                    int seedA=targetIds.front(),seedB=targetIds.back();float widest=-1;
                    for(size_t a=0;a<targetIds.size();++a)for(size_t b=a+1;b<targetIds.size();++b) {
                        const float d=lengthSq(targets[static_cast<size_t>(targetIds[a])].position-targets[static_cast<size_t>(targetIds[b])].position);
                        if(d>widest){widest=d;seedA=targetIds[a];seedB=targetIds[b];}
                    }
                    const Vec3 splitAxis=normalize(targets[static_cast<size_t>(seedB)].position-targets[static_cast<size_t>(seedA)].position);
                    std::sort(targetIds.begin(),targetIds.end(),[&](int lhs,int rhs){return dot(targets[static_cast<size_t>(lhs)].position-centroid,splitAxis)<dot(targets[static_cast<size_t>(rhs)].position-centroid,splitAxis);});
                    const size_t cut=std::max<size_t>(1,std::min(targetIds.size()-1,static_cast<size_t>(std::round(targetIds.size()*rng.range(.39f,.61f)))));
                    std::vector<int> first(targetIds.begin(),targetIds.begin()+cut),second(targetIds.begin()+cut,targetIds.end());
                    auto groupCentroid=[&](const std::vector<int>& group){Vec3 result{};for(int id:group)result=result+targets[static_cast<size_t>(id)].position;return result/static_cast<float>(group.size());};
                    const Vec3 firstDirection=normalize(groupCentroid(first)-parentPosition),secondDirection=normalize(groupCentroid(second)-parentPosition);
                    const float firstScore=dot(firstDirection,branchDirection)+first.size()*.012f;const float secondScore=dot(secondDirection,branchDirection)+second.size()*.012f;
                    const size_t lateralAt=std::min(shared.nodes.size()-1,static_cast<size_t>(rng.range(.68f,.88f)*(shared.nodes.size()-1)));
                    const int lateralParent=shared.nodes[lateralAt];const Vec3 lateralDirection=nodes[static_cast<size_t>(lateralParent)].direction;
                    if(firstScore>=secondScore) {
                        routeTargets(branchPoint,std::move(first),depth+1,branchDirection);
                        routeTargets(lateralParent,std::move(second),depth+1,lateralDirection);
                    } else {
                        routeTargets(branchPoint,std::move(second),depth+1,branchDirection);
                        routeTargets(lateralParent,std::move(first),depth+1,lateralDirection);
                    }
                };
                std::vector<int> targetIds;targetIds.reserve(targets.size());for(size_t i=0;i<targets.size();++i)targetIds.push_back(static_cast<int>(i));
                const Vec3 lobeDirection=normalize(nodes[static_cast<size_t>(lobeParent)].direction*.72f+normalize(lobeCenter-nodes[static_cast<size_t>(lobeParent)].position)*.52f+Vec3{0,.040f,0});
                routeTargets(lobeParent,std::move(targetIds),0,lobeDirection);
            }

            struct TwigTip {int node;Vec3 direction;Vec3 target;Vec3 curvature;float phase;};
            struct LeafBudSite {int parent;Vec3 outward;float phase;bool peripheral;};
            std::vector<LeafBudSite> leafBudSites;leafBudSites.reserve(42000);
            for(size_t padIndex=0;padIndex<pads.size();++padIndex) {
                const FoliagePad& pad=pads[padIndex];Vec3 forward=normalize(Vec3{pad.outward.x,0,pad.outward.z});const Vec3 lateral{-forward.z,0,forward.x};
                auto samplePadTarget=[&]() {
                    const float theta=rng.range(0,2*pi),vertical=pad.peripheral?rng.range(-.38f,1.0f):rng.range(-1,1);const float horizontal=std::sqrt(std::max(0.0f,1-vertical*vertical));
                    const float radial=pad.radius*std::pow(rng.unit(),.3333333f);
                    const float forwardComponent=pad.peripheral?std::sin(theta)*horizontal*.46f+.28f:std::sin(theta)*horizontal*.76f;
                    Vec3 target=pad.center+(lateral*(std::cos(theta)*horizontal)+forward*forwardComponent+Vec3{0,vertical*(pad.peripheral?.58f:.66f),0})*radial;
                    target=target+Vec3{0,pad.radius*(pad.peripheral?.085f:.035f),0};target.y=clamp(target.y,2.05f,p.trunkHeight+p.crownHeight*1.02f);return target;
                };

                const float vigor=clamp(pad.vigor,.38f,1.20f);const int nominalSeeds=pad.peripheral?5+static_cast<int>(padIndex&1):4;
                std::vector<TwigTip> tips;const int seeds=std::max(2,static_cast<int>(std::round(nominalSeeds*clamp(vigor,.52f,1.18f))));
                tips.reserve(720);
                for(int seed=0;seed<seeds;++seed) {
                    const float sourceFraction=(seed+rng.range(.18f,.82f))/seeds;const size_t sourceAt=std::min(pad.support.nodes.size()-1,static_cast<size_t>(sourceFraction*(pad.support.nodes.size()-1)));
                    const int parent=pad.support.nodes[sourceAt];const Vec3 target=samplePadTarget();const float phase=seed*2.51327412f+padIndex*.381966f+rng.range(-.20f,.20f);
                    const Vec3 curvature=normalize(Vec3{rng.range(-1,1),rng.range(-.30f,.55f),rng.range(-1,1)});
                    tips.push_back({parent,normalize(nodes[static_cast<size_t>(parent)].direction*.30f+normalize(target-nodes[static_cast<size_t>(parent)].position)*.75f+Vec3{0,.035f,0}),target,curvature,phase});
                }

                const int seasons=std::max(7,static_cast<int>(std::round((pad.peripheral?11.0f:9.0f)+(vigor-1)*3.2f)));
                const size_t tipLimit=static_cast<size_t>(std::round((pad.peripheral?230.0f:120.0f)*clamp(vigor,.46f,1.18f)));
                const float survival=(pad.peripheral?.90f:.82f)*clamp(.62f+.38f*vigor,.68f,1.04f);
                for(int season=0;season<seasons&&!tips.empty();++season) {
                    std::vector<TwigTip> next;next.reserve(std::min(tipLimit,tips.size()*2));
                    for(const TwigTip& tip:tips) {
                        if(next.size()>=tipLimit)break;
                        Vec3 target=tip.target;
                        if(lengthSq(target-nodes[static_cast<size_t>(tip.node)].position)<.018f)target=samplePadTarget();
                        const Vec3 curvature=normalize(tip.curvature*.82f+Vec3{rng.range(-1,1),rng.range(-.32f,.55f),rng.range(-1,1)}*.18f);
                        Vec3 direction=normalize(tip.direction*.52f+normalize(target-nodes[static_cast<size_t>(tip.node)].position)*.36f+curvature*.12f+Vec3{rng.range(-.035f,.035f),rng.range(-.030f,.045f),rng.range(-.035f,.035f)});
                        const float unitLength=.024f+.096f*std::pow(rng.unit(),2.0f);const int terminal=addNode(tip.node,direction,3,unitLength/p.segmentLength);
                        next.push_back({terminal,direction,target,curvature,tip.phase+rng.range(-.10f,.10f)});

                        const float activation=(pad.peripheral?.37f:.28f)*clamp(.68f+.32f*vigor,.70f,1.06f);
                        if(next.size()<tipLimit&&rng.unit()<activation) {
                            Vec3 side,localUp;basis(direction,side,localUp);const float phase=tip.phase+2.51327412f+rng.range(-.20f,.20f);
                            Vec3 radial=normalize(side*std::cos(phase)+localUp*std::sin(phase));if(radial.y<-.42f)radial=normalize(radial+Vec3{0,.24f,0});
                            const Vec3 lateralTarget=samplePadTarget();const Vec3 lateralDirection=normalize(direction*.34f+radial*.64f+normalize(lateralTarget-nodes[static_cast<size_t>(terminal)].position)*.24f+Vec3{0,.035f,0});
                            const float lateralLength=.022f+.086f*std::pow(rng.unit(),2.35f);const int lateralNode=addNode(terminal,lateralDirection,3,lateralLength/p.segmentLength);
                            const Vec3 lateralCurvature=normalize(curvature*.42f+radial*.38f+Vec3{rng.range(-.5f,.5f),rng.range(-.18f,.38f),rng.range(-.5f,.5f)}*.20f);
                            next.push_back({lateralNode,lateralDirection,lateralTarget,lateralCurvature,phase});
                        }
                    }
                    if(season>=seasons-3&&season<seasons-1) {
                        const float cohortChance=survival*(season==seasons-2?.23f:.12f);
                        for(const TwigTip& tip:next)if(rng.unit()<cohortChance)leafBudSites.push_back({tip.node,forward,tip.phase,pad.peripheral});
                    }
                    tips=std::move(next);
                }
                for(const TwigTip& tip:tips)if(rng.unit()<survival*.68f)leafBudSites.push_back({tip.node,forward,tip.phase,pad.peripheral});
            }

            // A surviving old twig normally activates its terminal bud and only
            // occasionally one of the crowded subapical candidates.  This yields
            // a branching high-order spray whose endpoints fill the pad volume,
            // rather than comb-like rows of leaves along long decorative rays.
            for(size_t siteIndex=0;siteIndex<leafBudSites.size();++siteIndex) {
                const LeafBudSite& site=leafBudSites[siteIndex];const int parent=site.parent;const Vec3 parentDirection=nodes[static_cast<size_t>(parent)].direction;Vec3 side,localUp;basis(parentDirection,side,localUp);
                const int activated=1+(rng.unit()<(site.peripheral?.14f:.055f)?1:0);
                for(int bud=0;bud<activated;++bud) {
                    const float phase=site.phase+siteIndex*.0618034f+bud*2.51327412f+rng.range(-.15f,.15f);
                    Vec3 radial=normalize(side*std::cos(phase)+localUp*std::sin(phase));if(radial.y<-.40f)radial=normalize(radial+Vec3{0,.25f,0});
                    if(dot(radial,site.outward)<-.16f)radial=normalize(radial+site.outward*.62f);
                    const Vec3 direction=site.peripheral
                        ?normalize(parentDirection*.26f+radial*.48f+site.outward*.34f+Vec3{0,.12f,0})
                        :normalize(parentDirection*.31f+radial*.67f+site.outward*.12f+Vec3{0,.060f,0});
                    const float shootLength=.0205f+.0595f*std::pow(rng.unit(),4.2f);const int shoot=addNode(parent,direction,4,shootLength/p.segmentLength);
                    nodes[static_cast<size_t>(shoot)].growthUnitStart=parent;nodes[static_cast<size_t>(shoot)].birthSeason=110;nodes[static_cast<size_t>(shoot)].currentYear=true;
                }
            }
            for(size_t i=1;i<nodes.size();++i)nodes[i].lightExposure=skyExposure(nodes[i].position,nodes,std::min(nodes.size(),static_cast<size_t>(2200)));
            return nodes;
        }

        // A mature oak is a hierarchy of rhythmic growth units and partial
        // reiterations, not a radial set of continuously splitting curves.
        int leader=static_cast<int>(nodes.size())-1;std::vector<int> trunkNodes;
        while(nodes[static_cast<size_t>(leader)].position.y<p.trunkHeight+p.crownHeight*.40f) {
            const float y=nodes[static_cast<size_t>(leader)].position.y;
            leader=addNode(leader,{std::sin(y*.57f)*.035f,1,std::cos(y*.49f)*.035f},0);trunkNodes.push_back(leader);
        }

        struct AxisTip {int node;Vec3 direction;int order;int generation;float vigor;};
        std::vector<AxisTip> active;active.reserve(16000);
        // Unequal sequential reiterations arise from different trunk heights and
        // partially repeat the orthotropic juvenile axis.
        constexpr int reiterationCount=7;
        for(int r=0;r<reiterationCount;++r) {
            const float heightFraction=.10f+.66f*(static_cast<float>(r)/reiterationCount)+rng.range(-.035f,.035f);
            const size_t trunkAt=std::min(trunkNodes.size()-1,static_cast<size_t>(clamp(heightFraction,0,1)*(trunkNodes.size()-1)));
            const float az=r*2.39996323f+rng.range(-.38f,.38f);const Vec3 radial{std::cos(az),0,std::sin(az)};
            const float rise=.82f+(.38f-.82f)*heightFraction;const Vec3 direction=normalize(radial+Vec3{0,rise,0}*rng.range(.55f,.82f));
            active.push_back({trunkNodes[trunkAt],direction,1,0,rng.range(.78f,1.18f)});
        }
        // A mature oak loses a single conical leader: several unequal upper
        // reiterations share dominance and close the uneven dome.
        for(int crownFork=0;crownFork<3;++crownFork){const float az=.7f+crownFork*2.05f+rng.range(-.24f,.24f);active.push_back({leader,normalize(Vec3{std::cos(az)*.42f,.82f+(crownFork==0?.12f:0),std::sin(az)*.42f}),1,0,rng.range(.76f,.94f)});}

        auto sampledShootLength=[&](int order,float vigor) {
            static constexpr float maximum[]{0,.30f,.20f,.12f,.08f};
            static constexpr float mode[]{0,.030f,.027f,.023f,.020f};
            const float exponent=order==1?2.2f:(order==2?3.0f:4.2f);
            const float tail=std::pow(rng.unit(),exponent);float length=mode[order]+(maximum[order]-mode[order])*tail;
            if(rng.unit()<.055f&&order<4)length*=1.55f; // proleptic/Lammas shoot
            return clamp(length*vigor,mode[order]*.65f,maximum[order]);
        };
        // Safety guard only; crown population is determined by the 72-season
        // bud lifecycle, not by the old 14k visual inventory target.
        constexpr size_t maxOakNodes=250000;
        auto growUnit=[&](int parent,Vec3 direction,int order,float length) {
            const int pieces=std::max(1,static_cast<int>(std::ceil(length/.11f)));int tip=parent;
            for(int piece=0;piece<pieces&&nodes.size()<maxOakNodes;++piece)tip=addNode(tip,direction,order,(length/pieces)/p.segmentLength);
            return tip;
        };

        for(int season=0;season<110&&!active.empty()&&nodes.size()<maxOakNodes;++season) {
          std::vector<AxisTip> next;next.reserve(std::min<size_t>(maxOakNodes-nodes.size(),active.size()*2));
          for(const AxisTip axis:active) {
            const float mortality=axis.order==4?.08f:(axis.order==3?.055f:(axis.order==2?.012f:.003f));
            if(rng.unit()<mortality)continue;
            Vec3 outward{nodes[static_cast<size_t>(axis.node)].position.x,0,nodes[static_cast<size_t>(axis.node)].position.z};
            if(lengthSq(outward)>.01f)outward=normalize(outward);
            Vec3 terminalDirection=normalize(axis.direction*.94f+outward*.055f+Vec3{rng.range(-.035f,.035f),rng.range(-.018f,.035f),rng.range(-.035f,.035f)});
            if(axis.order==1&&axis.generation>12)terminalDirection=normalize(terminalDirection+Vec3{0,-.035f,0});
            const float length=sampledShootLength(axis.order,axis.vigor);const int terminal=growUnit(axis.node,terminalDirection,axis.order,length);
            if(terminal==axis.node)break;
            nodes[static_cast<size_t>(terminal)].age=axis.generation;
            if(axis.order<4||rng.unit()>.32f)next.push_back({terminal,terminalDirection,axis.order,axis.generation+1,axis.vigor*rng.range(.94f,1.01f)});

            // About four subapical buds occupy a pseudowhorl immediately below
            // the terminal bud. Only a position- and order-dependent subset grows.
            const int lateralOrder=std::min(4,axis.order+1);Vec3 side,up;basis(terminalDirection,side,up);
            const float subapicalActivation=axis.order==1?.16f:(axis.order==2?.20f:(axis.order==3?.40f:.0f));
            const int lateralParent=nodes[static_cast<size_t>(terminal)].parent;
            for(int bud=0;bud<4&&nodes.size()<maxOakNodes;++bud)if(rng.unit()<subapicalActivation*axis.vigor) {
                const float angle=bud*pi*.5f+rng.range(-.22f,.22f)+axis.generation*.61f;
                const Vec3 radial=side*std::cos(angle)+up*std::sin(angle);
                const Vec3 childDirection=normalize(terminalDirection*.54f+radial*.74f+Vec3{0,.10f,0});
                next.push_back({lateralParent,childDirection,lateralOrder,0,axis.vigor*rng.range(.64f,.84f)});
            }

            // Six to ten median buds are typical (occasionally up to 21), but
            // most remain dormant; their low activation makes crown windows.
            int medianBuds=clamp(static_cast<int>(std::round(rng.range(5.5f,10.5f))),0,21);
            if(rng.unit()<.04f)medianBuds+=static_cast<int>(rng.range(4,11));
            const float medianActivation=axis.order==1?.025f:(axis.order==2?.020f:(axis.order==3?.025f:.0f));
            for(int bud=0;bud<medianBuds&&nodes.size()<maxOakNodes;++bud)if(rng.unit()<medianActivation*axis.vigor) {
                const float angle=bud*2.51327412f+axis.generation*.37f;const Vec3 radial=side*std::cos(angle)+up*std::sin(angle);
                const Vec3 childDirection=normalize(terminalDirection*.66f+radial*.55f);
                next.push_back({axis.node,childDirection,lateralOrder,0,axis.vigor*rng.range(.48f,.70f)});
            }
          }
          active=std::move(next);
        }

        // Preserve a small population of recently shed/dead high-order twigs.
        for(size_t i=1;i<nodes.size();++i)if(nodes[i].axisOrder>=3&&nodes[i].children==0&&rng.unit()<.075f)nodes[i].alive=false;
        return nodes;
    }
    if(p.species==TreeSpecies::NorwaySpruce) {
        const size_t trunkCount=nodes.size();const size_t first=std::max<size_t>(3,static_cast<size_t>(p.trunkHeight/p.segmentLength));
        for(size_t level=first;level+2<trunkCount;level+=3) {
            const float phase=static_cast<float>(level)*.83f;
            for(int arm=0;arm<4;++arm) {
                const float angle=phase+arm*pi*.5f;const float relativeHeight=nodes[level].position.y/trunkTarget;
                const Vec3 direction=normalize(Vec3{std::cos(angle),-.30f+.24f*relativeHeight,std::sin(angle)});
                nodes.push_back({nodes[level].position+direction*p.segmentLength,direction,static_cast<int>(level),.02f,0,0,1,1,1,true});nodes[level].children++;
            }
        }
    }

    for(int season=0;season<p.growthIterations&&!attractors.empty();++season) {
        const size_t count=nodes.size();std::vector<Vec3>sums(count);std::vector<int>influences(count);
        for(const Vec3 a:attractors) {
            int best=-1;float bestD=p.attractionRadius*p.attractionRadius;
            for(size_t i=0;i<count;++i)if(nodes[i].alive){const float d=lengthSq(a-nodes[i].position);if(d<bestD){bestD=d;best=static_cast<int>(i);}}
            if(best>=0){sums[static_cast<size_t>(best)]+=normalize(a-nodes[static_cast<size_t>(best)].position);influences[static_cast<size_t>(best)]++;}
        }
        int added=0;
        for(size_t i=0;i<count;++i) {
            BranchNode& bud=nodes[i];bud.age++;
            if(!bud.alive||influences[i]==0)continue;
            bud.lightExposure=skyExposure(bud.position,nodes,count);
            const float assimilation=bud.lightExposure*p.waterAvailability;
            bud.carbonReserve=clamp(bud.carbonReserve*.72f+assimilation*.55f-.12f,0,2);
            if(bud.carbonReserve<.16f)continue;
            Vec3 light=normalize(sums[i]/static_cast<float>(influences[i]));
            const float side=1-std::abs(dot(light,{0,1,0}));
            const float dominance=t.apicalDominance*(1-side);
            Vec3 desired=light*t.phototropism+bud.direction*(.72f+dominance)+Vec3{0,t.gravitropism,0};
            if(bud.axisOrder>0){float sag=clamp(static_cast<float>(bud.age)/24,0,1);if(p.species==TreeSpecies::WeepingWillow)sag*=1.35f;desired.y-=t.branchDroop*sag;}
            if(p.species==TreeSpecies::EnglishOak&&bud.axisOrder>0) {
                Vec3 outward{bud.position.x,0,bud.position.z};
                if(lengthSq(outward)>.04f)desired+=normalize(outward)*(.16f+.035f*bud.axisOrder);
            }
            desired=normalize(desired+Vec3{rng.range(-.025f,.025f),0,rng.range(-.025f,.025f)});
            const float turn=p.species==TreeSpecies::WeepingWillow?.34f:.52f;
            Vec3 direction=normalize(lerp(bud.direction,desired,turn));
            const Vec3 pos=bud.position+direction*p.segmentLength;
            bool duplicate=false;for(size_t j=0;j<count;++j)if(lengthSq(pos-nodes[j].position)<p.segmentLength*p.segmentLength*.16f){duplicate=true;break;}
            if(!duplicate){const int order=bud.axisOrder+((bud.children>0||dot(direction,bud.direction)<.76f)?1:0);nodes.push_back({pos,direction,static_cast<int>(i),.02f,0,0,std::min(order,4),bud.lightExposure,bud.carbonReserve*.55f,true});bud.children++;bud.carbonReserve*=.62f;++added;}
        }
        if(added==0)break;
        attractors.erase(std::remove_if(attractors.begin(),attractors.end(),[&](Vec3 a){for(size_t i=count;i<nodes.size();++i)if(lengthSq(a-nodes[i].position)<p.killRadius*p.killRadius)return true;return false;}),attractors.end());
    }
    if(p.species==TreeSpecies::EnglishOak&&p.fullBiologicalInventory) {
        std::vector<int> frontier;frontier.reserve(4096);
        for(size_t i=1;i<nodes.size();++i)if(nodes[i].children==0)frontier.push_back(static_cast<int>(i));
        const size_t target=14000;size_t cursor=0;
        while(nodes.size()<target&&cursor<frontier.size()) {
            int parent=frontier[cursor++];const int extensionCount=2+static_cast<int>(rng.unit()*3);int tip=parent;
            for(int step=0;step<extensionCount&&nodes.size()<target;++step) {
                const BranchNode& source=nodes[static_cast<size_t>(tip)];
                Vec3 outward{source.position.x,0,source.position.z};
                if(lengthSq(outward)>.01f)outward=normalize(outward);
                const Vec3 jitter{rng.range(-.14f,.14f),rng.range(-.035f,.11f),rng.range(-.14f,.14f)};
                const Vec3 direction=normalize(source.direction*1.18f+outward*.10f+jitter);
                const Vec3 position=nodes[static_cast<size_t>(tip)].position+direction*p.segmentLength*rng.range(.48f,.78f);
                const int child=static_cast<int>(nodes.size());nodes.push_back({position,direction,tip,.007f,0,0,std::min(4,nodes[static_cast<size_t>(tip)].axisOrder+1),nodes[static_cast<size_t>(tip)].lightExposure,1,true});nodes[static_cast<size_t>(tip)].children++;tip=child;
            }
            frontier.push_back(tip);
            const int laterals=1+(rng.unit()<.42f?1:0);
            for(int lateral=0;lateral<laterals&&nodes.size()<target;++lateral) {
                const BranchNode& source=nodes[static_cast<size_t>(tip)];Vec3 side,u;basis(source.direction,side,u);const float sign=lateral?1.0f:-1.0f;
                const Vec3 direction=normalize(source.direction*.42f+side*sign*rng.range(.55f,.88f)+u*rng.range(-.18f,.28f));
                const int child=static_cast<int>(nodes.size());nodes.push_back({source.position+direction*p.segmentLength*rng.range(.42f,.68f),direction,tip,.006f,0,0,4,source.lightExposure,1,true});nodes[static_cast<size_t>(tip)].children++;frontier.push_back(child);
            }
        }
    }
    return nodes;
}

TreeMesh TreeGenerator::buildMesh(std::vector<BranchNode>& nodes,const TreeParameters& p) const {
    TreeMesh mesh;if(nodes.size()<2)return mesh;const SpeciesTraits t=traits(p.species);
    mesh.structuralSegments=static_cast<uint32_t>(std::min<size_t>(nodes.size()-1,2000));mesh.fineShootSegments=static_cast<uint32_t>(nodes.size()-1)-mesh.structuralSegments;
    std::vector<float> area(nodes.size());
    for(size_t i=0;i<nodes.size();++i) {
        if(p.species==TreeSpecies::EnglishOak&&p.fullBiologicalInventory)area[i]=(nodes[i].alive&&nodes[i].currentYear)?.01672f:.000001f;
        else area[i]=(nodes[i].children==0?.00016f:.000003f)*(.35f+.9f*nodes[i].lightExposure);
    }
    for(size_t ri=nodes.size();ri-->1;)area[static_cast<size_t>(nodes[ri].parent)]+=area[ri];
    for(size_t i=0;i<nodes.size();++i) {
        float radius=p.species==TreeSpecies::EnglishOak&&p.fullBiologicalInventory
            ?clamp(std::pow(std::max(area[i],.000001f)*.000307f,1/2.10f),.0016f,.68f)
            :clamp(std::pow(area[i],1/t.pipeExponent)*.82f,.0016f,.45f);
        if(p.species==TreeSpecies::EnglishOak) {
            // Pipe-model area preserves the trunk, while botanical branch order
            // constrains the young wood that otherwise becomes centimetre-thick wire.
            static constexpr float orderLimit[]{.68f,.38f,.16f,.055f,.0090f};
            radius=std::min(radius,orderLimit[std::min(nodes[i].axisOrder,4)]);
            if(nodes[i].children==0)radius=std::min(radius,.0028f);
        }
        nodes[i].radius=radius;
    }

    // Build a transported frame along each botanical axis.  Recomputing a
    // cylinder frame independently for every segment twists the ring vertices
    // at bends and exposes dark seams.  The transported frame and root-to-node
    // arc length keep both geometry and bark relief continuous through a limb.
    std::vector<int> continuationChild(nodes.size(),-1);
    std::vector<float> continuationScore(nodes.size(),-2.0f),arcLength(nodes.size());
    for(size_t i=1;i<nodes.size();++i) {
        const size_t parent=static_cast<size_t>(nodes[i].parent);const Vec3 segment=normalize(nodes[i].position-nodes[parent].position);
        arcLength[i]=arcLength[parent]+length(nodes[i].position-nodes[parent].position);
        if(nodes[i].axisOrder==nodes[parent].axisOrder) {
            const float score=dot(segment,nodes[parent].direction);
            if(score>continuationScore[parent]){continuationScore[parent]=score;continuationChild[parent]=static_cast<int>(i);}
        }
    }
    std::vector<Vec3> tangents(nodes.size()),frameSides(nodes.size()),frameUps(nodes.size());
    for(size_t i=0;i<nodes.size();++i) {
        Vec3 incoming=nodes[i].direction;
        if(nodes[i].parent>=0)incoming=normalize(nodes[i].position-nodes[static_cast<size_t>(nodes[i].parent)].position);
        Vec3 tangent=incoming;
        if(continuationChild[i]>=0) {
            const Vec3 outgoing=normalize(nodes[static_cast<size_t>(continuationChild[i])].position-nodes[i].position);
            tangent=normalize(incoming+outgoing);
        }
        tangents[i]=tangent;
        if(nodes[i].parent<0)basis(tangent,frameSides[i],frameUps[i]);
        else {
            const Vec3 inherited=frameSides[static_cast<size_t>(nodes[i].parent)];const Vec3 projected=inherited-tangent*dot(inherited,tangent);
            if(lengthSq(projected)<.0025f)basis(tangent,frameSides[i],frameUps[i]);
            else {frameSides[i]=normalize(projected);frameUps[i]=normalize(cross(tangent,frameSides[i]));}
        }
    }

    // Major oak forks carry load into the supporting axis as longitudinal
    // reaction wood. A child-only collar can hide an intersection, but it
    // cannot form the broad Y-shaped saddle seen on mature trunks. Propagate
    // at most two dominant load lobes down the parent axis (and briefly above
    // the crotch) so nearby continuation rings deform before the daughter
    // emerges. The bounded two-lobe field keeps both topology and build cost
    // independent of the full biological shoot inventory.
    std::vector<std::array<Vec3,2>> reactionLoads(nodes.size());
    auto addReactionLoad=[&](size_t node,Vec3 load) {
        if(lengthSq(load)<1.0e-8f)return;
        auto& lobes=reactionLoads[node];
        for(Vec3& existing:lobes) {
            if(lengthSq(existing)>1.0e-8f&&dot(normalize(existing),normalize(load))>.72f) {
                existing+=load;
                const float magnitude=length(existing);
                if(magnitude>.22f)existing=existing*(.22f/magnitude);
                return;
            }
        }
        Vec3* target=lengthSq(lobes[0])<lengthSq(lobes[1])?&lobes[0]:&lobes[1];
        if(lengthSq(*target)<lengthSq(load))*target=load;
    };
    if(p.species==TreeSpecies::EnglishOak) {
        for(size_t child=1;child<nodes.size();++child) {
            const size_t junction=static_cast<size_t>(nodes[child].parent);
            if(continuationChild[junction]==static_cast<int>(child)||nodes[child].axisOrder>1||
               nodes[child].radius<.018f)continue;
            const Vec3 daughterAxis=normalize(nodes[child].position-nodes[junction].position);
            Vec3 loadDirection=daughterAxis-tangents[junction]*dot(daughterAxis,tangents[junction]);
            if(lengthSq(loadDirection)<.01f)continue;
            loadDirection=normalize(loadDirection);
            const float ratio=clamp(nodes[child].radius/std::max(nodes[junction].radius,.001f),0,1);
            const float strength=.19f*std::pow(ratio,1.35f);
            const float extent=clamp(.18f+nodes[junction].radius*2.25f,.38f,1.35f);

            size_t cursor=junction;float travelled=0;
            while(true) {
                const float x=clamp(travelled/extent,0,1),falloff=(1-x)*(1-x);
                addReactionLoad(cursor,loadDirection*(strength*falloff));
                if(nodes[cursor].parent<0||travelled>=extent)break;
                const size_t ancestor=static_cast<size_t>(nodes[cursor].parent);
                if(nodes[ancestor].axisOrder!=nodes[junction].axisOrder)break;
                travelled+=length(nodes[cursor].position-nodes[ancestor].position);
                cursor=ancestor;
            }

            int forward=continuationChild[junction];travelled=0;
            const float forwardExtent=extent*.42f;
            while(forward>=0&&travelled<forwardExtent) {
                const size_t forwardNode=static_cast<size_t>(forward);
                travelled+=length(nodes[forwardNode].position-
                                  nodes[static_cast<size_t>(nodes[forwardNode].parent)].position);
                const float x=clamp(travelled/std::max(forwardExtent,.001f),0,1);
                addReactionLoad(forwardNode,loadDirection*(strength*(1-x)*(1-x)*.72f));
                forward=continuationChild[forwardNode];
            }
        }
    }

    const float speciesMaterial=static_cast<float>(static_cast<unsigned>(p.species))*.1f;
    const float buttressPhase=p.seed*.017f;
    // Keep seven mesh ranges for deterministic topology, but do not present
    // them as a seven-pointed star.  Four roots carry most of the visible load;
    // the remaining roots are shorter, narrower and become subsurface near the
    // collar.  Their irregular azimuths also drive the bole's basal lobes so the
    // flutes and roots read as one structure instead of two stacked patterns.
    constexpr int oakRootCount=7;
    constexpr std::array<float,oakRootCount> oakRootStrength{
        1.00f,.34f,.82f,.24f,.93f,.39f,.70f
    };
    constexpr std::array<float,oakRootCount> oakRootJitter{
        -.11f,.20f,-.23f,.09f,-.04f,.27f,-.18f
    };
    auto oakRootAngle=[&](int root) {
        return (pi*.5f-buttressPhase+2*pi*root)/oakRootCount+oakRootJitter[root];
    };
    for(size_t i=1;i<nodes.size();++i) {
        const BranchNode& b=nodes[i];const size_t parentIndex=static_cast<size_t>(b.parent);const BranchNode& a=nodes[parentIndex];const uint32_t base=static_cast<uint32_t>(mesh.branchVertices.size());
        const int sides=p.species==TreeSpecies::EnglishOak?(b.axisOrder==0?48:(b.axisOrder==1?32:(b.axisOrder==2?16:8))):14;
        const bool continuation=continuationChild[parentIndex]==static_cast<int>(i);const float childRatio=b.radius/std::max(a.radius,.0001f);
        const bool structuralCollar=!continuation&&b.axisOrder<=2;
        const bool codominantCollar=structuralCollar&&childRatio>=.72f;
        const int ringVertices=sides+1,ringCount=codominantCollar?6:(structuralCollar?5:(b.axisOrder<=2?3:2));const Vec3 segmentAxis=normalize(b.position-a.position);const float segmentLength=length(b.position-a.position);
        // A lateral limb is born inside its supporting pipe, not as a full-width
        // cylinder glued to the outside.  The old 1--3 cm overlap was smaller
        // than the radius of a mature scaffold limb, exposing the first ring as
        // a hard cuff.  Keep a narrow insertion ring buried in the parent and
        // let it swell around the emergence point.  This is intentionally mesh
        // only: it does not alter botanical nodes, pipe-model radii, or crown
        // architecture.
        const float overlap=continuation?0.0f:std::min(segmentLength*(codominantCollar?.46f:(structuralCollar?.38f:.20f)),
            structuralCollar?std::max(b.radius*(codominantCollar?.46f:.38f),a.radius*(codominantCollar?.21f:.15f)):std::max(b.radius*.24f,.004f));
        const float uvOverlap=std::min(overlap,structuralCollar?.022f:.008f);
        const float buriedRadius=continuation?a.radius:
            std::min(b.radius*(structuralCollar?.46f:.78f),a.radius*(structuralCollar?.25f:.34f));
        const Vec3 collarStart=a.position-segmentAxis*overlap;
        const float collarPathLength=segmentLength+overlap;
        // A modest Hermite bend makes a structural daughter share the parent's
        // grain direction while it is still below the bark, then turn toward
        // its own axis outside the collar.  Handles are deliberately short to
        // prevent decorative loops at acute oak forks.
        const Vec3 collarEntryTangent=structuralCollar?
            normalize(segmentAxis*(codominantCollar?.80f:.88f)+
                      tangents[parentIndex]*(codominantCollar?.20f:.12f)):segmentAxis;
        const Vec3 collarExitTangent=tangents[i];
        const Vec3 collarEntryHandle=collarEntryTangent*(collarPathLength*.58f);
        const Vec3 collarExitHandle=collarExitTangent*(collarPathLength*.58f);
        const bool carriesOwnReaction=lengthSq(reactionLoads[i][0])+lengthSq(reactionLoads[i][1])>
                                       1.0e-8f;
        Vec3 startSide=frameSides[parentIndex],startUp=frameUps[parentIndex];
        if(!continuation){Vec3 projected=startSide-collarEntryTangent*dot(startSide,collarEntryTangent);if(lengthSq(projected)<.0025f)projected=startUp-collarEntryTangent*dot(startUp,collarEntryTangent);if(lengthSq(projected)<.0025f)basis(collarEntryTangent,startSide,startUp);else{startSide=normalize(projected);startUp=normalize(cross(collarEntryTangent,startSide));}}
        for(int ring=0;ring<ringCount;++ring)for(int k=0;k<=sides;++k) {
            const float linearQ=static_cast<float>(ring)/(ringCount-1),q=structuralCollar?std::pow(linearQ,1.35f):linearQ,smoothQ=q*q*(3-2*q);const float texU=static_cast<float>(k)/sides;const float angle=2*pi*texU;
            Vec3 ringCenter=lerp(collarStart,b.position,q);Vec3 ringTangent=normalize(lerp(continuation?tangents[parentIndex]:collarEntryTangent,collarExitTangent,smoothQ));
            if(structuralCollar) {
                const float q2=q*q,q3=q2*q;
                ringCenter=collarStart*(2*q3-3*q2+1)+collarEntryHandle*(q3-2*q2+q)+
                           b.position*(-2*q3+3*q2)+collarExitHandle*(q3-q2);
                const Vec3 derivative=collarStart*(6*q2-6*q)+collarEntryHandle*(3*q2-4*q+1)+
                                      b.position*(-6*q2+6*q)+collarExitHandle*(3*q2-2*q);
                if(lengthSq(derivative)>.000001f)ringTangent=normalize(derivative);
            }
            Vec3 ringSide=lerp(startSide,frameSides[i],smoothQ);ringSide=ringSide-ringTangent*dot(ringSide,ringTangent);if(lengthSq(ringSide)<.0025f){Vec3 fallbackUp;basis(ringTangent,ringSide,fallbackUp);}else ringSide=normalize(ringSide);const Vec3 ringUp=normalize(cross(ringTangent,ringSide));const Vec3 radial=normalize(ringSide*std::cos(angle)+ringUp*std::sin(angle));const Vec3 circumferential=normalize(ringUp*std::cos(angle)-ringSide*std::sin(angle));
            float flare=1;const float ringHeight=ringCenter.y;
            float dFlareTheta=0,dFlareArc=0;
            if(p.species==TreeSpecies::EnglishOak&&a.axisOrder==0&&b.axisOrder==0) {
                const float basal=std::exp(-std::max(0.0f,ringHeight)/.68f);
                float rootLobes=0,dRootLobesTheta=0;
                for(int root=0;root<oakRootCount;++root) {
                    const float strength=oakRootStrength[root];
                    const float delta=std::atan2(std::sin(angle-oakRootAngle(root)),
                                                 std::cos(angle-oakRootAngle(root)));
                    const float sigma=.22f+.075f*strength;
                    const float lobe=std::exp(-.5f*delta*delta/(sigma*sigma));
                    const float amplitude=.14f+.12f*strength;
                    rootLobes+=amplitude*lobe;
                    dRootLobesTheta+=amplitude*lobe*(-delta/(sigma*sigma));
                }
                flare+=basal*(.22f+rootLobes);
                dFlareTheta=basal*dRootLobesTheta;
                dFlareArc=-(flare-1.0f)/.68f*ringTangent.y;
            } else if(a.parent<0&&ring==0)flare=1.75f+.22f*std::max(0.0f,std::sin(angle*4));
            if(p.species==TreeSpecies::EnglishOak&&b.axisOrder<=1&&
               (continuation||carriesOwnReaction)) {
                float reactionFlare=0,reactionTheta=0,reactionArc=0;
                const float blendDerivative=(ringCount==2?1.0f:6*q*(1-q))/
                                            std::max(collarPathLength,.001f);
                for(int lobe=0;lobe<2;++lobe) {
                    const Vec3 startLoad=continuation?reactionLoads[parentIndex][lobe]:Vec3{};
                    const Vec3 rawLoad=lerp(startLoad,
                                            reactionLoads[i][lobe],smoothQ);
                    Vec3 load=rawLoad-ringTangent*dot(rawLoad,ringTangent);
                    const float magnitude=length(load);if(magnitude<.0001f)continue;
                    const Vec3 direction=load/magnitude;
                    const float alignment=dot(radial,direction);
                    const float x=clamp((alignment+.18f)/1.18f,0,1);
                    const float lobeWeight=x*x*(3-2*x);
                    const float dLobeDx=6*x*(1-x);
                    const float dxDAlignment=alignment>-.18f&&alignment<1.0f?1/1.18f:0;
                    const float dAlignmentTheta=dot(circumferential,direction);
                    reactionFlare+=magnitude*(.10f+.90f*lobeWeight);
                    reactionTheta+=magnitude*.90f*dLobeDx*dxDAlignment*dAlignmentTheta;

                    const Vec3 rawDerivative=(reactionLoads[i][lobe]-startLoad)*blendDerivative;
                    const Vec3 loadDerivative=rawDerivative-
                                              ringTangent*dot(rawDerivative,ringTangent);
                    const float magnitudeDerivative=dot(direction,loadDerivative);
                    const Vec3 directionDerivative=(loadDerivative-
                                                    direction*magnitudeDerivative)/magnitude;
                    const float alignmentDerivative=dot(radial,directionDerivative);
                    reactionArc+=magnitudeDerivative*(.10f+.90f*lobeWeight)+
                                 magnitude*.90f*dLobeDx*dxDAlignment*alignmentDerivative;
                }
                if(reactionFlare>.26f) {
                    const float scale=.26f/reactionFlare;
                    reactionFlare=.26f;reactionTheta*=scale;reactionArc*=scale;
                }
                flare+=reactionFlare;dFlareTheta+=reactionTheta;dFlareArc+=reactionArc;
            }
            float baseRadius=0,dBaseRadiusDq=0,dBaseRadiusDTheta=0;
            if(continuation) {
                baseRadius=a.radius+(b.radius-a.radius)*smoothQ;
                // Two-ring fine shoots are linearly interpolated by the
                // triangles even though their endpoint samples coincide with
                // smoothstep. Preserve the physical taper in their normals.
                dBaseRadiusDq=(b.radius-a.radius)*(ringCount==2?1.0f:6*q*(1-q));
            } else {
                const float emergenceQ=clamp(overlap/std::max(collarPathLength,.001f),.08f,.52f);
                const float neckEnd=clamp(emergenceQ+(structuralCollar?.34f:.24f),.20f,.76f);
                const float neckX=clamp(q/neckEnd,0,1),neckBlend=neckX*neckX*(3-2*neckX);
                const float dNeckDq=q>0&&q<neckEnd?6*neckX*(1-neckX)/neckEnd:0;
                const float collarWidth=structuralCollar?.18f:.14f,collarOffset=(q-emergenceQ)/collarWidth;
                const float collarPulse=std::exp(-collarOffset*collarOffset);
                const float collarStrength=codominantCollar?.14f:(structuralCollar?.10f:.035f);
                // A real branch collar is a saddle of reaction wood, not an
                // inflatable ring.  Weight the flare toward the parent's grain
                // direction and analytically carry its angular derivative into
                // the normal.  This removes the circumferential donut visible
                // at close range while retaining the upper/lower collar lobes.
                const float parentAlignment=dot(radial,tangents[parentIndex]);
                const float parentAlignmentDerivative=dot(circumferential,tangents[parentIndex]);
                const float saddleFloor=codominantCollar?.42f:.25f;
                const float saddleSpan=1-saddleFloor;
                const float saddleWeight=structuralCollar?
                    saddleFloor+saddleSpan*parentAlignment*parentAlignment:1.0f;
                const float dSaddleDTheta=structuralCollar?
                    2*saddleSpan*parentAlignment*parentAlignmentDerivative:0.0f;
                baseRadius=buriedRadius+(b.radius-buriedRadius)*neckBlend+
                           b.radius*collarStrength*collarPulse*saddleWeight;
                dBaseRadiusDq=(b.radius-buriedRadius)*dNeckDq-
                    b.radius*collarStrength*collarPulse*(2*collarOffset/collarWidth)*saddleWeight;
                dBaseRadiusDTheta=b.radius*collarStrength*collarPulse*dSaddleDTheta;
            }
            const float arcStart=std::max(0.0f,arcLength[parentIndex]-uvOverlap),arc=arcStart+(arcLength[i]-arcStart)*q;
            const float phase5=angle*5+arc*.38f,phase11=angle*9-arc*.24f,phase2=angle*2+arc*.61f;const float relief=1+.0050f*std::sin(phase5)+.0025f*std::sin(phase11)+.0015f*std::sin(phase2);
            const float dReliefTheta=.0250f*std::cos(phase5)+.0225f*std::cos(phase11)+.0030f*std::cos(phase2);const float dReliefArc=.00190f*std::cos(phase5)-.00060f*std::cos(phase11)+.000915f*std::cos(phase2);
            const float radius=baseRadius*flare*relief;const float maturity=std::sqrt(clamp(baseRadius/.18f,0,1));const float ageShade=.86f+.12f*maturity;
            const float dBaseRadiusArc=dBaseRadiusDq/std::max(collarPathLength,.001f);const float dRadiusTheta=dBaseRadiusDTheta*flare*relief+baseRadius*(relief*dFlareTheta+flare*dReliefTheta);const float dRadiusArc=dBaseRadiusArc*flare*relief+baseRadius*(relief*dFlareArc+flare*dReliefArc);const Vec3 surfaceNormal=normalize(radial-circumferential*(dRadiusTheta/std::max(radius,.00001f))-ringTangent*dRadiusArc);
            mesh.branchVertices.push_back({ringCenter+radial*radius,surfaceNormal,vary(t.barkColor,ageShade),speciesMaterial,texU,arc});
        }
        for(int ring=0;ring<ringCount-1;++ring)for(int k=0;k<sides;++k){const uint32_t current=base+ring*ringVertices+static_cast<uint32_t>(k),next=current+1,endCurrent=current+ringVertices,endNext=endCurrent+1;mesh.branchIndices.insert(mesh.branchIndices.end(),{current,next,endCurrent,next,endNext,endCurrent});}
    }

    if(p.species==TreeSpecies::EnglishOak&&p.fullBiologicalInventory) {
        // Irregular oak buttresses begin as tall, broad webs inside the bole,
        // flatten as they spread, and disappear below grade while still broad.
        // This avoids the exposed tapered sausages / spear tips produced by a
        // uniform elliptical tube.  The last rings remain deliberately below
        // grade, so their small open ends can never become visible caps.
        constexpr int rootSegments=12,rootSides=16;const float trunkRadius=nodes.front().radius;
        for(int root=0;root<oakRootCount;++root) {
            const float theta=oakRootAngle(root),strength=oakRootStrength[root];
            Vec3 radial=frameSides[0]*std::cos(theta)+frameUps[0]*std::sin(theta);radial.y=0;
            if(lengthSq(radial)<.01f)radial={std::cos(theta),0,std::sin(theta)};
            radial=normalize(radial);const Vec3 side{-radial.z,0,radial.x};
            const float lengthVariation=1.0f+.08f*std::sin(root*1.91f+1.2f);
            const float rootLength=.34f+trunkRadius*(1.05f+1.45f*strength)*lengthVariation;
            const uint32_t base=static_cast<uint32_t>(mesh.branchVertices.size());
            auto rootPoint=[&](float q,float angle) {
                q=clamp(q,0,1);const float smoothQ=q*q*(3-2*q),taper=1-smoothQ;
                const float dominance=strength<.45f?.74f:1.0f;
                const float shoulder=1.0f+.20f*std::exp(-std::pow((q-.20f)/.19f,2.0f));
                const float width=.0025f+trunkRadius*(.16f+.34f*strength)*
                                  taper*shoulder*dominance;
                const float height=.0015f+trunkRadius*(.30f+.42f*strength)*
                                   taper*(1-.32f*q)*dominance;
                const float bend=std::sin(q*pi)*(.055f+.045f*(1-strength))*
                                 std::sin(root*1.37f+.4f);
                const float burialStart=.30f+.30f*strength;
                float burial=clamp((q-burialStart)/std::max(1-burialStart,.001f),0,1);
                burial=burial*burial*(3-2*burial);
                const float burialDepth=.085f+trunkRadius*(.075f+.035f*(1-strength));
                const Vec3 center=radial*(trunkRadius*.04f+rootLength*q)+side*bend+
                                  Vec3{0,height*.52f-.016f-burialDepth*burial,0};
                return center+side*(std::cos(angle)*width)+Vec3{0,std::sin(angle)*height,0};
            };
            for(int segment=0;segment<=rootSegments;++segment) {
                const float q=static_cast<float>(segment)/rootSegments;const float arc=q*rootLength;
                for(int k=0;k<=rootSides;++k) {
                    const float texU=static_cast<float>(k)/rootSides,angle=2*pi*texU,dq=.25f/rootSegments,dTheta=.25f*(2*pi/rootSides);const Vec3 point=rootPoint(q,angle),longitudinal=rootPoint(q+dq,angle)-rootPoint(q-dq,angle),around=rootPoint(q,angle+dTheta)-rootPoint(q,angle-dTheta);Vec3 normal=normalize(cross(longitudinal,around));const Vec3 centerline=rootPoint(q,0)*.5f+rootPoint(q,pi)*.5f;if(dot(normal,point-centerline)<0)normal=normal*-1.0f;
                    const float shade=.82f+.16f*(1-q);mesh.branchVertices.push_back({point,normal,vary(t.barkColor,shade),speciesMaterial,texU,arc});
                }
            }
            const int ringVertices=rootSides+1;
            for(int segment=0;segment<rootSegments;++segment)for(int k=0;k<rootSides;++k) {
                const uint32_t a=base+segment*ringVertices+k,b=a+1,c=a+ringVertices,d=c+1;mesh.branchIndices.insert(mesh.branchIndices.end(),{a,b,c,b,d,c});
            }
        }
    }

    Rng rng(p.seed^0x9e3779b9u);const Vec3 sun=normalize(Vec3{std::sin(p.sunlightAzimuth),1.35f,std::cos(p.sunlightAzimuth)});
    auto emitLeaf=[&](size_t i,int ordinal=-1,int clusterSize=1) {
        const float w=t.leafWidth*rng.range(.68f,.98f),h=t.leafLength*rng.range(.68f,.98f),exposure=nodes[i].lightExposure;Vec3 center{},facing{},s{},u{};
        if(ordinal>=0&&p.species==TreeSpecies::EnglishOak){
            // Oak leaves alternate around the current-year shoot.  Extend that shoot
            // visually from the terminal node instead of stacking a rosette at every
            // branch vertex; this removes the artificial bottle-brush silhouette.
            Vec3 twigSide,twigUp;basis(nodes[i].direction,twigSide,twigUp);
            // Quercus commonly expresses a 2/5 alternate spiral: 144 degrees.
            const float angle=ordinal*2.51327412f+i*.6180339f+rng.range(-.11f,.11f);
            const Vec3 radial=normalize(twigSide*std::cos(angle)+twigUp*std::sin(angle));
            // Crowded oak growth units carry many buds over only a few cm;
            // eleven leaves now fit within the measured <=8 cm order-4 shoot.
            Vec3 attachment=nodes[i].position;
            if(nodes[i].currentYear&&nodes[i].growthUnitStart>=0) {
                const Vec3 start=nodes[static_cast<size_t>(nodes[i].growthUnitStart)].position;
                const float along=(ordinal+.5f)/std::max(1,clusterSize);
                attachment=lerp(start,nodes[i].position,along);
            }
            facing=normalize(Vec3{0,.42f,0}+sun*.20f+radial*.54f+Vec3{rng.range(-.10f,.10f),rng.range(-.04f,.08f),rng.range(-.10f,.10f)});
            // The petiole leaves the shoot radially.  The blade midrib follows
            // that petiole, not the parent branch axis.
            u=normalize(radial-facing*dot(radial,facing));
            if(lengthSq(u)<.01f)basis(facing,s,u);else s=normalize(cross(u,facing));
            center=attachment+u*(.013f+h*.92f);
        }
        else{center=nodes[i].position+Vec3{rng.range(-.09f,.09f),rng.range(-.035f,.11f),rng.range(-.09f,.09f)};facing=normalize(lerp(sun,Vec3{rng.range(-1,1),rng.range(.15f,1),rng.range(-1,1)},.66f));if(p.species==TreeSpecies::UmbrellaAcacia)facing=normalize(lerp(facing,{0,1,0},.5f));basis(facing,s,u);}
        const float young=clamp(1-static_cast<float>(nodes[i].age)/32,0,1);const uint32_t green=vary(t.leafColor,.66f+.19f*exposure+.08f*young+rng.range(-.035f,.035f));const uint32_t base=static_cast<uint32_t>(mesh.leafVertices.size());const float leafMaterial=1+speciesMaterial;
        const int outline=p.species==TreeSpecies::NorwaySpruce?4:10;mesh.leafVertices.push_back({center,facing,green,leafMaterial,.5f,.5f});
        for(int k=0;k<outline;++k){const float angle=2*pi*k/outline;float lobe=1;if(p.species==TreeSpecies::EnglishOak)lobe=.78f+.22f*std::abs(std::sin(angle*3));const Vec3 edge=center+s*(std::cos(angle)*w*lobe)+u*(std::sin(angle)*h);mesh.leafVertices.push_back({edge,facing,green,leafMaterial,.5f+.5f*std::cos(angle),.5f+.5f*std::sin(angle)});}
        for(int k=0;k<outline;++k){const uint32_t a=base+1+static_cast<uint32_t>(k),b=base+1+static_cast<uint32_t>((k+1)%outline);mesh.leafIndices.insert(mesh.leafIndices.end(),{base,a,b});}
        mesh.leafCount++;mesh.totalLeafAreaM2+=pi*w*h*.72f;
    };
    if(p.species==TreeSpecies::EnglishOak&&p.fullBiologicalInventory) {
        std::vector<size_t> candidates,anchors;
        for(size_t i=1;i<nodes.size();++i) {
            if(nodes[i].alive&&nodes[i].currentYear&&nodes[i].axisOrder==4&&nodes[i].children==0&&nodes[i].radius<=.0048f)candidates.push_back(i);
        }
        if(candidates.empty())for(size_t i=1;i<nodes.size();++i)if(nodes[i].children==0)candidates.push_back(i);
        anchors=candidates;
        mesh.leafVertices.reserve(anchors.size()*12*11);mesh.leafIndices.reserve(anchors.size()*12*30);
        for(size_t anchor:anchors) {
            // Measured mean is 11.2 leaves per Q. robur shoot. Keep every
            // individual shoot within the observed compact population instead
            // of forcing a whole-crown leaf quota onto too few terminals.
            const int leaves=8+static_cast<int>(rng.unit()*7.0f);
            for(int ordinal=0;ordinal<leaves;++ordinal)emitLeaf(anchor,ordinal,leaves);
        }
    } else for(size_t i=1;i<nodes.size();++i) {
        const bool terminal=nodes[i].children==0;const bool thin=nodes[i].radius<.024f;float chance=.10f*t.leafDensity;int terminalCluster=7,innerCluster=1;
        if(p.species==TreeSpecies::NorwaySpruce){chance=.62f;terminalCluster=18;innerCluster=9;}
        else if(p.species==TreeSpecies::SilverBirch){chance=.20f;terminalCluster=10;innerCluster=3;}
        else if(p.species==TreeSpecies::WeepingWillow){chance=.26f;terminalCluster=13;innerCluster=4;}
        else if(p.species==TreeSpecies::UmbrellaAcacia){chance=.38f;terminalCluster=20;innerCluster=7;}
        if(!(terminal||(thin&&rng.unit()<chance)))continue;
        const int cluster=terminal?terminalCluster:innerCluster;
        for(int leaf=0;leaf<cluster;++leaf)emitLeaf(i);
    }
    return mesh;
}
}
