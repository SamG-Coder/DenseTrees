#pragma once

#include <cstddef>
#include <type_traits>

namespace dense {

struct EnvironmentFloat2 {
    float x{};
    float y{};
};

struct EnvironmentFloat3 {
    float x{};
    float y{};
    float z{};
};

// Byte-for-byte CPU mirror of the future HLSL EnvironmentCB at register(b1).
// Registers c0-c7 preserve the original 128-byte environment contract.  The
// appended c8 register carries global hydrology without shifting any existing
// shader field. Light directions point from a shaded world position toward the
// emitter, so daytime sun Y is positive.
struct alignas(16) EnvironmentCB {
    // c0
    float time{};
    float deltaTime{};
    float timeOfDay{};
    float sunElevation{};

    // c1
    EnvironmentFloat3 sunDirection{};
    float sunIntensity{};

    // c2
    EnvironmentFloat3 sunColor{};
    float moonPhase{};

    // c3
    EnvironmentFloat3 moonDirection{};
    float moonIntensity{};

    // c4
    EnvironmentFloat3 moonColor{};
    float windSpeed{};

    // c5
    EnvironmentFloat2 windDirection{};
    float windStrength{};
    float windGustFrequency{};

    // c6
    float rainIntensity{};
    float wetnessFactor{};
    float puddleCoverage{};
    float lightningFlash{};

    // c7 - required extension for unified atmosphere evaluation
    float fogDensity{};
    float fogHeightFalloff{};
    float stormIntensity{};
    float starVisibility{};

    // c8 - global hydrology extension
    float waterTableHeight{};    // Absolute world-space Y.
    float floodCoverage{};       // Normalized global flood progression.
    float waterRippleStrength{}; // Rain-driven ripple contribution.
    float environmentPadding{};
};

static_assert(sizeof(EnvironmentFloat2) == 8);
static_assert(sizeof(EnvironmentFloat3) == 12);
static_assert(std::is_standard_layout_v<EnvironmentCB>);
static_assert(std::is_trivially_copyable_v<EnvironmentCB>);
static_assert(alignof(EnvironmentCB) == 16);
static_assert(sizeof(EnvironmentCB) == 144);

static_assert(offsetof(EnvironmentCB,time) == 0);
static_assert(offsetof(EnvironmentCB,deltaTime) == 4);
static_assert(offsetof(EnvironmentCB,timeOfDay) == 8);
static_assert(offsetof(EnvironmentCB,sunElevation) == 12);
static_assert(offsetof(EnvironmentCB,sunDirection) == 16);
static_assert(offsetof(EnvironmentCB,sunIntensity) == 28);
static_assert(offsetof(EnvironmentCB,sunColor) == 32);
static_assert(offsetof(EnvironmentCB,moonPhase) == 44);
static_assert(offsetof(EnvironmentCB,moonDirection) == 48);
static_assert(offsetof(EnvironmentCB,moonIntensity) == 60);
static_assert(offsetof(EnvironmentCB,moonColor) == 64);
static_assert(offsetof(EnvironmentCB,windSpeed) == 76);
static_assert(offsetof(EnvironmentCB,windDirection) == 80);
static_assert(offsetof(EnvironmentCB,windStrength) == 88);
static_assert(offsetof(EnvironmentCB,windGustFrequency) == 92);
static_assert(offsetof(EnvironmentCB,rainIntensity) == 96);
static_assert(offsetof(EnvironmentCB,wetnessFactor) == 100);
static_assert(offsetof(EnvironmentCB,puddleCoverage) == 104);
static_assert(offsetof(EnvironmentCB,lightningFlash) == 108);
static_assert(offsetof(EnvironmentCB,fogDensity) == 112);
static_assert(offsetof(EnvironmentCB,fogHeightFalloff) == 116);
static_assert(offsetof(EnvironmentCB,stormIntensity) == 120);
static_assert(offsetof(EnvironmentCB,starVisibility) == 124);
static_assert(offsetof(EnvironmentCB,waterTableHeight) == 128);
static_assert(offsetof(EnvironmentCB,floodCoverage) == 132);
static_assert(offsetof(EnvironmentCB,waterRippleStrength) == 136);
static_assert(offsetof(EnvironmentCB,environmentPadding) == 140);

struct EnvironmentControls {
    bool advanceTime{true};
    float dayLengthSeconds{600.0f};
    float timeScale{1.0f};

    // Orientation of the sun's sunrise-to-sunset plane around world Y.
    float sunAzimuthRadians{0.55f};
    float sunIntensityScale{1.0f};
    float moonPhase{0.75f};
    float moonIntensityScale{0.35f};

    EnvironmentFloat2 windDirection{0.82f,0.57f};
    float windSpeed{1.0f};
    float windStrength{0.72f};
    float windGustFrequency{1.36f};
    float stormWindBoost{0.65f};

    float rainIntensity{0.0f};
    float wetnessAccumulationRate{0.035f};
    float wetnessDryingRate{0.006f};
    // The global water table follows accumulated surface wetness much more
    // slowly than the material wetness response.  This prevents a short
    // shower from flooding every eligible basin while retaining water after
    // the rain has stopped.
    float waterTableRiseRate{0.018f};
    float waterTableDrainRate{0.0025f};
    // Scene-calibrated absolute world heights.  The dry value lies below the
    // sampled pasture minimum (-3.90 m); the flood value retains 0.55 m of
    // clearance beneath the oak's root-grade plateau at world Y=0.
    float waterTableDryHeight{-4.05f};
    float waterTableFloodHeight{-0.55f};
    float maximumPuddleCoverage{0.85f};
    // Applied to the filtered water-table saturation.  The historical name is
    // retained for source compatibility with existing controls and presets.
    float puddleStartWetness{0.32f};

    float stormRainThreshold{0.55f};
    float stormSunAttenuation{0.72f};
    float stormMoonAttenuation{0.82f};
    float baseFogDensity{0.00032f};
    float fogHeightFalloff{0.020f};
    float stormFogDensityBoost{1.20f};
    float wetFogDensityBoost{0.18f};

    float lightningPeak{8.0f};
    float lightningDecayRate{6.0f};
};

struct EnvironmentState {
    double totalTimeSeconds{0.0};
    float timeOfDay{14.0f};
    float wetnessFactor{0.0f};
    // Normalized regional saturation.  This is CPU-only state;
    // the shader receives the derived world height and flood progression.
    float waterTableLevel{0.0f};
    float waterTableHeight{-4.05f};
    float floodCoverage{0.0f};
    float waterRippleStrength{0.0f};
    float puddleCoverage{0.0f};
    float lightningFlash{0.0f};
    float stormIntensity{0.0f};
    float fogDensity{0.00032f};
    float starVisibility{0.0f};
};

class EnvironmentSimulation {
public:
    EnvironmentSimulation();
    explicit EnvironmentSimulation(EnvironmentControls initialControls);

    // Controls and state are intentionally editable so a debug UI can bind
    // directly to them. update() sanitizes all externally edited values.
    EnvironmentControls controls{};
    EnvironmentState state{};

    const EnvironmentCB& update(float deltaTimeSeconds);
    void reset(EnvironmentState initialState = {});
    void triggerLightning();
    void triggerLightning(float peak);

    [[nodiscard]] const EnvironmentCB& constants() const { return constants_; }

private:
    EnvironmentCB constants_{};
};

} // namespace dense
