#include "environment_simulation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dense {
namespace {

constexpr float pi = 3.14159265358979323846f;

float finiteOr(float value,float fallback) {
    return std::isfinite(value)?value:fallback;
}

float saturate(float value) {
    return std::clamp(value,0.0f,1.0f);
}

float smoothStep(float low,float high,float value) {
    if(!(high>low))return value>=high?1.0f:0.0f;
    const float t=saturate((value-low)/(high-low));
    return t*t*(3.0f-2.0f*t);
}

float wrapHours(float hours) {
    if(!std::isfinite(hours))return 12.0f;
    hours=std::fmod(hours,24.0f);
    return hours<0.0f?hours+24.0f:hours;
}

EnvironmentFloat2 normalizedWind(EnvironmentFloat2 direction) {
    direction.x=finiteOr(direction.x,1.0f);
    direction.y=finiteOr(direction.y,0.0f);
    const float squared=direction.x*direction.x+direction.y*direction.y;
    if(!(squared>1.0e-8f)||!std::isfinite(squared))return {1.0f,0.0f};
    const float inverseLength=1.0f/std::sqrt(squared);
    return {direction.x*inverseLength,direction.y*inverseLength};
}

EnvironmentFloat3 normalized(EnvironmentFloat3 direction) {
    const float squared=direction.x*direction.x+direction.y*direction.y+
                        direction.z*direction.z;
    if(!(squared>1.0e-8f)||!std::isfinite(squared))return {0.0f,1.0f,0.0f};
    const float inverseLength=1.0f/std::sqrt(squared);
    return {direction.x*inverseLength,direction.y*inverseLength,
            direction.z*inverseLength};
}

EnvironmentFloat3 lerp(EnvironmentFloat3 low,EnvironmentFloat3 high,float t) {
    return {low.x+(high.x-low.x)*t,
            low.y+(high.y-low.y)*t,
            low.z+(high.z-low.z)*t};
}

float exactWetnessStep(float wetness,float rain,float accumulationRate,
                       float dryingRate,float deltaTime) {
    const float wetRate=rain*std::clamp(accumulationRate,0.0f,10.0f);
    const float dryRate=(1.0f-rain)*std::clamp(dryingRate,0.0f,10.0f);
    const float combinedRate=wetRate+dryRate;
    if(!(combinedRate>0.0f)||!(deltaTime>0.0f))return saturate(wetness);
    const float equilibrium=wetRate/combinedRate;
    return saturate(equilibrium+(saturate(wetness)-equilibrium)*
                     std::exp(-combinedRate*deltaTime));
}

} // namespace

EnvironmentSimulation::EnvironmentSimulation() {
    update(0.0f);
}

EnvironmentSimulation::EnvironmentSimulation(EnvironmentControls initialControls)
    : controls(std::move(initialControls)) {
    update(0.0f);
}

void EnvironmentSimulation::reset(EnvironmentState initialState) {
    state=initialState;
    update(0.0f);
}

void EnvironmentSimulation::triggerLightning() {
    triggerLightning(controls.lightningPeak);
}

void EnvironmentSimulation::triggerLightning(float peak) {
    const float safePeak=std::clamp(finiteOr(peak,0.0f),0.0f,64.0f);
    state.lightningFlash=std::max(
        std::clamp(finiteOr(state.lightningFlash,0.0f),0.0f,64.0f),safePeak);
}

