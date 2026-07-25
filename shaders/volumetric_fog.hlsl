struct FogCluster
{
    uint lightCount;
    uint lightIndices[32];
};

struct FogPointLight
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
};

cbuffer FogConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float4x4 shadowCascadeMatrices[3];
    float4 cameraPositionNear;
    float4 cameraForwardFar;
    float4 sunDirectionDensity;
    float4 sunColorAnisotropy;
    float4 fogParams;             // height falloff, base height, max distance, shadow enabled
    float4 ambientFogColor;
    float4 shadowCascadeSplits;
    uint4 clusterDimsLightCount;  // x, y, z, source light count
};

StructuredBuffer<FogCluster> clusters : register(t0);
StructuredBuffer<FogPointLight> pointLights : register(t1);
Texture2DArray<float> shadowMap : register(t2);
Texture2D<float> sceneDepth : register(t3);
Texture3D<float4> fogVolume : register(t4);
Texture2DMS<float, 4> sceneDepthMS : register(t5);
RWTexture3D<float4> fogVolumeOut : register(u0);

SamplerComparisonState shadowSampler : register(s0);
SamplerState linearClampSampler : register(s1);

float SliceDepth(float slice)
{
    const float nearZ = cameraPositionNear.w;
    const float farZ = min(cameraForwardFar.w, fogParams.z);
    return nearZ * pow(farZ / nearZ, slice / clusterDimsLightCount.z);
}

float3 WorldRay(float2 uv)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(float4(ndc, 1.0, 1.0), inverseViewProjection);
    world.xyz /= max(abs(world.w), 1e-5);
    return normalize(world.xyz - cameraPositionNear.xyz);
}

float SunVisibility(float3 worldPosition)
{
    if (fogParams.w < 0.5)
        return 1.0;
    float viewDepth = dot(worldPosition - cameraPositionNear.xyz,
                          normalize(cameraForwardFar.xyz));
    uint cascade = viewDepth < shadowCascadeSplits.x ? 0u :
                   (viewDepth < shadowCascadeSplits.y ? 1u : 2u);
    float4 projected = mul(float4(worldPosition, 1.0),
                           shadowCascadeMatrices[cascade]);
    projected.xyz /= max(abs(projected.w), 1e-5);
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || projected.z <= 0.0 || projected.z >= 1.0)
        return 1.0;
    return shadowMap.SampleCmpLevelZero(
        shadowSampler, float3(uv, cascade), projected.z - 0.0025);
}

float HenyeyGreenstein(float cosineTheta, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / max(4.0 * 3.14159265 * pow(1.0 + g2 - 2.0 * g * cosineTheta, 1.5), 1e-4);
}

