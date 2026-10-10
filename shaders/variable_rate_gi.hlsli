// Budgeted, compact diffuse-ray dispatch inspired by Infinity Ward's VRRT.
// History contains only actual ray samples; spatial reconstruction never feeds
// temporal history. Gradient rays replay a stored seed and bypass hit caching.
cbuffer VariableRateGIConstants : register(b6) {
    uint vrTilesX;
    uint vrTilesY;
    uint vrRayBudget;
    uint vrHistoryValid;
    uint vrEnabled;
    uint vrDebug;
    uint2 vrPadding;
};

// Mirrors VariableRateGIDX12::HistoryStride (48 bytes).
struct VRHistory {
    float3 position; uint surface;
    uint normal; uint seed; uint sequence; float count;
    uint irradiance; uint firstSample; uint frame; uint padding;
};
struct VRGradient {
    uint pixel; uint radiance; uint seed; uint sequence;
    float change; uint traced; uint2 padding;
};
StructuredBuffer<VRHistory> vrPreviousHistory : register(t101);
StructuredBuffer<VRHistory> vrCurrentHistory : register(t102);
Texture2D<float4> vrIrradiance : register(t106);
RWStructuredBuffer<uint> vrCounters : register(u20);
RWStructuredBuffer<uint2> vrRequests : register(u21);
RWStructuredBuffer<uint> vrSamples : register(u22);
RWStructuredBuffer<VRGradient> vrGradients : register(u23);
RWStructuredBuffer<uint4> vrJobs : register(u24);
RWStructuredBuffer<VRHistory> vrHistoryWrite : register(u25);
RWTexture2D<float4> vrIrradianceWrite : register(u26);
RWByteAddressBuffer vrArguments : register(u27);

uint VRPixelIndex(uint2 pixel) { return pixel.y * (uint)screenWidth + pixel.x; }
uint2 VRPixel(uint index) { return uint2(index % (uint)screenWidth, index / (uint)screenWidth); }
uint VRSeed(uint2 pixel) {
    return MatVarHashUint(pixel.x * 2654435761u ^ pixel.y * 2246822519u);
}
uint VRPackNormal(float3 n) {
    uint3 packed = (uint3)round(saturate(n * 0.5 + 0.5) * 1023.0);
    return packed.x | (packed.y << 10u) | (packed.z << 20u);
}
float3 VRUnpackNormal(uint n) {
    return normalize(float3(n & 1023u, (n >> 10u) & 1023u, (n >> 20u) & 1023u)
                     * (2.0 / 1023.0) - 1.0);
}

bool VRHistoryMatches(VRHistory h, PrimaryRayGuide guide) {
    float tolerance = max(0.025, PrimaryRayFootprint(guide.position) * 2.0);
    return h.count > 0.0 && h.count <= 64.0 && isfinite(h.count) &&
        h.surface == guide.surface &&
        dot(VRUnpackNormal(h.normal), guide.normal) >= 0.95 &&
        length(h.position - guide.previousPosition) <= tolerance;
}

// Native RR stores an unjittered previous VP. Search the small raster footprint
// instead of assuming its projected texel is exactly the previous jittered one.
uint VRFindHistory(PrimaryRayGuide guide) {
    if (vrHistoryValid == 0u || any(guide.previousPixel < 0)) return 0xffffffffu;
    uint best = 0xffffffffu;
    float bestDistance = 1e30;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 source = guide.previousPixel + int2(x, y);
            if (any(source < 0) || any(source >= int2(screenWidth, screenHeight))) continue;
            uint index = VRPixelIndex(uint2(source));
            VRHistory h = vrPreviousHistory[index];
            if (!VRHistoryMatches(h, guide)) continue;
            float distance = length(h.position - guide.previousPosition);
            if (distance < bestDistance) { best = index; bestDistance = distance; }
        }
    }
    return best;
}

