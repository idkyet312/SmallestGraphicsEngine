// Grass pixel shader - a cheap subset of clustered_dx12_ps.hlsl.
//
// Grass blades are untextured, rough (0.85), and cover enormous numbers of
// mostly-edge pixels, so they were paying the full PBR/DDGI/PCF/point-light
// shader for no visible benefit. This keeps exactly the terms that read on a
// thin blade -- Lambert sun, SH sky ambient, one shadow tap, the same aerial
// fog and AgX tonemap so the field stays color-matched to the scene -- and
// drops everything else. Binds a strict subset of the main root signature.

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

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
    float ambientLightingIntensity;
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
    // Grass reuses this slot (unused by the cheap grass path) as the normal
    // falloff strength; see the direct-light block in main().
    float normalFalloff;
    float normalTexH;
};

cbuffer SHBuffer : register(b7) {
    float4 shCoeffs[9];
    float skyIntensity;
    float3 shPadding;
};

cbuffer ShadowCascadeBuffer : register(b8) {
    matrix shadowCascadeMatrices[3];
    float4 shadowCascadeSplits;
    float4 shadowCascadeTexelWorld;
    float4 shadowCascadeDepthRange;
};

Texture2DArray<float> shadowMap : register(t0);
SamplerComparisonState shadowSampler : register(s0);

struct PS_INPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 fragPosLightSpace : TEXCOORD4;
    float  colorVariation : TEXCOORD5;
};

// One comparison tap: the linear-filtered comparison sampler gives hardware
// 2x2 PCF, which is plenty of softness for a 1-2 cm wide blade.
float SampleShadowCascadeCheap(float3 worldPos, float3 normal,
                               float3 lightDir, uint cascade) {
    // Same normal-offset + per-cascade bias as clustered_dx12_ps.hlsl so the
    // field's shadow response matches the turf underneath.
    float ndotl = saturate(dot(normal, lightDir));
    float texelWorld = shadowCascadeTexelWorld[cascade];
    float3 samplePos = worldPos + normal * texelWorld * 1.8;
    float4 fragPosLightSpace = mul(float4(samplePos, 1.0),
                                   shadowCascadeMatrices[cascade]);

    float3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    float2 shadowUV = projCoords.xy * float2(0.5, -0.5) + 0.5;

    if (projCoords.z <= 0.0 || projCoords.z >= 1.0 ||
        shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0) {
        return 1.0;
    }

    float slope = clamp(sqrt(1.0 - ndotl * ndotl) / max(ndotl, 0.1), 0.0, 8.0);
    float bias = texelWorld * (1.0 + 2.0 * slope) /
                 max(shadowCascadeDepthRange[cascade], 1e-3);
    return shadowMap.SampleCmpLevelZero(
        shadowSampler, float3(shadowUV, cascade), projCoords.z - bias);
}

