#include "environment_simulation.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(bool condition,std::string_view message) {
    if(!condition)fail(message);
}

bool near(float a,float b,float tolerance=1.0e-5f) {
    return std::abs(a-b)<=tolerance;
}

float length(dense::EnvironmentFloat3 value) {
    return std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
}

void requireFinite(const dense::EnvironmentCB& constants) {
    const auto values=std::bit_cast<std::array<float,sizeof(constants)/sizeof(float)>>(constants);
    for(float value:values)
        require(std::isfinite(value),"environment constants contain a non-finite value");
}

} // namespace

int main() {
    dense::EnvironmentSimulation simulation;
    simulation.controls.advanceTime=false;
    simulation.controls.rainIntensity=0.0f;

    simulation.state.timeOfDay=12.0f;
    const auto noon=simulation.update(0.0f);
    require(near(noon.sunElevation,1.57079632679f,2.0e-5f),
            "midday sun did not reach zenith");
    require(near(length(noon.sunDirection),1.0f)&&noon.sunDirection.y>.999f,
            "midday sun direction is not normalized and upward");
    require(noon.sunIntensity>.99f&&noon.moonIntensity<1.0e-5f&&
                noon.starVisibility<1.0e-5f,
            "midday light did not suppress moon and stars");
    require(near(noon.sunColor.x,1.0f)&&near(noon.sunColor.y,.95f)&&
                near(noon.sunColor.z,.85f),
            "midday sun colour missed its target");
    require(near(noon.waterTableHeight,-4.05f)&&noon.floodCoverage==0.0f&&
                noon.waterRippleStrength==0.0f&&noon.environmentPadding==0.0f,
            "dry default hydrology did not remain below the rendered pasture");

    simulation.state.timeOfDay=6.0f;
    const auto dawn=simulation.update(0.0f);
    require(std::abs(dawn.sunElevation)<2.0e-5f&&dawn.sunIntensity>0.0f&&
                dawn.sunIntensity<.15f,
            "sunrise transition is discontinuous or implausibly bright");
    require(near(dawn.sunColor.x,1.0f)&&near(dawn.sunColor.y,.30f)&&
                near(dawn.sunColor.z,.05f),
            "horizon sun colour missed its warm target");

    simulation.state.timeOfDay=18.0f;
    require(simulation.update(0.0f).moonIntensity<1.0e-6f,
            "moonlight activated before the sun crossed below the horizon");
    simulation.state.timeOfDay=18.25f;
    require(simulation.update(0.0f).moonIntensity>0.0f,
            "moonlight did not enter smoothly during astronomical twilight");

    simulation.state.timeOfDay=0.0f;
    const auto midnight=simulation.update(0.0f);
    require(midnight.sunDirection.y<-.999f&&midnight.sunIntensity<1.0e-6f,
            "midnight sun remained above the horizon");
    require(midnight.moonIntensity>.20f&&midnight.starVisibility>.50f,
            "night did not enable moonlight and stars");
    require(near(midnight.moonDirection.x,-midnight.sunDirection.x)&&
                near(midnight.moonDirection.y,-midnight.sunDirection.y)&&
                near(midnight.moonDirection.z,-midnight.sunDirection.z),
            "moon direction is not opposite the sun");

    simulation.controls.moonPhase=0.0f;
    require(simulation.update(0.0f).moonIntensity==0.0f,
            "new moon still emits directional moonlight");

    dense::EnvironmentSimulation clock;
    clock.controls.advanceTime=true;
    clock.controls.dayLengthSeconds=24.0f;
    clock.controls.timeScale=1.0f;
    clock.state.timeOfDay=23.75f;
    clock.update(.50f);
    require(near(clock.state.timeOfDay,.25f),"time-of-day did not wrap at 24 hours");

    dense::EnvironmentSimulation weather;
    weather.controls.advanceTime=false;
    weather.controls.rainIntensity=1.0f;
    weather.controls.wetnessAccumulationRate=.5f;
    weather.controls.wetnessDryingRate=.1f;
    weather.controls.maximumPuddleCoverage=.8f;
    weather.controls.puddleStartWetness=.3f;
    weather.state.wetnessFactor=0.0f;
    weather.update(2.0f);
    require(near(weather.state.wetnessFactor,1.0f-std::exp(-1.0f),2.0e-6f),
            "wetness accumulation does not follow its exact exponential response");
    require(weather.state.waterTableLevel>0.0f&&
                weather.state.waterTableLevel<weather.state.wetnessFactor&&
                weather.state.puddleCoverage==0.0f&&
                weather.state.floodCoverage==0.0f&&
                near(weather.state.waterTableHeight,-4.05f)&&
                weather.state.waterRippleStrength==0.0f,
            "a short shower bypassed the slower global water-table response");
    weather.update(60.0f);
    require(weather.state.waterTableLevel>.5f&&
                weather.state.waterTableLevel<weather.state.wetnessFactor&&
                weather.state.puddleCoverage>.4f&&weather.state.puddleCoverage<.8f&&
                weather.state.floodCoverage>.5f&&weather.state.floodCoverage<1.0f&&
                weather.state.waterTableHeight>-4.05f&&
                weather.state.waterTableHeight<-.55f&&
                near(weather.state.waterRippleStrength,
                     weather.state.floodCoverage),
            "sustained rain did not raise the water table and fill eligible basins");
    const float rainWetness=weather.state.wetnessFactor;
    const float rainWaterTable=weather.state.waterTableLevel;
    const float rainPuddles=weather.state.puddleCoverage;
    weather.controls.rainIntensity=0.0f;
    weather.update(2.0f);
    require(near(weather.state.wetnessFactor,rainWetness*std::exp(-.2f),2.0e-6f),
            "wet surfaces did not dry exponentially after rain stopped");
    require(weather.state.waterTableLevel>=rainWaterTable&&
                weather.state.puddleCoverage>=rainPuddles,
            "saturated soil stopped recharging the table immediately with the rain");
    weather.update(1200.0f);
    require(weather.state.waterTableLevel<.1f&&weather.state.puddleCoverage==0.0f&&
                weather.state.floodCoverage==0.0f&&
                near(weather.state.waterTableHeight,-4.05f),
            "the global water table did not drain after prolonged dry weather");

    dense::EnvironmentSimulation fullFlood;
    fullFlood.controls.advanceTime=false;
    fullFlood.controls.rainIntensity=1.0f;
    fullFlood.state.wetnessFactor=1.0f;
    fullFlood.state.waterTableLevel=1.0f;
    const auto floodedFrame=fullFlood.update(0.0f);
    require(near(floodedFrame.waterTableHeight,-.55f)&&
                floodedFrame.waterTableHeight<0.0f&&
                near(floodedFrame.floodCoverage,1.0f)&&
                near(floodedFrame.waterRippleStrength,1.0f)&&
                near(floodedFrame.puddleCoverage,
                     fullFlood.controls.maximumPuddleCoverage),
            "full saturation did not produce a root-safe contiguous flood level");

    dense::EnvironmentSimulation oneStep;
    dense::EnvironmentSimulation tenSteps;
    oneStep.controls.advanceTime=false;
    tenSteps.controls.advanceTime=false;
    oneStep.controls.rainIntensity=.65f;
    tenSteps.controls.rainIntensity=.65f;
    oneStep.state.wetnessFactor=.27f;
    tenSteps.state.wetnessFactor=.27f;
    oneStep.state.waterTableLevel=.45f;
    tenSteps.state.waterTableLevel=.45f;
    oneStep.triggerLightning(8.0f);
    tenSteps.triggerLightning(8.0f);
    oneStep.update(1.0f);
    for(int i=0;i<10;++i)tenSteps.update(.1f);
    require(near(oneStep.state.wetnessFactor,tenSteps.state.wetnessFactor,2.0e-6f)&&
                near(oneStep.state.waterTableLevel,
                     tenSteps.state.waterTableLevel,2.0e-6f)&&
                near(oneStep.state.puddleCoverage,
                     tenSteps.state.puddleCoverage,2.0e-6f)&&
                near(oneStep.state.lightningFlash,tenSteps.state.lightningFlash,2.0e-6f),
            "weather integration depends on frame partitioning");
    require(near(oneStep.state.lightningFlash,8.0f*std::exp(-6.0f),2.0e-6f),
            "lightning did not decay to near zero over one second");

    dense::EnvironmentSimulation negativeDelta;
    negativeDelta.controls.advanceTime=false;
    negativeDelta.controls.rainIntensity=.7f;
    negativeDelta.state.timeOfDay=9.0f;
    negativeDelta.state.wetnessFactor=.4f;
    negativeDelta.state.waterTableLevel=.35f;
    negativeDelta.triggerLightning();
    const auto beforeNegative=negativeDelta.update(0.0f);
    const auto afterNegative=negativeDelta.update(-10.0f);
    require(near(afterNegative.time,beforeNegative.time)&&
                near(afterNegative.timeOfDay,beforeNegative.timeOfDay)&&
                near(afterNegative.wetnessFactor,beforeNegative.wetnessFactor)&&
                near(negativeDelta.state.waterTableLevel,.35f)&&
                near(afterNegative.puddleCoverage,beforeNegative.puddleCoverage)&&
                near(afterNegative.waterTableHeight,beforeNegative.waterTableHeight)&&
                near(afterNegative.floodCoverage,beforeNegative.floodCoverage)&&
                near(afterNegative.lightningFlash,beforeNegative.lightningFlash),
            "negative frame delta changed environmental state");

    dense::EnvironmentSimulation storm;
    storm.controls.advanceTime=false;
    storm.controls.rainIntensity=1.0f;
    storm.controls.windDirection={0.0f,0.0f};
    storm.controls.windStrength=1.0f;
    storm.state.timeOfDay=0.0f;
    const auto stormFrame=storm.update(0.0f);
    require(near(stormFrame.stormIntensity,1.0f)&&
                stormFrame.fogDensity>storm.controls.baseFogDensity,
            "heavy rain did not produce a unified storm/fog response");
    require(stormFrame.windStrength>storm.controls.windStrength&&
                near(stormFrame.windDirection.x,1.0f)&&
                near(stormFrame.windDirection.y,0.0f),
            "storm wind boost or zero-direction fallback is invalid");
    require(stormFrame.starVisibility<.10f,
            "heavy storm did not suppress the night sky");

    storm.controls.windDirection={std::numeric_limits<float>::max(),
                                  std::numeric_limits<float>::max()};
    const auto overflowWind=storm.update(0.0f);
    require(near(overflowWind.windDirection.x,1.0f)&&
                near(overflowWind.windDirection.y,0.0f),
            "overflowing wind input did not use the finite fallback");

    dense::EnvironmentSimulation deterministicA;
    dense::EnvironmentSimulation deterministicB;
    deterministicA.controls.rainIntensity=.78f;
    deterministicB.controls.rainIntensity=.78f;
    deterministicA.triggerLightning();
    deterministicB.triggerLightning();
    for(float delta:{.016f,.021f,.007f,.125f,.033f}) {
        deterministicA.update(delta);
        deterministicB.update(delta);
    }
    require(std::memcmp(&deterministicA.constants(),&deterministicB.constants(),
                        sizeof(dense::EnvironmentCB))==0,
            "identical environment inputs did not produce identical constants");
    require(near(deterministicA.state.waterTableLevel,
                 deterministicB.state.waterTableLevel),
            "identical weather inputs did not reproduce the CPU water table");
    requireFinite(deterministicA.constants());

    dense::EnvironmentSimulation sanitizedHydrology;
    sanitizedHydrology.controls.advanceTime=false;
    sanitizedHydrology.controls.rainIntensity=1.0f;
    sanitizedHydrology.controls.waterTableRiseRate=
        std::numeric_limits<float>::infinity();
    sanitizedHydrology.controls.waterTableDrainRate=
        std::numeric_limits<float>::quiet_NaN();
    sanitizedHydrology.controls.waterTableDryHeight=
        -std::numeric_limits<float>::infinity();
    sanitizedHydrology.controls.waterTableFloodHeight=
        std::numeric_limits<float>::quiet_NaN();
    sanitizedHydrology.state.waterTableLevel=
        std::numeric_limits<float>::quiet_NaN();
    const auto sanitizedFrame=sanitizedHydrology.update(1.0f);
    require(std::isfinite(sanitizedHydrology.state.waterTableLevel)&&
                sanitizedHydrology.state.waterTableLevel>=0.0f&&
                sanitizedHydrology.state.waterTableLevel<=1.0f&&
                std::isfinite(sanitizedFrame.puddleCoverage)&&
                std::isfinite(sanitizedFrame.waterTableHeight)&&
                std::isfinite(sanitizedFrame.floodCoverage)&&
                std::isfinite(sanitizedFrame.waterRippleStrength)&&
                sanitizedFrame.environmentPadding==0.0f,
            "invalid hydrology controls escaped normalization");

    std::cout << "Environment simulation tests passed\n";
    return EXIT_SUCCESS;
}
