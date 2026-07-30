#ifndef FOLIAGE_BRDF_HLSLI
#define FOLIAGE_BRDF_HLSLI

float FoliageWrappedDiffuse(float signedNdotL)
{
    return saturate((signedNdotL + 0.08) / 1.08);
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
    const float3 chlorophyllTint = float3(0.54, 0.82, 0.29);
    const float scatter = backLight * 0.34 + forwardScatter * 0.08;
    return albedo * chlorophyllTint * lightRadiance * scatter * coverage *
           attenuation * lerp(0.38, 1.0, shadowVisibility);
}

float3 EvaluateFoliageSkyScatter(
    float3 albedo, float3 frontSky, float3 backSky,
    float ambientLightingIntensity)
{
    return albedo * (frontSky + backSky) * 0.18 *
           ambientLightingIntensity;
}

#endif
