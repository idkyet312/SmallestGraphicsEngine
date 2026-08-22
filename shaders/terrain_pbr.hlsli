#ifndef TERRAIN_PBR_HLSLI
#define TERRAIN_PBR_HLSLI

// Array slices: grass, dirt, sand, rock. World-space triplanar projection avoids
// stretching on cliffs and needs no terrain UV seams.
struct TerrainPBR {
    float3 albedo;
    float3 normal;
    float roughness;
    float metallic;
    float occlusion;
};

float TerrainBlendNoise(float2 p) {
    float low = MatVarNoise(float3(p * 0.075, 3.17));
    float high = MatVarNoise(float3(p * 0.23, 11.4));
    return low * 0.72 + high * 0.28;
}

float4 TerrainLayerWeights(float3 worldPos, float3 geometricNormal) {
    const float slope = 1.0 - saturate(abs(geometricNormal.y));
    const float noise = TerrainBlendNoise(worldPos.xz);
    const float noisyHeight = worldPos.y + (noise - 0.5) * 1.5;

    float rock = smoothstep(0.30, 0.68, slope + (noise - 0.5) * 0.12);
    const float flat = 1.0 - smoothstep(0.18, 0.52, slope);
    // The low beach shelf is a complete sand material. World height guarantees
    // a solid coastal band; noisy height only breaks up its inland transition.
    const float beachCore =
        (1.0 - smoothstep(0.80, 1.20, worldPos.y)) * flat;
    const float beachTransition =
        (1.0 - smoothstep(0.75, 2.25, noisyHeight)) * flat;
    float sand = max(beachCore, beachTransition);
    // Most playable island ground, including 2.5 m house pads, is grass.
    // Dirt is a broken-up secondary patch layer instead of a height band that
    // previously swallowed the downloaded grass texture across the whole level.
    float grass = smoothstep(1.45, 2.35, noisyHeight) * flat;
    float dirt = 0.08 + smoothstep(0.58, 0.79, noise) * 0.40 * flat +
                 smoothstep(0.12, 0.46, slope) * 0.32;

    // Default compound has four deliberate footpaths between central clearing
    // and house entrances. materialType=3 is set only for built-in levels, so
    // custom maps keep full control over their ground composition.
    if (fmod(materialType, 4.0) > 2.5) {
        float axisDistance = min(abs(worldPos.x), abs(worldPos.z));
        float pathReach = max(abs(worldPos.x), abs(worldPos.z));
        float path = (1.0 - smoothstep(0.72, 1.28, axisDistance)) *
                     (1.0 - smoothstep(13.2, 15.5, pathReach));
        path *= 0.82 + TerrainBlendNoise(worldPos.xz * 1.8 + 29.0) * 0.18;
        grass *= 1.0 - path * 0.92;
        dirt += path * 2.6;
    }

    sand *= 1.0 - rock;
    dirt *= (1.0 - rock) * (1.0 - sand);
    grass *= (1.0 - rock) * (1.0 - sand);
    float4 weights = float4(grass, dirt, sand, rock);
    weights += 0.0001;
    // Height/slope masks should create readable material regions, not an even
    // four-way soup. Mild contrast preserves soft natural boundaries while each
    // scan keeps its authored colour and normal response.
    weights = pow(weights, 1.35);
    return weights / dot(weights, 1.0);
}

float3 TerrainProjectionWeights(float3 normal) {
    float3 weights = pow(abs(normal), 5.0);
    return weights / max(dot(weights, 1.0), 1e-4);
}

// Triplanar axes whose weight rounds to nothing are skipped rather than sampled
// and multiplied by ~0. TerrainProjectionWeights raises |normal| to the 5th
// power and normalises, so on ground that is anywhere near flat the Y weight is
// ~1 and X/Z collapse to ~0 -- two thirds of these fetches were feeding a
// multiply by zero. The threshold is well below what an 8-bit texture can
// resolve, so the output is unchanged.
static const float kTriplanarEpsilon = 0.002;