const EnvironmentCB& EnvironmentSimulation::update(float deltaTimeSeconds) {
    const float deltaTime=std::max(0.0f,finiteOr(deltaTimeSeconds,0.0f));

    if(!std::isfinite(state.totalTimeSeconds)||state.totalTimeSeconds<0.0)
        state.totalTimeSeconds=0.0;
    state.totalTimeSeconds+=static_cast<double>(deltaTime);

    state.timeOfDay=wrapHours(state.timeOfDay);
    if(controls.advanceTime&&deltaTime>0.0f) {
        const float dayLength=std::max(1.0f,
            finiteOr(controls.dayLengthSeconds,600.0f));
        const float timeScale=std::clamp(
            finiteOr(controls.timeScale,1.0f),-100.0f,100.0f);
        state.timeOfDay=wrapHours(state.timeOfDay+
            deltaTime*(24.0f/dayLength)*timeScale);
    }

    const float rain=saturate(finiteOr(controls.rainIntensity,0.0f));
    state.wetnessFactor=exactWetnessStep(
        finiteOr(state.wetnessFactor,0.0f),rain,
        finiteOr(controls.wetnessAccumulationRate,0.035f),
        finiteOr(controls.wetnessDryingRate,0.006f),deltaTime);

    const float puddleStart=std::clamp(
        finiteOr(controls.puddleStartWetness,0.32f),0.0f,0.99f);
    const float puddleCapacity=saturate(
        finiteOr(controls.maximumPuddleCoverage,0.85f));
    state.puddleCoverage=puddleCapacity*smoothStep(
        puddleStart,1.0f,state.wetnessFactor);

    const float stormThreshold=std::clamp(
        finiteOr(controls.stormRainThreshold,0.55f),0.0f,0.99f);
    state.stormIntensity=smoothStep(stormThreshold,1.0f,rain);

    const float lightningDecay=std::clamp(
        finiteOr(controls.lightningDecayRate,6.0f),0.0f,100.0f);
    state.lightningFlash=std::clamp(
        finiteOr(state.lightningFlash,0.0f),0.0f,64.0f)*
        std::exp(-lightningDecay*deltaTime);
    if(state.lightningFlash<1.0e-5f)state.lightningFlash=0.0f;

    const float solarPhase=(state.timeOfDay-6.0f)*(pi/12.0f);
    const float sinElevation=std::sin(solarPhase);
    const float cosElevation=std::cos(solarPhase);
    const float azimuth=finiteOr(controls.sunAzimuthRadians,0.55f);
    const EnvironmentFloat3 sunDirection=normalized({
        std::cos(azimuth)*cosElevation,
        sinElevation,
        std::sin(azimuth)*cosElevation});
    const float sunElevation=std::asin(std::clamp(sunDirection.y,-1.0f,1.0f));

    const float daylight=smoothStep(-0.08f,0.10f,sunDirection.y);
    const float elevationEnergy=0.12f+0.88f*
        std::sqrt(std::max(0.0f,sunDirection.y));
    const float stormSunAttenuation=saturate(
        finiteOr(controls.stormSunAttenuation,0.72f));
    const float sunIntensity=std::clamp(
        finiteOr(controls.sunIntensityScale,1.0f),0.0f,32.0f)*daylight*elevationEnergy*
        (1.0f-state.stormIntensity*stormSunAttenuation);
    const float middayBlend=smoothStep(0.02f,0.55f,sunDirection.y);
    const EnvironmentFloat3 sunColor=lerp(
        {1.0f,0.30f,0.05f},{1.0f,0.95f,0.85f},middayBlend);

    const float moonPhase=saturate(finiteOr(controls.moonPhase,0.75f));
    const EnvironmentFloat3 moonDirection={-sunDirection.x,-sunDirection.y,
                                            -sunDirection.z};
    // Moonlight begins only once the sun is visibly below the horizon. This
    // keeps sunset directional shadows from flipping to the opposite sky at
    // the geometric horizon while preserving a smooth astronomical twilight.
    const float night=saturate(1.0f-smoothStep(-0.16f,-0.05f,sunDirection.y));
    const float stormMoonAttenuation=saturate(
        finiteOr(controls.stormMoonAttenuation,0.82f));
    const float moonIntensity=std::clamp(
        finiteOr(controls.moonIntensityScale,0.35f),0.0f,8.0f)*moonPhase*night*
        (1.0f-state.stormIntensity*stormMoonAttenuation);

    const EnvironmentFloat2 windDirection=normalizedWind(controls.windDirection);
    const float stormWindBoost=std::clamp(
        finiteOr(controls.stormWindBoost,0.65f),0.0f,4.0f);
    const float windMultiplier=1.0f+state.stormIntensity*stormWindBoost;
    const float windSpeed=std::clamp(finiteOr(controls.windSpeed,1.0f),0.0f,15.0f)*
                          windMultiplier;
    const float windStrength=std::clamp(
        std::max(0.0f,finiteOr(controls.windStrength,0.72f))*windMultiplier,
        0.0f,3.0f);
    const float gustFrequency=std::clamp(
        finiteOr(controls.windGustFrequency,1.36f),0.0f,30.0f);

    const float baseFog=std::clamp(
        finiteOr(controls.baseFogDensity,0.00032f),0.0f,1.0f);
    const float stormFogBoost=std::clamp(
        finiteOr(controls.stormFogDensityBoost,1.20f),0.0f,16.0f);
    const float wetFogBoost=std::clamp(
        finiteOr(controls.wetFogDensityBoost,0.18f),0.0f,16.0f);
    state.fogDensity=baseFog*(1.0f+state.stormIntensity*stormFogBoost+
                             state.wetnessFactor*wetFogBoost);
    state.starVisibility=night*(1.0f-0.92f*state.stormIntensity)*
                         (1.0f-0.25f*moonPhase);

    constants_.time=static_cast<float>(state.totalTimeSeconds);
    constants_.deltaTime=deltaTime;
    constants_.timeOfDay=state.timeOfDay;
    constants_.sunElevation=sunElevation;
    constants_.sunDirection=sunDirection;
    constants_.sunIntensity=sunIntensity;
    constants_.sunColor=sunColor;
    constants_.moonPhase=moonPhase;
    constants_.moonDirection=moonDirection;
    constants_.moonIntensity=moonIntensity;
    constants_.moonColor={0.08f,0.12f,0.25f};
    constants_.windSpeed=windSpeed;
    constants_.windDirection=windDirection;
    constants_.windStrength=windStrength;
    constants_.windGustFrequency=gustFrequency;
    constants_.rainIntensity=rain;
    constants_.wetnessFactor=state.wetnessFactor;
    constants_.puddleCoverage=state.puddleCoverage;
    constants_.lightningFlash=state.lightningFlash;
    constants_.fogDensity=state.fogDensity;
    constants_.fogHeightFalloff=std::clamp(
        finiteOr(controls.fogHeightFalloff,0.020f),0.0f,4.0f);
    constants_.stormIntensity=state.stormIntensity;
    constants_.starVisibility=state.starVisibility;
    return constants_;
}

} // namespace dense
