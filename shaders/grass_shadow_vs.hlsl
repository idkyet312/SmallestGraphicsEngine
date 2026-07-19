cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

cbuffer GrassParams : register(b6) {
    float gTime;
    float gWindStrength;
    float gWindSpeed;
    float gEyeX;
    float gEyeZ;
    float gShadowDensity;
    float gFadeBand;
    uint  gFirstBlade;
    float gHelicopterX;
    float gHelicopterZ;
    float gHelicopterWindRadius;
    float gHelicopterWindStrength;
    float gRangeCount;
};

struct BladeInstance {
    float3 root;
    float height;
    float2 dir;
    float2 lean;
    float width;
    float phase;
    float2 pad;
};

StructuredBuffer<BladeInstance> blades : register(t6);

struct VSInput {
    float4 shape : POSITION;
    uint instanceId : SV_InstanceID;
};

float4 main(VSInput input) : SV_POSITION {
    // Walking a contiguous density prefix selects whole early tufts and leaves
    // the rest bare. Stride through the complete cell instead. Range count and
    // density reuse raster-only GrassParams fields in this shadow-only shader.
    const uint rangeCount = max((uint)gRangeCount, 1u);
    const uint stride = max((uint)floor(1.0 / max(gShadowDensity, 0.001)), 1u);
    const uint offset = (gFirstBlade * 1664525u + 1013904223u) % stride;
    const uint localIndex = (input.instanceId * stride + offset) % rangeCount;
    const BladeInstance blade = blades[gFirstBlade + localIndex];
    const float t = input.shape.x;
    const float side = input.shape.y;
    const float authoredForward = input.shape.z;
    const float authoredWidth = input.shape.w;
    // Slight inflation survives shadow-map filtering without turning each thin
    // blade into a wide dark bar on the terrain.
    const float shadowWidth = max(blade.width * 2.0, 0.045);
    const float2 fromHelicopter = blade.root.xz - float2(gHelicopterX, gHelicopterZ);
    const float helicopterDistance = length(fromHelicopter);
    const float helicopterFalloff = pow(saturate(
        1.0 - helicopterDistance / max(gHelicopterWindRadius, 1e-3)), 0.65);
    const float2 helicopterDirection = helicopterDistance > 1e-3
        ? fromHelicopter / helicopterDistance : float2(1.0, 0.0);
    const float rotorPulse = 0.88 + 0.12 * sin(
        gTime * 22.0 + helicopterDistance * 1.7 + blade.phase);
    float2 tip = blade.lean + float2(0.22, 0.12) + helicopterDirection *
        helicopterFalloff * gHelicopterWindStrength * rotorPulse;
    const float tipLength = length(tip);
    if (tipLength > 0.97) tip *= 0.97 / tipLength;
    const float2 dynamicBend = tip * (t * t);
    const float2 forwardDir = float2(-blade.dir.y, blade.dir.x);
    const float2 normalizedBend = forwardDir * authoredForward + dynamicBend;
    const float2 bend = normalizedBend * blade.height;
    const float droop = sqrt(max(0.0, 1.0 - dot(dynamicBend, dynamicBend)));
    float3 position;
    position.x = blade.root.x + blade.dir.x * shadowWidth * authoredWidth * side + bend.x;
    position.y = blade.root.y + blade.height * t * droop;
    position.z = blade.root.z + blade.dir.y * shadowWidth * authoredWidth * side + bend.y;
    return mul(float4(position, 1.0), lightSpaceMatrix);
}
