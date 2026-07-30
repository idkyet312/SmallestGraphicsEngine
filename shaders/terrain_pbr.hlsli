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
    float sand = (1.0 - smoothstep(0.65, 2.35, noisyHeight)) *
                 (1.0 - smoothstep(0.20, 0.52, slope));
    const float flat = 1.0 - smoothstep(0.18, 0.52, slope);
    // Most playable island ground, including 2.5 m house pads, is grass.
    // Dirt is a broken-up secondary patch layer instead of a height band that
    // previously swallowed the downloaded grass texture across the whole level.
    float grass = smoothstep(1.45, 2.35, noisyHeight) * flat;
    float dirt = 0.08 + smoothstep(0.58, 0.79, noise) * 0.40 * flat +
                 smoothstep(0.12, 0.46, slope) * 0.32;

    // Default compound has four deliberate footpaths between central clearing
    // and house entrances. materialType=3 is set only for built-in levels, so
    // custom maps keep full control over their ground composition.
    if (materialType > 2.5) {
        float axisDistance = min(abs(worldPos.x), abs(worldPos.z));
        float pathReach = max(abs(worldPos.x), abs(worldPos.z));
        float path = (1.0 - smoothstep(0.72, 1.28, axisDistance)) *
                     (1.0 - smoothstep(13.2, 15.5, pathReach));
        path *= 0.82 + TerrainBlendNoise(worldPos.xz * 1.8 + 29.0) * 0.18;
        grass *= 1.0 - path * 0.92;
        dirt += path * 2.6;
    }

    sand *= 1.0 - rock;
    dirt *= (1.0 - rock) * (1.0 - sand * 0.75);
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

float4 SampleTerrainArray(Texture2DArray map, float3 worldPos,
                          float3 projectionWeights, float layer, float scale) {
    float4 x = map.Sample(texSampler, float3(worldPos.zy * scale, layer));
    float4 y = map.Sample(texSampler, float3(worldPos.xz * scale, layer));
    float4 z = map.Sample(texSampler, float3(worldPos.xy * scale, layer));
    return x * projectionWeights.x + y * projectionWeights.y +
           z * projectionWeights.z;
}

float3 SampleTerrainNormalLayer(float3 worldPos, float3 geometricNormal,
                                float3 projectionWeights, float layer,
                                float scale, float strength) {
    float3 nx = normalMap.Sample(texSampler,
        float3(worldPos.zy * scale, layer)).xyz * 2.0 - 1.0;
    float3 ny = normalMap.Sample(texSampler,
        float3(worldPos.xz * scale, layer)).xyz * 2.0 - 1.0;
    float3 nz = normalMap.Sample(texSampler,
        float3(worldPos.xy * scale, layer)).xyz * 2.0 - 1.0;
    nx.y *= normalYSign;
    ny.y *= normalYSign;
    nz.y *= normalYSign;
    nx.xy *= strength;
    ny.xy *= strength;
    nz.xy *= strength;
    nx = normalize(nx);
    ny = normalize(ny);
    nz = normalize(nz);

    float sx = geometricNormal.x < 0.0 ? -1.0 : 1.0;
    float sy = geometricNormal.y < 0.0 ? -1.0 : 1.0;
    float sz = geometricNormal.z < 0.0 ? -1.0 : 1.0;
    float3 wx = normalize(float3(nx.z * sx, nx.y, nx.x));
    float3 wy = normalize(float3(ny.x, ny.z * sy, ny.y));
    float3 wz = normalize(float3(nz.x, nz.y, nz.z * sz));
    return normalize(wx * projectionWeights.x + wy * projectionWeights.y +
                     wz * projectionWeights.z);
}

TerrainPBR SampleTerrainPBR(float3 worldPos, float3 geometricNormal,
                            float cameraDistance) {
    TerrainPBR result;
    const float3 projectionWeights = TerrainProjectionWeights(geometricNormal);
    const float4 layerWeights = TerrainLayerWeights(worldPos, geometricNormal);
    // Grass004 uses one third of the original UV tiling, making each visible
    // texture tile three times larger. Other layers retain scan dimensions.
    const float scales[4] = { 0.16667, 0.4831, 0.06667, 0.4130 };
    // Preserve fine leafy-grass relief. Other broad terrain layers stay softer
    // to avoid noisy distant slopes.
    const float normalStrengths[4] = { 1.15, 0.72, 0.55, 0.92 };

    result.albedo = 0.0;
    result.normal = 0.0;
    result.roughness = 0.0;
    result.metallic = 0.0;
    result.occlusion = 0.0;
    [unroll] for (uint layer = 0; layer < 4; ++layer) {
        const float weight = layerWeights[layer];
        result.albedo += SampleTerrainArray(
            albedoMap, worldPos, projectionWeights, layer, scales[layer]).rgb * weight;
        result.normal += SampleTerrainNormalLayer(
            worldPos, geometricNormal, projectionWeights, layer,
            scales[layer], normalStrengths[layer]) * weight;
        const float4 packedPBR = SampleTerrainArray(
            metalRoughMap, worldPos, projectionWeights, layer,
            scales[layer]);
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
        (1.0 - smoothstep(0.55, 1.55, worldPos.y));
    result.albedo *= lerp(1.0, 0.68, wetSand);
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
    return result;
}

#endif
