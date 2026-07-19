cbuffer AOConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4 cameraNearFar;       // xyz camera position, w near plane
    float4 lightDirection;      // xyz surface-to-sun direction, w contact strength
    float4 aoParams;            // radius, strength, bias, contact distance
    float4 screenParams;        // width, height, 1/width, 1/height
    float4 filterParams;        // x: pre-forward contact-caster depth bound
};

Texture2D<float> sceneDepth : register(t0);
Texture2DMS<float, 4> sceneDepthMS : register(t1);
Texture2D<float> staticCasterDepth : register(t2);
SamplerState pointClamp : register(s0);

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    float2 p = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = p;
    output.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float LinearDepth(float depth)
{
    const float nearZ = cameraNearFar.w;
    const float farZ = aoParams.w;
    return nearZ * farZ / max(farZ - depth * (farZ - nearZ), 1e-5);
}

float3 ReconstructWorld(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(float4(ndc, depth, 1.0), inverseViewProjection);
    return world.xyz / max(abs(world.w), 1e-5);
}

float SampleDepth(float2 uv, bool multisampled)
{
    if (!multisampled)
        return sceneDepth.SampleLevel(pointClamp, saturate(uv), 0.0);
    int2 pixel = clamp(int2(saturate(uv) * screenParams.xy),
                       int2(0, 0), int2(screenParams.xy) - 1);
    float depth = sceneDepthMS.Load(pixel, 0);
    [unroll]
    for (uint sampleIndex = 1; sampleIndex < 4; ++sampleIndex)
        depth = min(depth, sceneDepthMS.Load(pixel, sampleIndex));
    return depth;
}

static const float kViewModelMaxDepth = 1.25;

bool IsViewModelDepth(float deviceDepth)
{
    return deviceDepth < 0.99999 && LinearDepth(deviceDepth) < kViewModelMaxDepth;
}

float SampleContactCasterDepth(float2 uv, bool multisampled)
{
    if (filterParams.x > 0.5)
        return staticCasterDepth.SampleLevel(pointClamp, saturate(uv), 0.0);
    return SampleDepth(uv, multisampled);
}

float AmbientVisibility(float2 uv, float deviceDepth, bool multisampled)
{
    if (deviceDepth >= 0.99999) return 1.0;
    const float centerDepth = LinearDepth(deviceDepth);
    const float radius = aoParams.x;
    const float pixelRadius = clamp(radius * screenParams.y /
        max(centerDepth * 1.15, 0.1), 2.0, 42.0);
    static const float2 directions[8] = {
        float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
        float2(0.707, 0.707), float2(-0.707, 0.707),
        float2(0.707, -0.707), float2(-0.707, -0.707)
    };
    float occlusion = 0.0;
    [unroll]
    for (uint direction = 0; direction < 8; ++direction) {
        [unroll]
        for (uint tap = 1; tap <= 2; ++tap) {
            float2 offset = directions[direction] * pixelRadius *
                (tap * 0.5) * screenParams.zw;
            float sampleDeviceDepth = SampleDepth(uv + offset, multisampled);
            float sampleViewDepth = LinearDepth(sampleDeviceDepth);
            float delta = centerDepth - sampleViewDepth - aoParams.z;
            occlusion += saturate(delta / max(radius, 1e-3)) *
                saturate(1.0 - delta / max(radius * 2.0, 1e-3));
        }
    }
    return saturate(1.0 - occlusion * (aoParams.y / 16.0));
}

float ContactVisibility(float2 uv, float deviceDepth, bool multisampled)
{
    if (deviceDepth >= 0.99999 || lightDirection.w <= 0.0) return 1.0;
    if (IsViewModelDepth(deviceDepth)) return 1.0;
    float3 world = ReconstructWorld(uv, deviceDepth);
    float3 rayDirection = normalize(lightDirection.xyz);
    const float maxDistance = max(aoParams.w * 0.004, 1.5);
    float visibility = 1.0;
    [unroll]
    for (uint step = 1; step <= 10; ++step) {
        float t = maxDistance * (step / 10.0);
        float4 projected = mul(float4(world + rayDirection * t, 1.0), viewProjection);
        if (projected.w <= 0.0) continue;
        float3 ndc = projected.xyz / projected.w;
        float2 rayUV = ndc.xy * float2(0.5, -0.5) + 0.5;
        if (any(rayUV <= 0.0) || any(rayUV >= 1.0)) break;
        // Use depth captured before gun and animated enemies were drawn.
        float sampledDepth = SampleContactCasterDepth(rayUV, multisampled);
        if (IsViewModelDepth(sampledDepth)) continue;
        float thickness = 0.00035 + t * 0.000035;
        if (sampledDepth + thickness < ndc.z) {
            visibility = 1.0 - lightDirection.w *
                saturate(1.0 - t / maxDistance);
            break;
        }
    }
    return visibility;
}

float4 Shade(float2 uv, float depth, bool multisampled)
{
    float visibility = AmbientVisibility(uv, depth, multisampled) *
        ContactVisibility(uv, depth, multisampled);
    return float4(visibility.xxx, 1.0);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return Shade(input.uv, SampleDepth(input.uv, false), false);
}

float4 PSMainMSAA(VSOutput input) : SV_TARGET
{
    return Shade(input.uv, SampleDepth(input.uv, true), true);
}
