#ifndef SGE_TERRAIN_POM_HLSLI
#define SGE_TERRAIN_POM_HLSLI

// Newly supplied dirt/sand displacement is for POM. Preserve their neutral
// height-blend contribution so existing levels keep the same material regions.
float TerrainPOMBlendHeight(float sampledHeight, uint layer, uint neutralMask,
                           float3 projectionWeights) {
    if ((neutralMask & (1u << layer)) == 0) return sampledHeight;
    // Match the original triplanar accumulation, including skipped axes.
    float height = 0.0;
    if (projectionWeights.x > 0.002) height += (128.0 / 255.0) * projectionWeights.x;
    if (projectionWeights.y > 0.002) height += (128.0 / 255.0) * projectionWeights.y;
    if (projectionWeights.z > 0.002) height += (128.0 / 255.0) * projectionWeights.z;
    return height;
}

// Height shares the linear packed PBR array's alpha channel. Keep the original
// footprint throughout the march: displaced derivatives shimmer at occlusion
// boundaries and implicit derivatives are invalid inside divergent loops.
float TerrainPOMTrace(Texture2DArray<float4> packedMap, SamplerState samplerState,
                      float2 uv, float layer, float2 dx, float2 dy,
                      float2 ray, float viewNormal) {
    const uint steps = (uint)lerp(8.0, 4.0, saturate(viewNormal));
    const float stepDepth = 1.0 / steps;
    // Missing displacement maps are packed as 128/255. Centre relief around
    // that plane so a neutral height retains the original coordinates.
    const float neutralDepth = 1.0 - 128.0 / 255.0;
    const float2 startUV = uv + ray * neutralDepth;
    float depth = 0.0;
    float surfaceDepth = 1.0 - packedMap.SampleGrad(samplerState,
        float3(startUV, layer), dx, dy).a;
    float previousGap = -surfaceDepth;
    [loop] for (uint step = 0; step < steps; ++step) {
        if (depth >= surfaceDepth) break;
        previousGap = depth - surfaceDepth;
        depth += stepDepth;
        surfaceDepth = 1.0 - packedMap.SampleGrad(samplerState,
            float3(startUV - ray * depth, layer), dx, dy).a;
    }
    if (depth <= 0.0) return neutralDepth;
    const float gap = depth - surfaceDepth;
    const float intersection = saturate(-previousGap / max(gap - previousGap, 1e-5));
    const float hitDepth = depth - stepDepth + intersection * stepDepth;
    return neutralDepth - hitDepth;
}

float3 TerrainPOMPosition(Texture2DArray<float4> packedMap,
    SamplerState samplerState, float3 worldPos, float3 normal,
    float3 projectionWeights, float layer, float layerWeight, float scale,
    float2 zyDx, float2 zyDy, float2 xzDx, float2 xzDy,
    float2 xyDx, float2 xyDy, float3 viewDirection, float cameraDistance) {
    // One march per contributing material, rather than three. Fade where no
    // projection dominates so changing axes on rounded slopes cannot pop.
    float2 uv = worldPos.xz * scale;
    float2 dx = xzDx, dy = xzDy;
    float2 tangentView = viewDirection.xz;
    float viewNormal = viewDirection.y * (normal.y < 0.0 ? -1.0 : 1.0);
    float dominantWeight = projectionWeights.y;
    if (projectionWeights.x > dominantWeight) {
        uv = worldPos.zy * scale; dx = zyDx; dy = zyDy;
        tangentView = viewDirection.zy;
        viewNormal = viewDirection.x * (normal.x < 0.0 ? -1.0 : 1.0);
        dominantWeight = projectionWeights.x;
    }
    if (projectionWeights.z > dominantWeight) {
        uv = worldPos.xy * scale; dx = xyDx; dy = xyDy;
        tangentView = viewDirection.xy;
        viewNormal = viewDirection.z * (normal.z < 0.0 ? -1.0 : 1.0);
        dominantWeight = projectionWeights.z;
    }
    // Metres keep sand's broad texture projection from magnifying relief.
    const float depths[4] = { 0.035, 0.025, 0.050, 0.080 };
    const float depthMetres = depths[(uint)layer];
    const float depthUV = depthMetres * scale;
    const float footprint = max(length(dx), length(dy));
    const float fade = (1.0 - smoothstep(12.0, 28.0, cameraDistance)) *
        smoothstep(0.55, 0.85, dominantWeight) *
        smoothstep(0.02, 0.10, layerWeight) *
        smoothstep(0.10, 0.25, viewNormal) *
        (1.0 - smoothstep(depthUV * 0.5, depthUV * 2.0, footprint));
#ifdef SGE_TERRAIN_POM_DEPTH
    // A head-on ray has no UV travel, but still intersects the relief at a
    // different depth. The texture-only variant can skip that work.
    if (fade <= 0.001) return worldPos;
#else
    if (fade <= 0.001 || dot(tangentView, tangentView) <= 1e-8) return worldPos;
#endif
    const float travel = depthMetres * fade / max(viewNormal, 0.25);
    const float height = TerrainPOMTrace(packedMap, samplerState, uv, layer,
        dx, dy, tangentView * travel * scale, viewNormal);
    // The same displaced position feeds all three projections and every map.
    // The depth variant also projects the blended hit for raster depth. Other
    // variants use this only for texture coordinates; collision never moves.
    return worldPos + viewDirection * (height * travel);
}

#endif
