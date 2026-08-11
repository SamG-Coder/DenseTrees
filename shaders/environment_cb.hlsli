cbuffer EnvironmentCB : register(b1)
{
    float g_Time;
    float g_DeltaTime;
    float g_TimeOfDay;
    float g_SunElevation;

    float3 g_SunDirection;
    float g_SunIntensity;

    float3 g_SunColor;
    float g_MoonPhase;

    float3 g_MoonDirection;
    float g_MoonIntensity;

    float3 g_MoonColor;
    float g_WindSpeed;

    float2 g_WindDirection;
    float g_WindStrength;
    float g_WindGustFrequency;

    float g_RainIntensity;
    float g_WetnessFactor;
    float g_PuddleCoverage;
    float g_LightningFlash;

    float g_FogDensity;
    float g_FogHeightFalloff;
    float g_StormIntensity;
    float g_StarVisibility;
};
