#ifndef FOLIAGE_BRDF_HLSLI
#define FOLIAGE_BRDF_HLSLI

float FoliageWrappedDiffuse(float signedNdotL)
{
    return saturate((signedNdotL + 0.14) / 1.14);
}

float3 EvaluateFoliageTransmission(
    float3 albedo, float3 normal, float3 viewDir, float3 lightDir,
    float3 lightRadiance, float alphaCoverage, float attenuation,
    float shadowVisibility)
{
    const float backLight = saturate(dot(-normal, lightDir));
    const float forwardScatter =
        pow(saturate(dot(lightDir, -viewDir)), 4.0);
    // Mip-filtered card borders are geometrically thin but must not become
    // emissive outlines. Ramp transmission in only after stable leaf coverage.
    const float coverage = smoothstep(0.24, 0.72, alphaCoverage);
    const float3 chlorophyllTint = float3(0.58, 0.92, 0.32);
    const float scatter = backLight * 0.48 + forwardScatter * 0.13;
    return albedo * chlorophyllTint * lightRadiance * scatter * coverage *
           attenuation * lerp(0.14, 1.0, shadowVisibility);
}

float3 EvaluateFoliageSkyScatter(
    float3 albedo, float3 frontSky, float3 backSky,
    float ambientLightingIntensity)
{
    return albedo * (frontSky + backSky) * 0.24 *
           ambientLightingIntensity;
}

#endif
