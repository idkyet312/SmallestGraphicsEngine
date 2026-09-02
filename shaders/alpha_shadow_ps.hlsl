// Alpha-aware depth pass for ordinary imported materials. MASK materials use
// their authored cutoff; BLEND materials use stable screen-door coverage so
// their shadows are translucent instead of becoming solid blockers.

Texture2D<float4> baseColorTexture : register(t0);
SamplerState baseColorSampler : register(s0);

cbuffer AlphaShadowConstants : register(b8) {
    float opacity;
    float alphaCutoff;
    uint alphaMode;       // 1 = MASK, 2 = BLEND, 3 = luminance MASK
    uint hasTexture;
};

struct PS_INPUT {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

void main(PS_INPUT input) {
    const float4 texel = hasTexture != 0
        ? baseColorTexture.Sample(baseColorSampler, input.uv)
        : 1.0.xxxx;

    if (alphaMode == 3) {
        clip(max(texel.r, max(texel.g, texel.b)) * opacity - alphaCutoff);
        return;
    }

    const float coverage = saturate(texel.a * opacity);
    if (alphaMode == 1) {
        clip(coverage - alphaCutoff);
        return;
    }

    // Shadow-map pixel coordinates are stable while the cascade is stable, so
    // this produces fractional coverage without temporal shimmer.
    const float threshold = frac(52.9829189 * frac(dot(
        floor(input.position.xy), float2(0.06711056, 0.00583715))));
    clip(coverage - threshold);
}
