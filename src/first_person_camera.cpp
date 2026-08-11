#include "first_person_camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace dense {
namespace {

float positiveFinite(float value,float fallback,float minimum) {
    return std::isfinite(value)&&value>=minimum?value:fallback;
}

FirstPersonCameraSettings sanitize(FirstPersonCameraSettings value) {
    value.eyeHeight=positiveFinite(value.eyeHeight,1.68f,.1f);
    value.minimumEyeClearance=std::clamp(
        std::isfinite(value.minimumEyeClearance)?value.minimumEyeClearance:1.50f,
        .02f,value.eyeHeight);
    value.capsuleRadius=positiveFinite(value.capsuleRadius,.28f,0.0f);
    value.walkSpeed=positiveFinite(value.walkSpeed,2.8f,0.0f);
    value.sprintSpeed=std::max(value.walkSpeed,
        positiveFinite(value.sprintSpeed,5.5f,0.0f));
    value.minimumGroundNormalY=std::clamp(
        std::isfinite(value.minimumGroundNormalY)?value.minimumGroundNormalY:.819152f,
        0.0f,1.0f);
    value.maximumStepHeight=positiveFinite(value.maximumStepHeight,.35f,0.0f);
    value.maximumSweepDistance=positiveFinite(value.maximumSweepDistance,.18f,.001f);
    value.fixedTimeStep=positiveFinite(value.fixedTimeStep,1.0f/120.0f,.0001f);
    value.maximumFrameDelta=std::max(value.fixedTimeStep,
        positiveFinite(value.maximumFrameDelta,.10f,.0001f));
    value.maximumPhysicsSteps=std::max(1u,value.maximumPhysicsSteps);
    value.accelerationResponse=positiveFinite(value.accelerationResponse,12.0f,0.0f);
    value.brakingResponse=positiveFinite(value.brakingResponse,18.0f,0.0f);
    value.heightHalfLife=positiveFinite(value.heightHalfLife,.075f,.0001f);
    value.horizontalHalfExtent=positiveFinite(
        value.horizontalHalfExtent,EnvironmentGenerator::traversalHalfExtent,
                                               value.capsuleRadius+.01f);
    return value;
}

bool finite(Vec3 value) {
    return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);
}

} // namespace

FirstPersonCameraController::FirstPersonCameraController(
    FirstPersonCameraSettings settings,TerrainSurfaceSampler terrainSampler)
    :settings_(sanitize(settings)),terrainSampler_(std::move(terrainSampler)) {
    if(!terrainSampler_)terrainSampler_=[](float x,float z) {
        return EnvironmentGenerator::sampleTerrainSurface(x,z);
    };
    resetToDefaultDrop();
}

float FirstPersonCameraController::movementLimit() const {
    return std::max(0.0f,settings_.horizontalHalfExtent-settings_.capsuleRadius);
}

FirstPersonCameraController::GroundContact
FirstPersonCameraController::groundContact(float x,float z) const {
    const float radius=settings_.capsuleRadius;
    const std::array<Vec3,5> offsets{{{0,0,0},{radius,0,0},{-radius,0,0},
                                      {0,0,radius},{0,0,-radius}}};
    GroundContact contact;
    contact.supportHeight=-std::numeric_limits<float>::infinity();
    contact.minimumNormalY=1.0f;contact.valid=true;
    for(const Vec3 offset:offsets) {
        const TerrainSurfaceSample sample=terrainSampler_(x+offset.x,z+offset.z);
        if(!sample.insideBounds||!finite(sample.position)||!finite(sample.normal)||
           lengthSq(sample.normal)<1.0e-8f) {
            contact.valid=false;return contact;
        }
        const Vec3 normal=normalize(sample.normal);
        contact.supportHeight=std::max(contact.supportHeight,sample.position.y);
        contact.minimumNormalY=std::min(contact.minimumNormalY,normal.y);
    }
    contact.valid=std::isfinite(contact.supportHeight)&&
                  std::isfinite(contact.minimumNormalY);
    return contact;
}

void FirstPersonCameraController::resetToDefaultDrop() {
    reset(defaultDropX,defaultDropZ,defaultDropYaw,defaultDropPitch);
}

void FirstPersonCameraController::reset(float x,float z,float yaw,float pitch) {
    const float limit=movementLimit();
    x=std::clamp(std::isfinite(x)?x:0.0f,-limit,limit);
    z=std::clamp(std::isfinite(z)?z:0.0f,-limit,limit);
    state_={};state_.yaw=std::isfinite(yaw)?std::remainder(yaw,2*pi):0.0f;
    state_.pitch=clamp(std::isfinite(pitch)?pitch:0.0f,-1.45f,1.45f);
    const GroundContact contact=groundContact(x,z);
    state_.footPosition={x,contact.valid?contact.supportHeight:0.0f,z};
    state_.cameraEyeY=state_.footPosition.y+settings_.eyeHeight;
    state_.grounded=contact.valid;accumulator_=0;
}

void FirstPersonCameraController::setHorizontalHalfExtent(float halfExtent) {
    settings_.horizontalHalfExtent=positiveFinite(
        halfExtent,settings_.horizontalHalfExtent,settings_.capsuleRadius+.01f);
    const float oldX=state_.footPosition.x,oldZ=state_.footPosition.z;
    const float limit=movementLimit();
    state_.footPosition.x=std::clamp(oldX,-limit,limit);
    state_.footPosition.z=std::clamp(oldZ,-limit,limit);
    if(state_.footPosition.x!=oldX)state_.horizontalVelocity.x=0;
    if(state_.footPosition.z!=oldZ)state_.horizontalVelocity.z=0;
    const GroundContact contact=groundContact(state_.footPosition.x,state_.footPosition.z);
    if(contact.valid) {
        state_.footPosition.y=contact.supportHeight;state_.grounded=true;
        state_.cameraEyeY=std::max(state_.cameraEyeY,
            contact.supportHeight+settings_.minimumEyeClearance);
    } else state_.grounded=false;
}

