#include "environment_cb.hlsli"

// Byte-for-byte shader mirror of dense::MeshVertex.  The renderer must bind
// buffers with a 40-byte structure stride.
struct MeshVertex
{
    float3 position;
    float3 normal;
    uint color;
    float material;
    float2 uv;
};

StructuredBuffer<MeshVertex> BaseVertices : register(t0);
RWStructuredBuffer<MeshVertex> DeformedVertices : register(u0);

// Four 32-bit root constants at b0.
cbuffer TreeWindConstants : register(b0)
{
    uint g_TreeVertexCount;
    float g_TreeHeight;
    uint g_TreeWindPadding0;
    uint g_TreeWindPadding1;
};

static const float kFixedBaseHeight = 0.45;

// For p' = p + direction * scalar(p), transform a normal by the inverse
// transpose of J = I + direction (gradient scalar)^T.  This keeps branch
// shading coherent with the actual bend instead of merely rotating normals by
// an unrelated visual approximation.
float3 TransformNormalForScalarDisplacement(
    float3 normal,
    float3 direction,
    float3 scalarGradient)
{
    float denominator = 1.0 + dot(scalarGradient, direction);
    denominator = abs(denominator) < 1e-4
        ? (denominator < 0.0 ? -1e-4 : 1e-4)
        : denominator;
    return normalize(normal - scalarGradient *
        (dot(direction, normal) / denominator));
}

[numthreads(256, 1, 1)]
void TreeWindCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint vertexIndex = dispatchThreadId.x;
    if (vertexIndex >= g_TreeVertexCount)
        return;

    MeshVertex vertex = BaseVertices[vertexIndex];
    const float3 basePosition = vertex.position;

    // Roots, buttresses, and the lowest trunk remain exactly equal to the base
    // mesh.  Besides looking correct, this gives the updated BLAS a stable
    // ground contact from frame to frame.
    const float movableHeight = max(g_TreeHeight - kFixedBaseHeight, 1e-3);
    const float heightT = saturate(
        (basePosition.y - kFixedBaseHeight) / movableHeight);
    const float smoothHeight = heightT * heightT * (3.0 - 2.0 * heightT);
    const float bendWeight = smoothHeight * smoothHeight;
    const float dSmoothHeightDy =
        (6.0 * heightT * (1.0 - heightT)) / movableHeight;
    const float dBendWeightDy =
        2.0 * smoothHeight * dSmoothHeightDy;

    float2 windDirection = g_WindDirection;
    const float windDirectionLengthSq = dot(windDirection, windDirection);
    windDirection = windDirectionLengthSq > 1e-6
        ? windDirection * rsqrt(windDirectionLengthSq)
        : float2(1.0, 0.0);
    const float3 wind = float3(windDirection.x, 0.0, windDirection.y);
    const float3 crossWind = float3(-wind.z, 0.0, wind.x);

    // The response curves make ordinary 1 m/s debug settings visibly alive,
    // while asymptotically capping the maximum UI values.  At the maximum UI
    // values (15 m/s, strength 3), the global crown offset stays below 0.165 m.
    const float speedResponse = saturate(1.0 - exp(-0.35 * max(g_WindSpeed, 0.0)));
    const float strengthResponse = saturate(
        (1.0 - exp(-0.90 * max(g_WindStrength, 0.0))) /
        (1.0 - exp(-2.70)));
    const float windEnergy = strengthResponse * sqrt(speedResponse);

    const float gustRate = 0.55 + 0.07 * max(g_WindSpeed, 0.0) +
                           0.50 * max(g_WindGustFrequency, 0.0);
    const float gustPhase = g_Time * gustRate;
    const float primarySway = 0.58 + 0.42 * sin(gustPhase);
    const float lateralSway = sin(gustPhase * 1.73 + 1.19);
    const float3 crownOffset = wind * (0.140 * windEnergy * primarySway) +
                               crossWind * (0.024 * windEnergy * lateralSway);

    vertex.position += crownOffset * bendWeight;
    vertex.normal = TransformNormalForScalarDisplacement(
        vertex.normal,
        crownOffset,
        float3(0.0, dBendWeightDy, 0.0));

    // Tree materials encode their coarse kind in the integer portion:
    // branch/bark = 0.x and foliage = 1.x.  The low-amplitude wave is broad
    // enough that every vertex of a leaf fan moves coherently, yet fast enough
    // to read as leaf flutter rather than whole-crown sway.
    const uint materialKind = (uint)floor(max(vertex.material, 0.0) + 1e-4);
    if (materialKind == 1u && heightT > 0.0 && windEnergy > 0.0)
    {
        const float3 flutterPhaseGradient = float3(2.10, 0.47, -1.70);
        const float flutterPhase =
            g_Time * (4.0 + 0.35 * max(g_WindSpeed, 0.0)) +
            dot(basePosition, flutterPhaseGradient);
        const float flutterWave =
            (sin(flutterPhase) + 0.35 * sin(flutterPhase * 1.73 + 1.40)) /
            1.35;
        const float flutterWaveDerivative =
            (cos(flutterPhase) +
             0.35 * 1.73 * cos(flutterPhase * 1.73 + 1.40)) /
            1.35;
        const float flutterAmplitude = 0.012 * windEnergy;
        const float3 flutterDirection = normalize(
            crossWind * 0.82 + float3(0.0, 0.35, 0.0));
        const float flutterScalar =
            flutterAmplitude * bendWeight * flutterWave;
        const float3 flutterGradient = flutterAmplitude *
            (flutterPhaseGradient * (bendWeight * flutterWaveDerivative) +
             float3(0.0, dBendWeightDy * flutterWave, 0.0));

        vertex.position += flutterDirection * flutterScalar;
        vertex.normal = TransformNormalForScalarDisplacement(
            vertex.normal, flutterDirection, flutterGradient);
    }

    DeformedVertices[vertexIndex] = vertex;
}
