#ifndef SGE_VIRTUAL_SHADOW_SAMPLE
#define SGE_VIRTUAL_SHADOW_SAMPLE
// Each bilinear comparison footprint must stay inside its physical page.
// Missing neighbours use the conventional cascades instead of unrelated atlas
// texels. Coordinates are remapped per tap, including across virtual pages.
float VirtualShadowTap(Texture2DArray<float> map, SamplerComparisonState samp,
                       VirtualShadowConstants pages, uint cascade,
                       float2 uv, float depth) {
    if (any(uv < 0.0) || any(uv >= 1.0))
        return map.SampleCmpLevelZero(samp, float3(uv, cascade), depth);
    uint2 page = (uint2)(uv * 16.0);
    uint key = cascade * 256 + page.y * 16 + page.x;
    uint entry = pages.table[key / 4][key % 4];
    float2 local = frac(uv * 16.0) * 1024.0;
    if (entry == 0 || any(local < 0.5) || any(local > 1023.5))
        return map.SampleCmpLevelZero(samp, float3(uv, cascade), depth);
    uint slot = entry - 1;
    float2 atlas = (float2(slot % 4, slot / 4) * 1024.0 + local) / 4096.0;
    return map.SampleCmpLevelZero(samp, float3(atlas, 3), depth);
}

bool VirtualShadowAvailable(Texture2DArray<float> map, VirtualShadowConstants pages) {
    if (pages.config.x == 0) return false;
    uint w, h, layers;
    map.GetDimensions(w, h, layers);
    // Scope and DDGI may still bind the legacy three-slice resource.
    return layers == 4;
}

float VirtualShadowFilter(Texture2DArray<float> map, SamplerComparisonState samp,
                          VirtualShadowConstants pages, uint cascade,
                          float2 uv, float depth) {
    float result = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            result += VirtualShadowTap(map, samp, pages, cascade,
                uv + float2(x, y) * (1.25 / 16384.0), depth);
    return result / 9.0;
}
#endif
