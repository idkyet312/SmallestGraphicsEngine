cbuffer ParticleFrame : register(b0)
{
    float4x4 viewProjection;
    float3 cameraRight;
    float smokeIllumination;
    float3 cameraUp;
    float framePadding1;
};

struct ParticleInstance
{
    float3 position;
    float size;
    float3 velocity;
    float opacity;
    float3 color;
    uint kind; // 0 smoke, 1 blood, 2 spark
};

StructuredBuffer<ParticleInstance> particles : register(t0);
Texture2D particleTexture : register(t1);
SamplerState particleSampler : register(s0);

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 color : COLOR0;
    float opacity : TEXCOORD1;
    nointerpolation uint kind : TEXCOORD2;
};

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, 1.0), float2(1.0, -1.0)
    };
    ParticleInstance particle = particles[instanceId];
    float2 corner = corners[vertexId];
    float3 right = cameraRight;
    float3 up = cameraUp;
    float2 extent = particle.size.xx;

    if (particle.kind == 2) {
        float2 projected = float2(dot(particle.velocity, cameraRight),
                                  dot(particle.velocity, cameraUp));
        float projectedLength = length(projected);
        float2 along = projectedLength > 0.001
            ? projected / projectedLength : float2(0.0, 1.0);
        float2 across = float2(-along.y, along.x);
        right = cameraRight * along.x + cameraUp * along.y;
        up = cameraRight * across.x + cameraUp * across.y;
        extent.x *= 2.5 + min(length(particle.velocity) * 0.10, 5.0);
        extent.y *= 0.55;
    }

    float3 worldPosition = particle.position +
        right * corner.x * extent.x + up * corner.y * extent.y;
    VSOutput output;
    output.position = mul(float4(worldPosition, 1.0), viewProjection);
    output.uv = corner * float2(0.5, -0.5) + 0.5;
    output.color = particle.color;
    output.opacity = particle.opacity;
    output.kind = particle.kind;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float alpha;
    if (input.kind == 2) {
        float2 centered = abs(input.uv * 2.0 - 1.0);
        alpha = saturate(1.0 - centered.x) * saturate(1.0 - centered.y);
        alpha = sqrt(alpha) * input.opacity;
    } else {
        alpha = particleTexture.Sample(particleSampler, input.uv).a * input.opacity;
    }
    clip(alpha - 0.003);
    float3 color = input.color;
    if (input.kind == 0)
        color *= smokeIllumination;
#ifndef SGE_HDR_TARGET
    color = pow(color / (1.0 + color), 1.0 / 2.2);
#endif
    return float4(color, alpha);
}
