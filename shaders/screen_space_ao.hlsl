cbuffer AOConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4 cameraNearFar; // xyz camera, w near
    float4 lightDirection;
    float4 aoParams;      // radius, strength, bias, far
    float4 screenParams;  // width, height, inv width, inv height
    float4 filterParams;  // static depth, surface buffer
};

Texture2D<float> sceneDepth : register(t0);
Texture2DMS<float, 4> sceneDepthMS : register(t1);
Texture2D<float> staticCasterDepth : register(t2);
Texture2D<float4> surfaceData : register(t3);
Texture2D<float> rawAO : register(t4);
SamplerState pointClamp : register(s0);
SamplerState linearClamp : register(s1);

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
    output.position =
        float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float LinearDepth(float depth)
{
    const float nearZ = cameraNearFar.w;
    const float farZ = aoParams.w;
    return nearZ * farZ /
        max(farZ - depth * (farZ - nearZ), 1e-5);
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

float3 ReconstructNormal(float2 uv, float depth, bool multisampled)
{
    float3 stored = 0.0;
    float storedValid = 0.0;
    if (filterParams.y > 0.5) {
        stored = surfaceData.SampleLevel(pointClamp, uv, 0.0).xyz;
        float depthMatch = 1.0;
        if (filterParams.x > 0.5) {
            // Mesh terrain and other forward extensions update sceneDepth after
            // visibility resolve. Reject a stored normal when it belongs to
            // geometry now hidden behind that forward surface.
            float visibilityDepth =
                staticCasterDepth.SampleLevel(pointClamp, uv, 0.0);
            float sceneLinear = LinearDepth(depth);
            float visibilityLinear = LinearDepth(visibilityDepth);
            float tolerance = max(0.025, sceneLinear * 0.003);
            depthMatch = step(
                abs(sceneLinear - visibilityLinear), tolerance);
        }
        storedValid = depthMatch * step(0.25, dot(stored, stored));
    }
    float3 center = ReconstructWorld(uv, depth);
    float2 texel = screenParams.zw;
    float depthL = SampleDepth(uv - float2(texel.x, 0.0), multisampled);
    float depthR = SampleDepth(uv + float2(texel.x, 0.0), multisampled);
    float depthU = SampleDepth(uv - float2(0.0, texel.y), multisampled);
    float depthD = SampleDepth(uv + float2(0.0, texel.y), multisampled);
    float3 dxL = center - ReconstructWorld(uv - float2(texel.x, 0.0), depthL);
    float3 dxR = ReconstructWorld(uv + float2(texel.x, 0.0), depthR) - center;
    float3 dyU = center - ReconstructWorld(uv - float2(0.0, texel.y), depthU);
    float3 dyD = ReconstructWorld(uv + float2(0.0, texel.y), depthD) - center;
    float3 dx = length(dxL) < length(dxR) ? dxL : dxR;
    float3 dy = length(dyU) < length(dyD) ? dyU : dyD;
    float3 normal = normalize(cross(dx, dy));
    float3 toCamera = normalize(cameraNearFar.xyz - center);
    normal = dot(normal, toCamera) < 0.0 ? -normal : normal;
    return storedValid > 0.5 ? normalize(stored) : normal;
}

float InterleavedNoise(float2 pixel)
{
    return frac(52.9829189 *
        frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float HorizonVisibility(float2 uv, float deviceDepth, bool multisampled)
{
    if (deviceDepth >= 0.99999) return 1.0;
    float3 center = ReconstructWorld(uv, deviceDepth);
    float3 normal = ReconstructNormal(uv, deviceDepth, multisampled);
    float centerDepth = LinearDepth(deviceDepth);
    float radius = aoParams.x;
    float pixelRadius = clamp(radius * screenParams.y /
        max(centerDepth * 1.15, 0.1), 3.0, 56.0);
    float rotation = InterleavedNoise(uv * screenParams.xy) * 6.2831853;
    float2x2 rotate = float2x2(cos(rotation), -sin(rotation),
                               sin(rotation), cos(rotation));
    float occlusion = 0.0;

    [unroll]
    for (uint slice = 0; slice < 8; ++slice) {
        float angle = (slice + 0.5) * (6.2831853 / 8.0);
        float2 direction = mul(float2(cos(angle), sin(angle)), rotate);
        float horizon = 0.0;
        [unroll]
        for (uint step = 1; step <= 4; ++step) {
            float stepScale = (step - 0.35 +
                InterleavedNoise(uv * screenParams.xy + step * 17.0)) / 4.0;
            float2 sampleUV =
                uv + direction * pixelRadius * stepScale * screenParams.zw;
            if (any(sampleUV <= 0.0) || any(sampleUV >= 1.0)) continue;
            float sampleDepth = SampleDepth(sampleUV, multisampled);
            if (sampleDepth >= 0.99999) continue;
            float3 delta = ReconstructWorld(sampleUV, sampleDepth) - center;
            float distanceToSample = length(delta);
            if (distanceToSample < 1e-4 || distanceToSample > radius * 2.0)
                continue;
            float cosine = dot(normal, delta / distanceToSample);
            float distanceFalloff =
                saturate(1.0 - distanceToSample / max(radius, 1e-3));
            horizon = max(horizon,
                saturate(cosine - aoParams.z) * distanceFalloff);
        }
        occlusion += horizon;
    }
    occlusion *= aoParams.y / 8.0;
    return saturate(1.0 - occlusion);
}

static const float kViewModelMaxDepth = 1.25;

bool IsViewModelDepth(float deviceDepth)
{
    return deviceDepth < 0.99999 &&
           LinearDepth(deviceDepth) < kViewModelMaxDepth;
}

float SampleContactCasterDepth(float2 uv, bool multisampled)
{
    float currentDepth = SampleDepth(uv, multisampled);
    // Do not let first-person gun depth cast into the world.
    if (IsViewModelDepth(currentDepth))
        currentDepth = 1.0;
    if (filterParams.x <= 0.5)
        return currentDepth;

    float casterDepth =
        staticCasterDepth.SampleLevel(pointClamp, saturate(uv), 0.0);
    // Static depth was captured before mesh terrain. Combining it with current
    // depth preserves the view-model exclusion while letting terrain occlude
    // objects hidden behind a hill instead of projecting them through it.
    return min(casterDepth, currentDepth);
}

float ContactVisibility(float2 uv, float deviceDepth, bool multisampled)
{
    if (deviceDepth >= 0.99999 || lightDirection.w <= 0.0 ||
        IsViewModelDepth(deviceDepth)) return 1.0;
    float3 world = ReconstructWorld(uv, deviceDepth);
    float3 surfaceNormal =
        ReconstructNormal(uv, deviceDepth, multisampled);
    world += surfaceNormal * max(0.015, aoParams.x * 0.03);
    float3 rayDirection = normalize(lightDirection.xyz);
    const float maxDistance = max(aoParams.w * 0.004, 1.5);
    [unroll]
    for (uint step = 1; step <= 10; ++step) {
        float t = maxDistance * (step / 10.0);
        float4 projected =
            mul(float4(world + rayDirection * t, 1.0), viewProjection);
        if (projected.w <= 0.0) continue;
        float3 ndc = projected.xyz / projected.w;
        float2 rayUV = ndc.xy * float2(0.5, -0.5) + 0.5;
        if (any(rayUV <= 0.0) || any(rayUV >= 1.0)) break;
        float sampledDepth =
            SampleContactCasterDepth(rayUV, multisampled);
        if (IsViewModelDepth(sampledDepth)) continue;
        float thickness = 0.00035 + t * 0.000035;
        if (sampledDepth + thickness < ndc.z)
            return 1.0 - lightDirection.w *
                saturate(1.0 - t / maxDistance);
    }
    return 1.0;
}

float BilateralAO(float2 uv, float centerDepth, float3 centerNormal,
                  bool multisampled)
{
    float sum = 0.0;
    float weightSum = 0.0;
    [unroll]
    for (int y = -2; y <= 2; ++y) {
        [unroll]
        for (int x = -2; x <= 2; ++x) {
            float2 offset = float2(x, y);
            float2 sampleUV = uv + offset * screenParams.zw;
            float sampleDeviceDepth = SampleDepth(sampleUV, multisampled);
            float sampleLinearDepth = LinearDepth(sampleDeviceDepth);
            float3 sampleNormal =
                ReconstructNormal(sampleUV, sampleDeviceDepth, multisampled);
            float spatial = exp(-dot(offset, offset) * 0.32);
            float depthWeight = exp(-abs(sampleLinearDepth - centerDepth) *
                                    18.0 / max(centerDepth, 0.25));
            float normalWeight =
                pow(saturate(dot(centerNormal, sampleNormal)), 8.0);
            float weight = spatial * depthWeight * normalWeight;
            sum += rawAO.SampleLevel(linearClamp, sampleUV, 0.0) * weight;
            weightSum += weight;
        }
    }
    return sum / max(weightSum, 1e-4);
}

float4 PSGTAO(VSOutput input) : SV_TARGET
{
    float depth = SampleDepth(input.uv, false);
    return HorizonVisibility(input.uv, depth, false).xxxx;
}

float4 PSGTAOMSAA(VSOutput input) : SV_TARGET
{
    float depth = SampleDepth(input.uv, true);
    return HorizonVisibility(input.uv, depth, true).xxxx;
}

float4 Composite(float2 uv, bool multisampled)
{
    float depth = SampleDepth(uv, multisampled);
    if (depth >= 0.99999) return 1.0;
    float3 normal = ReconstructNormal(uv, depth, multisampled);
    float visibility = BilateralAO(
        uv, LinearDepth(depth), normal, multisampled);
    visibility *= ContactVisibility(uv, depth, multisampled);
    return float4(visibility.xxx, 1.0);
}

float4 PSBlurComposite(VSOutput input) : SV_TARGET
{
    return Composite(input.uv, false);
}

float4 PSBlurCompositeMSAA(VSOutput input) : SV_TARGET
{
    return Composite(input.uv, true);
}