float FogNoise(float3 p)
{
    // Broad moving density cells. Stable in world space; time only advects them.
    p += float3(ambientFogColor.w * 0.42, 0.0, ambientFogColor.w * 0.19);
    float n = sin(p.x * 0.105 + sin(p.z * 0.071)) *
              sin(p.z * 0.093 - p.y * 0.17);
    n += sin((p.x + p.z) * 0.037 + ambientFogColor.w * 0.31) * 0.55;
    return saturate(n * 0.32 + 0.62);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= clusterDimsLightCount.x || id.y >= clusterDimsLightCount.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(clusterDimsLightCount.xy);
    float3 ray = WorldRay(uv);
    float viewCos = max(dot(ray, normalize(cameraForwardFar.xyz)), 0.08);
    float3 accumulated = 0.0;
    float transmittance = 1.0;
    const float3 lightDirection = normalize(sunDirectionDensity.xyz);
    const float phase = HenyeyGreenstein(dot(lightDirection, ray), sunColorAnisotropy.w);

    [loop]
    for (uint z = 0; z < clusterDimsLightCount.z; ++z)
    {
        float nearDepth = SliceDepth((float)z);
        float farDepth = SliceDepth((float)z + 1.0);
        float centerDepth = sqrt(nearDepth * farDepth);
        float stepLength = (farDepth - nearDepth) / viewCos;
        float3 worldPosition = cameraPositionNear.xyz + ray * (centerDepth / viewCos);

        // Jungle gradient: a broad upper haze plus a softer, denser low ground
        // layer that pools in the valley floor. The ground layer's falloff is kept
        // gentle (2x, not a sharp spike) so it fades smoothly into the upper haze
        // with no hard seam where it cuts off across tall grass and terrain.
        float aboveBase = max(worldPosition.y - fogParams.y, 0.0);
        float heightDensity = exp(-aboveBase * fogParams.x);
        float groundLayer = exp(-aboveBase * (fogParams.x * 2.0)) * 0.9;
        float densityNoise = lerp(0.58, 1.28, FogNoise(worldPosition));
        heightDensity = (heightDensity + groundLayer) * densityNoise;
        heightDensity = max(heightDensity, 0.03);
        float extinction = max(sunDirectionDensity.w * heightDensity, 0.00001);
        float segmentTransmittance = exp(-extinction * stepLength);

        float shadow = SunVisibility(worldPosition);
        // Daylight fog must replace attenuated scene energy with sky irradiance.
        // A tiny ambient term made the former pass behave like red/brown smoke.
        float3 lighting = ambientFogColor.xyz +
            sunColorAnisotropy.xyz * phase * shadow * 0.55;
        uint clusterIndex = id.x + id.y * clusterDimsLightCount.x +
            z * clusterDimsLightCount.x * clusterDimsLightCount.y;
        FogCluster cluster = clusters[clusterIndex];
        uint count = min(cluster.lightCount, 32u);
        [loop]
        for (uint i = 0; i < count; ++i)
        {
            uint lightIndex = cluster.lightIndices[i];
            if (lightIndex >= clusterDimsLightCount.w)
                continue;
            FogPointLight light = pointLights[lightIndex];
            float3 toLight = light.position - worldPosition;
            float distanceToLight = length(toLight);
            float range = saturate(1.0 - distanceToLight / max(light.radius, 0.001));
            lighting += light.color * light.intensity * range * range * 0.10;
        }

        float3 segmentScattering = lighting * (1.0 - segmentTransmittance);
        accumulated += transmittance * segmentScattering;
        transmittance *= segmentTransmittance;
        fogVolumeOut[uint3(id.xy, z)] = float4(accumulated, transmittance);
    }
}

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
    float nearZ = cameraPositionNear.w;
    float farZ = cameraForwardFar.w;
    return nearZ * farZ / max(farZ - depth * (farZ - nearZ), 1e-5);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float deviceDepth = sceneDepth.SampleLevel(linearClampSampler, input.uv, 0.0);
    // Procedural sky already contains atmospheric scattering. Fogging depth=1
    // applied the full 180 m extinction to it and tinted the entire frame.
    if (deviceDepth >= 0.99999)
        return float4(0.0, 0.0, 0.0, 1.0);
    float viewDepth = min(LinearDepth(deviceDepth), fogParams.z);
    float nearZ = cameraPositionNear.w;
    float fogFar = min(cameraForwardFar.w, fogParams.z);
    float slice = log(max(viewDepth, nearZ) / nearZ) / log(fogFar / nearZ);
    float3 uvw = float3(input.uv, saturate(slice));
    float4 fog = fogVolume.SampleLevel(linearClampSampler, uvw, 0.0);
    return float4(fog.rgb, fog.a);
}

float4 PSMainMSAA(VSOutput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float deviceDepth = sceneDepthMS.Load(pixel, 0);
    [unroll]
    for (uint sampleIndex = 1; sampleIndex < 4; ++sampleIndex)
        deviceDepth = min(deviceDepth, sceneDepthMS.Load(pixel, sampleIndex));
    if (deviceDepth >= 0.99999)
        return float4(0.0, 0.0, 0.0, 1.0);
    float viewDepth = min(LinearDepth(deviceDepth), fogParams.z);
    float nearZ = cameraPositionNear.w;
    float fogFar = min(cameraForwardFar.w, fogParams.z);
    float slice = log(max(viewDepth, nearZ) / nearZ) / log(fogFar / nearZ);
    float4 fog = fogVolume.SampleLevel(
        linearClampSampler, float3(input.uv, saturate(slice)), 0.0);
    return float4(fog.rgb, fog.a);
}
