// Grass pixel shader - a cheap subset of clustered_dx12_ps.hlsl.
//
// Grass blades are untextured, rough (0.85), and cover enormous numbers of
// mostly-edge pixels, so they were paying the full PBR/DDGI/PCF/point-light
// shader for no visible benefit. This keeps exactly the terms that read on a
// thin blade -- Lambert sun, SH sky ambient, one shadow tap, the same aerial
// fog and AgX tonemap so the field stays color-matched to the scene -- and
// drops everything else. Binds a strict subset of the main root signature.

cbuffer LightBuffer : register(b1) {
    float3 lightPos;
    int lightType;
    float3 lightColor;
    float attConstant;
    float attLinear;
    float attQuadratic;
    float ambientStrength;
    float specularStrength;
    int shininess;
    float shadowBias;
    int enableShadows;
    float shadowTexelSize;   // unused here (single tap), kept for layout parity
};

cbuffer CameraBuffer : register(b2) {
    float3 viewPos;
    float cameraPadding;
};

cbuffer ObjectBuffer : register(b3) {
    float3 objectColor;
    float useTexture;
    float metalness;
    float roughness;
    float useNormalMap;
    float metalRoughMode;
    float opacity;
    float smokeMode;
    float alphaCut;
    float ambientScale;
    float occlusionStrength;
    float normalYSign;
    float viewFillStrength;
    float normalTexW;        // unused here, kept for layout parity
    float normalTexH;
};

cbuffer SHBuffer : register(b7) {
    float4 shCoeffs[9];
    float skyIntensity;
    float3 shPadding;
};

Texture2D<float> shadowMap : register(t0);
SamplerComparisonState shadowSampler : register(s0);

struct PS_INPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 fragPosLightSpace : TEXCOORD4;
};

// One comparison tap: the linear-filtered comparison sampler gives hardware
// 2x2 PCF, which is plenty of softness for a 1-2 cm wide blade.
float CalculateShadowCheap(float4 fragPosLightSpace, float3 normal, float3 lightDir) {
    if (enableShadows == 0) return 1.0;

    float3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    float2 shadowUV = projCoords.xy * float2(0.5, -0.5) + 0.5;

    if (projCoords.z <= 0.0 || projCoords.z >= 1.0 ||
        shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0) {
        return 1.0;
    }

    float ndotl = saturate(dot(normal, lightDir));
    float bias = max(shadowBias * (1.0 - ndotl), shadowBias * 0.25);
    return shadowMap.SampleCmpLevelZero(shadowSampler, shadowUV, projCoords.z - bias);
}

// Identical to clustered_dx12_ps.hlsl so the field's ambient matches the turf.
float3 sampleSkyIrradiance(float3 normal) {
    float3 result = shCoeffs[0].rgb * 0.282095;
    result += shCoeffs[1].rgb * 0.488603 * normal.y;
    result += shCoeffs[2].rgb * 0.488603 * normal.z;
    result += shCoeffs[3].rgb * 0.488603 * normal.x;
    result += shCoeffs[4].rgb * 1.092548 * normal.x * normal.y;
    result += shCoeffs[5].rgb * 1.092548 * normal.y * normal.z;
    result += shCoeffs[6].rgb * 0.315392 * (3.0 * normal.z * normal.z - 1.0);
    result += shCoeffs[7].rgb * 1.092548 * normal.x * normal.z;
    result += shCoeffs[8].rgb * 0.546274 * (normal.x * normal.x - normal.y * normal.y);
    return max(result, 0.0) * skyIntensity;
}

// Identical AgX (Punchy) blocks to clustered_dx12_ps.hlsl.
float3 agxDefaultContrastApprox(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return  15.5     * x4 * x2
          - 40.14    * x4 * x
          + 31.96    * x4
          - 6.868    * x2 * x
          + 0.4298   * x2
          + 0.1191   * x
          - 0.00232;
}

float3 tonemapAgXPunchy(float3 color) {
    const float3x3 agxIn = float3x3(
        0.842479062253094,  0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 agxOut = float3x3(
         1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433,  1.15107367264116);

    const float minEv = -12.47393;
    const float maxEv =  4.026069;

    color = mul(agxIn, color);
    color = clamp(log2(max(color, 1e-10)), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);
    color = agxDefaultContrastApprox(color);

    const float3 lw = float3(0.2126, 0.7152, 0.0722);
    float luma = dot(color, lw);
    color = pow(color, 1.35);
    color = luma + 1.4 * (color - luma);

    color = mul(agxOut, color);
    return saturate(color);
}

float4 main(PS_INPUT input) : SV_TARGET {
    float3 normal = normalize(input.normal);
    float3 viewDir = normalize(viewPos - input.fragPos);
    float3 albedo = objectColor;

    // Blades are two-sided cards; flip the normal to face the camera so the
    // back of a blade doesn't go black.
    if (dot(normal, viewDir) < 0.0) normal = -normal;

    float3 lightDir = lightType == 0
        ? normalize(lightPos)
        : normalize(lightPos - input.fragPos);

    float shadowVisibility = CalculateShadowCheap(input.fragPosLightSpace, normal, lightDir);

    // Ambient (flat + SH sky), dimmed by shadow the same way the main shader
    // dims its indirect terms.
    float3 result = ambientStrength * albedo * ambientScale;
    result += sampleSkyIrradiance(normal) * albedo * ambientScale;
    result *= lerp(0.28, 1.0, shadowVisibility);

    // Direct sun: Lambert only. Blade roughness is 0.85 -- the GGX lobe the
    // main shader would compute is visually nil at that roughness.
    float NdotL = saturate(dot(normal, lightDir));
    result += albedo / 3.14159265 * lightColor * NdotL * shadowVisibility;

    // Same aerial perspective as clustered_dx12_ps.hlsl.
    float cameraDistance = length(viewPos - input.fragPos);
    float3 cameraRay = -viewDir;
    float horizonAmount = exp(-abs(cameraRay.y) * 6.0);
    float3 fogZenith = float3(0.30, 0.55, 0.82);
    float3 fogHorizon = float3(0.78, 0.72, 0.60);
    float3 fogColor = lerp(fogZenith, fogHorizon, horizonAmount);
    float distanceFog = 1.0 - exp(-cameraDistance * 0.012);
    float heightFog = exp(-max(input.fragPos.y, 0.0) * 0.035);
    float fogAmount = saturate(distanceFog * lerp(0.45, 1.0, heightFog));
    result = lerp(result, fogColor, fogAmount * 0.72);

    result = tonemapAgXPunchy(max(result, 0.0));
    return float4(result, 1.0);
}