// Derivatives must be computed in uniform control flow: Sample() derives its mip
// from neighbouring lanes in the 2x2 quad, and a quad that diverges at the
// branches below would produce undefined LOD and visible seams. ddx/ddy are
// therefore taken up front, unconditionally, and the fetches use SampleGrad.
struct TriplanarGrads {
    float2 zyDx; float2 zyDy;
    float2 xzDx; float2 xzDy;
    float2 xyDx; float2 xyDy;
};

TriplanarGrads TerrainTriplanarGrads(float3 worldPos, float scale) {
    TriplanarGrads g;
    g.zyDx = ddx(worldPos.zy * scale); g.zyDy = ddy(worldPos.zy * scale);
    g.xzDx = ddx(worldPos.xz * scale); g.xzDy = ddy(worldPos.xz * scale);
    g.xyDx = ddx(worldPos.xy * scale); g.xyDy = ddy(worldPos.xy * scale);
    return g;
}

float4 SampleTerrainArray(Texture2DArray map, float3 worldPos,
                          float3 projectionWeights, float layer, float scale,
                          TriplanarGrads g) {
    float4 result = 0.0;
    if (projectionWeights.x > kTriplanarEpsilon)
        result += map.SampleGrad(texSampler, float3(worldPos.zy * scale, layer),
                                 g.zyDx, g.zyDy) * projectionWeights.x;
    if (projectionWeights.y > kTriplanarEpsilon)
        result += map.SampleGrad(texSampler, float3(worldPos.xz * scale, layer),
                                 g.xzDx, g.xzDy) * projectionWeights.y;
    if (projectionWeights.z > kTriplanarEpsilon)
        result += map.SampleGrad(texSampler, float3(worldPos.xy * scale, layer),
                                 g.xyDx, g.xyDy) * projectionWeights.z;
    return result;
}

float3 SampleTerrainNormalLayer(float3 worldPos, float3 geometricNormal,
                                float3 projectionWeights, float layer,
                                float scale, float strength,
                                TriplanarGrads g) {
    // Same zero-weight axis skip as SampleTerrainArray. Each axis is still
    // decoded, strength-scaled and normalised exactly as before being weighted,
    // so a contributing axis produces bit-identical output; only axes that were
    // being multiplied by ~0 are dropped.
    float sx = geometricNormal.x < 0.0 ? -1.0 : 1.0;
    float sy = geometricNormal.y < 0.0 ? -1.0 : 1.0;
    float sz = geometricNormal.z < 0.0 ? -1.0 : 1.0;
    float3 blended = 0.0;

    if (projectionWeights.x > kTriplanarEpsilon) {
        float3 nx = normalMap.SampleGrad(texSampler,
            float3(worldPos.zy * scale, layer), g.zyDx, g.zyDy).xyz * 2.0 - 1.0;
        nx.y *= normalYSign;
        nx.xy *= strength;
        nx = normalize(nx);
        blended += normalize(float3(nx.z * sx, nx.y, nx.x)) *
                   projectionWeights.x;
    }
    if (projectionWeights.y > kTriplanarEpsilon) {
        float3 ny = normalMap.SampleGrad(texSampler,
            float3(worldPos.xz * scale, layer), g.xzDx, g.xzDy).xyz * 2.0 - 1.0;
        ny.y *= normalYSign;
        ny.xy *= strength;
        ny = normalize(ny);
        blended += normalize(float3(ny.x, ny.z * sy, ny.y)) *
                   projectionWeights.y;
    }
    if (projectionWeights.z > kTriplanarEpsilon) {
        float3 nz = normalMap.SampleGrad(texSampler,
            float3(worldPos.xy * scale, layer), g.xyDx, g.xyDy).xyz * 2.0 - 1.0;
        nz.y *= normalYSign;
        nz.xy *= strength;
        nz = normalize(nz);
        blended += normalize(float3(nz.x, nz.y, nz.z * sz)) *
                   projectionWeights.z;
    }
    return normalize(blended);
}

