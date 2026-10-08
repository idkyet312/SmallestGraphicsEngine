// Surface radiance cascades: screen-placed probes, world-space DXR intervals.
// The nearest grid preserves spatial detail; each farther interval has four
// times as many directions and one quarter as many probes. Only this optional
// permutation declares these bindings; the reference resolves never see them.
Texture2DArray<float4> rcCascades[4] : register(t93);
Texture2D<float4> rcIrradiance : register(t97);
Texture2D<float4> rcPositions : register(t98);
Texture2D<float4> rcNormals : register(t99);
RWTexture2DArray<float4> rcCascadeOutput : register(u11);
RWTexture2D<float4> rcIrradianceOutput : register(u12);
RWTexture2D<float4> rcPositionOutput : register(u13);
RWTexture2D<float4> rcNormalOutput : register(u14);

struct RCGuide {
    float3 position;
    float3 normal;
    uint surface;
};

bool RCReadGuide(uint2 pixel, out RCGuide guide) {
    guide = (RCGuide)0;
    uint2 id = visBuffer.Load(int3(pixel, 0));
    float depth = depthBuffer.Load(int3(pixel, 0));
    if (id.x == 0u || depth >= 1.0) return false;
    guide.surface = VB_SURFACE_ID(id.x);
#if SGE_TERRAIN_VISIBILITY
    if (IsTerrainVisibilityID(id.x)) {
        guide.position = ReconstructTerrainWorldPos(pixel, depth);
        guide.normal = DecodeTerrainVBNormal(id.y);
        return all(isfinite(guide.position));
    }
#endif
    DrawCallData dc = drawCalls[id.x - 1u];
    float3 p0, p1, p2, n0, n1, n2;
    float2 uv0, uv1, uv2;
    GetTriangleVertices(dc, id.y, p0, p1, p2, n0, n1, n2, uv0, uv1, uv2);
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
    return all(isfinite(guide.position)) && all(isfinite(guide.normal));
}

uint2 RCProbePixel(uint2 probe, uint spacing) {
    return min(probe * spacing + spacing / 2u,
               uint2(screenWidth, screenHeight) - 1u);
}

float RCPixelFootprint(float3 position) {
    float viewDepth = max(abs(mul(float4(position, 1.0), viewMatrix).z), nearPlane);
    return 2.0 * viewDepth / max(screenHeight * abs(projMatrix[1][1]), 1.0);
}

float RCGuideWeight(RCGuide guide, float3 position, float3 normal,
                    uint surface, uint spacing) {
    if (guide.surface != surface || dot(guide.normal, normal) < 0.85) return 0.0;
    float footprint = RCPixelFootprint(position);
    float3 delta = guide.position - position;
    float planeError = abs(dot(delta, guide.normal));
    float planeLimit = max(0.02, footprint * spacing * 0.25);
    float distanceLimit = max(0.05, footprint * spacing * 2.0);
    if (planeError >= planeLimit || dot(delta, delta) > distanceLimit * distanceLimit)
        return 0.0;
    return 1.0 - planeError / planeLimit;
}

// Equal-area spherical cells make the four child directions have equal weight.
// Unlike latitude/longitude sampling, this needs no polar solid-angle correction.
float3 RCDirection(uint direction, uint resolution) {
    float2 uv = (float2(direction % resolution, direction / resolution) + 0.5) /
                resolution;
    float y = 1.0 - 2.0 * uv.y;
    float radius = sqrt(max(0.0, 1.0 - y * y));
    float phi = 6.2831853071 * uv.x;
    return float3(cos(phi) * radius, y, sin(phi) * radius);
}