float3 VRTrace(PrimaryRayGuide guide, uint seed, uint sequence, bool gradient) {
    gVRRTOverride = true;
    gVRRTGradient = gradient;
    gVRRTSeed = seed;
    gVRRTSequence = sequence;
    float3 radiance = TraceLumenGISample(guide.position, guide.normal, uint2(0, 0)).radiance;
    gVRRTOverride = false;
    gVRRTGradient = false;
    return radiance;
}

[numthreads(64, 1, 1)]
void VRReset(uint3 tid : SV_DispatchThreadID) {
    if (tid.x < 68u) vrCounters[tid.x] = 0u;
}

[numthreads(8, 8, 1)]
void VRGradientMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= vrTilesX || tid.y >= vrTilesY) return;
    uint tile = tid.y * vrTilesX + tid.x;
    uint phase = GICachePcg(tile ^ enhancedFrameIndex) & 63u;
    uint2 pixel = min(tid.xy * 8u + uint2(phase & 7u, phase >> 3u),
                      uint2(screenWidth, screenHeight) - 1u);
    VRGradient g = (VRGradient)0;
    g.pixel = 0xffffffffu;
    PrimaryRayGuide guide;
    if (all(pixel < uint2(screenWidth, screenHeight)) && ReadPrimaryRayGuide(pixel, guide) &&
        length(guide.position - cameraPos) < kLumenFarFieldEnd) {
        uint previous = VRFindHistory(guide);
        VRHistory h = (VRHistory)0;
        if (previous != 0xffffffffu) h = vrPreviousHistory[previous];
        g.seed = previous != 0xffffffffu ? h.seed : VRSeed(pixel);
        g.sequence = previous != 0xffffffffu ? h.sequence : enhancedFrameIndex * 4u;
        float3 sample = VRTrace(guide, g.seed, g.sequence, true);
        float previousLuma = RestirLuma(RestirUnpackRGB9E5(h.firstSample));
        float currentLuma = RestirLuma(sample);
        g.change = previous == 0xffffffffu ? 1.0 :
            saturate(2.0 * abs(currentLuma - previousLuma) /
                     (currentLuma + previousLuma + 0.1));
        g.pixel = VRPixelIndex(pixel);
        g.radiance = RestirPackRGB9E5(sample);
        g.traced = 1u;
        InterlockedAdd(vrCounters[64], 1u);
    }
    vrGradients[tile] = g;
}

uint VRBin(uint priority, uint requested, uint sample, uint hash) {
    // Requested rays outrank surplus rays. Randomized surplus ranks and
    // converged priorities ensure that partial buckets explore every pixel.
    return sample < requested ? max(4u, priority - min(sample, 3u))
                              : ((hash >> (sample * 2u)) & 3u);
}

[numthreads(8, 8, 1)]
void VRClassify(uint3 tid : SV_DispatchThreadID) {
    uint2 pixel = tid.xy;
    if (any(pixel >= uint2(screenWidth, screenHeight))) return;
    uint index = VRPixelIndex(pixel);
    PrimaryRayGuide guide;
    uint2 request = uint2(0u, 0xffffffffu);
    if (ReadPrimaryRayGuide(pixel, guide) &&
        length(guide.position - cameraPos) < kLumenFarFieldEnd) {
        request.y = VRFindHistory(guide);
        float count = request.y == 0xffffffffu ? 0.0 : vrPreviousHistory[request.y].count;
        VRGradient gradient = vrGradients[(pixel.y >> 3u) * vrTilesX + (pixel.x >> 3u)];
        uint hash = GICachePcg(index ^ (enhancedFrameIndex * 0x9e3779b9u));
        uint priority = request.y == 0xffffffffu || gradient.change > 0.2 ? 14u + (hash & 1u)
            : count < 4.0 ? 13u : count < 16.0 ? 12u : count < 32.0 ? 10u : 8u + (hash & 1u);
        uint requested = priority >= 14u ? 4u : priority >= 12u ? 2u : 1u;
        request.x = priority | (requested << 4u) | (1u << 12u);
        [unroll] for (uint sample = 0u; sample < 4u; ++sample)
            InterlockedAdd(vrCounters[VRBin(priority, requested, sample, hash)], 1u);
    }
    vrRequests[index] = request;
}

