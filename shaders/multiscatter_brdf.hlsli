#ifndef SGE_MULTISCATTER_BRDF_HLSLI
#define SGE_MULTISCATTER_BRDF_HLSLI

// Multiple-scattering energy compensation for GGX (Fdez-Aguera 2019).
//
// The split-sum specular term every path here uses integrates only ONE bounce
// off the microfacet surface. Real microfacets bounce light repeatedly inside
// their own grooves, and each of those extra bounces eventually leaves the
// surface. Dropping them loses energy, and the loss scales with roughness:
// smooth surfaces are nearly correct, while a rough metal keeps well under half
// the light it should. That deficit is exactly the "dark, chalky, plasticky"
// look on rough metals -- the material is not mis-authored, the BRDF is
// literally not returning the energy that went in.
//
// The correction is derived from the SAME preintegrated (scale, bias) LUT the
// single-scatter term already samples, so it costs no extra texture, no extra
// LUT channel, and no change to the LUT generator. Ess = scale + bias is the
// directional albedo of the single-scatter lobe: the fraction of incoming
// energy that survives one bounce. 1 - Ess is therefore the fraction that was
// dropped, and it is redistributed as a second, F0-tinted lobe -- tinted
// because each additional bounce is another Fresnel reflection, so multiply
// scattered light takes on the metal's colour rather than staying white.
//
// Energy conserving, never energy creating: the added term goes to zero as
// Ess -> 1 (smooth surfaces are untouched) and its total can never exceed the
// energy the single-scatter lobe failed to deliver.

// Multiplier applied to a single-scatter specular result to restore the
// energy lost to multiple bounces.
//
//   f0             specular reflectance at normal incidence
//   environmentBRDF  the (scale, bias) pair sampled from brdfIntegrationLUT
//
// Returns a per-channel gain >= 1. Because it is a plain multiplier it applies
// equally to analytic direct light and to prefiltered IBL, which keeps the two
// consistent -- compensating only one of them makes lit and reflected sides of
// the same object disagree about how bright the material is.
float3 MultiScatterEnergyCompensation(float3 f0, float2 environmentBRDF) {
    // Directional albedo of the single-scatter lobe for this view angle and
    // roughness. Saturated because an importance-sampled LUT can land a hair
    // above 1 at grazing angles, which would otherwise flip the correction
    // negative and punch black pixels along silhouettes.
    float singleScatterAlbedo = saturate(environmentBRDF.x + environmentBRDF.y);
    // Average Fresnel over the hemisphere. f0 is the standard approximation and
    // is what keeps the compensation tinted by the metal's own colour.
    float3 averageFresnel = f0;
    // Geometric series over the infinite remaining bounces: each one survives
    // with probability Ess and is tinted by F_avg again. Denominator is bounded
    // away from zero for any physical f0 (< 1), and clamped for safety against
    // authored albedo that reaches exactly 1.
    float3 multiScatterGain =
        (1.0 - singleScatterAlbedo) * averageFresnel /
        max(1.0 - averageFresnel * (1.0 - singleScatterAlbedo), 1e-4);
    return 1.0 + multiScatterGain;
}

#endif  // SGE_MULTISCATTER_BRDF_HLSLI
