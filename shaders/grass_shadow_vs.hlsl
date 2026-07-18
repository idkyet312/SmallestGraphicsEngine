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
    float gDrawDistance;
    float gFadeBand;
    uint  gFirstBlade;
    float gHelicopterX;
    float gHelicopterZ;
    float gHelicopterWindRadius;
    float gHelicopterWindStrength;
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
    float2 corner : POSITION;
    uint instanceId : SV_InstanceID;
};

float4 main(VSInput input) : SV_POSITION {
    const BladeInstance blade = blades[gFirstBlade + input.instanceId];
    const float t = input.corner.x;
    const float side = input.corner.y;
    const float taper = 1.0 - t * 0.72;
    // Shadow-map texels are much wider than a real blade. Inflate only the
    // caster silhouette so PCF does not erase it; visible grass stays unchanged.
    const float shadowWidth = max(blade.width * 5.0, 0.12);
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
    const float2 normalizedBend = tip * (t * t);
    const float2 bend = normalizedBend * blade.height;
    const float droop = sqrt(max(0.0, 1.0 - dot(normalizedBend, normalizedBend)));
    float3 position;
    position.x = blade.root.x + blade.dir.x * shadowWidth * taper * side + bend.x;
    position.y = blade.root.y + blade.height * t * droop;
    position.z = blade.root.z + blade.dir.y * shadowWidth * taper * side + bend.y;
    return mul(float4(position, 1.0), lightSpaceMatrix);
}