float CalculateShadowCheap(float3 worldPos, float3 normal, float3 lightDir) {
    if (enableShadows == 0) return 1.0;

    float viewDepth = mul(float4(worldPos, 1.0), view).z;
    uint cascade = viewDepth < shadowCascadeSplits.x ? 0u :
                   (viewDepth < shadowCascadeSplits.y ? 1u : 2u);
    float visibility = SampleShadowCascadeCheap(
        worldPos, normal, lightDir, cascade);
    if (cascade < 2u) {
        float nearSplit = cascade == 0u ? 0.0 :
            (cascade == 1u ? shadowCascadeSplits.x : shadowCascadeSplits.y);
        float farSplit = cascade == 0u ? shadowCascadeSplits.x :
            shadowCascadeSplits.y;
        float blend = smoothstep(
            lerp(nearSplit, farSplit, 0.90), farSplit, viewDepth);
        if (blend > 0.0)
            visibility = lerp(visibility, SampleShadowCascadeCheap(
                worldPos, normal, lightDir, cascade + 1u), blend);
    }
    return visibility;
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

#include "color_grade.hlsli"
// AgX (Punchy). Shared with clustered_dx12_ps and sky_ps.
#include "agx_tonemap.hlsli"

float4 main(PS_INPUT input) : SV_TARGET {
    float3 normal = normalize(input.normal);
    float3 viewDir = normalize(viewPos - input.fragPos);
    // Subtle stable blade-to-blade yellow/green and brightness variation.
    // Keep amplitude low to avoid noisy distant grass.
    float3 coolTint = float3(0.90, 1.03, 0.92);
    float3 warmTint = float3(1.06, 1.00, 0.78);
    float variationStrength = max(viewFillStrength, 0.0);
    float3 tint = lerp(
        1.0.xxx,
        lerp(coolTint, warmTint, input.colorVariation),
        variationStrength);
    float brightness = lerp(
        1.0,
        lerp(0.88, 1.10, input.colorVariation),
        variationStrength);
    float3 albedo = saturate(objectColor * tint * brightness);
    // The authored grass albedo is neutral grey (R=G=B), so all of the field's
    // green came from transmissionTint below -- which is gated on backNdotL and
    // therefore only ever reached blades facing AWAY from the sun. That left the
    // sun-facing half rendering the raw grey while the shaded half looked
    // correct. Fold the same leaf tint into the albedo so both sides carry the
    // colour, and let transmission add its warmth on top rather than being the
    // only source of it.
    // Normalised to unit luminance so it recolours without darkening: the raw
    // tint averages well below 1 and would dim the whole field.
    static const float3 kLeafTint = float3(0.72, 0.98, 0.50);
    static const float kLeafTintLuma =
        dot(kLeafTint, float3(0.2126, 0.7152, 0.0722));
    albedo *= kLeafTint / kLeafTintLuma;
    float albedoLuma = dot(albedo, float3(0.2126, 0.7152, 0.0722));
    // Desaturating toward luminance is what greyed the lit side: on the dim half
    // it is invisible, but direct sun amplifies it. Keep the blades' own colour.
    albedo = lerp(albedoLuma.xxx, albedo, 0.95);

    // Blades are two-sided cards; flip the normal to face the camera so the
    // back of a blade doesn't go black.
    if (dot(normal, viewDir) < 0.0) normal = -normal;

    float3 lightDir = lightType == 0
        ? normalize(lightPos)
        : normalize(lightPos - input.fragPos);

    float shadowVisibility = CalculateShadowCheap(input.fragPos, normal, lightDir);

    // Ambient (flat + SH sky), dimmed by shadow the same way the main shader
    // dims its indirect terms.
    float3 result = ambientStrength * albedo * ambientScale;
    result += sampleSkyIrradiance(normal) * albedo * ambientScale;
    result *= lerp(0.28, 1.0, shadowVisibility) *
              ambientLightingIntensity;

    // Thin vegetation is not an opaque card. Wrap direct light around both sides
    // and transmit warm sunlight through back-facing blades. This removes the
    // near-black foreground silhouettes while retaining object/terrain shadows.
    // normalFalloff scales how much a blade's orientation darkens it.
    //
    // At 1 this is the physical response: the wrapped term reaches zero once a
    // blade turns 0.32 away from the light, so away-facing patches drop to
    // ambient and the field splits into a bright half and a dark half. At 0
    // every blade is lit as though it faced the sun, which removes the split
    // entirely but is not physical. Values between fade one into the other, so
    // the split can be softened without flattening the field completely.
    float falloff = saturate(normalFalloff);
    float signedNdotL = dot(normal, lightDir);
    float wrappedNdotL = lerp(1.0, saturate((signedNdotL + 0.32) / 1.32),
                              falloff);
    float backNdotL = lerp(1.0, saturate(-signedNdotL), falloff);
    result += albedo / 3.14159265 * lightColor *
              wrappedNdotL * shadowVisibility *
              max(occlusionStrength, 0.0);

    // Broad low-energy leaf sheen. Roughness now remains useful on the cheap
    // grass shader without turning blades into metallic highlights.
    float3 halfDir = normalize(lightDir + viewDir);
    float specularPower = lerp(96.0, 10.0, saturate(roughness));
    // Blend toward abs() as falloff drops so the sheen stops being confined to
    // the half of the field turned toward the sun.
    float rawAlignment = dot(normal, halfDir);
    float specularAlignment = lerp(abs(rawAlignment), saturate(rawAlignment),
                                   falloff);
    float specularLobe = pow(specularAlignment, specularPower);
    float specularEnergy = (1.0 - saturate(roughness)) * 0.08;
    result += lightColor * specularLobe * specularEnergy *
              shadowVisibility * max(occlusionStrength, 0.0);

    float tipTransmission = lerp(0.55, 1.0, smoothstep(0.15, 0.92,
                                                        input.texCoord.y));
    // albedo already carries kLeafTint, so tinting again here would push the
    // back-lit half doubly green against the front.
    result += albedo * lightColor *
              backNdotL * tipTransmission * max(normalYSign, 0.0) *
              lerp(0.35, 1.0, shadowVisibility);

#ifdef SGE_HDR_TARGET
    result = max(result, 0.0);
#else
    result = tonemapAgXPunchy(max(result, 0.0));
#endif
    return float4(result, 1.0);
}
