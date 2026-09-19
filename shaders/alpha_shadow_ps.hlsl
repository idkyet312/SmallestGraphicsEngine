// Alpha-aware depth pass for ordinary imported materials.
//
// MASK materials keep their authored cutoff while the cutout is still resolved.
// Once one shadow texel spans many texture texels the binary test stops
// describing the surface at all: the chain-link fence is 2.2 mm of wire under
// an 11.7 mm level-0 shadow texel, so averaged alpha lands near 0.09 against a
// 0.33 cutoff and the whole panel clips away. Past that point coverage is
// estimated across the pixel's footprint and resolved with the same stable
// screen door the BLEND path uses. The receiver's 3x3 PCF
// (virtual_shadow_sample.hlsli) turns that into grey levels, so the panel
// occludes its real ~11% of light instead of 0% or 100%.
//
// BLEND materials use the screen door directly so their shadows are translucent
// instead of becoming solid blockers.

Texture2D<float4> baseColorTexture : register(t0);
SamplerState baseColorSampler : register(s0);

cbuffer AlphaShadowConstants : register(b8) {
    float opacity;
    float alphaCutoff;
    uint alphaMode;       // 1 = MASK, 2 = BLEND, 3 = luminance MASK
    uint hasTexture;
};

// Mips above this are not trusted. Assets cooked before the mip filter was
// fixed carry coarse levels whose alpha no longer matches the base image: the
// fence atlas holds ~12% of its texels above the cutoff through mip 3, then 4%
// at mip 4, 23% at mip 5 and 100% from mip 6, which no box filter of mip 0 can
// produce. Clamping the fetch to a level that still resembles the base keeps
// coverage honest; the multi-tap below is what makes reading a sharper mip than
// the footprint safe. Relax this once every cutout atlas has been re-cooked.
#define SGE_ALPHA_SHADOW_MAX_LOD 3.0

struct PS_INPUT {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// Shadow-map pixel coordinates are stable while the cascade is stable, so this
// produces fractional coverage without temporal shimmer. One definition shared
// by both paths: a material that changes alpha mode must not change density.
float StableScreenDoor(float2 pixel) {
    return frac(52.9829189 * frac(dot(
        floor(pixel), float2(0.06711056, 0.00583715))));
}

void main(PS_INPUT input) {
    // Mode 4 is the SGE_SHADOW_ALPHA_SOLID diagnostic: keep every fragment, so
    // the caster's silhouette is whatever actually rasterises into the map.
    if (alphaMode == 4) return;

    if (hasTexture == 0) {
        // Constant coverage. Kept in its own branch so no texture fetch runs
        // when the descriptor table holds nothing meaningful.
        const float flat = saturate(opacity);
        clip(alphaMode == 2 ? flat - StableScreenDoor(input.position.xy)
                            : flat - alphaCutoff);
        return;
    }

    // hasTexture and alphaMode are root constants, so every lane in the quad
    // agrees and the gradients below are well defined.
    const float2 duvdx = ddx(input.uv);
    const float2 duvdy = ddy(input.uv);
    const float footprintLod =
        baseColorTexture.CalculateLevelOfDetail(baseColorSampler, input.uv);
    const float lod = clamp(footprintLod, 0.0, SGE_ALPHA_SHADOW_MAX_LOD);

    if (alphaMode == 3) {
        const float3 rgb =
            baseColorTexture.SampleLevel(baseColorSampler, input.uv, lod).rgb;
        clip(max(rgb.r, max(rgb.g, rgb.b)) * opacity - alphaCutoff);
        return;
    }

    if (alphaMode == 2) {
        const float a =
            baseColorTexture.SampleLevel(baseColorSampler, input.uv, lod).a;
        clip(saturate(a * opacity) - StableScreenDoor(input.position.xy));
        return;
    }

    // MASK. Magnified and near-1:1 pixels keep the authored hard edge exactly:
    // the extra taps would all land in one texel and the screen-door weight is
    // zero there, so skip both.
    [branch] if (footprintLod < 1.0) {
        const float a =
            baseColorTexture.SampleLevel(baseColorSampler, input.uv, lod).a;
        clip(saturate(a * opacity) - alphaCutoff);
        return;
    }

    // Rotated-grid taps across the footprint, each one the authored binary
    // test. Their mean is the fraction of the footprint the cutout covers.
    const float2 taps[4] = {
        float2(-0.375, -0.125), float2( 0.125, -0.375),
        float2(-0.125,  0.375), float2( 0.375,  0.125)
    };
    float coverage = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const float2 uv = input.uv + taps[i].x * duvdx + taps[i].y * duvdy;
        const float a =
            baseColorTexture.SampleLevel(baseColorSampler, uv, lod).a;
        coverage += step(alphaCutoff, saturate(a * opacity));
    }
    coverage *= 0.25;

    // Fade from the hard test into the screen door as the footprint grows, so
    // a card does not visibly switch behaviour at one distance.
    const float threshold = lerp(0.5, StableScreenDoor(input.position.xy),
                                 saturate(footprintLod - 1.0));
    clip(coverage - threshold);
}