[numthreads(1, 1, 1)]
void VRBalance(uint3 tid : SV_DispatchThreadID) {
    uint remaining = vrRayBudget - min(vrRayBudget, vrCounters[64]);
    uint offset = 0u;
    [unroll] for (int bin = 15; bin >= 0; --bin) {
        uint quota = min(remaining, vrCounters[bin]);
        vrCounters[16u + bin] = quota;
        vrCounters[32u + bin] = offset;
        offset += quota;
        remaining -= quota;
    }
    vrCounters[65] = offset;
    // Flatten a 2D indirect dispatch for viewports exceeding 65535 groups.
    uint groups = (offset + 63u) / 64u;
    vrArguments.Store3(0u, uint3(min(groups, 65535u), max(1u, (groups + 65534u) / 65535u), 1u));
}

[numthreads(8, 8, 1)]
void VRCompact(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= uint2(screenWidth, screenHeight))) return;
    uint index = VRPixelIndex(tid.xy);
    uint2 request = vrRequests[index];
    if ((request.x & (1u << 12u)) == 0u) return;
    uint hash = GICachePcg(index ^ (enhancedFrameIndex * 0x9e3779b9u));
    uint mask = 0u;
    [unroll] for (uint sample = 0u; sample < 4u; ++sample) {
        uint bin = VRBin(request.x & 15u, (request.x >> 4u) & 7u, sample, hash);
        uint ticket;
        InterlockedAdd(vrCounters[48u + bin], 1u, ticket);
        if (ticket < vrCounters[16u + bin]) {
            uint job = vrCounters[32u + bin] + ticket;
            vrJobs[job] = uint4(index, sample, VRSeed(tid.xy), enhancedFrameIndex * 4u + sample);
            mask |= 1u << sample;
        }
    }
    vrRequests[index].x = request.x | (mask << 8u);
}

[numthreads(64, 1, 1)]
void VRTraceMain(uint3 tid : SV_DispatchThreadID) {
    uint job = tid.x + tid.y * (65535u * 64u);
    if (job >= vrCounters[65]) return;
    uint4 descriptor = vrJobs[job];
    PrimaryRayGuide guide;
    float3 sample = 0.0;
    if (ReadPrimaryRayGuide(VRPixel(descriptor.x), guide))
        sample = VRTrace(guide, descriptor.z, descriptor.w, false);
    vrSamples[descriptor.x * 4u + descriptor.y] = RestirPackRGB9E5(sample);
}

[numthreads(8, 8, 1)]
void VRAccumulate(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= uint2(screenWidth, screenHeight))) return;
    uint index = VRPixelIndex(tid.xy);
    uint2 request = vrRequests[index];
    VRHistory result = (VRHistory)0;
    PrimaryRayGuide guide;
    if ((request.x & (1u << 12u)) != 0u && ReadPrimaryRayGuide(tid.xy, guide)) {
        VRGradient gradient = vrGradients[(tid.y >> 3u) * vrTilesX + (tid.x >> 3u)];
        if (request.y != 0xffffffffu) result = vrPreviousHistory[request.y];
        if (gradient.change > 0.2) result.count = min(result.count, 4.0);
        uint mask = (request.x >> 8u) & 15u;
        uint samples = countbits(mask);
        float3 sum = 0.0;
        [unroll] for (uint sample = 0u; sample < 4u; ++sample)
            if ((mask & (1u << sample)) != 0u)
                sum += RestirUnpackRGB9E5(vrSamples[index * 4u + sample]);
        if (samples > 0u) {
            float oldCount = min(result.count, 64.0 - (float)samples);
            float3 old = RestirUnpackRGB9E5(result.irradiance);
            result.count = oldCount + (float)samples;
            result.irradiance = RestirPackRGB9E5((old * oldCount + sum) / result.count);
            uint first = (uint)firstbitlow(mask);
            result.seed = VRSeed(tid.xy);
            result.sequence = enhancedFrameIndex * 4u + first;
            result.firstSample = vrSamples[index * 4u + first];
        }
        result.position = guide.position;
        result.surface = guide.surface;
        result.normal = VRPackNormal(guide.normal);
        result.frame = enhancedFrameIndex;
    }
    vrHistoryWrite[index] = result;
}

