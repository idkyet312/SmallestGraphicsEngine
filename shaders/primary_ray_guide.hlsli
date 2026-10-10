#ifndef SGE_PRIMARY_RAY_GUIDE_HLSLI
#define SGE_PRIMARY_RAY_GUIDE_HLSLI

float3 DecodeTerrainVBNormal(uint packed);

struct PrimaryRayGuide {
    float3 position;
    float3 normal;
    float3 previousPosition;
    uint surface;
    int2 previousPixel;
};

// Read immutable raster inputs, never another lane's partially written guides.
bool ReadPrimaryRayGuide(uint2 pixel, out PrimaryRayGuide guide) {
    guide = (PrimaryRayGuide)0;
    guide.previousPixel = int2(-1, -1);
    uint2 id = visBuffer.Load(int3(pixel, 0));
    float depth = depthBuffer.Load(int3(pixel, 0));
    if (id.x == 0u || depth >= 1.0) return false;
#if SGE_TERRAIN_VISIBILITY
    if (IsTerrainVisibilityID(id.x)) {
        guide.position = ReconstructTerrainWorldPos(pixel, depth);
        guide.previousPosition = guide.position;
        guide.normal = DecodeTerrainVBNormal(id.y);
        guide.surface = VB_TERRAIN_ID;
    } else
#endif
    {
        DrawCallData dc = drawCalls[id.x - 1u];
        float3 p0, p1, p2, n0, n1, n2;
        float2 uv0, uv1, uv2;
        GetTriangleVertices(dc, id.y, p0, p1, p2, n0, n1, n2, uv0, uv1, uv2);
        float3 original0 = p0, original1 = p1, original2 = p2;
        float3 tangent = float3(1.0, 0.0, 0.0);
        ApplyPalmWind(p0, n0, tangent, dc.palmWindRoot, palmWind,
                      palmPrimary, palmSecondary, palmParams);
        tangent = float3(1.0, 0.0, 0.0);
        ApplyPalmWind(p1, n1, tangent, dc.palmWindRoot, palmWind,
                      palmPrimary, palmSecondary, palmParams);
        tangent = float3(1.0, 0.0, 0.0);
        ApplyPalmWind(p2, n2, tangent, dc.palmWindRoot, palmWind,
                      palmPrimary, palmSecondary, palmParams);
        float3 wp0 = mul(float4(p0, 1.0), dc.modelMatrix).xyz;
        float3 wp1 = mul(float4(p1, 1.0), dc.modelMatrix).xyz;
        float3 wp2 = mul(float4(p2, 1.0), dc.modelMatrix).xyz;
        guide.position = ReconstructWorldPos(pixel, depth);
        float3 bary = ComputeBarycentrics(guide.position, wp0, wp1, wp2);
        guide.normal = normalize(mul(float4(
            bary.x * n0 + bary.y * n1 + bary.z * n2, 0.0), dc.modelMatrix).xyz);
        if ((dc.flags & 1u) != 0u &&
            dot(guide.normal, cameraPos - guide.position) < 0.0)
            guide.normal = -guide.normal;
        guide.surface = asuint(dc.useNormalMap);
        float4 previousWind = palmWind;
        previousWind.x = palmWind.y;
        float3 old0 = ApplyPalmWindPosition(original0, dc.palmWindRoot,
            previousWind, palmPreviousPrimary, palmPreviousSecondary, palmParams);
        float3 old1 = ApplyPalmWindPosition(original1, dc.palmWindRoot,
            previousWind, palmPreviousPrimary, palmPreviousSecondary, palmParams);
        float3 old2 = ApplyPalmWindPosition(original2, dc.palmWindRoot,
            previousWind, palmPreviousPrimary, palmPreviousSecondary, palmParams);
        guide.previousPosition = mul(float4(
            bary.x * old0 + bary.y * old1 + bary.z * old2, 1.0),
            dc.previousModelMatrix).xyz;
    }
    float4 oldClip = mul(float4(guide.previousPosition, 1.0), previousViewProj);
    if (oldClip.w > 0.001) {
        float2 uv = oldClip.xy / oldClip.w * float2(0.5, -0.5) + 0.5;
        if (all(uv >= 0.0) && all(uv < 1.0))
            guide.previousPixel = int2(uv * float2(screenWidth, screenHeight));
    }
    return all(isfinite(guide.position)) && all(isfinite(guide.normal)) &&
           all(isfinite(guide.previousPosition));
}

float PrimaryRayFootprint(float3 position) {
    float z = max(abs(mul(float4(position, 1.0), viewMatrix).z), nearPlane);
    return 2.0 * z / max(screenHeight * abs(projMatrix[1][1]), 1.0);
}

bool EmissiveSelectCompatibleNeighbor(uint2 pixel, float3 position,
                                      float3 normal, out int2 previousPixel) {
    uint rng = EmissivePcg(pixel.x * 1997u ^ pixel.y * 7919u ^
                           enhancedFrameIndex * 104729u ^ 0xc9a27e1u);
    ReSTIRNeighborReservoir r = ReSTIREmptyNeighborReservoir();
    float rotation = EmissiveRandom(rng) * 6.2831853;
    float distanceToCamera = length(position - cameraPos);
    [loop] for (uint candidate = 0u; candidate < 16u; ++candidate) {
        float radius = 16.0 * sqrt(((float)candidate + 0.5) / 16.0);
        float angle = rotation + (float)candidate * 2.39996323;
        int2 source = int2(pixel) + int2(round(float2(cos(angle), sin(angle)) * radius));
        if (any(source < 0) || any(source >= int2(screenWidth, screenHeight)) ||
            all(source == int2(pixel))) continue;
        PrimaryRayGuide guide;
        if (!ReadPrimaryRayGuide(uint2(source), guide) ||
            any(guide.previousPixel < 0)) continue;
        // Reprojection/identity is geometry-only. No light reservoir is touched
        // until selection finishes, including when no usable light is stored.
        if (svgfStableSurfaceHistory.Load(int3(guide.previousPixel, 0)).x !=
            guide.surface) continue;
        float score = ReSTIRNeighborCompatibility(position, normal,
            guide.position, guide.normal, distanceToCamera);
        ReSTIRSelectNeighbor(r, guide.previousPixel, score, EmissiveRandom(rng));
    }
    previousPixel = r.pixel;
    return r.weightSum > 0.0;
}

#endif
