Texture2D<float4> sourceEnvironment : register(t0);
RWTexture2D<float4> prefilteredEnvironment : register(u0);
RWTexture2D<float2> brdfIntegrationLUT : register(u1);

SamplerState environmentSampler : register(s0);

cbuffer IBLConstants : register(b0) {
    uint outputWidth;
    uint outputHeight;
    float roughness;
    uint sampleCount;
    float environmentRotation;
};

static const float PI = 3.14159265359;

float RadicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley(uint index, uint count) {
    return float2(float(index) / float(max(count, 1u)), RadicalInverseVdC(index));
}

float3 ImportanceSampleGGX(float2 xi, float3 normal, float alpha) {
    float phi = 2.0 * PI * xi.x;
    float alpha2 = alpha * alpha;
    float cosTheta = sqrt((1.0 - xi.y) /
        max(1.0 + (alpha2 - 1.0) * xi.y, 1e-6));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    float3 tangentHalf = float3(cos(phi) * sinTheta, sin(phi) * sinTheta,
                                cosTheta);

    float3 up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0)
                                      : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * tangentHalf.x + bitangent * tangentHalf.y +
                     normal * tangentHalf.z);
}

float2 DirectionToEquirectangular(float3 direction) {
    direction = normalize(direction);
    return float2(atan2(direction.z, direction.x) / (2.0 * PI) + 0.5,
                  acos(clamp(direction.y, -1.0, 1.0)) / PI);
}

float3 EquirectangularToDirection(float2 uv) {
    float phi = (uv.x - 0.5) * 2.0 * PI;
    float theta = uv.y * PI;
    float sinTheta = sin(theta);
    return float3(sinTheta * cos(phi), cos(theta), sinTheta * sin(phi));
}

float DistributionGGX(float nDotH, float alpha) {
    float alpha2 = alpha * alpha;
    float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * denominator * denominator, 1e-6);
}

[numthreads(8, 8, 1)]
void PrefilterEnvironmentCS(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadID.xy;
    if (pixel.x >= outputWidth || pixel.y >= outputHeight) return;

    float2 uv = (float2(pixel) + 0.5) / float2(outputWidth, outputHeight);
    float3 normal = EquirectangularToDirection(uv);
    float3 view = normal;
    float3 filtered = 0.0;
    float totalWeight = 0.0;

    uint sourceWidth, sourceHeight, sourceMipCount;
    sourceEnvironment.GetDimensions(0, sourceWidth, sourceHeight, sourceMipCount);
    float texelSolidAngle = 4.0 * PI /
        max(float(sourceWidth * sourceHeight), 1.0);
    float alpha = max(roughness * roughness, 0.001);

    [loop]
    for (uint i = 0; i < sampleCount; ++i) {
        float3 halfVector = ImportanceSampleGGX(
            Hammersley(i, sampleCount), normal, alpha);
        float3 light = normalize(2.0 * dot(view, halfVector) * halfVector - view);
        float nDotL = saturate(dot(normal, light));
        if (nDotL <= 0.0) continue;

        float nDotH = saturate(dot(normal, halfVector));
        float hDotV = saturate(dot(halfVector, view));
        float pdf = DistributionGGX(nDotH, alpha) * nDotH /
                    max(4.0 * hDotV, 1e-5);
        float sampleSolidAngle = 1.0 / max(float(sampleCount) * pdf, 1e-5);
        float sourceMip = roughness <= 0.001 ? 0.0 :
            max(0.5 * log2(sampleSolidAngle / texelSolidAngle), 0.0);
        float2 sourceUV = DirectionToEquirectangular(light);
        sourceUV.x = frac(sourceUV.x + environmentRotation / (2.0 * PI));
        filtered += sourceEnvironment.SampleLevel(
            environmentSampler, sourceUV, sourceMip).rgb * nDotL;
        totalWeight += nDotL;
    }

    prefilteredEnvironment[pixel] = float4(
        filtered / max(totalWeight, 1e-5), 1.0);
}

float GeometrySchlickGGX(float nDotV, float surfaceRoughness) {
    float k = surfaceRoughness * surfaceRoughness * 0.5;
    return nDotV / max(nDotV * (1.0 - k) + k, 1e-5);
}

float GeometrySmith(float nDotV, float nDotL, float surfaceRoughness) {
    return GeometrySchlickGGX(nDotV, surfaceRoughness) *
           GeometrySchlickGGX(nDotL, surfaceRoughness);
}

[numthreads(8, 8, 1)]
void IntegrateBRDFCS(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadID.xy;
    if (pixel.x >= outputWidth || pixel.y >= outputHeight) return;

    float2 uv = (float2(pixel) + 0.5) / float2(outputWidth, outputHeight);
    float nDotV = max(uv.x, 1e-4);
    float surfaceRoughness = uv.y;
    float3 view = float3(sqrt(saturate(1.0 - nDotV * nDotV)), 0.0, nDotV);
    float scale = 0.0;
    float bias = 0.0;
    float3 normal = float3(0.0, 0.0, 1.0);
    float alpha = max(surfaceRoughness * surfaceRoughness, 0.001);

    [loop]
    for (uint i = 0; i < sampleCount; ++i) {
        float3 halfVector = ImportanceSampleGGX(
            Hammersley(i, sampleCount), normal, alpha);
        float3 light = normalize(2.0 * dot(view, halfVector) * halfVector - view);
        float nDotL = saturate(light.z);
        float nDotH = saturate(halfVector.z);
        float vDotH = saturate(dot(view, halfVector));
        if (nDotL <= 0.0) continue;

        float geometry = GeometrySmith(nDotV, nDotL, surfaceRoughness);
        float geometryVisibility = geometry * vDotH /
            max(nDotH * nDotV, 1e-5);
        float fresnel = pow(1.0 - vDotH, 5.0);
        scale += (1.0 - fresnel) * geometryVisibility;
        bias += fresnel * geometryVisibility;
    }

    brdfIntegrationLUT[pixel] = float2(scale, bias) /
        float(max(sampleCount, 1u));
}
