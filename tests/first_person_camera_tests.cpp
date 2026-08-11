#include "first_person_camera.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(bool condition,std::string_view message) {
    if(!condition)fail(message);
}

bool near(float a,float b,float tolerance=1.0e-4f) {
    return std::abs(a-b)<=tolerance;
}

dense::TerrainSurfaceSample flatTerrain(float x,float z) {
    return {{x,0,z},{0,1,0},std::abs(x)<=1000&&std::abs(z)<=1000};
}

void advance(dense::FirstPersonCameraController& controller,
             const dense::FirstPersonCameraInput& input,float seconds,
             float frameStep=1.0f/120.0f) {
    const int frames=static_cast<int>(std::lround(seconds/frameStep));
    for(int frame=0;frame<frames;++frame)controller.update(frameStep,input);
}

float horizontalDistance(const dense::FirstPersonCameraState& state) {
    return std::sqrt(state.footPosition.x*state.footPosition.x+
                     state.footPosition.z*state.footPosition.z);
}

} // namespace

int main() {
    dense::FirstPersonCameraController standing({},flatTerrain);
    standing.reset(0,0,0,0);
    const auto initialPose=standing.pose();
    require(standing.state().grounded&&near(standing.state().footPosition.y,0),
            "first-person controller did not acquire flat ground");
    require(near(initialPose.eye.y,1.68f)&&near(initialPose.forward.x,0)&&
                near(initialPose.forward.y,0)&&near(initialPose.forward.z,1),
            "default eye height or level forward direction is wrong");
    require(near(dense::length(initialPose.forward),1)&&near(dense::length(initialPose.right),1)&&
                near(dense::length(initialPose.up),1)&&
                std::abs(dense::dot(initialPose.forward,initialPose.right))<1.0e-5f,
            "camera pose basis is not orthonormal");

    dense::FirstPersonCameraInput look;
    look.yawDelta=dense::pi*.5f;look.pitchDelta=.25f;
    standing.update(0,look);
    const auto turned=standing.pose();
    require(turned.forward.x>.96f&&turned.forward.y<-.24f&&
                std::abs(turned.forward.z)<1.0e-4f,
            "yaw/pitch look input did not produce the expected forward vector");
    look={};look.pitchDelta=10;
    standing.update(0,look);
    require(near(standing.state().pitch,1.45f),"first-person pitch escaped its safe limit");

    dense::FirstPersonCameraInput forwardInput;forwardInput.forward=true;
    dense::FirstPersonCameraController level({},flatTerrain),pitched({},flatTerrain);
    level.reset(0,0,0,0);pitched.reset(0,0,0,1.2f);
    advance(level,forwardInput,2.0f);advance(pitched,forwardInput,2.0f);
    require(level.state().footPosition.z>4.8f&&
                near(level.state().footPosition.x,pitched.state().footPosition.x)&&
                near(level.state().footPosition.z,pitched.state().footPosition.z),
            "ground movement depends on view pitch or failed to advance");

    dense::FirstPersonCameraInput diagonalInput=forwardInput;diagonalInput.right=true;
    dense::FirstPersonCameraController straight({},flatTerrain),diagonal({},flatTerrain);
    straight.reset(0,0,0,0);diagonal.reset(0,0,0,0);
    advance(straight,forwardInput,2.0f);advance(diagonal,diagonalInput,2.0f);
    require(near(horizontalDistance(straight.state()),horizontalDistance(diagonal.state()),2.0e-3f),
            "diagonal WASD input moves faster than straight input");

    dense::FirstPersonCameraInput sprintInput=forwardInput;sprintInput.sprint=true;
    dense::FirstPersonCameraController walker({},flatTerrain),sprinter({},flatTerrain);
    walker.reset(0,0,0,0);sprinter.reset(0,0,0,0);
    advance(walker,forwardInput,2.0f);advance(sprinter,sprintInput,2.0f);
    require(horizontalDistance(sprinter.state())>horizontalDistance(walker.state())*1.8f,
            "sprint did not materially exceed walking speed");

    dense::FirstPersonCameraController frames120({},flatTerrain),frames60({},flatTerrain);
    frames120.reset(0,0,0,0);frames60.reset(0,0,0,0);
    advance(frames120,forwardInput,1.0f,1.0f/120.0f);
    advance(frames60,forwardInput,1.0f,1.0f/60.0f);
    require(near(frames120.state().footPosition.z,frames60.state().footPosition.z,2.0e-4f)&&
                near(frames120.state().horizontalVelocity.z,
                     frames60.state().horizontalVelocity.z,2.0e-4f),
            "fixed camera physics depends on render-frame partitioning");

    dense::FirstPersonCameraController longStall({},flatTerrain),clampedFrame({},flatTerrain);
    longStall.reset(0,0,0,0);clampedFrame.reset(0,0,0,0);
    longStall.update(5.0f,forwardInput);clampedFrame.update(.10f,forwardInput);
    require(near(longStall.state().footPosition.z,clampedFrame.state().footPosition.z,2.0e-4f),
            "long render stall bypassed the bounded fixed-step budget");

    dense::FirstPersonCameraSettings boundedSettings;
    boundedSettings.horizontalHalfExtent=2.0f;
    dense::FirstPersonCameraController bounded(boundedSettings,flatTerrain);
    bounded.reset(0,0,0,0);
    dense::FirstPersonCameraInput rightInput;rightInput.right=true;rightInput.sprint=true;
    advance(bounded,rightInput,5.0f);
    require(bounded.state().footPosition.x<=2.0f-boundedSettings.capsuleRadius+1.0e-4f&&
                near(bounded.state().footPosition.z,0),
            "capsule centre escaped its configured horizontal bounds");

    // Product defaults must follow the full authored world rather than the
    // former 170 m grass test field, while preserving an explicit terrain-edge
    // margin for the capsule's support samples.
    dense::FirstPersonCameraController mapTraversal;
    require(near(mapTraversal.settings().horizontalHalfExtent,
                 dense::EnvironmentGenerator::traversalHalfExtent),
            "first-person default is not wired to the authored map extent");
    mapTraversal.reset(220.0f,0,0,0);
    require(near(mapTraversal.state().footPosition.x,220.0f),
            "first-person traversal is still clamped to the former 170 m field");
    mapTraversal.reset(dense::EnvironmentGenerator::terrainHalfExtent,0,0,0);
    const float maximumCentre=dense::EnvironmentGenerator::traversalHalfExtent-
                              mapTraversal.settings().capsuleRadius;
    require(mapTraversal.state().footPosition.x<=maximumCentre+1.0e-4f&&
                mapTraversal.state().footPosition.x>=maximumCentre-1.0e-3f&&
                mapTraversal.state().footPosition.x<=
                    dense::EnvironmentGenerator::terrainHalfExtent-
                    dense::EnvironmentGenerator::traversalEdgeMargin,
            "first-person traversal did not preserve the terrain-edge margin");

    const dense::TerrainSurfaceSampler steepStrip=[](float x,float z) {
        const bool steep=x>.55f;
        const dense::Vec3 normal=steep?dense::normalize(dense::Vec3{-2,1,0}):
                                         dense::Vec3{0,1,0};
        return dense::TerrainSurfaceSample{{x,0,z},normal,true};
    };
    dense::FirstPersonCameraController sliding({},steepStrip);
    sliding.reset(0,0,0,0);advance(sliding,diagonalInput,2.0f);
    require(sliding.state().footPosition.x<.30f&&sliding.state().footPosition.z>3.0f,
            "steep-slope rejection failed to preserve tangential sliding");

    const dense::TerrainSurfaceSampler tallStep=[](float x,float z) {
        return dense::TerrainSurfaceSample{{x,x>.55f?.50f:0.0f,z},{0,1,0},true};
    };
    dense::FirstPersonCameraController blockedStep({},tallStep);
    blockedStep.reset(0,0,0,0);advance(blockedStep,rightInput,2.0f);
    require(blockedStep.state().footPosition.x<.30f&&near(blockedStep.state().footPosition.y,0),
            "controller climbed a step higher than its configured allowance");
    dense::FirstPersonCameraSettings highStepSettings;
    highStepSettings.maximumStepHeight=.60f;
    dense::FirstPersonCameraController allowedStep(highStepSettings,tallStep);
    allowedStep.reset(0,0,0,0);advance(allowedStep,rightInput,2.0f);
    require(allowedStep.state().footPosition.x>.8f&&near(allowedStep.state().footPosition.y,.5f),
            "controller rejected a step below its configured allowance");

    float movingGround=0;
    const dense::TerrainSurfaceSampler changingGround=[&](float x,float z) {
        return dense::TerrainSurfaceSample{{x,movingGround,z},{0,1,0},true};
    };
    dense::FirstPersonCameraController smoothed({},changingGround);
    smoothed.reset(0,0,0,0);movingGround=.30f;
    smoothed.update(1.0f/120.0f,{});
    require(near(smoothed.state().footPosition.y,.30f)&&
                smoothed.pose().eye.y>=1.80f&&smoothed.pose().eye.y<1.98f,
            "grounded camera did not smooth height while preserving clearance");
    advance(smoothed,{},1.0f);
    require(near(smoothed.pose().eye.y,1.98f,2.0e-4f),
            "camera eye did not converge to the 1.68 m grounded height");

    dense::FirstPersonCameraController productionDrop;
    const auto drop=productionDrop.pose();
    const dense::Vec3 toOak=dense::normalize(dense::Vec3{-drop.eye.x,0,-drop.eye.z});
    const dense::Vec3 facing=dense::normalize(dense::Vec3{drop.forward.x,0,drop.forward.z});
    require(productionDrop.state().grounded&&dense::dot(toOak,facing)>.999f&&
                std::abs(drop.eye.y-productionDrop.state().footPosition.y-1.68f)<1.0e-4f,
            "default first-person drop is not grounded and facing the oak");

    std::cout << "First-person camera tests passed\n";
    return EXIT_SUCCESS;
}