TerrainPBR SampleTerrainPBR(float3 worldPos, float3 geometricNormal,
                            float cameraDistance) {
    TerrainPBR result;
    const float3 projectionWeights = TerrainProjectionWeights(geometricNormal);
    const float4 layerWeights = TerrainLayerWeights(worldPos, geometricNormal);
    // The 2K sand scan repeats every 13.3 m. Its old 20-30 m projection enlarged
    // individual ripples into soft dunes and hid most of the added resolution.
    const float scales[4] = { 0.16667, 0.4831, 0.07500, 0.4130 };
    // Preserve fine leafy-grass relief. Other broad terrain layers stay softer
    // to avoid noisy distant slopes.
    const float normalStrengths[4] = { 1.15, 0.72, 0.64, 0.92 };

    result.albedo = 0.0;
    result.normal = 0.0;
    result.roughness = 0.0;
    result.metallic = 0.0;
    result.occlusion = 0.0;
    // Layers are weighted by height/slope and normalised, so at any given pixel
    // typically only one or two of the four contribute. Sampling all four cost
    // 4 layers x 3 maps x 3 triplanar axes = 36 fetches per pixel regardless of
    // weight, which dominated the frame on camera angles filled by terrain.
    // Skipping a layer whose weight is below 8-bit resolution leaves the blend
    // visually identical.
    const float kLayerEpsilon = 0.002;
    [unroll] for (uint layer = 0; layer < 4; ++layer) {
        const float weight = layerWeights[layer];
        // Gradients are taken before the weight test so every lane in the quad
        // evaluates them, keeping the derivative uniform even when neighbouring
        // pixels skip different layers.
        const TriplanarGrads grads =
            TerrainTriplanarGrads(worldPos, scales[layer]);
        if (weight <= kLayerEpsilon) continue;
        result.albedo += SampleTerrainArray(
            albedoMap, worldPos, projectionWeights, layer, scales[layer],
            grads).rgb * weight;
        result.normal += SampleTerrainNormalLayer(
            worldPos, geometricNormal, projectionWeights, layer,
            scales[layer], normalStrengths[layer], grads) * weight;
        const float4 packedPBR = SampleTerrainArray(
            metalRoughMap, worldPos, projectionWeights, layer,
            scales[layer], grads);
        result.roughness += packedPBR.g * weight;
        result.metallic += packedPBR.b * weight;
        result.occlusion += packedPBR.r * weight;
    }

    // Broad, non-repeating biome modulation breaks the obvious scan tiling. Cool
    // sheltered patches and dry sun-facing patches vary over tens of metres, well
    // above the texture frequency. Grass gets a slight green bias so blade roots
    // blend into the turf instead of sitting over pale ground.
    const float macroA = TerrainBlendNoise(worldPos.xz * 0.18 + 17.0);
    const float macroB = MatVarNoise(float3(
        worldPos.xz * 0.018 + float2(31.0, -19.0), 6.7));
    const float3 coolMacro = float3(0.86, 0.96, 0.84);
    const float3 warmMacro = float3(1.08, 1.01, 0.88);
    result.albedo *= lerp(coolMacro, warmMacro,
                          saturate(macroA * 0.58 + macroB * 0.42));
    result.albedo *= lerp(1.0.xxx, float3(0.88, 1.04, 0.78),
                          layerWeights.x * 0.34);

    // Damp sand around the waterline. Darker, smoother wet sand gives the coast
    // a readable transition before the ocean/foam pass.
    const float wetSand = layerWeights.z *
        (1.0 - smoothstep(0.05, 0.45, worldPos.y));
    result.albedo *= lerp(1.0, 0.76, wetSand);
    // Invalid/unbound SRVs return zero. Keep terrain readable and make binding
    // faults obvious as flat material colors instead of an all-black island.
    const float3 fallbackColors[4] = {
        float3(0.25, 0.43, 0.12),
        float3(0.34, 0.20, 0.10),
        float3(0.72, 0.58, 0.36),
        float3(0.31, 0.32, 0.30)
    };
    float3 fallbackAlbedo = 0.0;
    [unroll] for (uint fallbackLayer = 0; fallbackLayer < 4; ++fallbackLayer)
        fallbackAlbedo += fallbackColors[fallbackLayer] * layerWeights[fallbackLayer];
    if (dot(result.albedo, float3(0.2126, 0.7152, 0.0722)) < 0.002)
        result.albedo = fallbackAlbedo;
    // Darken only turf-covered terrain so it sits beneath the grass blades.
    // Weighting preserves seamless transitions into dirt, sand, and rock.
    result.albedo *= lerp(1.0, 0.88, layerWeights.x);
    // Close-range detail: a high-frequency albedo modulation + normal
    // perturbation that fades out with distance. Breaks up the tiling repeat and
    // adds crispness underfoot without new textures. Fades to nothing by ~40 m
    // so distant coarse-ring ground stays clean and cheap.
    const float detailFade = 1.0 - smoothstep(10.0, 40.0, cameraDistance);
    if (detailFade > 0.001) {
        const float d1 = MatVarNoise(float3(worldPos.xz * 1.7, 5.0));
        const float d2 = MatVarNoise(float3(worldPos.xz * 4.3, 9.0));
        const float detail = (d1 * 0.6 + d2 * 0.4) - 0.5;
        // Subtle albedo contrast + a tiny normal wobble for micro-relief.
        result.albedo *= 1.0 + detail * 0.13 * detailFade;
        result.normal.xz += float2(
            MatVarNoise(float3(worldPos.xz * 3.1 + 2.0, 1.0)) - 0.5,
            MatVarNoise(float3(worldPos.zx * 3.1 + 7.0, 1.0)) - 0.5) *
            0.26 * detailFade;
        result.normal = normalize(result.normal);
    }

    // Macro normal perturbation. The close-range wobble above is gone by 40 m,
    // so mid and far ground lights as one uniform slope. This varies over tens
    // of metres -- well below the texture frequency, above the detail fade --
    // and holds out to the horizon, which is what gives distant terrain shape.
    if (materialType >= 3.5) {
        const float m1 = MatVarNoise(float3(worldPos.xz * 0.055 + 41.0, 2.3));
        const float m2 = MatVarNoise(float3(worldPos.zx * 0.021 - 13.0, 8.1));
        result.normal.xz += float2(m1 - 0.5, m2 - 0.5) * 0.34;
        result.normal = normalize(result.normal);
    }

    const float grassResponse = layerWeights.x;
    const float normalDistanceFade =
        1.0 - smoothstep(24.0, 90.0, cameraDistance);
    const float nearNormalStrength = lerp(0.62, 0.92, grassResponse);
    result.normal = normalize(lerp(
        geometricNormal, normalize(result.normal),
        lerp(0.26, nearNormalStrength, normalDistanceFade)));
    // Natural ground is a dielectric. Some downloaded packed scans contain
    // non-zero blue channels or over-smooth roughness values that turn broad
    // terrain patches into polished metal under HDR sun/IBL. Preserve authored
    // micro-variation above physically plausible layer floors, then allow only
    // the waterline sand to become moderately smoother.
    const float4 roughnessFloors = float4(0.84, 0.89, 0.76, 0.82);
    float layerRoughness = dot(layerWeights, roughnessFloors);
    layerRoughness += (macroA - 0.5) * 0.055;
    const float dryRoughness =
        max(saturate(result.roughness), layerRoughness);
    const float wetRoughness = 0.58 + (macroB - 0.5) * 0.04;
    result.roughness = clamp(
        lerp(dryRoughness, wetRoughness, wetSand * 0.72), 0.54, 1.0);
    result.metallic = 0.0;
    result.occlusion = saturate(result.occlusion);
    // The aerial sand AO is strong enough to turn ripple troughs nearly black
    // under the restored high-contrast lighting. Retain its relief without
    // double-darkening it through both albedo and ambient occlusion.
    result.occlusion = lerp(
        result.occlusion, max(result.occlusion, 0.68),
        layerWeights.z * 0.82);
    return result;
}

#endif