bool FirstPersonCameraController::tryMove(float deltaX,float deltaZ) {
    const float limit=movementLimit();
    const float candidateX=std::clamp(state_.footPosition.x+deltaX,-limit,limit);
    const float candidateZ=std::clamp(state_.footPosition.z+deltaZ,-limit,limit);
    const GroundContact contact=groundContact(candidateX,candidateZ);
    if(!contact.valid||contact.minimumNormalY<settings_.minimumGroundNormalY||
       contact.supportHeight-state_.footPosition.y>settings_.maximumStepHeight)return false;
    state_.footPosition={candidateX,contact.supportHeight,candidateZ};
    state_.grounded=true;return true;
}

void FirstPersonCameraController::simulateStep(
    float deltaTime,const FirstPersonCameraInput& input) {
    const GroundContact current=groundContact(state_.footPosition.x,state_.footPosition.z);
    if(current.valid) {
        state_.footPosition.y=current.supportHeight;state_.grounded=true;
    } else {
        state_.grounded=false;state_.horizontalVelocity={};return;
    }

    const float forwardAxis=(input.forward?1.0f:0.0f)-(input.backward?1.0f:0.0f);
    const float rightAxis=(input.right?1.0f:0.0f)-(input.left?1.0f:0.0f);
    const Vec3 forward{std::sin(state_.yaw),0,std::cos(state_.yaw)};
    const Vec3 right{std::cos(state_.yaw),0,-std::sin(state_.yaw)};
    Vec3 wish=forward*forwardAxis+right*rightAxis;
    const float wishLength=length(wish);
    if(wishLength>1.0f)wish=wish/wishLength;
    const float speed=input.sprint?settings_.sprintSpeed:settings_.walkSpeed;
    const Vec3 targetVelocity=wish*speed;
    const float response=wishLength>1.0e-5f?settings_.accelerationResponse:
                                                settings_.brakingResponse;
    const float velocityBlend=1-std::exp(-response*deltaTime);
    state_.horizontalVelocity=lerp(state_.horizontalVelocity,targetVelocity,velocityBlend);
    state_.horizontalVelocity.y=0;

    const Vec3 displacement=state_.horizontalVelocity*deltaTime;
    const int sweepSteps=std::max(1,static_cast<int>(std::ceil(
        length(displacement)/settings_.maximumSweepDistance)));
    const float stepX=displacement.x/sweepSteps,stepZ=displacement.z/sweepSteps;
    for(int sweep=0;sweep<sweepSteps;++sweep) {
        const float oldX=state_.footPosition.x,oldZ=state_.footPosition.z;
        if(!tryMove(stepX,stepZ)) {
            const bool movedX=std::abs(stepX)<=1.0e-8f||tryMove(stepX,0);
            const bool movedZ=std::abs(stepZ)<=1.0e-8f||tryMove(0,stepZ);
            if(!movedX)state_.horizontalVelocity.x=0;
            if(!movedZ)state_.horizontalVelocity.z=0;
        }
        if(std::abs(stepX)>1.0e-8f&&
           std::abs(state_.footPosition.x-oldX)<std::abs(stepX)*.25f)
            state_.horizontalVelocity.x=0;
        if(std::abs(stepZ)>1.0e-8f&&
           std::abs(state_.footPosition.z-oldZ)<std::abs(stepZ)*.25f)
            state_.horizontalVelocity.z=0;
    }

    const float targetEyeY=state_.footPosition.y+settings_.eyeHeight;
    const float heightBlend=1-std::exp2(-deltaTime/settings_.heightHalfLife);
    state_.cameraEyeY+= (targetEyeY-state_.cameraEyeY)*heightBlend;
    state_.cameraEyeY=std::max(state_.cameraEyeY,
        state_.footPosition.y+settings_.minimumEyeClearance);
}

void FirstPersonCameraController::update(
    float elapsedSeconds,const FirstPersonCameraInput& input) {
    if(std::isfinite(input.yawDelta))
        state_.yaw=std::remainder(state_.yaw+input.yawDelta,2*pi);
    if(std::isfinite(input.pitchDelta))
        state_.pitch=clamp(state_.pitch+input.pitchDelta,-1.45f,1.45f);
    if(!std::isfinite(elapsedSeconds)||elapsedSeconds<=0)return;

    const float maximumSimulated=settings_.fixedTimeStep*settings_.maximumPhysicsSteps;
    accumulator_=std::min(maximumSimulated,accumulator_+
        std::min(elapsedSeconds,settings_.maximumFrameDelta));
    uint32_t steps=0;
    while(accumulator_+1.0e-7f>=settings_.fixedTimeStep&&
          steps<settings_.maximumPhysicsSteps) {
        simulateStep(settings_.fixedTimeStep,input);
        accumulator_=std::max(0.0f,accumulator_-settings_.fixedTimeStep);++steps;
    }
}

FirstPersonCameraPose FirstPersonCameraController::pose() const {
    const float cosinePitch=std::cos(state_.pitch);
    const Vec3 forward=normalize({std::sin(state_.yaw)*cosinePitch,
                                  -std::sin(state_.pitch),
                                  std::cos(state_.yaw)*cosinePitch});
    const Vec3 right=normalize({std::cos(state_.yaw),0,-std::sin(state_.yaw)});
    return {{state_.footPosition.x,state_.cameraEyeY,state_.footPosition.z},
            forward,right,normalize(cross(forward,right))};
}

} // namespace dense