float3 RCTraceInterval(RCGuide guide, float3 direction,
                       float start, float end, out bool hit) {
    RayDesc ray;
    ray.Origin = guide.position + guide.normal * (normalBias + 0.02);
    ray.Direction = direction;
    ray.TMin = start;
    ray.TMax = end;
    RayQuery<SGE_RAY_FLAGS_GI> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {}
    hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    if (!hit) return 0.0;
    bool resolved;
    float3 shaded = ShadeRayHit(ReadGIHit(query), direction, resolved);
    return SanitizeRaySample(resolved ? shaded :
        SampleReflectionProbe(direction, 1.0) * enhancedReflectionOcclusion);
}

static const float kRCBaseInterval = 0.25;

groupshared RCGuide rcGuide;
groupshared uint rcGuideValid;
groupshared uint2 rcNextProbes[4];
groupshared float rcNextWeights[4];
groupshared float rcNextWeightSum;
groupshared float3 rcNearRadiance[16];

[numthreads(64, 1, 1)]
void RadianceCascadeMain(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    uint spacing = rcProbeSpacing << rcLevel;
    uint resolution = 4u << rcLevel;
    uint direction = group.z * 64u + lane;
    uint2 probe = group.xy;
    if (lane == 0u) {
        uint2 pixel = RCProbePixel(probe, spacing);
        rcGuideValid = RCReadGuide(pixel, rcGuide) ? 1u : 0u;
        rcNextWeightSum = 0.0;
        if (rcGuideValid != 0u && rcLevel + 1u < rcLevelCount) {
            uint nextSpacing = spacing * 2u;
            uint2 nextSize = (uint2(screenWidth, screenHeight) + nextSpacing - 1u) /
                             nextSpacing;
            float2 coordinate = (float2(pixel) - nextSpacing / 2u) / nextSpacing;
            int2 base = int2(floor(coordinate));
            float2 fraction = frac(coordinate);
            [unroll] for (uint q = 0u; q < 4u; ++q) {
                uint2 corner = uint2(q & 1u, q >> 1u);
                rcNextProbes[q] = uint2(clamp(base + int2(corner), int2(0, 0),
                                            int2(nextSize) - 1));
                float2 weight = lerp(1.0 - fraction, fraction, float2(corner));
                RCGuide nextGuide;
                bool valid = RCReadGuide(RCProbePixel(rcNextProbes[q], nextSpacing),
                                         nextGuide);
                rcNextWeights[q] = valid ? weight.x * weight.y * RCGuideWeight(
                    nextGuide, rcGuide.position, rcGuide.normal, rcGuide.surface,
                    nextSpacing) : 0.0;
                rcNextWeightSum += rcNextWeights[q];
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    float3 incoming = 0.0;
    bool terminated = false;
    if (direction < resolution * resolution && rcGuideValid != 0u) {
        float3 rayDirection = RCDirection(direction, resolution);
        float rayLength = max(enhancedReflectionRayLength, 0.25);
        // Intervals grow 4x per level, matching the 4x direction count. The
        // base stays short because level 0 has only 16 directions: a long
        // first interval makes nearby bounce light alias probe to probe.
        // The last level runs out to the reference ray length.
        float unit = min(kRCBaseInterval, rayLength / 64.0);
        bool last = rcLevel + 1u == rcLevelCount;
        float start = rcLevel == 0u ? 0.0 : unit * exp2(2.0 * (rcLevel - 1u));
        float end = last ? rayLength : unit * exp2(2.0 * rcLevel);
        bool extend = !last && rcNextWeightSum <= 0.001;
        incoming = RCTraceInterval(rcGuide, rayDirection, start,
                                  extend ? rayLength : end, terminated);
        if (!terminated) {
            if (last || extend) {
                incoming = SampleReflectionProbe(rayDirection, 1.0);
            } else {
                uint2 parent = uint2(direction % resolution, direction / resolution);
                uint childResolution = resolution * 2u;
                [unroll] for (uint q = 0u; q < 4u; ++q) {
                    uint2 child = parent * 2u + uint2(q & 1u, q >> 1u);
                    uint childIndex = child.y * childResolution + child.x;
                    [unroll] for (uint p = 0u; p < 4u; ++p) {
                        if (rcNextWeights[p] > 0.0)
                            incoming += rcCascades[rcLevel + 1u].Load(
                                int4(rcNextProbes[p], childIndex, 0)).rgb *
                                (0.25 * rcNextWeights[p] / rcNextWeightSum);
                    }
                }
            }
        }
        incoming = SanitizeRaySample(incoming);
    }
    if (direction < resolution * resolution)
        rcCascadeOutput[uint3(probe, direction)] =
            float4(incoming, terminated ? 0.0 : 1.0);

    if (rcLevel == 0u) {
        // Diagnostics (SGE_RC_DEBUG): 2 = level-0 hits only, 3 = merged
        // far field only, 1 = probes that skipped the merge in red.
        if (lane < 16u)
            rcNearRadiance[lane] = rcEnabled == 3u ? (terminated ? incoming : 0.0)
                                 : rcEnabled == 4u ? (terminated ? 0.0 : incoming)
                                 : incoming;
        GroupMemoryBarrierWithGroupSync();
        if (lane == 0u) {
            float3 irradiance = 0.0;
            float cosineSum = 0.0;
            [unroll] for (uint d = 0u; d < 16u; ++d) {
                float cosine = max(dot(rcGuide.normal, RCDirection(d, 4u)), 0.0);
                irradiance += rcNearRadiance[d] * cosine;
                cosineSum += cosine;
            }
            // The resolve consumes E/pi, like its cosine-weighted Lumen ray.
            // Normalization preserves constant sky radiance at every orientation.
            irradiance /= max(cosineSum, 1e-5);
            if (rcEnabled == 2u)
                irradiance = rcNextWeightSum <= 0.001 ? float3(1, 0, 0) : float3(0, 0.3, 0);
            rcIrradianceOutput[probe] = float4(irradiance, (float)rcGuideValid);
            rcPositionOutput[probe] = float4(rcGuide.position, (float)rcGuideValid);
            rcNormalOutput[probe] = float4(rcGuide.normal,
                rcGuide.surface == 0xFFFFFFFFu ? -1.0 : (float)rcGuide.surface);
        }
    }
}

float3 SampleRadianceCascades(float3 position, float3 normal, uint2 pixel,
                              out bool resolved) {
    resolved = false;
    uint2 size = (uint2(screenWidth, screenHeight) + rcProbeSpacing - 1u) /
                  rcProbeSpacing;
    float2 coordinate = (float2(pixel) - rcProbeSpacing / 2u) / rcProbeSpacing;
    int2 base = int2(floor(coordinate));
    float2 fraction = frac(coordinate);
    uint surface = VB_SURFACE_ID(visBuffer.Load(int3(pixel, 0)).x);
    float3 irradiance = 0.0;
    float totalWeight = 0.0;
    [unroll] for (uint q = 0u; q < 4u; ++q) {
        uint2 corner = uint2(q & 1u, q >> 1u);
        int2 probe = clamp(base + int2(corner), int2(0, 0), int2(size) - 1);
        float4 guidePosition = rcPositions.Load(int3(probe, 0));
        float4 guideNormal = rcNormals.Load(int3(probe, 0));
        RCGuide guide;
        guide.position = guidePosition.xyz;
        guide.normal = guideNormal.xyz;
        guide.surface = guideNormal.w < 0.0 ? 0xFFFFFFFFu : (uint)guideNormal.w;
        float2 spatial = lerp(1.0 - fraction, fraction, float2(corner));
        float weight = guidePosition.w * spatial.x * spatial.y * RCGuideWeight(
            guide, position, normal, surface, rcProbeSpacing);
        irradiance += rcIrradiance.Load(int3(probe, 0)).rgb * weight;
        totalWeight += weight;
    }
    resolved = totalWeight > 0.001;
    return resolved ? irradiance * (giIntensity / totalWeight) : 0.0;
}
