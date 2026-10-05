#include "terrain_height.hlsli"
#include "terrain_visibility_id.hlsli"

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};
cbuffer CameraBuffer : register(b2) {
    float3 viewPos;
    float cameraPadding;
};

Texture2DArray<float4> albedoMap : register(t1);
Texture2DArray<float4> normalMap : register(t4);
Texture2DArray<float4> metalRoughMap : register(t5);
// The terrain table's fourth slot is otherwise unused (mesh emissive, t22).
Texture2D<float4> terrainSplatMap : register(t22);
SamplerState texSampler : register(s1);

#define materialType (pomShowAuthoredPaths != 0u ? 3.0f : 0.0f)
#define materialTime (1u | (pomNeutralHeightBlendMask << 1u))
#define normalYSign (-1.0)
#define terrainSplatEnabled pomSplatEnabled
#define terrainSplatInvExtent (0.5 / (88.0 * float2(islandScaleX, islandScaleZ)))
#define terrainSplatSampler texSampler
#define SGE_TERRAIN_FORWARD_SPLAT 1
#define SGE_TERRAIN_SPLAT_WRAP_SAMPLER 1

// Same noise as the forward shader and visibility resolve. Material boundaries
// must agree before the raster pass chooses the blended relief depth.
uint MatVarHashUint(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}
float MatVarHash(int3 cell) {
    uint value = MatVarHashUint(asuint(cell.x) ^
        MatVarHashUint(asuint(cell.y) + MatVarHashUint(asuint(cell.z))) +
        0x9e3779b9u);
    return float(value & 0x00ffffffu) * (1.0 / 16777216.0);
}
float MatVarNoise(float3 position) {
    int3 cell = (int3)floor(position);
    float3 blend = frac(position);
    blend = blend * blend * (3.0 - 2.0 * blend);
    float n00 = lerp(MatVarHash(cell), MatVarHash(cell + int3(1, 0, 0)), blend.x);
    float n10 = lerp(MatVarHash(cell + int3(0, 1, 0)),
                     MatVarHash(cell + int3(1, 1, 0)), blend.x);
    float n01 = lerp(MatVarHash(cell + int3(0, 0, 1)),
                     MatVarHash(cell + int3(1, 0, 1)), blend.x);
    float n11 = lerp(MatVarHash(cell + int3(0, 1, 1)),
                     MatVarHash(cell + int3(1, 1, 1)), blend.x);
    return lerp(lerp(n00, n10, blend.y), lerp(n01, n11, blend.y), blend.z);
}

#include "terrain_pbr.hlsli"

// Only packed height is needed here. Albedo, normals and lighting stay in the
// compute resolve, which runs once per visible pixel after depth testing.
float3 TerrainVisibilityPOMHit(float3 worldPos, float3 geometricNormal) {
    const float cameraDistance = length(viewPos - worldPos);
    const float3 viewDirection = (viewPos - worldPos) / max(cameraDistance, 1e-4);
    const float3 projectionWeights = TerrainProjectionWeights(geometricNormal);
    const float4 layerWeights = TerrainLayerWeights(worldPos, geometricNormal);
    const float scales[4] = { 0.16667, 0.4831, 0.07500, 0.4130 };
    // Evaluate derivatives before any distance/layer divergence in the quad.
    TriplanarGrads grads[4];
    [unroll] for (uint i = 0; i < 4; ++i)
        grads[i] = TerrainTriplanarGrads(worldPos, scales[i]);
    if (cameraDistance >= 28.0) return worldPos;

    float3 layerPositions[4] = { worldPos, worldPos, worldPos, worldPos };
    float4 layerHeights = 0.0;
    [unroll] for (uint layer = 0; layer < 4; ++layer) {
        if (layerWeights[layer] <= 0.002) continue;
        layerPositions[layer] = TerrainPOMPosition(
            metalRoughMap, texSampler, worldPos, geometricNormal,
            projectionWeights, layer, layerWeights[layer], scales[layer],
            grads[layer].zyDx, grads[layer].zyDy, grads[layer].xzDx,
            grads[layer].xzDy, grads[layer].xyDx, grads[layer].xyDy,
            viewDirection, cameraDistance);
        const float sampledHeight = SampleTerrainArray(metalRoughMap,
            layerPositions[layer], projectionWeights, layer, scales[layer],
            grads[layer]).a;
        const float blendHeight = TerrainPOMBlendHeight(sampledHeight, layer,
            pomNeutralHeightBlendMask, projectionWeights);
        if (layer == 0) layerHeights.x = blendHeight;
        else if (layer == 1) layerHeights.y = blendHeight;
        else if (layer == 2) layerHeights.z = blendHeight;
        else layerHeights.w = blendHeight;
    }
    const float4 blendWeights = TerrainHeightBlend(layerWeights, layerHeights);
    float3 hit = worldPos;
    [unroll] for (uint hitLayer = 0; hitLayer < 4; ++hitLayer)
        hit += (layerPositions[hitLayer] - worldPos) * blendWeights[hitLayer];
    return hit;
}