[numthreads(8, 8, 1)]
void VRReconstruct(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= uint2(screenWidth, screenHeight))) return;
    uint index = VRPixelIndex(tid.xy);
    VRHistory own = vrCurrentHistory[index];
    float3 irradiance = 0.0;
    PrimaryRayGuide guide;
    if (ReadPrimaryRayGuide(tid.xy, guide)) {
        if (own.count > 0.0) irradiance = RestirUnpackRGB9E5(own.irradiance);
        else {
            float weightSum = 0.0;
            float footprint = max(0.025, PrimaryRayFootprint(guide.position));
            [loop] for (int y = -2; y <= 2; ++y) {
                [loop] for (int x = -2; x <= 2; ++x) {
                    int2 pixel = int2(tid.xy) + int2(x, y);
                    if (any(pixel < 0) || any(pixel >= int2(screenWidth, screenHeight))) continue;
                    VRHistory neighbor = vrCurrentHistory[VRPixelIndex(uint2(pixel))];
                    float3 normal = VRUnpackNormal(neighbor.normal);
                    float3 delta = neighbor.position - guide.position;
                    if (neighbor.count <= 0.0 || neighbor.surface != guide.surface ||
                        dot(normal, guide.normal) < 0.95 ||
                        abs(dot(delta, guide.normal)) > footprint * 0.25 ||
                        length(delta) > footprint * 8.0) continue;
                    float weight = exp(-length(delta) / (footprint * 2.0));
                    irradiance += RestirUnpackRGB9E5(neighbor.irradiance) * weight;
                    weightSum += weight;
                }
            }
            irradiance = weightSum > 0.0 ? irradiance / weightSum :
                RayHitAmbient(guide.position, guide.normal);
        }
        if (vrDebug == 1u) irradiance = (float)countbits((vrRequests[index].x >> 8u) & 15u) / 4.0;
        if (vrDebug == 2u) irradiance = vrGradients[(tid.y >> 3u) * vrTilesX + (tid.x >> 3u)].change;
    }
    vrIrradianceWrite[tid.xy] = float4(SanitizeRaySample(irradiance), own.count);
}

float3 SampleVariableRateGI(float3 position, float3 normal, uint2 pixel,
                            uint surface, out bool tracedRay) {
    uint index = VRPixelIndex(pixel);
    VRHistory guide = vrCurrentHistory[index];
    tracedRay = ((vrRequests[index].x >> 8u) & 15u) != 0u;
    VRGradient gradient = vrGradients[(pixel.y >> 3u) * vrTilesX + (pixel.x >> 3u)];
    tracedRay = tracedRay || (gradient.traced != 0u && gradient.pixel == index);
    float footprint = max(0.025, PrimaryRayFootprint(position));
    // Edge-AA sub-samples may describe a different surface than the centre.
    // They use the cheap fallback instead of violating the primary-ray cap.
    if (guide.surface != surface || dot(VRUnpackNormal(guide.normal), normal) < 0.8 ||
        length(guide.position - position) > footprint * 4.0)
        return RayHitAmbient(position, normal) * giIntensity;
    return vrIrradiance.Load(int3(pixel, 0)).rgb * giIntensity;
}
